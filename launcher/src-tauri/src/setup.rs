//! First run: find the FINAL FANTASY XI folder, identify its build, and make the game for it.
//!
//! The game is the install's own code translated to C and compiled, so it cannot ship with the
//! launcher (README, "Rules"). The launcher ships the host (xi-host, prebuilt: the runtime, the
//! graphics, sound, input, the sign-in) and makes the rest here, once per build of the game: the
//! translation (xi-recomp, in process) compiled into a module (runtime/xi_game.h) by a C compiler,
//! clang if the machine has one, else zig, which the launcher downloads. The work is done in
//! <data>/work and only the module is kept, in <data>/games/<build>/.

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::ffi::OsString;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use tauri::{AppHandle, Emitter, Manager};

/// The engine's parts the build needs, relative to its root: copied to the work folder, and hashed
/// to tell a game built by an older launcher.
const ENGINE_PARTS: &[&str] = &[
    "meta",
    "runtime/guest.h",
    "runtime/runtime.h",
    "runtime/xi_game.h",
    "runtime/xi_module.c",
];

/// Where the engine sources are: bundled with the launcher (Resources/engine), else the repository
/// the launcher was built from (a development run).
pub fn engine_dir(app: &dyn Env) -> PathBuf {
    app.engine_dir()
}

/// What making the game needs from its surroundings: the launcher's window (AppHandle), or a
/// terminal (--make-game, main.rs).
pub trait Env: Sync {
    fn data_dir(&self) -> Result<PathBuf, String>;
    fn engine_dir(&self) -> PathBuf;
    fn progress(&self, fraction: f64, step: String);
    fn log(&self, line: &str);
}

/// The repository the launcher was built from: the engine for a development run.
pub fn repo_engine() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

impl Env for AppHandle {
    fn data_dir(&self) -> Result<PathBuf, String> {
        self.path().app_data_dir().map_err(|e| e.to_string())
    }
    fn engine_dir(&self) -> PathBuf {
        if let Ok(p) = self.path().resolve("engine", tauri::path::BaseDirectory::Resource) {
            if p.join("runtime/xi_module.c").exists() {
                return p;
            }
        }
        repo_engine()
    }
    fn progress(&self, fraction: f64, step: String) {
        let _ = self.emit("build-progress", BuildProgress { fraction: fraction.clamp(0.0, 1.0), step });
    }
    fn log(&self, line: &str) {
        let _ = self.emit("build-log", line.to_string());
    }
}

const MODULE_EXT: &str = if cfg!(target_os = "macos") { "dylib" } else if cfg!(windows) { "dll" } else { "so" };

/// The host the launcher ships: in the app as an app of its own (macOS: Contents/Helpers/FINAL
/// FANTASY XI.app, so the game has its own name and icon in the Dock), beside the launcher
/// (Windows, Linux), else the repository's build/ (a development run: build_posix.py xihost).
pub fn xi_host() -> Option<PathBuf> {
    let name = if cfg!(windows) { "xi-host.exe" } else { "xi-host" };
    let mut candidates = Vec::new();
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())) {
        candidates.push(dir.join("../Helpers/FINAL FANTASY XI.app/Contents/MacOS").join(name));
        candidates.push(dir.join(name));
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build").join(name));
    candidates.into_iter().find(|p| p.is_file())
}

/// A game made for a build: the program to run, and the module it loads (none for a game built
/// whole, as an older launcher made them).
pub struct Game {
    pub host: PathBuf,
    pub module: Option<PathBuf>,
}

fn data_dir(app: &dyn Env) -> Result<PathBuf, String> {
    app.data_dir()
}

fn games_dir(app: &dyn Env) -> Result<PathBuf, String> {
    Ok(data_dir(app)?.join("games"))
}

fn skip(name: &str) -> bool {
    name == "__pycache__" || name == ".DS_Store" || name.ends_with(".pyc")
}

fn files_under(root: &Path, rel: &Path, out: &mut Vec<PathBuf>) {
    let full = root.join(rel);
    if full.is_file() {
        out.push(rel.to_path_buf());
        return;
    }
    let Ok(rd) = fs::read_dir(&full) else { return };
    let mut names: Vec<_> = rd.filter_map(|e| e.ok()).map(|e| e.file_name()).collect();
    names.sort();
    for n in names {
        if !skip(&n.to_string_lossy()) {
            files_under(root, &rel.join(n), out);
        }
    }
}

fn engine_files(engine: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for part in ENGINE_PARTS {
        files_under(engine, Path::new(part), &mut out);
    }
    out
}

/// A hash of the engine sources: a game built from other sources is outdated.
pub fn engine_hash(engine: &Path) -> String {
    let mut h = Sha256::new();
    for rel in engine_files(engine) {
        h.update(rel.to_string_lossy().replace('\\', "/").as_bytes());
        h.update([0]);
        if let Ok(bytes) = fs::read(engine.join(&rel)) {
            h.update(&bytes);
        }
    }
    hex(&h.finalize())[..16].to_string()
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let bytes = fs::read(path).map_err(|e| format!("Cannot read {}: {e}", path.display()))?;
    Ok(hex(&Sha256::digest(&bytes)))
}

// --- the install ---------------------------------------------------------------------------------

/// The FINAL FANTASY XI folder at or under what the player chose: they may pick PlayOnline or
/// SquareEnix above it.
pub fn normalize_game_folder(chosen: &Path) -> Option<PathBuf> {
    ["", "FINAL FANTASY XI", "SquareEnix/FINAL FANTASY XI", "PlayOnline/SquareEnix/FINAL FANTASY XI"]
        .iter()
        .map(|sub| if sub.is_empty() { chosen.to_path_buf() } else { chosen.join(sub) })
        .find(|p| p.join("FFXiMain.dll").is_file() && p.join("FFXi.dll").is_file())
}

/// Where installs usually are: a Windows install, Wine and CrossOver prefixes, Steam, and on macOS
/// a PlayOnline folder copied to the home folder or the top of a drive.
pub fn detect_installs() -> Vec<String> {
    let mut roots: Vec<PathBuf> = Vec::new();
    let home = std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" }).map(PathBuf::from);
    let pol = "PlayOnline/SquareEnix/FINAL FANTASY XI";
    let steam = "steamapps/common/FFXINA/SquareEnix/FINAL FANTASY XI";
    if cfg!(windows) {
        for pf in ["C:/Program Files (x86)", "C:/Program Files", "D:/Program Files (x86)", "D:/"] {
            roots.push(Path::new(pf).join(pol));
        }
        for s in ["C:/Program Files (x86)/Steam", "C:/Program Files/Steam"] {
            roots.push(Path::new(s).join(steam));
        }
    }
    if let Some(h) = &home {
        for base in ["", "Games", "Documents", "Desktop"] {
            roots.push(h.join(base).join(pol));
        }
        for prefix in [".wine", "Games/ffxi", ".local/share/wineprefixes/ffxi"] {
            for pf in ["Program Files (x86)", "Program Files"] {
                roots.push(h.join(prefix).join("drive_c").join(pf).join(pol));
            }
        }
        roots.push(h.join("Library/Application Support/Steam").join(steam));
        roots.push(h.join(".local/share/Steam").join(steam));
        roots.push(h.join(".steam/steam").join(steam));
        // CrossOver bottles
        if let Ok(rd) = fs::read_dir(h.join("Library/Application Support/CrossOver/Bottles")) {
            for b in rd.flatten() {
                for pf in ["Program Files (x86)", "Program Files"] {
                    roots.push(b.path().join("drive_c").join(pf).join(pol));
                }
            }
        }
    }
    // the top two levels of each drive (macOS /Volumes, Linux /media and /mnt)
    for mount in ["/Volumes", "/media", "/mnt"] {
        let Ok(rd) = fs::read_dir(mount) else { continue };
        for vol in rd.flatten() {
            roots.push(vol.path().join(pol));
            if let Ok(rd) = fs::read_dir(vol.path()) {
                for d in rd.flatten().filter(|d| d.path().is_dir()) {
                    roots.push(d.path().join(pol));
                }
            }
        }
    }
    let mut found: Vec<String> = Vec::new();
    for r in roots {
        if let Some(p) = normalize_game_folder(&r) {
            let s = p.to_string_lossy().into_owned();
            if !found.contains(&s) {
                found.push(s);
            }
        }
    }
    found
}

/// The PlayOnlineViewer folder for an install: beside FINAL FANTASY XI as retail puts it, or next to
/// the PlayOnline or SquareEnix folder above it (a copy put there by hand).
pub fn find_viewer(game: &Path) -> Option<PathBuf> {
    ["../PlayOnlineViewer", "../../PlayOnlineViewer", "../../../PlayOnlineViewer"]
        .iter()
        .map(|rel| game.join(rel))
        .find(|p| p.join("pol.exe").is_file() || p.join("viewer").is_dir())
        .map(|p| p.canonicalize().unwrap_or(p))
}

#[derive(Serialize, Clone, Debug)]
pub struct GameBuild {
    /// The label in meta/builds.json ("2026-08-22"), empty when the build is not known.
    pub label: String,
    /// The client version the lobby sees ("30260805_0").
    pub version: String,
    pub ffximain_sha: String,
}

/// Which build of the game is in the folder: its FFXiMain.dll against meta/builds.json.
pub fn identify(engine: &Path, game: &Path) -> Result<GameBuild, String> {
    let sha = sha256_file(&game.join("FFXiMain.dll"))?;
    let text = fs::read_to_string(engine.join("meta/builds.json")).map_err(|e| format!("meta/builds.json: {e}"))?;
    let builds: serde_json::Value = serde_json::from_str(&text).map_err(|e| format!("meta/builds.json: {e}"))?;
    if let Some(map) = builds["builds"].as_object() {
        for (label, b) in map {
            if b["FFXiMain.dll"]["sha256"].as_str() == Some(sha.as_str()) {
                return Ok(GameBuild {
                    label: label.clone(),
                    version: b["version"].as_str().unwrap_or_default().to_string(),
                    ffximain_sha: sha,
                });
            }
        }
    }
    Ok(GameBuild { label: String::new(), version: String::new(), ffximain_sha: sha })
}

/// The game made for a build, if there is one, and whether it came from these engine sources.
fn built_game(app: &dyn Env, label: &str, engine_hash: &str) -> Option<(Game, bool)> {
    let dir = games_dir(app).ok()?.join(label);
    let info: serde_json::Value = serde_json::from_str(&fs::read_to_string(dir.join("build.json")).ok()?).ok()?;
    let game = if let Some(m) = info["module"].as_str() {
        Game { host: xi_host()?, module: Some(dir.join(m)).filter(|p| p.is_file()) }
    } else {
        Game { host: dir.join(info["host"].as_str()?), module: None }
    };
    if !game.host.is_file() || (info["module"].is_string() && game.module.is_none()) {
        return None;
    }
    let current = fs::read_to_string(dir.join("engine.txt")).map(|s| s.trim() == engine_hash).unwrap_or(false);
    Some((game, current))
}

/// The game for this install, when one has been made.
pub fn game_for(app: &dyn Env, game_path: &str) -> Option<Game> {
    let engine = engine_dir(app);
    let build = identify(&engine, Path::new(game_path)).ok()?;
    if build.label.is_empty() {
        return None;
    }
    built_game(app, &build.label, &engine_hash(&engine)).map(|(g, _)| g)
}

#[derive(Serialize, Default)]
pub struct SetupStatus {
    pub game_path: String,
    /// FFXiMain.dll and FFXi.dll are there
    pub game_ok: bool,
    pub build: Option<GameBuild>,
    /// A game has been made for this build.
    pub ready: bool,
    /// It was made from older engine sources: it still runs, a rebuild picks up the new ones.
    pub outdated: bool,
    pub host: String,
    /// PlayOnlineViewer is beside the game (PlayOnline accounts use it)
    pub viewer: bool,
    pub error: String,
}

pub fn status(app: &dyn Env, game_path: &str) -> SetupStatus {
    let mut s = SetupStatus { game_path: game_path.to_string(), ..Default::default() };
    if game_path.is_empty() {
        return s;
    }
    let game = Path::new(game_path);
    s.game_ok = game.join("FFXiMain.dll").is_file() && game.join("FFXi.dll").is_file();
    if !s.game_ok {
        s.error = "There is no FFXiMain.dll and FFXi.dll in this folder.".into();
        return s;
    }
    s.viewer = find_viewer(game).is_some();
    let engine = engine_dir(app);
    match identify(&engine, game) {
        Ok(b) => {
            if b.label.is_empty() {
                s.error = format!(
                    "This version of the game is not supported yet (FFXiMain.dll {}…). Each game update needs its own metadata; see \"Supporting a new client version\" in the README.",
                    &b.ffximain_sha[..12]
                );
            } else if let Some((game, current)) = built_game(app, &b.label, &engine_hash(&engine)) {
                s.ready = true;
                s.outdated = !current;
                s.host = game.host.to_string_lossy().into_owned();
            } else if xi_host().is_none() {
                s.error = "The launcher's game host (xi-host) is missing; reinstall the launcher.".into();
            }
            s.build = Some(b);
        }
        Err(e) => s.error = e,
    }
    s
}


// --- the C compiler ------------------------------------------------------------------------------

/// PATH for the tools the build runs. Started from Finder or the Dock, an app's PATH has none of
/// Homebrew's folders.
fn tool_path() -> OsString {
    let mut dirs: Vec<PathBuf> = std::env::var_os("PATH").map(|p| std::env::split_paths(&p).collect()).unwrap_or_default();
    for extra in ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"] {
        let p = PathBuf::from(extra);
        if !dirs.contains(&p) {
            dirs.push(p);
        }
    }
    std::env::join_paths(dirs).unwrap_or_default()
}

fn command(program: impl AsRef<std::ffi::OsStr>) -> Command {
    let mut c = Command::new(program);
    c.env("PATH", tool_path());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    c
}

fn output(program: impl AsRef<std::ffi::OsStr>, args: &[&str]) -> Option<String> {
    let o = command(program).args(args).stdin(Stdio::null()).stderr(Stdio::null()).output().ok()?;
    o.status.success().then(|| String::from_utf8_lossy(&o.stdout).trim().to_string())
}

/// The zig the launcher downloads when the machine has no C compiler: pinned, checked against the
/// SHA-256 ziglang.org publishes for it.
const ZIG_VERSION: &str = "0.16.0";

fn zig_target() -> &'static str {
    match (std::env::consts::ARCH, std::env::consts::OS) {
        ("aarch64", "macos") => "aarch64-macos",
        ("x86_64", "macos") => "x86_64-macos",
        ("aarch64", "linux") => "aarch64-linux",
        ("x86_64", "linux") => "x86_64-linux",
        ("x86_64", "windows") => "x86_64-windows",
        ("aarch64", "windows") => "aarch64-windows",
        _ => "",
    }
}

fn zig_dir(app: &dyn Env) -> Result<PathBuf, String> {
    Ok(data_dir(app)?.join("zig").join(ZIG_VERSION))
}

fn zig_exe(app: &dyn Env) -> Option<PathBuf> {
    let p = zig_dir(app).ok()?.join(if cfg!(windows) { "zig.exe" } else { "zig" });
    p.is_file().then_some(p)
}

/// The compiler for the module: clang or cc when the machine has one (Xcode's command line tools,
/// a Linux distribution's), else the launcher's zig. The command and what to call it.
pub fn compiler(app: &dyn Env) -> Option<(Vec<OsString>, String)> {
    // XI_CC=zig: the launcher's zig even where there is clang (to check the path players without one take)
    if !cfg!(windows) && std::env::var("XI_CC").as_deref() != Ok("zig") {
        for cc in ["clang", "cc"] {
            if let Some(v) = output(cc, &["--version"]) {
                return Some((vec![cc.into()], v.lines().next().unwrap_or(cc).to_string()));
            }
        }
    }
    zig_exe(app).map(|z| (vec![z.into_os_string(), "cc".into()], format!("zig {ZIG_VERSION} (the launcher's)")))
}

#[derive(Serialize)]
pub struct Prereq {
    pub id: &'static str,
    pub name: &'static str,
    pub ok: bool,
    pub detail: String,
    /// How to get it: a command to run
    pub fix: String,
    /// The launcher can get it itself (install_prereqs)
    pub installable: bool,
}

/// What making the game needs: a C compiler. Everything else ships with the launcher.
pub fn prereqs(app: &dyn Env) -> Vec<Prereq> {
    let cc = compiler(app);
    vec![Prereq {
        id: "compiler",
        name: "C compiler",
        ok: cc.is_some(),
        detail: cc.map(|(_, name)| name).unwrap_or_else(|| "none on this computer".into()),
        fix: if zig_target().is_empty() { "Install clang.".into() } else { format!("The launcher can download zig {ZIG_VERSION} (about 55 MB) from ziglang.org.") },
        installable: !zig_target().is_empty(),
    }]
}

// --- making the game -----------------------------------------------------------------------------

#[derive(Serialize, Clone)]
pub struct BuildProgress {
    /// 0..1 across the whole build
    pub fraction: f64,
    pub step: String,
}

#[derive(Serialize, Clone)]
pub struct BuildDone {
    pub ok: bool,
    pub message: String,
}

/// The build while it runs, so the window can cancel it.
#[derive(Default)]
pub struct Building {
    children: Mutex<Vec<Arc<Mutex<Child>>>>,
    busy: Mutex<bool>,
    cancelled: AtomicBool,
}

fn emit_log(app: &dyn Env, line: &str) {
    app.log(line)
}

fn emit_progress(app: &dyn Env, fraction: f64, step: impl Into<String>) {
    app.progress(fraction, step.into())
}

fn copy_tree(src: &Path, dst: &Path, files: &[PathBuf]) -> Result<(), String> {
    for rel in files {
        let to = dst.join(rel);
        if let Some(parent) = to.parent() {
            fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        fs::copy(src.join(rel), &to).map_err(|e| format!("{}: {e}", rel.display()))?;
    }
    Ok(())
}

/// Runs one compiler command to the end, killable from cancel(); its output goes to the log when it
/// fails.
fn run_cc(app: &dyn Env, building: &Building, mut cmd: Command, what: &str) -> Result<(), String> {
    if building.cancelled.load(Ordering::Relaxed) {
        return Err("Cancelled.".into());
    }
    cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped());
    let mut child = cmd.spawn().map_err(|e| format!("Cannot start the compiler: {e}"))?;
    // read as it comes: a compiler with a lot to say would otherwise fill the pipe and wait forever
    let mut stderr = child.stderr.take().unwrap();
    let reader = thread::spawn(move || {
        let mut text = String::new();
        let _ = stderr.read_to_string(&mut text);
        text
    });
    let child = Arc::new(Mutex::new(child));
    building.children.lock().unwrap().push(child.clone());
    let out = loop {
        if let Some(status) = child.lock().unwrap().try_wait().map_err(|e| e.to_string())? {
            break status;
        }
        thread::sleep(std::time::Duration::from_millis(50));
    };
    building.children.lock().unwrap().retain(|c| !Arc::ptr_eq(c, &child));
    let text = reader.join().unwrap_or_default();
    if building.cancelled.load(Ordering::Relaxed) {
        return Err("Cancelled.".into());
    }
    if !out.success() {
        for line in text.lines().take(40) {
            emit_log(app, line);
        }
        return Err(format!("The compiler failed on {what}."));
    }
    Ok(())
}

/// The flags every file of the module is compiled with (tools/build_posix.py CFLAGS, GEN_WARNINGS,
/// and module()'s).
fn module_cflags() -> Vec<&'static str> {
    vec![
        "-O2", "-g0", "-std=c11", "-DRT_GUEST_WINDOW", "-fno-strict-aliasing", "-fPIC", "-fvisibility=hidden", "-I", "runtime",
        "-I", "generated", "-Wno-unused-label", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
        "-Wno-unused-function", "-Wno-parentheses-equality", "-Wno-unreachable-code",
    ]
}

pub fn build(app: &dyn Env, building: &Building, game: &Path, label: &str) -> Result<String, String> {
    let engine = engine_dir(app);
    let hash = engine_hash(&engine);
    let data = data_dir(app)?;
    let work = data.join("work");
    let games = games_dir(app)?;
    let out = games.join(format!("{label}.new"));
    let (cc, cc_name) = compiler(app).ok_or("No C compiler: get one first.")?;

    emit_progress(app, 0.0, "Copying the engine…");
    let _ = fs::remove_dir_all(&work);
    copy_tree(&engine, &work, &engine_files(&engine))?;

    // the translation, in process (xi-recomp: the same output as tools/prepare.py and recomp.py)
    let mut last = std::time::Instant::now();
    let mut on = |ev: xi_recomp::Event| match ev {
        xi_recomp::Event::Progress { phase, what, done, total } => {
            if last.elapsed().as_millis() < 100 && done < total {
                return;
            }
            last = std::time::Instant::now();
            let f = if total > 0 { done as f64 / total as f64 } else { 0.0 };
            match phase {
                "prepare" => emit_progress(app, 0.01 + 0.03 * f, "Reading the game's files…"),
                _ if what == "main" => emit_progress(app, 0.04 + 0.08 * f, format!("Translating FFXiMain.dll ({done} of {total} functions)…")),
                _ => emit_progress(app, 0.12 + 0.01 * f, "Translating FFXi.dll…"),
            }
        }
        xi_recomp::Event::Message(m) => emit_log(app, m),
    };
    emit_progress(app, 0.01, "Reading the game's files…");
    xi_recomp::prepare(game, &work, &mut on).map_err(|e| format!("Reading the game's files: {e}"))?;
    xi_recomp::translate(&work, &mut on).map_err(|e| format!("Translating: {e}"))?;

    // compiled into the module, a file per job on every core
    let mut jobs: Vec<(PathBuf, Vec<&str>)> = Vec::new();
    for sub in ["all", "ffxi"] {
        let dir = work.join("generated").join(sub);
        let mut files: Vec<PathBuf> = fs::read_dir(&dir).map_err(|e| format!("{}: {e}", dir.display()))?
            .flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|x| x == "c")).collect();
        files.sort();
        for f in files {
            jobs.push((f, vec!["-I", if sub == "all" { "generated/all" } else { "generated/ffxi" }]));
        }
    }
    jobs.push((work.join("runtime/xi_module.c"), vec![]));
    let objdir = work.join("obj");
    fs::create_dir_all(&objdir).map_err(|e| e.to_string())?;
    let objs: Vec<PathBuf> = jobs.iter().enumerate().map(|(i, (src, _))| {
        objdir.join(format!("{i:03}-{}.o", src.file_stem().unwrap().to_string_lossy()))
    }).collect();
    // Objects by what made them (the compiler, its flags, the file and the headers it can include):
    // a file made before is not compiled again. A launcher update usually changes a few of the
    // translation's files; the rest come from here (<data>/cache/obj; pruned below).
    let cache = data.join("cache").join("obj");
    let header_digest = |sub: &str| -> Vec<u8> {
        let mut h = Sha256::new();
        let mut hs: Vec<PathBuf> = fs::read_dir(work.join("runtime")).map(|rd| rd.flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|x| x == "h")).collect()).unwrap_or_default();
        hs.sort();
        hs.push(work.join("generated/build.h"));
        if !sub.is_empty() {
            hs.push(work.join("generated").join(sub).join("funcs.h"));
        }
        for p in hs {
            h.update(p.file_name().unwrap().to_string_lossy().as_bytes());
            h.update(fs::read(&p).unwrap_or_default());
        }
        h.finalize().to_vec()
    };
    let digests: std::collections::HashMap<&str, Vec<u8>> = ["", "all", "ffxi"].iter().map(|s| (*s, header_digest(s))).collect();
    let flags = module_cflags().join(" ");
    let keys: Vec<String> = jobs
        .iter()
        .map(|(src, extra)| {
            let sub = extra.get(1).and_then(|d| d.strip_prefix("generated/")).unwrap_or("");
            let mut h = Sha256::new();
            h.update(cc_name.as_bytes());
            h.update(flags.as_bytes());
            h.update(extra.join(" ").as_bytes());
            h.update(&digests[sub]);
            h.update(fs::read(src).unwrap_or_default());
            hex(&h.finalize())
        })
        .collect();
    let hits = AtomicUsize::new(0);
    emit_log(app, &format!("compiling {} files with {cc_name}", jobs.len()));
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let failed: Mutex<Option<String>> = Mutex::new(None);
    let workers = thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    thread::scope(|scope| {
        for _ in 0..workers {
            scope.spawn(|| loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                if i >= jobs.len() || failed.lock().unwrap().is_some() {
                    return;
                }
                let (src, extra) = &jobs[i];
                let cached = cache.join(&keys[i][..2]).join(format!("{}.o", keys[i]));
                if cached.is_file() && fs::copy(&cached, &objs[i]).is_ok() {
                    hits.fetch_add(1, Ordering::Relaxed);
                } else {
                    let mut c = command(&cc[0]);
                    c.args(&cc[1..]).arg("-c").args(module_cflags()).args(extra).arg(src).arg("-o").arg(&objs[i]).current_dir(&work);
                    if let Err(e) = run_cc(app, building, c, &src.file_name().unwrap().to_string_lossy()) {
                        failed.lock().unwrap().get_or_insert(e);
                        return;
                    }
                    let _ = fs::create_dir_all(cached.parent().unwrap());
                    let tmp = cached.with_extension(format!("tmp{i}"));
                    if fs::copy(&objs[i], &tmp).is_ok() {
                        let _ = fs::rename(&tmp, &cached);
                    }
                }
                let d = done.fetch_add(1, Ordering::Relaxed) + 1;
                emit_progress(app, 0.13 + 0.82 * d as f64 / jobs.len() as f64, format!("Compiling the game ({d} of {})…", jobs.len()));
            });
        }
    });
    if let Some(e) = failed.into_inner().unwrap() {
        return Err(e);
    }
    let hits = hits.into_inner();
    if hits > 0 {
        emit_log(app, &format!("{hits} of {} files were made before (the cache)", jobs.len()));
    }

    emit_progress(app, 0.96, "Linking…");
    let _ = fs::remove_dir_all(&out);
    fs::create_dir_all(&out).map_err(|e| e.to_string())?;
    let module_name = format!("ffxi-game.{MODULE_EXT}");
    let mut c = command(&cc[0]);
    c.args(&cc[1..]);
    if cfg!(target_os = "macos") {
        c.arg("-dynamiclib").arg(format!("-Wl,-install_name,@rpath/{module_name}"));
    } else {
        c.arg("-shared");
    }
    c.arg("-o").arg(out.join(&module_name)).args(&objs);
    if !cfg!(windows) {
        c.arg("-lm");
    }
    run_cc(app, building, c, "the link")?;

    let prepared: serde_json::Value = serde_json::from_str(&fs::read_to_string(work.join("generated/build.json")).unwrap_or_default()).unwrap_or_default();
    let b = identify(&engine, game)?;
    let info = serde_json::json!({
        "build": prepared["build"].as_str().unwrap_or(label), "version": b.version, "ffximain_sha": b.ffximain_sha,
        "module": module_name, "compiler": cc_name,
    });
    fs::write(out.join("build.json"), serde_json::to_string_pretty(&info).unwrap()).map_err(|e| e.to_string())?;
    fs::write(out.join("engine.txt"), &hash).map_err(|e| e.to_string())?;
    fs::write(out.join("objects.txt"), keys.join("\n")).map_err(|e| e.to_string())?;
    let dest = games.join(label);
    let _ = fs::remove_dir_all(&dest);
    fs::rename(&out, &dest).map_err(|e| e.to_string())?;
    prune_cache(&cache, &games);
    // the translation and objects (hundreds of MB) are not needed once the game is made
    let _ = fs::remove_dir_all(&work);
    emit_progress(app, 1.0, "Ready");
    Ok(format!("The game is ready (build {label})."))
}

/// Drops the cached objects no game made here still uses (each game lists its own, objects.txt).
fn prune_cache(cache: &Path, games: &Path) {
    let mut keep = std::collections::HashSet::new();
    for g in fs::read_dir(games).into_iter().flatten().flatten() {
        if let Ok(t) = fs::read_to_string(g.path().join("objects.txt")) {
            keep.extend(t.lines().map(str::to_string));
        }
    }
    for d in fs::read_dir(cache).into_iter().flatten().flatten() {
        for f in fs::read_dir(d.path()).into_iter().flatten().flatten() {
            let name = f.file_name().to_string_lossy().into_owned();
            if !keep.contains(name.trim_end_matches(".o")) {
                let _ = fs::remove_file(f.path());
            }
        }
    }
}

pub fn start(app: AppHandle, building: Arc<Building>, game_path: String) -> Result<(), String> {
    let game = PathBuf::from(&game_path);
    let st = status(&app, &game_path);
    let label = match &st.build {
        Some(b) if !b.label.is_empty() => b.label.clone(),
        _ => return Err(if st.error.is_empty() { "Choose the FINAL FANTASY XI folder first.".into() } else { st.error }),
    };
    {
        let mut busy = building.busy.lock().unwrap();
        if *busy {
            return Err("The game is already being made.".into());
        }
        *busy = true;
    }
    building.cancelled.store(false, Ordering::Relaxed);
    thread::spawn(move || {
        let result = build(&app, &building, &game, &label);
        *building.busy.lock().unwrap() = false;
        let done = match result {
            Ok(message) => BuildDone { ok: true, message },
            Err(message) => BuildDone { ok: false, message },
        };
        let _ = app.emit("build-done", done);
    });
    Ok(())
}

pub fn cancel(building: &Building) {
    building.cancelled.store(true, Ordering::Relaxed);
    for child in building.children.lock().unwrap().iter() {
        let _ = child.lock().unwrap().kill();
    }
}

pub fn is_building(building: &Building) -> bool {
    *building.busy.lock().unwrap()
}

/// Gets zig: ziglang.org's build for this machine, checked against the SHA-256 its index gives,
/// unpacked into <data>/zig/<version>.
pub fn download_zig(app: &dyn Env, building: &Building) -> Result<String, String> {
    let target = zig_target();
    if target.is_empty() {
        return Err("There is no zig for this kind of computer; install clang.".into());
    }
    emit_log(app, "> https://ziglang.org/download/index.json");
    let index: serde_json::Value = ureq::get("https://ziglang.org/download/index.json")
        .call().map_err(|e| format!("ziglang.org: {e}"))?
        .into_json().map_err(|e| format!("ziglang.org: {e}"))?;
    let entry = &index[ZIG_VERSION][target];
    let (url, sha, size) = (
        entry["tarball"].as_str().ok_or(format!("ziglang.org has no zig {ZIG_VERSION} for {target}"))?,
        entry["shasum"].as_str().unwrap_or_default().to_string(),
        entry["size"].as_str().and_then(|s| s.parse::<u64>().ok()).unwrap_or(0),
    );
    emit_log(app, &format!("> {url}"));
    let resp = ureq::get(url).call().map_err(|e| format!("{url}: {e}"))?;
    let mut reader = resp.into_reader();
    let mut bytes = Vec::with_capacity(size as usize);
    let mut buf = vec![0u8; 1 << 16];
    loop {
        if building.cancelled.load(Ordering::Relaxed) {
            return Err("Cancelled.".into());
        }
        let n = reader.read(&mut buf).map_err(|e| format!("{url}: {e}"))?;
        if n == 0 {
            break;
        }
        bytes.extend_from_slice(&buf[..n]);
        if size > 0 {
            emit_progress(app, 0.9 * bytes.len() as f64 / size as f64, format!("Downloading zig… {} of {} MB", bytes.len() >> 20, size >> 20));
        }
    }
    let got = hex(&Sha256::digest(&bytes));
    if got != sha {
        return Err(format!("The zig download does not match ziglang.org's checksum ({got}, expected {sha}); not using it."));
    }
    emit_progress(app, 0.92, "Unpacking zig…");
    let dir = zig_dir(app)?;
    let tmp = dir.with_extension("tmp");
    let _ = fs::remove_dir_all(&tmp);
    fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
    if url.ends_with(".zip") {
        let mut z = zip::ZipArchive::new(std::io::Cursor::new(bytes)).map_err(|e| format!("zig: {e}"))?;
        z.extract(&tmp).map_err(|e| format!("zig: {e}"))?;
    } else {
        let mut t = tar::Archive::new(xz2::read::XzDecoder::new(std::io::Cursor::new(bytes)));
        t.unpack(&tmp).map_err(|e| format!("zig: {e}"))?;
    }
    // the archive holds one folder, zig-<target>-<version>
    let inner = fs::read_dir(&tmp).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).find(|p| p.is_dir()).ok_or("zig: an empty archive")?;
    let _ = fs::remove_dir_all(&dir);
    fs::rename(&inner, &dir).map_err(|e| e.to_string())?;
    let _ = fs::remove_dir_all(&tmp);
    emit_progress(app, 1.0, "zig is ready");
    Ok(format!("zig {ZIG_VERSION} is ready."))
}

/// Gets what making the game needs and the machine lacks: zig, when there is no C compiler.
pub fn install(app: AppHandle, building: Arc<Building>) -> Result<(), String> {
    if compiler(&app).is_some() {
        return Err("There is already a C compiler.".into());
    }
    {
        let mut busy = building.busy.lock().unwrap();
        if *busy {
            return Err("Busy.".into());
        }
        *busy = true;
    }
    building.cancelled.store(false, Ordering::Relaxed);
    thread::spawn(move || {
        let result = download_zig(&app, &building);
        *building.busy.lock().unwrap() = false;
        let done = match result {
            Ok(message) => BuildDone { ok: true, message },
            Err(message) => BuildDone { ok: false, message },
        };
        let _ = app.emit("install-done", done);
    });
    Ok(())
}
