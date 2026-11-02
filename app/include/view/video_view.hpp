//
// Created by fang on 2022/4/22.
//

#pragma once

#include <borealis.hpp>
#include <utils/event.hpp>
#include <functional>

class VideoProgressSlider;
class SVGImage;
class VideoProfile;

enum class OSDState {
    HIDDEN = 0,
    SHOWN = 1,
    ALWAYS_ON = 2,
};

class VideoView : public brls::Box {
public:
    VideoView();

    ~VideoView() override;

    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style, brls::FrameContext* ctx) override;

    void invalidate() override;

    View* getDefaultFocus() override { return this->isOsdShown ? this->btnToggle : this; }

    void onChildFocusGained(View* directChild, View* focusedView) override;

    View* getNextFocus(brls::FocusDirection direction, View* currentView) override { return this; }

    void setTitie(const std::string& title);
    void setTitle(const std::string& title) { this->setTitie(title); }
    void setLoadingNote(const std::string& note);
    void showLoading();
    void hideLoading(bool dimming = true);

    brls::Event<int>* getPlayEvent() { return &this->playIndexEvent; }

    brls::VoidEvent* getSettingEvent() { return &this->settingEvent; }
    brls::VoidEvent* getChannelEvent() { return &this->channelEvent; }

    void showChannelButton(const std::string& imageUrl);
    void hideChannelButton();

    VideoProfile* getProfile() { return this->profile; }

    void hideVideoQuality();
    void registerActions(const std::string& hintText, const brls::ControllerButton button,
        const brls::BrlsKeyCombination key, const brls::ActionListener& actionListener, bool hidden = false,
        bool allowRepeating = false);

    void showOSD(bool autoHide = true);

    // Draws a non-interactive content overlay above the video and below the OSD.
    void setContentOverlay(brls::View* overlay);

    // Live streams can consume transient EOF/file-error events and recover
    // without entering the normal finite-video close/error path.
    using PlaybackRecoveryHandler = std::function<bool(MpvEventEnum)>;
    void setPlaybackRecoveryHandler(PlaybackRecoveryHandler handler);

    void setLiveMode(bool live, int64_t uptimeSeconds = 0);
    void setStreamUptime(int64_t uptimeSeconds);
    int64_t getCurrentStreamUptime() const;
    void updateLiveProgressUI();

    /// Live rewind beyond the cache plays Kick's recording of the broadcast.
    /// With a recording the bar covers the whole stream; a seek the live cache
    /// can't reach (or any seek while the recording plays) goes to the handler
    /// with how far behind live to play, 0 meaning back to live.
    using LiveSeekHandler = std::function<void(int64_t behindSeconds)>;
    /// Kick's recording trails live by about this much (measured ~16 s; it
    /// grows in 10 s segments). Also the most the end of the bar snaps to
    /// live.
    static constexpr int64_t LIVE_RECORDING_DELAY = 20;
    /// The recording is only opened this far behind live or more, which
    /// stays inside what it holds even just before its next segment lands.
    int64_t liveRecordingGap() const { return LIVE_RECORDING_DELAY + 12; }
    void setLiveSeekHandler(LiveSeekHandler handler);
    void setLiveRecordingAvailable(bool available);
    void setWatchingRecording(bool watching);
    bool isWatchingRecording() const { return this->watchingRecording; }
    /// mpv opens a file at its start and seeks to the requested point once
    /// it runs. Keeps the (paused) picture covered until playback has
    /// reached `position`, then resumes.
    void holdUntilPosition(double position);
    bool isHoldingPosition() const { return this->holdPosition >= 0; }

    using CloseHandler = std::function<void()>;
    static void setGlobalCloseHandler(CloseHandler handler);
    static bool close(bool quit = false);

private:
    /// OSD
    BRLS_BIND(brls::Label, titleLabel, "video/osd/title");
    BRLS_BIND(brls::Box, btnSetting, "video/osd/setting");
    BRLS_BIND(brls::Box, btnChannel, "video/osd/channel");
    BRLS_BIND(brls::Image, channelImage, "video/osd/channel/image");
    BRLS_BIND(brls::Box, btnCast, "video/osd/cast");
    BRLS_BIND(brls::Box, btnToggle, "video/osd/toggle");
    BRLS_BIND(brls::Box, btnVideoQuality, "video/quality/box");
    BRLS_BIND(brls::Box, btnVideoSpeed, "video/speed/box");
    BRLS_BIND(brls::Box, btnVolume, "video/osd/volume");
    BRLS_BIND(brls::Box, btnClose, "video/close/box");
    BRLS_BIND(brls::Box, iconBox, "video/osd/icon/box");
    BRLS_BIND(brls::Label, iconVideoQuality, "video/quality/label");
    BRLS_BIND(brls::Label, iconVideoSpeed, "video/speed/label");
    BRLS_BIND(SVGImage, toggleIcon, "video/osd/toggle/icon");
    BRLS_BIND(SVGImage, volumeIcon, "video/osd/volume/icon");
    BRLS_BIND(SVGImage, osdSettingIcon, "video/osd/setting/icon");
    BRLS_BIND(brls::Box, osdTopBox, "video/osd/top/box");
    BRLS_BIND(brls::Box, osdBottomBox, "video/osd/bottom/box");
    // 用于显示缓冲组件
    BRLS_BIND(brls::Box, osdCenterBox, "video/osd/center/box");
    BRLS_BIND(brls::Label, centerLabel, "video/osd/center/label");
    BRLS_BIND(brls::View, osdSpinner, "video/osd/loading");
    // 用于通用的提示信息
    BRLS_BIND(brls::Box, osdInfoBox, "video/osd/info/box");
    BRLS_BIND(brls::Label, infoLabel, "video/osd/info/label");
    BRLS_BIND(SVGImage, infoIcon, "video/osd/info/icon");
    // 用于显示和控制视频时长
    BRLS_BIND(VideoProgressSlider, osdSlider, "video/osd/bottom/progress");
    BRLS_BIND(brls::Label, leftStatusLabel, "video/left/status");
    BRLS_BIND(brls::Label, centerStatusLabel, "video/center/status");
    BRLS_BIND(brls::Label, rightStatusLabel, "video/right/status");
    BRLS_BIND(brls::Label, videoSpeedLabel, "video/speed/label");
    BRLS_BIND(brls::Label, speedHintLabel, "video/speed/hint/label");
    BRLS_BIND(brls::Box, speedHintBox, "video/speed/hint/box");

    void registerMpvEvent();
    void unRegisterMpvEvent();

    bool toggleProfile();
    /// OSD
    void toggleOSD();
    void hideOSD();
    bool toggleSpeed();
    bool toggleVolume(brls::View* view);
    void setTvMode(bool state);

    /// @brief 延迟 200ms 触发进度跳转到 seeking_range
    void requestSeeking(int seek, int delay = 400);
    void requestVolume(int value, int delay = 400);
    void requestBrightness(float value);
    void buttonProcessing();
    /// Hold A on the progress bar and move the left stick or D-pad to pick
    /// a new position; releasing A seeks there. A tap still plays/pauses.
    bool isProgressBarFocused();
    void beginScrub();
    void scrubProcessing();
    void moveScrub(double deltaSeconds);
    void endScrub(bool released);
    /// Length of the bar in seconds: the VOD, or the live rewind range.
    double barLength() const;
    double liveBarLength() const;
    /// How far behind live playback is now, in seconds.
    int64_t currentLiveBehind() const;
    /// Plays the live stream `behind` seconds behind live.
    void seekLive(int64_t behind);
    void showLiveBehind(int64_t behind);
    /// Small "-time" over the bar at the scrub position (live only).
    void drawScrubTip(NVGcontext* vg);
    /// @brief notify videoview closed
    static void disableDimming(bool disable);

    int playIndex = -1;
    std::string customLoadingNote;
    brls::Event<int> playIndexEvent;
    brls::VoidEvent settingEvent;
    brls::VoidEvent channelEvent;
    View* lastFocusedView = nullptr;

    // OSD
    bool isOsdShown = false;
    brls::Time osdLastShowTime = 0;
    brls::Time profileLastShowTime = 0;
    const brls::Time OSD_SHOW_TIME = 5000000;  //默认5秒
    OSDState osdState = OSDState::HIDDEN;
    VideoProfile* profile;
    brls::View* contentOverlay = nullptr;
    PlaybackRecoveryHandler playbackRecoveryHandler;

    int64_t seekingRange = 0;
    size_t seekingIter = 0;

    // Hold-A scrubbing
    bool scrubbing = false;      // A went down while the progress bar had focus
    bool scrubMoved = false;     // the position changed, so releasing A seeks
    float scrubProgress = 0;     // previewed position, 0..1 of the bar
    int scrubDirection = 0;      // -1, 0 or 1
    brls::Time scrubDirectionSince = 0;
    brls::Time scrubLastTick = 0;

    MPVEvent::Subscription eventSubscribeID;

    NVGcolor bottomBarColor = brls::Application::getTheme().getColor("color/app");

    size_t volumeIter = 0;  // 音量UI关闭的延迟函数 handle
    int volumeInit = 0;
    float brightnessInit = 0;

    bool isLiveMode = false;
    int64_t streamUptimeBaseline = 0;
    std::chrono::steady_clock::time_point streamUptimeBaselineTime;
    int64_t liveRewindSeconds = 0;
    brls::Time lastLiveProgressTick = 0;
    bool liveRecordingAvailable = false;
    bool liveRecordingChecked = false;  // Kick answered whether there is one
    bool watchingRecording = false;
    double holdPosition = -1;
    std::string scrubTip;  // shown over the bar while scrubbing live
    float scrubTipProgress = 0;
    brls::Time holdStarted = 0;
    LiveSeekHandler liveSeekHandler;
    brls::Time lastRewindLimitNote = 0;
};
