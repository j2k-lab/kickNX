#include "utils/theme.hpp"
#include "utils/config.hpp"

#include <algorithm>

namespace kicknx::theme {

namespace {

// OLED keeps the original kickNX colours: pure black pages, near-black bars
// and the bright Kick green accent.
Palette makeOled() {
    Palette p;
    p.preset = Preset::Oled;
    p.id = "oled";
    p.name = "OLED (default)";
    p.background = nvgRGB(0, 0, 0);
    p.header = nvgRGB(16, 18, 24);
    p.surface = nvgRGB(32, 35, 45);
    p.surfaceAlt = nvgRGB(25, 28, 38);
    p.heroOverlay = nvgRGBA(18, 20, 28, 245);
    p.textPrimary = nvgRGB(245, 246, 250);
    p.textSecondary = nvgRGB(190, 197, 212);
    p.textMuted = nvgRGB(170, 177, 193);
    p.textFaint = nvgRGB(150, 158, 176);
    p.accent = nvgRGB(83, 252, 24);
    p.button = nvgRGB(24, 115, 36);
    p.buttonText = nvgRGB(250, 250, 252);
    p.buttonMuted = nvgRGB(50, 56, 70);
    p.errorBackground = nvgRGB(75, 35, 42);
    p.errorText = nvgRGB(245, 220, 224);
    p.live = nvgRGB(210, 120, 140);
    p.outline = nvgRGBA(255, 255, 255, 90);
    p.divider = nvgRGBA(255, 255, 255, 18);
    p.focusHighlight1 = nvgRGB(83, 252, 24);
    p.focusHighlight2 = nvgRGB(105, 255, 55);
    p.focusBackground = nvgRGB(31, 34, 39);
    return p;
}

// Lifted charcoal for LCD screens, where pure black looks muddy.
Palette makeDarkGrey() {
    Palette p = makeOled();
    p.preset = Preset::DarkGrey;
    p.id = "dark_grey";
    p.name = "Dark Grey";
    p.background = nvgRGB(30, 32, 38);
    p.header = nvgRGB(40, 43, 51);
    p.surface = nvgRGB(54, 58, 69);
    p.surfaceAlt = nvgRGB(46, 49, 59);
    p.heroOverlay = nvgRGBA(40, 43, 51, 245);
    p.buttonMuted = nvgRGB(72, 78, 94);
    p.errorBackground = nvgRGB(92, 44, 52);
    p.focusBackground = nvgRGB(66, 71, 84);
    return p;
}

// Bright pages for docked play in well-lit rooms. Bright Kick green is hard
// to read on white, so text and focus use a deeper green.
Palette makeLight() {
    Palette p;
    p.preset = Preset::Light;
    p.id = "light";
    p.name = "Light";
    p.background = nvgRGB(229, 232, 237);
    p.header = nvgRGB(206, 238, 196);
    p.surface = nvgRGB(250, 251, 253);
    p.surfaceAlt = nvgRGB(217, 221, 228);
    p.heroOverlay = nvgRGBA(255, 255, 255, 240);
    p.textPrimary = nvgRGB(20, 22, 28);
    p.textSecondary = nvgRGB(62, 68, 82);
    p.textMuted = nvgRGB(88, 95, 110);
    p.textFaint = nvgRGB(118, 125, 140);
    p.accent = nvgRGB(21, 128, 31);
    p.button = nvgRGB(24, 115, 36);
    p.buttonText = nvgRGB(255, 255, 255);
    p.buttonMuted = nvgRGB(96, 104, 120);
    p.errorBackground = nvgRGB(252, 226, 230);
    p.errorText = nvgRGB(140, 24, 42);
    p.live = nvgRGB(184, 38, 72);
    p.outline = nvgRGBA(0, 0, 0, 70);
    p.divider = nvgRGBA(0, 0, 0, 24);
    p.focusHighlight1 = nvgRGB(21, 128, 31);
    p.focusHighlight2 = nvgRGB(40, 165, 52);
    p.focusBackground = nvgRGB(222, 244, 216);
    return p;
}

size_t activeIndex = 0;

void applyBorealisColors(const Palette& p) {
    for (auto* theme : {&brls::Theme::getLightTheme(), &brls::Theme::getDarkTheme()}) {
        theme->addColor("brls/highlight/color1", p.focusHighlight1);
        theme->addColor("brls/highlight/color2", p.focusHighlight2);
        theme->addColor("brls/accent", p.focusHighlight1);
        theme->addColor("brls/highlight/background", p.focusBackground);
        theme->addColor("brls/click_pulse",
            nvgRGBA(static_cast<unsigned char>(p.focusHighlight1.r * 255),
                static_cast<unsigned char>(p.focusHighlight1.g * 255),
                static_cast<unsigned char>(p.focusHighlight1.b * 255), 45));
    }
}

void activate(size_t index) {
    activeIndex = index;
    applyBorealisColors(presets()[index]);
}

}  // namespace

const std::vector<Palette>& presets() {
    static const std::vector<Palette> all = {makeOled(), makeDarkGrey(), makeLight()};
    return all;
}

const Palette& current() { return presets()[activeIndex]; }

void load() {
    const std::string saved = AppConfig::instance().getItem(AppConfig::APP_COLOR_THEME, std::string("oled"));
    const auto& all = presets();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Palette& p) { return p.id == saved; });
    activate(it == all.end() ? 0 : static_cast<size_t>(std::distance(all.begin(), it)));
}

void set(Preset preset) {
    const auto& all = presets();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Palette& p) { return p.preset == preset; });
    if (it == all.end()) return;
    activate(static_cast<size_t>(std::distance(all.begin(), it)));
    AppConfig::instance().setItem(AppConfig::APP_COLOR_THEME, it->id);
}

}  // namespace kicknx::theme
