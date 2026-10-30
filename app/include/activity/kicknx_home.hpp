#pragma once

#include "api/http.hpp"

#include <borealis.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace kick {
struct Stream;
struct Category;
struct HomeData;
struct LiveEvent;
}

class KickRow;

class KickNXHome : public brls::Box {
public:
    KickNXHome();
    ~KickNXHome() override;

    static KickNXHome* getInstance() { return instance; }
    void openCategoryByName(const kick::Category& category) { openCategory(category); }

    void willAppear(bool resetState = false) override;

    /// Rebuilds Home shortly, once any picker on top of it has closed.
    static void scheduleThemeRebuild();

private:
    void setBusy(bool value, const std::string& message = "");
    void updateAccount();
    void updateProfileImage(const std::string& url);
    void beginLogin();
    void logout();
    void refreshHome();
    void search();
    void showAbout();
    void showThemePicker();
    /// Replaces Home with a freshly built one so the new theme takes effect.
    static void rebuildForTheme();
    void searchFor(const std::string& query);
    void openCategory(const kick::Category& category);
    void openChannelPage(const kick::Stream& stream);
    void openEvent(const kick::LiveEvent& event);
    void playChannel(const kick::Stream& stream);
    void updateHero(const kick::Stream& stream);
    void clearHero();
    void playConfiguredChannel();
    void showHomeData(kick::HomeData data);
    void clearRows();

    uint64_t beginBrowsingRequest(const std::string& message);
    bool finishBrowsingRequest(uint64_t requestId);
    void cancelBrowsingRequest();
    void showRequestError(const std::string& message, std::function<void()> retry);
    void clearRequestError();

    void rememberFocus(const std::string& row, const std::string& key);
    void restoreContentFocus();
    void focusFirstContentRow();
    bool isActiveActivity();

    void loadMoreFollowed();
    void loadMorePopular();
    void loadMorePopularClips();
    void loadMoreCategories();
    void loadMoreSearch();
    void loadMoreCategoryStreams();

    brls::Label* accountLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* loginButtonLabel = nullptr;
    brls::View* profileAvatar = nullptr;
    brls::Box* loginButton = nullptr;
    brls::Box* searchButton = nullptr;
    brls::Box* refreshButton = nullptr;
    brls::Box* aboutButton = nullptr;
    brls::Box* themeButton = nullptr;

    brls::Box* errorBox = nullptr;
    brls::Label* errorLabel = nullptr;
    brls::Box* retryButton = nullptr;

    brls::ScrollingFrame* homeScroll = nullptr;

    brls::Box* heroBox = nullptr;
    brls::Image* heroPreview = nullptr;
    brls::Label* heroLiveLabel = nullptr;
    brls::Label* heroChannelLabel = nullptr;
    brls::Label* heroTitleLabel = nullptr;
    brls::Label* heroMetaLabel = nullptr;
    std::function<void()> retryAction;

    KickRow* followedRow = nullptr;
    KickRow* offlineFollowedRow = nullptr;
    KickRow* popularRow = nullptr;
    KickRow* popularClipsRow = nullptr;
    KickRow* eventsRow = nullptr;
    KickRow* categoryStreamsRow = nullptr;
    KickRow* categoriesRow = nullptr;
    KickRow* searchRow = nullptr;

    HTTP::Cancel activeRequest;
    uint64_t requestGeneration = 0;

    std::string followedCursor;
    std::string popularCursor;
    std::string popularClipsCursor;
    std::string categoriesCursor;
    std::string searchCursor;
    std::string categoryStreamsCursor;
    std::string currentSearchQuery;
    std::string currentCategoryId;
    std::string currentCategoryName;

    std::string lastFocusedRow;
    std::string lastFocusedKey;

    bool busy = false;

    inline static KickNXHome* instance = nullptr;
    inline static bool themeRebuildPending = false;
    static constexpr long THEME_REBUILD_DELAY_MS = 500;
};
