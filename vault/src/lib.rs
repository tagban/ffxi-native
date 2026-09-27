//! Client versions of FINAL FANTASY XI, kept whole.
//!
//! FFXiMain.dll decides how the DATs are read, so a version is the DLLs and the DATs together: a
//! snapshot records every file of an install (path, size, SHA-256) as one manifest, and the files
//! themselves go in a content-addressed store, once each however many versions share them. From
//! there a version can be diffed against another, packed as a delta (only what changed),
//! published as plain static files for a server to hand out, fetched by the launcher, checked
//! against an install, and put back together as an install of its own.
//!
//! The vault:
//!   objects/ab/<sha256>        each file's bytes, by hash
//!   versions/<version>.json    a manifest (Manifest)
//!
//! A published site (publish; any static file server, or `xi-vault serve`):
//!   index.json                 Index: the versions, the one the server wants, the packs
//!   versions/<version>.json    their manifests
//!   objects/ab/<sha256>.zst    each file, zstd-compressed
//!   packs/<from>..<to>.tar.zst deltas, one download each (optional)
//!
//! This holds Square Enix's files. It never ships with the launcher: a vault is made from the
//! player's own install, and a site is whatever its operator chooses to host.

use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const FORMAT: &str = "xi-vault/1";

/// Folders of an install that are the player's, not the version's.
const SKIP_DIRS: &[&str] = &["USER", "TEMP"];
const SKIP_FILES: &[&str] = &[".DS_Store", "Thumbs.db", "desktop.ini"];

/// The builds the recompiler knows (meta/builds.json): FFXiMain.dll's hash -> build label, version.
const BUILDS_JSON: &str = include_str!("../../meta/builds.json");

pub type Result<T> = std::result::Result<T, String>;

fn err<E: std::fmt::Display>(what: impl std::fmt::Display) -> impl FnOnce(E) -> String {
    move |e| format!("{what}: {e}")
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    /// relative to the FINAL FANTASY XI folder, with '/'
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Manifest {
    pub format: String,
    /// The client version the lobby sees ("30260805_0"), or a name given to it.
    pub version: String,
    /// The recompiler's build label (meta/builds.json), empty when it does not know this build.
    pub build: String,
    pub ffximain_sha256: String,
    pub ffxi_sha256: String,
    /// seconds since 1970
    pub created: u64,
    /// sorted by path
    pub files: Vec<Entry>,
}

impl Manifest {
    pub fn bytes(&self) -> u64 {
        self.files.iter().map(|e| e.size).sum()
    }
    pub fn by_path(&self) -> BTreeMap<&str, &Entry> {
        self.files.iter().map(|e| (e.path.as_str(), e)).collect()
    }
}

/// What a server publishes (index.json).
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct Index {
    pub format: String,
    /// The version the server's lobby wants.
    pub current: String,
    pub versions: Vec<IndexVersion>,
    pub packs: Vec<IndexPack>,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct IndexVersion {
    pub version: String,
    pub build: String,
    pub files: usize,
    pub bytes: u64,
    /// Published with --since: only the files this version does not share with `base` are
    /// hosted; a player brings the rest from an install of `base` (their own). Empty: all hosted.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub base: String,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct IndexPack {
    pub from: String,
    pub to: String,
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

/// Progress: (what, done, total). Bytes where there are bytes, else files.
pub type Progress<'a> = &'a (dyn Fn(&str, u64, u64) + Sync);

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn sha256_file(path: &Path) -> Result<String> {
    let mut f = BufReader::with_capacity(1 << 20, File::open(path).map_err(err(path.display()))?);
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf).map_err(err(path.display()))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(hex(&h.finalize()))
}

/// The build and version for FFXiMain.dll's hash, from meta/builds.json.
pub fn known_build(ffximain_sha: &str) -> Option<(String, String)> {
    let v: serde_json::Value = serde_json::from_str(BUILDS_JSON).ok()?;
    for (label, b) in v["builds"].as_object()? {
        if b["FFXiMain.dll"]["sha256"].as_str() == Some(ffximain_sha) {
            return Some((label.clone(), b["version"].as_str().unwrap_or_default().to_string()));
        }
    }
    None
}

// --- the install ---------------------------------------------------------------------------------

/// Every file of the install that belongs to the version, relative, sorted.
pub fn install_files(game: &Path) -> Result<Vec<(String, u64)>> {
    fn walk(root: &Path, rel: &str, out: &mut Vec<(String, u64)>) -> Result<()> {
        let dir = if rel.is_empty() { root.to_path_buf() } else { root.join(rel) };
        for e in fs::read_dir(&dir).map_err(err(dir.display()))? {
            let e = e.map_err(err(dir.display()))?;
            let name = e.file_name().to_string_lossy().into_owned();
            let path = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
            let ft = e.file_type().map_err(err(&path))?;
            if ft.is_dir() {
                if !(rel.is_empty() && SKIP_DIRS.iter().any(|d| d.eq_ignore_ascii_case(&name))) {
                    walk(root, &path, out)?;
                }
            } else if ft.is_file() && !SKIP_FILES.contains(&name.as_str()) {
                out.push((path, e.metadata().map_err(err(&name))?.len()));
            }
        }
        Ok(())
    }
    if !game.join("FFXiMain.dll").is_file() {
        return Err(format!("{}: no FFXiMain.dll; this is not a FINAL FANTASY XI folder", game.display()));
    }
    let mut out = Vec::new();
    walk(game, "", &mut out)?;
    out.sort();
    Ok(out)
}

// --- the vault -----------------------------------------------------------------------------------

pub struct Vault {
    pub root: PathBuf,
}

impl Vault {
    pub fn open(root: impl Into<PathBuf>) -> Result<Vault> {
        let root = root.into();
        fs::create_dir_all(root.join("objects")).map_err(err(root.display()))?;
        fs::create_dir_all(root.join("versions")).map_err(err(root.display()))?;
        Ok(Vault { root })
    }

    pub fn object(&self, sha: &str) -> PathBuf {
        self.root.join("objects").join(&sha[..2]).join(sha)
    }

    pub fn has(&self, e: &Entry) -> bool {
        fs::metadata(self.object(&e.sha256)).map(|m| m.len() == e.size).unwrap_or(false)
    }

    /// Puts a file in the store under its hash (a clone on APFS, so free on the same volume).
    fn put_file(&self, src: &Path, sha: &str) -> Result<()> {
        let dst = self.object(sha);
        if dst.exists() {
            return Ok(());
        }
        fs::create_dir_all(dst.parent().unwrap()).map_err(err(dst.display()))?;
        let tmp = dst.with_extension(format!("tmp{}", std::process::id()));
        fs::copy(src, &tmp).map_err(err(src.display()))?;
        fs::rename(&tmp, &dst).map_err(err(dst.display()))
    }

    /// Puts bytes from a reader in the store, checking they are what the hash says.
    pub fn put_reader(&self, mut r: impl Read, sha: &str) -> Result<u64> {
        let dst = self.object(sha);
        fs::create_dir_all(dst.parent().unwrap()).map_err(err(dst.display()))?;
        let tmp = dst.with_extension(format!("tmp{}-{:?}", std::process::id(), std::thread::current().id()));
        let mut f = File::create(&tmp).map_err(err(tmp.display()))?;
        let mut h = Sha256::new();
        let mut buf = vec![0u8; 1 << 20];
        let mut n = 0u64;
        loop {
            let k = r.read(&mut buf).map_err(err(sha))?;
            if k == 0 {
                break;
            }
            h.update(&buf[..k]);
            f.write_all(&buf[..k]).map_err(err(tmp.display()))?;
            n += k as u64;
        }
        drop(f);
        let got = hex(&h.finalize());
        if got != sha {
            let _ = fs::remove_file(&tmp);
            return Err(format!("object {sha}: the bytes hash to {got}"));
        }
        fs::rename(&tmp, &dst).map_err(err(dst.display()))?;
        Ok(n)
    }

    pub fn manifest_path(&self, version: &str) -> PathBuf {
        self.root.join("versions").join(format!("{version}.json"))
    }

    pub fn load(&self, version: &str) -> Result<Manifest> {
        let p = self.manifest_path(version);
        let text = fs::read_to_string(&p).map_err(|e| format!("version {version} is not in the vault ({}: {e})", p.display()))?;
        serde_json::from_str(&text).map_err(err(p.display()))
    }

    pub fn save(&self, m: &Manifest) -> Result<()> {
        let p = self.manifest_path(&m.version);
        let tmp = p.with_extension("json.tmp");
        fs::write(&tmp, serde_json::to_string_pretty(m).unwrap()).map_err(err(tmp.display()))?;
        fs::rename(&tmp, &p).map_err(err(p.display()))
    }

    pub fn versions(&self) -> Result<Vec<Manifest>> {
        let mut out = Vec::new();
        for e in fs::read_dir(self.root.join("versions")).map_err(err("versions"))?.flatten() {
            let p = e.path();
            if p.extension().is_some_and(|x| x == "json") {
                if let Ok(m) = serde_json::from_str::<Manifest>(&fs::read_to_string(&p).unwrap_or_default()) {
                    out.push(m);
                }
            }
        }
        out.sort_by(|a, b| a.version.cmp(&b.version));
        Ok(out)
    }

    /// Records the install as a version: every file hashed and stored, the manifest saved.
    /// `name` overrides the version name (else the version meta/builds.json gives its FFXiMain.dll).
    pub fn snapshot(&self, game: &Path, name: Option<&str>, progress: Progress) -> Result<Manifest> {
        let list = install_files(game)?;
        let total: u64 = list.iter().map(|(_, s)| s).sum();
        let done = AtomicU64::new(0);
        let files = list
            .par_iter()
            .map(|(rel, size)| {
                let src = game.join(rel);
                let sha = sha256_file(&src)?;
                self.put_file(&src, &sha)?;
                let d = done.fetch_add(*size, Ordering::Relaxed) + size;
                progress("snapshot", d, total);
                Ok(Entry { path: rel.clone(), size: *size, sha256: sha })
            })
            .collect::<Result<Vec<Entry>>>()?;
        let sha_of = |p: &str| files.iter().find(|e| e.path.eq_ignore_ascii_case(p)).map(|e| e.sha256.clone()).unwrap_or_default();
        let (main, ffxi) = (sha_of("FFXiMain.dll"), sha_of("FFXi.dll"));
        let (build, version) = known_build(&main).unwrap_or_default();
        let version = match name {
            Some(n) => n.to_string(),
            None if !version.is_empty() => version,
            None => format!("unknown-{}", &main[..12]),
        };
        let m = Manifest {
            format: FORMAT.into(),
            version,
            build,
            ffximain_sha256: main,
            ffxi_sha256: ffxi,
            created: std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
            files,
        };
        self.save(&m)?;
        Ok(m)
    }

    /// Puts a version back together as an install of its own (clones on APFS: no extra space).
    pub fn materialize(&self, version: &str, out: &Path, progress: Progress) -> Result<Manifest> {
        let m = self.load(version)?;
        let missing: Vec<&Entry> = m.files.iter().filter(|e| !self.has(e)).collect();
        if !missing.is_empty() {
            return Err(format!("{} files of {version} are not in the vault (the first: {}); fetch them first", missing.len(), missing[0].path));
        }
        let total = m.bytes();
        let done = AtomicU64::new(0);
        m.files.par_iter().try_for_each(|e| -> Result<()> {
            let dst = out.join(&e.path);
            if fs::metadata(&dst).map(|md| md.len() == e.size).unwrap_or(false) && sha256_file(&dst).ok().as_deref() == Some(&e.sha256) {
                // already right
            } else {
                fs::create_dir_all(dst.parent().unwrap()).map_err(err(dst.display()))?;
                let _ = fs::remove_file(&dst);
                fs::copy(self.object(&e.sha256), &dst).map_err(err(dst.display()))?;
            }
            progress("materialize", done.fetch_add(e.size, Ordering::Relaxed) + e.size, total);
            Ok(())
        })?;
        Ok(m)
    }
}

// --- comparing -----------------------------------------------------------------------------------

#[derive(Serialize, Debug, Default)]
pub struct Diff {
    pub from: String,
    pub to: String,
    pub added: Vec<Entry>,
    pub changed: Vec<Entry>,
    pub removed: Vec<Entry>,
    /// what `to` needs that `from` does not have, by content (files that only moved need nothing)
    pub new_objects: Vec<Entry>,
}

impl Diff {
    pub fn new_bytes(&self) -> u64 {
        self.new_objects.iter().map(|e| e.size).sum()
    }
}

pub fn diff(a: &Manifest, b: &Manifest) -> Diff {
    let (pa, pb) = (a.by_path(), b.by_path());
    let mut d = Diff { from: a.version.clone(), to: b.version.clone(), ..Default::default() };
    for (p, e) in &pb {
        match pa.get(p) {
            None => d.added.push((*e).clone()),
            Some(old) if old.sha256 != e.sha256 => d.changed.push((*e).clone()),
            _ => {}
        }
    }
    for (p, e) in &pa {
        if !pb.contains_key(p) {
            d.removed.push((*e).clone());
        }
    }
    let have: BTreeSet<&str> = a.files.iter().map(|e| e.sha256.as_str()).collect();
    let mut seen = BTreeSet::new();
    for e in d.added.iter().chain(&d.changed) {
        if !have.contains(e.sha256.as_str()) && seen.insert(e.sha256.clone()) {
            d.new_objects.push(e.clone());
        }
    }
    d
}

/// An install checked against a version: what is missing or not what the manifest says.
#[derive(Serialize, Debug, Default)]
pub struct Check {
    pub missing: Vec<Entry>,
    pub wrong: Vec<Entry>,
    /// files the install has that the version does not
    pub extra: Vec<String>,
}

/// `full`: hash every file; else only sizes (fast, catches missing and truncated files).
pub fn verify(game: &Path, m: &Manifest, full: bool, progress: Progress) -> Result<Check> {
    let total = m.bytes();
    let done = AtomicU64::new(0);
    let results: Vec<(u8, Entry)> = m
        .files
        .par_iter()
        .filter_map(|e| {
            let p = game.join(&e.path);
            let r = match fs::metadata(&p) {
                Err(_) => Some((0u8, e.clone())),
                Ok(md) if md.len() != e.size => Some((1, e.clone())),
                Ok(_) if full && sha256_file(&p).ok().as_deref() != Some(e.sha256.as_str()) => Some((1, e.clone())),
                _ => None,
            };
            progress("verify", done.fetch_add(e.size, Ordering::Relaxed) + e.size, total);
            r
        })
        .collect();
    let mut c = Check::default();
    for (kind, e) in results {
        if kind == 0 { c.missing.push(e) } else { c.wrong.push(e) }
    }
    c.missing.sort_by(|a, b| a.path.cmp(&b.path));
    c.wrong.sort_by(|a, b| a.path.cmp(&b.path));
    let known: BTreeSet<&str> = m.files.iter().map(|e| e.path.as_str()).collect();
    c.extra = install_files(game)?.into_iter().map(|(p, _)| p).filter(|p| !known.contains(p.as_str())).collect();
    Ok(c)
}

/// Puts the missing and wrong files of a check back from the vault.
pub fn repair(game: &Path, vault: &Vault, c: &Check) -> Result<usize> {
    let todo: Vec<&Entry> = c.missing.iter().chain(&c.wrong).collect();
    for e in &todo {
        if !vault.has(e) {
            return Err(format!("{} is not in the vault; fetch the version first", e.path));
        }
    }
    todo.par_iter().try_for_each(|e| -> Result<()> {
        let dst = game.join(&e.path);
        fs::create_dir_all(dst.parent().unwrap()).map_err(err(dst.display()))?;
        let tmp = dst.with_extension("xi-vault-tmp");
        fs::copy(vault.object(&e.sha256), &tmp).map_err(err(dst.display()))?;
        fs::rename(&tmp, &dst).map_err(err(dst.display()))
    })?;
    Ok(todo.len())
}

// --- delta packs ---------------------------------------------------------------------------------

/// The header of a pack (xipack.json, its first entry).
#[derive(Serialize, Deserialize, Debug)]
pub struct PackHeader {
    pub format: String,
    pub from: String,
    pub to: String,
    /// the whole manifest of `to`: with the objects of `from`, all it needs
    pub manifest: Manifest,
    pub objects: Vec<Entry>,
}

/// One file: the objects `to` needs beyond `from`, and `to`'s manifest, as a zstd-compressed tar.
/// Returns (raw bytes in it, bytes written).
pub fn pack(vault: &Vault, from: &str, to: &str, out: &Path, level: i32, progress: Progress) -> Result<(u64, u64)> {
    let (a, b) = (vault.load(from)?, vault.load(to)?);
    let d = diff(&a, &b);
    let header = PackHeader { format: FORMAT.into(), from: from.into(), to: to.into(), manifest: b, objects: d.new_objects.clone() };
    let tmp = out.with_extension("tmp");
    let f = File::create(&tmp).map_err(err(tmp.display()))?;
    let mut z = zstd::Encoder::new(f, level).map_err(err("zstd"))?;
    z.multithread(std::thread::available_parallelism().map(|n| n.get() as u32).unwrap_or(4)).map_err(err("zstd"))?;
    z.long_distance_matching(true).map_err(err("zstd"))?;
    let mut t = tar::Builder::new(z);
    let hj = serde_json::to_vec_pretty(&header).unwrap();
    let mut hd = tar::Header::new_gnu();
    hd.set_size(hj.len() as u64);
    hd.set_mode(0o644);
    hd.set_cksum();
    t.append_data(&mut hd, "xipack.json", hj.as_slice()).map_err(err("pack"))?;
    let total = d.new_bytes();
    let mut done = 0;
    for e in &d.new_objects {
        let mut f = File::open(vault.object(&e.sha256)).map_err(err(&e.path))?;
        let mut hd = tar::Header::new_gnu();
        hd.set_size(e.size);
        hd.set_mode(0o644);
        hd.set_cksum();
        t.append_data(&mut hd, format!("objects/{}", e.sha256), &mut f).map_err(err(&e.path))?;
        done += e.size;
        progress("pack", done, total);
    }
    let z = t.into_inner().map_err(err("pack"))?;
    z.finish().map_err(err("zstd"))?;
    fs::rename(&tmp, out).map_err(err(out.display()))?;
    Ok((total, fs::metadata(out).map(|m| m.len()).unwrap_or(0)))
}

/// Takes a pack into the vault: its objects, and the version it brings. Needs `from` already here
/// only when the version is to be put together.
pub fn unpack(vault: &Vault, r: impl Read, progress: Progress) -> Result<Manifest> {
    let z = zstd::Decoder::new(r).map_err(err("zstd"))?;
    let mut t = tar::Archive::new(z);
    let mut header: Option<PackHeader> = None;
    let mut done = 0;
    for entry in t.entries().map_err(err("pack"))? {
        let mut entry = entry.map_err(err("pack"))?;
        let path = entry.path().map_err(err("pack"))?.to_string_lossy().into_owned();
        if path == "xipack.json" {
            let mut s = String::new();
            entry.read_to_string(&mut s).map_err(err("pack"))?;
            header = Some(serde_json::from_str(&s).map_err(err("xipack.json"))?);
        } else if let Some(sha) = path.strip_prefix("objects/") {
            done += vault.put_reader(&mut entry, sha)?;
            if let Some(h) = &header {
                progress("unpack", done, h.objects.iter().map(|e| e.size).sum());
            }
        }
    }
    let h = header.ok_or("not a pack: no xipack.json")?;
    vault.save(&h.manifest)?;
    Ok(h.manifest)
}

// --- publishing and fetching ---------------------------------------------------------------------

/// Writes a static site for these versions: index.json, their manifests, every object they use
/// (zstd), and with `packs`, a delta pack from each version to the next. `current` is the version
/// the server wants. With `since`, objects that version already has are left out: the site holds
/// only the difference, and players bring the rest from their own install of it.
pub fn publish(
    vault: &Vault,
    versions: &[String],
    current: &str,
    out: &Path,
    packs: bool,
    since: Option<&str>,
    progress: Progress,
) -> Result<Index> {
    let ms: Vec<Manifest> = versions.iter().map(|v| vault.load(v)).collect::<Result<_>>()?;
    fs::create_dir_all(out.join("versions")).map_err(err(out.display()))?;
    let base = since.map(|b| vault.load(b)).transpose()?;
    let skip: BTreeSet<&str> = base.iter().flat_map(|b| b.files.iter().map(|e| e.sha256.as_str())).collect();
    if let Some(b) = &base {
        // its manifest, so a player can tell whether their install is it
        fs::write(out.join("versions").join(format!("{}.json", b.version)), serde_json::to_string_pretty(b).unwrap())
            .map_err(err("manifest"))?;
    }
    let mut objects: BTreeMap<&str, &Entry> = BTreeMap::new();
    for m in &ms {
        for e in m.files.iter().filter(|e| !skip.contains(e.sha256.as_str())) {
            objects.entry(&e.sha256).or_insert(e);
        }
        fs::write(out.join("versions").join(format!("{}.json", m.version)), serde_json::to_string_pretty(m).unwrap())
            .map_err(err("manifest"))?;
    }
    let total: u64 = objects.values().map(|e| e.size).sum();
    let done = AtomicU64::new(0);
    objects.par_iter().try_for_each(|(sha, e)| -> Result<()> {
        let dst = out.join("objects").join(&sha[..2]).join(format!("{sha}.zst"));
        if !dst.exists() {
            fs::create_dir_all(dst.parent().unwrap()).map_err(err(dst.display()))?;
            let data = fs::read(vault.object(sha)).map_err(err(&e.path))?;
            let z = zstd::bulk::compress(&data, 9).map_err(err("zstd"))?;
            let tmp = dst.with_extension("tmp");
            fs::write(&tmp, z).map_err(err(tmp.display()))?;
            fs::rename(&tmp, &dst).map_err(err(dst.display()))?;
        }
        progress("publish", done.fetch_add(e.size, Ordering::Relaxed) + e.size, total);
        Ok(())
    })?;
    let mut index = Index { format: FORMAT.into(), current: current.into(), ..Default::default() };
    for m in &ms {
        index.versions.push(IndexVersion {
            version: m.version.clone(),
            build: m.build.clone(),
            files: m.files.len(),
            bytes: m.bytes(),
            base: match &base {
                Some(b) if b.version != m.version => b.version.clone(),
                _ => String::new(),
            },
        });
    }
    if packs {
        fs::create_dir_all(out.join("packs")).map_err(err("packs"))?;
        for w in ms.windows(2) {
            let name = format!("{}..{}.tar.zst", w[0].version, w[1].version);
            let p = out.join("packs").join(&name);
            pack(vault, &w[0].version, &w[1].version, &p, 19, progress)?;
            index.packs.push(IndexPack {
                from: w[0].version.clone(),
                to: w[1].version.clone(),
                path: format!("packs/{name}"),
                size: fs::metadata(&p).map(|m| m.len()).unwrap_or(0),
                sha256: sha256_file(&p)?,
            });
        }
    }
    fs::write(out.join("index.json"), serde_json::to_string_pretty(&index).unwrap()).map_err(err("index.json"))?;
    Ok(index)
}

fn url_join(base: &str, rel: &str) -> String {
    format!("{}/{}", base.trim_end_matches('/'), rel)
}

fn get_within(url: &str, timeout: std::time::Duration) -> Result<Box<dyn Read + Send + Sync>> {
    let r = ureq::get(url).timeout(timeout).call().map_err(err(url))?;
    Ok(Box::new(r.into_reader()))
}

fn get(url: &str) -> Result<Box<dyn Read + Send + Sync>> {
    get_within(url, std::time::Duration::from_secs(60))
}

pub fn fetch_index(base: &str) -> Result<Index> {
    fetch_index_within(base, std::time::Duration::from_secs(20))
}

/// The index, giving up after `timeout` (looking for a site that may not be there).
pub fn fetch_index_within(base: &str, timeout: std::time::Duration) -> Result<Index> {
    let mut s = String::new();
    get_within(&url_join(base, "index.json"), timeout)?.read_to_string(&mut s).map_err(err("index.json"))?;
    let index: Index = serde_json::from_str(&s).map_err(err("index.json"))?;
    if index.format != FORMAT {
        return Err(format!("{base}: not an xi-vault site ({})", index.format));
    }
    Ok(index)
}

/// The port a server's game versions are looked for on when it names no address.
pub const DEFAULT_PORT: u16 = 54080;

/// Brings a version from a published site into the vault: its manifest, then every object the
/// vault lacks (a pack instead when there is one from a version the vault has, and it is smaller).
pub fn fetch(vault: &Vault, base: &str, version: Option<&str>, progress: Progress) -> Result<Manifest> {
    let index = fetch_index(base)?;
    let version = version.unwrap_or(&index.current).to_string();
    if !index.versions.iter().any(|v| v.version == version) {
        return Err(format!("{base} does not have version {version}"));
    }
    let mut s = String::new();
    get(&url_join(base, &format!("versions/{version}.json")))?.read_to_string(&mut s).map_err(err("manifest"))?;
    let m: Manifest = serde_json::from_str(&s).map_err(err("manifest"))?;
    let mut need: BTreeMap<String, Entry> = BTreeMap::new();
    for e in &m.files {
        if !vault.has(e) {
            need.entry(e.sha256.clone()).or_insert_with(|| e.clone());
        }
    }
    // a difference-only site: the base version's files are the player's to bring
    if let Some(iv) = index.versions.iter().find(|v| v.version == version && !v.base.is_empty()) {
        let mut s = String::new();
        get(&url_join(base, &format!("versions/{}.json", iv.base)))?.read_to_string(&mut s).map_err(err("manifest"))?;
        let bm: Manifest = serde_json::from_str(&s).map_err(err("manifest"))?;
        let from_base: BTreeSet<&str> = bm.files.iter().map(|e| e.sha256.as_str()).collect();
        let lacking = need.keys().filter(|k| from_base.contains(k.as_str())).count();
        if lacking > 0 {
            return Err(format!(
                "The server publishes only what version {version} changes from version {}, and {lacking} files of {} are not \
                 here. Back up an install of version {} first (Launcher → Game files).",
                iv.base, iv.base, iv.base
            ));
        }
    }
    let need_bytes: u64 = need.values().map(|e| e.size).sum();
    if need.is_empty() {
        vault.save(&m)?;
        return Ok(m);
    }
    // a pack from a version this vault already has
    let have: BTreeSet<String> = vault.versions()?.into_iter().map(|m| m.version).collect();
    if let Some(p) = index.packs.iter().find(|p| p.to == version && have.contains(&p.from) && p.size < need_bytes) {
        unpack(vault, get(&url_join(base, &p.path))?, progress)?;
        if m.files.iter().all(|e| vault.has(e)) {
            vault.save(&m)?;
            return Ok(m);
        }
    }
    let done = AtomicU64::new(0);
    let pool = rayon::ThreadPoolBuilder::new().num_threads(8).build().map_err(err("threads"))?;
    pool.install(|| {
        need.par_iter().try_for_each(|(sha, e)| -> Result<()> {
            if vault.has(e) {
                return Ok(());
            }
            let url = url_join(base, &format!("objects/{}/{sha}.zst", &sha[..2]));
            let z = zstd::Decoder::new(get(&url)?).map_err(err(&url))?;
            vault.put_reader(z, sha)?;
            progress("fetch", done.fetch_add(e.size, Ordering::Relaxed) + e.size, need_bytes);
            Ok(())
        })
    })?;
    vault.save(&m)?;
    Ok(m)
}

/// Serves a published site over HTTP (GET and HEAD, no listings): a server operator's "one more
/// port". Blocks.
pub fn serve(site: &Path, addr: &str) -> Result<()> {
    let server = std::sync::Arc::new(tiny_http::Server::http(addr).map_err(err(addr))?);
    let site = site.canonicalize().map_err(err(site.display()))?;
    eprintln!("serving {} on http://{addr}/", site.display());
    // a player downloads 8 files at a time; so do many players
    let workers: Vec<_> = (0..32)
        .map(|_| {
            let (server, site) = (server.clone(), site.clone());
            std::thread::spawn(move || serve_requests(&server, &site))
        })
        .collect();
    for w in workers {
        let _ = w.join();
    }
    Ok(())
}

fn serve_requests(server: &tiny_http::Server, site: &Path) {
    for req in server.incoming_requests() {
        let url = req.url().split('?').next().unwrap_or("/").trim_start_matches('/').to_string();
        let ok = !url.is_empty() && !url.split('/').any(|c| c.is_empty() || c == "." || c == "..");
        let path = site.join(&url);
        let resp = match (ok, File::open(&path)) {
            (true, Ok(f)) if path.is_file() => {
                let len = f.metadata().map(|m| m.len()).ok();
                let ctype = if url.ends_with(".json") { "application/json" } else { "application/octet-stream" };
                let h = tiny_http::Header::from_bytes("Content-Type", ctype).unwrap();
                req.respond(tiny_http::Response::new(200.into(), vec![h], f, len.map(|l| l as usize), None))
            }
            _ => req.respond(tiny_http::Response::from_string("not found").with_status_code(404)),
        };
        if let Err(e) = resp {
            eprintln!("{url}: {e}");
        }
    }
}

/// Bytes as the player reads them.
pub fn human(n: u64) -> String {
    let n = n as f64;
    if n >= 1e9 { format!("{:.2} GB", n / 1e9) } else if n >= 1e6 { format!("{:.1} MB", n / 1e6) } else if n >= 1e3 { format!("{:.0} KB", n / 1e3) } else { format!("{n} B") }
}
