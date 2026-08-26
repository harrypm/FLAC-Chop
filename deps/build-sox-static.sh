#!/usr/bin/env bash
# Build a minimal static libSoX (+ static libFLAC + zlib) for self-contained
# FLAC-Chop builds. Produces $PREFIX/lib/libsox.a and friends, plus sox.h.
#
# SoX is configured with only the FLAC format handler and the built-in effects
# (trim / sinc / rate / dither) — no ogg/vorbis/mpeg/mad/lame/png/sndfile/etc.,
# so the dep tree stays tiny. FLAC-Chop only ever reads/writes FLAC and uses
# those four effects, so this is sufficient.
#
# Usage: deps/build-sox-static.sh <prefix>
# Env:   CC, CXX, CFLAGS, CXXFLAGS, LDFLAGS, MAKEFLAGS (honored)
#
# Cacheable: re-runs are a no-op once $PREFIX/lib/libsox.a exists.
#
# Designed to run under:
#   - MSYS2 MINGW64      (mingw-w64-x86_64-gcc)
#   - MSYS2 CLANGARM64   (mingw-w64-clang-aarch64-clang)
#   - Linux              (system gcc)
#   - macOS              (clang / brew toolchain)
set -euo pipefail

PREFIX="${1:?usage: build-sox-static.sh <prefix>}"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

WORKDIR="${SOX_BUILD_WORKDIR:-$PREFIX/build}"
mkdir -p "$WORKDIR"

# Fast path: already built.
if [ -f "$PREFIX/lib/libsox.a" ] && [ -f "$PREFIX/include/sox.h" ]; then
  echo "libsox static already present at $PREFIX — skipping"
  exit 0
fi

NJOBS="${SOX_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

# Detect Windows / MSYS2.
IS_WINDOWS=0
if [[ "${OS:-}" == "Windows_NT" ]] || [[ "${MSYSTEM:-}" != "" ]]; then
  IS_WINDOWS=1
fi

# ---------------------------------------------------------------------------
# 1. zlib (sox links it for several format handlers; static, no shared).
# ---------------------------------------------------------------------------
build_zlib() {
  local src="$WORKDIR/zlib-src"
  if [ ! -d "$src" ]; then
    curl -fsSL "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz" -o "$WORKDIR/zlib.tgz"
    tar -C "$WORKDIR" -xzf "$WORKDIR/zlib.tgz"
    mv "$WORKDIR"/zlib-* "$src"
  fi
  cd "$src"
  if [ "$IS_WINDOWS" = "1" ]; then
    # zlib's win32/Makefile.gcc uses PREFIX as a *compiler* path prefix (e.g.
    # /mingw64/bin/), NOT the install prefix. Leave it empty so `gcc` is found
    # on the MSYS2 PATH. DESTDIR+prefix="" installs to $PREFIX/{include,lib}.
    make -f win32/Makefile.gcc -j"$NJOBS" \
      CFLAGS="${CFLAGS:-} -fPIC" \
      DESTDIR="$PREFIX" \
      prefix="" install
  else
    ./configure --static --prefix="$PREFIX" ${CFLAGS:+CFLAGS="$CFLAGS"}
    make -j"$NJOBS"
    make install
  fi
}

# ---------------------------------------------------------------------------
# 2. libFLAC (static, no ogg). CMake build.
# ---------------------------------------------------------------------------
build_flac() {
  local src="$WORKDIR/flac-src"
  if [ ! -d "$src" ]; then
    curl -fsSL "https://downloads.xiph.org/release/flac/flac-1.4.3.tar.xz" -o "$WORKDIR/flac.txz"
    tar -C "$WORKDIR" -xJf "$WORKDIR/flac.txz"
    mv "$WORKDIR"/flac-* "$src"
  fi
  cmake -S "$src" -B "$WORKDIR/flac-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DWITH_OGG=OFF \
    -DBUILD_PROGRAMS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DINSTALL_MANPAGES=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    ${CC:+-DCMAKE_C_COMPILER="$CC"} \
    ${CFLAGS:+-DCMAKE_C_FLAGS="$CFLAGS"} \
    ${LDFLAGS:+-DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS"}
  cmake --build "$WORKDIR/flac-build" --parallel "$NJOBS"
  cmake --install "$WORKDIR/flac-build"
}

# ---------------------------------------------------------------------------
# 3. SoX 14.4.2 (static, FLAC-only, built-in effects).
# ---------------------------------------------------------------------------
build_sox() {
  local src="$WORKDIR/sox-src"
  if [ ! -d "$src" ]; then
    curl -fsSL "https://downloads.sourceforge.net/project/sox/sox/14.4.2/sox-14.4.2.tar.gz" \
      -o "$WORKDIR/sox.tgz"
    tar -C "$WORKDIR" -xzf "$WORKDIR/sox.tgz"
    mv "$WORKDIR"/sox-* "$src"
  fi
  cd "$src"
  # Point sox at our static zlib + libFLAC.
  local fp="$PREFIX"
  export CFLAGS="${CFLAGS:-} -I$fp/include"
  export LDFLAGS="${LDFLAGS:-} -L$fp/lib"
  ./configure \
    --enable-static \
    --disable-shared \
    --prefix="$PREFIX" \
    --with-flac \
    --without-ogg \
    --without-vorbis \
    --without-opus \
    --without-mpeg \
    --without-mad \
    --without-lame \
    --without-twolame \
    --without-png \
    --without-sndfile \
    --without-alsa \
    --without-ao \
    --without-oss \
    --without-sndio \
    --without-amr \
    --without-pulseaudio \
    --without-sun-audio
  make -j"$NJOBS"
  make install
  # SoX installs libsox.a as libsox.a (and on some platforms libsox.la).
  test -f "$PREFIX/lib/libsox.a"
  test -f "$PREFIX/include/sox.h"
}

build_zlib
build_flac
build_sox

echo ":: libsox static build complete at $PREFIX"
ls -la "$PREFIX/lib"/libsox.a "$PREFIX/lib"/libFLAC.a "$PREFIX/lib"/libz.a 2>/dev/null || true
echo ":: set SOX_STATIC_PREFIX=$PREFIX"
echo ":: set SOX_STATIC_LIBS=\"FLAC z\""
