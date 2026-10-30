#include "api/kick_helix.hpp"

#include "api/http.hpp"
#include "api/kick.hpp"
#include "api/kick_playlist.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kick {
namespace {

using json = nlohmann::json;

void replaceAll(std::string& value, const std::string& from, const std::string& to) {
    size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

std::string jsonString(const json& obj, const char* key, const std::string& fallback = "") {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& val = obj[key];
    if (val.is_string()) return val.get<std::string>();
    if (val.is_number_integer()) return std::to_string(val.get<long long>());
    if (val.is_number_unsigned()) return std::to_string(val.get<unsigned long long>());
    return fallback;
}

int jsonInt(const json& obj, const char* key, int fallback = 0) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& val = obj[key];
    if (val.is_number_integer()) return static_cast<int>(val.get<long long>());
    if (val.is_number_unsigned()) return static_cast<int>(val.get<unsigned long long>());
    if (val.is_number_float()) return static_cast<int>(val.get<double>());
    if (val.is_string()) {
        try {
            return std::stoi(val.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

float jsonFloat(const json& obj, const char* key, float fallback = 0.0f) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& val = obj[key];
    if (val.is_number()) return static_cast<float>(val.get<double>());
    if (val.is_string()) {
        try {
            return std::stof(val.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

bool jsonBool(const json& obj, const char* key, bool fallback = false) {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& val = obj[key];
    if (val.is_boolean()) return val.get<bool>();
    if (val.is_number_integer()) return val.get<long long>() != 0;
    return fallback;
}

bool isValidImageUrl(const std::string& url) {
    if (url.empty()) return false;
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 || url.rfind("//", 0) == 0) {
        return url.find(".svg") == std::string::npos;
    }
    return false;
}

std::string jsonImageUrl(const json& val) {
    if (val.is_string()) {
        std::string s = val.get<std::string>();
        if (isValidImageUrl(s)) return s;
        return {};
    }
    if (val.is_object()) {
        for (const char* key : {"src", "url", "responsive", "srcset"}) {
            if (val.contains(key) && val[key].is_string()) {
                std::string candidate = val[key].get<std::string>();
                const auto space = candidate.find(' ');
                if (space != std::string::npos && (std::string(key) == "srcset" || std::string(key) == "responsive")) {
                    candidate.erase(space);
                }
                if (isValidImageUrl(candidate)) return candidate;
            }
        }
    }
    return {};
}

std::string formatDurationMs(long long durationValue) {
    if (durationValue <= 0) return {};
    // Kick VOD durations in /api/v2/channels/{slug}/videos are in milliseconds
    // (or seconds if small). Normalize to total seconds.
    long long totalSeconds = durationValue > 86400 ? (durationValue / 1000) : durationValue;
    const long long hours = totalSeconds / 3600;
    const long long minutes = (totalSeconds % 3600) / 60;
    const long long seconds = totalSeconds % 60;
    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, seconds);
    }
    return std::string(buffer);
}

HTTP::Header kickHeaders(const Config&, const OAuthSession& session) {
    HTTP::Header headers = {
        "Accept: application/json",
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };
    if (!session.accessToken.empty()) {
        headers.push_back("Authorization: Bearer " + session.accessToken);
        headers.push_back("Cookie: session_token=" + session.accessToken);
    }
    return headers;
}

void throwIfCancelled(const HTTP::Cancel& cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Request cancelled");
}

std::string kickGet(
    const Config& config,
    OAuthSession& session,
    const std::string& url,
    const HTTP::Cancel& cancel) {
    throwIfCancelled(cancel);
    if (cancel) {
        return HTTP::get(url, kickHeaders(config, session), HTTP::Timeout{15000}, cancel);
    }
    return HTTP::get(url, kickHeaders(config, session), HTTP::Timeout{15000});
}

int parsePageNumber(const std::string& cursor) {
    if (cursor.empty()) return 1;
    try {
        const int page = std::stoi(cursor);
        return page > 0 ? page : 1;
    } catch (...) {
        return 1;
    }
}

int64_t parseUptimeSeconds(const std::string& createdAtStr, int64_t durationValue = 0) {
    if (durationValue > 0) {
        return durationValue > 86400 ? (durationValue / 1000) : durationValue;
    }
    if (createdAtStr.empty()) return 0;
    std::tm t = {};
    std::istringstream ss(createdAtStr);
    ss >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) {
        ss.clear();
        ss.str(createdAtStr);
        ss >> std::get_time(&t, "%Y-%m-%d %H:%M:%S");
    }
    if (!ss.fail()) {
        std::time_t tt = std::time(nullptr);
        std::time_t gt = std::mktime(std::gmtime(&tt));
        int64_t diff = int64_t(std::difftime(gt, std::mktime(&t)));
        if (diff > 0) return diff;
    }
    return 0;
}

UserProfile parseKickChannelProfile(const json& root, const std::string& fallbackSlug = "") {
    const json& item = (root.contains("data") && root["data"].is_object()) ? root["data"] : root;
    UserProfile profile;
    profile.id = jsonString(item, "id");
    profile.login = jsonString(item, "slug", fallbackSlug);
    if (profile.id.empty()) profile.id = profile.login;

    if (item.contains("user") && item["user"].is_object()) {
        const auto& user = item["user"];
        profile.displayName = jsonString(user, "username", profile.login);
        profile.description = jsonString(user, "bio");
        profile.profileImageUrl = jsonImageUrl(user.value("profile_pic", json()));
        if (profile.profileImageUrl.empty()) {
            profile.profileImageUrl = jsonImageUrl(user.value("profilepic", json()));
        }
    } else {
        profile.displayName = jsonString(item, "username", jsonString(item, "user_username", profile.login));
        profile.description = jsonString(item, "bio");
        profile.profileImageUrl = jsonImageUrl(item.value("profile_picture", json()));
        if (profile.profileImageUrl.empty()) {
            profile.profileImageUrl = jsonImageUrl(item.value("profile_pic", json()));
        }
    }

    if (item.contains("banner_image")) {
        profile.offlineImageUrl = jsonImageUrl(item["banner_image"]);
    }
    if (profile.offlineImageUrl.empty() && item.contains("offline_banner_image")) {
        profile.offlineImageUrl = jsonImageUrl(item["offline_banner_image"]);
    }
    profile.broadcasterType = jsonBool(item, "verified") ? "verified" : "";

    const json* lsObj = nullptr;
    if (root.contains("livestream") && root["livestream"].is_object()) {
        lsObj = &root["livestream"];
    } else if (item.contains("livestream") && item["livestream"].is_object()) {
        lsObj = &item["livestream"];
    }
    if (lsObj) {
        profile.uptimeSeconds = parseUptimeSeconds(
            jsonString(*lsObj, "created_at", jsonString(*lsObj, "start_time")),
            jsonInt(*lsObj, "duration", 0));
    }

    return profile;
}

static std::unordered_map<std::string, std::string> s_categoryBannerCache;
static std::mutex s_categoryBannerMutex;
static std::unordered_set<std::string> s_followedChannelsCache;
static std::mutex s_followedChannelsMutex;
static std::atomic<bool> s_followedChannelsDirty{false};

static std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static bool stringsEqualIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static void cacheCategoryBanner(const std::string& key, const std::string& url) {
    if (key.empty() || url.empty()) return;
    std::lock_guard<std::mutex> lock(s_categoryBannerMutex);
    s_categoryBannerCache[key] = url;
    s_categoryBannerCache[toLowerAscii(key)] = url;
}

static std::string getCachedCategoryBanner(const std::string& key) {
    if (key.empty()) return {};
    std::lock_guard<std::mutex> lock(s_categoryBannerMutex);
    auto it = s_categoryBannerCache.find(key);
    if (it != s_categoryBannerCache.end() && !it->second.empty()) return it->second;
    const std::string lower = toLowerAscii(key);
    it = s_categoryBannerCache.find(lower);
    if (it != s_categoryBannerCache.end() && !it->second.empty()) return it->second;
    return {};
}

static std::string slugifyCategoryName(const std::string& name) {
    std::string slug;
    slug.reserve(name.size());
    bool prevHyphen = false;
    for (char ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            prevHyphen = false;
        } else if (ch == ' ' || ch == '-' || ch == '_' || ch == '/' || ch == ':' || ch == '&') {
            if (!slug.empty() && !prevHyphen) {
                slug.push_back('-');
                prevHyphen = true;
            }
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug;
}

Category parseKickCategory(const json& item) {
    Category category;
    category.id = jsonString(item, "slug", jsonString(item, "id"));
    category.name = jsonString(item, "name");
    if (item.contains("banner")) {
        category.boxArtUrl = jsonImageUrl(item["banner"]);
    }
    if (category.boxArtUrl.empty() && item.contains("banner_image")) {
        category.boxArtUrl = jsonImageUrl(item["banner_image"]);
    }
    if (category.boxArtUrl.empty() && item.contains("banner_picture")) {
        category.boxArtUrl = jsonImageUrl(item["banner_picture"]);
    }
    if (category.boxArtUrl.empty() && item.contains("responsive")) {
        category.boxArtUrl = jsonImageUrl(item["responsive"]);
    }
    if (category.boxArtUrl.empty() && item.contains("category") && item["category"].is_object()) {
        category.boxArtUrl = jsonImageUrl(item["category"].value("banner", json()));
        if (category.boxArtUrl.empty()) {
            category.boxArtUrl = jsonImageUrl(item["category"].value("banner_image", json()));
        }
    }
    if (category.boxArtUrl.empty() && item.contains("sub_category") && item["sub_category"].is_object()) {
        category.boxArtUrl = jsonImageUrl(item["sub_category"].value("banner", json()));
    }
    if (category.boxArtUrl.empty() && item.contains("subcategory") && item["subcategory"].is_object()) {
        category.boxArtUrl = jsonImageUrl(item["subcategory"].value("banner", json()));
    }
    if (category.boxArtUrl.empty() && item.contains("icon")) {
        category.boxArtUrl = jsonImageUrl(item["icon"]);
    }

    const std::string rawSlug = jsonString(item, "slug");
    const std::string rawId = jsonString(item, "id");
    const std::string rawCatId = jsonString(item, "category_id");
    const std::string slugified = slugifyCategoryName(category.name);

    if (!category.boxArtUrl.empty()) {
        cacheCategoryBanner(category.id, category.boxArtUrl);
        cacheCategoryBanner(category.name, category.boxArtUrl);
        if (!rawSlug.empty()) cacheCategoryBanner(rawSlug, category.boxArtUrl);
        if (!rawId.empty()) cacheCategoryBanner(rawId, category.boxArtUrl);
        if (!rawCatId.empty()) cacheCategoryBanner(rawCatId, category.boxArtUrl);
        if (!slugified.empty()) cacheCategoryBanner(slugified, category.boxArtUrl);
    } else {
        std::string cached = getCachedCategoryBanner(category.id);
        if (cached.empty()) cached = getCachedCategoryBanner(category.name);
        if (cached.empty()) cached = getCachedCategoryBanner(rawSlug);
        if (cached.empty()) cached = getCachedCategoryBanner(rawId);
        if (cached.empty()) cached = getCachedCategoryBanner(rawCatId);
        if (cached.empty()) cached = getCachedCategoryBanner(slugified);
        if (!cached.empty()) category.boxArtUrl = cached;
    }
    return category;
}

Stream parseKickStream(const json& item) {
    Stream stream;
    stream.id = jsonString(item, "id", jsonString(item, "slug"));
    stream.title = jsonString(item, "session_title", jsonString(item, "title"));
    stream.language = jsonString(item, "language");
    stream.viewerCount = jsonInt(item, "viewers", jsonInt(item, "viewer_count", 0));
    stream.followersCount = jsonInt(item, "followersCount",
        jsonInt(item, "followers_count",
        jsonInt(item, "followers", 0)));
    stream.createdAt = jsonString(item, "created_at", jsonString(item, "start_time"));
    int64_t dur = jsonInt(item, "duration", 0);
    stream.uptimeSeconds = parseUptimeSeconds(stream.createdAt, dur);

    if (item.contains("livestream")) {
        if (item["livestream"].is_null()) {
            stream.isLive = false;
        } else if (item["livestream"].is_object()) {
            const auto& ls = item["livestream"];
            stream.isLive = jsonBool(ls, "is_live", true);
            const std::string sessionTitle = jsonString(ls, "session_title");
            if (!sessionTitle.empty()) stream.title = sessionTitle;
            stream.viewerCount = jsonInt(ls, "viewers", jsonInt(ls, "viewer_count", stream.viewerCount));
            if (stream.createdAt.empty()) {
                stream.createdAt = jsonString(ls, "created_at", jsonString(ls, "start_time"));
            }
            int64_t lsDur = jsonInt(ls, "duration", 0);
            if (lsDur > 0 || !stream.createdAt.empty()) {
                stream.uptimeSeconds = parseUptimeSeconds(stream.createdAt, lsDur);
            }
            if (ls.contains("thumbnail") && !jsonImageUrl(ls["thumbnail"]).empty()) {
                stream.thumbnailUrl = jsonImageUrl(ls["thumbnail"]);
            }
        } else {
            stream.isLive = false;
        }
    } else if (item.contains("channel") && item["channel"].is_object()) {
        const auto& ch = item["channel"];
        if (ch.contains("livestream")) {
            if (ch["livestream"].is_null()) {
                stream.isLive = false;
            } else if (ch["livestream"].is_object()) {
                const auto& ls = ch["livestream"];
                stream.isLive = jsonBool(ls, "is_live", true);
                const std::string sessionTitle = jsonString(ls, "session_title");
                if (!sessionTitle.empty()) stream.title = sessionTitle;
                stream.viewerCount = jsonInt(ls, "viewers", jsonInt(ls, "viewer_count", stream.viewerCount));
                if (stream.createdAt.empty()) {
                    stream.createdAt = jsonString(ls, "created_at", jsonString(ls, "start_time"));
                }
                int64_t lsDur = jsonInt(ls, "duration", 0);
                if (lsDur > 0 || !stream.createdAt.empty()) {
                    stream.uptimeSeconds = parseUptimeSeconds(stream.createdAt, lsDur);
                }
                if (ls.contains("thumbnail") && !jsonImageUrl(ls["thumbnail"]).empty()) {
                    stream.thumbnailUrl = jsonImageUrl(ls["thumbnail"]);
                }
            } else {
                stream.isLive = false;
            }
        } else if (ch.contains("is_live")) {
            stream.isLive = jsonBool(ch, "is_live", false);
        } else if (item.contains("is_live")) {
            stream.isLive = jsonBool(item, "is_live", false);
        } else {
            stream.isLive = false;
        }
    } else if (item.contains("is_live")) {
        stream.isLive = jsonBool(item, "is_live", false);
    } else {
        stream.isLive = false;
    }

    if (stream.title.empty()) {
        stream.title = jsonString(item, "description", stream.isLive ? "Live stream" : "Offline");
        if (stream.title.empty()) stream.title = stream.isLive ? "Live stream" : "Offline";
    }

    if (item.contains("thumbnail")) {
        stream.thumbnailUrl = jsonImageUrl(item["thumbnail"]);
    }
    if (stream.thumbnailUrl.empty()) {
        stream.thumbnailUrl = jsonString(item, "thumbnail_url");
    }

    if (item.contains("channel") && item["channel"].is_object()) {
        const auto& channel = item["channel"];
        if (stream.followersCount <= 0) {
            stream.followersCount = jsonInt(channel, "followersCount",
                jsonInt(channel, "followers_count",
                jsonInt(channel, "followers", 0)));
        }
        stream.userId = jsonString(channel, "id", jsonString(channel, "user_id"));
        stream.userLogin = jsonString(channel, "slug", jsonString(channel, "username"));
        if (channel.contains("user") && channel["user"].is_object()) {
            const auto& user = channel["user"];
            if (stream.followersCount <= 0) {
                stream.followersCount = jsonInt(user, "followersCount",
                    jsonInt(user, "followers_count",
                    jsonInt(user, "followers", 0)));
            }
            stream.userName = jsonString(user, "username", stream.userLogin);
            if (stream.thumbnailUrl.empty()) {
                stream.thumbnailUrl = jsonImageUrl(user.value("profile_pic", json()));
            }
        } else {
            stream.userName = jsonString(channel, "username", stream.userLogin);
            if (stream.thumbnailUrl.empty()) {
                stream.thumbnailUrl = jsonImageUrl(channel.value("profile_picture", json()));
            }
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(channel.value("banner_image", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(channel.value("banner_picture", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(channel.value("offline_banner_image", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(channel.value("profile_pic", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(channel.value("profile_picture", json()));
        }
    } else {
        stream.userLogin = jsonString(item, "channel_slug", jsonString(item, "slug"));
        stream.userId = jsonString(item, "channel_id", stream.userLogin);
        stream.userName = jsonString(
            item,
            "user_username",
            jsonString(item, "channel_name", jsonString(item, "username", stream.userLogin)));
        if (item.contains("user") && item["user"].is_object()) {
            const auto& user = item["user"];
            if (stream.userName.empty() || stream.userName == stream.userLogin) {
                stream.userName = jsonString(user, "username", stream.userName);
            }
            if (stream.followersCount <= 0) {
                stream.followersCount = jsonInt(user, "followersCount",
                    jsonInt(user, "followers_count",
                    jsonInt(user, "followers", 0)));
            }
            if (stream.thumbnailUrl.empty()) {
                stream.thumbnailUrl = jsonImageUrl(user.value("profile_pic", json()));
            }
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(item.value("banner_image", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(item.value("banner_picture", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(item.value("offline_banner_image", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(item.value("profile_picture", json()));
        }
        if (stream.thumbnailUrl.empty()) {
            stream.thumbnailUrl = jsonImageUrl(item.value("profile_pic", json()));
        }
    }

    if (stream.userName.empty()) stream.userName = stream.userLogin;
    if (stream.userId.empty()) stream.userId = stream.userLogin;
    if (stream.id.empty()) stream.id = stream.userId;

    if (item.contains("category") && item["category"].is_object()) {
        const auto& cat = item["category"];
        stream.gameId = jsonString(cat, "slug", jsonString(cat, "id"));
        stream.gameName = jsonString(cat, "name");
    } else if (item.contains("categories") && item["categories"].is_array() &&
               !item["categories"].empty() && item["categories"].front().is_object()) {
        const auto& cat = item["categories"].front();
        stream.gameId = jsonString(cat, "slug", jsonString(cat, "id"));
        stream.gameName = jsonString(cat, "name");
    } else {
        stream.gameName = jsonString(item, "category_name");
        stream.gameId = jsonString(item, "category_id", stream.gameName);
    }

    return stream;
}

json fetchKickChannelJson(
    const Config& config,
    OAuthSession& session,
    const std::string& login,
    const HTTP::Cancel& cancel) {
    const std::string slug = normalizeChannel(login);
    if (slug.empty()) throw std::runtime_error("Kick channel was not specified");

    try {
        return json::parse(kickGet(
            config,
            session,
            "https://kick.com/api/v2/channels/" + percentEncode(slug),
            cancel));
    } catch (const std::exception&) {
        throwIfCancelled(cancel);
        return json::parse(kickGet(
            config,
            session,
            "https://kick.com/api/v2/channels/" + percentEncode(slug) + "/info",
            cancel));
    }
}

UserProfile getUserByLogin(
    const Config& config,
    OAuthSession& session,
    const std::string& login,
    const HTTP::Cancel& cancel) {
    const std::string slug = normalizeChannel(login);
    const json root = fetchKickChannelJson(config, session, slug, cancel);
    UserProfile profile = parseKickChannelProfile(root, slug);
    if (profile.login.empty()) {
        throw std::runtime_error("Kick channel was not found");
    }
    return profile;
}

UserProfile getCurrentUser(
    const Config& config,
    OAuthSession& session,
    const HTTP::Cancel& cancel) {
    if (session.accessToken.empty() && session.login.empty()) return {};

    UserProfile profile;

    // 1. Try public channel profile if we know the user login/slug
    if (!session.login.empty()) {
        try {
            profile = getUserByLogin(config, session, session.login, cancel);
            if (!profile.profileImageUrl.empty()) {
                if (session.profileImageUrl != profile.profileImageUrl) {
                    session.profileImageUrl = profile.profileImageUrl;
                }
                return profile;
            }
        } catch (const std::exception& error) {
            brls::Logger::warning("kickNX: getUserByLogin for {} failed: {}", session.login, error.what());
        }
    }

    // 2. Try the authenticated user endpoint as fallback
    if (!session.accessToken.empty()) {
        try {
            const json root = json::parse(
                kickGet(config, session, "https://kick.com/api/v1/user", cancel));
            profile = parseKickChannelProfile(root, session.login);
            if (!profile.profileImageUrl.empty()) {
                session.profileImageUrl = profile.profileImageUrl;
                return profile;
            }
        } catch (const std::exception& error) {
            brls::Logger::warning("kickNX: /api/v1/user failed: {}", error.what());
        }
    }

    // 3. Fallback to cached profile image from OAuth session
    if (profile.profileImageUrl.empty() && !session.profileImageUrl.empty()) {
        profile.profileImageUrl = session.profileImageUrl;
    }
    if (profile.login.empty()) profile.login = session.login;
    if (profile.displayName.empty()) profile.displayName = session.displayName.empty() ? session.login : session.displayName;

    return profile;
}

void populateChannelInfoAndLiveStream(
    const json& root,
    const UserProfile& profile,
    ChannelInformation& info,
    Stream& liveStream,
    std::vector<Category>& categories) {
    const json& item = (root.contains("data") && root["data"].is_object()) ? root["data"] : root;

    info.broadcasterId = profile.id;
    info.broadcasterLogin = profile.login;
    info.broadcasterName = profile.displayName.empty() ? profile.login : profile.displayName;

    liveStream = Stream{};
    liveStream.userId = profile.id;
    liveStream.userLogin = profile.login;
    liveStream.userName = info.broadcasterName;
    liveStream.isLive = false;

    if (item.contains("livestream") && item["livestream"].is_object()) {
        const auto& ls = item["livestream"];
        liveStream = parseKickStream(ls);
        liveStream.userId = profile.id;
        liveStream.userLogin = profile.login;
        liveStream.userName = info.broadcasterName;
        liveStream.isLive = jsonBool(ls, "is_live", true);
        if (liveStream.thumbnailUrl.empty()) {
            liveStream.thumbnailUrl = profile.offlineImageUrl.empty()
                ? profile.profileImageUrl
                : profile.offlineImageUrl;
        }
        if (liveStream.uptimeSeconds <= 0) {
            liveStream.uptimeSeconds = parseUptimeSeconds(
                jsonString(ls, "created_at", jsonString(ls, "start_time")),
                jsonInt(ls, "duration", 0));
        }

        info.title = liveStream.title;
        info.broadcasterLanguage = liveStream.language;
        info.gameId = liveStream.gameId;
        info.gameName = liveStream.gameName;

        if (ls.contains("category") && ls["category"].is_object()) {
            Category cat = parseKickCategory(ls["category"]);
            if (!cat.name.empty()) {
                categories.push_back(std::move(cat));
            }
        }

        if (ls.contains("categories") && ls["categories"].is_array()) {
            for (const auto& catJson : ls["categories"]) {
                if (!catJson.is_object()) continue;
                Category cat = parseKickCategory(catJson);
                if (cat.name.empty()) continue;
                const auto dup = std::find_if(categories.begin(), categories.end(), [&](const Category& existing) {
                    if (!cat.id.empty() && !existing.id.empty() && stringsEqualIgnoreCase(existing.id, cat.id)) return true;
                    if (!cat.name.empty() && !existing.name.empty() && stringsEqualIgnoreCase(existing.name, cat.name)) return true;
                    return false;
                });
                if (dup == categories.end()) {
                    categories.push_back(std::move(cat));
                } else {
                    if (!isValidImageUrl(dup->boxArtUrl) && isValidImageUrl(cat.boxArtUrl)) {
                        dup->boxArtUrl = cat.boxArtUrl;
                    }
                    if (!cat.id.empty() && (dup->id.empty() || std::all_of(dup->id.begin(), dup->id.end(), ::isdigit))) {
                        dup->id = cat.id;
                    }
                }
            }
        }
        if (categories.empty() && !liveStream.gameName.empty()) {
            Category cat;
            cat.name = liveStream.gameName;
            cat.id = liveStream.gameId.empty() ? slugifyCategoryName(liveStream.gameName) : liveStream.gameId;
            cat.boxArtUrl = getCachedCategoryBanner(cat.name);
            if (cat.boxArtUrl.empty()) cat.boxArtUrl = getCachedCategoryBanner(cat.id);
            categories.push_back(std::move(cat));
        }
        if (ls.contains("tags") && ls["tags"].is_array()) {
            for (const auto& tag : ls["tags"]) {
                if (tag.is_string()) info.tags.push_back(tag.get<std::string>());
            }
        }
    }

    if (item.contains("recent_categories") && item["recent_categories"].is_array()) {
        for (const auto& catJson : item["recent_categories"]) {
            if (!catJson.is_object()) continue;
            Category cat = parseKickCategory(catJson);
            if (cat.name.empty()) continue;
            const auto dup = std::find_if(categories.begin(), categories.end(), [&](const Category& existing) {
                if (!cat.id.empty() && !existing.id.empty() && stringsEqualIgnoreCase(existing.id, cat.id)) return true;
                if (!cat.name.empty() && !existing.name.empty() && stringsEqualIgnoreCase(existing.name, cat.name)) return true;
                if (!cat.name.empty() && !existing.name.empty() && slugifyCategoryName(existing.name) == slugifyCategoryName(cat.name)) return true;
                return false;
            });
            if (dup == categories.end()) {
                categories.push_back(std::move(cat));
            } else {
                if (!isValidImageUrl(dup->boxArtUrl) && isValidImageUrl(cat.boxArtUrl)) {
                    dup->boxArtUrl = cat.boxArtUrl;
                }
                if (!cat.id.empty() && (dup->id.empty() || std::all_of(dup->id.begin(), dup->id.end(), ::isdigit))) {
                    dup->id = cat.id;
                }
            }
        }
    }

    if (info.gameName.empty() && !categories.empty()) {
        info.gameName = categories.front().name;
        info.gameId = categories.front().id;
    }
}

std::vector<ChannelVideo> getChannelVideos(
    const Config& config,
    OAuthSession& session,
    const std::string& channelSlug,
    const HTTP::Cancel& cancel) {
    const std::string slug = normalizeChannel(channelSlug);
    if (slug.empty()) return {};

    const json root = json::parse(kickGet(
        config,
        session,
        "https://kick.com/api/v2/channels/" + percentEncode(slug) + "/videos",
        cancel));

    const json* list = nullptr;
    if (root.is_array()) {
        list = &root;
    } else if (root.is_object() && root.contains("data") && root["data"].is_array()) {
        list = &root["data"];
    }
    if (!list) return {};

    std::vector<ChannelVideo> result;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        ChannelVideo video;
        std::string sourceUrl = jsonString(item, "source");
        if (sourceUrl.empty() && item.contains("video") && item["video"].is_object()) {
            sourceUrl = jsonString(item["video"], "source");
            if (sourceUrl.empty()) sourceUrl = jsonString(item["video"], "playback_url");
        }
        if (sourceUrl.empty()) sourceUrl = jsonString(item, "playback_url", jsonString(item, "stream_url"));

        std::string uuid;
        if (item.contains("video") && item["video"].is_object()) {
            uuid = jsonString(item["video"], "uuid", jsonString(item["video"], "id"));
        }
        if (uuid.empty()) uuid = jsonString(item, "uuid", jsonString(item, "id"));

        // Store the direct HLS .m3u8 source URL in `id` when available so resolveVod
        // can play it immediately, or fallback to the video UUID.
        video.id = !sourceUrl.empty() ? sourceUrl : uuid;
        video.url = sourceUrl;
        video.title = jsonString(item, "session_title", jsonString(item, "title", "Past Broadcast"));
        video.createdAt = jsonString(item, "created_at", jsonString(item, "start_time"));
        video.publishedAt = video.createdAt;
        if (item.contains("thumbnail")) {
            video.thumbnailUrl = jsonImageUrl(item["thumbnail"]);
        }
        video.duration = formatDurationMs(jsonInt(item, "duration", 0));
        video.language = jsonString(item, "language");
        video.viewCount = jsonInt(item, "views", jsonInt(item, "view_count", 0));

        if (!video.id.empty()) {
            result.push_back(std::move(video));
            if (result.size() >= 16) break;
        }
    }
    return result;
}

std::vector<ChannelClip> parseClipList(
    const json& root,
    const std::string& fallbackCreator,
    size_t limit) {
    const json* list = nullptr;
    if (root.is_object()) {
        if (root.contains("clips") && root["clips"].is_array()) {
            list = &root["clips"];
        } else if (root.contains("data") && root["data"].is_object() &&
                   root["data"].contains("clips") && root["data"]["clips"].is_array()) {
            list = &root["data"]["clips"];
        } else if (root.contains("data") && root["data"].is_array()) {
            list = &root["data"];
        }
    } else if (root.is_array()) {
        list = &root;
    }
    if (!list) return {};

    std::vector<ChannelClip> result;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        ChannelClip clip;
        // Event clips (web.kick.com) carry playback_url instead of clip_url.
        const std::string clipUrl = jsonString(item, "clip_url",
            jsonString(item, "video_url", jsonString(item, "playback_url")));
        const std::string rawId = jsonString(item, "id");
        clip.id = !clipUrl.empty() ? clipUrl : rawId;
        clip.url = clipUrl;
        clip.title = jsonString(item, "title", "Clip");
        clip.createdAt = jsonString(item, "created_at");
        clip.thumbnailUrl = jsonString(item, "thumbnail_url");
        if (item.contains("creator") && item["creator"].is_object()) {
            clip.creatorName = jsonString(item["creator"], "username");
        } else {
            clip.creatorName = fallbackCreator;
        }
        if (item.contains("channel") && item["channel"].is_object()) {
            clip.channelName = jsonString(item["channel"], "username");
        }
        clip.viewCount = jsonInt(item, "views",
            jsonInt(item, "view_count", jsonInt(item, "views_count", 0)));
        clip.duration = jsonFloat(item, "duration", 0.0f);

        if (!clip.id.empty()) {
            result.push_back(std::move(clip));
            if (result.size() >= limit) break;
        }
    }
    return result;
}

std::vector<ChannelClip> getChannelClips(
    const Config& config,
    OAuthSession& session,
    const std::string& channelSlug,
    const HTTP::Cancel& cancel) {
    const std::string slug = normalizeChannel(channelSlug);
    if (slug.empty()) return {};

    const json root = json::parse(kickGet(
        config,
        session,
        "https://kick.com/api/v2/channels/" + percentEncode(slug) + "/clips?sort=view",
        cancel));
    return parseClipList(root, slug, 16);
}

// Site-wide most-viewed clips for the last week, paged by Kick's own cursor.
ClipPage getPopularClips(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    std::string url = "https://kick.com/api/v2/clips?sort=view&time=week";
    if (!cursor.empty()) url += "&cursor=" + percentEncode(cursor);

    const json root = json::parse(kickGet(config, session, url, cancel));
    ClipPage page;
    page.loaded = true;
    page.items = parseClipList(root, "", 40);
    if (root.is_object() && root.contains("nextCursor") && root["nextCursor"].is_string())
        page.cursor = root["nextCursor"].get<std::string>();
    return page;
}

// Nested object lookup that tolerates missing keys and wrong types, so a
// change in Kick's newer event payloads never throws during parsing.
const json& jsonChild(const json& obj, const char* key) {
    static const json empty = json::object();
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_object()) return empty;
    return obj[key];
}

// Returns the first array found under data.<key>, data, or <key>.
const json* jsonList(const json& root, const char* key) {
    const json& data = jsonChild(root, "data");
    if (data.contains(key) && data[key].is_array()) return &data[key];
    if (root.is_object() && root.contains("data") && root["data"].is_array()) return &root["data"];
    if (root.is_object() && root.contains(key) && root[key].is_array()) return &root[key];
    return nullptr;
}

// Events from kick.com/events (web.kick.com/api/v1/events/browse). Live events
// come first, busiest first; upcoming and finished events follow.
EventPage getEvents(
    const Config& config,
    OAuthSession& session,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(kickGet(
        config, session, "https://web.kick.com/api/v1/events/browse", cancel));

    EventPage page;
    page.loaded = true;
    const json* list = jsonList(root, "events");
    if (!list) return page;

    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        LiveEvent event;
        event.id = jsonString(item, "id");
        event.slug = jsonString(item, "slug");
        if (event.id.empty() || event.slug.empty()) continue;
        if (jsonBool(item, "is_draft", false)) continue;

        event.title = jsonString(item, "title", jsonString(item, "name", event.slug));
        event.description = jsonString(item, "description");
        event.phase = jsonString(item, "phase");
        event.startTime = jsonString(item, "start_time");
        event.liveStreamCount = jsonInt(item, "num_livestreams", 0);
        event.viewCount = jsonInt(item, "view_count", 0);
        event.categoryName = jsonString(jsonChild(item, "category"), "name");

        const json& assets = jsonChild(item, "assets");
        event.thumbnailUrl = jsonString(assets, "thumbnail", jsonString(assets, "banner"));
        event.bannerUrl = jsonString(assets, "banner", event.thumbnailUrl);

        const json& organiser = jsonChild(item, "organiser");
        event.organiserName = jsonString(jsonChild(organiser, "user"), "username");
        event.organiserSlug = jsonString(jsonChild(organiser, "channel"), "slug");
        if (event.organiserName.empty()) event.organiserName = event.organiserSlug;

        page.items.push_back(std::move(event));
    }

    std::stable_sort(page.items.begin(), page.items.end(), [](const LiveEvent& a, const LiveEvent& b) {
        const auto rank = [](const LiveEvent& e) { return e.isLive() ? 0 : (e.phase == "pre" ? 1 : 2); };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a.liveStreamCount > b.liveStreamCount;
    });
    return page;
}

// Live streams in one event, mapped onto the regular stream cards so they
// play through the normal Kick player.
StreamPage getEventStreams(
    const Config& config,
    OAuthSession& session,
    const std::string& eventId,
    const HTTP::Cancel& cancel) {
    const json root = json::parse(kickGet(
        config, session,
        "https://web.kick.com/api/v1/events/" + percentEncode(eventId) + "/streams",
        cancel));

    StreamPage page;
    page.loaded = true;
    const json* list = jsonList(root, "streams");
    if (!list) return page;

    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        const json& channel = jsonChild(item, "channel");
        const json& user = jsonChild(item, "user");
        const json& livestream = jsonChild(item, "livestream");

        Stream stream;
        stream.userLogin = jsonString(channel, "slug");
        if (stream.userLogin.empty()) continue;
        stream.userId = jsonString(channel, "id");
        stream.userName = jsonString(user, "username", stream.userLogin);
        stream.id = jsonString(livestream, "id");
        stream.title = jsonString(livestream, "title");
        stream.language = jsonString(livestream, "language");
        stream.viewerCount = jsonInt(livestream, "view_count", 0);
        stream.thumbnailUrl = jsonString(jsonChild(livestream, "thumbnail"), "src");
        stream.isLive = true;
        page.items.push_back(std::move(stream));
    }
    return page;
}

// Most-viewed clips from one event. Kick ignores sort=; only
// sort_by=view_count is accepted (other values return Bad Request).
ClipPage getEventClips(
    const Config& config,
    OAuthSession& session,
    const std::string& eventId,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    std::string url = "https://web.kick.com/api/v1/events/" + percentEncode(eventId) +
        "/clips?sort_by=view_count";
    if (!cursor.empty()) url += "&cursor=" + percentEncode(cursor);

    const json root = json::parse(kickGet(config, session, url, cancel));
    ClipPage page;
    page.loaded = true;
    page.items = parseClipList(root, "", 40);
    page.cursor = jsonString(jsonChild(root, "data"), "cursor");
    return page;
}

StreamPage parseKickStreamListResponse(const std::string& response, const std::string& nextCursor = "") {
    const json root = json::parse(response);
    StreamPage page;
    page.loaded = true;
    page.cursor = nextCursor;

    const json* list = nullptr;
    if (root.is_array()) {
        list = &root;
    } else if (root.is_object()) {
        if (root.contains("data")) {
            const auto& data = root["data"];
            if (data.is_array()) {
                list = &data;
            } else if (data.is_object()) {
                for (const char* key : {"livestreams", "streams", "channels", "items", "data"}) {
                    if (data.contains(key) && data[key].is_array()) {
                        list = &data[key];
                        break;
                    }
                }
            }
        }
        if (!list) {
            for (const char* key : {"livestreams", "streams", "channels", "items"}) {
                if (root.contains(key) && root[key].is_array()) {
                    list = &root[key];
                    break;
                }
            }
        }
    }

    if (!list) return page;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        Stream stream = parseKickStream(item);
        // These endpoints (featured/popular/category) only return live
        // streams, and their items often carry no is_live/livestream field,
        // which made parseKickStream mark them offline.
        if (!stream.isLive) {
            const bool hasExplicitLiveInfo =
                item.contains("is_live") || item.contains("livestream") ||
                (item.contains("channel") && item["channel"].is_object() &&
                 (item["channel"].contains("is_live") ||
                  item["channel"].contains("livestream")));
            if (!hasExplicitLiveInfo) stream.isLive = true;
        }
        if (!stream.userLogin.empty()) {
            page.items.push_back(std::move(stream));
        }
    }
    return page;
}

StreamPage getPopular(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    const int pageNum = parsePageNumber(cursor);
    // Primary Kick v1 endpoint discovered via REA (ev_f70930... request-98)
    try {
        std::string url = "https://web.kick.com/api/v1/livestreams/featured?language=en";
        if (pageNum > 1) url += "&page=" + std::to_string(pageNum);
        StreamPage page = parseKickStreamListResponse(kickGet(config, session, url, cancel));
        if (!page.items.empty()) return page;
    } catch (const std::exception& error) {
        throwIfCancelled(cancel);
        brls::Logger::warning("Kick v1 featured livestreams failed, trying fallback: {}", error.what());
    }

    return parseKickStreamListResponse(
        kickGet(config, session, "https://kick.com/stream/featured-livestreams/en", cancel));
}

static const json* extractChannelList(const json& root) {
    if (root.is_array()) return &root;
    if (root.is_object()) {
        if (root.contains("channels") && root["channels"].is_array()) {
            return &root["channels"];
        }
        if (root.contains("data")) {
            if (root["data"].is_array()) {
                return &root["data"];
            }
            if (root["data"].is_object() && root["data"].contains("channels") && root["data"]["channels"].is_array()) {
                return &root["data"]["channels"];
            }
        }
    }
    return nullptr;
}

static std::string extractNextCursor(const json& root) {
    auto checkCursor = [](const json& obj, const char* key) -> std::string {
        if (!obj.is_object() || !obj.contains(key)) return "";
        const auto& val = obj[key];
        if (val.is_null()) return "";
        if (val.is_string()) return val.get<std::string>();
        if (val.is_number_integer()) {
            long long v = val.get<long long>();
            return v > 0 ? std::to_string(v) : "";
        }
        if (val.is_number_unsigned()) {
            unsigned long long v = val.get<unsigned long long>();
            return v > 0 ? std::to_string(v) : "";
        }
        return "";
    };

    std::string c = checkCursor(root, "nextCursor");
    if (!c.empty()) return c;
    c = checkCursor(root, "next_cursor");
    if (!c.empty()) return c;
    c = checkCursor(root, "cursor");
    if (!c.empty()) return c;
    if (root.contains("data") && root["data"].is_object()) {
        c = checkCursor(root["data"], "nextCursor");
        if (!c.empty()) return c;
        c = checkCursor(root["data"], "next_cursor");
        if (!c.empty()) return c;
        c = checkCursor(root["data"], "cursor");
        if (!c.empty()) return c;
    }
    return "";
}

void getFollowedChannels(
    const Config& config,
    OAuthSession& session,
    const HTTP::Cancel& cancel,
    StreamPage& liveResult,
    StreamPage& offlineResult) {
    liveResult.loaded = true;
    offlineResult.loaded = true;
    if (session.accessToken.empty()) return;

    static constexpr size_t MAX_OFFLINE_FOLLOWED = 40;
    static constexpr int MAX_PAGES_TO_SCAN = 5;

    std::unordered_set<std::string> seenLive;
    std::unordered_set<std::string> seenOffline;
    std::vector<Stream> offlineCandidates;

    auto processItems = [&](const json& list) {
        for (const auto& item : list) {
            if (!item.is_object()) continue;
            Stream stream = parseKickStream(item);
            if (stream.userLogin.empty()) continue;

            {
                std::lock_guard<std::mutex> lock(s_followedChannelsMutex);
                s_followedChannelsCache.insert(stream.userLogin);
                s_followedChannelsCache.insert(toLowerAscii(stream.userLogin));
                if (!stream.userId.empty()) s_followedChannelsCache.insert(stream.userId);
            }

            if (stream.isLive) {
                if (seenLive.insert(stream.userLogin).second) {
                    liveResult.items.push_back(std::move(stream));
                }
            } else {
                if (seenOffline.insert(stream.userLogin).second) {
                    offlineCandidates.push_back(std::move(stream));
                }
            }
        }
    };

    std::string nextCursor = "";
    int pageCount = 0;

    do {
        throwIfCancelled(cancel);
        try {
            std::string url = "https://kick.com/api/v2/channels/followed";
            if (!nextCursor.empty()) url += "?cursor=" + percentEncode(nextCursor);
            const json root = json::parse(kickGet(config, session, url, cancel));
            const json* list = extractChannelList(root);
            if (!list || list->empty()) {
                break;
            }

            processItems(*list);

            pageCount++;
            nextCursor = extractNextCursor(root);
            if (!nextCursor.empty()) {
                liveResult.cursor = nextCursor;
            }

            if (pageCount >= MAX_PAGES_TO_SCAN || offlineCandidates.size() >= MAX_OFFLINE_FOLLOWED) {
                break;
            }
        } catch (const std::exception& error) {
            throwIfCancelled(cancel);
            brls::Logger::warning("Kick followed channels request failed: {}", error.what());
            break;
        }
    } while (!nextCursor.empty());

    // Fallback/supplement: if offline channels were not found via v2, query api/v1/channels/followed
    if (offlineCandidates.empty()) {
        try {
            throwIfCancelled(cancel);
            const json root = json::parse(kickGet(config, session, "https://kick.com/api/v1/channels/followed", cancel));
            const json* list = extractChannelList(root);
            if (list) {
                processItems(*list);
            }
        } catch (const std::exception& error) {
            throwIfCancelled(cancel);
            brls::Logger::warning("Kick v1 followed channels fallback failed: {}", error.what());
        }
    }

    // Preserve natural follow order as returned by Kick (do not sort by follower count)
    if (offlineCandidates.size() > MAX_OFFLINE_FOLLOWED) {
        offlineCandidates.resize(MAX_OFFLINE_FOLLOWED);
    }

    offlineResult.items = std::move(offlineCandidates);
}

StreamPage getFollowed(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    StreamPage result;
    result.loaded = true;
    if (session.accessToken.empty()) return result;

    try {
        std::string url = "https://kick.com/api/v2/channels/followed";
        if (!cursor.empty()) url += "?cursor=" + percentEncode(cursor);
        const json root = json::parse(kickGet(config, session, url, cancel));
        const json* list = extractChannelList(root);
        if (list) {
            for (const auto& item : *list) {
                if (!item.is_object()) continue;
                Stream stream = parseKickStream(item);
                if (stream.userLogin.empty() || !stream.isLive) continue;
                result.items.push_back(std::move(stream));
            }
            result.cursor = extractNextCursor(root);
        }
    } catch (const std::exception& error) {
        throwIfCancelled(cancel);
        brls::Logger::warning("Kick followed channels request failed: {}", error.what());
    }
    return result;
}

CategoryPage getCategories(
    const Config& config,
    OAuthSession& session,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    const int pageNum = parsePageNumber(cursor);
    const std::string nextCursor = std::to_string(pageNum + 1);

    auto parseCategoriesJson = [&](const std::string& body) -> CategoryPage {
        const json root = json::parse(body);
        CategoryPage result;
        result.loaded = true;

        const json* list = nullptr;
        if (root.is_array()) {
            list = &root;
        } else if (root.is_object()) {
            if (root.contains("data")) {
                if (root["data"].is_array()) {
                    list = &root["data"];
                } else if (root["data"].is_object() &&
                           root["data"].contains("subcategories") &&
                           root["data"]["subcategories"].is_array()) {
                    list = &root["data"]["subcategories"];
                }
            }
        }
        if (!list) return result;
        for (const auto& item : *list) {
            if (!item.is_object()) continue;
            Category cat = parseKickCategory(item);
            if (!cat.name.empty()) result.items.push_back(std::move(cat));
        }
        if (result.items.size() >= 24) result.cursor = nextCursor;
        return result;
    };

    // Primary endpoint discovered via REA (ev_f70930... request-100)
    try {
        return parseCategoriesJson(kickGet(
            config,
            session,
            "https://web.kick.com/api/v1/subcategories?limit=50&page=" + std::to_string(pageNum),
            cancel));
    } catch (const std::exception& error) {
        throwIfCancelled(cancel);
        brls::Logger::warning("Kick web.kick.com subcategories failed, trying fallback: {}", error.what());
    }

    return parseCategoriesJson(kickGet(
        config,
        session,
        "https://kick.com/api/v1/subcategories?page=" + std::to_string(pageNum) + "&limit=50",
        cancel));
}

StreamPage getSearch(
    const Config& config,
    OAuthSession& session,
    const std::string& query,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    StreamPage result;
    result.loaded = true;
    if (!cursor.empty()) return result;

    std::unordered_set<std::string> seenLogins;

    // 1. Direct Kick v2 channel lookup so searching a streamer's slug directly
    // always resolves their live or offline channel card reliably.
    const std::string normalized = normalizeChannel(query);
    if (!normalized.empty()) {
        try {
            const json channelRoot = fetchKickChannelJson(config, session, normalized, cancel);
            const UserProfile profile = parseKickChannelProfile(channelRoot, normalized);
            if (!profile.login.empty()) {
                ChannelInformation info;
                Stream liveStream;
                std::vector<Category> cats;
                populateChannelInfoAndLiveStream(channelRoot, profile, info, liveStream, cats);
                if (!liveStream.isLive) {
                    liveStream.title = profile.description.empty() ? "Offline" : profile.description;
                    liveStream.thumbnailUrl = profile.profileImageUrl;
                }
                seenLogins.insert(profile.login);
                result.items.push_back(std::move(liveStream));
            }
        } catch (...) {
            throwIfCancelled(cancel);
        }
    }

    // 2. Kick search endpoint for matching channels
    try {
        const json root = json::parse(kickGet(
            config,
            session,
            "https://kick.com/api/search?searched_word=" + percentEncode(query),
            cancel));
        if (root.is_object() && root.contains("channels") && root["channels"].is_array()) {
            for (const auto& item : root["channels"]) {
                if (!item.is_object()) continue;
                Stream stream = parseKickStream(item);
                if (!stream.userLogin.empty() && seenLogins.insert(stream.userLogin).second) {
                    result.items.push_back(std::move(stream));
                }
            }
        }
    } catch (...) {
        throwIfCancelled(cancel);
    }

    return result;
}

StreamPage getCategoryStreams(
    const Config& config,
    OAuthSession& session,
    const std::string& gameId,
    const std::string& cursor,
    const HTTP::Cancel& cancel) {
    const int pageNum = parsePageNumber(cursor);
    const std::string nextCursor = std::to_string(pageNum + 1);

    try {
        StreamPage page = parseKickStreamListResponse(
            kickGet(
                config,
                session,
                "https://kick.com/stream/livestreams/en?page=" + std::to_string(pageNum) +
                    "&limit=30&subcategory=" + percentEncode(gameId) + "&sort=desc",
                cancel),
            nextCursor);
        if (!page.items.empty()) return page;
    } catch (...) {
        throwIfCancelled(cancel);
    }

    // Fallback: filter featured livestreams matching the category slug/name
    StreamPage popular = getPopular(config, session, "", cancel);
    StreamPage filtered;
    filtered.loaded = true;
    for (auto& item : popular.items) {
        if (item.gameId == gameId || item.gameName == gameId) {
            filtered.items.push_back(std::move(item));
        }
    }
    return filtered.items.empty() ? popular : filtered;
}

MediaPlayback resolveVod(
    const std::string& videoId,
    const HTTP::Cancel& cancel) {
    if (videoId.empty())
        throw std::runtime_error("The Kick VOD has no video ID");

    throwIfCancelled(cancel);
    std::string source;
    if (videoId.rfind("http://", 0) == 0 || videoId.rfind("https://", 0) == 0) {
        source = videoId;
    } else {
        HTTP::Header headers = {
            "Accept: application/json",
            "x-app-platform: web",
            "Origin: https://kick.com",
            "Referer: https://kick.com/",
        };
        const std::string response = cancel
            ? HTTP::get(
                  "https://kick.com/api/v1/video/" + percentEncode(videoId),
                  headers,
                  HTTP::Timeout{15000},
                  cancel)
            : HTTP::get(
                  "https://kick.com/api/v1/video/" + percentEncode(videoId),
                  headers,
                  HTTP::Timeout{15000});
        throwIfCancelled(cancel);

        const json root = json::parse(response);
        source = jsonString(root, "source");
        if (source.empty() && root.contains("data") && root["data"].is_object()) {
            source = jsonString(root["data"], "source");
            if (source.empty()) source = jsonString(root["data"], "playback_url");
            if (source.empty() && root["data"].contains("video") && root["data"]["video"].is_object()) {
                source = jsonString(root["data"]["video"], "source", jsonString(root["data"]["video"], "playback_url"));
            }
        }
        if (source.empty() && root.contains("video") && root["video"].is_object()) {
            source = jsonString(root["video"], "source", jsonString(root["video"], "playback_url"));
        }
        if (source.empty()) {
            source = jsonString(root, "playback_url", jsonString(root, "stream_url"));
        }
        if (source.empty()) {
            throw std::runtime_error("Kick did not return a playable VOD stream URL");
        }
    }

    MediaPlayback result;
    result.quality = "auto";
    result.url = source;

    if (source.find(".m3u8") != std::string::npos) {
        try {
            HTTP::Header m3u8Headers = {
                "Referer: https://kick.com/",
                "Origin: https://kick.com",
                "Accept: application/vnd.apple.mpegurl, application/x-mpegURL, */*",
            };
            const std::string playlistText = cancel
                ? HTTP::get(source, m3u8Headers, HTTP::Timeout{10000}, cancel)
                : HTTP::get(source, m3u8Headers, HTTP::Timeout{10000});
            throwIfCancelled(cancel);

            result.subtitles = kick::parseMasterSubtitles(source, playlistText);
            auto qualities = kick::parseMasterPlaylist(source, playlistText);
            if (qualities.size() > 1) {
                std::string preferred = kick::loadPreferredQuality();
                if (preferred.empty()) preferred = "auto";
                if (preferred != "auto") {
                    kick::Quality selected = kick::selectQuality(qualities, preferred);
                    if (!selected.url.empty() && selected.url != source) {
                        result.url = selected.url;
                        result.quality = selected.name;
                        brls::Logger::info("Kick VOD resolved direct variant: {} ({})", selected.name, selected.url);
                    }
                } else {
                    result.quality = "Auto";
                }
            }
        } catch (const std::exception& error) {
            brls::Logger::warning("Kick VOD variant resolution fallback to master: {}", error.what());
        }
    }

    return result;
}

MediaPlayback resolveClip(
    const std::string& clipSlug,
    const HTTP::Cancel& cancel) {
    if (clipSlug.empty())
        throw std::runtime_error("The Kick clip has no identifier");

    throwIfCancelled(cancel);
    if (clipSlug.rfind("http://", 0) == 0 || clipSlug.rfind("https://", 0) == 0) {
        MediaPlayback result;
        result.quality = "auto";
        result.url = clipSlug;
        return result;
    }

    HTTP::Header headers = {
        "Accept: application/json",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };
    const std::string response = cancel
        ? HTTP::get(
              "https://kick.com/api/v2/clips/" + percentEncode(clipSlug),
              headers,
              HTTP::Timeout{15000},
              cancel)
        : HTTP::get(
              "https://kick.com/api/v2/clips/" + percentEncode(clipSlug),
              headers,
              HTTP::Timeout{15000});
    throwIfCancelled(cancel);

    const json root = json::parse(response);
    const json& clipObj = (root.contains("clip") && root["clip"].is_object())
        ? root["clip"]
        : ((root.contains("data") && root["data"].is_object()) ? root["data"] : root);
    const std::string clipUrl = jsonString(clipObj, "clip_url", jsonString(clipObj, "video_url"));
    if (clipUrl.empty()) {
        throw std::runtime_error("Kick did not return a playable clip URL");
    }

    MediaPlayback result;
    result.quality = "auto";
    result.url = clipUrl;
    return result;
}

std::string sectionError(const std::exception& exception) {
    const std::string message = exception.what();
    return message.empty() ? "Unknown request error" : message;
}

template <typename Result, typename Work, typename Callback>
void loadPageAsync(
    const Config& config,
    Work work,
    Callback success,
    ErrorCallback failure,
    HTTP::Cancel cancel,
    const std::string& unknownError) {
    ThreadPool::instance().submit(
        [config, work = std::move(work), success = std::move(success),
            failure = std::move(failure), cancel = std::move(cancel), unknownError](HTTP&) mutable {
            try {
                OAuthSession session = ensureOAuth(config);
                throwIfCancelled(cancel);
                Result result = work(config, session, cancel);
                if (session.signedIn()) saveOAuth(session);
                throwIfCancelled(cancel);
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, unknownError));
            }
        });
}

}  // namespace

std::string mediaThumbnail(
    const std::string& pattern,
    int width,
    int height) {
    std::string result = pattern;
    replaceAll(result, "%{width}", std::to_string(width));
    replaceAll(result, "%{height}", std::to_string(height));
    replaceAll(result, "{width}", std::to_string(width));
    replaceAll(result, "{height}", std::to_string(height));
    return result;
}

std::string streamThumbnail(const std::string& pattern, int width, int height) {
    std::string result = pattern;
    replaceAll(result, "{width}", std::to_string(width));
    replaceAll(result, "{height}", std::to_string(height));
    return result;
}

std::string categoryThumbnail(const std::string& pattern, int width, int height) {
    return streamThumbnail(pattern, width, height);
}

void loadChannelProfileAsync(
    const Config& config,
    const std::string& login,
    UserProfileCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config,
         login,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                OAuthSession session = ensureOAuth(config);
                throwIfCancelled(cancel);
                UserProfile profile = getUserByLogin(config, session, login, cancel);
                if (session.signedIn()) saveOAuth(session);
                throwIfCancelled(cancel);
                brls::sync(std::bind(success, std::move(profile)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick channel error")));
            }
        });
}

static void resolveCategoryBanner(
    const Config& config,
    OAuthSession& session,
    Category& cat,
    const HTTP::Cancel& cancel) {
    if (isValidImageUrl(cat.boxArtUrl)) return;
    cat.boxArtUrl.clear();

    // 1. Check in-memory cache
    std::string cached = getCachedCategoryBanner(cat.name);
    if (cached.empty() && !cat.id.empty()) cached = getCachedCategoryBanner(cat.id);
    const std::string slugified = slugifyCategoryName(cat.name);
    if (cached.empty() && !slugified.empty()) cached = getCachedCategoryBanner(slugified);
    if (isValidImageUrl(cached)) {
        cat.boxArtUrl = cached;
        return;
    }

    // 2. Query Kick search endpoint (where subcategories contain banners)
    if (!cat.name.empty()) {
        for (const std::string& searchBase : {
            "https://web.kick.com/api/search?searched_word=",
            "https://kick.com/api/search?searched_word="
        }) {
            if (!cat.boxArtUrl.empty()) break;
            try {
                const std::string searchUrl = searchBase + percentEncode(cat.name);
                const json sroot = json::parse(kickGet(config, session, searchUrl, cancel));

                // CRITICAL: Check subcategories FIRST!
                const json* subcats = nullptr;
                if (sroot.contains("subcategories") && sroot["subcategories"].is_array()) {
                    subcats = &sroot["subcategories"];
                } else if (sroot.contains("data") && sroot["data"].is_object() &&
                           sroot["data"].contains("subcategories") && sroot["data"]["subcategories"].is_array()) {
                    subcats = &sroot["data"]["subcategories"];
                }

                if (subcats && !subcats->empty()) {
                    std::string matchedBanner;
                    std::string matchedSlug;
                    for (const auto& item : *subcats) {
                        if (!item.is_object()) continue;
                        Category parsed = parseKickCategory(item);
                        if (parsed.boxArtUrl.empty()) continue;

                        if (stringsEqualIgnoreCase(parsed.name, cat.name) ||
                            stringsEqualIgnoreCase(parsed.id, cat.id) ||
                            stringsEqualIgnoreCase(parsed.id, slugified) ||
                            slugifyCategoryName(parsed.name) == slugified) {
                            matchedBanner = parsed.boxArtUrl;
                            matchedSlug = parsed.id;
                            break;
                        }
                        if (matchedBanner.empty()) {
                            matchedBanner = parsed.boxArtUrl;
                            matchedSlug = parsed.id;
                        }
                    }

                    if (!matchedBanner.empty()) {
                        cat.boxArtUrl = matchedBanner;
                        if (!matchedSlug.empty()) cat.id = matchedSlug;
                        cacheCategoryBanner(cat.id, matchedBanner);
                        cacheCategoryBanner(cat.name, matchedBanner);
                        cacheCategoryBanner(slugified, matchedBanner);
                        return;
                    }
                }

                // Fallback to categories array if subcategories had no items
                if (sroot.contains("categories") && sroot["categories"].is_array()) {
                    for (const auto& item : sroot["categories"]) {
                        if (!item.is_object()) continue;
                        Category parsed = parseKickCategory(item);
                        if (!parsed.boxArtUrl.empty()) {
                            cat.boxArtUrl = parsed.boxArtUrl;
                            if (!parsed.id.empty()) cat.id = parsed.id;
                            cacheCategoryBanner(cat.id, cat.boxArtUrl);
                            cacheCategoryBanner(cat.name, cat.boxArtUrl);
                            cacheCategoryBanner(slugified, cat.boxArtUrl);
                            return;
                        }
                    }
                }
            } catch (...) {
                throwIfCancelled(cancel);
            }
        }
    }

    // 3. Try candidate slugs on direct endpoints
    std::vector<std::string> candidateSlugs;
    if (!cat.id.empty()) {
        candidateSlugs.push_back(cat.id);
    }
    if (!slugified.empty() && std::find(candidateSlugs.begin(), candidateSlugs.end(), slugified) == candidateSlugs.end()) {
        candidateSlugs.push_back(slugified);
    }
    if (cat.name.find('&') != std::string::npos) {
        std::string altName = cat.name;
        replaceAll(altName, "&", "and");
        std::string altSlug = slugifyCategoryName(altName);
        if (!altSlug.empty() && std::find(candidateSlugs.begin(), candidateSlugs.end(), altSlug) == candidateSlugs.end()) {
            candidateSlugs.push_back(altSlug);
        }
    }

    for (const auto& slug : candidateSlugs) {
        if (!cat.boxArtUrl.empty()) break;
        for (const std::string& baseUrl : {
            "https://web.kick.com/api/v1/subcategories/",
            "https://kick.com/api/v1/subcategories/",
            "https://web.kick.com/api/v1/subcategories?search=",
            "https://web.kick.com/api/v1/categories/"
        }) {
            try {
                const std::string subcatUrl = baseUrl + percentEncode(slug);
                const json subcat = json::parse(kickGet(config, session, subcatUrl, cancel));
                const json* target = nullptr;
                if (subcat.is_object()) {
                    if (subcat.contains("data")) {
                        if (subcat["data"].is_object()) {
                            target = &subcat["data"];
                        } else if (subcat["data"].is_array() && !subcat["data"].empty()) {
                            target = &subcat["data"].front();
                        }
                    } else if (subcat.contains("banner") || subcat.contains("icon")) {
                        target = &subcat;
                    }
                } else if (subcat.is_array() && !subcat.empty()) {
                    target = &subcat.front();
                }

                if (target && target->is_object()) {
                    Category parsed = parseKickCategory(*target);
                    if (!parsed.boxArtUrl.empty()) {
                        cat.boxArtUrl = parsed.boxArtUrl;
                        if (!parsed.id.empty()) cat.id = parsed.id;
                        cacheCategoryBanner(cat.id, cat.boxArtUrl);
                        cacheCategoryBanner(cat.name, cat.boxArtUrl);
                        cacheCategoryBanner(slug, cat.boxArtUrl);
                        cacheCategoryBanner(slugified, cat.boxArtUrl);
                        return;
                    }
                }
            } catch (...) {
                throwIfCancelled(cancel);
            }
        }
    }

    // 4. Fallback: query top subcategories from web.kick.com (caches top 50 games)
    try {
        const json root = json::parse(kickGet(
            config,
            session,
            "https://web.kick.com/api/v1/subcategories?limit=50&page=1",
            cancel));
        const json* list = nullptr;
        if (root.is_object() && root.contains("data")) {
            if (root["data"].is_array()) list = &root["data"];
            else if (root["data"].is_object() && root["data"].contains("subcategories") && root["data"]["subcategories"].is_array()) {
                list = &root["data"]["subcategories"];
            }
        } else if (root.is_array()) {
            list = &root;
        }
        if (list) {
            for (const auto& item : *list) {
                if (!item.is_object()) continue;
                Category parsed = parseKickCategory(item);
                if (!parsed.boxArtUrl.empty()) {
                    if (cat.boxArtUrl.empty() && (
                        stringsEqualIgnoreCase(parsed.name, cat.name) ||
                        stringsEqualIgnoreCase(parsed.id, cat.id) ||
                        stringsEqualIgnoreCase(parsed.id, slugified) ||
                        slugifyCategoryName(parsed.name) == slugified)) {
                        cat.boxArtUrl = parsed.boxArtUrl;
                        if (!parsed.id.empty()) cat.id = parsed.id;
                    }
                }
            }
        }
    } catch (...) {
        throwIfCancelled(cancel);
    }
}

void loadChannelPageAsync(
    const Config& config,
    const std::string& login,
    ChannelPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config,
         login,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                OAuthSession session = ensureOAuth(config);
                throwIfCancelled(cancel);

                const std::string slug = normalizeChannel(login);
                const json channelRoot = fetchKickChannelJson(config, session, slug, cancel);
                throwIfCancelled(cancel);

                ChannelPageData result;
                result.profile = parseKickChannelProfile(channelRoot, slug);
                if (result.profile.login.empty()) {
                    throw std::runtime_error("Kick channel was not found");
                }

                populateChannelInfoAndLiveStream(
                    channelRoot,
                    result.profile,
                    result.information,
                    result.liveStream,
                    result.categories);
                result.live = result.liveStream.isLive && !result.liveStream.id.empty();

                result.isFollowing = jsonBool(channelRoot, "is_following",
                    jsonBool(channelRoot, "following",
                    jsonBool(channelRoot, "isFollowing",
                    jsonBool(channelRoot, "followed", false))));
                if (!result.isFollowing && channelRoot.contains("user") && channelRoot["user"].is_object()) {
                    result.isFollowing = jsonBool(channelRoot["user"], "is_following",
                        jsonBool(channelRoot["user"], "following", false));
                }
                if (!result.isFollowing && channelRoot.contains("chatroom") && channelRoot["chatroom"].is_object()) {
                    result.isFollowing = jsonBool(channelRoot["chatroom"], "is_following",
                        jsonBool(channelRoot["chatroom"], "following", false));
                }
                if (!result.isFollowing) {
                    std::lock_guard<std::mutex> lock(s_followedChannelsMutex);
                    if (s_followedChannelsCache.count(slug) ||
                        s_followedChannelsCache.count(toLowerAscii(slug)) ||
                        s_followedChannelsCache.count(result.profile.login) ||
                        s_followedChannelsCache.count(toLowerAscii(result.profile.login)) ||
                        (!result.profile.id.empty() && s_followedChannelsCache.count(result.profile.id))) {
                        result.isFollowing = true;
                    }
                }

                for (auto& cat : result.categories) {
                    if (!isValidImageUrl(cat.boxArtUrl)) {
                        cat.boxArtUrl.clear();
                        resolveCategoryBanner(config, session, cat, cancel);
                    }
                }

                try {
                    result.videos = getChannelVideos(
                        config,
                        session,
                        result.profile.login,
                        cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.videosError = exception.what();
                }

                try {
                    result.clips = getChannelClips(
                        config,
                        session,
                        result.profile.login,
                        cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.clipsError = exception.what();
                }

                if (session.signedIn()) saveOAuth(session);
                throwIfCancelled(cancel);
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick channel page error")));
            }
        });
}

void setChannelFollowAsync(
    const Config& config,
    const std::string& channelSlug,
    bool follow,
    FollowCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [config,
         slug = normalizeChannel(channelSlug),
         follow,
         success = std::move(success),
         failure = std::move(failure)](HTTP&) mutable {
            try {
                OAuthSession session = ensureOAuth(config);
                if (!session.signedIn()) {
                    throw std::runtime_error("Sign in to follow channels on Kick.");
                }
                if (slug.empty()) {
                    throw std::runtime_error("Channel was not specified.");
                }

                const std::string path = "https://kick.com/api/v2/channels/" + percentEncode(slug) + "/follow";
                const std::string webPath = "https://web.kick.com/api/v2/channels/" + percentEncode(slug) + "/follow";

                bool ok = false;
                std::string lastErr;
                for (const std::string& url : {path, webPath}) {
                    try {
                        if (follow) {
                            HTTP::post(url, "{}", kickHeaders(config, session), HTTP::Timeout{15000});
                        } else {
                            HTTP::del(url, kickHeaders(config, session), HTTP::Timeout{15000});
                        }
                        ok = true;
                        break;
                    } catch (const std::exception& e) {
                        lastErr = e.what();
                    }
                }
                if (!ok) {
                    throw std::runtime_error(lastErr.empty() ? "Failed to update follow status" : lastErr);
                }

                {
                    std::lock_guard<std::mutex> lock(s_followedChannelsMutex);
                    if (follow) {
                        s_followedChannelsCache.insert(slug);
                        s_followedChannelsCache.insert(toLowerAscii(slug));
                    } else {
                        s_followedChannelsCache.erase(slug);
                        s_followedChannelsCache.erase(toLowerAscii(slug));
                    }
                }

                s_followedChannelsDirty.store(true);

                brls::sync([success = std::move(success), follow]() {
                    if (success) success(follow);
                });
            } catch (const std::exception& e) {
                brls::sync([failure = std::move(failure), msg = std::string(e.what())]() {
                    if (failure) failure(msg);
                });
            } catch (...) {
                brls::sync([failure = std::move(failure)]() {
                    if (failure) failure("Unknown follow error");
                });
            }
        });
}

bool isFollowedChannelsDirty() {
    return s_followedChannelsDirty.load();
}

void clearFollowedChannelsDirty() {
    s_followedChannelsDirty.store(false);
}

void resolveVodAsync(
    const std::string& videoId,
    MediaPlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [videoId,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                MediaPlayback result = resolveVod(videoId, cancel);
                throwIfCancelled(cancel);
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick VOD playback error")));
            }
        });
}

void resolveClipAsync(
    const std::string& clipSlug,
    MediaPlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [clipSlug,
         success = std::move(success),
         failure = std::move(failure),
         cancel = std::move(cancel)](HTTP&) mutable {
            try {
                MediaPlayback result = resolveClip(clipSlug, cancel);
                throwIfCancelled(cancel);
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick clip playback error")));
            }
        });
}

void loadHomeAsync(
    const Config& config,
    HomeCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit(
        [config, success = std::move(success), failure = std::move(failure),
            cancel = std::move(cancel)](HTTP&) mutable {
            try {
                HomeData result;
                result.account = ensureOAuth(config);
                throwIfCancelled(cancel);

                if (result.account.signedIn()) {
                    try {
                        result.profile = getCurrentUser(config, result.account, cancel);
                    } catch (const std::exception& exception) {
                        throwIfCancelled(cancel);
                        brls::Logger::warning(
                            "kickNX: profile image could not be loaded: {}",
                            exception.what());
                    }

                    try {
                        getFollowedChannels(config, result.account, cancel, result.followed, result.offlineFollowed);
                    } catch (const std::exception& exception) {
                        throwIfCancelled(cancel);
                        result.followedError = sectionError(exception);
                        result.offlineFollowedError = sectionError(exception);
                    }
                } else {
                    result.followed.loaded = true;
                    result.offlineFollowed.loaded = true;
                }

                try {
                    result.popular = getPopular(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.popularError = sectionError(exception);
                }

                try {
                    result.popularClips = getPopularClips(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.popularClipsError = sectionError(exception);
                }

                // Events are optional: a failure is logged and the row is hidden.
                try {
                    result.events = getEvents(config, result.account, cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    brls::Logger::warning("kickNX: events could not be loaded: {}", exception.what());
                }

                try {
                    result.categories = getCategories(config, result.account, "", cancel);
                } catch (const std::exception& exception) {
                    throwIfCancelled(cancel);
                    result.categoriesError = sectionError(exception);
                }

                if (result.account.signedIn()) saveOAuth(result.account);
                throwIfCancelled(cancel);
                if (!result.popular.loaded && !result.categories.loaded) {
                    throw std::runtime_error(
                        "Kick did not return any home sections. Check the connection and retry.");
                }
                brls::sync(std::bind(success, std::move(result)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick browsing error")));
            }
        });
}

void searchChannelsPageAsync(
    const Config& config,
    const std::string& query,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [query, cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getSearch(c, session, query, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown Kick search error");
}

void loadCategoryStreamsPageAsync(
    const Config& config,
    const std::string& gameId,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [gameId, cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getCategoryStreams(c, session, gameId, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown category request error");
}

void loadFollowedPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getFollowed(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown followed-streams request error");
}

void loadPopularPageAsync(
    const Config& config,
    const std::string& cursor,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getPopular(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown popular-streams request error");
}

void loadEventStreamsAsync(
    const Config& config,
    const std::string& eventId,
    StreamPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<StreamPage>(config,
        [eventId](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getEventStreams(c, session, eventId, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown event streams request error");
}

void loadEventClipsAsync(
    const Config& config,
    const std::string& eventId,
    const std::string& cursor,
    ClipPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<ClipPage>(config,
        [eventId, cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getEventClips(c, session, eventId, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown event clips request error");
}

void loadPopularClipsPageAsync(
    const Config& config,
    const std::string& cursor,
    ClipPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<ClipPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getPopularClips(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown popular-clips request error");
}

void loadCategoriesPageAsync(
    const Config& config,
    const std::string& cursor,
    CategoryPageCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    loadPageAsync<CategoryPage>(config,
        [cursor](const Config& c, OAuthSession& session, const HTTP::Cancel& requestCancel) {
            return getCategories(c, session, cursor, requestCancel);
        },
        std::move(success), std::move(failure), std::move(cancel),
        "Unknown categories request error");
}

}  // namespace kick
