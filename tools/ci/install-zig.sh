#!/bin/sh
# install-zig.sh <version> <dir>: ziglang.org's zig for this machine into <dir>, checked against the
# SHA-256 its download index publishes (the same check the launcher makes). Prints zig's folder.
set -eu
version=$1 dir=$2
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) target=x86_64-linux ;;
  Linux-aarch64) target=aarch64-linux ;;
  Darwin-arm64) target=aarch64-macos ;;
  Darwin-x86_64) target=x86_64-macos ;;
  MINGW*-x86_64|MSYS*-x86_64|CYGWIN*-x86_64) target=x86_64-windows ;;
  *) echo "no zig for $(uname -sm)" >&2; exit 1 ;;
esac
mkdir -p "$dir"
curl -fsSL https://ziglang.org/download/index.json -o "$dir/index.json"
set -- $(python3 -c "import json,sys; e=json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]]; print(e['tarball'], e['shasum'])" \
  "$dir/index.json" "$version" "$target")
url=$1 sha=$2
archive="$dir/$(basename "$url")"
curl -fsSL "$url" -o "$archive"
got=$( (sha256sum "$archive" 2>/dev/null || shasum -a 256 "$archive") | cut -d' ' -f1)
[ "$got" = "$sha" ] || { echo "zig: SHA-256 $got, expected $sha" >&2; exit 1; }
case "$archive" in
  *.zip) (cd "$dir" && unzip -q "$archive") ;;
  *) tar -xJf "$archive" -C "$dir" ;;
esac
rm "$archive" "$dir/index.json"
echo "$dir/zig-$target-$version"
