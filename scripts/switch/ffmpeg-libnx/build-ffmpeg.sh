#!/bin/bash
set -e

echo "=== Building FFmpeg 7.1 with libnx TLS and HTTPS support ==="
source /opt/devkitpro/switchvars.sh

RECIPE_DIR="/workspace/scripts/switch/ffmpeg-libnx"
BUILD_DIR="/tmp/ffmpeg-build"
LOCAL_ROOT="/workspace/.kicknx-build/ffmpeg-local-root"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -f "ffmpeg-7.1.tar.xz" ]; then
    echo "Downloading ffmpeg-7.1.tar.xz..."
    curl -L -o ffmpeg-7.1.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
fi

echo "Verifying SHA256 checksum..."
echo "40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6  ffmpeg-7.1.tar.xz" | sha256sum -c

rm -rf ffmpeg-7.1
echo "Extracting ffmpeg-7.1.tar.xz..."
tar -xf ffmpeg-7.1.tar.xz
cd ffmpeg-7.1

echo "Applying patches..."
patch -Np1 -i "$RECIPE_DIR/ffmpeg-7.1.patch"
patch -Np1 -i "$RECIPE_DIR/tls.patch"
python3 "$RECIPE_DIR/kicknx_patch_nvtegra_h264_refs.py" libavcodec/nvtegra_h264.c
python3 "$RECIPE_DIR/kicknx_patch_nvtegra_transfer_guard.py" libavutil/hwcontext_nvtegra.c

echo "Configuring FFmpeg for Nintendo Switch..."
./configure --prefix=$PORTLIBS_PREFIX --enable-gpl --disable-shared --enable-static \
  --cross-prefix=aarch64-none-elf- --enable-cross-compile \
  --arch=aarch64 --cpu=cortex-a57 --target-os=horizon --enable-pic \
  --extra-cflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec' \
  --extra-cxxflags='-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec' \
  --extra-ldflags="-fPIE -L${PORTLIBS_PREFIX}/lib -L${DEVKITPRO}/libnx/lib" \
  --disable-runtime-cpudetect --disable-programs --disable-debug --disable-doc --disable-autodetect \
  --enable-asm --enable-neon \
  --disable-avdevice --disable-encoders --disable-muxers \
  --enable-swscale --enable-swresample --enable-network \
  --disable-protocols --enable-protocol=file,http,https,ftp,tcp,udp,rtmp,tls,httpproxy \
  --enable-zlib --enable-bzlib --enable-libass --enable-libfreetype --enable-libfribidi --enable-libdav1d \
  --enable-libnx --enable-nvtegra

echo "Compiling FFmpeg (using $(nproc) cores)..."
make -j$(nproc)

echo "Installing to local dependency root and system portlibs..."
mkdir -p "$LOCAL_ROOT"
make DESTDIR="$LOCAL_ROOT" install
make install

echo "Cleaning up temporary build tree..."
cd /
rm -rf "$BUILD_DIR"

echo "=== FFmpeg with libnx TLS successfully built and installed! ==="
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm /opt/devkitpro/portlibs/switch/lib/libavformat.a | grep -E 'ff_https_protocol|ff_tls_protocol'
