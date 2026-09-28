// The FINAL FANTASY XI launcher: accounts and connections (PlayOnline or LandSandBoat), the game's
// settings, and starting host64 with the right arguments. Same program on macOS and Windows.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod config;
mod game;
mod pol;
mod setup;
mod versions;

use config::LauncherConfig;
use game::{LaunchRequest, Paths, Running};
use setup::Building;
use versions::Tasks;
use serde::Serialize;
use std::path::PathBuf;
use std::sync::Arc;
use tauri::{AppHandle, Manager, State};

const KEYCHAIN_SERVICE: &str = "com.tagban.ffxi-native";

fn config_dir(app: &AppHandle) -> Result<PathBuf, String> {
    app.path().app_config_dir().map_err(|e| e.to_string())
}

fn keychain(account_id: &str) -> Result<keyring::Entry, String> {
    keyring::Entry::new(KEYCHAIN_SERVICE, account_id).map_err(|e| e.to_string())
}

/// Passwords read from (or given to) the keychain this run, so a second Play does not ask again.
#[derive(Default)]
struct Passwords(std::sync::Mutex<std::collections::HashMap<String, String>>);

fn stored_password(passwords: &Passwords, account_id: &str) -> Option<String> {
    let mut cache = passwords.0.lock().unwrap();
    if let Some(p) = cache.get(account_id) {
        return Some(p.clone());
    }
    let p = keychain(account_id).ok()?.get_password().ok()?;
    cache.insert(account_id.to_string(), p.clone());
    Some(p)
}

/// host64 when the configuration names none: in the app bundle as its own app (macOS:
/// Contents/Helpers/FINAL FANTASY XI.app, so the game has its own Dock icon and name), beside the
/// launcher (Windows, or a plain build), else the repository's build/ (a development run).
fn default_host() -> PathBuf {
    let name = if cfg!(windows) { "host64.exe" } else { "host64" };
    let mut candidates = Vec::new();
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(|p| p.to_path_buf())) {
        candidates.push(dir.join("../Helpers/FINAL FANTASY XI.app/Contents/MacOS").join(name));
        candidates.push(dir.join(name));
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build").join(name));
    candidates.iter().find(|p| p.exists()).cloned().unwrap_or_else(|| candidates.remove(0))
}

fn default_registry(app: &AppHandle) -> PathBuf {
    app.path()
        .resolve("playonline.reg", tauri::path::BaseDirectory::Resource)
        .ok()
        .filter(|p| p.exists())
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../playonline.reg"))
}

fn paths(app: &AppHandle, cfg: &LauncherConfig) -> Result<Paths, String> {
    Ok(Paths {
        config_dir: config_dir(app)?,
        log_dir: app.path().app_log_dir().map_err(|e| e.to_string())?,
        // the one named in Settings, else the game made for this install (setup.rs), else a bundled one
        host: if !cfg.host_program.is_empty() {
            PathBuf::from(&cfg.host_program)
        } else {
            setup::game_for(app, &cfg.game_path).map(|g| g.host).unwrap_or_else(default_host)
        },
        module: if cfg.host_program.is_empty() { setup::game_for(app, &cfg.game_path).and_then(|g| g.module) } else { None },
        base_registry: if cfg.base_registry.is_empty() { default_registry(app) } else { PathBuf::from(&cfg.base_registry) },
    })
}

#[derive(Serialize)]
struct Defaults {
    host_program: String,
    base_registry: String,
    config_dir: String,
    log_dir: String,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Result<LauncherConfig, String> {
    Ok(config::load(&config_dir(&app)?))
}

#[tauri::command]
fn save_config(app: AppHandle, cfg: LauncherConfig) -> Result<(), String> {
    let dir = config_dir(&app)?;
    // accounts that went away take their stored password with them
    for old in config::load(&dir).accounts {
        if !cfg.accounts.iter().any(|a| a.id == old.id) {
            if let Ok(e) = keychain(&old.id) {
                let _ = e.delete_credential();
            }
        }
    }
    // the settings that hold while the game runs: a game that is running picks them up
    let _ = std::fs::write(dir.join("live.txt"), cfg.game.to_live());
    config::save(&dir, &cfg)
}

#[tauri::command]
fn get_defaults(app: AppHandle) -> Result<Defaults, String> {
    let p = paths(&app, &LauncherConfig::default())?;
    Ok(Defaults {
        host_program: p.host.to_string_lossy().into_owned(),
        base_registry: p.base_registry.to_string_lossy().into_owned(),
        config_dir: p.config_dir.to_string_lossy().into_owned(),
        log_dir: p.log_dir.to_string_lossy().into_owned(),
    })
}

#[tauri::command]
fn game_defaults() -> config::GameSettings {
    config::GameSettings::default()
}

/// The scene effects' keys and the engine's defaults, for the sliders and presets.
#[tauri::command]
fn fx_defaults() -> std::collections::BTreeMap<String, f32> {
    config::FX_DEFAULTS.iter().map(|(k, v)| (k.to_string(), *v)).collect()
}

#[tauri::command]
fn forget_password(passwords: State<Passwords>, account_id: String) -> Result<(), String> {
    passwords.0.lock().unwrap().remove(&account_id);
    match keychain(&account_id)?.delete_credential() {
        Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}

/// Starts the game for an account. An empty password uses the stored one; a given one is stored
/// when the account says to remember it. Returns whether the keychain now holds it (the window
/// records that in the account, with last_account, and saves).
#[tauri::command]
fn launch(
    app: AppHandle,
    running: State<Arc<Running>>,
    passwords: State<Passwords>,
    account_id: String,
    password: String,
    otp: String,
) -> Result<bool, String> {
    let mut cfg = config::load(&config_dir(&app)?);
    let account = cfg
        .accounts
        .iter()
        .find(|a| a.id == account_id)
        .cloned()
        .ok_or("No such account.")?;
    let mut saved = account.password_saved;
    let password = if password.is_empty() {
        let p = stored_password(&passwords, &account.id).ok_or("Enter the password.")?;
        saved = true;
        p
    } else {
        if account.save_password {
            keychain(&account.id)?.set_password(&password).map_err(|e| e.to_string())?;
            saved = true;
        }
        passwords.0.lock().unwrap().insert(account.id.clone(), password.clone());
        password
    };
    // the account's own version of the game, when its server wants another (versions.rs)
    if !account.game_path.is_empty() {
        cfg.game_path = account.game_path.clone();
    }
    let paths = paths(&app, &cfg)?;
    game::launch(app, running.inner().clone(), cfg, paths, LaunchRequest { account, password, otp })?;
    Ok(saved)
}

// --- first run: the install, and making the game for it (setup.rs) -------------------------------

#[tauri::command]
fn setup_status(app: AppHandle) -> Result<setup::SetupStatus, String> {
    let cfg = config::load(&config_dir(&app)?);
    Ok(setup::status(&app, &cfg.game_path))
}

#[tauri::command]
fn detect_installs() -> Vec<String> {
    setup::detect_installs()
}

/// Sets the FINAL FANTASY XI folder (or one above it: PlayOnline, SquareEnix) and says what is there.
#[tauri::command]
fn set_game_folder(app: AppHandle, path: String) -> Result<setup::SetupStatus, String> {
    let dir = config_dir(&app)?;
    let game = setup::normalize_game_folder(std::path::Path::new(&path))
        .ok_or_else(|| format!("There is no FFXiMain.dll in {path} or the FINAL FANTASY XI folder under it."))?;
    let mut cfg = config::load(&dir);
    cfg.game_path = game.to_string_lossy().into_owned();
    config::save(&dir, &cfg)?;
    Ok(setup::status(&app, &cfg.game_path))
}

#[tauri::command]
fn check_prereqs(app: AppHandle) -> Vec<setup::Prereq> {
    setup::prereqs(&app)
}

#[tauri::command]
fn install_prereqs(app: AppHandle, building: State<Arc<Building>>) -> Result<(), String> {
    setup::install(app, building.inner().clone())
}

/// Makes the game for an install: the launcher's, or `game_path` (a version put together for a server).
#[tauri::command]
fn start_build(app: AppHandle, building: State<Arc<Building>>, game_path: Option<String>) -> Result<(), String> {
    let cfg = config::load(&config_dir(&app)?);
    setup::start(app, building.inner().clone(), game_path.filter(|p| !p.is_empty()).unwrap_or(cfg.game_path))
}

/// Whether the game is made for an install (the launcher's, or another).
#[tauri::command]
fn game_status(app: AppHandle, game_path: String) -> setup::SetupStatus {
    setup::status(&app, &game_path)
}

// --- versions of the game's files (versions.rs) ------------------------------------------------------

#[tauri::command]
fn versions_status(app: AppHandle) -> Result<versions::VersionsStatus, String> {
    versions::status(&app)
}

#[tauri::command]
fn backup_install(app: AppHandle, tasks: State<Arc<Tasks>>) -> Result<(), String> {
    versions::backup(app, tasks.inner().clone())
}

#[tauri::command]
fn check_install(app: AppHandle, tasks: State<Arc<Tasks>>, full: bool, repair: bool) -> Result<(), String> {
    versions::check(app, tasks.inner().clone(), full, repair)
}

#[tauri::command]
fn check_server(app: AppHandle, account_id: String) -> Result<versions::ServerVersion, String> {
    versions::check_server(&app, &account_id)
}

#[tauri::command]
fn update_for_server(app: AppHandle, tasks: State<Arc<Tasks>>, account_id: String) -> Result<(), String> {
    versions::update_for_server(app, tasks.inner().clone(), account_id)
}

#[tauri::command]
fn cancel_build(building: State<Arc<Building>>) {
    setup::cancel(&building);
}

#[tauri::command]
fn is_building(building: State<Arc<Building>>) -> bool {
    setup::is_building(&building)
}

#[tauri::command]
fn stop(running: State<Arc<Running>>) {
    game::stop(&running);
}

#[tauri::command]
fn is_running(running: State<Arc<Running>>) -> bool {
    game::is_running(&running)
}

/// `ffxi-launcher --make-game <FINAL FANTASY XI folder> [--data <dir>]`: makes the game for an
/// install from a terminal, as the setup window does (setup.rs), into the launcher's data folder.
mod cli {
    use crate::setup::{self, Building, Env};
    use std::io::Write;
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;

    struct Term {
        data: PathBuf,
        last: Mutex<String>,
    }

    impl Env for Term {
        fn data_dir(&self) -> Result<PathBuf, String> {
            Ok(self.data.clone())
        }
        fn engine_dir(&self) -> PathBuf {
            // bundled: Contents/Resources/engine beside Contents/MacOS (macOS), usr/lib/<product>/engine
            // beside usr/bin (a Linux package or AppImage); else engine/ beside the program
            let exe = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_default();
            for p in [exe.join("../Resources/engine"), exe.join("../lib/FFXI Launcher/engine"), exe.join("engine")] {
                if p.join("runtime/xi_module.c").exists() {
                    return p;
                }
            }
            setup::repo_engine()
        }
        fn progress(&self, fraction: f64, step: String) {
            let mut last = self.last.lock().unwrap();
            if *last != step {
                eprint!("\r[{:5.1}%] {step:<70}", fraction * 100.0);
                let _ = std::io::stderr().flush();
                *last = step;
            }
        }
        fn log(&self, line: &str) {
            eprintln!("\r{line:<80}");
        }
    }

    fn default_data() -> PathBuf {
        let home = std::env::var_os(if cfg!(windows) { "APPDATA" } else { "HOME" }).map(PathBuf::from).unwrap_or_default();
        let base = if cfg!(target_os = "macos") {
            home.join("Library/Application Support")
        } else if cfg!(windows) {
            home
        } else {
            std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home.join(".local/share"))
        };
        base.join("com.tagban.ffxi-native")
    }

    pub fn make_game(args: &[String]) -> i32 {
        let mut game = None;
        let mut data = default_data();
        let mut i = 0;
        while i < args.len() {
            match args[i].as_str() {
                "--data" if i + 1 < args.len() => {
                    data = PathBuf::from(&args[i + 1]);
                    i += 1;
                }
                a => game = Some(PathBuf::from(a)),
            }
            i += 1;
        }
        let Some(game) = game.and_then(|g| setup::normalize_game_folder(&g)) else {
            eprintln!("usage: ffxi-launcher --make-game <FINAL FANTASY XI folder> [--data <dir>]");
            return 2;
        };
        let env = Term { data, last: Mutex::new(String::new()) };
        let st = setup::status(&env, &game.to_string_lossy());
        let Some(label) = st.build.as_ref().map(|b| b.label.clone()).filter(|l| !l.is_empty()) else {
            eprintln!("{}", st.error);
            return 1;
        };
        let building = Building::default();
        if setup::compiler(&env).is_none() {
            if let Err(e) = setup::download_zig(&env, &building) {
                eprintln!("\n{e}");
                return 1;
            }
            eprintln!();
        }
        let t = std::time::Instant::now();
        match setup::build(&env, &building, &game, &label) {
            Ok(msg) => {
                eprintln!("\n{msg} ({:.0} s)", t.elapsed().as_secs_f64());
                0
            }
            Err(e) => {
                eprintln!("\n{e}");
                1
            }
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("--make-game") {
        std::process::exit(cli::make_game(&args[2..]));
    }
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Arc::new(Running::default()))
        .manage(Passwords::default())
        .manage(Arc::new(Building::default()))
        .manage(Arc::new(Tasks::default()))
        .invoke_handler(tauri::generate_handler![
            get_config,
            save_config,
            get_defaults,
            game_defaults,
            fx_defaults,
            forget_password,
            launch,
            stop,
            is_running,
            setup_status,
            detect_installs,
            set_game_folder,
            check_prereqs,
            install_prereqs,
            start_build,
            cancel_build,
            is_building,
            game_status,
            versions_status,
            backup_install,
            check_install,
            check_server,
            update_for_server
        ])
        .build(tauri::generate_context!())
        .expect("error while starting the launcher")
        .run(|app, event| {
            // macOS: clicking the Dock icon brings back the window hidden while the game runs
            #[cfg(target_os = "macos")]
            if let tauri::RunEvent::Reopen { .. } = event {
                if let Some(w) = app.get_webview_window("main") {
                    let _ = w.show();
                    let _ = w.set_focus();
                }
            }
            // While a game runs the launcher is hidden, and out of the Dock on macOS: an exit the
            // system or the window layer asks for then (not the player's own quit) would take the
            // game's log and its messages (the settings key, the window's size) with it
            if let tauri::RunEvent::ExitRequested { api, code: None, .. } = &event {
                if game::is_running(&app.state::<Arc<Running>>()) {
                    api.prevent_exit();
                }
            }
            let _ = (app, event);
        });
}
