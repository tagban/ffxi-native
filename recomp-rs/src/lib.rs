//! FFXiMain.dll / FFXi.dll static recompiler: the tools/prepare.py + recomp/recomp.py pipeline
//! without Python, as a library a launcher can call in-process.
//!
//!   prepare(game, root, on)   identify the install's build (meta/builds.json), verify and copy the
//!                             retail DLLs into <root>/generated, unpack them (POL1), record
//!                             generated/build.json and write generated/build.h
//!   translate(root, on)       both modules to C: generated/all (FFXiMain) and generated/ffxi
//!
//! The output is byte-identical to the Python tools'. Everything under generated/ is derived from
//! Square Enix's code and is never committed (README, "Rules").
//!
//! No global state: each call reads what it needs from `root`; `on` receives progress points and
//! the lines the Python tools print, on the calling thread.
pub mod buildinfo;
pub mod disasm;
pub mod pe;
pub mod pol1;
pub mod program;
pub mod recomp;
pub mod x86c;

use std::fmt;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub enum Error {
    Io { path: PathBuf, err: std::io::Error },
    Pe(String),
    Unpack(String),
    Meta(String),
    Translate(String),
    /// the install is not a build meta/builds.json knows, or its two DLLs do not match
    Install(String),
}

impl Error {
    pub fn io(path: &Path, err: std::io::Error) -> Error {
        Error::Io { path: path.to_path_buf(), err }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io { path, err } => write!(f, "{}: {}", path.display(), err),
            Error::Pe(s) => write!(f, "PE: {}", s),
            Error::Unpack(s) | Error::Meta(s) | Error::Translate(s) | Error::Install(s) => f.write_str(s),
        }
    }
}

impl std::error::Error for Error {}

/// What the pipeline reports as it goes.
#[derive(Clone, Copy, Debug)]
pub enum Event<'a> {
    /// `@progress <phase> <what> <done> <total>` (tools/build_posix.py's progress lines)
    Progress { phase: &'a str, what: &'a str, done: usize, total: usize },
    /// a line the Python tools print
    Message(&'a str),
}

impl Event<'_> {
    /// The line FFXI_PROGRESS=1 prints for a progress point.
    pub fn progress_line(&self) -> Option<String> {
        match self {
            Event::Progress { phase, what, done, total } => Some(format!("@progress {} {} {} {}", phase, what, done, total)),
            Event::Message(_) => None,
        }
    }
}

/// The build prepare identified.
#[derive(Clone, Debug)]
pub struct Prepared {
    pub build: String,
    /// the game folder as recorded in generated/build.json (normalized)
    pub game: String,
    pub unpacked: PathBuf,
}

pub(crate) fn read(path: &Path) -> Result<Vec<u8>, Error> {
    std::fs::read(path).map_err(|e| Error::io(path, e))
}

pub(crate) fn read_json(path: &Path) -> Result<serde_json::Value, Error> {
    let text = std::fs::read_to_string(path).map_err(|e| Error::io(path, e))?;
    serde_json::from_str(&text).map_err(|e| Error::Meta(format!("{}: {}", path.display(), e)))
}

/// Leave unchanged files alone so an incremental C build only recompiles what moved. (Compared
/// as Python's text mode reads it: universal newlines.)
pub fn write_if_changed(path: &Path, text: &str) -> Result<(), Error> {
    if let Ok(old) = std::fs::read(path) {
        if let Ok(old) = String::from_utf8(old) {
            let same = if old.contains('\r') { old.replace("\r\n", "\n").replace('\r', "\n") == text } else { old == text };
            if same {
                return Ok(());
            }
        }
    }
    std::fs::write(path, text).map_err(|e| Error::io(path, e))
}

fn copy(from: &Path, to: &Path) -> Result<(), Error> {
    std::fs::copy(from, to).map(|_| ()).map_err(|e| Error::io(from, e))
}

/// tools/prepare.py: identify the install in `game` (the FINAL FANTASY XI folder) by the SHA-256
/// of FFXiMain.dll, check FFXi.dll is the same build, keep verified copies and the unpacked
/// images in <root>/generated (and generated/images/<build>/, which the next game update maps its
/// addresses from), and record the choice in generated/build.json. Also writes generated/build.h.
pub fn prepare(game: &Path, root: &Path, on: &mut dyn FnMut(Event)) -> Result<Prepared, Error> {
    let game = game.to_str().ok_or_else(|| Error::Install(format!("{}: not a UTF-8 path", game.display())))?;
    let game = buildinfo::normpath(game);
    let steps = 4;
    let step = |on: &mut dyn FnMut(Event), n: usize| on(Event::Progress { phase: "prepare", what: "install", done: n, total: steps });
    step(on, 0);
    let dll = Path::new(&game).join("FFXiMain.dll");
    let ffxi = Path::new(&game).join("FFXi.dll");
    let digest = buildinfo::sha256(&dll)?;
    let Some(label) = buildinfo::matching(root, &digest)? else {
        return Err(Error::Install(format!(
            "{} is build {}, which meta/builds.json does not know (a stand-in? run install.py restore)", dll.display(), digest)));
    };
    let known = buildinfo::known(root)?;
    let want = known[&label]["FFXi.dll"]["sha256"].as_str().unwrap_or_default().to_string();
    if buildinfo::sha256(&ffxi)? != want {
        return Err(Error::Install(format!("{} does not match FFXiMain.dll: build {} needs FFXi.dll {}", ffxi.display(), label, want)));
    }
    step(on, 1);

    let out = root.join("generated");
    std::fs::create_dir_all(&out).map_err(|e| Error::io(&out, e))?;
    // Keep the verified retail files too: the build and the loader use these copies, so nothing
    // depends on the state of the install afterwards.
    copy(&dll, &out.join("FFXiMain.retail.dll"))?;
    copy(&ffxi, &out.join("FFXi.retail.dll"))?;
    let unpacked = out.join("FFXiMain.unpacked.dll");
    let mut log = |s: String| on(Event::Message(&s));
    pol1::unpack(&dll, &unpacked, &mut log)?;
    step(on, 2);
    let mut log = |s: String| on(Event::Message(&s));
    pol1::unpack(&ffxi, &out.join("FFXi.unpacked.dll"), &mut log)?;
    step(on, 3);
    // One copy per build too: the next game update maps its addresses from this one
    // (tools/newbuild.py carry).
    let keep = out.join("images").join(&label);
    std::fs::create_dir_all(&keep).map_err(|e| Error::io(&keep, e))?;
    for name in ["FFXiMain.retail.dll", "FFXiMain.unpacked.dll", "FFXi.retail.dll", "FFXi.unpacked.dll"] {
        copy(&out.join(name), &keep.join(name))?;
    }
    buildinfo::record(root, &label, &game)?;
    if let Some(b) = buildinfo::current(root)? {
        buildinfo::write_build_h(root, &b)?;
    }
    step(on, 4);
    on(Event::Message(&format!("ok: {} (build {}, from {})", unpacked.display(), label, game)));
    Ok(Prepared { build: label, game, unpacked })
}

/// The recompiler runs translate makes (tools/build_posix.py translate and host64): FFXiMain
/// into generated/all with the build's hooks, FFXi.dll into generated/ffxi as module "ffxi".
pub fn translate_options(root: &Path) -> Result<Vec<recomp::Options>, Error> {
    let b = buildinfo::current(root)?
        .ok_or_else(|| Error::Install(format!("{} missing: run prepare first", buildinfo::chosen_path(root).display())))?;
    let gen = root.join("generated");
    let mut main = recomp::Options::new(b.ffximain_meta.clone(), gen.join("FFXiMain.unpacked.dll"), gen.join("FFXiMain.retail.dll"),
                                        gen.join("all"));
    for (name, addr) in &b.hooks {
        main.hooks.push((name.clone(), buildinfo::parse_hex(addr)?));
    }
    let mut ffxi = recomp::Options::new(b.ffxi_meta.clone(), gen.join("FFXi.unpacked.dll"), gen.join("FFXi.retail.dll"), gen.join("ffxi"));
    ffxi.module = "ffxi".into();
    Ok(vec![main, ffxi])
}

/// Translates both modules of the prepared build (after refreshing generated/build.h).
pub fn translate(root: &Path, on: &mut dyn FnMut(Event)) -> Result<Vec<recomp::Report>, Error> {
    let b = buildinfo::current(root)?
        .ok_or_else(|| Error::Install(format!("{} missing: run prepare first", buildinfo::chosen_path(root).display())))?;
    buildinfo::write_build_h(root, &b)?;
    translate_options(root)?.iter().map(|o| recomp::run(o, on)).collect()
}
