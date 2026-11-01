#include "view/kick_draft_preview.hpp"

#include "utils/image.hpp"
#include <borealis/core/cache_helper.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace {

class ComposerPreviewTexturePool {
public:
    static ComposerPreviewTexturePool& instance() {
        // Application-lifetime loader views avoid asynchronous image
        // completions targeting destroyed composer rows/previews.
        static auto* pool =
            new ComposerPreviewTexturePool();
        return *pool;
    }

    brls::Image* get(const std::string& url) {
        if (url.empty()) return nullptr;

        const auto found = entries.find(url);
        if (found != entries.end())
            return found->second;

        if (entries.size() >= MAX_ENTRIES)
            return nullptr;

        auto* image = new brls::Image();
        image->setFreeTexture(false);
        entries.emplace(url, image);
        Image::with(image, url);
        return image;
    }

private:
    static constexpr size_t MAX_ENTRIES = 512;
    std::unordered_map<
        std::string,
        brls::Image*> entries;
};

/// Emote names to emotes; the composer's own list wins over recent ones.
std::unordered_map<std::string, const kick::UserEmote*> emotesByName(
    const std::vector<kick::UserEmote>& emotes,
    const std::vector<kick::UserEmote>& recentEmotes,
    bool requireId) {
    std::unordered_map<std::string, const kick::UserEmote*> byName;
    byName.reserve(emotes.size() + recentEmotes.size());
    for (const auto* list : {&emotes, &recentEmotes}) {
        for (const auto& emote : *list) {
            if (emote.name.empty() || (requireId && emote.id.empty())) continue;
            byName.emplace(emote.name, &emote);
        }
    }
    return byName;
}

bool isSpace(char value) {
    return std::isspace(static_cast<unsigned char>(value));
}

}  // namespace

KickDraftPreview::KickDraftPreview(Style style) : previewStyle(std::move(style)) {}

void KickDraftPreview::configure(
    const std::string& value,
    const std::vector<kick::UserEmote>& emotes,
    const std::vector<kick::UserEmote>& recentEmotes) {
    draft = value;
    segments.clear();

    const auto byName = emotesByName(emotes, recentEmotes, false);

    // Runs of whitespace and runs of other characters become segments; a
    // word that is an emote name is drawn as the emote.
    size_t position = 0;
    while (position < draft.size()) {
        const bool whitespace = isSpace(draft[position]);
        size_t end = position + 1;
        while (end < draft.size() && isSpace(draft[end]) == whitespace)
            ++end;

        Segment segment;
        segment.text = draft.substr(position, end - position);
        if (!whitespace) {
            const auto found = byName.find(segment.text);
            if (found != byName.end()) {
                segment.emote = true;
                segment.url = found->second->imageUrl;
                segment.image =
                    ComposerPreviewTexturePool::instance().get(
                        found->second->imageUrl);
            }
        }
        segments.push_back(std::move(segment));
        position = end;
    }

    this->invalidate();
}

void KickDraftPreview::draw(
    NVGcontext* vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext* ctx) {
    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, width, height);

    const float FONT_SIZE = previewStyle.fontSize;
    constexpr float LINE_HEIGHT = 29.0f;
    constexpr float EMOTE_SIZE = 27.0f;
    constexpr int MAX_LINES = 2;

    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgFontSize(vg, FONT_SIZE);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    if (draft.empty()) {
        nvgFillColor(vg, previewStyle.placeholderColor);
        nvgText(vg, x, y + FONT_SIZE, previewStyle.placeholder.c_str(), nullptr);
        nvgRestore(vg);
        return;
    }

    float cursorX = x;
    float cursorY = y + FONT_SIZE;
    int line = 0;
    const float right = x + width;

    auto nextLine = [&]() {
        cursorX = x;
        cursorY += LINE_HEIGHT;
        ++line;
    };

    auto textWidth = [&](const std::string& text) {
        float bounds[4]{};
        return nvgTextBounds(vg, 0, 0, text.c_str(), nullptr, bounds);
    };

    bool needsTextureRefresh = false;

    for (const auto& segment : segments) {
        if (line >= MAX_LINES) break;

        if (segment.emote) {
            if (cursorX > x && cursorX + EMOTE_SIZE > right)
                nextLine();
            if (line >= MAX_LINES) break;

            int tex = segment.image ? segment.image->getTexture() : 0;
            if (tex <= 0 && !segment.url.empty()) {
                tex = brls::TextureCache::instance().getCache(segment.url);
            }

            if (tex > 0) {
                const float imageY = cursorY - FONT_SIZE - 5.0f;
                NVGpaint paint = nvgImagePattern(
                    vg, cursorX, imageY, EMOTE_SIZE, EMOTE_SIZE, 0, tex, 1.0f);
                nvgBeginPath(vg);
                nvgRect(vg, cursorX, imageY, EMOTE_SIZE, EMOTE_SIZE);
                nvgFillPaint(vg, paint);
                nvgFill(vg);
                cursorX += EMOTE_SIZE + 4.0f;
            } else {
                // The image is still loading: show the name meanwhile.
                needsTextureRefresh = true;
                const float size = textWidth(segment.text);
                if (cursorX > x && cursorX + size > right)
                    nextLine();
                nvgFillColor(vg, previewStyle.textColor);
                nvgText(vg, cursorX, cursorY, segment.text.c_str(), nullptr);
                cursorX += size;
            }
            continue;
        }

        const float size = textWidth(segment.text);
        if (cursorX > x && cursorX + size > right)
            nextLine();
        if (line >= MAX_LINES) break;

        nvgFillColor(vg, previewStyle.textColor);
        nvgText(vg, cursorX, cursorY, segment.text.c_str(), nullptr);
        cursorX += size;
    }

    nvgRestore(vg);

    if (needsTextureRefresh) {
        this->invalidate();
    }
}

namespace kicknx::draft {

std::string trimmed(std::string value) {
    while (!value.empty() && isSpace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && isSpace(value.back()))
        value.pop_back();
    return value;
}

void eraseLast(
    std::string& draft,
    const std::vector<kick::UserEmote>& emotes,
    const std::vector<kick::UserEmote>& recentEmotes) {
    if (draft.empty()) return;

    // Check if draft ends with an emote
    std::string temp = draft;
    while (!temp.empty() && isSpace(temp.back()))
        temp.pop_back();

    if (!temp.empty()) {
        const size_t lastSpace = temp.find_last_of(" \t\n\r");
        const size_t tokenStart = (lastSpace == std::string::npos) ? 0 : lastSpace + 1;
        const std::string lastToken = temp.substr(tokenStart);

        auto isLastToken = [&](const kick::UserEmote& e) {
            return !e.name.empty() && e.name == lastToken;
        };
        if (std::any_of(emotes.begin(), emotes.end(), isLastToken) ||
            std::any_of(recentEmotes.begin(), recentEmotes.end(), isLastToken)) {
            const size_t eraseStart = (tokenStart > 0 && isSpace(draft[tokenStart - 1]))
                                          ? tokenStart - 1
                                          : tokenStart;
            draft.erase(eraseStart);
            return;
        }
    }

    // Normal single-codepoint erase
    size_t position = draft.size() - 1;
    while (position > 0 &&
        (static_cast<unsigned char>(draft[position]) & 0xC0) == 0x80)
        --position;
    draft.erase(position);
}

std::string encodeEmotes(
    const std::string& message,
    const std::vector<kick::UserEmote>& emotes,
    const std::vector<kick::UserEmote>& recentEmotes) {
    const auto byName = emotesByName(emotes, recentEmotes, true);

    std::string encoded;
    size_t pos = 0;
    while (pos < message.size()) {
        const bool ws = isSpace(message[pos]);
        size_t end = pos + 1;
        while (end < message.size() && isSpace(message[end]) == ws)
            ++end;
        const std::string token = message.substr(pos, end - pos);
        const auto found = ws ? byName.end() : byName.find(token);
        if (found != byName.end()) {
            encoded += "[emote:" + found->second->id + ":" + found->second->name + "]";
        } else {
            encoded += token;
        }
        pos = end;
    }
    return encoded;
}

}  // namespace kicknx::draft
