#pragma once

#include <borealis.hpp>

#include "utils/theme.hpp"

#include <string>

/// Section header laid out like brls::Header (accent bar, title, right-aligned
/// count and an accent underline) but coloured from the active kickNX theme,
/// so it stays readable on the Kick preset.
class ThemeHeader : public brls::Box {
public:
    ThemeHeader() {
        auto style = brls::Application::getStyle();
        const auto& palette = kicknx::theme::current();

        this->setAxis(brls::Axis::ROW);
        this->setPaddingTop(style["brls/header/padding_top_bottom"]);
        this->setPaddingBottom(style["brls/header/padding_top_bottom"]);
        this->setLineBottom(1.0f);
        this->setLineColor(palette.accent);

        auto* bar = new brls::Rectangle(palette.accent);
        bar->setWidth(style["brls/header/rectangle_width"]);
        bar->setHeight(brls::View::AUTO);
        bar->setMarginRight(style["brls/header/rectangle_margin"]);
        this->addView(bar);

        title = new brls::Label();
        title->setFontSize(style["brls/header/font_size"]);
        title->setTextColor(palette.textPrimary);
        this->addView(title);

        auto* spacer = new brls::Box();
        spacer->setGrow(1.0f);
        this->addView(spacer);

        subtitle = new brls::Label();
        subtitle->setFontSize(style["brls/header/font_size"]);
        subtitle->setTextColor(palette.textMuted);
        subtitle->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        subtitle->setVisibility(brls::Visibility::GONE);
        this->addView(subtitle);
    }

    void setTitle(const std::string& text) { title->setText(text); }

    void setSubtitle(const std::string& text) {
        subtitle->setVisibility(brls::Visibility::VISIBLE);
        subtitle->setText(text);
    }

private:
    brls::Label* title = nullptr;
    brls::Label* subtitle = nullptr;
};
