#!/bin/bash
# The Windows build CI runs (.github/workflows/build.yml), in Git Bash on a Windows runner: the
# launcher's installer with the game host beside it (tools/windows_dist.py: xi-host.exe and SDL3.dll,
# built with zig), into dist/. Needs Rust (cargo) and Python on PATH.
set -euo pipefail
cd "$(dirname "$0")/../.."
zig=$(tools/ci/install-zig.sh 0.16.0 "$HOME/.local/zig")
export PATH="$zig:$HOME/.cargo/bin:$PATH"
command -v cargo-tauri >/dev/null || cargo install tauri-cli --version "^2" --locked
python tools/windows_dist.py --zig "$zig/zig.exe"

# the launcher's PlayOnline C (tested here), then the installer
(cd launcher/src-tauri && CARGO_TARGET_DIR=../../build/launcher-target cargo tauri build --bundles nsis)

mkdir -p dist
cp build/launcher-target/release/bundle/nsis/*.exe dist/
cp build/windows-x86_64/xi-host.exe dist/xi-host-windows-x86_64.exe
ls -la dist
