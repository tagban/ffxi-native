//! xi-recomp: the prepare and translate steps of tools/build_posix.py, without Python.
//!
//!   xi-recomp prepare --game <FINAL FANTASY XI folder> [--root <engine root>] [--progress]
//!   xi-recomp translate [--root <engine root>] [--progress] [--threads N]
//!   xi-recomp recomp --meta <meta.json> --image <unpacked.dll> --retail <retail.dll> --out <dir>
//!       [--functions 0x..,..] [--all] [--stats] [--chunk N] [--hooks name=0x..,..] [--module m] [--threads N]
//!   xi-recomp unpack <packed.dll> <out.dll>
//!
//! --root defaults to the current directory. --progress (or FFXI_PROGRESS=1) prints the
//! `@progress <phase> <what> <done> <total>` lines the launcher reads.
use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;
use xi_recomp::{recomp, Error, Event};

const USAGE: &str = "usage:
  xi-recomp prepare --game <FINAL FANTASY XI folder> [--root <engine root>] [--progress]
  xi-recomp translate [--root <engine root>] [--progress] [--threads N]
  xi-recomp recomp --meta <meta.json> --image <unpacked.dll> --retail <retail.dll> --out <dir>
      [--functions 0x..,..] [--all] [--stats] [--chunk N] [--hooks name=0x..,..] [--module m] [--threads N]
  xi-recomp unpack <packed.dll> <out.dll>";

struct Args {
    flags: Vec<(String, Option<String>)>,
    positional: Vec<String>,
}

/// --name value / --name=value / --switch; SWITCHES take no value.
fn parse(args: &[String]) -> Args {
    const SWITCHES: [&str; 3] = ["--progress", "--all", "--stats"];
    let mut a = Args { flags: Vec::new(), positional: Vec::new() };
    let mut i = 0;
    while i < args.len() {
        let s = &args[i];
        if let Some((k, v)) = s.split_once('=').filter(|_| s.starts_with("--")) {
            a.flags.push((k.to_string(), Some(v.to_string())));
        } else if SWITCHES.contains(&s.as_str()) {
            a.flags.push((s.clone(), None));
        } else if s.starts_with("--") {
            a.flags.push((s.clone(), args.get(i + 1).cloned()));
            i += 1;
        } else {
            a.positional.push(s.clone());
        }
        i += 1;
    }
    a
}

impl Args {
    fn get(&self, k: &str) -> Option<&str> {
        self.flags.iter().rev().find(|f| f.0 == k).and_then(|f| f.1.as_deref())
    }
    fn has(&self, k: &str) -> bool {
        self.flags.iter().any(|f| f.0 == k)
    }
    fn need(&self, k: &str) -> Result<&str, String> {
        self.get(k).ok_or_else(|| format!("{} is required\n{}", k, USAGE))
    }
    fn threads(&self) -> Result<usize, String> {
        self.get("--threads").map(|t| t.parse().map_err(|_| format!("--threads {}: not a number", t))).unwrap_or(Ok(0))
    }
}

fn hex(s: &str) -> Result<u64, String> {
    xi_recomp::buildinfo::parse_hex(s).map_err(|e| e.to_string())
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let Some(cmd) = argv.first() else {
        eprintln!("{}", USAGE);
        return ExitCode::from(2);
    };
    let a = parse(&argv[1..]);
    let progress = a.has("--progress") || std::env::var("FFXI_PROGRESS").as_deref() == Ok("1");
    let mut on = |e: Event| {
        let mut out = std::io::stdout().lock();
        match e {
            Event::Message(m) => {
                let _ = writeln!(out, "{}", m);
            }
            p => {
                if progress {
                    let _ = writeln!(out, "{}", p.progress_line().unwrap());
                    let _ = out.flush();
                }
            }
        }
    };
    // absolute, as the Python tools' ROOT is (it shows in the lines prepare prints)
    let root = PathBuf::from(a.get("--root").unwrap_or("."));
    let root = std::path::absolute(&root).unwrap_or(root);
    let r: Result<(), String> = (|| match cmd.as_str() {
        "prepare" => {
            let game = PathBuf::from(a.need("--game")?);
            xi_recomp::prepare(&game, &root, &mut on).map(|_| ()).map_err(|e| e.to_string())
        }
        "translate" => {
            let threads = a.threads()?;
            let b = xi_recomp::buildinfo::current(&root).map_err(|e| e.to_string())?
                .ok_or_else(|| "generated/build.json missing: run xi-recomp prepare first".to_string())?;
            xi_recomp::buildinfo::write_build_h(&root, &b).map_err(|e| e.to_string())?;
            for mut o in xi_recomp::translate_options(&root).map_err(|e| e.to_string())? {
                o.threads = threads;
                recomp::run(&o, &mut on).map_err(|e| e.to_string())?;
            }
            Ok(())
        }
        "recomp" => {
            let mut o = recomp::Options::new(a.need("--meta")?.into(), a.need("--image")?.into(), a.need("--retail")?.into(),
                                             a.need("--out")?.into());
            o.all = a.has("--all");
            o.stats = a.has("--stats");
            o.threads = a.threads()?;
            if let Some(c) = a.get("--chunk") {
                o.chunk = c.parse().map_err(|_| format!("--chunk {}: not a number", c))?;
            }
            if let Some(f) = a.get("--functions") {
                o.functions = f.split(',').filter(|x| !x.is_empty()).map(hex).collect::<Result<_, _>>()?;
            }
            if let Some(h) = a.get("--hooks") {
                for kv in h.split(',').filter(|x| !x.is_empty()) {
                    let (name, addr) = kv.split_once('=').ok_or_else(|| format!("--hooks: {} is not name=0xADDR", kv))?;
                    o.hooks.push((name.to_string(), hex(addr)?));
                }
            }
            o.module = a.get("--module").unwrap_or("").to_string();
            recomp::run(&o, &mut on).map(|_| ()).map_err(|e| e.to_string())
        }
        "unpack" => {
            let [src, out] = a.positional.as_slice() else { return Err(USAGE.to_string()) };
            let mut log = |s: String| println!("{}", s);
            xi_recomp::pol1::unpack(src.as_ref(), out.as_ref(), &mut log).map_err(|e: Error| e.to_string())
        }
        _ => Err(USAGE.to_string()),
    })();
    match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{}", e);
            ExitCode::FAILURE
        }
    }
}
