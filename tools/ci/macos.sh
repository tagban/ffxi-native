#!/bin/bash
# The macOS build CI runs (.github/workflows/build.yml): FFXI Launcher.app with xi-host and its
# libraries inside, zipped into dist/. Needs Homebrew and Rust (cargo) on PATH.
#
# Signed with a Developer ID and notarized when the repository has these secrets (else ad hoc):
#   MACOS_CERTIFICATE           the "Developer ID Application" certificate and key, a .p12, base64
#   MACOS_CERTIFICATE_PASSWORD  its password
#   APPLE_API_KEY               an App Store Connect API key (.p8), base64
#   APPLE_API_KEY_ID, APPLE_API_ISSUER
set -euo pipefail
cd "$(dirname "$0")/../.."
brew install sdl3 mbedtls pkg-config
export PATH="$HOME/.cargo/bin:$PATH"
command -v cargo-tauri >/dev/null || cargo install tauri-cli --version "^2" --locked
python3 tools/build_posix.py launcher
app="build/FFXI Launcher.app"
out="dist/FFXI-Launcher-macos-$(uname -m).zip"
mkdir -p dist

if [ -n "${MACOS_CERTIFICATE:-}" ]; then
  # a keychain of the run's own, holding only the certificate
  kc="$RUNNER_TEMP/signing.keychain-db" kcpw=$(uuidgen)
  security create-keychain -p "$kcpw" "$kc"
  security set-keychain-settings -lut 21600 "$kc"
  security unlock-keychain -p "$kcpw" "$kc"
  echo "$MACOS_CERTIFICATE" | base64 --decode > "$RUNNER_TEMP/cert.p12"
  security import "$RUNNER_TEMP/cert.p12" -k "$kc" -P "$MACOS_CERTIFICATE_PASSWORD" -T /usr/bin/codesign
  security set-key-partition-list -S apple-tool:,apple: -s -k "$kcpw" "$kc" >/dev/null
  security list-keychains -d user -s "$kc" $(security list-keychains -d user | tr -d '"')
  rm "$RUNNER_TEMP/cert.p12"
  tools/sign_macos.sh "$app"
  if [ -n "${APPLE_API_KEY:-}" ]; then
    export APPLE_API_KEY_PATH="$RUNNER_TEMP/AuthKey.p8"
    echo "$APPLE_API_KEY" | base64 --decode > "$APPLE_API_KEY_PATH"
    tools/notarize_macos.sh "$app" "$out"
  else
    echo "signed but not notarized: no APPLE_API_KEY secret"
    ditto -c -k --keepParent "$app" "$out"
  fi
else
  echo "no MACOS_CERTIFICATE secret: an ad hoc signature (Gatekeeper warns players)"
  codesign --verify --deep --strict "$app"
  # named apart, so it never replaces a notarized build in a release
  ditto -c -k --keepParent "$app" "${out%.zip}-unsigned.zip"
fi
ls -la dist
