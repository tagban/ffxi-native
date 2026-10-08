//! The launcher's persistent configuration. Shared things (the install, the game program, the
//! list of profiles) are in <app config dir>/launcher.json; each profile (an account to play, with
//! its own game settings) has a folder, profiles/<id>/, holding its profile.json and the game's own
//! files for it (its saved settings, the overlay's layout, its sign-in choices), so two profiles
//! can play at once without writing each other's files. Passwords are not in any of them; they go
//! to the OS keychain.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
pub enum AccountKind {
    /// A server with PlayOnline behind it: the launcher signs in and hands host64 the session value.
    Pol,
    /// A LandSandBoat server (xiloader's protocol): host64 signs in itself.
    Lsb,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct Account {
    pub id: String,
    pub name: String,
    pub kind: AccountKind,
    /// The PlayOnline ID (Pol) or the account name (Lsb).
    pub login: String,
    /// Where the game's servers are: the lobby, and the LandSandBoat sign-in.
    pub server: String,
    /// Pol: the PlayOnline sign-in server, when it is not `server`.
    pub pol_server: String,
    /// Remember the password in the OS keychain.
    pub save_password: bool,
    /// One is in the keychain. Kept here so showing the account never reads the keychain (each
    /// read can ask the user for access).
    pub password_saved: bool,
    /// Lsb: LandSandBoat's ports, 0 for xiloader's defaults (54231, 54230, 54001).
    pub auth_port: u16,
    pub data_port: u16,
    pub view_port: u16,
    /// Where the server publishes its game versions (xi-vault publish / serve): the launcher
    /// checks it on Play and brings the version the server wants. Empty: none.
    pub update_url: String,
    /// The FINAL FANTASY XI folder this account plays from, when not the launcher's (a version
    /// put together for its server, versions.rs). Empty: the launcher's.
    pub game_path: String,
    /// The version over the launcher's install this account plays, when its server wants another:
    /// an overlay of the files that version changes (<vault>/overlays/<version>, versions.rs), laid
    /// over the install by the game host. Empty: the install as it is.
    pub version_dir: String,
    /// Lsb: the xiloader protocol its login server speaks ("2.2.0"), as it last said (versions.rs);
    /// empty: the host finds out when signing in.
    pub loader: String,
    /// This profile's game settings (each its own: two playing at once can differ).
    pub game: GameSettings,
}

impl Default for Account {
    fn default() -> Self {
        Account {
            id: String::new(),
            name: String::new(),
            kind: AccountKind::Lsb,
            login: String::new(),
            server: "127.0.0.1".into(),
            pol_server: String::new(),
            save_password: true,
            password_saved: false,
            auth_port: 0,
            data_port: 0,
            view_port: 0,
            update_url: String::new(),
            loader: String::new(),
            game_path: String::new(),
            version_dir: String::new(),
            game: GameSettings::default(),
        }
    }
}

/// The game's own settings (what FINAL FANTASY XI's config tool writes), in
/// HKLM\SOFTWARE\PlayOnlineUS\SquareEnix\FinalFantasyXI.
#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct GameSettings {
    /// 0 full screen, 1 windowed, 2 borderless window, 3 borderless full screen (0034)
    pub window_mode: u32,
    pub window_width: u32, // 0001
    pub window_height: u32, // 0002
    /// 0 x 0 is the window's size
    pub menu_width: u32, // 0037
    pub menu_height: u32, // 0038
    pub background_width: u32, // 0003
    pub background_height: u32, // 0004
    /// The 3D world's resolution (the game draws it at the background resolution, then shrinks it
    /// into the window's): "performance" the window's, "balanced" 1.5 times it, "quality" twice it,
    /// "maximum" 4096 x 4096, "custom" background_width x background_height.
    pub render_quality: String,
    /// The size the player last gave the game's window (points; host64 reports it, --window-size
    /// opens it so next time); 0 x 0: the game's own.
    pub window_points: (u32, u32),
    /// Volume in percent: in the world, and on the title and login screens before it (which the
    /// game's own volume setting does not reach)
    pub volume: u32,
    pub title_volume: u32,
    /// 0-6 (0000)
    pub mip_mapping: u32,
    /// 0 high, 1 low, 2 uncompressed (0018)
    pub texture_compression: u32,
    /// 0 compressed, 1 uncompressed (0019)
    pub map_compression: u32,
    /// 0 compressed, 1 uncompressed, 2 high quality (0036)
    pub font: u32,
    /// 0 off, 1 normal, 2 smooth (0011)
    pub environment_animation: u32,
    pub bump_mapping: bool, // 0017
    pub graphics_stabilization: bool, // 0040
    pub hardware_mouse: bool, // 0021
    pub opening_movie: bool, // 0022
    pub simplified_ccs: bool, // 0023
    pub sound: bool, // 0007
    pub sound_in_background: bool, // 0035
    /// 12-20 (0029)
    pub max_sounds: u32,
    /// host64 --fps-divisor: 1 is 60 fps, 2 is 30 fps as shipped
    pub fps_divisor: u32,
    /// FFXI_PROBE=gpu: answer the game's occlusion probe from the GPU (the sun's lens flare hides
    /// behind walls) at a cost of a few ms a frame; off, it always reads fully visible
    pub gpu_probe: bool,
    /// host64 --ui-aspect: the interface's shape, centered in a wider window ("16:9"), or "off"
    /// to stretch it across the window as the game does
    pub ui_aspect: String,
    /// The 3D scene's shape: "auto" follows the window, "off" as the game draws it, or "16:9"
    pub aspect: String,
    /// Names over characters' heads: "fix" keeps their 4:3 shape in a wide window, "off" as drawn
    pub nameplates: String,
    /// Their size: "1", "1.25", or across x down ("1x1.2")
    pub nameplate_scale: String,
    /// The frame-rate counter in the corner
    pub fps_overlay: bool,
    /// FFXI_TEXSAVE=<data dir>/texsave: each texture the game loads that no texture pack replaces is
    /// saved there once (tools/texdump_png.py reads its index), to make packs from what is played
    pub save_textures: bool,
    /// The key that brings up the launcher's settings over the game: f9..f12, pause, scrolllock, none
    pub hotkey: String,
    /// The scene effects (macOS, gfx_metal.m): key -> value, as its settings file has them. Keys
    /// left out keep the engine's defaults; fx = 0 is the game as it shipped.
    pub fx: BTreeMap<String, f32>,
}

impl Default for GameSettings {
    fn default() -> Self {
        GameSettings {
            window_mode: 1,
            window_width: 1920,
            window_height: 1080,
            menu_width: 960,
            menu_height: 540,
            background_width: 4096,
            background_height: 4096,
            render_quality: "quality".into(),
            window_points: (0, 0),
            volume: 100,
            title_volume: 35,
            mip_mapping: 6,
            texture_compression: 1,
            map_compression: 1,
            font: 0,
            environment_animation: 1,
            bump_mapping: true,
            graphics_stabilization: false,
            hardware_mouse: false,
            opening_movie: true,
            simplified_ccs: false,
            sound: true,
            sound_in_background: true,
            max_sounds: 12,
            fps_divisor: 1,
            gpu_probe: false,
            ui_aspect: "off".into(),
            aspect: "auto".into(),
            nameplates: "fix".into(),
            nameplate_scale: "1".into(),
            fps_overlay: true,
            save_textures: false,
            hotkey: "f12".into(),
            fx: BTreeMap::from([("fx".to_string(), 0.0)]),
        }
    }
}

/// The scene effects' keys and the engine's defaults (gfx_metal.m, FX_SETTINGS). The engine only
/// changes the keys its file names, so the launcher writes every one.
pub const FX_DEFAULTS: &[(&str, f32)] = &[
    ("fx", 0.0), ("ao", 0.8), ("radius", 1.0), ("grade", 1.0), ("sat", 1.12), ("contrast", 0.2),
    ("sharpen", 0.3), ("upscale", 1.0), ("filter", 1.0), ("aniso", 16.0), ("fog", 0.004), ("fog_falloff", 0.08),
    ("fog_height", 2.0), ("fog_max", 0.5), ("fog_sun", 0.5), ("fog_g", 0.6), ("bloom", 0.3),
    ("threshold", 0.75), ("rays", 0.6), ("rays_decay", 0.965), ("rays_length", 0.85), ("light", 1.0),
    ("shadow", 0.3), ("shadow_length", 0.6), ("sun", 0.5), ("sun_distance", 40.0), ("sun_soft", 0.03),
    ("sun_face", 0.0), ("sun_min", 1.0), ("sun_direct", 0.3), ("sun_casters", 1.0), ("sun_near", 15.0),
    ("temporal", 0.85),
];

impl GameSettings {
    /// The settings that hold while the game runs, as host64 --live reads them (and the scene
    /// effects, through FFXI_FX_FILE): rewritten on every change, picked up within half a second.
    pub fn to_live(&self) -> String {
        let mut s = String::from("# written by the launcher; changes apply while the game runs\n");
        s += &format!("fps_divisor = {}\n", self.fps_divisor.clamp(1, 4));
        s += &format!("aspect = {}\n", if self.aspect.is_empty() { "auto" } else { &self.aspect });
        s += &format!("ui_aspect = {}\n", if self.ui_aspect.is_empty() { "off" } else { &self.ui_aspect });
        s += &format!("nameplates = {}\n", if self.nameplates == "off" { "off" } else { "fix" });
        s += &format!("nameplate_scale = {}\n", if self.nameplate_scale.is_empty() { "1" } else { &self.nameplate_scale });
        s += &format!("fps_overlay = {}\n", self.fps_overlay as u32);
        s += &format!("window_mode = {}\n", self.window_mode.min(3));
        s += &format!("volume = {}\n", self.volume.min(100));
        s += &format!("title_volume = {}\n", self.title_volume.min(100));
        s += &format!("hotkey = {}\n", if self.hotkey.is_empty() { "f12" } else { &self.hotkey });
        for (k, def) in FX_DEFAULTS {
            s += &format!("{k} = {}\n", self.fx.get(*k).copied().unwrap_or(*def));
        }
        s
    }

    /// The background (3D) resolution render_quality asks for. Drawing the world at 4096 x 4096
    /// and shrinking it into a 1080p window costs eight times the pixels of the window for little
    /// the eye sees; twice the window's is sharp and half that work.
    pub fn background(&self) -> (u32, u32) {
        let (w, h) = (self.window_width.max(640), self.window_height.max(480));
        let times = |k: f32| (((w as f32 * k) as u32).min(4096), ((h as f32 * k) as u32).min(4096));
        match self.render_quality.as_str() {
            "performance" => times(1.0),
            "balanced" => times(1.5),
            "maximum" => (4096, 4096),
            "custom" => (self.background_width, self.background_height),
            _ => times(2.0),
        }
    }

    /// The settings as a REGEDIT4 file for host64 --reg-final (loaded after the game's own saves).
    pub fn to_reg(&self) -> String {
        let b = |v: bool| v as u32;
        let values: [(&str, u32); 20] = [
            ("0000", self.mip_mapping.min(6)),
            ("0001", self.window_width),
            ("0002", self.window_height),
            ("0003", self.background().0),
            ("0004", self.background().1),
            ("0007", b(self.sound)),
            ("0011", self.environment_animation.min(2)),
            ("0017", b(self.bump_mapping)),
            ("0018", self.texture_compression.min(2)),
            ("0019", self.map_compression.min(1)),
            ("0021", b(self.hardware_mouse)),
            ("0022", b(self.opening_movie)),
            ("0023", b(self.simplified_ccs)),
            ("0029", self.max_sounds.clamp(12, 20)),
            ("0034", self.window_mode.min(3)),
            ("0035", b(self.sound_in_background)),
            ("0036", self.font.min(2)),
            ("0037", self.menu_width),
            ("0038", self.menu_height),
            ("0040", b(self.graphics_stabilization)),
        ];
        let mut s = String::from("REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\r\n");
        for (name, v) in values {
            s += &format!("\"{name}\"=dword:{v:08x}\r\n");
        }
        s
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(default)]
pub struct LauncherConfig {
    /// The FINAL FANTASY XI folder (PlayOnlineViewer beside it).
    pub game_path: String,
    /// Lsb: the xiloader protocol its login server speaks ("2.2.0"), as it last said (versions.rs);
    /// empty: the host finds out when signing in.
    pub loader: String,
    /// host64; empty: the one bundled with the launcher.
    pub host_program: String,
    /// The base registry export; empty: the bundled playonline.reg.
    pub base_registry: String,
    /// Where the game's versions are kept (xi-vault); empty: the app's data folder. On the same
    /// drive as the install, backing it up takes no extra space (APFS clones).
    pub vault_dir: String,
    /// DAT overlay folders (host64 --dats), the first wins.
    pub dats: Vec<String>,
    /// The profiles, each with its own settings (profiles/<id>/profile.json on disk).
    pub accounts: Vec<Account>,
    pub last_account: String,
}

/// One profile in launcher.json's list: which folder, and its name for people reading the file.
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(default)]
struct ProfileEntry {
    id: String,
    name: String,
}

/// launcher.json as it is on disk. `accounts` and `game` are how it was before profiles had folders
/// of their own (read once, to move them).
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(default)]
struct MasterFile {
    game_path: String,
    loader: String,
    host_program: String,
    base_registry: String,
    vault_dir: String,
    dats: Vec<String>,
    profiles: Vec<ProfileEntry>,
    last_account: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    accounts: Vec<Account>,
    #[serde(skip_serializing_if = "Option::is_none")]
    game: Option<GameSettings>,
}

pub fn path(config_dir: &Path) -> PathBuf {
    config_dir.join("launcher.json")
}

/// A profile's folder name: its id, kept to letters, digits, - and _.
fn folder_name(id: &str) -> String {
    let s: String = id.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').collect();
    if s.is_empty() { "profile".into() } else { s }
}

/// Where a profile keeps its settings and the game's files for it (host64 --data-dir).
pub fn profile_dir(config_dir: &Path, id: &str) -> PathBuf {
    config_dir.join("profiles").join(folder_name(id))
}

/// The name a profile's log goes by: its name (or account name), as a file name can have it.
pub fn log_name(account: &Account) -> String {
    let raw = if account.name.trim().is_empty() { &account.login } else { &account.name };
    let s: String = raw.chars().map(|c| if c.is_alphanumeric() || " -_.".contains(c) { c } else { '_' }).collect();
    let s = s.trim().trim_matches('.').to_string();
    if s.is_empty() { folder_name(&account.id) } else { s }
}

/// Writes a file through a temporary name of this process's own (two launchers, or a launcher and
/// a game, never share one), then puts it in place.
fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let tmp = path.with_extension(format!("tmp{}", std::process::id()));
    fs::write(&tmp, text).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path).map_err(|e| e.to_string())
}

pub fn load_profile(config_dir: &Path, id: &str) -> Option<Account> {
    let text = fs::read_to_string(profile_dir(config_dir, id).join("profile.json")).ok()?;
    let mut a: Account = serde_json::from_str(&text).ok()?;
    a.id = id.to_string();
    Some(a)
}

pub fn save_profile(config_dir: &Path, account: &Account) -> Result<(), String> {
    let text = serde_json::to_string_pretty(account).map_err(|e| e.to_string())?;
    write_atomic(&profile_dir(config_dir, &account.id).join("profile.json"), &text)
}

/// The game's own files that were shared by every account before profiles had folders: each
/// profile starts with a copy, so nothing set before is lost.
const SHARED_BEFORE: &[&str] = &["saved.reg", "signin.cfg", "overlay.ini", "background.png", "background.jpg"];

fn copy_shared_into(config_dir: &Path, dest: &Path) {
    let _ = fs::create_dir_all(dest);
    for name in SHARED_BEFORE {
        let from = config_dir.join(name);
        if from.is_file() && !dest.join(name).exists() {
            let _ = fs::copy(&from, dest.join(name));
        }
    }
}

pub fn load(config_dir: &Path) -> LauncherConfig {
    let master: MasterFile = fs::read_to_string(path(config_dir))
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default();
    let mut cfg = LauncherConfig {
        game_path: master.game_path.clone(),
        loader: master.loader.clone(),
        host_program: master.host_program.clone(),
        base_registry: master.base_registry.clone(),
        vault_dir: master.vault_dir.clone(),
        dats: master.dats.clone(),
        accounts: Vec::new(),
        last_account: master.last_account.clone(),
    };
    if !master.accounts.is_empty() && master.profiles.is_empty() {
        // launcher.json from before: every account takes the one set of game settings there was,
        // and a copy of the game's files; the old file is kept beside it
        let game = master.game.clone().unwrap_or_default();
        for mut a in master.accounts {
            a.game = game.clone();
            copy_shared_into(config_dir, &profile_dir(config_dir, &a.id));
            cfg.accounts.push(a);
        }
        let _ = fs::copy(path(config_dir), config_dir.join("launcher.before-profiles.json"));
        let _ = save(config_dir, &cfg);
        return cfg;
    }
    for p in master.profiles {
        let a = load_profile(config_dir, &p.id).unwrap_or_else(|| Account { id: p.id.clone(), name: p.name.clone(), ..Account::default() });
        cfg.accounts.push(a);
    }
    cfg
}

/// Saves launcher.json and every profile. Profiles no longer listed keep their folders: only the
/// window's Delete removes one (remove_profile).
pub fn save(config_dir: &Path, cfg: &LauncherConfig) -> Result<(), String> {
    for a in &cfg.accounts {
        save_profile(config_dir, a)?;
    }
    let master = MasterFile {
        game_path: cfg.game_path.clone(),
        loader: cfg.loader.clone(),
        host_program: cfg.host_program.clone(),
        base_registry: cfg.base_registry.clone(),
        vault_dir: cfg.vault_dir.clone(),
        dats: cfg.dats.clone(),
        profiles: cfg.accounts.iter().map(|a| ProfileEntry { id: a.id.clone(), name: log_name(a) }).collect(),
        last_account: cfg.last_account.clone(),
        accounts: Vec::new(),
        game: None,
    };
    let text = serde_json::to_string_pretty(&master).map_err(|e| e.to_string())?;
    write_atomic(&path(config_dir), &text)
}

/// A profile the player deleted: its folder (its settings and the game's files for it) goes too.
pub fn remove_profile(config_dir: &Path, id: &str) {
    let dir = profile_dir(config_dir, id);
    if dir.starts_with(config_dir.join("profiles")) && dir.is_dir() {
        let _ = fs::remove_dir_all(dir);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("ffxi-launcher-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn launcher_json_from_before_becomes_profiles() {
        let dir = temp("migrate");
        let mut game = GameSettings::default();
        game.window_width = 1280;
        let old = serde_json::json!({
            "game_path": "/games/FINAL FANTASY XI",
            "accounts": [
                { "id": "acct-a", "name": "Tagban", "login": "tagban", "server": "ffxi.cc" },
                { "id": "acct-b", "name": "Mule", "login": "mule", "server": "ffxi.cc" }
            ],
            "last_account": "acct-b",
            "game": game,
        });
        fs::write(path(&dir), old.to_string()).unwrap();
        fs::write(dir.join("saved.reg"), "REGEDIT4 saved").unwrap();
        fs::write(dir.join("overlay.ini"), "[Window]").unwrap();

        let cfg = load(&dir);
        assert_eq!(cfg.accounts.len(), 2);
        assert!(cfg.accounts.iter().all(|a| a.game.window_width == 1280));
        assert_eq!(cfg.last_account, "acct-b");
        for id in ["acct-a", "acct-b"] {
            let p = profile_dir(&dir, id);
            assert!(p.join("profile.json").is_file());
            assert_eq!(fs::read_to_string(p.join("saved.reg")).unwrap(), "REGEDIT4 saved");
            assert!(p.join("overlay.ini").is_file());
        }
        assert!(dir.join("launcher.before-profiles.json").is_file());
        // launcher.json is now the list
        let master: serde_json::Value = serde_json::from_str(&fs::read_to_string(path(&dir)).unwrap()).unwrap();
        assert!(master.get("accounts").is_none() && master.get("game").is_none());
        assert_eq!(master["profiles"][0]["name"], "Tagban");

        // each profile's settings are its own from now on
        let mut cfg = load(&dir);
        cfg.accounts[1].game.window_width = 800;
        save(&dir, &cfg).unwrap();
        let cfg = load(&dir);
        assert_eq!((cfg.accounts[0].game.window_width, cfg.accounts[1].game.window_width), (1280, 800));
        assert_eq!(cfg.game_path, "/games/FINAL FANTASY XI");

        // a profile left out of the list keeps its folder until it's removed on purpose
        let mut fewer = cfg.clone();
        fewer.accounts.truncate(1);
        save(&dir, &fewer).unwrap();
        assert!(profile_dir(&dir, "acct-b").is_dir());
        remove_profile(&dir, "acct-b");
        assert!(!profile_dir(&dir, "acct-b").exists());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn names_for_files() {
        let mut a = Account { id: "acct-x/../y".into(), name: "Tag: Ban!".into(), ..Account::default() };
        assert_eq!(log_name(&a), "Tag_ Ban_");
        a.name = "  ".into();
        a.login = "".into();
        assert_eq!(log_name(&a), "acct-xy");
        assert!(profile_dir(Path::new("/c"), "../../etc").starts_with("/c/profiles"));
    }
}
