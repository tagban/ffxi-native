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
  *) echo "no zig for $(uname -sm)" >&2; exit 1 ;;
esac
mkdir -p "$dir"
curl -fsSL https://ziglang.org/download/index.json -o "$dir/index.json"
set -- $(python3 -c "import json,sys; e=json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]]; print(e['tarball'], e['shasum'])" \
  "$dir/index.json" "$version" "$target")
url=$1 sha=$2
curl -fsSL "$url" -o "$dir/zig.tar.xz"
got=$( (sha256sum "$dir/zig.tar.xz" 2>/dev/null || shasum -a 256 "$dir/zig.tar.xz") | cut -d' ' -f1)
[ "$got" = "$sha" ] || { echo "zig: SHA-256 $got, expected $sha" >&2; exit 1; }
tar -xJf "$dir/zig.tar.xz" -C "$dir"
rm "$dir/zig.tar.xz" "$dir/index.json"
echo "$dir/zig-$target-$version"
