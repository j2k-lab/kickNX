#include "api/kick_chat.hpp"

#include "api/http.hpp"
#include "api/kick_auth.hpp"
#include "api/kick_playlist.hpp"
#include "api/kick_rewards.hpp"
#include "api/raw_websocket.hpp"
#include "utils/misc.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kick {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// Kick Pusher WebSocket endpoint (us2 cluster, public app key 32cbd69e4b950bf97679)
constexpr const char* DEFAULT_KICK_PUSHER_URL =
    "wss://ws-us2.pusher.com/app/32cbd69e4b950bf97679?protocol=7&client=js&version=8.4.0&flash=false";
constexpr size_t MAX_DEDUP_IDS = 128;

std::string settingsPath() {
#if defined(__SWITCH__)
    return "sdmc:/config/KickNX/settings.json";
#else
    return "settings.json";
#endif
}

std::string jsonFieldToString(const json& obj, const char* key, const std::string& fallback = "") {
    if (!obj.is_object() || !obj.contains(key)) return fallback;
    const auto& val = obj[key];
    if (val.is_string()) return val.get<std::string>();
    if (val.is_number_integer()) return std::to_string(val.get<long long>());
    if (val.is_number_unsigned()) return std::to_string(val.get<unsigned long long>());
    return fallback;
}

std::string kickEmoteImageUrl(const std::string& id) {
    if (id.empty()) return {};
    return "https://files.kick.com/emotes/" + id + "/fullsize";
}

// Kick names every collectible emote "collectibles…" (kick.com checks the
// same prefix), whichever group it arrives in.
std::string emoteCategory(const std::string& name, bool inChannelGroup) {
    if (name.rfind("collectibles", 0) == 0) return "collectible";
    return inChannelGroup ? "channel" : "global";
}

// Emote lists by channel and account, kept for the launch.
struct CachedEmotes {
    UserEmoteCatalogue catalogue;
    std::chrono::steady_clock::time_point loadedAt;
};
constexpr auto EMOTE_CACHE_LIFETIME = std::chrono::minutes(10);
std::mutex emoteCacheMutex;
std::unordered_map<std::string, CachedEmotes> emoteCache;

// GET /api/v2/channels/{slug}/me: "subscription" is an object while the
// account is subscribed and null otherwise.
bool isSubscribedResponse(const json& root) {
    if (!root.is_object()) return false;
    const json& data =
        root.contains("data") && root["data"].is_object() ? root["data"] : root;
    if (data.contains("subscription") &&
        data["subscription"].is_object() &&
        !data["subscription"].empty())
        return true;
    for (const char* key : {"is_subscribed", "subscribed"}) {
        if (data.contains(key) && data[key].is_boolean() && data[key].get<bool>())
            return true;
    }
    return false;
}

std::string utcTimestamp() {
    const auto now = std::time(nullptr);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &now);
#else
    gmtime_r(&now, &value);
#endif
    std::ostringstream output;
    output << std::put_time(&value, "%H:%M");
    return output.str();
}


std::string generateUuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis(0, 15);
    static const char* hexDigits = "0123456789abcdef";

    std::string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid.push_back('-');
        } else if (i == 14) {
            uuid.push_back('4'); // UUID v4
        } else if (i == 19) {
            uuid.push_back(hexDigits[(dis(gen) & 0x3) | 0x8]); // RFC 4122 variant
        } else {
            uuid.push_back(hexDigits[dis(gen)]);
        }
    }
    return uuid;
}

struct KickChatroomInfo {
    std::string channelId;
    std::string chatroomId;
    std::string provider = "centrifugo"; // "centrifugo" or "pusher"
    std::string centrifugoUrl = "wss://realtime.us-east-1.platform.kick.com/connection/websocket";
    std::string centrifugoToken;
    std::string pusherUrl = DEFAULT_KICK_PUSHER_URL;
};

// A channel's IDs never change, so each slug is looked up once per launch.
// Reconnecting chat and sending messages then skip that request (a new TLS
// connection each time, which the Switch can run short of).
struct KickChannelIds {
    std::string channelId;
    std::string chatroomId;
};

std::mutex channelIdCacheMutex;
std::unordered_map<std::string, KickChannelIds> channelIdCache;

KickChannelIds fetchChannelIds(const std::string& slug, HTTP::Cancel cancel) {
    {
        std::lock_guard<std::mutex> lock(channelIdCacheMutex);
        const auto found = channelIdCache.find(slug);
        if (found != channelIdCache.end()) return found->second;
    }

    const HTTP::Header headers = {
        "Accept: application/json",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };

    json root;
    try {
        root = json::parse(HTTP::get(
            "https://kick.com/api/v2/channels/" + percentEncode(slug),
            headers,
            HTTP::Timeout{15000},
            cancel));
    } catch (...) {
        root = json::parse(HTTP::get(
            "https://kick.com/api/v2/channels/" + percentEncode(slug) + "/info",
            headers,
            HTTP::Timeout{15000},
            cancel));
    }

    const json& data = (root.contains("data") && root["data"].is_object()) ? root["data"] : root;
    KickChannelIds ids;
    ids.channelId = jsonFieldToString(data, "id");
    if (data.contains("chatroom") && data["chatroom"].is_object()) {
        ids.chatroomId = jsonFieldToString(data["chatroom"], "id");
    }
    if (ids.chatroomId.empty()) {
        ids.chatroomId = jsonFieldToString(data, "chatroom_id", ids.channelId);
    }
    if (ids.chatroomId.empty()) {
        throw std::runtime_error("Kick channel returned no chatroom ID");
    }

    std::lock_guard<std::mutex> lock(channelIdCacheMutex);
    channelIdCache[slug] = ids;
    return ids;
}

KickChatroomInfo resolveKickChatroom(const std::string& channelSlug, HTTP::Cancel cancel = nullptr) {
    const std::string slug = normalizeChannel(channelSlug);
    if (slug.empty()) throw std::runtime_error("No Kick channel was specified");

    const HTTP::Header headers = {
        "Accept: application/json",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };

    const KickChannelIds ids = fetchChannelIds(slug, cancel);
    KickChatroomInfo info;
    info.channelId = ids.channelId;
    info.chatroomId = ids.chatroomId;

    // Negotiate realtime connection provider (Centrifugo vs Pusher)
    if (!info.channelId.empty()) {
        const std::string clientId = generateUuid();
        try {
            HTTP::Header postHeaders = headers;
            postHeaders.push_back("Content-Type: application/json");

            // Both web.kick.com calls go through one handle, so the token
            // request reuses the negotiation's connection instead of opening
            // another. The cancel flag lasts one request, so it is set for each.
            HTTP web;
            HTTP::set_option(web, postHeaders, HTTP::Timeout{8000});
            const auto webPost = [&](const std::string& url, const std::string& body) {
                if (cancel) HTTP::set_option(web, cancel);
                return web._post(url, body);
            };

            const json reqBody = {
                {"client", {{"id", clientId}, {"type", "web"}}},
                {"capabilities",
                 {{"accepted_providers",
                   json::array({{{"provider", "centrifugo"}}, {{"provider", "pusher"}}})}}},
            };

            std::string connResp;
            try {
                connResp = webPost(
                    "https://web.kick.com/api/v1/realtime/channels/" +
                        percentEncode(info.channelId) + "/chat/connection",
                    reqBody.dump());
            } catch (...) {
                connResp = webPost(
                    "https://web.kick.com/api/v1/realtime/connection",
                    reqBody.dump());
            }

            const json connRoot = json::parse(connResp);
            const json& connData = (connRoot.contains("data") && connRoot["data"].is_object())
                ? connRoot["data"]
                : connRoot;

            if (connData.contains("connections") && connData["connections"].is_array()) {
                for (const auto& connItem : connData["connections"]) {
                    if (!connItem.is_object()) continue;
                    const std::string provider = jsonFieldToString(connItem, "provider");
                    const auto& creds = connItem.value("credentials", json::object());

                    if (provider == "centrifugo") {
                        const std::string url = jsonFieldToString(creds, "url");
                        if (!url.empty()) info.centrifugoUrl = url;
                        info.provider = "centrifugo";

                        // Obtain Centrifugo connection token
                        try {
                            const json authBody = {{"client_id", clientId}};
                            const std::string authResp = webPost(
                                "https://web.kick.com/api/v1/realtime/auth/connection",
                                authBody.dump());
                            const json authRoot = json::parse(authResp);
                            if (authRoot.contains("data") && authRoot["data"].is_object()) {
                                const auto& inner = authRoot["data"];
                                if (inner.contains("data") && inner["data"].is_object()) {
                                    info.centrifugoToken = jsonFieldToString(inner["data"], "token");
                                } else {
                                    info.centrifugoToken = jsonFieldToString(inner, "token");
                                }
                            } else {
                                info.centrifugoToken = jsonFieldToString(authRoot, "token");
                            }
                        } catch (...) {
                        }
                        break;
                    } else if (provider == "pusher") {
                        const std::string appKey = jsonFieldToString(creds, "app_key");
                        const std::string cluster = jsonFieldToString(creds, "cluster");
                        if (!appKey.empty() && !cluster.empty()) {
                            info.pusherUrl = "wss://ws-" + cluster + ".pusher.com/app/" + appKey +
                                "?protocol=7&client=js&version=8.4.0&flash=false";
                            info.provider = "pusher";
                        }
                    }
                }
            } else {
                const std::string appKey = jsonFieldToString(connData, "app_key");
                const std::string cluster = jsonFieldToString(connData, "cluster");
                if (!appKey.empty() && !cluster.empty()) {
                    info.pusherUrl = "wss://ws-" + cluster + ".pusher.com/app/" + appKey +
                        "?protocol=7&client=js&version=8.4.0&flash=false";
                    info.provider = "pusher";
                }
            }
        } catch (...) {
            // Default to Centrifugo fallback or standard Pusher
        }
    }

    return info;
}

void appendTextAndMentions(
    const std::string& segment,
    std::vector<ChatFragment>& fragments) {
    if (segment.empty()) return;

    size_t cursor = 0;
    while (cursor < segment.size()) {
        const size_t atPos = segment.find('@', cursor);
        if (atPos == std::string::npos) {
            ChatFragment frag;
            frag.type = ChatFragmentType::Text;
            frag.text = segment.substr(cursor);
            fragments.push_back(std::move(frag));
            break;
        }

        const bool validStart =
            (atPos == 0) || std::isspace(static_cast<unsigned char>(segment[atPos - 1]));
        size_t endMention = atPos + 1;
        while (endMention < segment.size()) {
            const unsigned char c = static_cast<unsigned char>(segment[endMention]);
            if (!(std::isalnum(c) || c == '_' || c == '-')) break;
            ++endMention;
        }

        if (!validStart || endMention <= atPos + 1) {
            ChatFragment frag;
            frag.type = ChatFragmentType::Text;
            frag.text = segment.substr(cursor, (atPos - cursor) + 1);
            fragments.push_back(std::move(frag));
            cursor = atPos + 1;
            continue;
        }

        if (atPos > cursor) {
            ChatFragment before;
            before.type = ChatFragmentType::Text;
            before.text = segment.substr(cursor, atPos - cursor);
            fragments.push_back(std::move(before));
        }

        ChatFragment mention;
        mention.type = ChatFragmentType::Mention;
        mention.text = segment.substr(atPos, endMention - atPos);
        fragments.push_back(std::move(mention));
        cursor = endMention;
    }
}

void parseKickContentFragments(
    const std::string& rawContent,
    std::string& plainTextOut,
    std::vector<ChatFragment>& fragmentsOut) {
    plainTextOut.clear();
    fragmentsOut.clear();

    static constexpr const char* PREFIX = "[emote:";
    constexpr size_t PREFIX_LEN = 7;

    size_t cursor = 0;
    while (cursor < rawContent.size()) {
        const size_t start = rawContent.find(PREFIX, cursor);
        if (start == std::string::npos) {
            const std::string tail = rawContent.substr(cursor);
            plainTextOut += tail;
            appendTextAndMentions(tail, fragmentsOut);
            break;
        }

        const size_t colon = rawContent.find(':', start + PREFIX_LEN);
        const size_t closeBracket =
            (colon != std::string::npos) ? rawContent.find(']', colon + 1) : std::string::npos;

        if (colon == std::string::npos || closeBracket == std::string::npos) {
            const std::string tail = rawContent.substr(cursor);
            plainTextOut += tail;
            appendTextAndMentions(tail, fragmentsOut);
            break;
        }

        if (start > cursor) {
            const std::string before = rawContent.substr(cursor, start - cursor);
            plainTextOut += before;
            appendTextAndMentions(before, fragmentsOut);
        }

        const std::string emoteId =
            rawContent.substr(start + PREFIX_LEN, colon - (start + PREFIX_LEN));
        const std::string emoteName =
            rawContent.substr(colon + 1, closeBracket - (colon + 1));

        if (!emoteId.empty() && !emoteName.empty()) {
            ChatFragment emoteFrag;
            emoteFrag.type = ChatFragmentType::Emote;
            emoteFrag.emoteId = emoteId;
            emoteFrag.text = emoteName;
            emoteFrag.emoteUrl = kickEmoteImageUrl(emoteId);
            emoteFrag.animatedEmoteUrl = emoteFrag.emoteUrl;
            fragmentsOut.push_back(std::move(emoteFrag));
            plainTextOut += emoteName;
        }

        cursor = closeBracket + 1;
    }

    if (fragmentsOut.empty() && !plainTextOut.empty()) {
        ChatFragment fallback;
        fallback.type = ChatFragmentType::Text;
        fallback.text = plainTextOut;
        fragmentsOut.push_back(std::move(fallback));
    }
}

bool recordMessageId(
    const std::string& id,
    std::deque<std::string>& recentIds,
    std::set<std::string>& recentIdSet) {
    if (id.empty()) return true;
    if (recentIdSet.count(id)) return false;
    recentIdSet.insert(id);
    recentIds.push_back(id);
    while (recentIds.size() > MAX_DEDUP_IDS) {
        recentIdSet.erase(recentIds.front());
        recentIds.pop_front();
    }
    return true;
}

void dispatchKickChatEvent(
    const std::string& event,
    const json& eventData,
    std::deque<std::string>& recentIds,
    std::set<std::string>& recentIdSet,
    const ChatMessageCallback& messageCallback) {
    if (!eventData.is_object()) return;

    if (event == "App\\Events\\ChatMessageEvent" ||
        event.find("ChatMessageEvent") != std::string::npos) {
        const std::string messageId = jsonFieldToString(eventData, "id");
        if (!recordMessageId(messageId, recentIds, recentIdSet)) return;

        ChatMessage message;
        message.id = messageId;
        message.timestamp = utcTimestamp();
        message.color = "#53FC18";

        if (eventData.contains("sender") && eventData["sender"].is_object()) {
            const auto& sender = eventData["sender"];
            message.userName = jsonFieldToString(
                sender, "username", jsonFieldToString(sender, "slug", "viewer"));

            if (sender.contains("identity") && sender["identity"].is_object()) {
                const auto& identity = sender["identity"];
                const std::string senderColor = jsonFieldToString(identity, "color");
                if (!senderColor.empty()) message.color = senderColor;
            }
        } else {
            message.userName = "viewer";
        }

        // Text in scripts the Switch fonts lack becomes a short note; emote
        // tokens are ASCII, so they come through unchanged.
        const std::string rawContent =
            misc::replaceUnsupportedText(jsonFieldToString(eventData, "content"));
        parseKickContentFragments(rawContent, message.text, message.fragments);

        if (!message.text.empty() && messageCallback) {
            messageCallback(std::move(message));
        }
        return;
    }

    if (event == "App\\Events\\SubscriptionEvent" ||
        event == "App\\Events\\GiftedSubscriptionsEvent" ||
        event == "App\\Events\\LuckyUsersWhoGotGiftSubscriptionsEvent" ||
        event.find("Subscription") != std::string::npos) {
        ChatMessage subMessage;
        subMessage.id = "sub-" + std::to_string(std::time(nullptr)) + "-" +
            jsonFieldToString(eventData, "username", "kick");
        subMessage.userName = "Kick Sub";
        subMessage.color = "#53FC18";
        subMessage.timestamp = utcTimestamp();

        const std::string subUser = jsonFieldToString(
            eventData, "username", jsonFieldToString(eventData, "gifter_username"));
        const int months = eventData.value("months", 1);

        if (event.find("Gifted") != std::string::npos ||
            event.find("Gift") != std::string::npos) {
            size_t giftCount = 1;
            if (eventData.contains("gifted_usernames") &&
                eventData["gifted_usernames"].is_array()) {
                giftCount = eventData["gifted_usernames"].size();
            }
            subMessage.text = (subUser.empty() ? "An anonymous gifter" : subUser) +
                " gifted " + std::to_string(giftCount) +
                (giftCount == 1 ? " subscription!" : " subscriptions!");
        } else if (!subUser.empty()) {
            subMessage.text = subUser + " subscribed" +
                (months > 1 ? (" for " + std::to_string(months) + " months!") : "!");
        }

        if (!subMessage.text.empty() && messageCallback) {
            ChatFragment frag;
            frag.type = ChatFragmentType::Text;
            frag.text = subMessage.text;
            subMessage.fragments.push_back(std::move(frag));
            messageCallback(std::move(subMessage));
        }
    }
}

class CentrifugoEventListener {
public:
    CentrifugoEventListener(
        const KickChatroomInfo& chatroom,
        ChatMessageCallback onMessage,
        ChatStatusCallback onStatus,
        std::atomic_bool& stopRequested)
        : chatroomInfo(chatroom),
          socket(chatroom.centrifugoUrl, stopRequested),
          messageCallback(std::move(onMessage)),
          statusCallback(std::move(onStatus)) {}

    void listen(
        std::atomic_bool& stopRequested,
        std::deque<std::string>& recentIds,
        std::set<std::string>& recentIdSet) {
        // 1. Send connect command
        json connectFrame = {
            {"id", 1},
            {"connect", json::object()}
        };
        if (!chatroomInfo.centrifugoToken.empty()) {
            connectFrame["connect"]["token"] = chatroomInfo.centrifugoToken;
        }
        socket.sendText(connectFrame.dump());

        // 2. Send subscribe command to the chatroom channel
        const std::string channelName = "chatrooms." + chatroomInfo.chatroomId + ".v2";
        json subscribeFrame = {
            {"id", 2},
            {"subscribe", {{"channel", channelName}}}
        };
        socket.sendText(subscribeFrame.dump());

        std::string raw;
        while (!stopRequested.load()) {
            const bool received = socket.nextText(
                raw,
                stopRequested,
                [this]() { sendPong(); });
            if (!received) break;

            handleFrame(raw, recentIds, recentIdSet);
        }
    }

    void close() {
        socket.close();
    }

private:
    void sendPong() {
        try {
            socket.sendText("{}");
        } catch (...) {
        }
    }

    void handleFrame(
        const std::string& raw,
        std::deque<std::string>& recentIds,
        std::set<std::string>& recentIdSet) {
        if (raw.empty() || raw == "{}" || raw == "{}\n") {
            sendPong();
            return;
        }

        json root;
        try {
            root = json::parse(raw);
        } catch (...) {
            return;
        }
        if (!root.is_object() || root.empty()) {
            sendPong();
            return;
        }

        if (root.contains("id") && root["id"] == 1) {
            if (root.contains("error") && root["error"].is_object()) {
                const std::string msg = jsonFieldToString(root["error"], "message", "Centrifugo connect error");
                throw std::runtime_error(msg);
            }
            if (statusCallback) statusCallback("Live chat");
            return;
        }

        if (root.contains("id") && root["id"] == 2) {
            if (root.contains("error") && root["error"].is_object()) {
                const std::string msg = jsonFieldToString(root["error"], "message", "Centrifugo subscribe error");
                throw std::runtime_error(msg);
            }
            if (statusCallback) statusCallback("Live chat");
            return;
        }

        if (root.contains("push") && root["push"].is_object()) {
            if (statusCallback) statusCallback("Live chat");
            const auto& push = root["push"];

            if (push.contains("pub") && push["pub"].is_object()) {
                const auto& pub = push["pub"];
                if (pub.contains("data")) {
                    json envelope;
                    const auto& pubData = pub["data"];
                    if (pubData.is_object()) {
                        envelope = pubData;
                    } else if (pubData.is_string()) {
                        try {
                            envelope = json::parse(pubData.get<std::string>());
                        } catch (...) {
                        }
                    }

                    if (envelope.is_object()) {
                        const std::string event = jsonFieldToString(envelope, "event");
                        json eventData;
                        if (envelope.contains("data")) {
                            const auto& inner = envelope["data"];
                            if (inner.is_object()) {
                                eventData = inner;
                            } else if (inner.is_string()) {
                                try {
                                    eventData = json::parse(inner.get<std::string>());
                                } catch (...) {
                                }
                            }
                        }

                        dispatchKickChatEvent(
                            event,
                            eventData,
                            recentIds,
                            recentIdSet,
                            messageCallback);
                    }
                }
            } else if (push.contains("disconnect") && push["disconnect"].is_object()) {
                const std::string reason = jsonFieldToString(push["disconnect"], "reason", "Centrifugo disconnected");
                throw std::runtime_error(reason);
            }
        }
    }

    KickChatroomInfo chatroomInfo;
    RawWebSocket socket;
    ChatMessageCallback messageCallback;
    ChatStatusCallback statusCallback;
};

// Lightweight Pusher WebSocket Event Listener wrapper for Kick chatrooms
class PusherEventListener {
public:
    PusherEventListener(
        const KickChatroomInfo& chatroom,
        ChatMessageCallback onMessage,
        ChatStatusCallback onStatus,
        std::atomic_bool& stopRequested)
        : chatroomInfo(chatroom),
          socket(chatroom.pusherUrl, stopRequested),
          messageCallback(std::move(onMessage)),
          statusCallback(std::move(onStatus)) {}

    void listen(
        std::atomic_bool& stopRequested,
        std::deque<std::string>& recentIds,
        std::set<std::string>& recentIdSet) {
        std::string raw;
        while (!stopRequested.load()) {
            const bool received = socket.nextText(
                raw,
                stopRequested,
                [this]() { sendKeepAlivePing(); });
            if (!received) break;

            handleFrame(raw, recentIds, recentIdSet);
        }
    }

    void close() {
        socket.close();
    }

private:
    void subscribeChannel(const std::string& channelName) {
        if (channelName.empty()) return;
        const json subscribeFrame = {
            {"event", "pusher:subscribe"},
            {"data",
             {
                 {"auth", ""},
                 {"channel", channelName},
             }},
        };
        socket.sendText(subscribeFrame.dump());
    }

    void sendKeepAlivePing() {
        socket.sendText("{\"event\":\"pusher:ping\",\"data\":{}}");
    }

    void sendKeepAlivePong() {
        socket.sendText("{\"event\":\"pusher:pong\",\"data\":{}}");
    }

    static json parsePusherDataField(const json& root) {
        if (!root.contains("data")) return json::object();
        const auto& dataField = root["data"];
        if (dataField.is_object()) return dataField;
        if (dataField.is_string()) {
            const std::string rawData = dataField.get<std::string>();
            if (!rawData.empty()) {
                try {
                    return json::parse(rawData);
                } catch (...) {
                }
            }
        }
        return json::object();
    }

    void handleFrame(
        const std::string& raw,
        std::deque<std::string>& recentIds,
        std::set<std::string>& recentIdSet) {
        json root;
        try {
            root = json::parse(raw);
        } catch (...) {
            return;
        }
        if (!root.is_object()) return;

        const std::string event = jsonFieldToString(root, "event");

        // 1. Pusher connection handshake -> subscribe to Kick chatroom channels
        if (event == "pusher:connection_established") {
            subscribeChannel("chatrooms." + chatroomInfo.chatroomId + ".v2");
            subscribeChannel("chatrooms." + chatroomInfo.chatroomId);
            if (!chatroomInfo.channelId.empty()) {
                subscribeChannel("channel." + chatroomInfo.channelId);
            }
            if (statusCallback) statusCallback("Live chat");
            return;
        }

        // 2. Pusher subscription acknowledgment
        if (event == "pusher_internal:subscription_succeeded" ||
            event == "pusher:subscription_succeeded") {
            subscribed = true;
            if (statusCallback) statusCallback("Live chat");
            return;
        }

        // 3. Pusher keep-alive ping/pong handling
        if (event == "pusher:ping") {
            sendKeepAlivePong();
            return;
        }
        if (event == "pusher:pong") {
            if (subscribed && statusCallback) statusCallback("Live chat");
            return;
        }

        // 4. Pusher error frame
        if (event == "pusher:error") {
            const json errorData = parsePusherDataField(root);
            const std::string message = jsonFieldToString(
                errorData, "message", "Kick Pusher chat error");
            throw std::runtime_error(message);
        }

        // 5. Kick ChatMessageEvent & Subscriptions
        if (event == "App\\Events\\ChatMessageEvent" ||
            event.find("ChatMessageEvent") != std::string::npos ||
            event == "App\\Events\\SubscriptionEvent" ||
            event == "App\\Events\\GiftedSubscriptionsEvent" ||
            event == "App\\Events\\LuckyUsersWhoGotGiftSubscriptionsEvent" ||
            event.find("Subscription") != std::string::npos) {
            if (statusCallback) statusCallback("Live chat");
            const json eventData = parsePusherDataField(root);
            dispatchKickChatEvent(event, eventData, recentIds, recentIdSet, messageCallback);
        }
    }

    KickChatroomInfo chatroomInfo;
    RawWebSocket socket;
    ChatMessageCallback messageCallback;
    ChatStatusCallback statusCallback;
    bool subscribed = false;
};

}  // namespace

ChatPreferences loadChatPreferences() {
    ChatPreferences result;
    std::ifstream input(settingsPath());
    if (!input.is_open()) return result;

    try {
        const json root = json::parse(input);
        if (!root.is_object()) return result;
        const int mode = root.value(
            "chat_mode",
            static_cast<int>(ChatMode::Overlay));
        if (mode >= static_cast<int>(ChatMode::Off) &&
            mode <= static_cast<int>(ChatMode::Overlay))
            result.mode = static_cast<ChatMode>(mode);
        result.fontSize = std::clamp(root.value("chat_font_size", 16), 12, 22);
        result.opacity = std::clamp(root.value("chat_opacity", 40), 20, 100);
        result.dockedWidth = std::clamp(
            root.value("chat_docked_width", 330), 280, 420);
        result.overlayWidth = std::clamp(
            root.value("chat_overlay_width", 360), 280, 480);
        const int participation = root.value(
            "chat_participation",
            static_cast<int>(ChatParticipation::ReadOnly));
        if (participation >= static_cast<int>(ChatParticipation::ReadOnly) &&
            participation <= static_cast<int>(ChatParticipation::Interactive))
            result.participation =
                static_cast<ChatParticipation>(participation);

        const int composerMode = root.value(
            "chat_composer_mode",
            static_cast<int>(ChatComposerMode::KickNXComposer));
        if (composerMode >=
                static_cast<int>(ChatComposerMode::NintendoKeyboard) &&
            composerMode <=
                static_cast<int>(ChatComposerMode::KickNXComposer))
            result.composerMode =
                static_cast<ChatComposerMode>(composerMode);

        const int emoteMode = root.value(
            "chat_emote_mode",
            static_cast<int>(ChatEmoteMode::Static));
        if (emoteMode >= static_cast<int>(ChatEmoteMode::Static) &&
            emoteMode <= static_cast<int>(ChatEmoteMode::Animated))
            result.emoteMode = static_cast<ChatEmoteMode>(emoteMode);

        const int overlaySize = root.value(
            "chat_overlay_size",
            static_cast<int>(ChatOverlaySize::FullHeight));
        if (overlaySize >= static_cast<int>(ChatOverlaySize::FullHeight) &&
            overlaySize <= static_cast<int>(ChatOverlaySize::Compact))
            result.overlaySize = static_cast<ChatOverlaySize>(overlaySize);

        const int overlayPosition = root.value(
            "chat_overlay_position",
            static_cast<int>(ChatOverlayPosition::TopRight));
        if (overlayPosition >=
                static_cast<int>(ChatOverlayPosition::TopRight) &&
            overlayPosition <=
                static_cast<int>(ChatOverlayPosition::BottomLeft))
            result.overlayPosition =
                static_cast<ChatOverlayPosition>(overlayPosition);

        result.timestamps = root.value("chat_timestamps", false);
        result.watchRewards = root.value("watch_rewards", true);
    } catch (...) {
    }
    return result;
}

bool saveChatPreferences(const ChatPreferences& preferences) {
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

        root["chat_mode"] = static_cast<int>(preferences.mode);
        root["chat_participation"] =
            static_cast<int>(preferences.participation);
        root["chat_composer_mode"] =
            static_cast<int>(preferences.composerMode);
        root["chat_emote_mode"] =
            static_cast<int>(preferences.emoteMode);
        root["chat_overlay_size"] =
            static_cast<int>(preferences.overlaySize);
        root["chat_overlay_position"] =
            static_cast<int>(preferences.overlayPosition);
        root["chat_font_size"] = preferences.fontSize;
        root["chat_opacity"] = preferences.opacity;
        root["chat_docked_width"] = preferences.dockedWidth;
        root["chat_overlay_width"] = preferences.overlayWidth;
        root["chat_timestamps"] = preferences.timestamps;
        root["watch_rewards"] = preferences.watchRewards;

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

        chatPreferencesEvent()->fire(preferences);
        return true;
    } catch (...) {
        return false;
    }
}

brls::Event<ChatPreferences>* chatPreferencesEvent() {
    static brls::Event<ChatPreferences> event;
    return &event;
}

bool hasUserEmotesScope() {
    // Kick global and channel emotes are publicly accessible via /emotes/{slug}
    return true;
}

std::vector<UserEmote> loadRecentEmotes() {
    std::ifstream input(settingsPath());
    if (!input.is_open()) return {};

    try {
        const json root = json::parse(input);
        if (!root.is_object() ||
            !root.contains("chat_recent_emotes") ||
            !root["chat_recent_emotes"].is_array())
            return {};

        std::vector<UserEmote> result;
        for (const auto& value : root["chat_recent_emotes"]) {
            UserEmote emote;
            if (value.is_string()) {
                emote.name = value.get<std::string>();
            } else if (value.is_object()) {
                emote.id = jsonFieldToString(value, "id");
                emote.name = jsonFieldToString(value, "name");
                emote.ownerId = jsonFieldToString(value, "owner_id");
                emote.emoteType = jsonFieldToString(value, "emote_type");
                emote.imageUrl = jsonFieldToString(value, "image_url");
                emote.channelEmote = value.value("channel_emote", false);
            }

            if (emote.name.empty()) continue;
            result.push_back(std::move(emote));
            if (result.size() >= 24) break;
        }

        return result;
    } catch (...) {
        return {};
    }
}

void rememberRecentEmote(const UserEmote& emote) {
    if (emote.name.empty()) return;

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

        auto serialize = [](const UserEmote& value) {
            return json{
                {"id", value.id},
                {"name", value.name},
                {"owner_id", value.ownerId},
                {"emote_type", value.emoteType},
                {"image_url", value.imageUrl},
                {"channel_emote", value.channelEmote},
            };
        };

        json saved = json::array();
        saved.push_back(serialize(emote));

        if (root.contains("chat_recent_emotes") &&
            root["chat_recent_emotes"].is_array()) {
            for (const auto& value : root["chat_recent_emotes"]) {
                UserEmote current;
                if (value.is_string()) {
                    current.name = value.get<std::string>();
                } else if (value.is_object()) {
                    current.id = jsonFieldToString(value, "id");
                    current.name = jsonFieldToString(value, "name");
                    current.ownerId = jsonFieldToString(value, "owner_id");
                    current.emoteType = jsonFieldToString(value, "emote_type");
                    current.imageUrl = jsonFieldToString(value, "image_url");
                    current.channelEmote = value.value("channel_emote", false);
                }

                if (current.name.empty()) continue;

                const bool duplicate =
                    (!emote.id.empty() && !current.id.empty() && emote.id == current.id) ||
                    current.name == emote.name;
                if (duplicate) continue;

                saved.push_back(serialize(current));
                if (saved.size() >= 24) break;
            }
        }

        root["chat_recent_emotes"] = std::move(saved);

        const fs::path temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output.is_open()) return;
            output << root.dump(2) << '\n';
            if (!output.good()) return;
        }

        std::error_code error;
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        if (error) fs::remove(temporary, error);
    } catch (...) {
    }
}

void forgetCachedEmotes() {
    std::lock_guard<std::mutex> lock(emoteCacheMutex);
    emoteCache.clear();
}

void loadUserEmotesAsync(
    const std::string& channel,
    UserEmotesCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [channel,
         success = std::move(success),
         failure = std::move(failure)](HTTP&) mutable {
            try {
                const std::string slug = normalizeChannel(channel);
                if (slug.empty()) {
                    throw std::runtime_error("No Kick channel is active");
                }

                // Signed in, Kick also returns the emotes the account has
                // unlocked, such as collectibles.
                HTTP::Header headers = {
                    "Accept: application/json",
                    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36",
                    "x-app-platform: web",
                    "Origin: https://kick.com",
                    "Referer: https://kick.com/",
                };
                const OAuthSession session = loadOAuth();
                if (!session.accessToken.empty()) {
                    headers.push_back("Authorization: Bearer " + session.accessToken);
                    headers.push_back("Cookie: session_token=" + session.accessToken);
                }

                // Reopening the keyboard on the same channel reuses the last
                // list instead of asking Kick again.
                const auto now = std::chrono::steady_clock::now();
                const std::string cacheKey = slug + '\n' + session.accessToken;
                std::optional<UserEmoteCatalogue> cached;
                {
                    std::lock_guard<std::mutex> lock(emoteCacheMutex);
                    const auto found = emoteCache.find(cacheKey);
                    if (found != emoteCache.end()) {
                        if (now - found->second.loadedAt < EMOTE_CACHE_LIFETIME) {
                            brls::sync(std::bind(success, found->second.catalogue));
                            return;
                        }
                        cached = found->second.catalogue;
                    }
                }

                // One handle for both requests, so the emote list reuses the
                // subscription check's connection to kick.com.
                HTTP http;
                HTTP::set_option(http, headers, HTTP::Timeout{15000});

                // Subscriber emotes are offered only when Kick confirms this
                // account is subscribed; any failure here leaves them out.
                bool subscribed = false;
                if (!session.accessToken.empty()) {
                    try {
                        std::ostringstream body;
                        http._get(
                            "https://kick.com/api/v2/channels/" + percentEncode(slug) + "/me",
                            &body);
                        subscribed = isSubscribedResponse(json::parse(body.str()));
                    } catch (...) {
                    }
                }

                // Right after the Switch wakes, new TLS connections can fail
                // for a few minutes. Try again a couple of times before giving
                // up; Kick answering with an HTTP error is not retried.
                std::string response;
                for (int attempt = 1;; ++attempt) {
                    try {
                        std::ostringstream body;
                        http._get("https://kick.com/emotes/" + percentEncode(slug), &body);
                        response = body.str();
                        break;
                    } catch (const std::exception& exception) {
                        const bool httpStatus =
                            std::string(exception.what()).find("http status") != std::string::npos;
                        if (httpStatus || attempt >= 3) {
                            if (cached) {
                                brls::sync(std::bind(success, *cached));
                                return;
                            }
                            throw;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
                    }
                }
                const json root = json::parse(response);

                std::vector<UserEmote> emotes;
                std::set<std::string> seen;
                size_t lockedChannelEmotes = 0;
                constexpr size_t MAX_EMOTES = 400;

                if (root.is_array()) {
                    for (const auto& group : root) {
                        if (!group.is_object() ||
                            !group.contains("emotes") ||
                            !group["emotes"].is_array())
                            continue;

                        // Channel groups carry a slug: this channel's, plus
                        // one per channel the account is subscribed to, which
                        // are left out. The shared groups ("Global", "Emoji",
                        // "Collectibles") have none.
                        const std::string groupId = jsonFieldToString(group, "id");
                        const std::string groupSlug = jsonFieldToString(group, "slug");
                        if (!groupSlug.empty() && normalizeChannel(groupSlug) != slug)
                            continue;
                        const bool isChannelGroup = !groupSlug.empty();

                        for (const auto& item : group["emotes"]) {
                            if (!item.is_object()) continue;
                            const bool subscribersOnly = item.value("subscribers_only", false);
                            if (subscribersOnly && !(subscribed && isChannelGroup)) {
                                if (isChannelGroup) lockedChannelEmotes++;
                                continue;
                            }

                            UserEmote emote;
                            emote.id = jsonFieldToString(item, "id");
                            emote.name = jsonFieldToString(item, "name");
                            emote.ownerId = groupId;
                            emote.emoteType = emoteCategory(emote.name, isChannelGroup);
                            emote.channelEmote = emote.emoteType == "channel";
                            emote.imageUrl = kickEmoteImageUrl(emote.id);

                            if (emote.id.empty() ||
                                emote.name.empty() ||
                                !seen.insert(emote.id).second)
                                continue;

                            emotes.push_back(std::move(emote));
                            if (emotes.size() >= MAX_EMOTES) break;
                        }
                        if (emotes.size() >= MAX_EMOTES) break;
                    }
                }

                UserEmoteCatalogue catalogue;
                catalogue.emotes = std::move(emotes);
                catalogue.channelSubscribed = subscribed;
                catalogue.lockedChannelEmotes = lockedChannelEmotes;
                catalogue.subscriptionPermissionGranted = true;

                // A list without channel emotes is not kept, so a reply that
                // came back short is asked for again next time.
                const bool hasChannelEmotes = std::any_of(
                    catalogue.emotes.begin(), catalogue.emotes.end(),
                    [](const UserEmote& emote) { return emote.emoteType == "channel"; });
                if (hasChannelEmotes) {
                    std::lock_guard<std::mutex> lock(emoteCacheMutex);
                    if (emoteCache.size() >= 16) emoteCache.clear();
                    emoteCache[cacheKey] = {catalogue, now};
                }

                brls::sync(std::bind(success, std::move(catalogue)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("Unknown Kick emote error")));
            }
        });
}

bool hasChatWriteScope() {
    const OAuthSession session = loadOAuth();
    return !session.accessToken.empty();
}

void sendChatMessageAsync(
    const std::string& channel,
    const std::string& message,
    ChatSendCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [channel, message, success = std::move(success),
            failure = std::move(failure)](HTTP&) mutable {
            try {
                const std::string normalized = normalizeChannel(channel);
                if (normalized.empty())
                    throw std::runtime_error("No Kick channel is active");
                if (message.empty())
                    throw std::runtime_error("The chat message is empty");
                if (message.size() > 500)
                    throw std::runtime_error(
                        "Kick chat messages are limited to 500 characters");

                const OAuthSession session = loadOAuth();
                if (session.accessToken.empty())
                    throw std::runtime_error(
                        "Sending Kick chat messages requires a Bearer token in oauth.json");

                // Sending needs only the chatroom ID, not a chat connection.
                const KickChannelIds chatroom = fetchChannelIds(normalized, nullptr);
                const json body = {
                    {"content", message},
                    {"type", "message"},
                };

                const HTTP::Header headers = {
                    "Authorization: Bearer " + session.accessToken,
                    "Content-Type: application/json",
                    "Accept: application/json",
                    "x-app-platform: web",
                    "Origin: https://kick.com",
                    "Referer: https://kick.com/",
                };

                // A failed TLS connect means nothing reached Kick, so the
                // send is retried (twice, a moment apart) without risking a
                // duplicate message.
                std::string response;
                for (int attempt = 1;; ++attempt) {
                    try {
                        response = HTTP::post(
                            "https://kick.com/api/v2/messages/send/" +
                                percentEncode(chatroom.chatroomId),
                            body.dump(),
                            headers,
                            HTTP::Timeout{15000});
                        break;
                    } catch (const std::exception& exception) {
                        const std::string what = exception.what();
                        const bool connectFailed =
                            what.find("SSL connect") != std::string::npos ||
                            what.find("Couldn't connect") != std::string::npos;
                        if (!connectFailed || attempt >= 3) {
                            if (connectFailed) noteConnectionTrouble();
                            throw;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1500 * attempt));
                    }
                }

                const json root = json::parse(response);
                std::string messageId;
                if (root.contains("data") && root["data"].is_object()) {
                    messageId = jsonFieldToString(root["data"], "id");
                }
                brls::sync(std::bind(success, messageId));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(
                    failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(
                    failure,
                    std::string("Unknown Kick chat-send error")));
            }
        });
}

int fetchActiveChattersCount(const std::string& channelIdOrSlug, HTTP::Cancel cancel) {
    if (channelIdOrSlug.empty()) return 0;

    const HTTP::Header headers = {
        "Accept: application/json",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
    };

    std::string numericId = channelIdOrSlug;
    const bool isNumeric = !numericId.empty() && std::all_of(numericId.begin(), numericId.end(), ::isdigit);
    if (!isNumeric) {
        try {
            const KickChannelIds ids =
                fetchChannelIds(normalizeChannel(channelIdOrSlug), cancel);
            if (!ids.channelId.empty()) numericId = ids.channelId;
        } catch (...) {
        }
    }

    std::string response;
    try {
        response = HTTP::get(
            "https://web.kick.com/api/v1/channels/" +
                percentEncode(numericId) + "/chat/active-chatters",
            headers,
            HTTP::Timeout{8000},
            cancel);
    } catch (...) {
        try {
            response = HTTP::get(
                "https://kick.com/api/v2/channels/" +
                    percentEncode(channelIdOrSlug) + "/chat/active-chatters",
                headers,
                HTTP::Timeout{8000},
                cancel);
        } catch (...) {
            return 0;
        }
    }

    try {
        const json root = json::parse(response);

        const json& data = (root.contains("data") && root["data"].is_object())
            ? root["data"]
            : root;

        // 1. Check data["total_count"] (the exact key Kick returns for active chatters total)
        for (const char* key : {
            "total_count", "total", "count", "total_chatters", "chatters_count",
            "total_active_chatters", "total_active", "active_chatters"
        }) {
            if (data.contains(key) && data[key].is_number_integer()) {
                const int count = data[key].get<int>();
                if (count > 0) return count;
            }
            if (root.contains(key) && root[key].is_number_integer()) {
                const int count = root[key].get<int>();
                if (count > 0) return count;
            }
        }

        // 2. Check meta.total_count or meta.total
        if (root.contains("meta") && root["meta"].is_object()) {
            const auto& meta = root["meta"];
            for (const char* key : {
                "total_count", "total", "count", "total_chatters", "chatters_count",
                "total_active_chatters", "total_active"
            }) {
                if (meta.contains(key) && meta[key].is_number_integer()) {
                    const int count = meta[key].get<int>();
                    if (count > 0) return count;
                }
            }
        }

        // 3. Fallback: Count members across arrays inside data (moderators, vips, chatters, bots, etc.)
        int total = 0;
        std::set<std::string> uniqueUsers;
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (it.value().is_array()) {
                for (const auto& user : it.value()) {
                    if (user.is_object()) {
                        const std::string slug = jsonFieldToString(user, "slug");
                        const std::string username = jsonFieldToString(user, "username");
                        if (!slug.empty()) {
                            uniqueUsers.insert(slug);
                        } else if (!username.empty()) {
                            uniqueUsers.insert(username);
                        } else {
                            total++;
                        }
                    }
                }
            }
        }

        if (!uniqueUsers.empty()) {
            return static_cast<int>(uniqueUsers.size() + total);
        }
        return total;
    } catch (...) {
        return 0;
    }
}

ChatClient::ChatClient(
    std::string channel,
    ChatMessageCallback message,
    ChatStatusCallback status,
    ChattersCountCallback chatters)
    : channel(normalizeChannel(std::move(channel))),
      messageCallback(std::move(message)),
      statusCallback(std::move(status)),
      chattersCallback(std::move(chatters)) {
#ifdef BOREALIS_USE_STD_THREAD
    thread = std::make_shared<std::thread>(&ChatClient::threadEntry, this);
    if (chattersCallback) {
        chattersThread = std::make_shared<std::thread>(
            &ChatClient::chattersThreadEntry, this);
    }
#else
    if (pthread_create(&thread, nullptr, &ChatClient::threadEntry, this) == 0)
        threadStarted = true;
    if (chattersCallback &&
        pthread_create(
            &chattersThread,
            nullptr,
            &ChatClient::chattersThreadEntry,
            this) == 0)
        chattersThreadStarted = true;
#endif
}

ChatClient::~ChatClient() {
    stop();
}

void ChatClient::stop() {
    if (stopRequested.exchange(true)) return;
    cancelRequests->store(true);
#ifdef BOREALIS_USE_STD_THREAD
    if (thread && thread->joinable()) thread->join();
    thread.reset();
    if (chattersThread && chattersThread->joinable()) chattersThread->join();
    chattersThread.reset();
#else
    if (threadStarted) {
        pthread_join(thread, nullptr);
        threadStarted = false;
    }
    if (chattersThreadStarted) {
        pthread_join(chattersThread, nullptr);
        chattersThreadStarted = false;
    }
#endif
}

void* ChatClient::threadEntry(void* context) {
    static_cast<ChatClient*>(context)->run();
    return nullptr;
}

void* ChatClient::chattersThreadEntry(void* context) {
    static_cast<ChatClient*>(context)->chattersRun();
    return nullptr;
}

void ChatClient::chattersRun() {
    // Wait up to 2.5s for initial channelId resolution
    for (int i = 0; i < 25 && !stopRequested.load(); ++i) {
        {
            std::lock_guard<std::mutex> lock(channelIdMutex);
            if (!resolvedChannelId.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    while (!stopRequested.load()) {
        std::string targetId;
        {
            std::lock_guard<std::mutex> lock(channelIdMutex);
            targetId = resolvedChannelId;
        }
        if (targetId.empty()) {
            targetId = channel;
        }

        // The chatter count is optional; skip it while connections fail.
        if (!targetId.empty() &&
            !connectionTroubleWithin(std::chrono::seconds(60))) {
            const int count = fetchActiveChattersCount(targetId, cancelRequests);
            if (count > 0 && chattersCallback && !stopRequested.load()) {
                chattersCallback(count);
            }
        }

        // Sleep 45 seconds in 200ms intervals so stopRequested triggers prompt shutdown
        for (int i = 0; i < 225 && !stopRequested.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

void ChatClient::run() {
    int retrySeconds = 1;
    std::deque<std::string> recentIds;
    std::set<std::string> recentIdSet;

    while (!stopRequested.load()) {
        try {
            if (statusCallback) statusCallback("Connecting to Kick chat…");

            const KickChatroomInfo chatroom = resolveKickChatroom(channel, cancelRequests);
            if (stopRequested.load()) break;

            {
                std::lock_guard<std::mutex> lock(channelIdMutex);
                resolvedChannelId = !chatroom.channelId.empty()
                    ? chatroom.channelId
                    : chatroom.chatroomId;
            }

            if (chatroom.provider == "centrifugo" && !chatroom.centrifugoUrl.empty()) {
                CentrifugoEventListener listener(
                    chatroom,
                    messageCallback,
                    statusCallback,
                    stopRequested);
                retrySeconds = 1;
                listener.listen(stopRequested, recentIds, recentIdSet);
            } else {
                PusherEventListener listener(
                    chatroom,
                    messageCallback,
                    statusCallback,
                    stopRequested);
                retrySeconds = 1;
                listener.listen(stopRequested, recentIds, recentIdSet);
            }
        } catch (const std::exception& exception) {
            if (stopRequested.load()) break;
            // Tells optional background work (rewards, watch time) to hold
            // off while connections are failing.
            if (std::string(exception.what()).find("SSL") != std::string::npos)
                noteConnectionTrouble();
            if (statusCallback)
                statusCallback(
                    std::string("Chat reconnecting: ") + exception.what());
        } catch (...) {
            if (stopRequested.load()) break;
            if (statusCallback)
                statusCallback("Chat reconnecting after an unknown error");
        }

        if (stopRequested.load()) break;
        // Waits in short steps so stop() is not held up by a retry delay.
        for (int tick = 0;
             tick < retrySeconds * 10 && !stopRequested.load();
             ++tick)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retrySeconds = std::min(retrySeconds * 2, 20);
    }
}

}  // namespace kick
