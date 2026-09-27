//! Which retail build the engine root is working on (tools/buildinfo.py, tools/build.py
//! write_build_h).
//!
//! meta/builds.json lists every build the recompiler knows. prepare matches the install's
//! FFXiMain.dll against it and records the choice in generated/build.json; everything after that
//! reads the choice from there, so nothing depends on the install again.
use crate::Error;
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};

pub fn sha256(path: &Path) -> Result<String, Error> {
    let d = Sha256::digest(crate::read(path)?);
    Ok(d.iter().map(|b| format!("{:02x}", b)).collect())
}

fn builds_path(root: &Path) -> PathBuf {
    root.join("meta").join("builds.json")
}

pub fn chosen_path(root: &Path) -> PathBuf {
    root.join("generated").join("build.json")
}

fn load_json(path: &Path) -> Result<Value, Error> {
    let text = std::fs::read_to_string(path).map_err(|e| Error::io(path, e))?;
    serde_json::from_str(&text).map_err(|e| Error::Meta(format!("{}: {}", path.display(), e)))
}

fn meta_err(what: &str) -> Error {
    Error::Meta(format!("meta/builds.json: {}", what))
}

/// meta/builds.json "builds", in file order.
pub fn known(root: &Path) -> Result<Map<String, Value>, Error> {
    match load_json(&builds_path(root))?.get("builds") {
        Some(Value::Object(m)) => Ok(m.clone()),
        _ => Err(meta_err("no \"builds\" object")),
    }
}

fn str_at<'a>(v: &'a Value, path: &[&str]) -> Result<&'a str, Error> {
    let mut cur = v;
    for k in path {
        cur = cur.get(*k).ok_or_else(|| meta_err(&format!("missing {}", path.join("."))))?;
    }
    cur.as_str().ok_or_else(|| meta_err(&format!("{} is not a string", path.join("."))))
}

/// The build label whose FFXiMain.dll has this hash, or None.
pub fn matching(root: &Path, ffximain_sha: &str) -> Result<Option<String>, Error> {
    for (label, b) in known(root)? {
        if str_at(&b, &["FFXiMain.dll", "sha256"])? == ffximain_sha {
            return Ok(Some(label));
        }
    }
    Ok(None)
}

/// Python json.dumps of a string (ensure_ascii).
pub fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            '\u{8}' => o.push_str("\\b"),
            '\u{c}' => o.push_str("\\f"),
            c if (c as u32) < 0x20 || (c as u32) > 0x7e => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    o.push_str(&format!("\\u{:04x}", u));
                }
            }
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

/// generated/build.json, as json.dump(indent=2) writes it.
pub fn record(root: &Path, label: &str, game: &str) -> Result<(), Error> {
    let path = chosen_path(root);
    let text = format!("{{\n  \"build\": {},\n  \"game\": {}\n}}\n", json_str(label), json_str(game));
    std::fs::write(&path, text).map_err(|e| Error::io(&path, e))
}

/// The recorded build with its builds.json entry.
pub struct Current {
    pub build: String,
    pub game: String,
    pub ffximain_meta: PathBuf,
    pub ffxi_meta: PathBuf,
    pub ffximain_sha: String,
    pub ffxi_sha: String,
    pub version: String,
    pub addresses: Vec<(String, String)>,
    pub hooks: Vec<(String, String)>,
    pub crt: Vec<(String, String)>,
}

fn pairs(v: Option<&Value>, what: &str) -> Result<Vec<(String, String)>, Error> {
    match v {
        None => Ok(Vec::new()),
        Some(Value::Object(m)) => m
            .iter()
            .map(|(k, v)| Ok((k.clone(), v.as_str().ok_or_else(|| meta_err(&format!("{}.{} is not a string", what, k)))?.to_string())))
            .collect(),
        Some(_) => Err(meta_err(&format!("{} is not an object", what))),
    }
}

/// buildinfo.current(): None when prepare has not run.
pub fn current(root: &Path) -> Result<Option<Current>, Error> {
    let path = chosen_path(root);
    if !path.exists() {
        return Ok(None);
    }
    let chosen = load_json(&path)?;
    let build = str_at(&chosen, &["build"])?.to_string();
    let game = str_at(&chosen, &["game"])?.to_string();
    let all = known(root)?;
    let b = all.get(&build).ok_or_else(|| meta_err(&format!("unknown build {}", build)))?;
    let meta = root.join("meta");
    let addresses = match b.get("addresses") {
        None => return Err(meta_err("missing addresses")),
        v => pairs(v, "addresses")?,
    };
    let crt = match b.get("crt") {
        None => return Err(meta_err("missing crt")),
        v => pairs(v, "crt")?,
    };
    Ok(Some(Current {
        ffximain_meta: meta.join(str_at(b, &["FFXiMain.dll", "meta"])?),
        ffxi_meta: meta.join(str_at(b, &["FFXi.dll", "meta"])?),
        ffximain_sha: str_at(b, &["FFXiMain.dll", "sha256"])?.to_string(),
        ffxi_sha: str_at(b, &["FFXi.dll", "sha256"])?.to_string(),
        version: str_at(b, &["version"])?.to_string(),
        hooks: pairs(b.get("hooks"), "hooks")?,
        addresses,
        crt,
        build,
        game,
    }))
}

/// Python int(v, 16).
pub fn parse_hex(v: &str) -> Result<u64, Error> {
    let t = v.trim();
    let (neg, t) = match t.strip_prefix('-') { Some(r) => (true, r), None => (false, t.strip_prefix('+').unwrap_or(t)) };
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    let n = u64::from_str_radix(&t.replace('_', ""), 16)
        .map_err(|_| Error::Meta(format!("invalid literal for int() with base 16: '{}'", v)))?;
    if neg { Ok(n.wrapping_neg()) } else { Ok(n) }
}

/// generated/build.h: the addresses of this build that the runtime and tests name directly.
/// Written only when its text changes.
pub fn write_build_h(root: &Path, b: &Current) -> Result<(), Error> {
    let mut lines = vec![
        "/* Generated by tools/build.py from meta/builds.json. Do not edit. */".to_string(),
        "#pragma once".to_string(),
        format!("#define FFXI_BUILD \"{}\"", b.build),
        format!("#define FFXI_VERSION \"{}\" /* the version string patch.ver carries */", b.version),
    ];
    for (k, v) in &b.addresses {
        lines.push(format!("#define FFXI_{} 0x{:08x}u", k.to_uppercase(), parse_hex(v)?));
    }
    for (k, v) in &b.hooks {
        lines.push(format!("#define FFXI_HOOK_{} 0x{:08x}u /* rt_hook_{} runs here */", k.to_uppercase(), parse_hex(v)?, k));
    }
    for (k, v) in &b.crt {
        let a = parse_hex(v)?;
        lines.push(format!("#define CRT_{} 0x{:08x}u", k.to_uppercase(), a));
        lines.push(format!("#define F_{} f_{:08x}", k.to_uppercase(), a));
    }
    let text = lines.join("\n") + "\n";
    crate::write_if_changed(&root.join("generated").join("build.h"), &text)
}

/// os.path.normpath, for the game path recorded in build.json.
pub fn normpath(p: &str) -> String {
    if cfg!(windows) {
        return normpath_nt(p);
    }
    if p.is_empty() {
        return ".".into();
    }
    let initial = if p.starts_with('/') { if p.starts_with("//") && !p.starts_with("///") { 2 } else { 1 } } else { 0 };
    let mut out: Vec<&str> = Vec::new();
    for c in p.split('/') {
        if c.is_empty() || c == "." {
            continue;
        }
        if c != ".." || (initial == 0 && out.is_empty()) || out.last() == Some(&"..") {
            out.push(c);
        } else if !out.is_empty() {
            out.pop();
        }
    }
    let s = "/".repeat(initial) + &out.join("/");
    if s.is_empty() { ".".into() } else { s }
}

/// ntpath.normpath (drive or UNC prefix, then the same folding with backslashes).
fn normpath_nt(p: &str) -> String {
    let p = p.replace('/', "\\");
    let (prefix, rest) = if let Some(unc) = p.strip_prefix("\\\\") {
        // \\server\share
        let parts: Vec<&str> = unc.splitn(3, '\\').collect();
        if parts.len() >= 2 {
            let pre = format!("\\\\{}\\{}", parts[0], parts[1]);
            let rest = p[pre.len()..].to_string();
            (pre, rest)
        } else {
            (String::new(), p.clone())
        }
    } else if p.len() >= 2 && p.as_bytes()[1] == b':' {
        (p[..2].to_string(), p[2..].to_string())
    } else {
        (String::new(), p.clone())
    };
    let root = rest.starts_with('\\');
    let mut out: Vec<&str> = Vec::new();
    for c in rest.split('\\') {
        if c.is_empty() || c == "." {
            continue;
        }
        if c == ".." {
            if out.last().is_some_and(|l| *l != "..") {
                out.pop();
                continue;
            }
            if root {
                continue;
            }
        }
        out.push(c);
    }
    let mut s = prefix + if root { "\\" } else { "" } + &out.join("\\");
    if s.is_empty() {
        s = ".".into();
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg(not(windows))]
    fn normpath_posix() {
        for (a, b) in [("/a//b/./c/", "/a/b/c"), ("a/../../b", "../b"), ("//x/y", "//x/y"), ("///x", "/x"), ("/..", "/"), ("", ".")] {
            assert_eq!(normpath(a), b, "{}", a);
        }
    }

    #[test]
    fn json_strings() {
        assert_eq!(json_str("C:\\FF \"XI\"\n"), "\"C:\\\\FF \\\"XI\\\"\\n\"");
        assert_eq!(json_str("é𝄞\u{7f}"), "\"\\u00e9\\ud834\\udd1e\\u007f\"");
    }
}
