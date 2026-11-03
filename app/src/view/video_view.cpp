#include "view/video_view.hpp"
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#include "utils/gesture.hpp"
#include "utils/keybind.hpp"
#include "utils/misc.hpp"
#include "utils/image.hpp"
#include "view/mpv_core.hpp"
#include "view/svg_image.hpp"
#include "view/video_profile.hpp"
#include "view/video_progress_slider.hpp"

const int VIDEO_SEEK_NODELAY = 0;
// Seconds L/R move the preview while A is held on the progress bar.
const double SCRUB_SHOULDER_STEP = 10.0;

using namespace brls::literals;

/// Seconds behind live at `progress` along a live bar of `length` seconds.
/// The last 2% of the bar snaps to live, but never more than the recording
/// trails live by, so a long broadcast's last minutes stay reachable.
static int64_t liveBehindAt(float progress, double length) {
    const double behind = (1.0 - progress) * length;
    if (behind <= std::min<double>(0.02 * length, VideoView::LIVE_RECORDING_DELAY)) return 0;
    return static_cast<int64_t>(behind);
}

static int getSeekRange(int current) {
    current = abs(current);
    if (current < 60) return 5;
    if (current < 300) return 10;
    if (current < 600) return 20;
    if (current < 1200) return 60;
    return current / 15;
}

VideoView::VideoView() {
    this->inflateFromXMLRes("xml/view/video_view.xml");
    brls::Logger::debug("VideoView: created");
    this->setHideHighlightBorder(true);
    this->setHideHighlightBackground(true);
    this->setHideClickAnimation(true);

    auto& mpv = MPVCore::instance();
    // 停止正在播放的音乐
    mpv.reset();
    mpv.enableVO(true);

    // The progress bar is always reachable with the controller so it can be
    // scrubbed by holding A. TV mode only changes what B does.
    this->setTvMode(true);

    this->registerAction(
        "hints/back"_i18n, brls::BUTTON_B,
        [this](brls::View* view) {
            if (MPVCore::OSD_TV_MODE && this->isOsdShown) {
                this->toggleOSD();
                return true;
            }
            return close();
        },
        true);

    this->registerActions(
        "\uE08F", brls::BUTTON_LB, KeyBind::getRewind(),
        [this](brls::View* view) -> bool {
            // While A is held on the progress bar, L/R fine-tune the scrub.
            if (this->scrubbing) {
                this->moveScrub(-SCRUB_SHOULDER_STEP);
                return true;
            }
            this->seekingRange -= getSeekRange(this->seekingRange);
            this->requestSeeking(seekingRange);
            return true;
        },
        false, true);

    this->registerActions(
        "\uE08E", brls::BUTTON_RB, KeyBind::getForward(),
        [this](brls::View* view) -> bool {
            if (this->scrubbing) {
                this->moveScrub(SCRUB_SHOULDER_STEP);
                return true;
            }
            this->seekingRange += getSeekRange(this->seekingRange);
            this->requestSeeking(seekingRange);
            return true;
        },
        false, true);

    this->registerActions(
        "toggleOSD", brls::BUTTON_Y, KeyBind::getVideoOsd(),
        [this](brls::View* view) -> bool {
            // 拖拽进度时不要影响显示 OSD
            if (!this->seekingRange) this->toggleOSD();
            return true;
        },
        true);

    /// Kick channel page button (hidden for non-Kick playback)
    this->btnChannel->registerClickAction(
        [this](brls::View*) {
            this->channelEvent.fire();
            return true;
        });
    this->btnChannel->addGestureRecognizer(
        new brls::TapGestureRecognizer(
            this->btnChannel));

    /// 播放器设置按钮
    this->btnSetting->registerClickAction([this](brls::View* view) {
        this->settingEvent.fire();
        return true;
    });
    this->btnSetting->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnSetting));
    this->registerActions(
        "main/player/setting"_i18n, brls::BUTTON_X, KeyBind::getSetting(),
        [this](brls::View* view) {
            this->settingEvent.fire();
            return true;
        },
        true);

    this->registerActions(
        "volumeUp", brls::BUTTON_NAV_UP, KeyBind::getVolumeUp(),
        [this](brls::View* view) -> bool {
            auto& state = brls::Application::getControllerState();
            if (state.buttons[brls::BUTTON_RT]) {
                this->requestVolume((int)MPVCore::instance().volume + 5, 400);
                return true;
            }
            return false;
        },
        true, true);

    this->registerActions(
        "volumeDown", brls::BUTTON_NAV_DOWN, KeyBind::getVolumeDown(),
        [this](brls::View* view) -> bool {
            auto& state = brls::Application::getControllerState();
            if (state.buttons[brls::BUTTON_RT]) {
                this->requestVolume((int)MPVCore::instance().volume - 5, 400);
                return true;
            }
            return false;
        },
        true, true);

    /// 音量按钮
    this->btnVolume->registerClickAction([this](brls::View* view) { return this->toggleVolume(view); });
    this->btnVolume->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnVolume));

    this->registerMpvEvent();

    osdSlider->getProgressSetEvent().subscribe([this](float progress) {
        brls::Logger::verbose("Set progress: {}", progress);
        this->showOSD(true);
        if (this->isLiveMode) {
            this->scrubTip.clear();
            this->seekLive(liveBehindAt(progress, this->liveBarLength()));
            return;
        }
        MPVCore::instance().seek(progress * 100, "absolute-percent");
    });

    // Hold-A scrubbing. A on the progress bar no longer toggles play straight
    // away: releasing it without moving does, otherwise it seeks. While A is
    // held, the D-pad and left stick move the position instead of focus.
    osdSlider->setHideClickAnimation(true);
    osdSlider->registerAction(
        "Scrub", brls::BUTTON_A,
        [this](brls::View*) {
            this->beginScrub();
            return true;
        },
        true);
    for (auto button : {brls::BUTTON_NAV_LEFT, brls::BUTTON_NAV_RIGHT,
                        brls::BUTTON_NAV_UP, brls::BUTTON_NAV_DOWN}) {
        osdSlider->registerAction(
            "", button, [this](brls::View*) { return this->scrubbing; }, true, true);
    }

    osdSlider->getProgressEvent().subscribe([this](float progress) {
        this->showOSD(false);
        if (this->isLiveMode) {
            const int64_t behind = liveBehindAt(progress, this->liveBarLength());
            this->showLiveBehind(behind);
            this->scrubTip = behind == 0 ? "LIVE" : "-" + misc::sec2Time(behind);
            this->scrubTipProgress = progress;
        }
    });

    /// 组件触摸事件
    /// 单击控制 OSD
    /// 双击控制播放与暂停
    /// 长按加速
    /// 滑动调整进度
    /// 左右侧滑动调整音量，在支持调节背光的设备上左侧滑动调节背光亮度，右侧调节音量
    this->addGestureRecognizer(new OsdGestureRecognizer([this](OsdGestureStatus status) {
        auto& mpv = MPVCore::instance();
        if (status.osdGestureType == OsdGestureType::TAP) {
            this->toggleOSD();
            return;
        }
        if (!MPVCore::TOUCH_GESTURE) return;

        switch (status.osdGestureType) {
        case OsdGestureType::DOUBLE_TAP_END:
            mpv.togglePlay();
            break;
        case OsdGestureType::LONG_PRESS_START: {
            if (this->isLiveMode) break;
            float cur = MPVCore::VIDEO_SPEED == 100 ? 2.0 : MPVCore::VIDEO_SPEED * 0.01f;
            MPVCore::instance().setSpeed(cur);
            // 绘制临时加速标识
            this->speedHintLabel->setText(fmt::format(fmt::runtime("main/player/speed_up"_i18n), cur));
            this->speedHintBox->setVisibility(brls::Visibility::VISIBLE);
            break;
        }
        case OsdGestureType::LONG_PRESS_CANCEL:
        case OsdGestureType::LONG_PRESS_END:
            mpv.setSpeed(1.0f);
            this->speedHintBox->setVisibility(brls::Visibility::GONE);
            break;
        case OsdGestureType::HORIZONTAL_PAN_START:
            break;
        case OsdGestureType::HORIZONTAL_PAN_UPDATE:
            this->requestSeeking(fmin(120.0f, this->barLength()) * status.deltaX);
            break;
        case OsdGestureType::HORIZONTAL_PAN_CANCEL:
            // 立即取消
            this->requestSeeking(0, VIDEO_SEEK_NODELAY);
            break;
        case OsdGestureType::HORIZONTAL_PAN_END:
            // 立即跳转
            this->requestSeeking(fmin(120.0f, this->barLength()) * status.deltaX, VIDEO_SEEK_NODELAY);
            break;
        case OsdGestureType::LEFT_VERTICAL_PAN_START:
            if (brls::Application::getPlatform()->canSetBacklightBrightness()) {
                this->brightnessInit = brls::Application::getPlatform()->getBacklightBrightness();
                infoIcon->setImageFromSVGRes("icon/ico-sun-fill.svg");
                osdInfoBox->setVisibility(brls::Visibility::VISIBLE);
                break;
            }
        case OsdGestureType::RIGHT_VERTICAL_PAN_START:
            this->volumeInit = mpv.volume;
            infoIcon->setImageFromSVGRes("icon/ico-volume.svg");
            osdInfoBox->setVisibility(brls::Visibility::VISIBLE);
            break;
        case OsdGestureType::LEFT_VERTICAL_PAN_UPDATE:
            if (brls::Application::getPlatform()->canSetBacklightBrightness()) {
                this->requestBrightness(this->brightnessInit + status.deltaY);
                break;
            }
        case OsdGestureType::RIGHT_VERTICAL_PAN_UPDATE:
            this->requestVolume(this->volumeInit + status.deltaY * 100);
            break;
        case OsdGestureType::LEFT_VERTICAL_PAN_CANCEL:
        case OsdGestureType::LEFT_VERTICAL_PAN_END:
            if (brls::Application::getPlatform()->canSetBacklightBrightness()) {
                osdInfoBox->setVisibility(brls::Visibility::GONE);
                break;
            }
        case OsdGestureType::RIGHT_VERTICAL_PAN_CANCEL:
        case OsdGestureType::RIGHT_VERTICAL_PAN_END:
            osdInfoBox->setVisibility(brls::Visibility::GONE);
            break;
        default:
            break;
        }
    }));

    /// 播放/暂停 按钮
    this->btnToggle->registerClickAction([](...) {
        MPVCore::instance().togglePlay();
        return true;
    });
    this->btnToggle->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnToggle));

    this->btnClose->registerClickAction([](...) {
        brls::sync([]() { close(); });
        return true;
    });
    this->btnClose->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnClose));

    this->registerActions(
        "main/player/toggle"_i18n, brls::BUTTON_A, KeyBind::getVideoPause(), [this](brls::View* view) {
            MPVCore::instance().togglePlay();
            if (MPVCore::OSD_ON_TOGGLE) {
                this->showOSD(true);
            }
            return true;
        });

    /// 视频详情信息
    this->profile = new VideoProfile();
    this->addView(this->profile);
    this->registerActions(
        "profile", brls::BUTTON_BACK, KeyBind::getVideoProfile(),
        [this](brls::View* view) { return this->toggleProfile(); }, true);
    this->btnCast->registerClickAction([this](...) { return this->toggleProfile(); });
    this->btnCast->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnCast));

    /// 倍速按钮
    this->btnVideoSpeed->registerClickAction([this](...) { return this->toggleSpeed(); });
    this->btnVideoSpeed->addGestureRecognizer(new brls::TapGestureRecognizer(this->btnVideoSpeed));
    this->registerActions(
        "main/player/speed"_i18n, brls::BUTTON_LSB, KeyBind::getVideoSpeed(),
        [this](...) { return this->toggleSpeed(); }, true);
}

VideoView::~VideoView() {
    brls::Logger::debug("trying delete VideoView...");
    Image::cancel(channelImage);
    this->unRegisterMpvEvent();
    disableDimming(false);

    MPVCore::instance().stop();
}

void VideoView::setContentOverlay(brls::View* overlay) {
    if (contentOverlay == overlay) return;

    if (contentOverlay) {
        this->removeView(contentOverlay, false);
        contentOverlay = nullptr;
    }

    contentOverlay = overlay;
    if (!contentOverlay) return;

    contentOverlay->setFocusable(false);

    // Position zero keeps the overlay below every OSD control in the view
    // hierarchy, which also preserves touch/click routing to the controls.
    this->addView(contentOverlay, 0);
}

void VideoView::setPlaybackRecoveryHandler(
    PlaybackRecoveryHandler handler) {
    playbackRecoveryHandler = std::move(handler);
}

void VideoView::setTitie(const std::string& title) { this->titleLabel->setText(title); }

void VideoView::showChannelButton(
    const std::string& imageUrl) {
    if (imageUrl.empty()) {
        hideChannelButton();
        return;
    }

    btnChannel->setVisibility(
        brls::Visibility::VISIBLE);
    Image::cancel(channelImage);
    channelImage->setImageFromRes(
        "img/video-card-bg.png");
    Image::with(channelImage, imageUrl);
}

void VideoView::hideChannelButton() {
    Image::cancel(channelImage);
    channelImage->setImageFromRes(
        "img/video-card-bg.png");
    btnChannel->setVisibility(
        brls::Visibility::GONE);
}

void VideoView::requestSeeking(int seek, int delay) {
    auto& mpv = MPVCore::instance();
    if (this->isLiveMode) {
        // `seek` is the total of the L/R presses so far, counted from where
        // playback is now; nothing moves until the presses stop.
        const int64_t target = std::clamp<int64_t>(
            this->currentLiveBehind() - seek, 0, static_cast<int64_t>(this->liveBarLength()));

        // The same "-time" over the bar as scrubbing, at the spot L/R step to.
        showOSD(false);
        this->showLiveBehind(target);
        this->scrubTip = target == 0 ? "LIVE" : "-" + misc::sec2Time(target);
        this->scrubTipProgress = this->osdSlider->getProgress();

        brls::cancelDelay(this->seekingIter);
        if (delay <= VIDEO_SEEK_NODELAY) {
            this->scrubTip.clear();
            this->seekingRange = 0;
            this->showOSD(true);
            if (seek == 0) return;
            this->seekLive(target);
        } else if (delay > 0) {
            ASYNC_RETAIN
            this->seekingIter = brls::delay(delay, [ASYNC_TOKEN, seek, target]() {
                ASYNC_RELEASE
                this->scrubTip.clear();
                this->seekingRange = 0;
                this->showOSD(true);
                if (seek == 0) return;
                this->seekLive(target);
            });
        }
        return;
    }

    if (mpv.duration <= 0) {
        this->seekingRange = 0;
        return;
    }
    double progress = (mpv.playback_time + seek) / mpv.duration;

    if (progress < 0) {
        progress = 0;
        seek = (int64_t)mpv.playback_time * -1;
    } else if (progress > 1) {
        progress = 1;
        seek = mpv.duration;
    }

    // The target time over the bar, as for live streams.
    showOSD(false);
    osdSlider->setProgress(progress);
    leftStatusLabel->setText(misc::sec2Time(mpv.duration * progress));
    this->scrubTip = misc::sec2Time(static_cast<int64_t>(mpv.duration * progress));
    this->scrubTipProgress = static_cast<float>(progress);

    // 延迟触发跳转进度
    brls::cancelDelay(this->seekingIter);
    if (delay <= VIDEO_SEEK_NODELAY) {
        this->scrubTip.clear();
        this->showOSD(true);
        this->seekingRange = 0;
        if (seek == 0) return;
        MPVCore::instance().seek(seek, "relative");
    } else if (delay > 0) {
        ASYNC_RETAIN
        this->seekingIter = brls::delay(delay, [ASYNC_TOKEN, seek]() {
            ASYNC_RELEASE
            this->scrubTip.clear();
            this->showOSD(true);
            this->seekingRange = 0;
            if (seek == 0) return;
            MPVCore::instance().seek(seek, "relative");
        });
    }
}

void VideoView::requestVolume(int value, int delay) {
    if (value < 0) value = 0;
    if (value > 200) value = 200;
    MPVCore::instance().setInt("volume", value);
    infoLabel->setText(fmt::format("{:+d} %", value));

    if (delay == 0) return;
    if (this->volumeIter == 0) {
        osdInfoBox->setVisibility(brls::Visibility::VISIBLE);
        infoIcon->setImageFromSVGRes("icon/ico-volume.svg");
    } else {
        brls::cancelDelay(this->volumeIter);
    }
    ASYNC_RETAIN
    volumeIter = brls::delay(delay, [ASYNC_TOKEN]() {
        ASYNC_RELEASE
        osdInfoBox->setVisibility(brls::Visibility::GONE);
        this->volumeIter = 0;
    });
}

void VideoView::requestBrightness(float value) {
    if (value < 0) value = 0.0f;
    if (value > 1) value = 1.0f;
    brls::Application::getPlatform()->setBacklightBrightness(value);
    infoLabel->setText(fmt::format("{} %", (int)(value * 100)));
    infoIcon->setImageFromSVGRes("icon/ico-sun-fill.svg");
    osdInfoBox->setVisibility(brls::Visibility::VISIBLE);
}

void VideoView::draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style, brls::FrameContext* ctx) {
    auto& mpv = MPVCore::instance();
    if (!mpv.isValid()) return;

    // draw video
    mpv.draw(this->getFrame(), this->getAlpha());
    if (this->holdPosition >= 0) {
        const bool reached = std::fabs(mpv.playback_time - this->holdPosition) < 20;
        if (reached || brls::getCPUTimeUsec() - this->holdStarted > 12000000) {
            this->holdPosition = -1;
            mpv.command("set", "pause", "no");
        } else {
            // Cover the frames from before the seek, and keep the spinner
            // up while the file sits paused.
            if (this->osdCenterBox->getVisibility() != brls::Visibility::VISIBLE)
                this->osdCenterBox->setVisibility(brls::Visibility::VISIBLE);
            nvgBeginPath(vg);
            nvgRect(vg, x, y, w, h);
            nvgFillColor(vg, nvgRGB(0, 0, 0));
            nvgFill(vg);
        }
    }

    if (MPVCore::BOTTOM_BAR) {
        NVGcontext* vg = brls::Application::getNVGContext();
        bottomBarColor.a = alpha;
        float progress = this->isLiveMode ? (this->currentLiveBehind() <= 0 ? 1.0f : this->osdSlider->getProgress()) : (mpv.playback_time / mpv.duration);
        progress = progress > 1.0f ? 1.0f : progress;
        nvgFillColor(vg, this->isLiveMode ? a(nvgRGB(83, 252, 24)) : bottomBarColor);
        nvgBeginPath(vg);
        nvgRect(vg, x, y + h - 2, w * progress, 2);
        nvgFill(vg);
    }

    // Kick chat and other content overlays belong above the video but below
    // the player OSD, so buttons, gestures, and the progress bar remain usable.
    if (contentOverlay &&
        contentOverlay->getVisibility() == brls::Visibility::VISIBLE) {
        contentOverlay->frame(ctx);
    }

    // draw osd
    brls::Time current = brls::getCPUTimeUsec();
    if (current < this->osdLastShowTime) {
        if (!this->isOsdShown) {
            this->isOsdShown = true;
        }

        osdTopBox->setVisibility(brls::Visibility::VISIBLE);
        osdBottomBox->setVisibility(brls::Visibility::VISIBLE);
        if (this->isLiveMode && !this->scrubbing && (current - this->lastLiveProgressTick > 500000)) {
            this->lastLiveProgressTick = current;
            this->updateLiveProgressUI();
        }
        osdBottomBox->frame(ctx);
        osdTopBox->frame(ctx);
        if (!this->scrubTip.empty()) this->drawScrubTip(vg);

    } else if (this->isOsdShown) {
        this->isOsdShown = false;
        // 当焦点位于video组件内部重新赋予焦点，用来隐藏屏幕上的高亮框
        if (isChildFocused()) brls::Application::giveFocus(this);
        osdTopBox->setVisibility(brls::Visibility::INVISIBLE);
        osdBottomBox->setVisibility(brls::Visibility::INVISIBLE);
    }

    // hot key
    this->buttonProcessing();

    // draw speed hint
    if (speedHintBox->getVisibility() == brls::Visibility::VISIBLE) {
        speedHintBox->frame(ctx);
        brls::Rect frame = speedHintLabel->getFrame();

        // a1-3 周期 800，范围 800 * 0.3 / 2 = 120, 0 - 120 - 0
        int ta1 = ((current >> 10) % 800) * 0.3;
        float tx = frame.getMinX() - 50;
        float ty = frame.getMinY() + 4.5;

        for (int i = 0; i < 3; i++) {
            int offx = tx + i * 15;
            int ta2 = (ta1 + i * 40) % 240;
            if (ta2 > 120) ta2 = 240 - ta2;

            nvgBeginPath(vg);
            nvgMoveTo(vg, offx, ty);
            nvgLineTo(vg, offx, ty + 12);
            nvgLineTo(vg, offx + 12, ty + 6);
            nvgFillColor(vg, a(nvgRGBA(255, 255, 255, ta2 + 80)));
            nvgClosePath(vg);
            nvgFill(vg);
        }
    }

    // cache info
    osdCenterBox->frame(ctx);

    // center hint
    osdInfoBox->frame(ctx);

    // draw video profile
    if (profile->getVisibility() == brls::Visibility::VISIBLE) {
        if (current - this->profileLastShowTime > 2000000) {
            profile->update();
            this->profileLastShowTime = current;
        }
        profile->frame(ctx);
    }
}

void VideoView::invalidate() { View::invalidate(); }

void VideoView::onChildFocusGained(View* directChild, View* focusedView) {
    Box::onChildFocusGained(directChild, focusedView);
    // 只有在全屏显示OSD时允许OSD组件获取焦点
    if (this->isOsdShown) {
        // 当弹幕按钮隐藏时不可获取焦点
        if (focusedView->getParent()->getVisibility() == brls::Visibility::GONE) {
            brls::Application::giveFocus(this);
        }
        // 设定自定义导航
        if (focusedView == this->osdSettingIcon) {
            this->osdSettingIcon->setCustomNavigationRoute(brls::FocusDirection::DOWN,
                this->isLiveMode ? "video/osd/toggle" : "video/speed/label");
        }
        lastFocusedView = focusedView;
        return;
    }
    brls::Application::giveFocus(this);
}

void VideoView::buttonProcessing() {
    if (this->scrubbing) {
        this->scrubProcessing();
        if (this->scrubbing) return;
    }

    // The player keeps drawing underneath overlays such as the chat composer.
    // Only react to stick/d-pad input while focus belongs to the player;
    // otherwise waking the OSD would pull focus out of the overlay on top.
    brls::View* currentFocus = brls::Application::getCurrentFocus();
    if (currentFocus != this && !isChildFocused()) return;
    // While the player is closing it is still drawn for a frame but is no
    // longer the top screen; never pull focus back into it.
    auto stack = brls::Application::getActivitiesStack();
    if (stack.empty() || stack.back() != getParentActivity()) return;

    // 获取按键数据
    auto& state = brls::Application::getControllerState();

    const bool navInputActive =
        std::abs(state.axes[brls::ControllerAxis::LEFT_X]) > 0.4f ||
        std::abs(state.axes[brls::ControllerAxis::LEFT_Y]) > 0.4f ||
        state.buttons[brls::BUTTON_NAV_RIGHT] || state.buttons[brls::BUTTON_NAV_LEFT] ||
        state.buttons[brls::BUTTON_NAV_UP] || state.buttons[brls::BUTTON_NAV_DOWN];

    // 当OSD显示时上下左右切换选择按钮，持续显示OSD
    if (this->isOsdShown) {
        if (navInputActive) {
            if (this->osdState == OSDState::SHOWN) this->showOSD(true);
        }
    } else {
        // 当OSD隐藏时，轻推左摇杆或十字键唤醒播放控制栏，不影响流播放
        if (navInputActive && !this->seekingRange) {
            this->showOSD(true);
            if (this->lastFocusedView && this->lastFocusedView->isFocusable()) {
                brls::Application::giveFocus(this->lastFocusedView);
            } else if (this->btnToggle && this->btnToggle->isFocusable()) {
                brls::Application::giveFocus(this->btnToggle);
            }
        }
    }
}

bool VideoView::isProgressBarFocused() {
    brls::View* focus = brls::Application::getCurrentFocus();
    return focus && (focus == osdSlider || focus->getParent() == osdSlider);
}

double VideoView::barLength() const {
    return this->isLiveMode ? this->liveBarLength() : MPVCore::instance().duration;
}

double VideoView::liveBarLength() const {
    // With a recording the whole broadcast can be rewound; without one,
    // only the live cache.
    // Without one (Kick said so), only what is cached right now.
    if (this->liveRecordingChecked && !this->liveRecordingAvailable) {
        const double cacheBack = std::min(MPVCore::instance().getCacheBackSeconds(), 180.0);
        return std::max<double>(10.0, this->liveRewindSeconds + cacheBack);
    }
    // No fixed window: the whole broadcast once its start is known, and
    // until then what the cache holds.
    const int64_t uptime = this->getCurrentStreamUptime();
    if (uptime > 0) return std::max<double>(10.0, uptime);
    const double cacheBack = std::min(MPVCore::instance().getCacheBackSeconds(), 180.0);
    return std::max<double>(10.0, this->liveRewindSeconds + cacheBack);
}

int64_t VideoView::currentLiveBehind() const {
    if (!this->watchingRecording) return this->liveRewindSeconds;
    // While the recording opens, mpv reports 0 until it has seeked; show the
    // spot it is going to instead.
    const double position = this->holdPosition >= 0 ? this->holdPosition : MPVCore::instance().playback_time;
    return std::max<int64_t>(0, this->getCurrentStreamUptime() - static_cast<int64_t>(position));
}

void VideoView::beginScrub() {
    if (this->barLength() <= 0) return;
    // An L/R seek still waiting to fire would land in the middle of the
    // scrub; finish it now so the scrub starts from where it was going.
    if (this->seekingRange != 0) this->requestSeeking(this->seekingRange, VIDEO_SEEK_NODELAY);
    this->scrubbing = true;
    this->scrubMoved = false;
    this->scrubProgress = osdSlider->getProgress();
    this->scrubDirection = 0;
    this->scrubLastTick = brls::getCPUTimeUsec();
    // Keep the OSD up while the button is held.
    this->showOSD(false);
}

void VideoView::scrubProcessing() {
    auto& state = brls::Application::getControllerState();
    auto stack = brls::Application::getActivitiesStack();
    const bool playerOnTop = !stack.empty() && stack.back() == getParentActivity();
    const double length = this->barLength();

    if (!playerOnTop || !isProgressBarFocused() || length <= 0) {
        this->endScrub(false);
        return;
    }
    if (!state.buttons[brls::BUTTON_A]) {
        this->endScrub(true);
        return;
    }

    const brls::Time now = brls::getCPUTimeUsec();
    const float dt = std::min(0.1f, static_cast<float>(now - this->scrubLastTick) / 1000000.0f);
    this->scrubLastTick = now;

    // The left stick is analog (a light tilt moves slowly); the D-pad moves
    // one 5 s step per press and then glides while held.
    const float stick = state.axes[brls::ControllerAxis::LEFT_X];
    const bool dpadLeft = state.buttons[brls::BUTTON_LEFT];
    const bool dpadRight = state.buttons[brls::BUTTON_RIGHT];
    float amount = 0;
    bool dpad = false;
    if (std::fabs(stick) > 0.2f) {
        amount = (std::fabs(stick) - 0.2f) / 0.8f * (stick < 0 ? -1.0f : 1.0f);
    } else if (dpadLeft != dpadRight) {
        amount = dpadLeft ? -1.0f : 1.0f;
        dpad = true;
    }

    const int direction = amount < 0 ? -1 : (amount > 0 ? 1 : 0);
    double deltaSeconds = 0;
    if (direction != this->scrubDirection) {
        this->scrubDirection = direction;
        this->scrubDirectionSince = now;
        if (dpad) deltaSeconds += direction * 5.0;
    }

    if (direction != 0) {
        const float held = static_cast<float>(now - this->scrubDirectionSince) / 1000000.0f;
        // Base speed scales with the length so long VODs stay quick to cross;
        // holding a direction speeds up further, up to 5x after 6 s.
        const double base = std::clamp(length / 120.0, 10.0, 120.0);
        const double accel = 1.0 + std::min(held, 6.0f) * 0.7;
        if (!dpad || held > 0.35f) deltaSeconds += amount * base * accel * dt;
    }

    this->moveScrub(deltaSeconds);
}

void VideoView::moveScrub(double deltaSeconds) {
    const double length = this->barLength();
    if (deltaSeconds == 0 || length <= 0) return;

    this->scrubProgress = std::clamp(
        this->scrubProgress + static_cast<float>(deltaSeconds / length), 0.0f, 1.0f);
    this->scrubMoved = true;
    osdSlider->setProgress(this->scrubProgress);

    // Like kick.com, small text over the bar: how far behind live, or the
    // time in a VOD.
    this->scrubTipProgress = this->scrubProgress;
    if (this->isLiveMode) {
        const int64_t behind = liveBehindAt(this->scrubProgress, length);
        this->scrubTip = behind == 0 ? "LIVE" : "-" + misc::sec2Time(behind);
    } else {
        this->scrubTip = misc::sec2Time(static_cast<int64_t>(this->scrubProgress * length));
        leftStatusLabel->setText(this->scrubTip);
    }
}

void VideoView::endScrub(bool released) {
    const bool moved = this->scrubMoved;
    this->scrubTip.clear();
    this->scrubbing = false;
    this->scrubMoved = false;
    this->scrubDirection = 0;

    if (!released) {
        // Focus left the bar (or the player closed): drop the preview.
        this->showOSD(true);
        return;
    }

    if (moved) {
        // Seeks (VOD or live rewind) and brings back the auto-hiding OSD.
        osdSlider->getProgressSetEvent().fire(this->scrubProgress);
        return;
    }

    // A tap without moving keeps the old behaviour: play / pause.
    MPVCore::instance().togglePlay();
    this->showOSD(true);
}

void VideoView::registerMpvEvent() {
    auto& mpv = MPVCore::instance();
    this->eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
        auto& mpv = MPVCore::instance();
        // brls::Logger::info("mpv event => : {}", event);
        switch (event) {
        case MpvEventEnum::MPV_RESUME:
            this->customLoadingNote.clear();
            if (MPVCore::OSD_ON_TOGGLE) {
                this->showOSD(true);
            }
            this->hideLoading(true);
            this->toggleIcon->setImageFromSVGRes("icon/ico-pause.svg");
            break;
        case MpvEventEnum::MPV_PAUSE:
            if (MPVCore::OSD_ON_TOGGLE) {
                this->showOSD(false);
            }
            hideLoading(false);
            this->toggleIcon->setImageFromSVGRes("icon/ico-play.svg");
            break;
        case MpvEventEnum::START_FILE:
            if (MPVCore::OSD_ON_TOGGLE) {
                this->showOSD(false);
            }
            break;
        case MpvEventEnum::LOADING_START:
            this->showLoading();
            break;
        case MpvEventEnum::LOADING_END:
            this->hideLoading();
            break;
        case MpvEventEnum::UPDATE_DURATION:
            if (this->seekingRange == 0 && !this->scrubbing) {
                if (this->isLiveMode) {
                    // A freshly loaded recording ends where Kick had got to,
                    // a little before live: a truer clock than the uptime the
                    // stream was opened with, which can be minutes stale.
                    if (this->watchingRecording && mpv.duration > 0)
                        this->setStreamUptime(mpv.duration + LIVE_RECORDING_DELAY);
                    this->updateLiveProgressUI();
                } else {
                    this->rightStatusLabel->setText(misc::sec2Time(mpv.duration));
                    this->osdSlider->setProgress(mpv.playback_time / mpv.duration);
                }
            }
            break;
        case MpvEventEnum::UPDATE_PROGRESS:
            if (this->seekingRange == 0 && !this->scrubbing) {
                if (this->isLiveMode) {
                    this->updateLiveProgressUI();
                } else {
                    this->leftStatusLabel->setText(misc::sec2Time(mpv.video_progress));
                    this->osdSlider->setProgress(mpv.playback_time / mpv.duration);
                }
            }
            break;
        case MpvEventEnum::VIDEO_SPEED_CHANGE:
            if (std::fabs(mpv.video_speed - 1) < 1e-5) {
                this->videoSpeedLabel->setText("main/player/speed"_i18n);
            } else {
                this->videoSpeedLabel->setText(fmt::format("{}x", mpv.video_speed));
            }
            break;
        case MpvEventEnum::END_OF_FILE:
            if (playbackRecoveryHandler &&
                playbackRecoveryHandler(event)) {
                // Kick can briefly end the current HLS presentation while
                // switching from an inserted commercial back to the live feed.
                this->showLoading();
                break;
            }

            disableDimming(false);
            this->toggleIcon->setImageFromSVGRes("icon/ico-play.svg");
            this->playIndexEvent.fire(++this->playIndex);
            break;
        case MpvEventEnum::VIDEO_MUTE:
            this->volumeIcon->setImageFromSVGRes("icon/ico-volume-off.svg");
            break;
        case MpvEventEnum::VIDEO_UNMUTE:
            this->volumeIcon->setImageFromSVGRes("icon/ico-volume.svg");
            break;
        case MpvEventEnum::MPV_FILE_ERROR: {
            if (playbackRecoveryHandler &&
                playbackRecoveryHandler(event)) {
                this->showLoading();
                break;
            }

            auto dialog = new brls::Dialog("main/player/error"_i18n);
            dialog->addButton(
                "hints/back"_i18n,
                []() { VideoView::close(true); });
            dialog->open();
            break;
        }
        default:;
        }
    });
}

void VideoView::unRegisterMpvEvent() {
    auto& mpv = MPVCore::instance();
    mpv.getEvent()->unsubscribe(eventSubscribeID);
}

// Loading
void VideoView::setLoadingNote(const std::string& note) {
    this->customLoadingNote = note;
    if (!this->customLoadingNote.empty()) {
        this->centerLabel->setText(this->customLoadingNote);
        this->centerLabel->setVisibility(brls::Visibility::VISIBLE);
        this->osdCenterBox->setVisibility(brls::Visibility::VISIBLE);
    } else {
        this->centerLabel->setVisibility(brls::Visibility::GONE);
    }
}

void VideoView::showLoading() {
    if (!this->customLoadingNote.empty()) {
        this->centerLabel->setText(this->customLoadingNote);
        this->centerLabel->setVisibility(brls::Visibility::VISIBLE);
    } else {
        this->centerLabel->setVisibility(brls::Visibility::GONE);
    }
    this->osdCenterBox->setVisibility(brls::Visibility::VISIBLE);
    disableDimming(false);
}

void VideoView::hideLoading(bool dimming) {
    this->customLoadingNote.clear();
    this->centerLabel->setVisibility(brls::Visibility::GONE);
    this->osdCenterBox->setVisibility(brls::Visibility::GONE);
    disableDimming(dimming);
}

bool VideoView::toggleProfile() {
    if (profile->getVisibility() == brls::Visibility::VISIBLE) {
        profile->setVisibility(brls::Visibility::INVISIBLE);
        return false;
    }
    profile->setVisibility(brls::Visibility::VISIBLE);
    profile->update();
    return true;
}

/// OSD
void VideoView::toggleOSD() {
    if (this->isOsdShown) {
        this->hideOSD();
    } else {
        this->showOSD(true);
    }
}

void VideoView::showOSD(bool autoHide) {
    if (autoHide) {
        this->osdLastShowTime = brls::getCPUTimeUsec() + VideoView::OSD_SHOW_TIME;
        this->osdState = OSDState::SHOWN;
    } else {
        this->osdLastShowTime = std::numeric_limits<std::time_t>::max();
        this->osdState = OSDState::ALWAYS_ON;
    }
}

void VideoView::hideOSD() {
    this->osdLastShowTime = 0;
    this->osdState = OSDState::HIDDEN;
}

void VideoView::setTvMode(bool state) {
    btnToggle->setCustomNavigationRoute(brls::FocusDirection::RIGHT, state ? (brls::View*)osdSlider : (brls::View*)iconBox);
    volumeIcon->setCustomNavigationRoute(brls::FocusDirection::UP, state ? (brls::View*)osdSlider : (brls::View*)osdSettingIcon);
    iconVideoQuality->setCustomNavigationRoute(brls::FocusDirection::UP, state ? (brls::View*)osdSlider : (brls::View*)osdSettingIcon);
    iconVideoSpeed->setCustomNavigationRoute(brls::FocusDirection::UP, state ? (brls::View*)osdSlider : (brls::View*)osdSettingIcon);
    osdSlider->setFocusable(state);
}

bool VideoView::toggleSpeed() {
    if (this->isLiveMode) return false;
    brls::Dropdown* dropdown = new brls::Dropdown(
        "main/player/speed"_i18n, {"2.0x", "1.75x", "1.5x", "1.25x", "1.0x", "0.75x", "0.5x"},
        [](int selected) { MPVCore::instance().setSpeed((200 - selected * 25) / 100.0f); },
        int(200 - MPVCore::instance().video_speed * 100) / 25);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool VideoView::toggleVolume(brls::View* view) {
    // 一直显示 OSD
    this->showOSD(false);
    auto theme = brls::Application::getTheme();
    auto container = new brls::Box();
    container->setHideClickAnimation(true);
    container->registerAction("hints/back"_i18n, brls::BUTTON_B, [this](brls::View* view) {
        // 几秒后自动关闭 OSD
        this->showOSD(true);
        view->dismiss();
        // 保存结果
        return true;
    });
    container->addGestureRecognizer(new brls::TapGestureRecognizer(container, [this, container]() {
        // 几秒后自动关闭 OSD
        this->showOSD(true);
        container->dismiss();
        // 保存结果
        return true;
    }));
    // 滑动条背景
    auto sliderBox = new brls::Box();
    sliderBox->setAlignItems(brls::AlignItems::CENTER);
    sliderBox->setHeight(40);
    sliderBox->setCornerRadius(4);
    sliderBox->setBackgroundColor(theme.getColor("color/grey_1"));
    float sliderX = view->getX() - 120;
    if (sliderX < 0) sliderX = 20;
    if (sliderX > brls::Application::ORIGINAL_WINDOW_WIDTH - 332)
        sliderX = brls::Application::ORIGINAL_WINDOW_WIDTH - 332;
    sliderBox->setTranslationX(sliderX);
    sliderBox->setTranslationY(view->getY() - 70);
    // 滑动条
    auto slider = new brls::Slider();
    slider->setMargins(8, 16, 8, 16);
    slider->setWidth(300);
    slider->setHeight(20);
    slider->setProgress(MPVCore::instance().getInt("volume") / 200.0f);
    slider->getProgressEvent()->subscribe([](float progress) { MPVCore::instance().setInt("volume", progress * 200); });
    sliderBox->addView(slider);
    container->addView(sliderBox);
    auto frame = new brls::AppletFrame(container);
    frame->setInFadeAnimation(true);
    frame->setHeaderVisibility(brls::Visibility::GONE);
    frame->setFooterVisibility(brls::Visibility::GONE);
    frame->setBackgroundColor(theme.getColor("brls/backdrop"));
    brls::Application::pushActivity(new brls::Activity(frame));
    return true;
}

static VideoView::CloseHandler s_globalCloseHandler = nullptr;

void VideoView::setGlobalCloseHandler(CloseHandler handler) {
    s_globalCloseHandler = std::move(handler);
}

bool VideoView::close(bool quit) {
    if (s_globalCloseHandler) {
        s_globalCloseHandler();
    }
    MPVCore::instance().stop();
    disableDimming(false);

    auto stack = brls::Application::getActivitiesStack();
    if (stack.size() > 1) {
        // Borealis moves focus to the screen below before the player is
        // deleted. Anything that hands focus back to the player in between
        // (stick input, orientation changes) would leave focus on a deleted
        // view, so move it off the player right before it is freed.
        brls::Activity* leaving = stack.back();
        return brls::Application::popActivity(brls::TransitionAnimation::NONE, [leaving]() {
            brls::View* focus = brls::Application::getCurrentFocus();
            if (!focus || focus->getParentActivity() != leaving) return;
            auto remaining = brls::Application::getActivitiesStack();
            for (auto it = remaining.rbegin(); it != remaining.rend(); ++it) {
                if (*it != leaving && (*it)->getContentView()) {
                    brls::Application::giveFocus((*it)->getContentView());
                    return;
                }
            }
        });
    }

    if (quit) {
        brls::Application::quit();
        return true;
    }

    auto dialog = new brls::Dialog("hints/exit_hint"_i18n);
    dialog->addButton("hints/cancel"_i18n, []() {});
    dialog->addButton("hints/ok"_i18n, []() { brls::Application::quit(); });
    dialog->open();
    return false;
}

void VideoView::disableDimming(bool disable) {
    brls::Application::getPlatform()->disableScreenDimming(disable, "Playing video", AppVersion::getPackageName());
    brls::Application::setAutomaticDeactivation(!disable);
}

void VideoView::hideVideoQuality() {
    this->btnVideoQuality->setVisibility(brls::Visibility::GONE);
    for (brls::View* view : this->btnVideoQuality->getChildren()) view->setVisibility(brls::Visibility::GONE);
}

void VideoView::registerActions(const std::string& hintText, const brls::ControllerButton button,
    const brls::BrlsKeyCombination key, const brls::ActionListener& actionListener, bool hidden, bool allowRepeating) {
    this->registerAction(hintText, button, actionListener, hidden, allowRepeating);
    this->registerAction(key, actionListener, allowRepeating);
}

void VideoView::setLiveMode(bool live, int64_t uptimeSeconds) {
    this->isLiveMode = live;
    this->liveRewindSeconds = 0;
    this->watchingRecording = false;
    if (live) {
        this->setStreamUptime(uptimeSeconds);
        this->osdSlider->setProgress(1.0f);
        this->leftStatusLabel->setVisibility(brls::Visibility::GONE);
        if (this->centerStatusLabel) this->centerStatusLabel->setVisibility(brls::Visibility::GONE);
        this->btnVideoSpeed->setVisibility(brls::Visibility::GONE);
        this->btnVideoSpeed->setFocusable(false);
        this->updateLiveProgressUI();
        MPVCore::instance().setSpeed(1.0);
    } else {
        this->leftStatusLabel->setVisibility(brls::Visibility::VISIBLE);
        if (this->centerStatusLabel) this->centerStatusLabel->setVisibility(brls::Visibility::VISIBLE);
        this->btnVideoSpeed->setVisibility(brls::Visibility::VISIBLE);
        this->btnVideoSpeed->setFocusable(true);
    }
}

void VideoView::setStreamUptime(int64_t uptimeSeconds) {
    this->streamUptimeBaseline = uptimeSeconds;
    this->streamUptimeBaselineTime = std::chrono::steady_clock::now();
    if (this->isLiveMode) {
        this->updateLiveProgressUI();
    }
}

int64_t VideoView::getCurrentStreamUptime() const {
    if (streamUptimeBaseline <= 0) return 0;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - streamUptimeBaselineTime).count();
    return streamUptimeBaseline + std::max<int64_t>(0, elapsed);
}

void VideoView::updateLiveProgressUI() {
    if (!this->isLiveMode) return;

    const int64_t behind = this->currentLiveBehind();
    if (behind <= 0 && !this->watchingRecording) {
        const int64_t uptime = this->getCurrentStreamUptime();
        this->osdSlider->setProgress(1.0f);
        this->leftStatusLabel->setVisibility(brls::Visibility::GONE);
        if (this->centerStatusLabel) this->centerStatusLabel->setVisibility(brls::Visibility::GONE);
        this->rightStatusLabel->setVisibility(brls::Visibility::VISIBLE);
        this->rightStatusLabel->setText(uptime > 0 ? misc::sec2Time(uptime) : "LIVE");
    } else {
        this->showLiveBehind(behind);
    }
}

void VideoView::showLiveBehind(int64_t behind) {
    const int64_t uptime = this->getCurrentStreamUptime();
    const float progress = std::clamp(
        1.0f - static_cast<float>(static_cast<double>(behind) / this->liveBarLength()), 0.0f, 1.0f);
    this->osdSlider->setProgress(progress);

    // One time, like kick.com: where playback is in the broadcast, which
    // counts up while playing (the uptime at live). How far behind live is
    // shown over the bar while scrubbing. Without a known start only that
    // distance can be told.
    std::string time;
    if (uptime > 0) time = misc::sec2Time(std::max<int64_t>(0, uptime - std::max<int64_t>(0, behind)));
    else time = behind <= 0 ? "LIVE" : "-" + misc::sec2Time(behind);
    this->leftStatusLabel->setVisibility(brls::Visibility::GONE);
    if (this->centerStatusLabel) this->centerStatusLabel->setVisibility(brls::Visibility::GONE);
    this->rightStatusLabel->setVisibility(brls::Visibility::VISIBLE);
    this->rightStatusLabel->setText(time);
}

void VideoView::seekLive(int64_t behind) {
    auto& mpv = MPVCore::instance();
    behind = std::clamp<int64_t>(behind, 0, static_cast<int64_t>(this->liveBarLength()));

    if (this->watchingRecording) {
        // The recording stops where it was when it was opened, a little
        // before live; anything past that is reloaded (or goes live) by the
        // player.
        const int64_t target = this->getCurrentStreamUptime() - behind;
        if (behind >= this->liveRecordingGap() && mpv.duration > 0 && target < mpv.duration - 5) {
            mpv.seek(std::max<int64_t>(0, target), "absolute");
        } else if (this->liveSeekHandler) {
            this->liveSeekHandler(behind);
        }
        this->showLiveBehind(behind);
        return;
    }

    // How far back the live cache reaches, keeping a little margin. It holds
    // well under a minute; never trust a larger figure.
    const double cacheBack = std::min(mpv.getCacheBackSeconds(), 180.0);
    const int64_t reach = this->liveRewindSeconds + static_cast<int64_t>(cacheBack) - 2;
    if (behind > reach) {
        // Also while Kick hasn't answered yet: the player looks it up then.
        const bool mayHaveRecording = this->liveRecordingAvailable || !this->liveRecordingChecked;
        // Between the cache and the recording's end there is nothing to
        // play. Opening the recording there sat loading until it gave up, so
        // land on whichever edge is closer.
        const int64_t gap = this->liveRecordingGap();
        if (mayHaveRecording && this->liveSeekHandler && behind < gap)
            behind = (gap - behind <= behind - reach) ? gap : std::max<int64_t>(0, reach);
        if (mayHaveRecording && this->liveSeekHandler && behind >= gap) {
            // Keeps the bar on the chosen spot while the player looks up and
            // opens the recording, instead of snapping back to live.
            this->liveRewindSeconds = behind;
            this->showLiveBehind(behind);
            this->liveSeekHandler(behind);
            return;
        }
        if (cacheBack > 0) {
            behind = std::max<int64_t>(0, reach);
            const brls::Time now = brls::getCPUTimeUsec();
            if (this->liveRecordingChecked && !this->liveRecordingAvailable &&
                now - this->lastRewindLimitNote > 20000000) {
                this->lastRewindLimitNote = now;
                brls::Application::notify(
                    "This channel has no live VOD, so only the last few seconds can be rewound.");
            }
        }
    }

    const int64_t diff = this->liveRewindSeconds - behind;
    this->liveRewindSeconds = behind;
    if (diff != 0) mpv.seek(diff, "relative");
    this->updateLiveProgressUI();
}

void VideoView::holdUntilPosition(double position) {
    this->holdPosition = position > 5 ? position : -1;
    this->holdStarted = brls::getCPUTimeUsec();
}

void VideoView::drawScrubTip(NVGcontext* vg) {
    const brls::Rect bar = this->osdSlider->getFrame();
    if (bar.getWidth() <= 0) return;
    const float fontSize = 18;
    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgFontSize(vg, fontSize);
    float bounds[4];
    nvgTextBounds(vg, 0, 0, this->scrubTip.c_str(), nullptr, bounds);
    const float textWidth = bounds[2] - bounds[0];
    const float padding = 6;
    const float boxWidth = textWidth + padding * 2;
    const float boxHeight = fontSize + padding;
    const brls::Rect view = this->getFrame();
    float x = bar.getMinX() + this->scrubTipProgress * bar.getWidth() - boxWidth / 2;
    x = std::clamp(x, view.getMinX() + 4, view.getMaxX() - boxWidth - 4);
    const float y = bar.getMinY() - boxHeight - 4;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, boxWidth, boxHeight, 4);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
    nvgFill(vg);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + padding, y + boxHeight / 2, this->scrubTip.c_str(), nullptr);
}

void VideoView::setLiveSeekHandler(LiveSeekHandler handler) {
    this->liveSeekHandler = std::move(handler);
}

void VideoView::setLiveRecordingAvailable(bool available) {
    this->liveRecordingAvailable = available;
    this->liveRecordingChecked = true;
    if (this->isLiveMode) this->updateLiveProgressUI();
}

void VideoView::setWatchingRecording(bool watching) {
    this->watchingRecording = watching;
    if (!watching) this->holdPosition = -1;
    this->liveRewindSeconds = 0;
    if (this->isLiveMode) this->updateLiveProgressUI();
}