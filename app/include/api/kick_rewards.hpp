#pragma once

#include "api/kick.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#ifdef BOREALIS_USE_STD_THREAD
#include <thread>
#else
#include <pthread.h>
#endif

namespace kick {

/// Kick's Daily Reward: unlocked by watching live streams for a while each
/// day, then claimed for a random collectible.
struct DailyReward {
    std::string id;
    std::string name;
    /// "in_progress", "claimable" or "claimed".
    std::string status;
    int progress = 0;
    int threshold = 0;
    /// When today's window ends (Unix seconds, UTC); 0 when unknown.
    int64_t resetsAt = 0;

    /// When Kick reported `progress`, and whether watch time has been
    /// counting since (watch reports are reaching Kick). Lets the display
    /// count up minute by minute between checks.
    std::chrono::steady_clock::time_point fetchedAt{};
    bool counting = false;

    bool claimable() const { return status == "claimable"; }
    bool claimed() const { return status == "claimed"; }

    /// Progress in minutes as of now: Kick's figure plus the minutes watched
    /// since, never past the threshold (only Kick says when it is ready).
    int estimatedProgress() const {
        if (claimable() || claimed()) return threshold;
        int value = progress;
        if (counting) {
            value += static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::steady_clock::now() - fetchedAt).count());
        }
        return std::clamp(value, 0, threshold);
    }
};

/// What a claim returned.
struct ClaimedReward {
    std::string name;
    std::string rarity;
    /// Picture of the card that was won (an emote or badge).
    std::string cardUrl;
    /// Set when the prize was watch-time progress, or a duplicate turned
    /// into it.
    int watchTimeMinutes = 0;
    bool duplicate = false;
};

/// Blocking: reads the Daily Reward for the signed-in account. Empty when
/// Kick lists none. Throws on network errors.
std::optional<DailyReward> fetchDailyReward(
    std::shared_ptr<std::atomic_bool> cancel = nullptr);

void claimDailyRewardAsync(
    const std::string& challengeId,
    std::function<void(ClaimedReward)> success,
    ErrorCallback failure);

/// Chat records TLS connect failures here; optional background work (the
/// viewer connection, reward checks) holds off while they are recent so it
/// never competes with the stream and chat for the Switch's few connections.
void noteConnectionTrouble();
bool connectionTroubleWithin(std::chrono::seconds window);

/// One background thread per live stream that, one request at a time:
///  - reports watching to Kick every 2 minutes over a brief connection, so
///    the time counts toward the Daily Reward (only when countWatchTime
///    is set), and
///  - checks the Daily Reward every few minutes.
/// It starts 25 seconds after the stream opens, after playback and chat.
class WatchSession {
public:
    /// Called on the session's thread; hop to the UI thread to use it.
    using RewardCallback = std::function<void(DailyReward)>;

    WatchSession(std::string channel, bool countWatchTime, RewardCallback onReward);
    ~WatchSession();

    WatchSession(const WatchSession&) = delete;
    WatchSession& operator=(const WatchSession&) = delete;

    /// Checks the reward again at the next chance (after a claim).
    void refreshReward();

private:
    static void* threadEntry(void* context);
    void run();
    void watch(std::chrono::steady_clock::time_point& nextRewardCheck);
    void checkReward(std::chrono::steady_clock::time_point& nextRewardCheck);
    /// False once stopped. Reward refreshes end the wait early only when
    /// wakeOnRefresh is set.
    bool waitFor(std::chrono::seconds duration, bool wakeOnRefresh = true);

    std::string channel;
    bool countWatchTime;
    RewardCallback onReward;
    std::atomic_bool stopRequested{false};
    std::atomic_bool refreshRequested{false};
    /// Only touched on the session's thread.
    std::optional<DailyReward> lastReward;
    bool viewerConnected = false;
    void reportReward();
    void setCounting(bool counting);
    std::shared_ptr<std::atomic_bool> cancelRequests =
        std::make_shared<std::atomic_bool>(false);
    std::mutex wakeMutex;
    std::condition_variable wake;

#ifdef BOREALIS_USE_STD_THREAD
    std::shared_ptr<std::thread> thread;
#else
    pthread_t thread{};
    bool threadStarted = false;
#endif
};

}  // namespace kick
