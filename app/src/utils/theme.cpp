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
    p.headerAccent = p.accent;
    p.headerTextPrimary = p.textPrimary;
    p.headerTextSecondary = p.textSecondary;
    p.headerTextMuted = p.textMuted;
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

// Kick green (#8BB06F) pages with light grey cards and near-black text, under the
// dark Kick "K" pattern on headers. The focus ring pulses white/black so it
// shows on the green page, the grey cards and the dark headers alike.
Palette makeKick() {
    Palette p;
    p.preset = Preset::Kick;
    p.id = "kick";
    p.name = "Kick";
    p.background = nvgRGB(139, 176, 111);
    p.header = nvgRGB(120, 156, 94);
    p.surface = nvgRGB(218, 221, 226);
    p.surfaceAlt = nvgRGB(226, 236, 218);
    p.heroOverlay = nvgRGBA(218, 221, 226, 240);
    p.textPrimary = nvgRGB(12, 14, 16);
    p.textSecondary = nvgRGB(34, 40, 38);
    p.textMuted = nvgRGB(38, 62, 42);
    p.textFaint = nvgRGB(58, 86, 62);
    p.accent = nvgRGB(12, 14, 16);
    p.button = nvgRGB(24, 115, 36);
    p.buttonText = nvgRGB(255, 255, 255);
    p.buttonMuted = nvgRGB(70, 78, 90);
    p.errorBackground = nvgRGB(252, 226, 230);
    p.errorText = nvgRGB(140, 24, 42);
    p.live = nvgRGB(184, 38, 72);
    p.outline = nvgRGBA(0, 0, 0, 70);
    p.divider = nvgRGBA(0, 0, 0, 24);
    p.focusHighlight1 = nvgRGB(255, 255, 255);
    p.focusHighlight2 = nvgRGB(12, 14, 16);
    p.focusBackground = nvgRGB(236, 243, 230);
    p.keepButtonFillOnFocus = true;
    p.headerImage = "img/kick_theme_header.png";
    p.headerAccent = nvgRGB(139, 176, 111);
    p.headerTextPrimary = nvgRGB(245, 246, 250);
    p.headerTextSecondary = nvgRGB(200, 206, 214);
    p.headerTextMuted = nvgRGB(170, 177, 193);
    p.pageImage = "img/kick_theme_background.png";
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
    static const std::vector<Palette> all = {makeOled(), makeDarkGrey(), makeKick()};
    return all;
}

const Palette& current() { return presets()[activeIndex]; }

void load() {
    std::string saved = AppConfig::instance().getItem(AppConfig::APP_COLOR_THEME, std::string("oled"));
    // Kick replaced the pre-release Light preset in the same slot.
    if (saved == "light") saved = "kick";
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

namespace {

// Paints artwork over the whole of its parent's frame, scaled to cover it and
// centred, with the parent's rounded corners. A plain brls::Image laid out as
// an absolute child of a padded header was sized and placed from its own
// frame and ended up in the header's top-right corner, so this ignores its
// own layout and draws from the parent's frame instead.
class Backdrop : public brls::View {
public:
    explicit Backdrop(const std::string& image) {
        loader = new brls::Image();
        loader->setImageFromRes(image);
        this->setPositionType(brls::PositionType::ABSOLUTE);
        this->setPositionTop(0);
        this->setPositionLeft(0);
        this->setWidthPercentage(100);
        this->setHeightPercentage(100);
        this->setFocusable(false);
    }

    ~Backdrop() override { delete loader; }

    void draw(NVGcontext* vg, float, float, float, float, brls::Style, brls::FrameContext*) override {
        brls::View* parent = this->getParent();
        const int texture = loader->getTexture();
        const float imageWidth = loader->getOriginalImageWidth();
        const float imageHeight = loader->getOriginalImageHeight();
        if (!parent || texture <= 0 || imageWidth <= 0 || imageHeight <= 0) return;

        const brls::Rect frame = parent->getFrame();
        const float scale = std::max(frame.getWidth() / imageWidth, frame.getHeight() / imageHeight);
        const float drawWidth = imageWidth * scale;
        const float drawHeight = imageHeight * scale;
        const float drawX = frame.getMinX() + (frame.getWidth() - drawWidth) / 2;
        const float drawY = frame.getMinY() + (frame.getHeight() - drawHeight) / 2;

        const NVGpaint paint = nvgImagePattern(vg, drawX, drawY, drawWidth, drawHeight, 0, texture, this->getAlpha());
        nvgBeginPath(vg);
        nvgRoundedRect(vg, frame.getMinX(), frame.getMinY(), frame.getWidth(), frame.getHeight(),
            parent->getCornerRadius());
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

private:
    brls::Image* loader = nullptr;
};

void addBackdrop(brls::Box* box, const std::string& image) {
    if (!box || image.empty()) return;
    box->addView(new Backdrop(image), 0);
}

}  // namespace

void decorateHeader(brls::Box* header, const Palette& palette) {
    addBackdrop(header, palette.headerImage);
}

void decoratePage(brls::Box* page, const Palette& palette) {
    addBackdrop(page, palette.pageImage);
}

}  // namespace kicknx::theme
