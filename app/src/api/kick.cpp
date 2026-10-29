#include "api/kick.hpp"

#include "api/http.hpp"
#include "utils/debug_log.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>

namespace kick {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

void removeLegacyConfigFiles() {
#if defined(__SWITCH__)
    static constexpr const char* LEGACY_PATHS[] = {
        "sdmc:/switch/kicknx.txt",
        "sdmc:/switch/kickNX/kicknx.txt",
        "sdmc:/config/KickNX/kicknx.txt",
    };

    for (const char* path : LEGACY_PATHS) {
        std::error_code error;
        const bool removed = fs::remove(path, error);
        if (error) {
            brls::Logger::warning(
                "Unable to remove obsolete kickNX config {}: {}",
                path,
                error.message());
        } else if (removed) {
            brls::Logger::info("Removed obsolete kickNX config: {}", path);
        }
    }
#endif
}

std::string settingsPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/KickNX/settings.json";
#else
    return "settings.json";
#endif
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

void applyConfigLine(Config& config, const std::string& rawLine) {
    std::string line = trim(rawLine);
    if (line.empty() || line.front() == '#' || line.front() == ';') return;

    const auto equals = line.find('=');
    if (equals == std::string::npos) {
        if (config.channel.empty()) config.channel = normalizeChannel(line);
        return;
    }

    std::string key = trim(line.substr(0, equals));
    std::string value = trim(line.substr(equals + 1));
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (key == "channel") config.channel = normalizeChannel(value);
    else if (key == "preferred_quality" || key == "quality") config.preferredQuality = value;
    else if (key == "oauth_client_id") config.oauthClientId = value;
}

HTTP::Header kickPlaybackHeaders() {
    return {
        "Accept: application/json",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };
}

std::string extractPlaybackUrlFromJson(const json& root) {
    if (root.is_string()) {
        return trim(root.get<std::string>());
    }
    if (!root.is_object()) return {};

    for (const char* key : {"playback_url", "playbackUrl", "stream_url", "source"}) {
        if (root.contains(key)) {
            const auto& val = root[key];
            if (val.is_string()) {
                const std::string s = trim(val.get<std::string>());
                if (!s.empty()) return s;
            } else if (val.is_object() && val.contains("live") && val["live"].is_string()) {
                const std::string s = trim(val["live"].get<std::string>());
                if (!s.empty()) return s;
            }
        }
    }

    if (root.contains("data")) {
        const auto& data = root["data"];
        if (data.is_string()) {
            const std::string s = trim(data.get<std::string>());
            if (!s.empty()) return s;
        } else if (data.is_object()) {
            for (const char* key : {"playback_url", "playbackUrl", "stream_url", "source", "live"}) {
                if (data.contains(key)) {
                    const auto& val = data[key];
                    if (val.is_string()) {
                        const std::string s = trim(val.get<std::string>());
                        if (!s.empty()) return s;
                    } else if (val.is_object() && val.contains("live") && val["live"].is_string()) {
                        const std::string s = trim(val["live"].get<std::string>());
                        if (!s.empty()) return s;
                    }
                }
            }
        }
    }

    if (root.contains("livestream") && root["livestream"].is_object()) {
        const auto& ls = root["livestream"];
        for (const char* key : {"playback_url", "playbackUrl", "stream_url", "source"}) {
            if (ls.contains(key)) {
                const auto& val = ls[key];
                if (val.is_string()) {
                    const std::string s = trim(val.get<std::string>());
                    if (!s.empty()) return s;
                } else if (val.is_object() && val.contains("live") && val["live"].is_string()) {
                    const std::string s = trim(val["live"].get<std::string>());
                    if (!s.empty()) return s;
                }
            }
        }
    }

    return {};
}

std::string fetchKickPlaybackUrl(const Config& config) {
    const std::string encodedSlug = percentEncode(config.channel);
    const auto headers = kickPlaybackHeaders();

    // Primary endpoint discovered via REA (GET https://kick.com/api/v2/channels/{slug}/playback-url)
    try {
        const std::string response = HTTP::get(
            "https://kick.com/api/v2/channels/" + encodedSlug + "/playback-url",
            headers,
            HTTP::Timeout{15000});
        const json root = json::parse(response);
        const std::string url = extractPlaybackUrlFromJson(root);
        if (!url.empty()) return url;
    } catch (const std::exception& error) {
        brls::Logger::warning("Kick v2 playback-url primary request failed for {}: {}", config.channel, error.what());
    }

    // Fallback: GET https://kick.com/api/v2/channels/{slug}
    std::string fallbackVideoId;
    try {
        const std::string response = HTTP::get(
            "https://kick.com/api/v2/channels/" + encodedSlug,
            headers,
            HTTP::Timeout{15000});
        const json root = json::parse(response);
        const std::string url = extractPlaybackUrlFromJson(root);
        if (!url.empty()) return url;

        if (root.is_object() && root.contains("livestream") && root["livestream"].is_object()) {
            const auto& ls = root["livestream"];
            if (ls.contains("vod_id") && ls["vod_id"].is_string()) {
                fallbackVideoId = ls["vod_id"].get<std::string>();
            } else if (ls.contains("id")) {
                if (ls["id"].is_string()) fallbackVideoId = ls["id"].get<std::string>();
                else if (ls["id"].is_number_integer()) fallbackVideoId = std::to_string(ls["id"].get<long long>());
            }
        }
    } catch (const std::exception& error) {
        brls::Logger::warning("Kick v2 channel fallback failed for {}: {}", config.channel, error.what());
    }

    // Secondary fallback: POST https://web.kick.com/api/v1/stream/{videoId}/playback
    if (!fallbackVideoId.empty()) {
        try {
            HTTP::Header postHeaders = headers;
            postHeaders.push_back("Content-Type: application/json");
            const std::string response = HTTP::post(
                "https://web.kick.com/api/v1/stream/" + percentEncode(fallbackVideoId) + "/playback",
                "{\"token\":\"\"}",
                postHeaders,
                HTTP::Timeout{15000});
            const json root = json::parse(response);
            const std::string url = extractPlaybackUrlFromJson(root);
            if (!url.empty()) return url;
        } catch (const std::exception& error) {
            brls::Logger::warning("Kick v1 stream playback fallback failed: {}", error.what());
        }
    }

    throw std::runtime_error(
        "The Kick channel is offline, unavailable, or returned no live playback URL");
}

std::string buildMasterUrl(const std::string& rawPlaybackUrl) {
    if (rawPlaybackUrl.find("player_version=") != std::string::npos) {
        return rawPlaybackUrl;
    }
    const char separator = rawPlaybackUrl.find('?') == std::string::npos ? '?' : '&';
    return rawPlaybackUrl + separator +
        "platform=web"
        "&player_backend=mediaplayer"
        "&player_version=1.56.2"
        "&supported_codecs=avc1,h264";
}

std::string getMasterPlaylist(const std::string& url) {
    HTTP::Header headers = {
        "Referer: https://kick.com/",
        "Origin: https://kick.com",
        "Accept: application/vnd.apple.mpegurl, application/x-mpegURL, */*",
    };
    return HTTP::get(url, headers, HTTP::Timeout{15000});
}

}  // namespace

std::string loadPreferredQuality() {
    std::ifstream input(settingsPath());
    if (!input.is_open()) return "1080p60";

    try {
        const json root = json::parse(input);
        if (!root.is_object()) return "1080p60";
        const std::string quality = trim(root.value("preferred_quality", ""));
        return quality.empty() ? "1080p60" : quality;
    } catch (...) {
        return "1080p60";
    }
}

bool savePreferredQuality(const std::string& quality) {
    try {
        const fs::path path(settingsPath());
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());

        json root = json::object();
        {
            std::ifstream input(path);
            if (input.is_open()) {
                try {
                    const json existing = json::parse(input);
                    if (existing.is_object()) root = existing;
                } catch (...) {
                }
            }
        }

        root["preferred_quality"] = quality;
        const fs::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output.is_open()) return false;
            output << root.dump(2) << '\n';
            if (!output.good()) return false;
        }

        std::error_code error;
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

DecoderMode loadDecoderMode() {
    std::ifstream input(settingsPath());
    if (!input.is_open()) return DecoderMode::Software;

    try {
        const json root = json::parse(input);
        if (!root.is_object()) return DecoderMode::Software;

        if (root.contains("kick_decoder_mode")) {
            const auto& stored = root.at("kick_decoder_mode");
            if (stored.is_number_integer()) {
                const int value = stored.get<int>();
                if (value >= static_cast<int>(DecoderMode::Software) &&
                    value <= static_cast<int>(DecoderMode::Hybrid))
                    return static_cast<DecoderMode>(value);
            } else if (stored.is_string()) {
                std::string value = stored.get<std::string>();
                std::transform(value.begin(), value.end(), value.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (value == "hardware") return DecoderMode::Hardware;
                if (value == "hybrid") return DecoderMode::Hybrid;
                return DecoderMode::Software;
            }
        }

        // Migrate the former two-state preference without changing an existing
        // user's selection: true remains Hardware, false remains Software.
        if (root.contains("kick_hardware_decode")) {
            return root.value("kick_hardware_decode", false)
                ? DecoderMode::Hardware
                : DecoderMode::Software;
        }
        return DecoderMode::Software;
    } catch (...) {
        return DecoderMode::Software;
    }
}

bool decoderUsesHardware(DecoderMode mode) {
    return mode == DecoderMode::Hardware || mode == DecoderMode::Hybrid;
}

bool saveDecoderMode(DecoderMode mode) {
    try {
        const fs::path path(settingsPath());
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());

        json root = json::object();
        {
            std::ifstream input(path);
            if (input.is_open()) {
                try {
                    const json existing = json::parse(input);
                    if (existing.is_object()) root = existing;
                } catch (...) {
                }
            }
        }

        root["kick_decoder_mode"] = static_cast<int>(mode);
        // Keep the legacy key coherent for older builds that read the same SD
        // card. Hybrid is hardware-capable, so it maps to true there.
        root["kick_hardware_decode"] = decoderUsesHardware(mode);

        const fs::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output.is_open()) return false;
            output << root.dump(2) << '\n';
            if (!output.good()) return false;
        }

        std::error_code error;
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

Config loadConfig() {
    Config config;
#if defined(__SWITCH__)
    static std::once_flag legacyCleanup;
    std::call_once(legacyCleanup, removeLegacyConfigFiles);
#else
    const std::vector<std::string> candidates = {
        "kicknx.txt",
    };

    for (const auto& path : candidates) {
        std::ifstream input(path);
        if (!input.is_open()) continue;
        for (std::string line; std::getline(input, line);) applyConfigLine(config, line);
        if (!config.channel.empty()) break;
    }
#endif
    const std::string savedQuality = loadPreferredQuality();
    if (!savedQuality.empty()) config.preferredQuality = savedQuality;
    config.channel = normalizeChannel(config.channel);
    return config;
}

Resolution resolveLive(const Config& config) {
    Config clean = config;
    clean.channel = normalizeChannel(clean.channel);
    if (clean.channel.empty())
        throw std::runtime_error("No Kick channel was selected");

    const std::string rawPlaybackUrl = fetchKickPlaybackUrl(clean);
    std::string masterUrl = rawPlaybackUrl;
    std::string playlist;
    try {
        playlist = getMasterPlaylist(masterUrl);
    } catch (...) {
        masterUrl = buildMasterUrl(rawPlaybackUrl);
        playlist = getMasterPlaylist(masterUrl);
    }

    auto qualities = parseMasterPlaylist(masterUrl, playlist);
    if (qualities.size() <= 1) {
        qualities.push_back({"source", "Source", masterUrl});
    }

    Resolution result;
    result.channel = clean.channel;
    result.masterUrl = masterUrl;
    result.selected = selectQuality(qualities, clean.preferredQuality);
    // If the user selected "auto", use the multivariant master playlist so MPV
    // can dynamically adapt between renditions. Otherwise, use the resolved
    // variant URL (which preserves the signed ?token= query parameters) so MPV
    // streams exclusively at the requested resolution.
    if (result.selected.id == "auto" || result.selected.url.empty()) {
        result.selected.url = masterUrl;
    }
    result.qualities = std::move(qualities);
    result.subtitles = parseMasterSubtitles(masterUrl, playlist);
#if defined(KICKNX_PLAYBACK_DEBUG)
    kicknx::debug::log(
        "TW_SUBTITLE",
        "channel=%s master_subtitles_count=%zu",
        result.channel.c_str(),
        result.subtitles.size());
#endif
    return result;
}

void resolveLiveAsync(
    const Config& config,
    ResolveCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [config, success = std::move(success), failure = std::move(failure)](HTTP&) mutable {
            try {
                auto result = resolveLive(config);
                brls::sync(std::bind(std::move(success), std::move(result)));
            } catch (const std::exception& exception) {
                const std::string message = exception.what();
                brls::sync(std::bind(std::move(failure), message));
            } catch (...) {
                brls::sync(std::bind(
                    std::move(failure),
                    std::string("Unknown Kick resolver error")));
            }
        });
}

std::string mpvExtra() {
    // All required HTTP headers (Referer, User-Agent, tls-verify, ca-file, timeout)
    // are already configured globally in MPVCore::init(). Passing raw option strings
    // with unquoted spaces/symbols into loadfile breaks MPV's per-file option parser.
    return "";
}

}  // namespace kick
