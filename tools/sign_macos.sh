#!/bin/bash
# sign_macos.sh <FFXI Launcher.app> [identity]: a Developer ID signature, hardened runtime and a secure
# timestamp on everything in the app, inside out, as notarization wants. The identity: the argument,
# else MACOS_SIGN_IDENTITY, else the keychain's first "Developer ID Application".
#
# One entitlement, on the game host only: library validation off. The game module it loads
# (runtime/xi_game.h) is compiled on the player's machine, so no team signed it; the hardened runtime
# would refuse it otherwise. The launcher keeps library validation.
set -euo pipefail
app=$1
id=${2:-${MACOS_SIGN_IDENTITY:-}}
if [ -z "$id" ]; then
  id=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)
fi
[ -n "$id" ] || { echo "sign_macos.sh: no Developer ID Application identity in the keychain" >&2; exit 1; }
echo "signing $app as: $id"

ent=$(mktemp -t xi-host-entitlements).plist
cat > "$ent" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.cs.disable-library-validation</key><true/>
</dict>
</plist>
PLIST
sign() { codesign --force --timestamp --options runtime --sign "$id" "$@"; }

game="$app/Contents/Helpers/FINAL FANTASY XI.app"
for lib in "$game/Contents/Frameworks/"*.dylib; do sign "$lib"; done
sign --entitlements "$ent" "$game"
sign "$app"
rm -f "$ent"
codesign --verify --deep --strict --verbose=2 "$app"
codesign -d --entitlements - "$game" 2>/dev/null | grep -q disable-library-validation
echo "signed: $app"
