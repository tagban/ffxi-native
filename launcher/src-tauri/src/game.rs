//! Running the game: sign in (PlayOnline accounts), start host64 with the right arguments, stream
//! its output to the window and a log file, and keep the PlayOnline session up until it exits.

use crate::config::{Account, AccountKind, LauncherConfig};
use crate::pol::PolSession;
use serde::Serialize;
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager};

#[derive(Serialize, Clone)]
pub struct GameState {
    /// "idle", "signing-in", "running", "exited" or "error"
    pub state: &'static str,
    pub message: String,
}

/// The game while it runs, so the window can stop it.
#[derive(Default)]
pub struct Running {
    child: Mutex<Option<Arc<Mutex<Child>>>>,
    busy: Mutex<bool>,
}

/// Where the launcher keeps what it writes: the generated settings, the game's saves, the log.
pub struct Paths {
    pub config_dir: PathBuf,
    pub log_dir: PathBuf,
    pub host: PathBuf,
    /// the game module host64 loads (--module), when the game is made as one (setup.rs)
    pub module: Option<PathBuf>,
    pub base_registry: PathBuf,
}

pub struct LaunchRequest {
    pub account: Account,
    pub password: String,
    pub otp: String,
    /// The player's own install, when the game runs from a version kept for a server (the vault's
    /// copies hold FINAL FANTASY XI only): its PlayOnlineViewer is used.
    pub own_game_path: String,
}

fn state(app: &AppHandle, state: &'static str, message: impl Into<String>) {
    let _ = app.emit("game-state", GameState { state, message: message.into() });
}

/// The launcher steps aside while the game runs, and comes back when it exits.
/// On macOS it also leaves the Dock while hidden, so the game's is the only icon there.
fn show_window(app: &AppHandle, show: bool) {
    #[cfg(target_os = "macos")]
    {
        let policy = if show { tauri::ActivationPolicy::Regular } else { tauri::ActivationPolicy::Accessory };
        let _ = app.set_activation_policy(policy);
    }
    if let Some(w) = app.get_webview_window("main") {
        if show {
            let _ = w.unminimize();
            let _ = w.show();
            let _ = w.set_focus();
        } else {
            let _ = w.hide();
        }
    }
}

fn log_line(app: &AppHandle, log: &Mutex<Option<File>>, line: &str) {
    if let Some(f) = log.lock().unwrap().as_mut() {
        let _ = writeln!(f, "{line}");
    }
    let _ = app.emit("game-log", line.to_string());
}

/// host64's command line (without the password, which goes in FFXI_PASSWORD).
fn host_args(cfg: &LauncherConfig, paths: &Paths, account: &Account, session: Option<&str>, otp: &str, own_game: &str) -> Vec<String> {
    let mut a: Vec<String> = vec!["--game".into(), cfg.game_path.clone()];
    if let Some(m) = &paths.module {
        a.extend(["--module".into(), m.to_string_lossy().into_owned()]);
    }
    // PlayOnlineViewer, when it is not where the game looks (beside FINAL FANTASY XI). A version
    // kept for a server (<vault>/installs/<version>/FINAL FANTASY XI) has none: the player's own
    // install's. Without it the game has no data\dic (its name dictionary): every new character's
    // name is refused as "already in use" (error 3322).
    let game = std::path::Path::new(&cfg.game_path);
    if !game.join("../PlayOnlineViewer").is_dir() {
        let viewer = crate::setup::find_viewer(game)
            .or_else(|| (!own_game.is_empty()).then(|| crate::setup::find_viewer(std::path::Path::new(own_game))).flatten());
        if let Some(v) = viewer {
            a.extend(["--viewer".into(), v.to_string_lossy().into_owned()]);
        }
    }
    a.extend(["--reg".into(), paths.base_registry.to_string_lossy().into_owned()]);
    a.extend(["--reg-overlay".into(), paths.config_dir.join("saved.reg").to_string_lossy().into_owned()]);
    a.extend(["--reg-final".into(), paths.config_dir.join("settings.reg").to_string_lossy().into_owned()]);
    a.extend(["--data-dir".into(), paths.config_dir.to_string_lossy().into_owned()]);
    a.extend(["--server".into(), account.server.clone()]);
    for d in cfg.dats.iter().filter(|d| !d.is_empty()) {
        a.extend(["--dats".into(), d.clone()]);
    }
    a.extend(["--fps-divisor".into(), cfg.game.fps_divisor.clamp(1, 4).to_string()]);
    // settings the Game settings page changes while the game runs (config.rs to_live)
    a.extend(["--live".into(), paths.config_dir.join("live.txt").to_string_lossy().into_owned()]);
    if !cfg.game.ui_aspect.is_empty() {
        a.extend(["--ui-aspect".into(), cfg.game.ui_aspect.clone()]);
    }
    match account.kind {
        AccountKind::Pol => a.extend(["--session".into(), session.unwrap_or_default().to_string()]),
        AccountKind::Lsb => {
            a.extend(["--user".into(), account.login.clone()]);
            if !otp.is_empty() {
                a.extend(["--otp".into(), otp.to_string()]);
            }
            for (flag, port) in [("--authport", account.auth_port), ("--dataport", account.data_port), ("--viewport", account.view_port)] {
                if port != 0 {
                    a.extend([flag.into(), port.to_string()]);
                }
            }
            if !account.loader.is_empty() {
                a.extend(["--loader".into(), account.loader.clone()]);
            }
        }
    }
    let (ww, wh) = cfg.game.window_points;
    if ww >= 320 && wh >= 200 && cfg.game.window_mode == 1 {
        a.extend(["--window-size".into(), format!("{ww}x{wh}")]);
    }
    a
}

fn check(cfg: &LauncherConfig, paths: &Paths, account: &Account) -> Result<(), String> {
    if cfg.game_path.is_empty() || !Path::new(&cfg.game_path).join("FFXiMain.dll").exists() {
        return Err("Choose the FINAL FANTASY XI folder (the one with FFXiMain.dll) first.".into());
    }
    if !paths.host.exists() {
        return Err(format!("The game has not been made for this install yet (no {}). Open Setup and choose Make the game.", paths.host.display()));
    }
    if account.login.is_empty() {
        return Err(match account.kind {
            AccountKind::Pol => "The account has no PlayOnline ID.".into(),
            AccountKind::Lsb => "The account has no account name.".into(),
        });
    }
    if account.server.is_empty() {
        return Err("The account has no server.".into());
    }
    Ok(())
}

pub fn launch(app: AppHandle, running: Arc<Running>, cfg: LauncherConfig, paths: Paths, req: LaunchRequest) -> Result<(), String> {
    check(&cfg, &paths, &req.account)?;
    fs::create_dir_all(&paths.config_dir).map_err(|e| e.to_string())?;
    fs::write(paths.config_dir.join("settings.reg"), cfg.game.to_reg()).map_err(|e| e.to_string())?;
    fs::write(paths.config_dir.join("live.txt"), cfg.game.to_live()).map_err(|e| e.to_string())?;
    {
        let mut busy = running.busy.lock().unwrap();
        if *busy {
            return Err("The game is already running.".into());
        }
        *busy = true;
    }

    thread::spawn(move || {
        let result = run(&app, &running, &cfg, &paths, req);
        *running.child.lock().unwrap() = None;
        *running.busy.lock().unwrap() = false;
        if let Some(w) = app.get_webview_window("main") {
            let _ = w.set_always_on_top(false);
        }
        show_window(&app, true);
        match result {
            Ok(msg) => state(&app, "exited", msg),
            Err(msg) => state(&app, "error", msg),
        }
    });
    Ok(())
}

fn run(app: &AppHandle, running: &Running, cfg: &LauncherConfig, paths: &Paths, req: LaunchRequest) -> Result<String, String> {
    let account = &req.account;
    let mut pol = None;
    let mut session = None;
    if account.kind == AccountKind::Pol {
        let host = if account.pol_server.is_empty() { &account.server } else { &account.pol_server };
        state(app, "signing-in", format!("Signing in to PlayOnline as {}…", account.login));
        let mut s = PolSession::sign_in(host, &account.login, &req.password).map_err(|e| format!("Sign-in failed: {e}"))?;
        session = Some(s.ffxi_session_value().map_err(|e| format!("No FFXI session: {e}"))?);
        pol = Some(s);
    }

    let _ = fs::create_dir_all(&paths.log_dir);
    let log_path = paths.log_dir.join("host64.log");
    // the session before stays beside it (a crash, then a retry)
    let _ = fs::rename(&log_path, paths.log_dir.join("host64.previous.log"));
    let log = Arc::new(Mutex::new(File::create(&log_path).ok()));
    let args = host_args(cfg, paths, account, session.as_deref(), &req.otp, &req.own_game_path);
    log_line(app, &log, &format!("> {} {}", paths.host.display(), args.join(" ")));

    let mut cmd = Command::new(&paths.host);
    cmd.args(&args).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    if let Some(dir) = paths.host.parent() {
        cmd.current_dir(dir);
    }
    if account.kind == AccountKind::Lsb {
        cmd.env("FFXI_PASSWORD", &req.password);
    }
    if cfg.game.gpu_probe {
        cmd.env("FFXI_PROBE", "gpu");
    }
    // the scene effects read the same file as --live
    cmd.env("FFXI_FX_FILE", paths.config_dir.join("live.txt"));
    if !cfg.game.fps_overlay {
        cmd.env("FFXI_FPS", "0");
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: its output comes here
    }
    let mut child = cmd.spawn().map_err(|e| format!("Cannot start {}: {e}", paths.host.display()))?;
    let pipes: Vec<Box<dyn Read + Send>> = vec![
        Box::new(child.stdout.take().unwrap()),
        Box::new(child.stderr.take().unwrap()),
    ];
    let lobby_error = Arc::new(Mutex::new(None::<u32>));
    for pipe in pipes {
        let (app, log, lobby_error) = (app.clone(), log.clone(), lobby_error.clone());
        thread::spawn(move || {
            for line in BufReader::new(pipe).lines().map_while(Result::ok) {
                log_line(&app, &log, &line);
                if let Some(msg) = line.strip_prefix("@launcher ") {
                    host_message(&app, msg, &lobby_error);
                }
            }
        });
    }
    let child = Arc::new(Mutex::new(child));
    *running.child.lock().unwrap() = Some(child.clone());
    state(app, "running", format!("Playing as {}", account.name_or_login()));
    show_window(app, false);

    let status = loop {
        if let Some(status) = child.lock().unwrap().try_wait().map_err(|e| e.to_string())? {
            break status;
        }
        match pol.as_mut() {
            Some(s) => {
                if !s.pump(500) {
                    log_line(app, &log, "[launcher] the PlayOnline session closed while the game runs");
                    pol = None;
                }
            }
            None => thread::sleep(Duration::from_millis(500)),
        }
    };
    drop(pol); // signs out
    if let Some(code) = *lobby_error.lock().unwrap() {
        return Err(lobby_error_text(code));
    }
    match status.code() {
        Some(0) => Ok("The game exited.".into()),
        Some(c) => Err(format!("The game exited with code {c}. The log is at {}.", log_path.display())),
        None => Ok("The game was stopped.".into()),
    }
}

/// What host64 tells the launcher on its stdout (host64.c, --live): its hotkey was pressed, or the
/// lobby turned the game away.
fn host_message(app: &AppHandle, msg: &str, lobby_error: &Mutex<Option<u32>>) {
    let mut words = msg.split_whitespace();
    match words.next() {
        Some("settings") => {
            show_window(app, true);
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.set_always_on_top(true);
                let _ = w.set_focus();
            }
            let _ = app.emit("open-page", "game");
        }
        Some("window-size") => {
            // the size the player gave the window: it opens so next time
            let n: Vec<u32> = words.filter_map(|w| w.parse().ok()).collect();
            if let (Some(&w), Some(&h), Ok(dir)) = (n.first(), n.get(1), app.path().app_config_dir()) {
                let mut cfg = crate::config::load(&dir);
                cfg.game.window_points = (w, h);
                let _ = crate::config::save(&dir, &cfg);
                let _ = app.emit("window-size", (w, h));
            }
        }
        Some("lobby-error") => {
            if let Some(code) = words.next().and_then(|c| c.parse::<u32>().ok()) {
                *lobby_error.lock().unwrap() = Some(code);
                let _ = app.emit("lobby-error", code);
            }
        }
        _ => {}
    }
}

/// The lobby's error codes (LandSandBoat's loginErrors) the launcher can say something useful about.
pub fn lobby_error_text(code: u32) -> String {
    match code {
        331 => "The server needs a different version of the game (lobby error 331). Update the game for this server, then play again.".into(),
        201 => "The server still has this character signed in from the last session (lobby error 201). Wait a minute or two, then play again.".into(),
        _ => format!("The server's lobby turned the game away (error {code})."),
    }
}

pub fn stop(running: &Running) {
    if let Some(child) = running.child.lock().unwrap().as_ref() {
        let _ = child.lock().unwrap().kill();
    }
}

pub fn is_running(running: &Running) -> bool {
    *running.busy.lock().unwrap()
}

impl Account {
    fn name_or_login(&self) -> &str {
        if self.name.is_empty() {
            &self.login
        } else {
            &self.name
        }
    }
}
