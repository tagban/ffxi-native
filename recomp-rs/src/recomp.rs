//! The recompiler driver (recomp/recomp.py): translate a module's functions and write
//! funcs.h (prototypes), funcs_NNN.c (translations) and table.c (address -> function table for
//! indirect calls, and the pinned retail build's constants).
//!
//! `functions` translates those entries plus everything they reach by direct call or tail jump
//! (the closure), which is what a differential test needs; `all` translates every function.
//!
//! `hooks` names instructions where the host may step in (meta/builds.json "hooks"): before each,
//! the translation calls the host function pointer rt_hook_<name> (GuestFn, defined in table.c,
//! NULL until the host sets it) with the guest's registers stored, and reloads them after.
use crate::disasm::Disasm;
use crate::pe::Pe;
use crate::program::Program;
use crate::x86c::{FunctionTranslator, Translated};
use crate::{write_if_changed, Error, Event};
use indexmap::IndexMap;
use rayon::prelude::*;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;

/// recomp.py's command line.
#[derive(Clone, Debug)]
pub struct Options {
    pub meta: PathBuf,
    pub image: PathBuf,
    pub out: PathBuf,
    /// the retail (POL1-packed) DLL the loader rebuilds .text from
    pub retail: PathBuf,
    pub functions: Vec<u64>,
    pub all: bool,
    pub stats: bool,
    pub chunk: usize,
    /// (name, address)
    pub hooks: Vec<(String, u64)>,
    /// a second module (e.g. ffxi for FFXi.dll): functions are named <module>_XXXXXXXX, the module
    /// has its own relocation delta, and table.c defines RtModule rt_module_<module> instead of the
    /// rt_table/rt_image_* globals FFXiMain uses
    pub module: String,
    /// worker threads; 0 = rayon's default (one per core)
    pub threads: usize,
}

impl Options {
    pub fn new(meta: PathBuf, image: PathBuf, retail: PathBuf, out: PathBuf) -> Options {
        Options { meta, image, out, retail, functions: Vec::new(), all: true, stats: false, chunk: 400, hooks: Vec::new(),
                  module: String::new(), threads: 0 }
    }
}

/// What a run translated.
#[derive(Clone, Debug, Default)]
pub struct Report {
    pub module: String,
    pub functions: usize,
    pub with_unimpl: usize,
    /// unimplemented instruction counts, most common first (all of them; recomp.py prints 40)
    pub unimpl: Vec<(String, usize)>,
    /// entry patches (jmp, int3); FFXiMain only
    pub patches: Option<(usize, usize)>,
}

/// Progress lines every this many functions (the launcher's progress bar).
const PROGRESS_STEP: usize = 250;

/// Runs the recompiler; `on` gets recomp.py's printed lines as messages and its
/// `@progress translate <module> <done> <total>` points.
pub fn run(opts: &Options, on: &mut dyn FnMut(Event)) -> Result<Report, Error> {
    let meta = crate::read_json(&opts.meta)?;
    let pe = Pe::parse(crate::read(&opts.image)?)?;
    let prefix = if opts.module.is_empty() { "f_".to_string() } else { format!("{}_", opts.module) };
    let hooks: HashMap<u64, String> = opts.hooks.iter().map(|(n, a)| (*a, n.clone())).collect();
    let prog = Program::new(&meta, &pe, &prefix, hooks)?;
    drop(pe);
    let what = if opts.module.is_empty() { "main" } else { opts.module.as_str() };

    let mut todo: Vec<u64> = if opts.all {
        let mut v: Vec<u64> = prog.entries.iter().copied().collect();
        v.sort();
        v
    } else {
        for &e in &opts.functions {
            if !prog.entries.contains(&e) {
                return Err(Error::Translate(format!("{:#x} is not a function entry in the metadata", e)));
            }
        }
        opts.functions.clone()
    };

    // translated, in the order recomp.py's queue pops them (the unimplemented-instruction counts
    // keep that first-seen order for ties)
    let mut done: Vec<(u64, Translated)> = Vec::new();
    if opts.all {
        todo.reverse(); // queue.pop() takes the highest entry first
        done = translate_all(&prog, &todo, what, opts.threads, on)?;
    } else {
        // the closure, one at a time (references in sorted order: recomp.py extends its queue from
        // a set, so its order, and so the order of ties in the counts, may differ)
        let mut md = Disasm::new(true);
        let mut seen = HashSet::new();
        let mut queue = todo.clone();
        while let Some(e) = queue.pop() {
            if !seen.insert(e) {
                continue;
            }
            let t = FunctionTranslator::new(&prog, &mut md, e, &prog.functions[&e]).translate()?;
            if seen.len() % PROGRESS_STEP == 0 {
                on(Event::Progress { phase: "translate", what, done: seen.len(), total: todo.len().max(seen.len()) });
            }
            let mut refs = t.referenced.clone();
            refs.sort();
            refs.dedup();
            queue.extend(refs.into_iter().filter(|r| !seen.contains(r)));
            done.push((e, t));
        }
    }

    let mut unimpl: IndexMap<String, usize> = IndexMap::new();
    let mut unimpl_funcs = HashSet::new();
    let mut hooked = HashSet::new();
    for (e, t) in &done {
        for (_, mn, why) in &t.unimpl {
            let key = if why.starts_with("x87") { why } else { mn };
            *unimpl.entry(key.clone()).or_insert(0) += 1;
            unimpl_funcs.insert(*e);
        }
        hooked.extend(t.hooked.iter().copied());
    }
    let mut counts: Vec<(String, usize)> = unimpl.into_iter().collect();
    counts.sort_by(|a, b| b.1.cmp(&a.1)); // stable: Counter.most_common
    let mut by_entry: Vec<(u64, Vec<String>)> = done.into_iter().map(|(e, t)| (e, t.lines)).collect();
    by_entry.sort_by_key(|x| x.0);
    let entries: Vec<u64> = by_entry.iter().map(|x| x.0).collect();

    std::fs::create_dir_all(&opts.out).map_err(|e| Error::io(&opts.out, e))?;
    // A second module relocates independently of FFXiMain: its translation reads its own delta.
    let delta = if opts.module.is_empty() {
        String::new()
    } else {
        format!("\n#undef RD\n#define RD rt_delta_{}\nextern uint32_t rt_delta_{};\n", opts.module, opts.module)
    };
    let mut hook_names: Vec<&String> = prog.hooks.values().collect();
    hook_names.sort();
    let mut h = format!("/* generated by recomp.py - do not edit, do not commit */\n#pragma once\n#include \"guest.h\"\n{}\n", delta);
    for e in &entries {
        h += &format!("void {}{:08x}(Guest* g);\n", prog.prefix, e);
    }
    for name in &hook_names {
        h += &format!("extern GuestFn rt_hook_{};\n", name);
    }
    write(&opts.out.join("funcs.h"), &h)?;

    // chunks are independent files: built and written in parallel (same bytes, any order)
    let chunk = opts.chunk.max(1);
    let parts: Vec<(usize, &[(u64, Vec<String>)])> = by_entry.chunks(chunk).enumerate().collect();
    let pool = rayon::ThreadPoolBuilder::new().num_threads(opts.threads).build()
        .map_err(|e| Error::Translate(format!("thread pool: {}", e)))?;
    let written: Result<Vec<String>, Error> = pool.install(|| {
        parts.par_iter().map(|(k, part)| {
            let name = format!("funcs_{:03}.c", k);
            let mut text = String::from("/* generated by recomp.py - do not edit, do not commit */\n#include \"funcs.h\"\n\n\
                #if defined(_MSC_VER)\n#pragma warning(disable: 4102 4189 4101 4702)\n\
                #pragma code_seg(\".xlat\") /* translated code in its own section: the profiler tells it from the runtime */\n\
                #endif\n\n");
            for (_, lines) in part.iter() {
                for l in lines {
                    text += l;
                    text.push('\n');
                }
                text.push('\n');
            }
            write_if_changed(&opts.out.join(&name), &text).map(|_| name)
        }).collect()
    });
    let chunks: HashSet<String> = written?.into_iter().collect();
    let listing = std::fs::read_dir(&opts.out).map_err(|e| Error::io(&opts.out, e))?;
    for f in listing {
        let f = f.map_err(|e| Error::io(&opts.out, e))?;
        let name = f.file_name().to_string_lossy().into_owned();
        if name.starts_with("funcs_") && name.ends_with(".c") && !chunks.contains(&name) {
            std::fs::remove_file(f.path()).map_err(|e| Error::io(&f.path(), e))?;
        }
    }
    let mut missed: Vec<u64> = prog.hooks.keys().filter(|a| !hooked.contains(*a)).copied().collect();
    missed.sort();
    if !missed.is_empty() && opts.all {
        let list: Vec<String> = missed.iter().map(|a| format!("{}={:#x}", prog.hooks[a], a)).collect();
        return Err(Error::Translate(format!("--hooks: {} is not an instruction in any translated function", list.join(", "))));
    }
    let kinds = patch_kinds(&prog, &entries);
    let img = image_constants(&prog, &opts.retail, &opts.image)?;

    let mut report = Report { module: what.to_string(), functions: entries.len(), with_unimpl: unimpl_funcs.len(),
                              unimpl: counts.clone(), patches: None };
    let mut t = String::from("/* generated by recomp.py - do not edit, do not commit */\n#include \"runtime.h\"\n#include \"funcs.h\"\n\n");
    let top: Vec<String> = counts.iter().take(40).map(|(k, v)| format!("  {:7}  {}", v, k)).collect();
    if !opts.module.is_empty() {
        // A second module: one descriptor the runtime registers (rt_add_module), and its delta.
        let m = &opts.module;
        t += &format!("uint32_t rt_delta_{};\n\nstatic const RtEntry table[] = {{\n", m);
        for e in &entries {
            t += &format!("    {{ 0x{:08X}u, {}{:08x} }},\n", e, prog.prefix, e);
        }
        t += "};\n\n/* The pinned retail build this translation belongs to. */\n";
        t += &format!("const RtModule rt_module_{} = {{\n    \"{}\", table, {}, &rt_delta_{},\n", m, m, entries.len(), m);
        let order = ["base", "timestamp", "size", "text_rva", "text_size", "pol1_rva", "pol1_src_len", "oep", "reloc_rva"];
        let vals: Vec<String> = order.iter().map(|k| format!("0x{:X}u", img.iter().find(|x| x.0 == *k).unwrap().1)).collect();
        t += &format!("    {}", vals.join(", "));
        t += ",\n};\n";
        write(&opts.out.join("table.c"), &t)?;
        on(Event::Message(&format!("module {}: translated {} functions ({} with unimplemented instructions)",
                                   m, entries.len(), unimpl_funcs.len())));
        if opts.stats || !counts.is_empty() {
            for l in &top {
                on(Event::Message(l));
            }
        }
        return Ok(report);
    }
    for name in &hook_names {
        t += &format!("GuestFn rt_hook_{}; /* recomp.py --hooks */\n", name);
    }
    if !prog.hooks.is_empty() {
        t += "\n";
    }
    t += "const RtEntry rt_table[] = {\n";
    for e in &entries {
        t += &format!("    {{ 0x{:08X}u, f_{:08x} }},\n", e, e);
    }
    t += &format!("}};\nconst unsigned rt_table_count = {};\n\n", entries.len());
    t += "/* How each entry is redirected into its translation: 0 = 5-byte jmp, 1 = int3 (too close\n \
          * to the next entry, or a body shorter than 5 bytes with no padding after it). */\n";
    t += "const unsigned char rt_table_patch[] = {\n";
    for row in kinds.chunks(32) {
        let r: Vec<String> = row.iter().map(|k| k.to_string()).collect();
        t += &format!("    {},\n", r.join(","));
    }
    t += "};\n\n";
    t += "/* The pinned retail build this translation belongs to. */\n";
    for (name, value) in &img {
        t += &format!("const uint32_t rt_image_{} = 0x{:X}u;\n", name, value);
    }
    write(&opts.out.join("table.c"), &t)?;
    let jmp = kinds.iter().filter(|&&k| k == 0).count();
    report.patches = Some((jmp, kinds.len() - jmp));
    on(Event::Message(&format!("entry patches: {} jmp, {} int3", jmp, kinds.len() - jmp)));
    on(Event::Message(&format!("translated {} functions ({} with unimplemented instructions)", entries.len(), unimpl_funcs.len())));
    if opts.stats || !counts.is_empty() {
        let total: usize = counts.iter().map(|c| c.1).sum();
        on(Event::Message(&format!("unimplemented instructions: {}", total)));
        for l in &top {
            on(Event::Message(l));
        }
    }
    Ok(report)
}

fn write(path: &std::path::Path, text: &str) -> Result<(), Error> {
    std::fs::write(path, text).map_err(|e| Error::io(path, e))
}

/// Translates `order` in parallel (one capstone handle per worker); the results come back in
/// `order`. Progress is reported from this thread, in order, every PROGRESS_STEP functions.
fn translate_all(prog: &Program, order: &[u64], what: &str, threads: usize, on: &mut dyn FnMut(Event))
    -> Result<Vec<(u64, Translated)>, Error>
{
    let total = order.len();
    let counter = AtomicUsize::new(0);
    let (tx, rx) = mpsc::channel::<usize>();
    let pool = rayon::ThreadPoolBuilder::new().num_threads(threads).build()
        .map_err(|e| Error::Translate(format!("thread pool: {}", e)))?;
    let results = std::thread::scope(|s| {
        let worker = s.spawn(|| {
            let tx = tx; // dropped when the work is done, which ends the loop below
            pool.install(|| {
                order.par_iter().map_init(|| (Disasm::new(true), tx.clone()), |(md, tx), &e| {
                    let r = FunctionTranslator::new(prog, md, e, &prog.functions[&e]).translate();
                    let n = counter.fetch_add(1, Ordering::Relaxed) + 1;
                    if n.is_multiple_of(PROGRESS_STEP) {
                        let _ = tx.send(n);
                    }
                    r.map(|t| (e, t))
                }).collect::<Vec<_>>()
            })
        });
        let mut emitted = 0;
        for n in rx {
            while emitted + PROGRESS_STEP <= n {
                emitted += PROGRESS_STEP;
                on(Event::Progress { phase: "translate", what, done: emitted, total });
            }
        }
        worker.join()
    });
    let results = results.map_err(|_| Error::Translate("a translation thread panicked".into()))?;
    results.into_iter().collect()
}

/// 0 if a 5-byte jmp fits at the entry without touching another entry or live bytes, else 1.
fn patch_kinds(prog: &Program, entries: &[u64]) -> Vec<u8> {
    let mut all: Vec<u64> = prog.entries.iter().copied().collect();
    all.sort();
    let nxt: HashMap<u64, u64> = all.windows(2).map(|w| (w[0], w[1])).collect();
    entries
        .iter()
        .map(|&e| {
            let mut ok = !prog.inner.contains(&e) && nxt.get(&e).copied().unwrap_or(e + 5) as i64 - e as i64 >= 5;
            if ok {
                let ranges = &prog.functions[&e];
                let end = ranges.iter().filter(|&&(lo, hi)| lo <= e && e < hi).map(|r| r.1).max().unwrap_or(e);
                if end < e + 5 {
                    let tail = prog.read(end, e + 5 - end);
                    ok = tail.iter().all(|b| *b == 0xCC || *b == 0x90);
                }
            }
            if ok { 0 } else { 1 }
        })
        .collect()
}

/// What the loader needs to rebuild .text from the retail DLL and check it is the right build.
///
/// A build packed with something other than POL1 (the 2003 ASProtect builds) cannot be unpacked by
/// the loader: it maps the unpacked image instead, already whole. Its constants are the unpacked
/// image's, with pol1_src_len 0 and reloc_rva its relocation directory (which covers .text too).
fn image_constants(prog: &Program, retail: &std::path::Path, image: &std::path::Path)
    -> Result<Vec<(&'static str, u64)>, Error> {
    let pe = Pe::parse(crate::read(retail)?)?;
    if pe.section(b"POL1").is_err() {
        let un = Pe::parse(crate::read(image)?)?;
        let text = un.section(b".text")?;
        return Ok(vec![
            ("base", prog.base),
            ("reloc_rva", un.dirs.get(5).map(|d| d.0).unwrap_or(0) as u64),
            ("timestamp", un.timestamp as u64),
            ("size", un.size_of_image as u64),
            ("text_rva", text.virtual_address as u64),
            ("text_size", text.virtual_size as u64),
            ("pol1_rva", 0),
            ("pol1_src_len", 0),
            ("oep", prog.base + un.entry as u64),
        ]);
    }
    let text = pe.section(b".text")?;
    let pol1 = pe.section(b"POL1")?;
    let (src_len, dst_len, oep) = crate::pol1::parse_stub(&pe)?;
    let reloc = pe.section(b".reloc")?;
    Ok(vec![
        ("base", prog.base),
        ("reloc_rva", reloc.virtual_address as u64),
        ("timestamp", pe.timestamp as u64),
        ("size", pe.size_of_image as u64),
        ("text_rva", text.virtual_address as u64),
        ("text_size", dst_len),
        ("pol1_rva", pol1.virtual_address as u64),
        ("pol1_src_len", src_len),
        ("oep", prog.base + oep),
    ])
}
