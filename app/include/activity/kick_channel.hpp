#pragma once

#include "api/http.hpp"
#include "api/kick.hpp"
#include "api/kick_helix.hpp"

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class HRecyclerFrame;

class ChannelActionButton : public brls::Box {
public:
    ChannelActionButton() {
        this->setFocusable(true);
    }

    std::function<brls::View*()> downTarget;
    std::function<brls::View*()> upTarget;

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            brls::View* target = downTarget();
            if (target)
                return target;
        }
        if (direction == brls::FocusDirection::UP && upTarget) {
            brls::View* target = upTarget();
            if (target)
                return target;
        }
        return brls::Box::getNextFocus(direction, currentView);
    }
};

class KickChannelPage : public brls::Box {
public:
    explicit KickChannelPage(std::string channelLogin);
    ~KickChannelPage() override;

    brls::View* getDefaultFocus() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

private:
    void load();
    // Called as soon as the page is being left. Cancels in-flight requests and
    // makes any late async callback a no-op so it never touches (or gives
    // focus to) views of a page that is already being torn down.
    void markClosing();
    bool closing = false;
    void showData(kick::ChannelPageData data);
    void playLive();
    void playVod(
        const std::string& id,
        const std::string& title,
        const std::string& details);
    void playClip(
        const std::string& slug,
        const std::string& title,
        const std::string& details);
    void showMediaDetails(
        const std::string& title,
        const std::string& details);
    void updateFollowButton(bool isFollowing);
    void toggleFollow();
    brls::View* getActionFocus();
    brls::View* getFirstRowFocus();

    std::string channelLogin;
    kick::ChannelPageData data;
    HTTP::Cancel requestCancel;

    brls::ScrollingFrame* scroll = nullptr;
    brls::Box* content = nullptr;
    brls::Box* headerBox = nullptr;
    brls::Box* bodyBox = nullptr;
    brls::Label* status = nullptr;
    ChannelActionButton* backButton = nullptr;
    brls::Image* offlineImage = nullptr;
    brls::Box* identity = nullptr;
    brls::View* avatar = nullptr;
    brls::Label* name = nullptr;
    brls::Label* liveState = nullptr;
    brls::Label* headline = nullptr;
    brls::Label* description = nullptr;
    brls::Label* metadata = nullptr;
    ChannelActionButton* playButton = nullptr;
    brls::Label* playButtonLabel = nullptr;
    ChannelActionButton* followButton = nullptr;
    brls::Label* followButtonLabel = nullptr;
    bool following = false;
    bool followPending = false;
    ChannelActionButton* refreshButton = nullptr;
    brls::Box* videosHolder = nullptr;
    brls::Box* clipsHolder = nullptr;
    brls::Box* categoriesHolder = nullptr;
    HRecyclerFrame* videosRecycler = nullptr;
    HRecyclerFrame* clipsRecycler = nullptr;
    HRecyclerFrame* categoriesRecycler = nullptr;
};
