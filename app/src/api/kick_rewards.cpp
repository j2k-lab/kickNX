#include "api/kick_rewards.hpp"

#include "api/http.hpp"
#include "api/kick_auth.hpp"
#include "api/kick_playlist.hpp"
#include "api/raw_websocket.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace kick {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Public client key kick.com's own web app sends when asking for a viewer
// connection token (NEXT_PUBLIC_WEBSOCKET_CLIENT_TOKEN in its bundle).
constexpr const char* VIEWER_CLIENT_TOKEN =
    "e1393935a959b4020a4491574f6490129f678acdaa92760471263db43487f823";
constexpr const char* VIEWER_TOKEN_URL = "https://websockets.kick.com/viewer/v1/token";
constexpr const char* VIEWER_CONNECT_URL = "wss://websockets.kick.com/viewer/v1/connect";
constexpr const char* CHALLENGES_URL = "https://web.kick.com/api/v1/gamification/challenges";

// How often kick.com reports watching (Kick's global settings give 120 for
// "tracking.user.watch.livestream").
constexpr auto WATCH_EVENT_EVERY = std::chrono::seconds(120);

// Background work waits for the stream and chat to finish connecting.
constexpr auto START_DELAY = std::chrono::seconds(25);
constexpr auto TROUBLE_WINDOW = std::chrono::seconds(60);

std::atomic<int64_t> lastTroubleTicks{0};

struct NotLive : std::runtime_error {
    NotLive() : std::runtime_error("The channel is not live") {}
};

std::string jsonString(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return {};
    const auto& value = object[key];
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    return {};
}

int jsonInt(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return 0;
    const auto& value = object[key];
    if (value.is_number()) return static_cast<int>(value.get<double>());
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

const json& dataOf(const json& root) {
    return root.is_object() && root.contains("data") ? root["data"] : root;
}

HTTP::Header kickHeaders(const std::string& accessToken) {
    HTTP::Header headers = {
        "Accept: application/json",
        "x-app-platform: web",
        "Origin: https://kick.com",
        "Referer: https://kick.com/",
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36",
    };
    if (!accessToken.empty()) {
        headers.push_back("Authorization: Bearer " + accessToken);
        headers.push_back("Cookie: session_token=" + accessToken);
    }
    return headers;
}

// "2026-11-08T00:00:00Z" to Unix seconds; 0 if it does not parse.
int64_t parseUtcTime(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 3)
        return 0;
    // Days from 1970-01-01 (Howard Hinnant's days_from_civil).
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear =
        (153 * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

DailyReward parseChallenge(const json& item) {
    DailyReward reward;
    reward.id = jsonString(item, "id");
    reward.name = jsonString(item, "name");
    reward.status = jsonString(item, "status");
    if (item.contains("condition") && item["condition"].is_object()) {
        reward.progress = jsonInt(item["condition"], "progress");
        reward.threshold = jsonInt(item["condition"], "threshold");
    }
    if (item.contains("window") && item["window"].is_object())
        reward.resetsAt = parseUtcTime(jsonString(item["window"], "ends_at"));
    reward.fetchedAt = Clock::now();
    return reward;
}

}  // namespace

void noteConnectionTrouble() {
    lastTroubleTicks.store(Clock::now().time_since_epoch().count());
}

bool connectionTroubleWithin(std::chrono::seconds window) {
    const int64_t ticks = lastTroubleTicks.load();
    if (ticks == 0) return false;
    const Clock::time_point at{Clock::duration(ticks)};
    return Clock::now() - at < window;
}

std::optional<DailyReward> fetchDailyReward(std::shared_ptr<std::atomic_bool> cancel) {
    const OAuthSession session = loadOAuth();
    if (session.accessToken.empty()) return std::nullopt;

    const json root = json::parse(HTTP::get(
        CHALLENGES_URL,
        kickHeaders(session.accessToken),
        HTTP::Timeout{10000},
        cancel));
    const json& data = dataOf(root);
    if (!data.is_array()) return std::nullopt;

    // The daily one is what the button tracks; other challenges (events)
    // come and go.
    const json* chosen = nullptr;
    for (const auto& item : data) {
        if (!item.is_object()) continue;
        if (jsonString(item, "recurrence") == "daily") {
            chosen = &item;
            break;
        }
        if (!chosen) chosen = &item;
    }
    if (!chosen) return std::nullopt;

    DailyReward reward = parseChallenge(*chosen);
    if (reward.id.empty() || reward.threshold <= 0) return std::nullopt;
    return reward;
}

void claimDailyRewardAsync(
    const std::string& challengeId,
    std::function<void(ClaimedReward)> success,
    ErrorCallback failure) {
    ThreadPool::instance().submit(
        [challengeId, success = std::move(success), failure = std::move(failure)](HTTP&) mutable {
            try {
                const OAuthSession session = loadOAuth();
                if (session.accessToken.empty())
                    throw std::runtime_error("Sign in to claim rewards");

                const json root = json::parse(HTTP::post(
                    std::string(CHALLENGES_URL) + "/" + percentEncode(challengeId) + "/claim",
                    std::string(),
                    kickHeaders(session.accessToken),
                    HTTP::Timeout{15000}));
                const json& data = dataOf(root);
                if (!data.is_object())
                    throw std::runtime_error("Kick did not confirm the claim");

                ClaimedReward reward;
                if (data.contains("winner") && data["winner"].is_object()) {
                    const json& winner = data["winner"];
                    reward.cardUrl = jsonString(winner, "card_url");
                    if (reward.cardUrl.empty())
                        reward.cardUrl = jsonString(winner, "thumbnail_url");
                    reward.name = jsonString(winner, "name");
                    if (reward.name.empty()) reward.name = jsonString(winner, "title");
                    reward.rarity = jsonString(winner, "rarity");
                    if (winner.contains("watch_time") && winner["watch_time"].is_object())
                        reward.watchTimeMinutes =
                            jsonInt(winner["watch_time"], "watch_time_minutes");
                }
                if (data.contains("consolation") && data["consolation"].is_object()) {
                    reward.duplicate = true;
                    reward.watchTimeMinutes =
                        jsonInt(data["consolation"], "watch_time_minutes");
                }

                brls::sync(std::bind(success, std::move(reward)));
            } catch (const std::exception& exception) {
                brls::sync(std::bind(failure, std::string(exception.what())));
            } catch (...) {
                brls::sync(std::bind(failure, std::string("The reward could not be claimed")));
            }
        });
}

WatchSession::WatchSession(std::string channel, bool countWatchTime, RewardCallback onReward)
    : channel(normalizeChannel(std::move(channel))),
      countWatchTime(countWatchTime),
      onReward(std::move(onReward)) {
#ifdef BOREALIS_USE_STD_THREAD
    thread = std::make_shared<std::thread>(&WatchSession::threadEntry, this);
#else
    if (pthread_create(&thread, nullptr, &WatchSession::threadEntry, this) == 0)
        threadStarted = true;
#endif
}

WatchSession::~WatchSession() {
    {
        // Set under the lock so a wait that is just starting cannot miss it
        // and hold the player's exit for minutes.
        std::lock_guard<std::mutex> lock(wakeMutex);
        stopRequested.store(true);
    }
    cancelRequests->store(true);
    wake.notify_all();
#ifdef BOREALIS_USE_STD_THREAD
    if (thread && thread->joinable()) thread->join();
#else
    if (threadStarted) pthread_join(thread, nullptr);
#endif
}

void WatchSession::refreshReward() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex);
        refreshRequested.store(true);
    }
    wake.notify_all();
}

void* WatchSession::threadEntry(void* context) {
    static_cast<WatchSession*>(context)->run();
    return nullptr;
}

bool WatchSession::waitFor(std::chrono::seconds duration, bool wakeOnRefresh) {
    std::unique_lock<std::mutex> lock(wakeMutex);
    wake.wait_for(lock, duration, [this, wakeOnRefresh]() {
        return stopRequested.load() || (wakeOnRefresh && refreshRequested.load());
    });
    return !stopRequested.load();
}

void WatchSession::checkReward(Clock::time_point& nextRewardCheck) {
    refreshRequested.store(false);
    const auto now = Clock::now();
    if (connectionTroubleWithin(TROUBLE_WINDOW)) {
        nextRewardCheck = now + std::chrono::seconds(60);
        return;
    }
    try {
        const auto reward = fetchDailyReward(cancelRequests);
        if (!reward) {
            nextRewardCheck = now + std::chrono::minutes(15);
            return;
        }
        lastReward = reward;
        reportReward();

        if (reward->claimable()) {
            nextRewardCheck = now + std::chrono::minutes(30);
        } else if (reward->claimed()) {
            nextRewardCheck = now + std::chrono::minutes(20);
        } else {
            // The display counts up by itself in between; Kick is asked
            // every 4 minutes to correct it, and again just after the
            // reward should be ready so Claim appears without a long wait.
            const int minutesLeft = std::max(1, reward->threshold - reward->progress);
            const auto untilReady =
                std::chrono::seconds(minutesLeft * 60 + 45);
            nextRewardCheck = now +
                (viewerConnected ? std::min<std::chrono::seconds>(std::chrono::minutes(4), untilReady)
                                 : std::chrono::minutes(4));
        }
    } catch (...) {
        nextRewardCheck = now + std::chrono::minutes(2);
    }
}

void WatchSession::reportReward() {
    if (!lastReward || !onReward || stopRequested.load()) return;
    DailyReward reward = *lastReward;
    reward.counting = viewerConnected;
    onReward(reward);
}

void WatchSession::setCounting(bool counting) {
    if (viewerConnected == counting) return;
    if (lastReward) {
        DailyReward& reward = *lastReward;
        if (!counting) {
            // The estimate so far becomes the new starting point.
            reward.counting = true;
            reward.progress = reward.estimatedProgress();
            reward.counting = false;
        }
        reward.fetchedAt = Clock::now();
    }
    viewerConnected = counting;
    reportReward();
}

void WatchSession::watch(Clock::time_point& nextRewardCheck) {
    const OAuthSession session = loadOAuth();
    const HTTP::Header headers = kickHeaders(session.accessToken);

    // The watch event names the broadcast; /info is the reply that carries
    // its recording ID too.
    const json info = json::parse(HTTP::get(
        "https://kick.com/api/v2/channels/" + percentEncode(channel) + "/info",
        headers,
        HTTP::Timeout{10000},
        cancelRequests));
    const json& channelData = dataOf(info);
    if (!channelData.is_object() ||
        !channelData.contains("livestream") ||
        !channelData["livestream"].is_object())
        throw NotLive();
    const json& livestream = channelData["livestream"];
    const json channelId = channelData.value("id", json());
    const json livestreamId = livestream.value("id", json());
    const json vodId = livestream.value("vod_id", json());
    if (channelId.is_null() || livestreamId.is_null() || vodId.is_null()) throw NotLive();
    const std::string channelIdText = jsonString(channelData, "id");

    HTTP::Header tokenHeaders = headers;
    tokenHeaders.push_back(std::string("X-CLIENT-TOKEN: ") + VIEWER_CLIENT_TOKEN);

    const json channelMessage = {{"message", {{"channelId", channelIdText}}}};
    const json watchMessage = {{"message",
        {{"name", "tracking.user.watch.livestream"},
         {"channel_id", channelId},
         {"livestream_id", livestreamId},
         {"vod_id", vodId}}}};

    // A connection held open for the whole stream took one of the Switch's
    // few TLS connections from chat and message sends. Instead, connect only
    // long enough to report each watch event, then let it go.
    const auto reportWatching = [&]() {
        ThreadPool::instance().releaseIdleConnections();

        const json tokenRoot = json::parse(HTTP::get(
            VIEWER_TOKEN_URL, tokenHeaders, HTTP::Timeout{10000}, cancelRequests));
        const std::string token = jsonString(dataOf(tokenRoot), "token");
        if (token.empty()) throw std::runtime_error("Kick sent no viewer token");

        RawWebSocket socket(
            std::string(VIEWER_CONNECT_URL) + "?token=" + percentEncode(token),
            stopRequested);
        const auto send = [&socket](const char* type, const json& data) {
            socket.sendText(json({{"type", type}, {"data", data}}).dump());
        };
        send("channel_handshake", channelMessage);
        send("user_event", watchMessage);
        // Gives Kick a moment to take the event before the connection ends.
        waitFor(std::chrono::seconds(2), false);
        send("channel_disconnect", channelMessage);
        socket.close();
    };

    // Whatever ends this, the display stops counting with it.
    struct StopCounting {
        WatchSession& session;
        ~StopCounting() { session.setCounting(false); }
    } stopCounting{*this};

    auto nextReport = Clock::now();
    int failures = 0;
    while (!stopRequested.load()) {
        const auto now = Clock::now();
        if (now >= nextReport) {
            if (connectionTroubleWithin(TROUBLE_WINDOW)) {
                setCounting(false);
                nextReport = now + std::chrono::seconds(30);
            } else {
                try {
                    reportWatching();
                    failures = 0;
                    setCounting(true);
                    nextReport = Clock::now() + WATCH_EVENT_EVERY;
                } catch (const std::exception& exception) {
                    if (stopRequested.load()) return;
                    const std::string what = exception.what();
                    if (what.find("SSL") != std::string::npos ||
                        what.find("resolve") != std::string::npos)
                        noteConnectionTrouble();
                    brls::Logger::warning("kickNX: viewer check-in: {}", what);
                    setCounting(false);
                    failures = std::min(failures + 1, 4);
                    nextReport = Clock::now() + std::chrono::seconds(60 * failures);
                }
            }
        }

        // Reward checks share this thread, so at most one extra request is
        // ever in flight next to the stream and chat.
        if (refreshRequested.load() || Clock::now() >= nextRewardCheck)
            checkReward(nextRewardCheck);

        const auto wake = std::min(nextReport, nextRewardCheck);
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(wake - Clock::now());
        if (!waitFor(std::max(std::chrono::seconds(1), left))) return;
    }
}

void WatchSession::run() {
    if (!waitFor(START_DELAY)) return;
    refreshRequested.store(true);

    auto nextRewardCheck = Clock::now();
    auto retryDelay = std::chrono::seconds(30);

    const auto waitUntilNextCheck = [&]() {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
            nextRewardCheck - Clock::now());
        return waitFor(std::max(std::chrono::seconds(1), left));
    };

    while (!stopRequested.load()) {
        if (refreshRequested.load() || Clock::now() >= nextRewardCheck)
            checkReward(nextRewardCheck);

        if (!countWatchTime || loadOAuth().accessToken.empty()) {
            if (!waitUntilNextCheck()) break;
            continue;
        }
        if (connectionTroubleWithin(TROUBLE_WINDOW)) {
            if (!waitFor(std::chrono::seconds(30), false)) break;
            continue;
        }

        const auto started = Clock::now();
        try {
            watch(nextRewardCheck);
        } catch (const NotLive&) {
            // A recording or an offline channel: nothing to count. Rewards
            // are still checked.
            if (!waitFor(std::chrono::seconds(300))) break;
            continue;
        } catch (const std::exception& exception) {
            if (stopRequested.load()) break;
            const std::string what = exception.what();
            if (what.find("SSL") != std::string::npos ||
                what.find("connect") != std::string::npos ||
                what.find("resolve") != std::string::npos)
                noteConnectionTrouble();
            brls::Logger::warning("kickNX: viewer connection: {}", what);
        } catch (...) {
            if (stopRequested.load()) break;
        }

        // A connection that held for a while starts the backoff over.
        if (Clock::now() - started > std::chrono::minutes(2))
            retryDelay = std::chrono::seconds(30);
        // Backoff waits are not cut short by reward refreshes, so a failing
        // connection is never retried faster than this.
        if (!waitFor(retryDelay, false)) break;
        retryDelay = std::min(retryDelay * 2, std::chrono::seconds(300));
    }
}

}  // namespace kick
