#!/bin/bash
# The Linux build CI runs (.github/workflows/build.yml), for this machine's architecture: the launcher's
# .deb and AppImage with the portable game host inside (tools/linux_dist.py), into dist/. Ubuntu 22.04
# or later; needs Rust (cargo) on PATH, and sudo for the packages.
set -euo pipefail
cd "$(dirname "$0")/../.."
arch=$(uname -m)

# the launcher's (Tauri: webkit2gtk), and SDL's back ends' headers (its libraries are dlopen'd)
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
  clang pkg-config cmake ninja-build python3 curl xz-utils file dpkg-dev \
  libwebkit2gtk-4.1-dev libayatana-appindicator3-dev librsvg2-dev libssl-dev libsecret-1-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
  libxkbcommon-dev libwayland-dev libwayland-bin wayland-protocols libdecor-0-dev libpulse-dev \
  libasound2-dev libpipewire-0.3-dev libdrm-dev libgbm-dev libegl-dev libgl-dev libdbus-1-dev libudev-dev

zig=$(tools/ci/install-zig.sh 0.16.0 "$HOME/.local/zig")
export PATH="$zig:$HOME/.cargo/bin:$PATH"
# the AppImage tools (linuxdeploy) are AppImages: unpacked and run, with no FUSE needed
export APPIMAGE_EXTRACT_AND_RUN=1
command -v cargo-tauri >/dev/null || cargo install tauri-cli --version "^2" --locked

python3 tools/build_posix.py launcher

# what the players' machines need: glibc 2.28 for the game host, the build machine's for the launcher
max_glibc() { objdump -T "$1" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1; }
host=build/linux-$arch/xi-host
echo "xi-host needs $(max_glibc "$host"); ffxi-launcher needs $(max_glibc build/launcher-target/release/ffxi-launcher)"
[ "$(printf '%s\nGLIBC_2.28\n' "$(max_glibc "$host")" | sort -V | tail -1)" = GLIBC_2.28 ] ||
  { echo "xi-host needs a glibc newer than 2.28" >&2; exit 1; }
if objdump -p "$host" | grep NEEDED | grep -qv -E 'lib(c|m|dl|pthread|rt)\.so|ld-linux'; then
  echo "xi-host links more than the C library:" >&2; objdump -p "$host" | grep NEEDED >&2; exit 1
fi

mkdir -p dist
cp build/*.deb build/*.AppImage dist/
cp "$host" "dist/xi-host-linux-$arch"
ls -la dist
