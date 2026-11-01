#pragma once

#include "api/kick_chat.hpp"

#include <borealis.hpp>

#include <string>
#include <vector>

/// The message being typed in a chat composer, with known emote names drawn
/// as their images. Shared by the landscape composer and the portrait
/// keyboard, which pass their own text size and placeholder.
class KickDraftPreview : public brls::View {
public:
    struct Style {
        float fontSize = 19.0f;
        std::string placeholder;
        NVGcolor placeholderColor = nvgRGB(166, 174, 191);
        NVGcolor textColor = nvgRGB(245, 246, 250);
    };

    explicit KickDraftPreview(Style style);

    void configure(
        const std::string& value,
        const std::vector<kick::UserEmote>& emotes,
        const std::vector<kick::UserEmote>& recentEmotes);

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override;

private:
    struct Segment {
        std::string text;
        std::string url;
        bool emote = false;
        brls::Image* image = nullptr;
    };

    Style previewStyle;
    std::string draft;
    std::vector<Segment> segments;
};

namespace kicknx::draft {

/// Copy of `value` without leading or trailing whitespace.
std::string trimmed(std::string value);

/// Backspace: removes a whole emote name when the draft ends with one, and
/// otherwise the last UTF-8 character.
void eraseLast(
    std::string& draft,
    const std::vector<kick::UserEmote>& emotes,
    const std::vector<kick::UserEmote>& recentEmotes);

/// Writes emote names in the message in Kick's [emote:id:name] format.
std::string encodeEmotes(
    const std::string& message,
    const std::vector<kick::UserEmote>& emotes,
    const std::vector<kick::UserEmote>& recentEmotes);

}  // namespace kicknx::draft
