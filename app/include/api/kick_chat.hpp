#pragma once

#include "api/kick.hpp"

#include <borealis.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef BOREALIS_USE_STD_THREAD
#include <thread>
#else
#include <pthread.h>
#endif

namespace kick {

enum class ChatMode {
    Off = 0,
    RightPanel = 1,
    Overlay = 2,
};

enum class ChatParticipation {
    ReadOnly = 0,
    Interactive = 1,
};

enum class ChatComposerMode {
    NintendoKeyboard = 0,
    KickNXComposer = 1,
};

enum class ChatEmoteMode {
    Static = 0,
    Animated = 1,
};

enum class ChatOverlaySize {
    FullHeight = 0,
    Compact = 1,
};

enum class ChatOverlayPosition {
    TopRight = 0,
    TopLeft = 1,
    BottomRight = 2,
    BottomLeft = 3,
};

enum class ChatFragmentType {
    Text = 0,
    Emote = 1,
    Mention = 2,
};

struct ChatPreferences {
    ChatMode mode = ChatMode::Overlay;
    ChatParticipation participation = ChatParticipation::ReadOnly;
    ChatComposerMode composerMode = ChatComposerMode::NintendoKeyboard;
    ChatEmoteMode emoteMode = ChatEmoteMode::Static;
    ChatOverlaySize overlaySize = ChatOverlaySize::FullHeight;
    ChatOverlayPosition overlayPosition = ChatOverlayPosition::TopRight;
    int fontSize = 16;
    int opacity = 40;
    int dockedWidth = 330;
    int overlayWidth = 360;
    bool timestamps = false;
};

struct ChatFragment {
    ChatFragmentType type = ChatFragmentType::Text;
    std::string text;
    std::string emoteId;
    std::string emoteUrl;
    std::string animatedEmoteUrl;
};

struct ChatMessage {
    std::string id;
    std::string userName;
    std::string text;
    std::string color;
    std::string timestamp;
    std::vector<ChatFragment> fragments;
};

using ChatMessageCallback = std::function<void(ChatMessage)>;
using ChatStatusCallback = std::function<void(const std::string&)>;
using ChatSendCallback = std::function<void(const std::string&)>;
using ChattersCountCallback = std::function<void(int count)>;

struct UserEmote {
    std::string id;
    std::string name;
    std::string ownerId;
    std::string emoteType;
    std::string imageUrl;
    bool channelEmote = false;
};

struct UserEmoteCatalogue {
    std::vector<UserEmote> emotes;
    bool channelSubscribed = false;
    bool subscriptionPermissionGranted = false;
};

using UserEmotesCallback =
    std::function<void(UserEmoteCatalogue)>;

ChatPreferences loadChatPreferences();
bool saveChatPreferences(const ChatPreferences& preferences);
brls::Event<ChatPreferences>* chatPreferencesEvent();

bool hasChatWriteScope();
bool hasUserEmotesScope();

void loadUserEmotesAsync(
    const std::string& channel,
    UserEmotesCallback success,
    ErrorCallback failure);

std::vector<UserEmote> loadRecentEmotes();
void rememberRecentEmote(const UserEmote& emote);

void sendChatMessageAsync(
    const std::string& channel,
    const std::string& message,
    ChatSendCallback success,
    ErrorCallback failure);

int fetchActiveChattersCount(const std::string& channelIdOrSlug);

class ChatClient {
public:
    ChatClient(
        std::string channel,
        ChatMessageCallback message,
        ChatStatusCallback status,
        ChattersCountCallback chatters = nullptr);
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

    void stop();

private:
    static void* threadEntry(void* context);
    static void* chattersThreadEntry(void* context);
    void run();
    void chattersRun();

    std::string channel;
    ChatMessageCallback messageCallback;
    ChatStatusCallback statusCallback;
    ChattersCountCallback chattersCallback;
    std::atomic_bool stopRequested{false};
    std::mutex channelIdMutex;
    std::string resolvedChannelId;

#ifdef BOREALIS_USE_STD_THREAD
    std::shared_ptr<std::thread> thread;
    std::shared_ptr<std::thread> chattersThread;
#else
    pthread_t thread{};
    bool threadStarted = false;
    pthread_t chattersThread{};
    bool chattersThreadStarted = false;
#endif
};

}  // namespace kick
