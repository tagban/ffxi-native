"""A 32-bit Windows thread, as far as a packer stub can tell: the image mapped, a TEB and PEB at fs,
structured exception handling, and the handful of APIs a stub calls - under Unicorn, never native.

Used by tools/aspack_unpack.py to run the 2003-era ASProtect/ASPack stub to its original entry
point. Nothing here runs retail code on the host CPU.
"""
import os
import struct

import pefile
from unicorn import *  # noqa: F401,F403
from unicorn.x86_const import *  # noqa: F401,F403

TRAP = 0x7f000000        # API traps: one 16-byte slot per (dll, name)
TRAP_SIZE = 0x100000
HEAP = 0x60000000        # VirtualAlloc
HEAP_SIZE = 0x08000000
STACK = 0x5f000000
STACK_SIZE = 0x100000
MODULE_HANDLE = 0x70000000  # fake modules: MODULE_HANDLE + MODULE_SPAN * n, each a synthetic PE
MODULE_SPAN = 0x200000
SYSTEM_DLLS = ('C:/Windows/SysWOW64', 'C:/Windows/System32')
GDT = 0x5e000000
TEB = 0x5e100000
PEB = 0x5e101000
SCRATCH = 0x5e102000     # strings handed back by emulated APIs

# APIs answered with a plain success: name -> number of stdcall arguments
QUIET = {
    'VirtualFree': 3, 'CloseHandle': 1, 'SetLastError': 1, 'GetSystemTime': 1, 'GetLocalTime': 1,
    'Sleep': 1, 'FreeLibrary': 1, 'GetVersionExA': 1, 'GetSystemInfo': 1,
    'InitializeCriticalSection': 1, 'EnterCriticalSection': 1, 'LeaveCriticalSection': 1,
    'DeleteCriticalSection': 1, 'QueryPerformanceCounter': 1, 'GetStartupInfoA': 1,
}

# APIs answered with a fixed value: a US English Windows XP machine. name -> (stdcall arguments, result)
VALUE = {
    'GetThreadLocale': (0, 0x409), 'GetUserDefaultLCID': (0, 0x409), 'GetSystemDefaultLCID': (0, 0x409),
    'GetUserDefaultLangID': (0, 0x409), 'GetSystemDefaultLangID': (0, 0x409), 'GetACP': (0, 1252),
    'GetOEMCP': (0, 437), 'GetKeyboardType': (1, 4), 'GetDriveTypeA': (1, 3),
}

# APIs answered with a failure, as on a clean machine: name -> (stdcall arguments, result)
FAIL = {
    'RegOpenKeyExA': (5, 2), 'RegOpenKeyA': (3, 2), 'RegQueryValueExA': (6, 2), 'RegCloseKey': (1, 0),
    'SetErrorMode': (1, 0), 'GetTimeZoneInformation': (1, 0), 'GetCursorPos': (1, 0),
    'LoadStringA': (4, 0), 'FindResourceA': (3, 0), 'GetSystemMetrics': (1, 0), 'GetDC': (1, 0),
    'GetLocaleInfoA': (4, 0), 'GetLocaleInfoW': (4, 0), 'EnumCalendarInfoA': (4, 0),
    'RegCreateKeyExA': (9, 5), 'RegSetValueExA': (6, 5), 'FindWindowA': (2, 0), 'GetFileAttributesA': (1, 0xffffffff),
}

STDCALL = None
SDK_LIBS = 'C:/Program Files (x86)/Windows Kits/10/Lib'


def stdcall_sizes():
    """name -> bytes of stdcall arguments, from the x86 import libraries' decorated names
    (`__imp__RegSetValueA@20`). Only used for APIs not answered above: they get a logged success."""
    import glob
    import re
    sizes = {}
    for lib in sorted(glob.glob(SDK_LIBS + '/*/um/x86/*.lib')):
        if os.path.basename(lib).lower() not in ('kernel32.lib', 'user32.lib', 'advapi32.lib', 'gdi32.lib',
                                                 'ole32.lib', 'oleaut32.lib', 'shell32.lib', 'winmm.lib', 'ws2_32.lib'):
            continue
        for m in re.finditer(rb'__imp__([A-Za-z0-9_]+)@(\d+)', open(lib, 'rb').read()):
            sizes.setdefault(m.group(1).decode(), int(m.group(2)))
    return sizes


EXC_ACCESS = 0xC0000005
EXC_BREAKPOINT = 0x80000003
EXC_SINGLE_STEP = 0x80000004
EXC_DIV_ZERO = 0xC0000094
EXC_ILLEGAL = 0xC000001D
EXC_PRIV = 0xC0000096
CTX = {'SegGs': 0x8c, 'SegFs': 0x90, 'SegEs': 0x94, 'SegDs': 0x98, 'Edi': 0x9c, 'Esi': 0xa0,
       'Ebx': 0xa4, 'Edx': 0xa8, 'Ecx': 0xac, 'Eax': 0xb0, 'Ebp': 0xb4, 'Eip': 0xb8, 'SegCs': 0xbc,
       'EFlags': 0xc0, 'Esp': 0xc4, 'SegSs': 0xc8}
CTX_SIZE = 0x2cc
REGS = {'Edi': UC_X86_REG_EDI, 'Esi': UC_X86_REG_ESI, 'Ebx': UC_X86_REG_EBX, 'Edx': UC_X86_REG_EDX,
        'Ecx': UC_X86_REG_ECX, 'Eax': UC_X86_REG_EAX, 'Ebp': UC_X86_REG_EBP, 'Eip': UC_X86_REG_EIP,
        'EFlags': UC_X86_REG_EFLAGS, 'Esp': UC_X86_REG_ESP}
FAULTS = (UC_ERR_READ_UNMAPPED, UC_ERR_WRITE_UNMAPPED, UC_ERR_FETCH_UNMAPPED,
          UC_ERR_READ_PROT, UC_ERR_WRITE_PROT, UC_ERR_FETCH_PROT)


def gdt_entry(base, limit, access, flags=0xc):
    return struct.pack('<Q', (limit & 0xffff) | ((base & 0xffffff) << 16) | (access << 40)
                       | (((limit >> 16) & 0xf) << 48) | (flags << 52) | (((base >> 24) & 0xff) << 56))


class Run:
    """One emulated DllMain(DLL_PROCESS_ATTACH) of a packed DLL mapped at `base`, stopped when
    execution first reaches the first section (the original entry point)."""

    def __init__(self, pe, base, pref=None, limit=4_000_000_000, verbose=False):
        self.pe = pe
        self.base = base
        self.pref = pref if pref is not None else pe.OPTIONAL_HEADER.ImageBase
        self.size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
        first = pe.sections[0]
        self.text_lo = base + first.VirtualAddress
        self.text_hi = self.text_lo + first.Misc_VirtualSize
        self.modules = {}       # lower-case dll name -> handle
        self.names = {}         # handle -> dll name
        self.traps = {}         # trap address -> (dll, name or ordinal)
        self.trap_of = {}
        self.module_name_ptrs = []  # dll-name strings passed to LoadLibrary/GetModuleHandle
        self.calls = []         # (dll, name) of every API the stub called, in order
        self.heap_next = HEAP
        self.oep = None
        self.limit = limit
        self.verbose = verbose
        self.pending = None     # (code, address, context eip, params) raised by a hook
        self.in_flight = []     # handler calls not yet returned
        self.exceptions = 0
        self.ticks = 0x01000000

        mu = self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        mu.mem_map(base, self.size)
        image = bytearray(pe.get_memory_mapped_image(ImageBase=base))
        image += b'\0' * max(0, self.size - len(image))
        mu.mem_write(base, bytes(image[:self.size]))
        mu.mem_map(TRAP, TRAP_SIZE)
        mu.mem_write(TRAP, b'\xc3' * TRAP_SIZE)
        mu.mem_map(HEAP, HEAP_SIZE)
        mu.mem_map(STACK, STACK_SIZE)
        self.segments()

        # The packed file's own imports (the stub's): each IAT slot points at a trap.
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        for desc in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', ()):
            dll = desc.dll.decode().lower()
            self.module(dll)
            for imp in desc.imports:
                name = imp.name.decode() if imp.name else imp.ordinal
                mu.mem_write(base + (imp.address - pe.OPTIONAL_HEADER.ImageBase), struct.pack('<I', self.trap(dll, name)))

        # DllMain(hinst, DLL_PROCESS_ATTACH, 0), returning to a sentinel trap.
        sp = STACK + STACK_SIZE - 0x100
        mu.mem_write(sp, struct.pack('<IIII', self.trap('<return>', 'DllMain'), base, 1, 0))
        mu.reg_write(UC_X86_REG_ESP, sp)
        self.seh_ret = self.trap('<return>', 'handler')
        self.entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        mu.hook_add(UC_HOOK_CODE, self.on_text, begin=self.text_lo, end=self.text_hi - 1)
        mu.hook_add(UC_HOOK_CODE, self.on_trap, begin=TRAP, end=TRAP + TRAP_SIZE - 1)
        mu.hook_add(UC_HOOK_INTR, self.on_intr)

    def segments(self):
        """Flat cs/ds/es/ss, and fs at the TEB through a GDT (Unicorn has no 32-bit fs base)."""
        mu = self.mu
        mu.mem_map(GDT, 0x200000)
        gdt = (b'\0' * 8 + gdt_entry(0, 0xfffff, 0x9a) + gdt_entry(0, 0xfffff, 0x92)
               + gdt_entry(TEB, 0xfff, 0x92, 0x4))
        mu.mem_write(GDT, gdt)
        mu.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        teb = bytearray(0x1000)
        struct.pack_into('<III', teb, 0, 0xffffffff, STACK + STACK_SIZE, STACK)
        struct.pack_into('<I', teb, 0x18, TEB)
        struct.pack_into('<II', teb, 0x20, 0x1000, 0x1004)  # process and thread id
        struct.pack_into('<I', teb, 0x30, PEB)
        mu.mem_write(TEB, bytes(teb))
        peb = bytearray(0x1000)
        struct.pack_into('<I', peb, 8, self.base)  # ImageBaseAddress; BeingDebugged (+2) stays 0
        mu.mem_write(PEB, bytes(peb))
        mu.reg_write(UC_X86_REG_CS, 0x08)
        mu.reg_write(UC_X86_REG_SS, 0x10)
        mu.reg_write(UC_X86_REG_DS, 0x10)
        mu.reg_write(UC_X86_REG_ES, 0x10)
        mu.reg_write(UC_X86_REG_FS, 0x18)
        mu.reg_write(UC_X86_REG_GS, 0x10)

    def module(self, dll):
        dll = dll.lower()
        if not dll.endswith('.dll'):
            dll += '.dll'
        if dll not in self.modules:
            h = MODULE_HANDLE + MODULE_SPAN * (len(self.modules) + 1)
            self.modules[dll] = h
            self.names[h] = dll
            self.synthesize(dll, h)
        return self.modules[dll]

    def synthesize(self, dll, base):
        """A PE image at `base` with nothing but an export directory: every name the real system
        DLL exports (names only, read from the host's own 32-bit copy), each pointing at a trap.
        Stubs that walk export tables and match names by hash then find what GetProcAddress would."""
        import os
        names = []
        for d in SYSTEM_DLLS:
            p = os.path.join(d, dll)
            if os.path.exists(p):
                spe = pefile.PE(p, fast_load=True)
                spe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
                exp = getattr(spe, 'DIRECTORY_ENTRY_EXPORT', None)
                if exp:
                    names = sorted(e.name.decode() for e in exp.symbols if e.name)
                break
        img = bytearray(MODULE_SPAN)
        img[0:2] = b'MZ'
        struct.pack_into('<I', img, 0x3c, 0x80)
        img[0x80:0x84] = b'PE' + bytes(2)
        struct.pack_into('<HH', img, 0x84, 0x14c, 0)          # machine, sections
        struct.pack_into('<H', img, 0x94, 0xe0)               # SizeOfOptionalHeader
        struct.pack_into('<H', img, 0x98, 0x10b)              # PE32
        struct.pack_into('<I', img, 0x98 + 28, base)          # ImageBase
        struct.pack_into('<I', img, 0x98 + 56, MODULE_SPAN)   # SizeOfImage
        struct.pack_into('<I', img, 0x98 + 92, 16)            # NumberOfRvaAndSizes
        exp = 0x1000
        n = len(names)
        funcs, nptrs, ords, strs = exp + 0x40, exp + 0x40 + 4 * n, exp + 0x40 + 8 * n, exp + 0x40 + 10 * n
        dllname = strs
        img[dllname:dllname + len(dll) + 1] = dll.encode() + bytes(1)
        at = dllname + len(dll) + 1
        for i, nm in enumerate(names):
            struct.pack_into('<I', img, funcs + 4 * i, self.trap(dll, nm) - base)
            struct.pack_into('<I', img, nptrs + 4 * i, at)
            struct.pack_into('<H', img, ords + 2 * i, i)
            b = nm.encode() + bytes(1)
            img[at:at + len(b)] = b
            at += len(b)
        if at > MODULE_SPAN:
            raise SystemExit('export table of %s does not fit' % dll)
        struct.pack_into('<IIHHIIIIIII', img, exp, 0, 0, 0, 0, dllname, 1, n, n, funcs, nptrs, ords)
        struct.pack_into('<II', img, 0x98 + 96, exp, at - exp)  # export directory
        self.mu.mem_map(base, MODULE_SPAN)
        self.mu.mem_write(base, bytes(img))

    def trap(self, dll, name):
        key = (dll, name)
        if key not in self.trap_of:
            a = TRAP + 16 * len(self.trap_of)
            self.trap_of[key] = a
            self.traps[a] = key
        return self.trap_of[key]

    def cstr(self, addr, n=256):
        """A NUL-terminated string, read in small pieces so it never runs off mapped memory."""
        out = b''
        while len(out) < n:
            piece = bytes(self.mu.mem_read(addr + len(out), 16 - (addr + len(out)) % 16))
            z = piece.find(b'\0')
            if z >= 0:
                return (out + piece[:z]).decode('latin-1')
            out += piece
        return out[:n].decode('latin-1')

    def alloc(self, size):
        """Zeroed memory from the emulated heap (never reused: a stub allocates little)."""
        self.sizes = getattr(self, 'sizes', {})
        addr = self.heap_next
        self.heap_next += (max(size, 1) + 0xf) & ~0xf
        if self.heap_next > HEAP + HEAP_SIZE:
            raise SystemExit('emulated heap exhausted')
        self.sizes[addr] = size
        return addr

    def arg(self, i):
        sp = self.mu.reg_read(UC_X86_REG_ESP)
        return struct.unpack('<I', self.mu.mem_read(sp + 4 + 4 * i, 4))[0]

    def ret(self, value, nargs):
        mu = self.mu
        sp = mu.reg_read(UC_X86_REG_ESP)
        ra = struct.unpack('<I', mu.mem_read(sp, 4))[0]
        mu.reg_write(UC_X86_REG_EAX, value & 0xffffffff)
        mu.reg_write(UC_X86_REG_ESP, sp + 4 + 4 * nargs)
        mu.reg_write(UC_X86_REG_EIP, ra)

    # --- hooks ------------------------------------------------------------------------------------
    def on_text(self, mu, address, size, _):
        # A protector probes the section before it is decrypted (a lone `ret` it plants and calls):
        # that is not the entry point. The real one is the first instruction there that is not.
        op = bytes(mu.mem_read(address, 1))[0]
        if op in (0xc3, 0xc2):
            self.probes = getattr(self, 'probes', 0) + 1
            return
        self.oep = address
        mu.emu_stop()

    def on_intr(self, mu, intno, _):
        eip = mu.reg_read(UC_X86_REG_EIP)
        if intno == 3:  # int3: Windows reports the int3 itself
            self.pending = (EXC_BREAKPOINT, eip - 1, eip - 1, [0])
        elif intno == 1:
            self.pending = (EXC_SINGLE_STEP, eip, eip, [])
        elif intno == 0:
            self.pending = (EXC_DIV_ZERO, eip, eip, [])
        elif intno == 6:
            self.pending = (EXC_ILLEGAL, eip, eip, [])
        elif intno == 13:
            self.pending = (EXC_PRIV, eip, eip, [])
        else:
            self.pending = (EXC_ACCESS, eip, eip, [0, 0xffffffff])
        mu.emu_stop()

    def on_trap(self, mu, address, size, _):
        dll, name = self.traps.get(address, (None, None))
        if dll is None:
            raise SystemExit('jump into the trap area at %#x' % address)
        if dll == '<return>':
            if name == 'handler':
                return self.handler_returned()
            if name == 'probe':
                self.probe_done = True
                return mu.emu_stop()
            raise SystemExit('the stub returned from DllMain without reaching the original entry point')
        self.calls.append((dll, name))
        if self.verbose:
            print('  call %s!%s' % (dll, name))
        a = self.arg
        if name == 'GetModuleHandleA':
            if a(0) == 0:
                return self.ret(self.base, 1)
            self.module_name_ptrs.append(a(0))
            return self.ret(self.module(self.cstr(a(0))), 1)
        if name == 'LoadLibraryA':
            self.module_name_ptrs.append(a(0))
            return self.ret(self.module(self.cstr(a(0))), 1)
        if name == 'GetProcAddress':
            h, p = a(0), a(1)
            fn = p if p < 0x10000 else self.cstr(p)
            return self.ret(self.trap(self.names.get(h, '?%#x' % h), fn), 2)
        if name == 'RaiseException':
            code, n, p = a(0), min(a(2), 15), a(3)
            params = list(struct.unpack('<%dI' % n, mu.mem_read(p, 4 * n))) if n and p else []
            self.ret(0, 4)  # unwind the call: the context resumes after it
            eip = mu.reg_read(UC_X86_REG_EIP)
            if self.verbose:
                print('  RaiseException %#x %s' % (code, [hex(x) for x in params]))
            self.pending = (code, eip, eip, params)
            mu.emu_stop()
            return
        if name in ('GetLocalTime', 'GetSystemTime'):
            # SYSTEMTIME: the week the build was made (a protector may check dates)
            mu.mem_write(a(0), struct.pack('<8H', 2003, 9, 3, 10, 12, 0, 0, 0))
            return self.ret(0, 1)
        if name == 'GetVolumeInformationA':
            if a(1) and a(2):
                mu.mem_write(a(1), b'SYSTEM' + bytes(1))
            for i, v in ((3, 0x1c2d3e4f), (4, 255), (5, 0x700ff)):
                if a(i):
                    mu.mem_write(a(i), struct.pack('<I', v))
            if a(6) and a(7) >= 5:
                mu.mem_write(a(6), b'NTFS' + bytes(1))
            return self.ret(1, 8)
        if name == 'GetDiskFreeSpaceA':
            for i, v in ((1, 8), (2, 512), (3, 0x01000000), (4, 0x02000000)):
                if a(i):
                    mu.mem_write(a(i), struct.pack('<I', v))
            return self.ret(1, 5)
        if name in ('GetComputerNameA', 'GetUserNameA'):
            text = b'VANADIEL' if name == 'GetComputerNameA' else b'player'
            mu.mem_write(a(0), text + bytes(1))
            if a(1):
                mu.mem_write(a(1), struct.pack('<I', len(text) + (name == 'GetUserNameA')))
            return self.ret(1, 2)
        if name in ('GetTempPathA', 'GetCurrentDirectoryA'):
            path = b'C:\\TEMP\\' if name == 'GetTempPathA' else b'C:\\Program Files\\PlayOnline'
            if a(0) > len(path):
                mu.mem_write(a(1), path + bytes(1))
            return self.ret(len(path), 2)
        if name == 'GetSystemTimeAsFileTime':
            mu.mem_write(a(0), struct.pack('<Q', 126_000_000_000_000_000 + 0x0005_0000_0000_0000 // 64))
            return self.ret(0, 1)
        if name == 'GlobalMemoryStatus':
            mu.mem_write(a(0), struct.pack('<8I', 32, 30, 512 << 20, 256 << 20, 1 << 30, 1 << 29, 2 << 30, 1 << 30))
            return self.ret(0, 1)
        if name == 'VirtualQuery':
            addr = a(0)
            regions = [(self.base, self.size, 0x1000000), (HEAP, HEAP_SIZE, 0x20000), (STACK, STACK_SIZE, 0x20000)]
            regions += [(h, MODULE_SPAN, 0x1000000) for h in self.names]
            base, size, kind = next(((b, s, k) for b, s, k in regions if b <= addr < b + s), (addr & ~0xfff, 0x1000, 0))
            page = addr & ~0xfff
            mbi = struct.pack('<7I', page, base, 0x40, base + size - page, 0x1000 if kind else 0x10000, 0x40 if kind else 1, kind)
            if a(2) >= len(mbi):
                mu.mem_write(a(1), mbi)
            return self.ret(len(mbi), 3)
        if name == 'RtlUnwind':
            # A handler taking the exception (Delphi's except blocks): the chain is cut back to its
            # frame, and the handler call never returns to the dispatcher. Intermediate frames'
            # unwind calls are skipped: a stub's finally blocks have nothing a dump needs.
            target = a(0)
            if target:
                mu.mem_write(TEB, struct.pack('<I', target))
            self.in_flight.clear()
            return self.ret(a(3), 4)
        if name == 'TlsAlloc':
            self.tls = getattr(self, 'tls', {})
            i = len(self.tls)
            self.tls[i] = 0
            return self.ret(i, 0)
        if name == 'TlsGetValue':
            return self.ret(getattr(self, 'tls', {}).get(a(0), 0), 1)
        if name == 'TlsSetValue':
            self.tls = getattr(self, 'tls', {})
            self.tls[a(0)] = a(1)
            return self.ret(1, 2)
        if name == 'TlsFree':
            return self.ret(1, 1)
        if name in ('LocalAlloc', 'GlobalAlloc', 'HeapAlloc', 'HeapReAlloc'):
            size = a(1) if name != 'HeapReAlloc' else a(3)
            if name == 'HeapAlloc':
                size = a(2)
            addr = self.alloc(size)
            if name == 'HeapReAlloc' and a(2):
                old = a(2)
                keep = min(self.sizes.get(old, 0), size)
                mu.mem_write(addr, bytes(mu.mem_read(old, keep)))
            return self.ret(addr, {'LocalAlloc': 2, 'GlobalAlloc': 2, 'HeapAlloc': 3, 'HeapReAlloc': 4}[name])
        if name in ('LocalFree', 'GlobalFree'):
            return self.ret(0, 1)
        if name == 'HeapFree':
            return self.ret(1, 3)
        if name in ('GetProcessHeap', 'HeapCreate'):
            return self.ret(0x00150000, 0 if name == 'GetProcessHeap' else 3)
        if name == 'VirtualAlloc':
            size = (a(1) + 0xfff) & ~0xfff
            if a(0):  # a reservation at a chosen address: honour it when it is ours
                return self.ret(a(0), 4)
            self.heap_next = (self.heap_next + 0xffff) & ~0xffff  # allocation granularity, as on Windows
            addr = self.heap_next
            self.heap_next += size
            if self.heap_next > HEAP + HEAP_SIZE:
                raise SystemExit('emulated heap exhausted')
            return self.ret(addr, 4)
        if name == 'VirtualProtect':
            if a(3):
                mu.mem_write(a(3), struct.pack('<I', 0x40))
            return self.ret(1, 4)
        if name == 'GetTickCount':
            self.ticks += 16
            return self.ret(self.ticks, 0)
        if name == 'GetVersion':
            return self.ret(0x0a280105, 0)  # Windows XP SP2
        if name == 'GetCurrentProcess':
            return self.ret(0xffffffff, 0)
        if name in ('GetCurrentProcessId', 'GetCurrentThreadId'):
            return self.ret(0x1000, 0)
        if name == 'GetLastError':
            return self.ret(0, 0)
        if name == 'IsDebuggerPresent':
            return self.ret(0, 0)
        if name == 'GetModuleFileNameA':
            path = b'C:\\Program Files\\PlayOnline\\SquareEnix\\FINAL FANTASY XI\\game.dll\0'
            if a(2):
                mu.mem_write(a(1), path[:a(2)])
            return self.ret(len(path) - 1, 3)
        if name == 'CreateFileA':
            return self.ret(0xffffffff, 7)  # INVALID_HANDLE_VALUE: no \\.\SICE, no files
        if name == 'GetCommandLineA':
            mu.mem_write(SCRATCH, b'pol.exe\0')
            return self.ret(SCRATCH, 0)
        if name in ('MessageBoxA', 'ExitProcess'):
            raise SystemExit('the stub gave up: %s %s' % (name, self.cstr(a(1)) if name == 'MessageBoxA' else ''))
        if name == 'lstrlenA':
            return self.ret(len(self.cstr(a(0), 4096)) if a(0) else 0, 1)
        if name in ('lstrcpyA', 'lstrcatA'):
            src = self.cstr(a(1), 4096).encode('latin-1')
            dst = a(0)
            if name == 'lstrcatA':
                dst += len(self.cstr(a(0), 4096))
            mu.mem_write(dst, src + bytes(1))
            return self.ret(a(0), 2)
        if name in ('lstrcmpA', 'lstrcmpiA'):
            x, y = self.cstr(a(0), 4096), self.cstr(a(1), 4096)
            if name == 'lstrcmpiA':
                x, y = x.lower(), y.lower()
            return self.ret((x > y) - (x < y), 2)
        if name in ('GetWindowsDirectoryA', 'GetSystemDirectoryA'):
            path = b'C:\\WINDOWS' + (b'\\system32' if name == 'GetSystemDirectoryA' else b'')
            if a(1) > len(path):
                mu.mem_write(a(0), path + bytes(1))
            return self.ret(len(path), 2)
        if name in FAIL or name in VALUE:
            nargs, value = FAIL.get(name) or VALUE[name]
            return self.ret(value, nargs)
        if name in QUIET:
            return self.ret(1, QUIET[name])
        global STDCALL
        if STDCALL is None:
            STDCALL = stdcall_sizes()
        if name in STDCALL:
            self.unanswered = getattr(self, 'unanswered', [])
            self.unanswered.append(name)
            if self.verbose:
                print('  (answered %s with a plain success)' % name)
            return self.ret(1, STDCALL[name] // 4)
        raise SystemExit('the stub called %s!%s, which this emulator does not answer' % (dll, name))

    # --- structured exception handling -----------------------------------------------------------
    def save_context(self, at, eip):
        mu = self.mu
        ctx = bytearray(CTX_SIZE)
        struct.pack_into('<I', ctx, 0, 0x1003f)  # CONTEXT_ALL; debug registers stay 0
        for k, r in REGS.items():
            struct.pack_into('<I', ctx, CTX[k], mu.reg_read(r))
        struct.pack_into('<I', ctx, CTX['Eip'], eip)
        for k, v in (('SegCs', 0x1b), ('SegSs', 0x23), ('SegDs', 0x23), ('SegEs', 0x23), ('SegFs', 0x3b)):
            struct.pack_into('<I', ctx, CTX[k], v)
        mu.mem_write(at, bytes(ctx))

    def load_context(self, at):
        mu = self.mu
        ctx = bytes(mu.mem_read(at, CTX_SIZE))
        for k, r in REGS.items():
            v = struct.unpack_from('<I', ctx, CTX[k])[0]
            if k == 'EFlags':
                v = (v & ~0x100) | 0x202
            mu.reg_write(r, v)

    def dispatch(self, code, address, eip, params, frame=None):
        """Call the next handler on the fs:[0] chain the way KiUserExceptionDispatcher does:
        handler(ExceptionRecord, EstablisherFrame, ContextRecord, DispatcherContext).
        Returns the handler's address, with the stack set up for the call."""
        self.exceptions += 1
        if self.exceptions > 200000:
            raise SystemExit('exception storm')
        mu = self.mu
        if frame is None:
            frame = struct.unpack('<I', mu.mem_read(TEB, 4))[0]
        if frame == 0xffffffff:
            raise SystemExit('unhandled exception %#x at %#x' % (code, address))
        handler = struct.unpack('<I', mu.mem_read(frame + 4, 4))[0]
        if self.in_flight and self.in_flight[-1][0] is None:
            ctx_at = self.in_flight.pop()[1]  # searching on: the same context
        else:
            sp = (mu.reg_read(UC_X86_REG_ESP) - 0x400) & ~0xf
            ctx_at = sp - CTX_SIZE
            self.save_context(ctx_at, eip)
        rec_at = ctx_at - 0x60
        rec = struct.pack('<IIIII', code, 0, 0, address, len(params)) + b''.join(struct.pack('<I', p) for p in params)
        mu.mem_write(rec_at, rec.ljust(0x50, b'\0'))
        sp = rec_at - 0x20
        mu.mem_write(sp, struct.pack('<IIIII', self.seh_ret, rec_at, frame, ctx_at, 0))
        mu.reg_write(UC_X86_REG_ESP, sp)
        self.in_flight.append(((code, address, eip, params, frame), ctx_at))
        if self.verbose:
            print('  exception %#x at %#x -> handler %#x' % (code, address, handler))
        return handler

    def handler_returned(self):
        mu = self.mu
        (code, address, eip, params, frame), ctx_at = self.in_flight.pop()
        disposition = mu.reg_read(UC_X86_REG_EAX)
        if disposition == 0:  # ExceptionContinueExecution
            self.load_context(ctx_at)
            return
        if disposition == 1:  # ExceptionContinueSearch
            nxt = struct.unpack('<I', mu.mem_read(frame, 4))[0]
            self.in_flight.append((None, ctx_at))
            mu.reg_write(UC_X86_REG_EIP, self.dispatch(code, address, eip, params, nxt))
            return
        raise SystemExit('handler returned disposition %d' % disposition)

    def go(self):
        start = self.entry
        while True:
            self.pending = None
            try:
                self.mu.emu_start(start, 0, count=self.limit)
            except UcError as e:
                eip = self.mu.reg_read(UC_X86_REG_EIP)
                if e.errno in FAULTS:
                    write = e.errno in (UC_ERR_WRITE_UNMAPPED, UC_ERR_WRITE_PROT)
                    self.pending = (EXC_ACCESS, eip, eip, [1 if write else 0, 0])
                elif e.errno == UC_ERR_INSN_INVALID:
                    self.pending = (EXC_ILLEGAL, eip, eip, [])
                else:
                    raise SystemExit('emulation failed: %s at eip %#x' % (e, eip))
            if self.oep is not None:
                break
            if self.pending is None:
                raise SystemExit('stopped at %#x before the first section (instruction limit?)'
                                 % self.mu.reg_read(UC_X86_REG_EIP))
            start = self.dispatch(*self.pending)
        self.image = bytes(self.mu.mem_read(self.base, self.size))
        return self

    def probe(self, fn, args, limit=200000):
        """Call `fn(*args)` (stdcall or cdecl) after the run and return eax, or None if it faults
        or does not return: used to tell what one of a protector's own API emulations is."""
        mu = self.mu
        saved = {r: mu.reg_read(r) for r in REGS.values()}
        sp = STACK + STACK_SIZE // 2
        mu.mem_write(sp, struct.pack('<I', self.trap('<return>', 'probe')) + b''.join(struct.pack('<I', x) for x in args))
        mu.reg_write(UC_X86_REG_ESP, sp)
        self.probe_done = False
        try:
            mu.emu_start(fn, 0, count=limit)
            result = mu.reg_read(UC_X86_REG_EAX) if self.probe_done else None
        except (UcError, SystemExit):
            result = None
        for r, v in saved.items():
            mu.reg_write(r, v)
        return result

    def identify_emulation(self, fn):
        """Which API a protector's in-heap emulation stands in for, by asking it."""
        k32 = self.module('kernel32.dll')
        mu = self.mu
        mu.mem_write(SCRATCH + 0x100, b'GetTickCount' + bytes(1))
        if self.probe(fn, [k32, SCRATCH + 0x100]) == self.trap('kernel32.dll', 'GetTickCount'):
            return ('kernel32.dll', 'GetProcAddress')
        if self.probe(fn, [0]) == self.base:
            return ('kernel32.dll', 'GetModuleHandleA')
        if self.probe(fn, [0x13572468]) == 0x13572468:
            return ('kernel32.dll', 'LockResource')  # Win32 LockResource returns its handle
        r = self.probe(fn, [])
        known = {0x0a280105: 'GetVersion', SCRATCH: 'GetCommandLineA', 0xffffffff: 'GetCurrentProcess',
                 0x1000: 'GetCurrentProcessId'}
        if r in known:
            return ('kernel32.dll', known[r])
        return None
