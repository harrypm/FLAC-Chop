#!/usr/bin/env bash
# Build libFLAC 1.5.x from source into /usr/local for the CI Linux jobs.
#
# ubuntu-22.04's apt libflac-dev is 1.3.4, but the copy-in ld-compress-ng
# uses the 1.5 chained-Ogg-FLAC decoder APIs (FLAC__STREAM_DECODER_END_OF_
# LINK, FLAC__stream_decoder_set_decode_chained_stream). MSYS2 and Homebrew
# already ship 1.5.x, so only the Linux runners need this.
#
# Usage (after apt-get install -y libogg-dev):
#   scripts/ci_build_flac.sh [version]     # default 1.5.0
#
# Exports PKG_CONFIG_PATH=/usr/local/lib/pkgconfig via GITHUB_ENV so the
# ld-compress-ng subproject's pkg_check_modules(FLAC) resolves THIS build,
# and runs ldconfig so spawned binaries find /usr/local/lib at runtime.
set -eux

FLAC_VERSION="${1:-1.5.0}"

curl -fsSL \
  "https://github.com/xiph/flac/releases/download/${FLAC_VERSION}/flac-${FLAC_VERSION}.tar.xz" \
  -o /tmp/flac.tar.xz
rm -rf "/tmp/flac-${FLAC_VERSION}" /tmp/flac-build
tar -xf /tmp/flac.tar.xz -C /tmp

# Shared libs into /usr/local; Ogg support ON (the 1.5 chained-stream API
# is the Ogg-FLAC decoder path) — libogg-dev must be installed beforehand.
cmake -S "/tmp/flac-${FLAC_VERSION}" -B /tmp/flac-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local
sudo cmake --build /tmp/flac-build --target install
sudo ldconfig
/usr/local/bin/flac --version

echo "PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}" >> "$GITHUB_ENV"
