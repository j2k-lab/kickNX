#pragma once

#include <borealis.hpp>

#include "utils/theme.hpp"

#include <string>

/// One line of card caption: muted text with one part in the theme accent
/// (Kick green), such as "Game · [1.2K viewers]" or "0:30 · [3K views]".
/// Empty parts are hidden; when space runs out the muted parts are cut short
/// and the accent part stays whole.
class KickDetailLine : public brls::Box {
public:
    explicit KickDetailLine(float fontSize) {
        const auto& palette = kicknx::theme::current();
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);

        before = makeLabel(fontSize, palette.textMuted);
        accent = makeLabel(fontSize, palette.accent);
        accent->setShrink(0.0f);
        after = makeLabel(fontSize, palette.textMuted);
    }

    void setParts(
        const std::string& beforeText,
        const std::string& accentText = "",
        const std::string& afterText = "") {
        show(before, beforeText, false);
        show(accent, accentText, !beforeText.empty());
        show(after, afterText, !beforeText.empty() || !accentText.empty());
    }

    void clear() { setParts(""); }

private:
    brls::Label* makeLabel(float fontSize, NVGcolor color) {
        auto* label = new brls::Label();
        label->setFontSize(fontSize);
        label->setSingleLine(true);
        label->setTextColor(color);
        label->setVisibility(brls::Visibility::GONE);
        this->addView(label);
        return label;
    }

    static void show(brls::Label* label, const std::string& text, bool spaced) {
        label->setText(text);
        label->setMarginLeft(spaced ? 5.0f : 0.0f);
        label->setVisibility(text.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    brls::Label* before = nullptr;
    brls::Label* accent = nullptr;
    brls::Label* after = nullptr;
};
