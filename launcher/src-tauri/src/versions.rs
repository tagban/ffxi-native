//! Versions of the game's files (xi-vault, ../../vault): backing up the install, checking and
//! repairing it, and bringing the version a server wants from the address it publishes them at.
//!
//! A version is FFXiMain.dll, FFXi.dll and the DATs together (FFXiMain.dll decides how the DATs are
//! read), so a server's version is never mixed into the player's install: it is put together
//! beside it, in <vault>/installs/<version>/FINAL FANTASY XI, and the account plays from there.

use crate::config::{self, LauncherConfig};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::thread;
use tauri::{AppHandle, Emitter, Manager};
use xi_vault::{Manifest, Vault};

/// One vault job at a time; the window shows its progress (task-progress, task-done).
#[derive(Default)]
pub struct Tasks {
    busy: Mutex<Option<&'static str>>,
}

#[derive(Serialize, Clone)]
struct TaskProgress {
    task: &'static str,
    fraction: f64,
    step: String,
}

#[derive(Serialize, Clone)]
pub struct TaskDone {
    pub task: &'static str,
    pub ok: bool,
    pub message: String,
    /// what the job found or made (a check's counts, the folder a version was put together in)
    pub detail: serde_json::Value,
}

pub fn vault_dir(app: &AppHandle, cfg: &LauncherConfig) -> Result<PathBuf, String> {
    if !cfg.vault_dir.is_empty() {
        return Ok(PathBuf::from(&cfg.vault_dir));
    }
    Ok(app.path().app_data_dir().map_err(|e| e.to_string())?.join("vault"))
}

fn config_dir(app: &AppHandle) -> Result<PathBuf, String> {
    app.path().app_config_dir().map_err(|e| e.to_string())
}

fn step_text(what: &str, done: u64, total: u64) -> String {
    let verb = match what {
        "snapshot" => "Backing up",
        "verify" => "Checking",
        "fetch" => "Downloading",
        "unpack" => "Unpacking",
        "materialize" => "Putting the version together",
        _ => what,
    };
    format!("{verb}… {} of {}", xi_vault::human(done), xi_vault::human(total))
}

/// Runs a vault job on its own thread, with its progress to the window.
fn run_task(
    app: AppHandle,
    tasks: Arc<Tasks>,
    task: &'static str,
    job: impl FnOnce(&AppHandle, &(dyn Fn(&str, u64, u64) + Sync)) -> Result<(String, serde_json::Value), String> + Send + 'static,
) -> Result<(), String> {
    {
        let mut busy = tasks.busy.lock().unwrap();
        if let Some(t) = *busy {
            return Err(format!("Busy ({t}); wait for it to finish."));
        }
        *busy = Some(task);
    }
    thread::spawn(move || {
        let last = Mutex::new(std::time::Instant::now() - std::time::Duration::from_secs(1));
        let progress = |what: &str, done: u64, total: u64| {
            let mut l = last.lock().unwrap();
            if l.elapsed().as_millis() < 150 && done < total {
                return;
            }
            *l = std::time::Instant::now();
            let fraction = if total > 0 { done as f64 / total as f64 } else { 1.0 };
            let _ = app.emit("task-progress", TaskProgress { task, fraction, step: step_text(what, done, total) });
        };
        let result = job(&app, &progress);
        *tasks.busy.lock().unwrap() = None;
        let done = match result {
            Ok((message, detail)) => TaskDone { task, ok: true, message, detail },
            Err(message) => TaskDone { task, ok: false, message, detail: serde_json::Value::Null },
        };
        let _ = app.emit("task-done", done);
    });
    Ok(())
}

/// The version in the vault that this install is, by its FFXiMain.dll and FFXi.dll.
fn version_of(vault: &Vault, game: &Path) -> Option<Manifest> {
    xi_vault::identify(vault, game).ok().flatten()
}

#[derive(Serialize)]
pub struct VersionInfo {
    version: String,
    build: String,
    files: usize,
    bytes: u64,
    /// put together as an install of its own
    installed: String,
}

#[derive(Serialize)]
pub struct VersionsStatus {
    vault_dir: String,
    versions: Vec<VersionInfo>,
    /// the version the chosen install is, when it is backed up
    install_version: String,
}

pub fn status(app: &AppHandle) -> Result<VersionsStatus, String> {
    let cfg = config::load(&config_dir(app)?);
    let dir = vault_dir(app, &cfg)?;
    let vault = Vault::open(&dir)?;
    let versions = vault
        .versions()?
        .into_iter()
        .map(|m| {
            let inst = install_path(&dir, &m.version);
            VersionInfo {
                installed: if inst.join("FFXiMain.dll").is_file() { inst.to_string_lossy().into_owned() } else { String::new() },
                files: m.files.len(),
                bytes: m.bytes(),
                version: m.version,
                build: m.build,
            }
        })
        .collect();
    let install_version = if cfg.game_path.is_empty() {
        String::new()
    } else {
        version_of(&vault, Path::new(&cfg.game_path)).map(|m| m.version).unwrap_or_default()
    };
    Ok(VersionsStatus { vault_dir: dir.to_string_lossy().into_owned(), versions, install_version })
}

fn install_path(vault_dir: &Path, version: &str) -> PathBuf {
    vault_dir.join("installs").join(version).join("FINAL FANTASY XI")
}

/// Backs up the chosen install: every file of its version into the vault.
pub fn backup(app: AppHandle, tasks: Arc<Tasks>) -> Result<(), String> {
    let cfg = config::load(&config_dir(&app)?);
    if cfg.game_path.is_empty() {
        return Err("Choose the game folder first.".into());
    }
    let dir = vault_dir(&app, &cfg)?;
    run_task(app, tasks, "backup", move |_, progress| {
        let vault = Vault::open(&dir)?;
        let m = vault.snapshot(Path::new(&cfg.game_path), None, progress)?;
        Ok((
            format!("Backed up version {}: {} files, {}.", m.version, m.files.len(), xi_vault::human(m.bytes())),
            serde_json::json!({ "version": m.version }),
        ))
    })
}

/// Checks the chosen install against its backed-up version; with `repair`, puts back what is
/// missing or damaged.
pub fn check(app: AppHandle, tasks: Arc<Tasks>, full: bool, repair: bool) -> Result<(), String> {
    let cfg = config::load(&config_dir(&app)?);
    let dir = vault_dir(&app, &cfg)?;
    let game = PathBuf::from(&cfg.game_path);
    run_task(app, tasks, if repair { "repair" } else { "check" }, move |_, progress| {
        let vault = Vault::open(&dir)?;
        let m = version_of(&vault, &game)
            .ok_or("This install's version is not backed up, so there is nothing to check it against. Back it up first (while it is whole).")?;
        let c = xi_vault::verify(&game, &m, full, progress)?;
        let bad = c.missing.len() + c.wrong.len();
        let detail = serde_json::json!({
            "version": m.version, "missing": c.missing.len(), "wrong": c.wrong.len(),
            "files": c.missing.iter().chain(&c.wrong).take(20).map(|e| e.path.clone()).collect::<Vec<_>>(),
        });
        if bad == 0 {
            return Ok((format!("Every file of version {} is there and whole.", m.version), detail));
        }
        if repair {
            let n = xi_vault::repair(&game, &vault, &c)?;
            return Ok((format!("Repaired {n} files from the backup of version {}.", m.version), detail));
        }
        Ok((format!("{} files missing and {} damaged (version {}). Repair puts them back from the backup.", c.missing.len(), c.wrong.len(), m.version), detail))
    })
}

#[derive(Serialize)]
pub struct ServerVersion {
    /// the version the server wants
    pub current: String,
    pub build: String,
    /// the recompiler knows that build (else the launcher cannot make the game for it)
    pub supported: bool,
    /// the install this account would play from has it
    pub have: bool,
    /// the install folder for it: the player's own, or one put together in the vault
    pub game_path: String,
}

/// Where the account's server publishes its game versions: the address it names, else the first
/// place xi_vault::site_candidates finds one (then remembered). A remembered address that stops answering
/// is looked for again: a server that moved its versions elsewhere.
fn update_url(app: &AppHandle, account_id: &str) -> Result<(String, xi_vault::Index), String> {
    let cdir = config_dir(app)?;
    let mut cfg = config::load(&cdir);
    let account = cfg.accounts.iter_mut().find(|a| a.id == account_id).ok_or("No such account.")?;
    let saved = account.update_url.clone();
    let failed = if saved.is_empty() {
        None
    } else {
        match xi_vault::fetch_index(&saved) {
            Ok(index) => return Ok((saved, index)),
            Err(e) => Some(e),
        }
    };
    let candidates: Vec<String> = xi_vault::site_candidates(&account.server).into_iter().filter(|u| *u != saved).collect();
    match xi_vault::first_site(&candidates) {
        Some((url, index)) => {
            account.update_url = url.clone();
            config::save(&cdir, &cfg)?;
            Ok((url, index))
        }
        None => Err(failed.unwrap_or_else(|| "This server has no game updates address.".into())),
    }
}

/// The account's server's game versions: where they are published, and the one its server wants.
struct Site {
    url: String,
    index: xi_vault::Index,
    want: String,
}

/// Asks the game server first (a LandSandBoat that answers LOGIN_VERSION_INFO): the version it
/// wants (its CLIENT_VER, up or down from what the player has) and where it publishes it (its
/// UPDATE_URL). Else the site found by update_url, and the version that site hands out.
fn server_site(app: &AppHandle, account_id: &str) -> Result<Site, String> {
    let cdir = config_dir(app)?;
    let mut cfg = config::load(&cdir);
    let account = cfg.accounts.iter_mut().find(|a| a.id == account_id).ok_or("No such account.")?;
    let info = match account.kind {
        config::AccountKind::Lsb if !account.server.trim().is_empty() => {
            let port = if account.auth_port == 0 { xi_vault::LOGIN_PORT } else { account.auth_port };
            xi_vault::server_info(&format!("{}:{port}", account.server.trim()), std::time::Duration::from_secs(2)).ok()
        }
        _ => None,
    };
    let named = info.as_ref().map(|i| i.update_url.trim().to_string()).filter(|u| !u.is_empty());
    let (url, index) = match named {
        Some(u) => {
            let index = xi_vault::fetch_index(&u).map_err(|e| format!("The server's game updates address ({u}) did not answer: {e}"))?;
            if account.update_url != u {
                account.update_url = u.clone();
                config::save(&cdir, &cfg)?;
            }
            (u, index)
        }
        None => update_url(app, account_id)?,
    };
    let want = match info.filter(|i| !i.client_ver.trim().is_empty()) {
        Some(i) => xi_vault::pick_version(&index, i.client_ver.trim())
            .ok_or(format!("The server wants version {}, which its game updates address does not publish.", i.client_ver.trim()))?,
        None => index.current.clone(),
    };
    Ok(Site { url, index, want })
}

/// What version the account's server wants, and whether the player has it.
pub fn check_server(app: &AppHandle, account_id: &str) -> Result<ServerVersion, String> {
    let Site { index, want, .. } = server_site(app, account_id)?;
    let cfg = config::load(&config_dir(app)?);
    let info = index.versions.iter().find(|v| v.version == want).ok_or("The server's index names no current version.")?;
    let dir = vault_dir(app, &cfg)?;
    let vault = Vault::open(&dir)?;
    // the player's own install, if it is that version; else one put together from the vault
    let own = Path::new(&cfg.game_path);
    let (have, game_path) = if xi_vault::is_version(&vault, own, &want) {
        (true, cfg.game_path.clone())
    } else {
        // one put together before (a server that went back to an older version, or another
        // server on it): all of it, not a copy cut short
        let p = install_path(&dir, &want);
        let whole = p.join("FFXiMain.dll").is_file() && xi_vault::is_version(&vault, &p, &want);
        (whole, p.to_string_lossy().into_owned())
    };
    if have {
        if game_path != cfg.game_path && !cfg.game_path.is_empty() {
            xi_vault::share_player_dirs(Path::new(&cfg.game_path), Path::new(&game_path))?;
        }
        // the account plays that version from now on, whichever it played before
        let cdir = config_dir(app)?;
        let mut cfg = config::load(&cdir);
        let own_path = cfg.game_path.clone();
        if let Some(a) = cfg.accounts.iter_mut().find(|a| a.id == account_id) {
            let want = if game_path == own_path { String::new() } else { game_path.clone() };
            if a.game_path != want {
                a.game_path = want;
                config::save(&cdir, &cfg)?;
            }
        }
    }
    Ok(ServerVersion {
        supported: !info.build.is_empty(),
        build: info.build.clone(),
        current: want,
        have,
        game_path,
    })
}

/// Brings the version the account's server wants: only the files the vault lacks, then put
/// together beside the player's install; the account plays from there from now on.
pub fn update_for_server(app: AppHandle, tasks: Arc<Tasks>, account_id: String) -> Result<(), String> {
    let Site { url, want, .. } = server_site(&app, &account_id)?;
    let cfg = config::load(&config_dir(&app)?);
    let account = cfg.accounts.iter().find(|a| a.id == account_id).cloned().ok_or("No such account.")?;
    let dir = vault_dir(&app, &cfg)?;
    run_task(app, tasks, "update", move |app, progress| {
        let vault = Vault::open(&dir)?;
        // the player's install first, if it is not backed up: its files are most of any version
        if !cfg.game_path.is_empty() && version_of(&vault, Path::new(&cfg.game_path)).is_none() {
            vault.snapshot(Path::new(&cfg.game_path), None, progress)?;
        }
        let m = xi_vault::fetch(&vault, &url, Some(&want), progress)?;
        let game_path = if xi_vault::is_version(&vault, Path::new(&cfg.game_path), &m.version) {
            cfg.game_path.clone()
        } else {
            let p = install_path(&dir, &m.version);
            vault.materialize(&m.version, &p, progress)?;
            if !cfg.game_path.is_empty() {
                xi_vault::share_player_dirs(Path::new(&cfg.game_path), &p)?;
            }
            p.to_string_lossy().into_owned()
        };
        // the account plays that version from now on
        let cdir = config_dir(app)?;
        let mut cfg = config::load(&cdir);
        if let Some(a) = cfg.accounts.iter_mut().find(|a| a.id == account.id) {
            a.game_path = if game_path == cfg.game_path { String::new() } else { game_path.clone() };
        }
        config::save(&cdir, &cfg)?;
        Ok((format!("Version {} is ready.", m.version), serde_json::json!({ "version": m.version, "game_path": game_path, "build": m.build })))
    })
}
