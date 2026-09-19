#!/bin/sh
# NexIOS picolibc offline build driver (issue #72).
# Toolchain: meson + ninja via `python3 -m pip install --user meson ninja`
# (pinned: meson 1.12.0, ninja 1.13.2; brew install meson ninja is fallback).
# Performs ZERO network I/O: the vendored tarball under third_party/ is the
# sole source (--wrap-mode=nodownload); a missing tarball fails closed.
set -eu

VERSION="${PICOLIBC_VERSION:-1.8.12}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARBALL="$ROOT/third_party/picolibc-$VERSION.tar.xz"
SHAFILE="$TARBALL.sha256"
BLD_BASE="$ROOT/build/picolibc"
SYSROOT="$BLD_BASE/sysroot"

# Locate meson/ninja (pip --user site first, then PATH).
for d in "$HOME/Library/Python/3.14/bin" "$HOME/.local/bin"; do
    case ":$PATH:" in *":$d:"*) ;; *) PATH="$d:$PATH" ;; esac
done
command -v meson >/dev/null || {
    echo "error: meson not found (python3 -m pip install --user meson ninja)" >&2
    exit 1
}
command -v ninja >/dev/null || {
    echo "error: ninja not found (python3 -m pip install --user meson ninja)" >&2
    exit 1
}

[ -f "$TARBALL" ] || {
    echo "error: missing $TARBALL (vendored tarball required; no download)" >&2
    exit 1
}
[ -f "$SHAFILE" ] || {
    echo "error: missing $SHAFILE" >&2
    exit 1
}
(cd "$ROOT/third_party" && shasum -a 256 -c "$(basename "$SHAFILE")") || exit 1

rm -rf "$BLD_BASE/src" "$BLD_BASE/bld"
mkdir -p "$BLD_BASE"
tar -xf "$TARBALL" -C "$BLD_BASE"
mv "$BLD_BASE/picolibc-$VERSION" "$BLD_BASE/src"

meson setup "$BLD_BASE/bld" "$BLD_BASE/src" \
    --cross-file "$ROOT/tools/picolibc-x86_64-elf.ini" \
    --prefix="$SYSROOT" \
    --wrap-mode=nodownload \
    -Ddefault_library=static \
    -Db_staticpic=false \
    -Dmultilib=false \
    -Dtests=false \
    -Dsemihost=false \
    -Dpicocrt=true \
    -Dspecsdir=lib
ninja -C "$BLD_BASE/bld"
ninja -C "$BLD_BASE/bld" install

# Static-only assertions (fail-closed).
[ -f "$SYSROOT/lib/libc.a" ] || { echo "error: libc.a missing" >&2; exit 1; }
[ -f "$SYSROOT/lib/libm.a" ] || { echo "error: libm.a missing" >&2; exit 1; }
if find "$SYSROOT" -name '*.so*' | grep -q .; then
    echo "error: shared objects leaked into sysroot" >&2
    exit 1
fi
echo "picolibc $VERSION sysroot ready: $SYSROOT"
