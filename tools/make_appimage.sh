#!/usr/bin/env bash
# Packages the port as a distributable AppImage: the executable plus its HD
# texture replacements, with a launcher and icon. The disc image is not
# included (it is the user's own), so the port asks for it on first launch and
# remembers the answer.
#
# Usage: tools/make_appimage.sh [build-dir] [output-dir]
#
# appimagetool is fetched into the output directory on first use; it needs
# network access to pull the AppImage runtime unless --runtime-file is used.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build/port-gcc"}
OUT=${2:-"$ROOT/build/appimage"}
BIN="$BUILD/metroid_prime_port"
TEXTURES="$ROOT/textures"
ICON="$ROOT/packaging/metroid_prime_port.png"
TOOL="$OUT/appimagetool-x86_64.AppImage"
TOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

if [[ ! -x "$BIN" ]]; then
    echo "no executable at $BIN - build first" >&2
    exit 1
fi
# A build with MP_EMBED_RESOURCES carries the textures inside the executable.
EMBEDDED=0
grep -q '^MP_EMBED_RESOURCES:BOOL=ON' "$BUILD/CMakeCache.txt" 2>/dev/null && EMBEDDED=1
if [[ $EMBEDDED = 0 && ! -d "$TEXTURES" ]]; then
    echo "no texture replacements at $TEXTURES" >&2
    exit 1
fi

APPDIR="$OUT/MetroidPrime.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"

install -m755 "$BIN" "$APPDIR/usr/bin/metroid_prime_port"
# Otherwise the port looks for replacements next to the executable.
if [[ $EMBEDDED = 0 ]]; then
    cp -r "$TEXTURES" "$APPDIR/usr/bin/textures"
fi
install -m644 "$ICON" "$APPDIR/metroid-prime.png"
# File managers and AppImage launchers read the icon from here.
ln -s metroid-prime.png "$APPDIR/.DirIcon"

# No shared libraries are bundled. The release binary is built on an old base
# (.github/workflows/linux-release.yml), so the system's own freetype, libpng,
# zstd and OpenSSL are at least as new as the ones it was linked against, while
# a bundled copy drags in its build host's sonames (AlmaLinux's freetype wants
# libbz2.so.1 and harfbuzz, which Debian doesn't provide under those names).
# An AppImage made from a desktop build only runs on distributions at least as
# new as that desktop.

# Third-party notices have to travel with anything that is handed out. The
# vendored components are in the tree; the fetched ones are in the build
# directory, so the AppDir is assembled from whichever of those exist.
mkdir -p "$APPDIR/usr/share/licenses/metroid-prime-port"
collect_notice() {
    # $1 = source licence file, $2 = name it is filed under.
    [[ -f "$1" ]] || return 0
    cp "$1" "$APPDIR/usr/share/licenses/metroid-prime-port/$2"
}
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${build_dir:-$(dirname -- "$BIN")}
# The port's own grant and notice first. A build that links MIT and zlib
# components has to carry their terms, and a grant nobody can read inside the
# package is not much of a grant.
collect_notice "$repo_root/LICENSE" port-license.txt
collect_notice "$repo_root/NOTICE" port-notice.txt
collect_notice "$repo_root/extern/aurora/LICENSE" aurora.txt
collect_notice "$repo_root/extern/musyx/LICENSE" musyx.txt
collect_notice "$repo_root/extern/astcenc/LICENSE.txt" astcenc.txt
for dep in sdl-src imgui-src fmt-src zstd-src; do
    for notice in "$build_dir"/_deps/"$dep"/LICENSE* "$build_dir"/_deps/"$dep"/COPYING*; do
        [[ -f "$notice" ]] || continue
        cp "$notice" "$APPDIR/usr/share/licenses/metroid-prime-port/${dep}.txt"
        break
    done
done

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/metroid_prime_port" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

cat > "$APPDIR/metroid-prime.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Metroid Prime
Comment=Native recompilation of Metroid Prime
Exec=metroid_prime_port
Icon=metroid-prime
Categories=Game;
Terminal=false
EOF

if [[ ! -x "$TOOL" ]]; then
    echo "fetching appimagetool"
    curl -sSL -o "$TOOL" "$TOOL_URL"
    chmod +x "$TOOL"
fi

# FUSE is not available everywhere; the tool can unpack itself instead.
RUN=()
if ! "$TOOL" --version >/dev/null 2>&1; then
    RUN=(--appimage-extract-and-run)
fi

ARCH=x86_64 "$TOOL" "${RUN[@]}" "$APPDIR" "$OUT/MetroidPrime-x86_64.AppImage"
echo "wrote $OUT/MetroidPrime-x86_64.AppImage"
