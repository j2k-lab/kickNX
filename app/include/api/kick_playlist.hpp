#pragma once

#include <string>
#include <vector>

namespace kick {

struct Quality {
    std::string id;
    std::string name;
    std::string url;
};

struct SubtitleTrack {
    std::string name;
    std::string language;
    std::string url;
    bool isDefault = false;
};

std::vector<Quality> parseMasterPlaylist(
    const std::string& masterUrl,
    const std::string& playlistText);

std::vector<SubtitleTrack> parseMasterSubtitles(
    const std::string& masterUrl,
    const std::string& playlistText);

Quality selectQuality(
    const std::vector<Quality>& qualities,
    const std::string& preferred = "source");

std::string percentEncode(const std::string& value);
std::string normalizeChannel(std::string value);

}  // namespace kick
