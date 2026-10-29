#pragma once

#include "api/kick_chat.hpp"
#include "utils/orientation.hpp"

#include <borealis.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class RecyclingGrid;
class KickDraftPreview;

class KickChatComposer : public brls::Box {
public:
    explicit KickChatComposer(std::string channel);
    ~KickChatComposer() override;

    bool isTranslucent() override {
        return true;
    }

    brls::View* getDefaultFocus() override {
        return draftButton ? draftButton : this;
    }

private:
    // Closes this overlay when the display rotates, since its fixed-size
    // layout would otherwise be left covering the portrait keyboard.
    std::optional<brls::Event<kicknx::portrait::DisplayOrientation>::Subscription>
        orientationSubscription;
    std::shared_ptr<bool> aliveToken = std::make_shared<bool>(true);

    enum class Tab {
        Recent = 0,
        Channel = 1,
        All = 2,
    };

    void openKeyboard();
    void insertEmote(const kick::UserEmote& emote);
    void sendMessage();
    void setTab(Tab value);
    void cycleTab(int direction);
    void refreshGrid();
    void updateDraft();
    void updateTabs();
    void updateChannelTabVisibility();
    void reconcileRecentEmotes();
    std::vector<kick::UserEmote> visibleEmotes() const;

    std::string channel;
    std::string draft;
    std::vector<kick::UserEmote> emotes;
    std::vector<kick::UserEmote> recentEmotes;
    Tab tab = Tab::Recent;
    bool loading = true;
    bool sending = false;
    bool channelSubscribed = false;
    bool subscriptionPermissionGranted = false;

    brls::Label* statusLabel = nullptr;
    KickDraftPreview* draftPreview = nullptr;
    brls::Label* counterLabel = nullptr;
    brls::Label* focusedEmoteLabel = nullptr;
    brls::Label* sendLabel = nullptr;
    brls::Box* draftButton = nullptr;
    brls::Box* sendButton = nullptr;
    brls::Box* recentTab = nullptr;
    brls::Box* channelTab = nullptr;
    brls::Box* allTab = nullptr;
    RecyclingGrid* grid = nullptr;
};
