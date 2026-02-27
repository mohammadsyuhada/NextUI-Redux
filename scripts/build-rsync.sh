#!/bin/sh
# Build a statically-linked rsync binary for aarch64 (TrimUI devices)
# Run this inside the tg5040/tg5050 Docker toolchain container.
#
# Usage:
#   ./scripts/build-rsync.sh
#
# Output:
#   skeleton/SYSTEM/shared/bin/rsync

set -e

RSYNC_VERSION="3.2.7"
RSYNC_URL="https://download.samba.org/pub/rsync/rsync-${RSYNC_VERSION}.tar.gz"
BUILD_DIR="/tmp/rsync-build"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

if [ -z "$CROSS_COMPILE" ]; then
    echo "Error: CROSS_COMPILE is not set. Run this inside the Docker toolchain."
    exit 1
fi

echo "Building rsync ${RSYNC_VERSION} for aarch64 (static)..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "Downloading rsync..."
wget -q "$RSYNC_URL"
tar xzf "rsync-${RSYNC_VERSION}.tar.gz"
cd "rsync-${RSYNC_VERSION}"

echo "Configuring..."
./configure \
    --host=aarch64-linux-gnu \
    CC="${CROSS_COMPILE}gcc" \
    CFLAGS="-static -O2" \
    LDFLAGS="-static" \
    --disable-xxhash \
    --disable-zstd \
    --disable-lz4 \
    --disable-openssl \
    --disable-md2man

echo "Compiling..."
make -j$(nproc)

echo "Stripping binary..."
${CROSS_COMPILE}strip rsync

echo "Installing to project..."
cp rsync "$PROJECT_DIR/skeleton/SYSTEM/shared/bin/rsync"

echo "Cleaning up..."
rm -rf "$BUILD_DIR"

echo "Done! rsync binary installed to skeleton/SYSTEM/shared/bin/rsync"
ls -la "$PROJECT_DIR/skeleton/SYSTEM/shared/bin/rsync"
