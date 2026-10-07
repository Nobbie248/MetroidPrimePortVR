#!/usr/bin/env bash
# Builds a Flatpak bundle of the port.
#
# A Flatpak brings its own runtime, so unlike the AppImage it does not care what
# glibc the host has; the GPU driver still comes from the host. The disc image
# is not included and is read-only from inside the sandbox, so the port asks for
# it on first launch as usual.
#
# Usage: tools/make_flatpak.sh [output-dir]
#
# Needs flatpak and flatpak-builder, and downloads the runtime and SDK on first
# use. The build compiles the whole game, so expect it to take a while.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/flatpak"}
MANIFEST="$ROOT/flatpak/io.github.odrannnn.metroidprimeport.yml"
APP_ID=$(sed -n 's/^app-id: *//p' "$MANIFEST")

for tool in flatpak flatpak-builder; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool is not installed; install flatpak and flatpak-builder" >&2
        exit 1
    fi
done

# The manifest's git source points at the repository, so the tree has to be
# committed for the build to see the current code.
if [[ -n "$(git -C "$ROOT" status --porcelain)" ]]; then
    echo "warning: uncommitted changes are not part of a git source" >&2
fi

# Flatpak shows the newest metainfo release as the app's version, so it has to
# match the release being built.
METAINFO="$ROOT/packaging/$APP_ID.metainfo.xml"
APP_VERSION=$(sed -n 's/^ *versionName "\(.*\)"/\1/p' "$ROOT/android/app/build.gradle")
META_VERSION=$(sed -n 's/^ *<release version="\([^"]*\)".*/\1/p' "$METAINFO" | head -n 1)
if [[ "$APP_VERSION" != "$META_VERSION" ]]; then
    echo "the newest release in $METAINFO is $META_VERSION, but versionName is" \
        "$APP_VERSION; add a <release> entry for $APP_VERSION" >&2
    exit 1
fi

mkdir -p "$OUT"
# rofiles-fuse gives the build a source tree whose files are immutable, so a
# build step cannot modify the checkout it came from. It needs FUSE, and the
# fusermount3 helper is setuid - which does not help once the caller is already
# inside a user namespace, because a setuid binary cannot gain privilege inside
# one. On such a host every attempt fails with
#   fusermount3: mount failed: Permission denied
# even with user_allow_other set in /etc/fuse.conf and even though a manual
# rofiles-fuse mount as the same user succeeds. The flag below swaps the
# mechanism and is otherwise equivalent, so try it first and fall back only if
# the real thing is refused.
build_with_rofiles() {
    flatpak-builder --force-clean --repo="$OUT/repo" "$OUT/build" "$MANIFEST" "$@"
}

if ! build_with_rofiles; then
    echo >&2
    echo "note: retrying without rofiles-fuse (see the comment above)" >&2
    build_with_rofiles --disable-rofiles-fuse
fi
flatpak build-bundle "$OUT/repo" "$OUT/MetroidPrimePort.flatpak" "$APP_ID"
echo "wrote $OUT/MetroidPrimePort.flatpak"
echo "install with: flatpak install --user $OUT/MetroidPrimePort.flatpak"
