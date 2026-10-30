#pragma once

#include <borealis.hpp>

#include <string>
#include <vector>

/// Colour presets for the kickNX browsing screens (Home, channel, details,
/// login/about pages) and the docked chat panel. The video OSD and the chat
/// overlay drawn on top of the video stay dark for readability.
namespace kicknx::theme {

enum class Preset {
    Oled = 0,
    DarkGrey = 1,
    Kick = 2,
};

struct Palette {
    Preset preset;
    std::string id;
    std::string name;

    NVGcolor background;    // page background
    NVGcolor header;        // top bars and full-page panels
    NVGcolor surface;       // cards (streams, categories, clips)
    NVGcolor surfaceAlt;    // empty/info boxes, hero, credit and QR cards
    NVGcolor heroOverlay;   // translucent info panel over the hero preview

    NVGcolor textPrimary;   // titles, channel names, message text
    NVGcolor textSecondary; // body copy and descriptions
    NVGcolor textMuted;     // subtitles, captions, status lines
    NVGcolor textFaint;     // fine print and placeholders

    NVGcolor accent;        // Kick green for text, focus rings and "LIVE"
    NVGcolor button;        // filled action buttons
    NVGcolor buttonText;    // text on filled buttons
    NVGcolor buttonMuted;   // de-emphasised buttons such as "Following"

    NVGcolor errorBackground;
    NVGcolor errorText;
    NVGcolor live;          // live status on the details page
    NVGcolor outline;       // idle rings
    NVGcolor divider;       // hairline separators between chat messages

    NVGcolor focusHighlight1; // Borealis focus highlight
    NVGcolor focusHighlight2;
    NVGcolor focusBackground; // fill behind a focused card, so text stays readable

    /// Keep a filled button's own colour when focused instead of painting the
    /// focus fill over it. Needed on Kick, where the pale focus fill would
    /// wash out the white button text.
    bool keepButtonFillOnFocus = false;

    /// Artwork drawn behind page headers (Home, About, theme picker, events);
    /// empty for a plain header colour. Text on those headers uses the
    /// header* colours below, which differ from page text when the artwork
    /// is dark but the page is bright.
    std::string headerImage;
    NVGcolor headerAccent;
    NVGcolor headerTextPrimary;
    NVGcolor headerTextSecondary;
    NVGcolor headerTextMuted;

    /// Full-screen artwork behind the sign-in page and the tall streamer-page
    /// header, empty for none. Sign-in cards then use the page background
    /// colour so they stand out.
    std::string pageImage;
};

/// All presets, in the order they are offered to the viewer.
const std::vector<Palette>& presets();

/// The active palette. OLED until load() or set() picks another.
const Palette& current();

/// Reads the saved preset from the app config and applies it.
void load();

/// Saves and applies a preset. Screens built afterwards use it.
void set(Preset preset);

/// Puts the palette's header artwork, if any, behind a header box's children.
void decorateHeader(brls::Box* header, const Palette& palette = current());

/// Puts the palette's full-screen artwork, if any, behind a page's children.
void decoratePage(brls::Box* page, const Palette& palette = current());

}  // namespace kicknx::theme
