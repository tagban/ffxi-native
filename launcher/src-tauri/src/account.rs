//! Making an account on a LandSandBoat server, as xiloader does: the same TLS request to its login
//! port as signing in (host/lsb_login.c), with command 0x20. The server answers with a result:
//! 3 made, 4 the name is taken, 8 the server does not let accounts be made this way, 9 an error.
//! (LandSandBoat's src/login/auth_session.cpp; the server's login.ACCOUNT_CREATION must be on.)

use std::io::{Read, Write};
use std::net::ToSocketAddrs;
use std::sync::Arc;
use std::time::Duration;

/// xiloader's login port.
const AUTH_PORT: u16 = 54231;

/// The loader versions a request can say it is (the newest first), as the host signs in with.
const LOADER_VERSIONS: [[u8; 3]; 2] = [[2, 2, 0], [2, 1, 2]];

/// The login server's certificate is its own (xiloader takes any): so is it here.
#[derive(Debug)]
struct AnyCertificate(Vec<rustls::SignatureScheme>);

impl rustls::client::danger::ServerCertVerifier for AnyCertificate {
    fn verify_server_cert(
        &self,
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &[rustls::pki_types::CertificateDer<'_>],
        _: &rustls::pki_types::ServerName<'_>,
        _: &[u8],
        _: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }
    fn verify_tls13_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }
    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.0.clone()
    }
}

/// One JSON request to the login server, and its JSON reply.
fn exchange(host: &str, port: u16, request: &str) -> Result<serde_json::Value, String> {
    let timeout = Duration::from_secs(8);
    let addr = (host, port)
        .to_socket_addrs()
        .map_err(|e| format!("{host}: {e}"))?
        .next()
        .ok_or(format!("{host}: no address"))?;
    let tcp = std::net::TcpStream::connect_timeout(&addr, timeout).map_err(|e| format!("{host}:{port}: {e}"))?;
    tcp.set_read_timeout(Some(timeout)).ok();
    tcp.set_write_timeout(Some(timeout)).ok();
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let schemes = provider.signature_verification_algorithms.supported_schemes();
    let config = rustls::ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|e| format!("TLS: {e}"))?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(AnyCertificate(schemes)))
        .with_no_client_auth();
    let name = rustls::pki_types::ServerName::try_from(host.to_string()).map_err(|e| format!("{host}: {e}"))?;
    let conn = rustls::ClientConnection::new(Arc::new(config), name).map_err(|e| format!("TLS: {e}"))?;
    let mut tls = rustls::StreamOwned::new(conn, tcp);
    tls.write_all(request.as_bytes()).map_err(|e| format!("{host}:{port}: {e}"))?;
    tls.flush().map_err(|e| format!("{host}:{port}: {e}"))?;
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        match tls.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if serde_json::from_slice::<serde_json::Value>(&buf).is_ok() {
                    break;
                }
            }
            Err(e) if buf.is_empty() => return Err(format!("{host}:{port}: {e}")),
            Err(_) => break,
        }
    }
    serde_json::from_slice(&buf).map_err(|_| "the login server's answer could not be read".to_string())
}

/// "update to version '2.2.x'" / "requires version 2.1.x": the version a refusal names.
fn named_version(message: &str) -> Option<[u8; 3]> {
    let chars: Vec<char> = message.chars().collect();
    for i in 0..chars.len() {
        if !chars[i].is_ascii_digit() || (i > 0 && chars[i - 1].is_ascii_digit()) || i + 2 >= chars.len() || chars[i + 1] != '.' {
            continue;
        }
        let major = chars[i].to_digit(10)? as u8;
        if let Some(minor) = chars[i + 2].to_digit(10) {
            if let Some(v) = LOADER_VERSIONS.iter().copied().find(|v| v[0] == major && v[1] == minor as u8) {
                return Some(v);
            }
        }
    }
    None
}

/// Makes the account; the words to show the player on success.
pub fn create(server: &str, port: u16, user: &str, password: &str) -> Result<String, String> {
    let user = user.trim();
    if user.is_empty() || user.len() > 16 {
        return Err("An account name is 1 to 16 characters.".into());
    }
    if password.is_empty() || password.len() > 32 {
        return Err("A password is 1 to 32 characters.".into());
    }
    let host = server.trim();
    if host.is_empty() {
        return Err("Fill in the server first.".into());
    }
    let port = if port == 0 { AUTH_PORT } else { port };
    let mut version = LOADER_VERSIONS[0];
    for attempt in 0..2 {
        let request = serde_json::json!({
            "command": 0x20,
            "username": user,
            "password": password,
            "new_password": "",
            "otp": "",
            "trust_token": "",
            "trust_this_computer": false,
            "version": version,
        })
        .to_string();
        let reply = exchange(host, port, &request)?;
        if let Some(message) = reply.get("error_message").and_then(|m| m.as_str()).filter(|m| !m.is_empty()) {
            match named_version(message) {
                Some(v) if attempt == 0 && v != version => {
                    version = v;
                    continue;
                }
                _ => return Err(format!("The server says: {message}")),
            }
        }
        return match reply.get("result").and_then(|r| r.as_i64()) {
            Some(3) => Ok(format!("The account {user} was made. You can play with it now.")),
            Some(4) => Err(format!("The name {user} is taken on this server.")),
            Some(8) => Err("This server does not let accounts be made from the launcher. Ask its staff how to make one.".into()),
            Some(9) => Err("The server could not make the account (an error on its side).".into()),
            Some(r) => Err(format!("The server answered {r}.")),
            None => Err("The login server's answer had no result.".into()),
        };
    }
    Err("The server wants another version of the loader.".into())
}
