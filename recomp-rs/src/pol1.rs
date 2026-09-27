//! Static POL1 unpacker (tools/pol1_unpack.py): rebuild a POL1-packed PlayOnline/FFXI DLL's .text
//! without running it.
//!
//! The POL1 stub, on DLL_PROCESS_ATTACH only, decompresses the POL1 section into .text, applies a
//! private .text relocation table with a delta hard-coded to 0 (a no-op at the preferred base) and
//! jumps to the real entry point. The compression is a plain LZSS:
//!
//!   flag byte, then 8 items, most significant flag bit first
//!     bit 1: literal byte
//!     bit 0: two bytes b0 b1; offset = ((b0 << 8) | b1) & 0xfff, length = (b0 >> 4) + 3,
//!            copy byte by byte from (out - offset) (overlap allowed); offset 0 ends the stream
//!
//! The stub's immediates (src_len, dst_len, the original entry point) are parsed out of the stub
//! itself, so this works for any POL1 module whose stub has the same shape; it refuses otherwise.
use crate::disasm::Disasm;
use crate::pe::Pe;
use crate::Error;
use std::path::Path;

fn fail(msg: String) -> Error {
    Error::Unpack(msg)
}

pub fn lzss_decompress(src: &[u8], expected_len: usize) -> Result<Vec<u8>, Error> {
    let mut out: Vec<u8> = Vec::with_capacity(expected_len);
    let mut i = 0;
    let at = |i: usize| src.get(i).copied().ok_or_else(|| fail(format!("compressed stream truncated at src {:#x}", i)));
    while i < src.len() {
        let mut flags = src[i];
        i += 1;
        for _ in 0..8 {
            if flags & 0x80 != 0 {
                // `shl bl,1 ; jae match`: carry set = literal
                out.push(at(i)?);
                i += 1;
            } else {
                let (b0, b1) = (at(i)?, at(i + 1)?);
                let offset = (((b0 as usize) << 8) | b1 as usize) & 0xFFF;
                if offset == 0 {
                    return Ok(out);
                }
                i += 2;
                let length = (b0 >> 4) as usize + 3;
                if out.len() < offset {
                    return Err(fail(format!("back-reference before start of output at src {:#x}", i)));
                }
                let start = out.len() - offset;
                for k in 0..length {
                    out.push(out[start + k]);
                }
            }
            flags <<= 1;
            if out.len() > expected_len + 0x1000 {
                return Err(fail("output overran the expected .text size".into()));
            }
        }
    }
    Err(fail("stream ended without an end marker".into()))
}

/// Python int(s, 16) for the "0x..." operands capstone prints.
fn hex(s: &str) -> Result<u64, Error> {
    let t = s.trim();
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    u64::from_str_radix(&t.replace('_', ""), 16).map_err(|_| fail(format!("invalid literal for int() with base 16: '{}'", s)))
}

/// The stub's (src_len, dst_len, original entry rva).
///
/// Expected shape (addresses from FFXiMain 2026-08-22):
///   cmp byte ptr [esp+8],1 ; jne <tail>          ; DLL_PROCESS_ATTACH only
///   ... mov eax,<dst_len> ; ... push <src_len> ; ... call <decompress>
///   ... <tail>: jmp <original entry>              ; the last instruction before <decompress>
pub fn parse_stub(pe: &Pe) -> Result<(u64, u64, u64), Error> {
    let ep = pe.entry as u64;
    let code = pe.get_data(ep, Some(0x100))?;
    if code.len() < 5 || &code[..5] != b"\x80\x7c\x24\x08\x01" {
        let head: String = code.iter().take(5).map(|b| format!("{:02x}", b)).collect();
        return Err(fail(format!("entry point is not a POL1 stub (unexpected prologue {})", head)));
    }
    let mut md = Disasm::new(false);
    let mut insns = Vec::new();
    md.lite(code, ep, |l| insns.push(l));
    let (mut dst_len, mut src_len, mut decompress) = (None, None, None);
    let mut last_push = None;
    for l in &insns {
        if l.mnemonic == "mov" && l.op_str.starts_with("eax, 0x") && dst_len.is_none() {
            dst_len = Some(hex(&l.op_str[5..])?);
        } else if l.mnemonic == "push" && l.op_str.starts_with("0x") {
            last_push = Some(hex(&l.op_str)?);
        } else if l.mnemonic == "call" {
            decompress = Some(hex(&l.op_str)?);
            src_len = last_push;
            break;
        }
    }
    let (Some(src_len), Some(dst_len), Some(decompress)) = (src_len, dst_len, decompress) else {
        return Err(fail("could not read dst_len/src_len/decompressor from the stub".into()));
    };
    let mut oep = None;
    for l in &insns {
        if l.address >= decompress {
            break;
        }
        if l.mnemonic == "jmp" && l.op_str.starts_with("0x") {
            oep = Some(hex(&l.op_str)?); // keep the last one before the decompressor
        }
    }
    let oep = oep.ok_or_else(|| fail("no jmp to the original entry point before the decompressor".into()))?;
    Ok((src_len, dst_len, oep))
}

/// Unpacks `src` into `out`: the input with .text filled in and marked as code, entry point at
/// the original one (like unpacked/rebuild.py produces from a LoadLibrary dump). `log` gets the
/// lines pol1_unpack.py prints.
pub fn unpack(src: &Path, out: &Path, log: &mut dyn FnMut(String)) -> Result<(), Error> {
    let pe = Pe::parse(crate::read(src)?)?;
    let text = pe.section(b".text")?;
    let pol1 = pe.section(b"POL1")?;
    if text.size_of_raw_data != 0 {
        return Err(fail(".text already has raw data: not packed".into()));
    }
    let (src_len, dst_len, oep) = parse_stub(&pe)?;
    log(format!("stub: src_len={:#x} dst_len={:#x} original_entry={:#x} (image {:#x})",
                src_len, dst_len, oep, pe.image_base as u64 + oep));
    if dst_len != text.virtual_size as u64 {
        return Err(fail(format!("dst_len {:#x} != .text virtual size {:#x}", dst_len, text.virtual_size)));
    }
    let packed = pe.section_data(pol1, None, None);
    let code = lzss_decompress(&packed[..(src_len as usize).min(packed.len())], dst_len as usize)?;
    if code.len() as u64 != dst_len {
        return Err(fail(format!("decompressed {:#x} bytes, expected {:#x}", code.len(), dst_len)));
    }
    log(format!("decompressed {} bytes", code.len()));

    // Splice: .text gets raw data appended at the end of the file, marked as code. pefile's
    // write() repacks every header it parsed; only these four fields change.
    let align = pe.file_alignment as usize;
    let text_header = text.header_offset;
    let text_characteristics = text.characteristics;
    let opt = pe.opt_offset;
    let mut raw = pe.data;
    let pad = if align == 0 { 0 } else { (align - raw.len() % align) % align };
    raw.resize(raw.len() + pad, 0);
    let text_ptr = raw.len() as u32;
    let blob_len = code.len() + if align == 0 { 0 } else { (align - code.len() % align) % align };
    raw.extend_from_slice(&code);
    raw.resize(text_ptr as usize + blob_len, 0);
    let put = |raw: &mut Vec<u8>, at: usize, v: u32| raw[at..at + 4].copy_from_slice(&v.to_le_bytes());
    put(&mut raw, text_header + 20, text_ptr); // PointerToRawData
    put(&mut raw, text_header + 16, blob_len as u32); // SizeOfRawData
    put(&mut raw, text_header + 36, (text_characteristics & !0x80) | 0x20); // CNT_UNINITIALIZED_DATA -> CNT_CODE
    put(&mut raw, opt + 16, oep as u32); // AddressOfEntryPoint: skip the stub, .text is already unpacked
    std::fs::write(out, &raw).map_err(|e| Error::io(out, e))?;
    log(format!("wrote {}", out.display()));
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn lzss() {
        // three literals, a 3-byte back-reference 3 back, then the end marker
        let src = [0xE0, b'a', b'b', b'c', 0x00, 0x03, 0x00, 0x00];
        assert_eq!(super::lzss_decompress(&src, 6).unwrap(), b"abcabc");
        assert!(super::lzss_decompress(&[0x00, 0x00, 0x05], 6).is_err()); // reference before the start
    }
}
