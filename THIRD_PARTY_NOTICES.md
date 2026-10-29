# Third-party notices

This file summarizes the principal upstream projects and third-party libraries used by kickNX. Copyright headers and license files inside third-party source directories remain authoritative.

## twiNX

- Upstream: `DomazinUS/twiNX`
- Role: Direct project foundation; provided the initial Nintendo Switch client structure, MPV/Borealis video playback pipeline, and controller-first navigation.
- License: GNU General Public License version 3 (GPL-3.0-only)

kickNX is a direct fork of twiNX, adapted and rewritten specifically for Kick streaming, Centrifugo/Pusher real-time chat, and modern Switch homebrew.

## Switchfin

- Upstream: `dragonflylee/switchfin`
- Role: Original Nintendo Switch platform foundation, Borealis UI architecture, and MPV wrapper base inherited through twiNX.
- License: Apache License 2.0

A copy of Apache-2.0 is stored at `LICENSES/Apache-2.0.txt`. Existing upstream copyright and license headers have been preserved.

## Borealis

- Upstream: `dragonflylee/borealis`
- Vendored commit: `7f03684805befffb1be18a1b2bdb81fa66945af2`
- Role: Controller-first, hardware-accelerated user-interface framework (NanoVG / OpenGL).
- License: Apache License 2.0

The upstream license and NOTICE remain in `library/borealis/`; the NOTICE is also preserved at `LICENSES/NOTICE-Borealis.txt`.

## QR Code generator (Nayuki)

- Upstream: Project Nayuki (`https://www.nayuki.io/page/qr-code-generator-library`)
- Role: QR code generation used for the Kick TV fast-login pairing flow (`kick.com/tv/login`).
- License: MIT License

The copyright notice and MIT license are preserved in `app/src/utils/qrcodegen.cpp` and `app/include/utils/qrcodegen.hpp`.

## LunaSVG

- Upstream: `sammycage/lunasvg`
- Vendored commit: `f924651b85cac47dbe15f51a4aa320461fc1d07b`
- Role: SVG rendering for icons and UI elements.
- License: MIT License

The upstream license remains in `library/lunasvg/LICENSE` and is copied to `LICENSES/MIT-LunaSVG.txt`.

## mpv

- Role: Media playback engine powering video streaming, live caching, and audio decoding.
- Upstream license: GPL version 2 or later; LGPL builds are possible only when configured without GPL-only files.

The Nintendo Switch build links against devkitPro's `switch-libmpv`. The combined kickNX distribution is provided under GPL-3.0-only.

## FFmpeg

- Recipe: FFmpeg 7.1 with Nintendo Switch patches (`switch-ffmpeg`)
- Role: Video/audio demuxing, decoding, color conversion, and hardware NVTEGRA decoding.
- Build configuration: `--enable-gpl`

The reproducible package recipe and hardware decoding patch scripts are maintained in `scripts/switch/ffmpeg-libnx/`. GPL-2.0 and GPL-3.0 license texts are included under `LICENSES/`.

## Other dependencies

kickNX also utilizes libcurl, libnx, devkitPro libraries, WebP, nlohmann/json, and additional components inherited from twiNX, Switchfin, and Borealis. Their source directories, package metadata, and upstream license notices remain applicable:
- **Material Design Icons:** `resources/material/LICENSE.txt` (Apache-2.0)
- **libcurl:** MIT/X derivate license
- **libnx:** ISC License

## Combined-project license

The root `LICENSE` contains the GNU General Public License version 3 (GPL-3.0-only). This licensing choice covers the combined kickNX distribution and original kickNX-specific code while preserving the more permissive licenses of inherited Apache-2.0 and MIT components.
