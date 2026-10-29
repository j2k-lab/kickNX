#pragma once

#include "api/kick_playlist.hpp"

#include <functional>
#include <string>
#include <vector>

namespace kick {

struct Config {
    std::string channel;
    std::string preferredQuality = "1080p60";
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
// experimental path. Hybrid starts in hardware, falls back to software for a
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

std::string mpvExtra();

}  // namespace kick
