# Building kickNX for Nintendo Switch

## Supported playback stack

kickNX 0.5.0 is validated with this Switch playback stack:

```text
MPV:    switch-libmpv 0.39.0-4   (devkitPro's package)
FFmpeg: switch-ffmpeg 7.1-12     (built from this repository)
```

The FFmpeg recipe in `scripts/switch/ffmpeg-libnx` adds libnx TLS support and
kickNX guards for NVTEGRA H.264 references, decode maps and hardware-frame
transfers. MPV is devkitPro's stock package. Using a different FFmpeg may
compile, but it is not the playback stack validated against Kick streams.

The recommended way to build is the Docker image in this repository, which
provides devkitPro, MPV and every other library. You only build the guarded
FFmpeg yourself.

## 1. Build the guarded FFmpeg package

This step uses the devkitPro MSYS2 shell (on Linux, a devkitPro install with
`makepkg`). You only need to repeat it when the FFmpeg recipe changes.

```bash
cd scripts/switch/ffmpeg-libnx
rm -rf src pkg
makepkg -Csf --noconfirm
```

Expected package:

```text
switch-ffmpeg-7.1-12-any.pkg.tar.zst
```

Extract it into the repository-local dependency root:

```bash
cd ../../..
rm -rf .kicknx-build/ffmpeg-local-root
mkdir -p .kicknx-build/ffmpeg-local-root
bsdtar -xf scripts/switch/ffmpeg-libnx/switch-ffmpeg-7.1-12-any.pkg.tar.zst \
  -C .kicknx-build/ffmpeg-local-root
```

The `.kicknx-build` directory is ignored by Git. CMake picks it up
automatically; the build log then shows
`Using kickNX FFmpeg override: .../.kicknx-build/ffmpeg-local-root/...`.

## 2. Build kickNX with Docker (recommended)

Install Docker Desktop (Windows, macOS) or Docker Engine (Linux).

Build the image once from the repository root. Rebuild it only when the
`Dockerfile` changes:

```bash
docker build -t kicknx-switch-builder .
```

The image is based on `devkitpro/devkita64:latest`, so the library versions
come from devkitPro at the time the image is built. Check that it has the
validated MPV:

```bash
docker run --rm kicknx-switch-builder dkp-pacman -Q switch-libmpv
```

It should report `switch-libmpv 0.39.0-4`.

Then build kickNX. The repository is mounted into the container, so the output
lands in your checkout.

PowerShell:

```powershell
docker run --rm -v "${PWD}:/workspace" -w /workspace kicknx-switch-builder bash build-switch.sh
```

Linux, macOS or Git Bash:

```bash
docker run --rm -v "$(pwd):/workspace" -w /workspace kicknx-switch-builder bash build-switch.sh
```

In Git Bash on Windows, prefix the command with `MSYS_NO_PATHCONV=1` so Git
Bash does not rewrite `/workspace` into a Windows path.

## 3. Build kickNX without Docker

Install a current devkitPro Nintendo Switch environment with `devkitA64`,
`libnx`, CMake, Ninja, pkg-config and the packages the `Dockerfile` installs
(`switch-libmpv`, `switch-curl`, `switch-libwebp`, `switch-glfw`, `switch-mesa`
and the others listed there). Borealis and LunaSVG source are included in the
repository.

Windows PowerShell:

```powershell
.\build-switch.ps1
```

To use an FFmpeg package extracted elsewhere:

```powershell
.\build-switch.ps1 -FfmpegRoot "C:\path\to\switch-portlibs"
```

Linux or another POSIX shell:

```bash
./build-switch.sh
```

Or with a custom dependency root:

```bash
KICKNX_FFMPEG_ROOT=/path/to/switch-portlibs ./build-switch.sh
```

## Output

```text
build_switch/kickNX.nro
build_switch/KickNX.elf
```

Keep each NRO together with its matching ELF. Atmosphere crash addresses can
only be symbolicated accurately with the ELF from the exact same build.

## Dependency warning

When no custom FFmpeg root is available, CMake uses the FFmpeg libraries
reported by the installed `mpv.pc`. The project may compile, but that build does
not include the TLS and NVTEGRA guards validated for kickNX 0.5.0.

## Optional: patched MPV 0.36 recipe

`scripts/switch/mpv` holds an older MPV 0.36.0-7 recipe with an extra guard
for frame side data. kickNX 0.5.0 is not validated with it; it is kept for
reference. To try it, in devkitPro MSYS2:

```bash
cd scripts/switch/mpv
rm -rf src pkg build
makepkg -Csf --noconfirm
pacman -U --noconfirm switch-libmpv-0.36.0-7-any.pkg.tar.zst
```
