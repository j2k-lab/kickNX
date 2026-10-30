#pragma once

#include "api/kick_playlist.hpp"

#include <functional>
#include <string>
#include <vector>

namespace kick {

struct Config {
    std::string channel;
    std::string preferredQuality = "auto";
    std::string oauthClientId = "kicknx-switch-client";
};

struct Resolution {
    std::string channel;
    std::string masterUrl;
    std::vector<Quality> qualities;
    Quality selected;
    std::vector<SubtitleTrack> subtitles;
};

using ResolveCallback = std::function<void(Resolution)>;
using ErrorCallback = std::function<void(const std::string&)>;

Config loadConfig();
std::string loadPreferredQuality();
bool savePreferredQuality(const std::string& quality);

enum class DecoderMode {
    Software = 0,
    Hardware = 1,
    Hybrid = 2,
};

// Software decoding remains the safest mode. Hardware is the original
// path. Hybrid starts in hardware, falls back to software for a
// Kick presentation transition, and only restores hardware after a second
// transition has completed and playback has remained stable.
DecoderMode loadDecoderMode();
bool saveDecoderMode(DecoderMode mode);
bool decoderUsesHardware(DecoderMode mode);

Resolution resolveLive(const Config& config);
void resolveLiveAsync(
    const Config& config,
    ResolveCallback success,
    ErrorCallback failure);

// Asks Kick whether a channel currently has a livestream. `known` is false
// when the answer could not be determined (network error, unexpected reply),
// so callers never treat a failed lookup as "offline".
using LiveCheckCallback = std::function<void(bool known, bool live)>;
void checkChannelLiveAsync(const std::string& channel, LiveCheckCallback done);

// Per-file mpv options for Kick live playback, mirroring twiNX's Twitch setup.
std::string mpvExtra();

// Live caption tracks advertised by the current stream's master playlist.
// They are loaded into mpv only when the viewer picks one, because each
// caption track is a separate live HLS feed that competes with the video.
void setLiveSubtitles(std::vector<SubtitleTrack> tracks);
const std::vector<SubtitleTrack>& liveSubtitles();
// Language of the caption the viewer chose ("" = captions off).
void setLiveCaptionLanguage(const std::string& language);
const std::string& liveCaptionLanguage();

}  // namespace kick
