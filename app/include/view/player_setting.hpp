//
// Created by fang on 2023/3/4.
//

#pragma once

#include <borealis.hpp>
#include <api/jellyfin/media.hpp>
#include <string>

class ButtonClose;

class PlayerSetting : public brls::Box {
public:
    PlayerSetting(const jellyfin::Source* src = nullptr, std::string kickChannel = "");
    ~PlayerSetting() override;

    bool isTranslucent() override { return true; }

    View* getDefaultFocus() override { return this->settings->getDefaultFocus(); }

    inline static int selectedSubtitle = 0;
    inline static int selectedAudio = 0;

    enum class Equalizer {
        BRIGHTNESS,
        CONTRAST,
        SATURATION,
        HUE,
        GAMMA,
    };

private:
    BRLS_BIND(brls::ScrollingFrame, settings, "player/settings");
    BRLS_BIND(brls::Box, cancel, "player/cancel");

    BRLS_BIND(brls::SelectorCell, subtitleTrack, "setting/track/subtitle");
    BRLS_BIND(brls::SelectorCell, audioTrack, "setting/track/audio");
    BRLS_BIND(brls::SelectorCell, btnKickQuality, "setting/kick/quality");
    BRLS_BIND(brls::SelectorCell, btnKickDecoder, "setting/kick/decoder");
    BRLS_BIND(brls::SelectorCell, btnPortraitOrientation, "setting/portrait/orientation");
    BRLS_BIND(brls::SelectorCell, btnKeyboardHaptic, "setting/portrait/keyboard_haptic");
    BRLS_BIND(brls::SelectorCell, btnKickChatMode, "setting/kick/chat_mode");
    BRLS_BIND(brls::SelectorCell, btnKickChatParticipation, "setting/kick/chat_participation");
    BRLS_BIND(brls::SelectorCell, btnKickChatComposer, "setting/kick/chat_composer");
    BRLS_BIND(brls::SelectorCell, btnKickChatEmotes, "setting/kick/chat_emotes");
    BRLS_BIND(brls::SelectorCell, btnKickChatOverlaySize, "setting/kick/chat_overlay_size");
    BRLS_BIND(brls::SelectorCell, btnKickChatOverlayPosition, "setting/kick/chat_overlay_position");
    BRLS_BIND(brls::SelectorCell, btnKickChatDockedWidth, "setting/kick/chat_docked_width");
    BRLS_BIND(brls::SelectorCell, btnKickChatOverlayWidth, "setting/kick/chat_overlay_width");
    BRLS_BIND(brls::SelectorCell, btnKickChatFont, "setting/kick/chat_font");
    BRLS_BIND(brls::SelectorCell, btnKickChatOpacity, "setting/kick/chat_opacity");
    BRLS_BIND(brls::BooleanCell, btnKickChatTimestamps, "setting/kick/chat_timestamps");
    BRLS_BIND(brls::SliderCell, btnSubsync, "setting/video/subsync");
    // equalizer setting
    BRLS_BIND(brls::RadioCell, btnEqualizerReset, "setting/equalizer/reset");
    BRLS_BIND(brls::SliderCell, btnEqualizerBrightness, "setting/equalizer/brightness");
    BRLS_BIND(brls::SliderCell, btnEqualizerContrast, "setting/equalizer/contrast");
    BRLS_BIND(brls::SliderCell, btnEqualizerSaturation, "setting/equalizer/saturation");
    BRLS_BIND(brls::SliderCell, btnEqualizerGamma, "setting/equalizer/gamma");
    BRLS_BIND(brls::SliderCell, btnEqualizerHue, "setting/equalizer/hue");

    void setupEqualizer(brls::SliderCell* cell, const std::string& title, Equalizer item, double initValue);

    void registerHideBackground(brls::View* view);
};
