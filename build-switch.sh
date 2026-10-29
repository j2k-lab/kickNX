#!/usr/bin/env bash
set -euo pipefail

: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
JOBS="${JOBS:-8}"

git config --global --add safe.directory /workspace || true

cmake_args=(
  -S .
  -B build_switch
  -G Ninja
  -DPLATFORM_SWITCH=ON
  -DBUILTIN_NSP=OFF
  -DCMAKE_BUILD_TYPE=Release
  "-DPKG_CONFIG_EXECUTABLE=$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config"
)

if [[ -n "${KICKNX_FFMPEG_ROOT:-}" ]]; then
  cmake_args+=("-DKICKNX_FFMPEG_ROOT=$KICKNX_FFMPEG_ROOT")
fi

cmake "${cmake_args[@]}"
ninja -C build_switch -j"$JOBS" KickNX.nro
