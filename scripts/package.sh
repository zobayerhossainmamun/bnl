#!/usr/bin/env sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION="${VERSION:-v1.0.0}"
RES="resources"
LICENSE="$RES/LICENSE.txt"
README="$RES/README.txt"
INSTALLER="installer"
OUT="binaries"

mkdir -p "$OUT"
ABS_OUT="$(cd "$OUT" && pwd)"

for f in "$LICENSE" "$README"; do
    if [ ! -f "$f" ]; then
        echo "missing required file: $f" >&2
        exit 1
    fi
done

package() {
    name="$1"        # e.g. linux-x64
    build_dir="$2"   # e.g. build/linux-release

    bnl="$build_dir/bin/bnl"
    if [ ! -f "$bnl" ]; then
        echo "skip   $name: $bnl not built"
        return 0
    fi

    stem="bnlang-$name-$VERSION"
    tmp="$(mktemp -d 2>/dev/null || mktemp -d -t bnl-pkg)"

    cp "$bnl"     "$tmp/bnl"
    cp "$LICENSE" "$tmp/LICENSE.txt"
    cp "$README"  "$tmp/README.txt"
    chmod +x "$tmp/bnl"

    out="$ABS_OUT/$stem.tar.gz"
    rm -f "$out"
    ( cd "$tmp" && tar -czf "$out" bnl LICENSE.txt README.txt )
    rm -rf "$tmp"
    echo "build  $out"
}

package "linux-x64"   "build/linux-release"
package "linux-x86"   "build/linux-x86-release"
package "macos-arm64" "build/macos-release"
package "macos-x64"   "build/macos-x64-release"

# --- Installers --------------------------------------------------------
# Look for installer files of any extension at the conventional path:
#   installer/<os>/bnlang-<os>-<arch>-<version>-installer.*
for os_name in linux macos; do
    dir="$INSTALLER/$os_name"
    [ -d "$dir" ] || continue
    for f in "$dir"/bnlang-"$os_name"-*-"$VERSION"-installer.*; do
        [ -f "$f" ] || continue
        base="$(basename "$f")"
        cp -f "$f" "$ABS_OUT/$base"
        echo "copy   $ABS_OUT/$base"
    done
done

echo
echo "Done. Artifacts in: $OUT"
