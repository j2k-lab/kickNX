# Changelog

All notable changes to **kickNX** are documented in this file.

kickNX is an independent, controller-first Kick client for Nintendo Switch
homebrew, created by **j2k** with AI co-development by **Gemini** and **Claude**, based on **twiNX**.

The format is inspired by [Keep a Changelog](https://keepachangelog.com/).

---

## Notes
- **Subtitles Status**: Subtitle tracks in Player Settings remain inactive until Kick updates their Amazon IVS backend to serve live transcode tracks (Kick's web player currently relies on client-side browser speech recognition).
- **Backend Notice**: Kick may change or update their backend at any time, which may affect app functionality.

---

## [0.5.0]

### Added
- **Colour themes**: OLED (the default), Dark Grey and Kick. Choose one from the new **Theme** button on Home. The picker is a pop-up with a small preview of each theme, and the choice is saved and applied straight away.
- **Kick theme**: a muted Kick green (`#8BB06F`) page with light grey cards, a dark "K" monogram pattern behind the Home, About, theme picker and event headers, and the full pattern behind the sign-in screen. The focus ring pulses white and black so it shows everywhere.
- **Docked chat follows the theme**: the docked chat panel uses the active theme's colours. The chat overlay on top of the video keeps its dark look.
- **Live events**: a Live events row on Home lists Kick events that are live now. Each event opens a page with its banner, details, live streams and its most-viewed clips, which load more as you scroll.
- **In-app chat keyboard in landscape**: the landscape composer opens on an on-screen keyboard (with Caps Lock and symbols) instead of the system keyboard, with a green Emotes key to switch to the emote picker. Its keys are see-through so the stream stays visible. The Nintendo keyboard is still available under Player Settings → Message composer.
- **Stream title on the live OSD**: the top of the player shows the channel and stream title, like VODs and clips.

### Changed
- **Larger profile picture** on the Home screen.
- **Kick-only client**: the Jellyfin, media library, server setup, settings page, USB file browser, local music player, Apache/WebDAV remote sources, analytics and danmaku code have been removed. kickNX now starts directly on the Kick home screen.
- **Live playback**: "Auto" quality uses Kick's adaptive master playlist, and streams open with the same cache settings as twiNX.
- **Offline streams**: when Kick reports a channel has gone offline, the player shows a message and returns to the previous screen.

### Fixed
- **OSD focus**: pushing the left stick or D-pad no longer pulls focus away from the chat composer, and it still wakes the OSD when you are watching the stream.
- **Emote backspace**: one press removes a whole emote from the chat draft.
- **Crash after closing a stream quickly**: backing out of a stream, especially right after closing an overlay such as Tesla/Ultrahand, could leave focus on the deleted player's close button, and pressing A then crashed the app.
- **Sign-in, playback and refresh failures**: kickNX now limits how many secure connections it opens at once, so thumbnails can no longer use up the Switch's limit and make sign-in, playback URLs or Home sections fail with "SSL connect error".
- **Damaged settings file**: a corrupted `config.json` no longer makes kickNX quit at launch; it is kept as `config.json.bak` and default settings are used.

---

## [0.4.0]

### Added
- **Channel Follow & Unfollow**: Dedicated interactive Follow / Following button on streamer profile pages with real-time follow status detection, custom Kick green styling, toast notifications, guest sign-in prompts, automatic synchronization with followed channels, and automatic hiding when viewing your own channel profile.
- **Centered Streamer Page & Touch Back Navigation**: Centered the streamer avatar, channel name, live status, broadcast title, metadata, and action buttons in the profile header for a balanced presentation that fills the screen, along with a dedicated top-left `< Back` touch button for intuitive touchscreen and controller return navigation.
- **Portrait Emote Composer**: Added a dedicated `Emotes` key to the portrait keyboard that opens a portrait-tailored Kick emoji composer featuring Recent and All tabs, a responsive 6-column emote grid, and quick `< Keyboard` return navigation.
- **Universal Quality & Decoder Selection**: Enabled stream quality preferences and hardware/hybrid/software decoder selection across VODs and clips in Player Settings.
- **Clickable Profile Avatar**: The top-right profile picture is now an interactive, focusable icon. Clicking or tapping it immediately navigates to your own Kick channel page.
- **kickNX Buffering Indicator**: Replaced the generic progress spinner with a custom 3D blocky kickNX logo buffering indicator featuring a tight lime-green letter-shimmer sweep and sleek neon progress bar.
- **Kick Bright Green Section Styling**: Section divider lines and header accent tabs across all browsing, channel profile, and settings screens are now styled in signature Kick bright green (`#53FC18`).
- **OLED True Black Playback & Profiles**: Maintained clean OLED true black backgrounds across channel profiles, About, and video player loading surfaces, eliminating grey background flashes during stream and VOD buffering.
- **Home & About Brand Headers**: Replaced standard text titles with the high-resolution transparent 3D kickNX brand logo on the home screen and About page.
- **Profile Back Button**: Added a dedicated Kick green touch-and-controller `< Back` button on streamer profiles matching channel action buttons.
- **Dedicated Category Card Placeholder**: Introduced `category-card-bg.png` (`350x410`) matching the exact vertical portrait card aspect ratio (`175x205`), eliminating distortion and keeping the card aesthetic clean when thumbnails are missing or loading.
- **Seamless Live Stream Continuity**: Replaced aggressive recovery pause/buffer drops and intrusive toast popups with transparent FFmpeg HLS auto-reconnection (`reconnect=1,reconnect_streamed=1`), preventing random stream pauses and toast interruptions.


### Fixed
- **Chat status banner**: The "Connecting to Kick chat..." message no longer stays stuck at the top of the chat once connected or after messages arrive.
- **Scrub bar crash**: Fixed a crash when pressing B to exit a stream immediately after dragging the progress bar.
- **Popular live streams**: Streams in the popular, featured and category lists now show their viewer count instead of "OFFLINE".
- **Emote composer rotation**: The emote composer now closes when the display rotates instead of staying open over the portrait keyboard.
- **Home title**: "Kick for Nintendo Switch" no longer wraps onto a second line after returning home quickly.

### Changed
- **OLED black chat**: The docked and portrait chat panels now use a true OLED black background.
- **Simplified playback settings**: Removed options that had no effect on Kick playback (show bottom progress bar, show OSD when paused, video flip, rotation, aspect ratio) and the empty Common section. Aspect ratio is always automatic.

---

## [0.3.2]

### Added
- **Kick Lime Green Playback Bar & Live Uptime**: Authentic Kick green (`#53FC18`) scrubber with real-time stream uptime pinned to the live edge.
- **OLED True Black Theme**: True black home screen background for OLED Switch displays with high-contrast header.
- **Fixed Clip Metadata**: Shows real clip titles, view counts, and creator usernames.
- **Channel Category Navigation**: Clicking a category on a streamer's profile page returns home and immediately loads that category's live streams.
- **Bottom Sheet Emote Composer**: Redesigned the in-player KickNX emote composer as an interactive bottom drawer with rounded corners, translucent backdrop, and continuous real-time video playback visible behind the sheet.
- **Kick Logo Placeholder & WebP Thumbnail Fixes**: Replaced legacy Jellyfin card placeholder with a centered Kick logo; fixed missing thumbnails for clips and streamer profile categories with subcategory fallback caching.
- **User Profile Avatar**: Restored top-right profile avatar display for signed-in users with public channel fallback and instant session loading.

---

## [0.3.1]

### Added
- **Fast VOD & Clip Loading**: Instant transition into player view with dedicated loading indicator and notice.
- **Active Chatter Counter**: Displays real-time live chatter count in chat header with neon green highlight.
- **Subtitles Support**: Multi-language WebVTT subtitle track selection in Player Settings (inactive pending Kick IVS backend update).

---

## [0.3.0]

### Added
- Official Kick TV QR code & 6-digit link authentication flow (`kick.com/tv/login`) with automatic authorization polling via the Kick mobile app.
- NanoVG-rendered QR code component (`QRCodeView`) with auto-scaling and visual centering.
- 60 FPS video playback presentation with 60Hz display timing alignment (`override-display-fps = 60`, `video-sync = audio`, `framedrop = vo`).
- Direct single-variant VOD resolution in `resolveVod`, downloading only the preferred quality rendition and eliminating multi-hour manifest download stalls.
- Updated Kick stream quality ladders across player settings and stream details to match Kick's exact native resolutions (`1080p60`, `720p60`, `480p`, `360p`, `160p`, `Auto`).
- Custom FFmpeg 7.1 build with native libnx Horizon TLS/HTTPS support (`sslInitialize`) and NVTEGRA transfer guards.
- URL query parameter preservation in `absoluteUrl` so signed AWS IVS and CloudFront authentication tokens carry through to variant playlists and media segments.
- Optimized demuxer probe parameters (128 KB probesize, 1s analyze duration, `http_persistent=1` keep-alive connections).

### Changed
- Official Kick mobile app QR code pairing authentication.
- Removed legacy `opengl-glfinish` blocking calls that stalled the GPU on Mesa/Nouveau and capped playback to 30fps.
- Replaced per-file options in `loadfile` with global player options in `MPVCore::init()`.

### Fixed
- Fixed resolution selection bug where manually picking 720p60 or other qualities remained stuck on 1080p60 due to `resolveLive` overwriting the selected rendition with the root multivariant playlist.
- Fixed VOD playback taking minutes to load by avoiding sequential parsing of all variant playlists.
- Fixed choppy 30fps playback by removing `glFinish` stalls and setting 60Hz presentation timing.
- Fixed "stream: No protocol handler found to open URL https://..." playback errors by providing custom `libavformat` HTTPS protocol support with Horizon OS SSL.
- Fixed live stream recovery and playback loops caused by broken `loadfile` syntax.

---

## [0.2.0]

### Added
- Real-time live chat powered by Pusher WebSockets (`wss://ws-us2.pusher.com`).
- Kick chat emote sheet and on-screen message composer.
- Channel details pages with past broadcasts (VODs), clips, categories, and streamer info.
- Followed channels row for live streams on the home dashboard.
- Live broadcaster and category search with real-time viewer count indicators.

---

## [0.1.0]

### Added
- Initial native Nintendo Switch port for Kick.com based on twiNX.
- Kick v1/v2 REST API resolvers for browsing, stream discovery, and account sessions.
- Guest browsing mode without requiring an account.
- Controller-first Borealis interface, Joy-Con motion portrait mode, and touch controls.

---

## Project credits

- **kickNX** was created and developed by **j2k** with AI co-development by **Gemini** and **Claude**, based on **twiNX**.
- Built using open-source technologies including Borealis, MPV, FFmpeg, libcurl, and libnx.
- Upstream foundations: twiNX, Switchfin, and Borealis.

kickNX is an independent project and is not affiliated with, endorsed by or sponsored by Kick, Nintendo, or Amazon.
