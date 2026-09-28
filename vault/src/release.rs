//! The server operator's double-click tool (xi-vault with no arguments, or `xi-vault release`).
//!
//! After PlayOnline updates the game on the operator's PC, it reads the install, asks the update
//! server what it hands out now, and when the install is newer, writes an update bundle of only the
//! files the server lacks. With an SSH login set up, it uploads the bundle and has the server's
//! xi-vault take it in (`xi-vault apply`); else it says how.
//!
//! Its settings are xi-release.json beside it: the game folder, the server, the SSH login.

use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::io::{BufRead, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use xi_vault::*;

#[derive(Serialize, Deserialize, Default)]
struct Settings {
    #[serde(default)]
    game: String,
    #[serde(default)]
    server: String,
    /// the SSH login for the server ("root@ffxi.cc"); "" to upload by hand; absent: not asked yet
    #[serde(default)]
    upload: Option<String>,
    #[serde(default = "default_site")]
    remote_site: String,
    #[serde(default = "default_xi_vault")]
    remote_xi_vault: String,
}

fn default_site() -> String {
    "/srv/xi-vault/site".into()
}

fn default_xi_vault() -> String {
    "xi-vault".into()
}

pub struct Args {
    pub game: Option<PathBuf>,
    pub server: Option<String>,
    pub out: Option<PathBuf>,
    pub upload: Option<String>,
    pub current: bool,
}

/// Beside the program (where a double-clicked tool keeps its things), else here.
fn home() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| PathBuf::from("."))
}

fn ask(q: &str) -> String {
    print!("{q} ");
    let _ = std::io::stdout().flush();
    let mut s = String::new();
    let _ = std::io::stdin().lock().read_line(&mut s);
    // a folder dragged into a Windows console comes quoted
    s.trim().trim_matches('"').trim().to_string()
}

fn yes_no(q: &str, default: bool) -> bool {
    let a = ask(&format!("{q} [{}]", if default { "Y/n" } else { "y/N" })).to_ascii_lowercase();
    if a.is_empty() { default } else { a.starts_with('y') }
}

fn is_game(p: &Path) -> bool {
    p.join("FFXiMain.dll").is_file()
}

/// Where PlayOnline put the game: beside this program, the registry, the usual folders.
fn find_game() -> Option<PathBuf> {
    let mut tries: Vec<PathBuf> = home().ancestors().map(Path::to_path_buf).collect();
    if cfg!(windows) {
        for region in ["PlayOnlineUS", "PlayOnlineEU", "PlayOnline"] {
            let key = format!(r"HKLM\SOFTWARE\WOW6432Node\{region}\InstallFolder");
            if let Ok(o) = Command::new("reg").args(["query", &key, "/v", "0001"]).output() {
                let text = String::from_utf8_lossy(&o.stdout).into_owned();
                if let Some(line) = text.lines().find(|l| l.contains("REG_SZ")) {
                    if let Some(p) = line.split("REG_SZ").nth(1) {
                        tries.push(PathBuf::from(p.trim()));
                    }
                }
            }
        }
        for pf in [r"C:\Program Files (x86)", r"C:\Program Files"] {
            tries.push(Path::new(pf).join(r"PlayOnline\SquareEnix\FINAL FANTASY XI"));
        }
    }
    tries.into_iter().find(|p| is_game(p))
}

/// "ffxi.cc" -> http://ffxi.cc:54080; an address with a scheme stays as it is.
fn server_url(s: &str) -> String {
    let s = s.trim().trim_end_matches('/');
    if s.contains("://") { s.to_string() } else if s.contains(':') { format!("http://{s}") } else { format!("http://{s}:{DEFAULT_PORT}") }
}

/// "30260805_0" as numbers, for which is newer.
fn version_key(v: &str) -> Option<(u64, u64)> {
    let (d, n) = v.split_once('_')?;
    Some((d.parse().ok()?, n.parse().ok()?))
}

fn rule() {
    println!("{}", "-".repeat(72));
}

pub fn release(a: Args, interactive: bool, p: Progress) -> Result<()> {
    let settings_path = home().join("xi-release.json");
    let mut s: Settings = std::fs::read_to_string(&settings_path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
    if s.remote_site.is_empty() {
        s.remote_site = default_site();
    }
    if s.remote_xi_vault.is_empty() {
        s.remote_xi_vault = default_xi_vault();
    }
    println!("FINAL FANTASY XI: publish a game update to your server's players");
    rule();

    // the game
    let mut game = a.game.clone().or_else(|| Some(PathBuf::from(&s.game)).filter(|p| is_game(p))).or_else(find_game);
    while !game.as_deref().is_some_and(is_game) {
        if !interactive {
            return Err("give the FINAL FANTASY XI folder (--game)".into());
        }
        if let Some(g) = &game {
            println!("{} has no FFXiMain.dll.", g.display());
        }
        let g = ask("Where is your FINAL FANTASY XI folder? (drag it here, then press Enter)\n>");
        if g.is_empty() {
            return Err("no game folder".into());
        }
        game = Some(PathBuf::from(g));
    }
    let game = game.unwrap();
    s.game = game.to_string_lossy().into_owned();

    // the server
    let mut server = a.server.clone().unwrap_or_else(|| s.server.clone());
    if server.is_empty() {
        if !interactive {
            return Err("give the update server (--server)".into());
        }
        server = ask("Your update server (for example ffxi.cc):\n>");
        if server.is_empty() {
            return Err("no server".into());
        }
    }
    let url = server_url(&server);
    s.server = server;
    if let Some(u) = &a.upload {
        s.upload = Some(u.clone());
    }
    if s.upload.is_none() && interactive {
        println!("\nThis can upload the update to your server over SSH (the server needs xi-vault on it).");
        s.upload = Some(ask("Your SSH login for it (like root@ffxi.cc), or Enter to copy updates there yourself:\n>"));
    }
    if interactive || a.game.is_some() || a.server.is_some() {
        let _ = std::fs::write(&settings_path, serde_json::to_string_pretty(&s).unwrap());
    }
    println!("\nGame:   {}\nServer: {url}", game.display());

    // what the server hands out
    let index = fetch_index(&url).map_err(|e| format!("could not reach the update server: {e}"))?;
    let current = index.current.clone();
    let entry = index.versions.iter().find(|v| v.version == current).ok_or(format!("the server's index names {current} but does not list it"))?;
    let cm = fetch_manifest(&url, &current)?;
    let bm = if entry.base.is_empty() { None } else { Some(fetch_manifest(&url, &entry.base)?) };
    println!("The server hands out: {current}\n");

    // the install, quickly by patch.cfg, then every file
    if let (Some(mine), Some(theirs)) = (patched_version(&game).as_deref().and_then(version_key), version_key(&current)) {
        if mine < theirs {
            println!("\nYour game is {}, older than the server's {current}. Update it with PlayOnline first.", patched_version(&game).unwrap());
            return Ok(());
        }
    }
    let m = hash_install(&game, None, "reading your game files", p)?;
    println!("Your game is:        {}", m.version);
    rule();

    let files = |m: &Manifest| m.files.iter().map(|e| (e.path.clone(), e.sha256.clone())).collect::<BTreeSet<_>>();
    if files(&m) == files(&cm) {
        println!("The server already hands out exactly this game. Nothing to do.");
        return Ok(());
    }
    let d = diff(&cm, &m);
    if m.version == current {
        println!("Your game is {current} too, but {} files differ from what the server hands out under that name", d.added.len() + d.changed.len() + d.removed.len());
        println!("(a mod on one side, or a damaged file). Nothing was made. The first ones:");
        for e in d.added.iter().chain(&d.changed).chain(&d.removed).take(10) {
            println!("  {}", e.path);
        }
        return Ok(());
    }
    if index.versions.iter().any(|v| v.version == m.version) {
        println!("The server has {} already, but hands out {current}.", m.version);
        println!("To hand out {} on the server: {} current {} {}", m.version, s.remote_xi_vault, s.remote_site, m.version);
        return Ok(());
    }
    println!("{current} -> {}:", m.version);
    println!("  {} new files, {} changed, {} removed", d.added.len(), d.changed.len(), d.removed.len());
    let mut folders: std::collections::BTreeMap<&str, (usize, u64)> = Default::default();
    for e in &d.new_objects {
        let top = e.path.split('/').next().filter(|_| e.path.contains('/')).unwrap_or("(game folder)");
        let f = folders.entry(top).or_default();
        f.0 += 1;
        f.1 += e.size;
    }
    for (k, (n, b)) in &folders {
        println!("    {k:<14} {n:>6} files  {:>10}", human(*b));
    }
    let dlls_changed = m.ffximain_sha256 != cm.ffximain_sha256 || m.ffxi_sha256 != cm.ffxi_sha256;
    let unknown = m.build.is_empty();
    if dlls_changed {
        println!("\n  The game's program changed (FFXiMain.dll / FFXi.dll).");
    }
    if unknown {
        rule();
        println!("  NOTE: the launcher does not know this FFXiMain.dll yet. Players on Mac and Linux");
        println!("  cannot play this version until a launcher update adds it. Upload it now if you like,");
        println!("  but do not hand it out (or change the server's CLIENT_VER) before that update.");
    }
    rule();

    // the bundle
    let mut hosted: BTreeSet<String> = cm.files.iter().map(|e| e.sha256.clone()).collect();
    if let Some(b) = &bm {
        hosted.extend(b.files.iter().map(|e| e.sha256.clone()));
    }
    let name = format!("ffxi-update-{}.tar", m.version);
    let out = a.out.clone().unwrap_or_else(home).join(&name);
    let h = make_bundle(&game, &m, &hosted, &entry.base, &current, &out, p)?;
    let size = std::fs::metadata(&out).map(|m| m.len()).unwrap_or(0);
    println!("Made {}\n  {} files, {}; the update file is {}", out.display(), h.objects, human(h.bytes), human(size));

    // to the server
    let login = s.upload.clone().unwrap_or_default();
    let apply = |current: bool| format!("{} apply {} <file>{}", s.remote_xi_vault, s.remote_site, if current { " --current" } else { "" });
    if login.is_empty() {
        println!("\nTo publish it: copy it to your server, then there run\n  {}", apply(false));
        println!("and to hand it out to players, add --current (and set CLIENT_VER in LandSandBoat's login.lua to {}).", m.version);
        return Ok(());
    }
    if interactive && !yes_no(&format!("\nUpload it to {login} now?"), true) {
        println!("Not uploaded. To publish it by hand: copy it there and run\n  {}", apply(false));
        return Ok(());
    }
    let hand_out = if interactive {
        println!("\nHanding it out means players' launchers download it on their next Play.");
        println!("Set CLIENT_VER in LandSandBoat's login.lua to {} at the same time.", m.version);
        yes_no("Hand it out to players now?", false)
    } else {
        a.current
    };
    let remote = format!("/tmp/{name}");
    println!("> scp {} {login}:{remote}", out.display());
    let ok = Command::new("scp").arg(&out).arg(format!("{login}:{remote}")).status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        return Err(format!("the upload to {login} failed (can you sign in with: ssh {login}?)"));
    }
    let cmd = format!(
        "{} apply '{}' '{remote}'{}; rc=$?; rm -f '{remote}'; exit $rc",
        s.remote_xi_vault,
        s.remote_site.replace('\'', ""),
        if hand_out { " --current" } else { "" }
    );
    println!("> ssh {login} {cmd}");
    let ok = Command::new("ssh").arg(&login).arg(&cmd).status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        return Err(format!("the server did not take the update in (above); the bundle is still at {}", out.display()));
    }
    rule();
    if hand_out {
        println!("Done: your server hands out {} now. Players get it on their next Play.", m.version);
    } else {
        println!("Done: {} is on your server. To hand it out there: {} current {} {}", m.version, s.remote_xi_vault, s.remote_site, m.version);
    }
    Ok(())
}
