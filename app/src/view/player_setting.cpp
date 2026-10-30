#include "api/kick.hpp"
#include "api/kick_chat.hpp"
#include "utils/config.hpp"
#include "utils/event.hpp"
#include "utils/haptics.hpp"
#include "utils/orientation.hpp"
#include "utils/thread.hpp"
#include "view/button_close.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace brls::literals;

namespace {

const std::vector<std::string>& kickQualityLabels() {
    static const std::vector<std::string> labels = {
        "1080p60", "720p60", "480p", "360p", "160p", "Auto (Recommended)",
    };
    return labels;
}

const std::vector<std::string>& kickQualityValues() {
    static const std::vector<std::string> values = {
        "1080p60", "720p60", "480p", "360p", "160p", "auto",
    };
    return values;
}

std::atomic_uint64_t kickQualitySwitchGeneration{0};

int kickQualityIndex(const std::string& quality) {
    const auto& values = kickQualityValues();
    if (quality == "source") {
        const auto found = std::find(values.begin(), values.end(), "1080p60");
        if (found != values.end()) return static_cast<int>(std::distance(values.begin(), found));
    }
    if (quality == "720p") {
        const auto found = std::find(values.begin(), values.end(), "720p60");
        if (found != values.end()) return static_cast<int>(std::distance(values.begin(), found));
    }
    const auto found = std::find(values.begin(), values.end(), quality);
    return found == values.end() ? 0 : static_cast<int>(std::distance(values.begin(), found));
}

const std::vector<std::string>& kickDecoderLabels() {
    static const std::vector<std::string> labels = {
        "Software",
        "Hardware",
        "Hybrid (recommended)",
    };
    return labels;
}

const std::vector<std::string>& portraitOrientationLabels() {
    static const std::vector<std::string> labels = {
        "Auto (Joy-Con sensor)",
        "Landscape (recommended)",
        "Portrait clockwise",
        "Portrait counter-clockwise",
    };
    return labels;
}

const std::vector<std::string>& keyboardHapticLabels() {
    static const std::vector<std::string> labels = {
        "Off", "Light", "Medium", "Strong", "Maximum",
    };
    return labels;
}


const std::vector<std::string>& chatModeLabels() {
    static const std::vector<std::string> labels = {
        "Off", "Right panel", "Overlay (recommended)",
    };
    return labels;
}

const std::vector<std::string>& chatParticipationLabels() {
    static const std::vector<std::string> labels = {
        "Read only",
        "Interactive",
    };
    return labels;
}

const std::vector<std::string>& chatComposerLabels() {
    static const std::vector<std::string> labels = {
        "Nintendo keyboard",
        "kickNX keyboard (recommended)",
    };
    return labels;
}

const std::vector<std::string>& chatEmoteModeLabels() {
    static const std::vector<std::string> labels = {
        "Static",
        "Animated",
    };
    return labels;
}

const std::vector<std::string>& chatOverlaySizeLabels() {
    static const std::vector<std::string> labels = {
        "Full height",
        "Compact",
    };
    return labels;
}

const std::vector<std::string>& chatOverlayPositionLabels() {
    static const std::vector<std::string> labels = {
        "Top right",
        "Top left",
        "Bottom right",
        "Bottom left",
    };
    return labels;
}

const std::vector<std::string>& chatDockedWidthLabels() {
    static const std::vector<std::string> labels = {
        "Compact", "TV style", "Wide",
    };
    return labels;
}

const std::vector<int>& chatDockedWidthValues() {
    static const std::vector<int> values = {300, 330, 380};
    return values;
}

const std::vector<std::string>& chatOverlayWidthLabels() {
    static const std::vector<std::string> labels = {
        "Compact", "Standard", "Wide",
    };
    return labels;
}

const std::vector<int>& chatOverlayWidthValues() {
    static const std::vector<int> values = {300, 360, 430};
    return values;
}

const std::vector<std::string>& chatFontLabels() {
    static const std::vector<std::string> labels = {
        "Small", "Medium", "Large",
    };
    return labels;
}

const std::vector<int>& chatFontValues() {
    static const std::vector<int> values = {13, 16, 19};
    return values;
}

const std::vector<std::string>& chatOpacityLabels() {
    static const std::vector<std::string> labels = {
        "40% (recommended)", "60%", "80%", "95%",
    };
    return labels;
}

const std::vector<int>& chatOpacityValues() {
    static const std::vector<int> values = {40, 60, 80, 95};
    return values;
}

int closestIndex(const std::vector<int>& values, int wanted) {
    int best = 0;
    int distance = std::abs(values.front() - wanted);
    for (size_t index = 1; index < values.size(); ++index) {
        const int candidate = std::abs(values[index] - wanted);
        if (candidate < distance) {
            distance = candidate;
            best = static_cast<int>(index);
        }
    }
    return best;
}

}  // namespace

PlayerSetting::PlayerSetting(std::string kickChannel) {
    this->inflateFromXMLRes("xml/view/player_setting.xml");
    brls::Logger::debug("PlayerSetting: create");
    this->audioTrack->detail->setVisibility(brls::Visibility::GONE);
    this->subtitleTrack->detail->setVisibility(brls::Visibility::GONE);

    const std::string savedQuality = kick::loadPreferredQuality().empty()
        ? "auto"
        : kick::loadPreferredQuality();
    btnKickQuality->init(
        "Kick stream quality",
        kickQualityLabels(),
        kickQualityIndex(savedQuality),
        [kickChannel](int selected) {
            if (selected < 0 || static_cast<size_t>(selected) >= kickQualityValues().size())
                return;

            const std::string quality = kickQualityValues().at(selected);
            const std::string qualityLabel = kickQualityLabels().at(selected);
            const uint64_t generation =
                kickQualitySwitchGeneration.fetch_add(1) + 1;
            if (!kick::savePreferredQuality(quality))
                brls::Application::notify("kickNX: could not save quality preference");

            if (kickChannel.empty()) {
                brls::Application::notify("kickNX: preferred quality set to " + qualityLabel);
                return;
            }

            auto config = kick::loadConfig();
                config.channel = kickChannel;
                config.preferredQuality = quality;
                brls::Logger::info(
                    "kickNX: selected Kick quality {}; waiting for final "
                    "selector value before reloading",
                    qualityLabel);

                ThreadPool::instance().submit(
                    [config, generation](HTTP&) mutable {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(650));

                        if (generation != kickQualitySwitchGeneration.load())
                            return;

                        try {
                            auto result = kick::resolveLive(config);
                            brls::sync(
                                [generation, result = std::move(result)]()
                                    mutable {
                                    if (generation !=
                                        kickQualitySwitchGeneration.load())
                                        return;
                                    MPVCore::BOTTOM_BAR = false;
                                    MPVCore::instance().setUrl(
                                        result.selected.url,
                                        kick::mpvExtra());
                                    brls::Application::notify(
                                        "kickNX: switched to " +
                                        result.selected.name);
                                });
                        } catch (const std::exception& exception) {
                            const std::string message = exception.what();
                            brls::sync([generation, message]() {
                                if (generation !=
                                    kickQualitySwitchGeneration.load())
                                    return;
                                brls::Application::notify(
                                    "kickNX quality error: " + message);
                            });
                        } catch (...) {
                            brls::sync([generation]() {
                                if (generation !=
                                    kickQualitySwitchGeneration.load())
                                    return;
                                brls::Application::notify(
                                    "kickNX quality error: unknown resolver "
                                    "error");
                            });
                        }
                    });
            });

        auto& orientationController =
            kicknx::portrait::OrientationController::instance();
        btnPortraitOrientation->init(
            "Display orientation (experimental)",
            portraitOrientationLabels(),
            static_cast<int>(orientationController.mode()),
            [](int selected) {
                const int bounded = std::clamp(selected, 0, 3);
                auto& controller =
                    kicknx::portrait::OrientationController::instance();
                if (!controller.setMode(
                        static_cast<kicknx::portrait::OrientationMode>(bounded))) {
                    brls::Application::notify(
                        "kickNX: could not save orientation preference");
                    return;
                }
                if (bounded == 0) {
                    brls::Application::notify(
                        "kickNX: automatic Joy-Con orientation enabled");
                }
            });

        btnKeyboardHaptic->init(
            "Touch keyboard vibration intensity",
            keyboardHapticLabels(),
            std::clamp(
                static_cast<int>(std::lround(
                    kicknx::haptics::keyboardIntensity() * 4.0f)),
                0,
                4),
            [](int selected) {
                const float intensity =
                    static_cast<float>(std::clamp(selected, 0, 4)) / 4.0f;
                if (!kicknx::haptics::setKeyboardIntensity(intensity))
                    brls::Application::notify(
                        "kickNX: could not save vibration preference");
                kicknx::haptics::keyboardPulse();
            });

        const kick::DecoderMode decoderMode =
            kick::loadDecoderMode();

        btnKickDecoder->init(
            "Kick decoder",
            kickDecoderLabels(),
            static_cast<int>(decoderMode),
            [kickChannel](int selected) {
                if (selected < static_cast<int>(kick::DecoderMode::Software) ||
                    selected > static_cast<int>(kick::DecoderMode::Hybrid))
                    return;

                const auto mode = static_cast<kick::DecoderMode>(selected);
                if (!kick::saveDecoderMode(mode)) {
                    brls::Application::notify(
                        "kickNX: could not save decoder preference");
                    return;
                }

                auto& mpv = MPVCore::instance();
                if (mode == kick::DecoderMode::Software) {
                    mpv.command("set", "hwdec", "no");
                } else {
                    mpv.command("set", "vd-lavc-dr", "no");
                    mpv.command(
                        "set",
                        "hwdec",
                        MPVCore::PLAYER_HWDEC_METHOD.c_str());
                }

                brls::Application::notify(
                    mode == kick::DecoderMode::Software
                        ? "kickNX: switching to software decoder..."
                        : mode == kick::DecoderMode::Hybrid
                            ? "kickNX: switching to Hybrid decoder..."
                            : "kickNX: switching to hardware decoder...");

                if (kickChannel.empty()) {
                    return;
                }

                auto config = kick::loadConfig();
                config.channel = kickChannel;
                kick::resolveLiveAsync(
                    config,
                    [](kick::Resolution result) {
                        MPVCore::BOTTOM_BAR = false;
                        MPVCore::instance().setUrl(
                            result.selected.url,
                            kick::mpvExtra());
                    },
                    [](const std::string& error) {
                        brls::Application::notify(
                            "kickNX decoder reload error: " + error);
                    });
            });

    if (kickChannel.empty()) {
        btnKickChatMode->setVisibility(brls::Visibility::GONE);
        btnKickChatParticipation->setVisibility(brls::Visibility::GONE);
        btnKickChatComposer->setVisibility(brls::Visibility::GONE);
        btnKickChatEmotes->setVisibility(brls::Visibility::GONE);
        btnKickChatOverlaySize->setVisibility(brls::Visibility::GONE);
        btnKickChatOverlayPosition->setVisibility(brls::Visibility::GONE);
        btnKickChatDockedWidth->setVisibility(brls::Visibility::GONE);
        btnKickChatOverlayWidth->setVisibility(brls::Visibility::GONE);
        btnKickChatFont->setVisibility(brls::Visibility::GONE);
        btnKickChatOpacity->setVisibility(brls::Visibility::GONE);
        btnKickChatTimestamps->setVisibility(brls::Visibility::GONE);
    } else {
        const kick::ChatPreferences chat = kick::loadChatPreferences();

        btnKickChatMode->init(
            "Live chat",
            chatModeLabels(),
            static_cast<int>(chat.mode),
            [](int selected) {
                auto preferences = kick::loadChatPreferences();
                preferences.mode = static_cast<kick::ChatMode>(
                    std::clamp(selected, 0, 2));
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify("kickNX: could not save chat mode");
            });

        btnKickChatParticipation->init(
            "Chat participation",
            chatParticipationLabels(),
            static_cast<int>(chat.participation),
            [](int selected) {
                auto preferences = kick::loadChatPreferences();
                preferences.participation =
                    selected == 1
                        ? kick::ChatParticipation::Interactive
                        : kick::ChatParticipation::ReadOnly;
                if (!kick::saveChatPreferences(preferences)) {
                    brls::Application::notify(
                        "kickNX: could not save chat participation");
                    return;
                }

                if (preferences.participation ==
                        kick::ChatParticipation::Interactive &&
                    !kick::hasChatWriteScope()) {
                    brls::Application::notify(
                        "Interactive chat requires signing out and "
                        "signing in again once");
                }
            });

        btnKickChatComposer->init(
            "Message composer",
            chatComposerLabels(),
            static_cast<int>(chat.composerMode),
            [](int selected) {
                auto preferences =
                    kick::loadChatPreferences();
                preferences.composerMode =
                    selected == 0
                        ? kick::ChatComposerMode::NintendoKeyboard
                        : kick::ChatComposerMode::KickNXComposer;

                if (!kick::saveChatPreferences(preferences)) {
                    brls::Application::notify(
                        "kickNX: could not save composer mode");
                }
            });

        btnKickChatEmotes->init(
            "Chat emotes",
            chatEmoteModeLabels(),
            static_cast<int>(chat.emoteMode),
            [](int selected) {
                auto preferences =
                    kick::loadChatPreferences();
                preferences.emoteMode =
                    selected == 1
                        ? kick::ChatEmoteMode::Animated
                        : kick::ChatEmoteMode::Static;

                if (!kick::saveChatPreferences(preferences)) {
                    brls::Application::notify(
                        "kickNX: could not save chat emote mode");
                    return;
                }

                brls::Application::notify(
                    preferences.emoteMode ==
                            kick::ChatEmoteMode::Animated
                        ? "Animated chat emotes enabled"
                        : "Static chat emotes enabled");
            });

        btnKickChatOverlaySize->init(
            "Overlay chat size",
            chatOverlaySizeLabels(),
            static_cast<int>(chat.overlaySize),
            [](int selected) {
                auto preferences = kick::loadChatPreferences();
                preferences.overlaySize =
                    selected == 1
                        ? kick::ChatOverlaySize::Compact
                        : kick::ChatOverlaySize::FullHeight;
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "kickNX: could not save overlay chat size");
            });

        btnKickChatOverlayPosition->init(
            "Overlay chat position",
            chatOverlayPositionLabels(),
            static_cast<int>(chat.overlayPosition),
            [](int selected) {
                auto preferences = kick::loadChatPreferences();
                preferences.overlayPosition =
                    static_cast<kick::ChatOverlayPosition>(
                        std::clamp(selected, 0, 3));
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "kickNX: could not save overlay chat position");
            });

        btnKickChatDockedWidth->init(
            "Right chat panel width",
            chatDockedWidthLabels(),
            closestIndex(
                chatDockedWidthValues(),
                chat.dockedWidth),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >=
                        chatDockedWidthValues().size())
                    return;
                auto preferences = kick::loadChatPreferences();
                preferences.dockedWidth =
                    chatDockedWidthValues().at(selected);
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "kickNX: could not save docked chat width");
            });

        btnKickChatOverlayWidth->init(
            "Overlay chat width",
            chatOverlayWidthLabels(),
            closestIndex(
                chatOverlayWidthValues(),
                chat.overlayWidth),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >=
                        chatOverlayWidthValues().size())
                    return;
                auto preferences = kick::loadChatPreferences();
                preferences.overlayWidth =
                    chatOverlayWidthValues().at(selected);
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify(
                        "kickNX: could not save overlay chat width");
            });

        btnKickChatFont->init(
            "Chat text size",
            chatFontLabels(),
            closestIndex(chatFontValues(), chat.fontSize),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >= chatFontValues().size())
                    return;
                auto preferences = kick::loadChatPreferences();
                preferences.fontSize = chatFontValues().at(selected);
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify("kickNX: could not save chat text size");
            });

        btnKickChatOpacity->init(
            "Chat background",
            chatOpacityLabels(),
            closestIndex(chatOpacityValues(), chat.opacity),
            [](int selected) {
                if (selected < 0 ||
                    static_cast<size_t>(selected) >= chatOpacityValues().size())
                    return;
                auto preferences = kick::loadChatPreferences();
                preferences.opacity = chatOpacityValues().at(selected);
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify("kickNX: could not save chat opacity");
            });

        btnKickChatTimestamps->init(
            "Chat timestamps",
            chat.timestamps,
            [](bool enabled) {
                auto preferences = kick::loadChatPreferences();
                preferences.timestamps = enabled;
                if (!kick::saveChatPreferences(preferences))
                    brls::Application::notify("kickNX: could not save timestamp preference");
            });
    }

    this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    this->cancel->registerClickAction([](...) {
        brls::Application::popActivity();
        return true;
    });
    this->cancel->addGestureRecognizer(new brls::TapGestureRecognizer(this->cancel));

    auto& mpv = MPVCore::instance();

    std::vector<std::string> audioTrack;
    std::vector<std::string> subTrack = {"main/player/none"_i18n};
    std::vector<int64_t> subTrackIds = {0};
    int currentSubIndex = 0;

    int64_t count = mpv.getInt("track-list/count");
    for (int64_t n = 0; n < count; n++) {
        std::string type = mpv.getString(fmt::format("track-list/{}/type", n));
        std::string title = mpv.getString(fmt::format("track-list/{}/title", n));
        if (title.empty()) title = mpv.getString(fmt::format("track-list/{}/lang", n));
        if (title.empty()) title = fmt::format("{} track {}", type, n);
        if (type == "sub") {
            subTrack.push_back(title);
            int64_t trackId = mpv.getInt(fmt::format("track-list/{}/id", n));
            subTrackIds.push_back(trackId);
            if (mpv.getString(fmt::format("track-list/{}/selected", n)) == "yes") {
                currentSubIndex = static_cast<int>(subTrack.size() - 1);
            }
        } else if (type == "audio") {
            audioTrack.push_back(title);
        }
    }

    // Subtitle selection
    const auto& liveCaptions = kick::liveSubtitles();
    if (!liveCaptions.empty()) {
        // Kick live captions are not preloaded (each is its own live feed), so
        // list the advertised languages and load only the chosen one.
        std::vector<std::string> captionNames = {"main/player/none"_i18n};
        int currentCaption = 0;
        for (size_t i = 0; i < liveCaptions.size(); ++i) {
            captionNames.push_back(liveCaptions[i].name);
            if (!kick::liveCaptionLanguage().empty() &&
                liveCaptions[i].language == kick::liveCaptionLanguage())
                currentCaption = static_cast<int>(i + 1);
        }
        this->subtitleTrack->setVisibility(brls::Visibility::VISIBLE);
        this->subtitleTrack->init("main/player/subtitle"_i18n, captionNames, currentCaption,
            [&mpv, currentCaption](int selected) mutable {
                if (selected == currentCaption) return;
                // Drop the caption feed that is loaded now, if any.
                if (currentCaption > 0) mpv.command("sub-remove");
                currentCaption = selected;

                const auto& captions = kick::liveSubtitles();
                if (selected <= 0 || static_cast<size_t>(selected) > captions.size()) {
                    kick::setLiveCaptionLanguage("");
                    mpv.command("set", "sid", "no");
                    return;
                }
                const auto& caption = captions[selected - 1];
                kick::setLiveCaptionLanguage(caption.language);
                mpv.command("sub-add", caption.url.c_str(), "select",
                    caption.name.c_str(), caption.language.c_str());
            });
    } else if (subTrack.size() > 1) {
        this->subtitleTrack->setVisibility(brls::Visibility::VISIBLE);
        this->subtitleTrack->init("main/player/subtitle"_i18n, subTrack, currentSubIndex, [&mpv, subTrackIds](int selected) {
            selectedSubtitle = selected;
            if (selected <= 0 || static_cast<size_t>(selected) >= subTrackIds.size() || subTrackIds[selected] <= 0) {
                mpv.command("set", "sid", "no");
            } else {
                mpv.setInt("sid", subTrackIds[selected]);
            }
        });
    } else {
        std::vector<std::string> defaultSub = {"main/player/none"_i18n};
        this->subtitleTrack->setVisibility(brls::Visibility::VISIBLE);
        this->subtitleTrack->init("main/player/subtitle"_i18n, defaultSub, 0, [&mpv](int selected) {
            mpv.command("set", "sid", "no");
        });
    }
    // Audio track selection
    if (audioTrack.size() > 1) {
        int64_t value = mpv.getInt("aid", 1) - 1;
        this->audioTrack->init("main/player/audio"_i18n, audioTrack, value, [&mpv](int selected) {
            selectedAudio = selected + 1;
            mpv.setInt("aid", selectedAudio);
        });
        this->audioTrack->detail->setVisibility(brls::Visibility::GONE);
    } else {
        this->audioTrack->setVisibility(brls::Visibility::GONE);
    }

    auto& conf = AppConfig::instance();

    /// Subsync
    double subDelay = mpv.getDouble("sub-delay");
    btnSubsync->title->setMarginRight(0);
    btnSubsync->slider->setMarginRight(0);
    btnSubsync->slider->setPointerSize(20);
    btnSubsync->setDetailText(fmt::format("{:.1f}", subDelay));
    btnSubsync->init("main/setting/playback/subsync"_i18n, (subDelay + 10) * 0.05f, [this](float value) {
        float data = value * 20 - 10.f;
        MPVCore::instance().setDouble("sub-delay", data);
        btnSubsync->setDetailText(fmt::format("{:.1f}", data));
    });

    btnEqualizerReset->registerClickAction([this](View* view) {
        btnEqualizerBrightness->slider->setProgress(0.5f);
        btnEqualizerContrast->slider->setProgress(0.5f);
        btnEqualizerSaturation->slider->setProgress(0.5f);
        btnEqualizerGamma->slider->setProgress(0.5f);
        btnEqualizerHue->slider->setProgress(0.5f);
        return true;
    });
    registerHideBackground(btnEqualizerReset);
    setupEqualizer(btnEqualizerBrightness, "main/setting/equalizer/brightness"_i18n, Equalizer::BRIGHTNESS,
        mpv.getDouble("brightness"));
    setupEqualizer(
        btnEqualizerContrast, "main/setting/equalizer/contrast"_i18n, Equalizer::CONTRAST, mpv.getDouble("contrast"));
    setupEqualizer(btnEqualizerSaturation, "main/setting/equalizer/saturation"_i18n, Equalizer::SATURATION,
        mpv.getDouble("saturation"));
    setupEqualizer(btnEqualizerGamma, "main/setting/equalizer/gamma"_i18n, Equalizer::GAMMA, mpv.getDouble("hue"));
    setupEqualizer(btnEqualizerHue, "main/setting/equalizer/hue"_i18n, Equalizer::HUE, mpv.getDouble("gamma"));
}

PlayerSetting::~PlayerSetting() { brls::Logger::debug("PlayerSetting: delete"); }

void PlayerSetting::setupEqualizer(brls::SliderCell* cell, const std::string& title, Equalizer item, double initValue) {
    if (initValue < -100)
        initValue = -100;
    else if (initValue > 100)
        initValue = 100;

    cell->detail->setWidth(50);
    cell->title->setWidth(116);
    cell->title->setMarginRight(0);
    cell->slider->setStep(0.05f);
    cell->slider->setMarginRight(0);
    cell->slider->setPointerSize(20);
    cell->setDetailText(fmt::format("{:.0f}", initValue));
    cell->init(title, (initValue + 100) * 0.005f, [cell, item](float value) {
        auto& mpv = MPVCore::instance();
        int data = (int)(value * 200 - 100);
        cell->setDetailText(std::to_string(data));
        switch (item) {
        case Equalizer::BRIGHTNESS:
            mpv.setInt("brightness", data);
            break;
        case Equalizer::CONTRAST:
            mpv.setInt("contrast", data);
            break;
        case Equalizer::SATURATION:
            mpv.setInt("saturation", data);
            break;
        case Equalizer::GAMMA:
            mpv.setInt("gamma", data);
            break;
        case Equalizer::HUE:
            mpv.setInt("hue", data);
            break;
        default:;
        }
    });
    registerHideBackground(cell->getDefaultFocus());
}

void PlayerSetting::registerHideBackground(brls::View* view) {
    view->getFocusEvent()->subscribe([this](...) { this->setBackgroundColor(nvgRGBAf(0.0f, 0.0f, 0.0f, 0.0f)); });
    view->getFocusLostEvent()->subscribe(
        [this](...) { this->setBackgroundColor(brls::Application::getTheme().getColor("brls/backdrop")); });
}
