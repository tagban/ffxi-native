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

/// Where the account's server publishes its game versions: the address it names, else the
/// server itself on xi-vault's port, when something answers there (then remembered).
fn update_url(app: &AppHandle, account_id: &str) -> Result<(String, xi_vault::Index), String> {
    let cdir = config_dir(app)?;
    let mut cfg = config::load(&cdir);
    let account = cfg.accounts.iter_mut().find(|a| a.id == account_id).ok_or("No such account.")?;
    if !account.update_url.is_empty() {
        let index = xi_vault::fetch_index(&account.update_url)?;
        return Ok((account.update_url.clone(), index));
    }
    let host = account.server.trim();
    if host.is_empty() || host.contains('/') || host.matches(':').count() > 1 {
        return Err("This server has no game updates address.".into());
    }
    let url = format!("http://{host}:{}", xi_vault::DEFAULT_PORT);
    let index = xi_vault::fetch_index_within(&url, std::time::Duration::from_millis(1500))
        .map_err(|_| "This server has no game updates address.".to_string())?;
    account.update_url = url.clone();
    config::save(&cdir, &cfg)?;
    Ok((url, index))
}

/// What version the account's server wants, and whether the player has it.
pub fn check_server(app: &AppHandle, account_id: &str) -> Result<ServerVersion, String> {
    let (_, index) = update_url(app, account_id)?;
    let cfg = config::load(&config_dir(app)?);
    let info = index.versions.iter().find(|v| v.version == index.current).ok_or("The server's index names no current version.")?;
    let dir = vault_dir(app, &cfg)?;
    let vault = Vault::open(&dir)?;
    // the player's own install, if it is that version; else one put together from the vault
    let own = Path::new(&cfg.game_path);
    let own_version = version_of(&vault, own).map(|m| m.version);
    let (have, game_path) = if own_version.as_deref() == Some(index.current.as_str()) {
        (true, cfg.game_path.clone())
    } else {
        let p = install_path(&dir, &index.current);
        (p.join("FFXiMain.dll").is_file(), p.to_string_lossy().into_owned())
    };
    Ok(ServerVersion {
        supported: !info.build.is_empty(),
        build: info.build.clone(),
        current: index.current,
        have,
        game_path,
    })
}

/// Brings the version the account's server wants: only the files the vault lacks, then put
/// together beside the player's install; the account plays from there from now on.
pub fn update_for_server(app: AppHandle, tasks: Arc<Tasks>, account_id: String) -> Result<(), String> {
    let (url, _) = update_url(&app, &account_id)?;
    let cfg = config::load(&config_dir(&app)?);
    let account = cfg.accounts.iter().find(|a| a.id == account_id).cloned().ok_or("No such account.")?;
    let dir = vault_dir(&app, &cfg)?;
    run_task(app, tasks, "update", move |app, progress| {
        let vault = Vault::open(&dir)?;
        // the player's install first, if it is not backed up: its files are most of any version
        if !cfg.game_path.is_empty() && version_of(&vault, Path::new(&cfg.game_path)).is_none() {
            vault.snapshot(Path::new(&cfg.game_path), None, progress)?;
        }
        let m = xi_vault::fetch(&vault, &url, None, progress)?;
        let own = version_of(&vault, Path::new(&cfg.game_path)).map(|m| m.version);
        let game_path = if own.as_deref() == Some(m.version.as_str()) {
            cfg.game_path.clone()
        } else {
            let p = install_path(&dir, &m.version);
            vault.materialize(&m.version, &p, progress)?;
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
