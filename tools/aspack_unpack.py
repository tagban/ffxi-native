"""Static ASPack unpacker for the 2003-era FFXI DLLs (FFXiMain.dll / FFXi.dll build 2003-09-05).

Before the POL1 packer (tools/pol1_unpack.py), Square Enix shipped the client DLLs packed with
ASPack 2.12: section names blanked, the entry point at the start of a stub section, and an empty
`.adata` section after it. The entry stub is the classic one:

    60 E8 03 00 00 00 E9 EB 04 5D 45 55 C3 E8 01 00 00 00 EB 5D BB ED FF FF FF ...

Rather than re-implement ASPack's decompressor, its own stub is run under an emulator (Unicorn),
never natively: the image is mapped, the stub's few API calls (GetModuleHandleA, LoadLibraryA,
GetProcAddress, VirtualAlloc, VirtualFree, VirtualProtect) are answered by the hooks here, and the
run stops when execution first reaches the first section: the original entry point.

Two things the rest of the pipeline needs that a memory dump alone does not give:

  - Relocations. The recompiler must know which dwords are image addresses (recomp.py,
    text_relocations). The stub is run twice, at the preferred base and at another one; ASPack
    relocates the unpacked image itself, so every dword that differs by exactly the base delta is a
    relocation site. They are written as an ordinary base-relocation table at the start of a new
    `.reloc` section (the same shape the POL1 images have) and the PE directory points at it.
  - Imports. The stub resolves the original import table into the IAT; GetProcAddress here
    returns a trap address per (dll, name), so the IAT can be put back to hint/name thunks and the
    import directory pointed at the original descriptors.

Section names are restored by role: the first section is `.text`; the stub's two sections are
kept (named `.aspack`, `.adata`) so every RVA stays where the retail file has it.

Usage: python aspack_unpack.py <packed.dll> <out.dll>
"""
import struct
import sys

import pefile

from winemu import Run  # noqa: E402

ASPACK_SIG = bytes.fromhex('60e803000000e9eb045d4555c3e801000000eb5d')


def relocation_sites(a, b, delta, lo, hi):
    """RVAs in [lo, hi) where run b's dword is run a's plus delta."""
    sites = []
    i = lo
    while i + 4 <= hi:
        x = struct.unpack_from('<I', a, i)[0]
        y = struct.unpack_from('<I', b, i)[0]
        if x != y and (x + delta) & 0xffffffff == y:
            sites.append(i)
            i += 4
        else:
            i += 1
    return sites


def reloc_table(sites):
    out = bytearray()
    pages = {}
    for s in sites:
        pages.setdefault(s & ~0xfff, []).append(s & 0xfff)
    for page in sorted(pages):
        ents = [(3 << 12) | o for o in sorted(pages[page])]
        if len(ents) % 2:
            ents.append(0)
        out += struct.pack('<II', page, 8 + 2 * len(ents))
        out += struct.pack('<%dH' % len(ents), *ents)
    return bytes(out)


def resolve_slot(run, value):
    """The (dll, name) an IAT slot calls: a trap directly, or through one of ASProtect's
    redirection stubs (`jmp <api>` or `push <api> ; ret`) in its own heap."""
    if value in run.traps:
        return run.traps[value]
    from winemu import HEAP, HEAP_SIZE
    if not HEAP <= value < HEAP + HEAP_SIZE:
        return None
    b = bytes(run.mu.mem_read(value, 6))
    if b[0] == 0xe9:
        target = (value + 5 + struct.unpack_from('<i', b, 1)[0]) & 0xffffffff
    elif b[0] == 0x68 and b[5] == 0xc3:
        target = struct.unpack_from('<I', b, 1)[0]
    else:
        return None
    return run.traps.get(target)


def find_iat(run, image, lo, hi):
    """The import address table: runs of resolvable slots, each thunk array ended by a zero
    dword. Returns [(first slot rva, [(dll, name), ...])], one entry per thunk array."""
    from winemu import HEAP, HEAP_SIZE
    found = {}
    for off in range(lo, hi - 4, 4):
        r = resolve_slot(run, struct.unpack_from('<I', image, off)[0])
        if r and not r[0].startswith('<'):
            found[off] = r
    # The table is one dense block (MSVC puts it at the start of .rdata); an API pointer the
    # program stored elsewhere (a function pointer in .data) is not part of it. Keep the slots of
    # the section holding most of them.
    sections = sorted({(sec_lo, sec_hi) for sec_lo, sec_hi in run.section_spans})
    def section_of(off):
        return next((sp for sp in sections if sp[0] <= off < sp[1]), None)
    from collections import Counter
    home = Counter(section_of(off) for off in found).most_common(1)[0][0]
    found = {off: r for off, r in found.items() if section_of(off) == home}
    # A slot inside the table that is neither: one of the protector's own API emulations
    # (GetProcAddress, GetModuleHandleA, GetVersion...). Only those between resolved neighbours
    # (or a zero terminator) are asked what they are.
    for off in sorted(found):
        nxt = off + 4
        while nxt not in found:
            v = struct.unpack_from('<I', image, nxt)[0]
            if v == 0 or not HEAP <= v < HEAP + HEAP_SIZE:
                break
            r = run.identify_emulation(v)
            if r is None:
                raise SystemExit('IAT slot %#x holds %#x: an API emulation this unpacker cannot identify' % (nxt, v))
            found[nxt] = r
            nxt += 4
    arrays = []
    for off in sorted(found):
        if arrays and off == arrays[-1][0] + 4 * len(arrays[-1][1]) and found[off][0] == arrays[-1][1][-1][0]:
            arrays[-1][1].append(found[off])
        else:
            arrays.append((off, [found[off]]))
    for start, entries in arrays:
        end = start + 4 * len(entries)
        if struct.unpack_from('<I', image, end)[0] != 0:
            raise SystemExit('thunk array at %#x (%s) runs into %#x without a zero terminator'
                             % (start, entries[0][0], struct.unpack_from('<I', image, end)[0]))
    return arrays


def build_idata(arrays, rva):
    """A fresh import section at `rva` for the thunk arrays found in place: descriptors whose
    FirstThunk is the original IAT array, OriginalFirstThunk a copy here, hint/name entries here.
    Returns (section bytes, {slot rva: thunk value}, descriptor count)."""
    n = len(arrays)
    desc_size = 20 * (n + 1)
    oft_size = sum(4 * (len(e) + 1) for _, e in arrays)
    names = bytearray()
    name_rva = {}

    def intern(blob):
        if blob not in name_rva:
            name_rva[blob] = rva + desc_size + oft_size + len(names)
            names.extend(blob)
            if len(names) % 2:
                names.append(0)
        return name_rva[blob]

    descs = bytearray()
    ofts = bytearray()
    slots = {}
    oft_at = rva + desc_size
    for start, entries in arrays:
        dll = entries[0][0]
        thunks = []
        for _, fn in entries:
            if isinstance(fn, int):
                thunks.append(0x80000000 | fn)
            else:
                thunks.append(intern(b'\0\0' + fn.encode() + b'\0'))
        dll_rva = intern(dll.upper().encode() + b'\0')
        descs += struct.pack('<5I', oft_at + len(ofts), 0, 0, dll_rva, start)
        for k, t in enumerate(thunks):
            slots[start + 4 * k] = t
        ofts += struct.pack('<%dI' % (len(thunks) + 1), *(thunks + [0]))
    descs += bytes(20)
    return bytes(descs + ofts + names), slots, n


def section_header(name, vsize, rva, raw_size, raw_ptr, chars):
    return struct.pack('<8sIIIIIIHHI', name.ljust(8, b'\0')[:8], vsize, rva, raw_size, raw_ptr, 0, 0, 0, 0, chars)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    src, out = sys.argv[1], sys.argv[2]
    pe = pefile.PE(src)
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    if pe.get_data(ep, len(ASPACK_SIG)) != ASPACK_SIG:
        raise SystemExit('entry point is not an ASPack 2.12 stub: %s' % pe.get_data(ep, 20).hex())
    pref = pe.OPTIONAL_HEADER.ImageBase
    stub_idx = next(i for i, s in enumerate(pe.sections) if s.VirtualAddress <= ep < s.VirtualAddress + max(s.Misc_VirtualSize, 1))

    a = Run(pe, pref).go()
    a.section_spans = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections]
    print('original entry point %#x (rva %#x); %d API traps, modules %s' % (
        a.oep, a.oep - pref, len(a.traps), sorted(a.modules)))

    alt = pref + 0x10000000
    pe_b = pefile.PE(src)
    pe_b.relocate_image(alt)  # what the Windows loader does to the stub before it runs
    b = Run(pe_b, alt, pref=pref).go()
    if b.oep - alt != a.oep - pref:
        raise SystemExit('the two runs disagree on the entry point')
    delta = alt - pref
    # Relocation sites in the original sections only (not the stub's).
    sites = []
    for i, s in enumerate(pe.sections):
        if i >= stub_idx:
            break
        sites += relocation_sites(a.image, b.image, delta, s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
    print('%d relocation sites' % len(sites))

    image = bytearray(a.image)

    # Imports: ASProtect wiped the import directory, so it is rebuilt from the resolved IAT.
    first = pe.sections[0]
    # (never in .text: a stray dword there can look like an API pointer)
    arrays = find_iat(a, image, pe.sections[1].VirtualAddress, pe.sections[stub_idx].VirtualAddress)
    print('import address table: %d thunk arrays, %d slots, dlls %s' % (
        len(arrays), sum(len(e) for _, e in arrays), sorted({e[0][0] for _, e in arrays})))

    align_f = pe.OPTIONAL_HEADER.FileAlignment
    align_s = pe.OPTIONAL_HEADER.SectionAlignment
    last = pe.sections[-1]
    idata_rva = (last.VirtualAddress + max(last.Misc_VirtualSize, 1) + align_s - 1) & ~(align_s - 1)
    idata, slots, n_imp = build_idata(arrays, idata_rva)
    for slot, thunk in slots.items():
        struct.pack_into('<I', image, slot, thunk)  # as on disk: the loader fills them in
    rel = reloc_table(sites)
    rel_rva = (idata_rva + len(idata) + align_s - 1) & ~(align_s - 1)

    # Rebuild the file: every section raw == virtual, then .idata and .reloc.
    hdr_size = pe.OPTIONAL_HEADER.SizeOfHeaders
    sec_table = pe.sections[0].get_file_offset()
    if sec_table + 40 * (len(pe.sections) + 2) > hdr_size:
        raise SystemExit('no room in the headers for two more sections')
    data = bytearray(pe.__data__[:hdr_size])
    ptr = (hdr_size + align_f - 1) & ~(align_f - 1)
    data += bytes(ptr - len(data))
    headers = []
    for i, s in enumerate(pe.sections):
        nm = s.Name.rstrip(b'\0') or (b'.text' if i == 0 else b'.aspack' if i == stub_idx else b'.sec%d' % i)
        raw = bytes(image[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]).rstrip(b'\0')
        raw += bytes((-len(raw)) % align_f)
        chars = s.Characteristics
        if i == 0:
            chars = (chars & ~0x80) | 0x20 | 0x20000000 | 0x40000000  # code, execute, read
        headers.append(section_header(nm, s.Misc_VirtualSize, s.VirtualAddress, len(raw), ptr if raw else 0, chars))
        data += raw
        ptr += len(raw)
    for nm, body, rva, chars in ((b'.idata', idata, idata_rva, 0xc0000040), (b'.reloc', rel, rel_rva, 0x42000040)):
        raw = body + bytes((-len(body)) % align_f)
        headers.append(section_header(nm, len(body), rva, len(raw), ptr, chars))
        data += raw
        ptr += len(raw)
    data[sec_table:sec_table + 40 * len(headers)] = b''.join(headers)

    out_pe = pefile.PE(data=bytes(data), fast_load=True)
    out_pe.FILE_HEADER.NumberOfSections = len(headers)
    out_pe.OPTIONAL_HEADER.SizeOfImage = (rel_rva + len(rel) + align_s - 1) & ~(align_s - 1)
    out_pe.OPTIONAL_HEADER.AddressOfEntryPoint = a.oep - pref
    dd = out_pe.OPTIONAL_HEADER.DATA_DIRECTORY
    dd[1].VirtualAddress, dd[1].Size = idata_rva, 20 * (n_imp + 1)
    dd[5].VirtualAddress, dd[5].Size = rel_rva, len(rel)
    dd[12].VirtualAddress, dd[12].Size = 0, 0
    out_pe.write(out)
    print('wrote %s' % out)


if __name__ == '__main__':
    main()
