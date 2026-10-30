#include "tab/remote_view.hpp"
#include "utils/debug_log.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"
#include "activity/kick_channel.hpp"
#include "api/kick_helix.hpp"
#include "view/player_setting.hpp"
#include "view/kick_chat_composer.hpp"
#include "view/kick_keyboard.hpp"
#include "api/kick_chat.hpp"
#include "api/kick.hpp"
#include "utils/thread.hpp"
#include "utils/misc.hpp"
#include "utils/config.hpp"
#include "utils/image.hpp"
#include "utils/haptics.hpp"
#include "utils/orientation.hpp"
#include "utils/theme.hpp"
#include <borealis/core/cache_helper.hpp>

#include <utility>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>
#include <limits>
#include <stb_image.h>

using namespace brls::literals;

namespace {

NVGcolor chatColor(const std::string& value) {
    if (value.size() != 7 || value.front() != '#')
        return nvgRGB(238, 240, 246);
    try {
        const int red = std::stoi(value.substr(1, 2), nullptr, 16);
        const int green = std::stoi(value.substr(3, 2), nullptr, 16);
        const int blue = std::stoi(value.substr(5, 2), nullptr, 16);
        return nvgRGB(red, green, blue);
    } catch (...) {
        return nvgRGB(238, 240, 246);
    }
}

class KickEmoteTexturePool {
public:
    static KickEmoteTexturePool& instance() {
        static auto* pool = new KickEmoteTexturePool();
        return *pool;
    }

    brls::Image* get(const std::string& url) {
        if (url.empty()) return nullptr;
        auto found = entries.find(url);
        if (found != entries.end()) return found->second;
        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning(
                    "kickNX: emote texture pool reached {} entries; new emotes use text fallback",
                    MAX_ENTRIES);
            }
            return nullptr;
        }
        auto* image = new brls::Image();
        image->setFreeTexture(false);
        entries.emplace(url, image);
        Image::with(image, url);
        return image;
    }

private:
    static constexpr size_t MAX_ENTRIES = 192;
    std::unordered_map<std::string, brls::Image*> entries;
    bool capacityWarningShown = false;
};

class KickAnimatedEmoteTexturePool {
public:
    struct Entry {
        std::vector<int> textures;
        std::vector<int> delaysMs;
        int totalDurationMs = 0;
        bool ready = false;
        bool failed = false;
        std::chrono::steady_clock::time_point startedAt{};
    };

    static KickAnimatedEmoteTexturePool& instance() {
        static auto* pool = new KickAnimatedEmoteTexturePool();
        return *pool;
    }

    Entry* get(const std::string& url) {
        if (url.empty()) return nullptr;

        auto found = entries.find(url);
        if (found != entries.end())
            return found->second.get();

        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning(
                    "kickNX: animated emote pool reached {} entries; "
                    "additional emotes use their static image",
                    MAX_ENTRIES);
            }
            return nullptr;
        }

        auto entry = std::make_shared<Entry>();
        Entry* result = entry.get();
        entries.emplace(url, entry);

        ThreadPool::instance().submit(
            [entry, url](HTTP& http) {
                try {
                    std::ostringstream body;
                    HTTP::set_option(
                        http,
                        HTTP::Timeout{12000});
                    http._get(url, &body);
                    const std::string data = body.str();

                    if (data.empty() ||
                        data.size() > MAX_DOWNLOAD_BYTES ||
                        data.size() > static_cast<size_t>(
                            std::numeric_limits<int>::max()))
                        throw std::runtime_error(
                            "animated emote download is empty or too large");

                    int* sourceDelays = nullptr;
                    int width = 0;
                    int height = 0;
                    int frameCount = 0;
                    int components = 0;
                    stbi_uc* decoded =
                        stbi_load_gif_from_memory(
                            reinterpret_cast<const stbi_uc*>(data.data()),
                            static_cast<int>(data.size()),
                            &sourceDelays,
                            &width,
                            &height,
                            &frameCount,
                            &components,
                            4);

                    if (!decoded || width <= 0 || height <= 0 ||
                        frameCount <= 0) {
                        if (sourceDelays)
                            stbi_image_free(sourceDelays);
                        if (decoded)
                            stbi_image_free(decoded);
                        throw std::runtime_error(
                            "animated emote GIF could not be decoded");
                    }

                    const size_t pixelsPerFrame =
                        static_cast<size_t>(width) *
                        static_cast<size_t>(height) * 4;
                    const size_t decodedBytes =
                        pixelsPerFrame *
                        static_cast<size_t>(frameCount);

                    if (width > MAX_DIMENSION ||
                        height > MAX_DIMENSION ||
                        frameCount > MAX_FRAMES ||
                        decodedBytes > MAX_DECODED_BYTES) {
                        stbi_image_free(decoded);
                        if (sourceDelays)
                            stbi_image_free(sourceDelays);
                        throw std::runtime_error(
                            "animated emote exceeds KickNX safety limits");
                    }

                    auto pixels =
                        std::make_shared<std::vector<uint8_t>>(
                            decoded,
                            decoded + decodedBytes);
                    stbi_image_free(decoded);

                    std::vector<int> delays;
                    delays.reserve(static_cast<size_t>(frameCount));
                    for (int index = 0; index < frameCount; ++index) {
                        const int delay = sourceDelays
                            ? sourceDelays[index]
                            : 100;
                        delays.push_back(
                            std::clamp(delay, 20, 1000));
                    }
                    if (sourceDelays)
                        stbi_image_free(sourceDelays);

                    brls::sync(
                        [entry,
                         pixels,
                         delays = std::move(delays),
                         width,
                         height,
                         frameCount,
                         pixelsPerFrame]() mutable {
                            NVGcontext* vg =
                                brls::Application::getNVGContext();
                            if (!vg) {
                                entry->failed = true;
                                return;
                            }

                            std::vector<int> textures;
                            textures.reserve(
                                static_cast<size_t>(frameCount));
                            for (int index = 0;
                                 index < frameCount;
                                 ++index) {
                                const uint8_t* frame =
                                    pixels->data() +
                                    pixelsPerFrame *
                                        static_cast<size_t>(index);
                                const int texture =
                                    nvgCreateImageRGBA(
                                        vg,
                                        width,
                                        height,
                                        0,
                                        frame);
                                if (texture <= 0) {
                                    for (const int created : textures)
                                        nvgDeleteImage(vg, created);
                                    entry->failed = true;
                                    return;
                                }
                                textures.push_back(texture);
                            }

                            entry->textures = std::move(textures);
                            entry->delaysMs = std::move(delays);
                            entry->totalDurationMs = 0;
                            for (const int delay : entry->delaysMs)
                                entry->totalDurationMs += delay;
                            entry->startedAt =
                                std::chrono::steady_clock::now();
                            entry->ready =
                                !entry->textures.empty() &&
                                entry->totalDurationMs > 0;

                            brls::Logger::verbose(
                                "kickNX: animated emote loaded: {} frames",
                                entry->textures.size());
                        });
                } catch (const std::exception& exception) {
                    brls::Logger::warning(
                        "kickNX: animated emote fallback to static: {}",
                        exception.what());
                    brls::sync([entry]() {
                        entry->failed = true;
                    });
                } catch (...) {
                    brls::sync([entry]() {
                        entry->failed = true;
                    });
                }
            });

        return result;
    }

    int texture(Entry* entry) const {
        if (!entry || !entry->ready ||
            entry->textures.empty() ||
            entry->delaysMs.size() != entry->textures.size() ||
            entry->totalDurationMs <= 0)
            return 0;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() -
                entry->startedAt)
                .count();
        int position = static_cast<int>(
            elapsed % entry->totalDurationMs);

        for (size_t index = 0;
             index < entry->textures.size();
             ++index) {
            if (position < entry->delaysMs[index])
                return entry->textures[index];
            position -= entry->delaysMs[index];
        }
        return entry->textures.back();
    }

private:
    static constexpr size_t MAX_ENTRIES = 48;
    static constexpr size_t MAX_DOWNLOAD_BYTES = 4 * 1024 * 1024;
    static constexpr size_t MAX_DECODED_BYTES = 16 * 1024 * 1024;
    static constexpr int MAX_DIMENSION = 128;
    static constexpr int MAX_FRAMES = 80;

    std::unordered_map<
        std::string,
        std::shared_ptr<Entry>> entries;
    bool capacityWarningShown = false;
};

class KickChatMessageView : public brls::View {
private:
    struct EmoteVisual {
        brls::Image* staticImage = nullptr;
        KickAnimatedEmoteTexturePool::Entry* animation = nullptr;
    };

public:
    /// Docked chat follows the app theme; the overlay on top of the video
    /// keeps its light-on-dark colours.
    void setThemed(bool value) { themed = value; }

    void configure(
        const kick::ChatMessage& value,
        const kick::ChatPreferences& preferences,
        float availableWidth,
        float availableHeight) {
        clearEmotes();

        message = value;
        fontSize = preferences.fontSize;
        showTimestamp = preferences.timestamps;
        contentWidth = std::max(100.0f, availableWidth);
        contentHeightLimit = std::max(
            fontSize * 1.28f + 7.0f,
            availableHeight);

        for (const auto& fragment : message.fragments) {
            if (fragment.type != kick::ChatFragmentType::Emote)
                continue;

            EmoteVisual visual;
            if (!fragment.emoteUrl.empty())
                visual.staticImage =
                    KickEmoteTexturePool::instance().get(
                        fragment.emoteUrl);

            if (preferences.emoteMode ==
                    kick::ChatEmoteMode::Animated &&
                !fragment.animatedEmoteUrl.empty())
                visual.animation =
                    KickAnimatedEmoteTexturePool::instance().get(
                        fragment.animatedEmoteUrl);

            emoteImages.push_back(visual);
        }

        this->setWidth(contentWidth);
        this->setHeight(
            estimateHeightFor(
                message,
                preferences,
                contentWidth,
                contentHeightLimit));
        this->setVisibility(brls::Visibility::VISIBLE);
        this->invalidate();
    }

    void clear() {
        clearEmotes();
        message = {};
        this->setHeight(0);
        this->setVisibility(brls::Visibility::GONE);
    }

    static float estimateHeightFor(
        const kick::ChatMessage& message,
        const kick::ChatPreferences& preferences,
        float availableWidth,
        float availableHeight) {
        const float fontSize = preferences.fontSize;
        const float contentWidth =
            std::max(100.0f, availableWidth);
        const float contentHeightLimit =
            std::max(
                fontSize * 1.28f + 7.0f,
                availableHeight);
        const float lineHeight = fontSize * 1.28f;
        const float emoteSize =
            std::max(18.0f, fontSize * 1.35f);
        const int maxLines =
            maxLineCountFor(fontSize, contentHeightLimit);

        NVGcontext* vg = brls::Application::getNVGContext();
        if (!vg)
            return lineHeight + 7.0f;

        nvgSave(vg);
        nvgFontFaceId(
            vg,
            brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize);
        nvgTextAlign(
            vg,
            NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        auto textWidth = [&](const std::string& value) {
            if (value.empty()) return 0.0f;
            float bounds[4]{};
            return nvgTextBounds(
                vg,
                0,
                0,
                value.c_str(),
                nullptr,
                bounds);
        };

        float cursor = 0.0f;
        int lineCount = 1;

        auto nextLine = [&]() {
            cursor = 0.0f;
            lineCount =
                std::min(lineCount + 1, maxLines);
        };

        auto consumeWord = [&](const std::string& value) {
            if (value.empty() || lineCount >= maxLines + 1)
                return;

            const float wordWidth = textWidth(value);

            if (cursor > 0.0f &&
                cursor + wordWidth > contentWidth)
                nextLine();

            cursor += wordWidth;
        };

        auto consumeWrappedText =
            [&](const std::string& value) {
                std::string token;

                for (size_t index = 0;
                     index < value.size();
                     ++index) {
                    token.push_back(value[index]);

                    const bool boundary =
                        value[index] == ' ' ||
                        value[index] == '\n' ||
                        index + 1 == value.size();

                    if (!boundary) continue;

                    if (!token.empty() &&
                        token.back() == '\n') {
                        token.pop_back();
                        consumeWord(token);
                        token.clear();

                        if (lineCount < maxLines)
                            nextLine();
                    } else {
                        consumeWord(token);
                        token.clear();
                    }

                    if (lineCount >= maxLines)
                        break;
                }
            };

        if (preferences.timestamps &&
            !message.timestamp.empty()) {
            consumeWord(
                "[" + message.timestamp + "] ");
        }

        consumeWord(message.userName + ": ");

        for (const auto& fragment : message.fragments) {
            if (lineCount >= maxLines) break;

            if (fragment.type ==
                kick::ChatFragmentType::Emote) {
                if (cursor > 0.0f &&
                    cursor + emoteSize > contentWidth)
                    nextLine();

                cursor += emoteSize + 3.0f;
            } else {
                consumeWrappedText(fragment.text);
            }
        }

        nvgRestore(vg);

        return std::clamp(
                   lineCount,
                   1,
                   maxLines) *
                   lineHeight +
               7.0f;
    }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override {
        if (message.text.empty()) return;

        // Never allow one message to paint into the next row, even if a
        // malformed/very wide fragment produces an unexpected wrap.
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);

        const float lineHeight = fontSize * 1.28f;
        const float emoteSize =
            std::max(18.0f, fontSize * 1.35f);
        const float right = x + width;
        float cursorX = x;
        float cursorY = y + fontSize;
        int line = 0;
        const int maxLines = maxLineCount();

        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        auto textWidth = [&](const std::string& value) {
            if (value.empty()) return 0.0f;
            float bounds[4]{};
            return nvgTextBounds(
                vg, 0, 0, value.c_str(), nullptr, bounds);
        };

        auto nextLine = [&]() {
            cursorX = x;
            cursorY += lineHeight;
            ++line;
        };

        auto drawWord = [&](const std::string& value, NVGcolor color) {
            if (value.empty() || line >= maxLines) return;
            const float wordWidth = textWidth(value);

            if (cursorX > x && cursorX + wordWidth > right)
                nextLine();
            if (line >= maxLines) return;

            nvgFillColor(vg, color);
            nvgText(vg, cursorX, cursorY, value.c_str(), nullptr);
            cursorX += wordWidth;
        };

        auto drawWrappedText = [&](const std::string& value, NVGcolor color) {
            std::string token;
            for (size_t index = 0; index < value.size(); ++index) {
                token.push_back(value[index]);
                const bool boundary =
                    value[index] == ' ' ||
                    value[index] == '\n' ||
                    index + 1 == value.size();

                if (!boundary) continue;

                if (!token.empty() && token.back() == '\n') {
                    token.pop_back();
                    drawWord(token, color);
                    token.clear();
                    nextLine();
                } else {
                    drawWord(token, color);
                    token.clear();
                }

                if (line >= maxLines) break;
            }
        };

        if (showTimestamp && !message.timestamp.empty()) {
            drawWord(
                "[" + message.timestamp + "] ",
                themed ? kicknx::theme::current().textFaint : nvgRGB(145, 154, 174));
        }

        const NVGcolor messageColor =
            themed ? kicknx::theme::current().textPrimary : nvgRGB(245, 246, 250);

        drawWord(
            message.userName + ": ",
            chatColor(message.color));

        size_t emoteIndex = 0;
        for (const auto& fragment : message.fragments) {
            if (line >= maxLines) break;

            if (fragment.type == kick::ChatFragmentType::Emote) {
                if (cursorX > x && cursorX + emoteSize > right)
                    nextLine();
                if (line >= maxLines) break;

                const EmoteVisual* visual =
                    emoteIndex < emoteImages.size()
                        ? &emoteImages[emoteIndex]
                        : nullptr;
                ++emoteIndex;

                int texture = 0;
                if (visual && visual->animation)
                    texture =
                        KickAnimatedEmoteTexturePool::instance().texture(
                            visual->animation);
                if (texture <= 0 && visual &&
                    visual->staticImage)
                    texture = visual->staticImage->getTexture();

                if (texture > 0) {
                    const float imageY =
                        cursorY - fontSize - (emoteSize - fontSize) * 0.35f;
                    NVGpaint paint = nvgImagePattern(
                        vg,
                        cursorX,
                        imageY,
                        emoteSize,
                        emoteSize,
                        0,
                        texture,
                        1.0f);
                    nvgBeginPath(vg);
                    nvgRect(vg, cursorX, imageY, emoteSize, emoteSize);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);
                    cursorX += emoteSize + 3.0f;
                } else {
                    drawWord(
                        fragment.text + " ",
                        messageColor);
                }
            } else {
                drawWrappedText(
                    fragment.text,
                    messageColor);
            }
        }

        nvgBeginPath(vg);
        nvgMoveTo(vg, x, y + height - 1);
        nvgLineTo(vg, right, y + height - 1);
        nvgStrokeColor(vg, themed ? kicknx::theme::current().divider : nvgRGBA(255, 255, 255, 18));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);

        nvgRestore(vg);
    }

private:
    static int maxLineCountFor(
        float fontSize,
        float contentHeightLimit) {
        const float lineHeight = fontSize * 1.28f;
        return std::max(
            1,
            static_cast<int>(
                std::floor(
                    std::max(0.0f, contentHeightLimit - 7.0f) /
                    std::max(1.0f, lineHeight))));
    }

    int maxLineCount() const {
        return maxLineCountFor(fontSize, contentHeightLimit);
    }

    float estimateHeight() const {
        kick::ChatPreferences preferences;
        preferences.fontSize = fontSize;
        preferences.timestamps = showTimestamp;
        return estimateHeightFor(
            message,
            preferences,
            contentWidth,
            contentHeightLimit);
    }

    void clearEmotes() {
        // Shared loader views remain alive in the bounded app-lifetime pool.
        emoteImages.clear();
    }

    kick::ChatMessage message;
    std::vector<EmoteVisual> emoteImages;
    float fontSize = 16.0f;
    float contentWidth = 300.0f;
    float contentHeightLimit = brls::Application::contentHeight;
    bool showTimestamp = false;
    bool themed = false;
};


class PortraitDraftPreview : public brls::View {
public:
    void configure(
        const std::string& value,
        const std::vector<kick::UserEmote>& emotes) {
        draft = value;
        segments.clear();

        std::unordered_map<std::string, const kick::UserEmote*> byName;
        byName.reserve(emotes.size());
        for (const auto& emote : emotes) {
            if (!emote.name.empty()) byName.emplace(emote.name, &emote);
        }

        size_t position = 0;
        while (position < draft.size()) {
            const bool whitespace = std::isspace(
                static_cast<unsigned char>(draft[position]));
            size_t end = position + 1;
            while (end < draft.size() &&
                static_cast<bool>(std::isspace(
                    static_cast<unsigned char>(draft[end]))) == whitespace)
                ++end;

            Segment segment;
            segment.text = draft.substr(position, end - position);
            if (!whitespace) {
                const auto found = byName.find(segment.text);
                if (found != byName.end()) {
                    segment.emote = true;
                    segment.url = found->second->imageUrl;
                    segment.image = KickEmoteTexturePool::instance().get(
                        found->second->imageUrl);
                }
            }
            segments.push_back(std::move(segment));
            position = end;
        }
        this->invalidate();
    }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style,
        brls::FrameContext*) override {
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);

        constexpr float FONT_SIZE = 20.0f;
        constexpr float LINE_HEIGHT = 29.0f;
        constexpr float EMOTE_SIZE = 27.0f;
        constexpr int MAX_LINES = 2;

        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, FONT_SIZE);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        if (draft.empty()) {
            nvgFillColor(vg, nvgRGB(150, 158, 176));
            nvgText(vg, x, y + FONT_SIZE, "Type a message...", nullptr);
            nvgRestore(vg);
            return;
        }

        auto textWidth = [&](const std::string& text) {
            float bounds[4]{};
            return nvgTextBounds(
                vg, 0, 0, text.c_str(), nullptr, bounds);
        };

        float cursorX = x;
        float cursorY = y + FONT_SIZE;
        int line = 0;
        const float right = x + width;
        auto nextLine = [&]() {
            cursorX = x;
            cursorY += LINE_HEIGHT;
            ++line;
        };

        bool needsTextureRefresh = false;

        for (const auto& segment : segments) {
            if (line >= MAX_LINES) break;
            const float segmentWidth = segment.emote
                ? EMOTE_SIZE + 4.0f
                : textWidth(segment.text);
            if (cursorX > x && cursorX + segmentWidth > right) nextLine();
            if (line >= MAX_LINES) break;

            if (segment.emote) {
                int tex = segment.image ? segment.image->getTexture() : 0;
                if (tex <= 0 && !segment.url.empty()) {
                    tex = brls::TextureCache::instance().getCache(segment.url);
                }

                if (tex > 0) {
                    const float imageY = cursorY - FONT_SIZE - 5.0f;
                    const NVGpaint paint = nvgImagePattern(
                        vg,
                        cursorX,
                        imageY,
                        EMOTE_SIZE,
                        EMOTE_SIZE,
                        0,
                        tex,
                        1.0f);
                    nvgBeginPath(vg);
                    nvgRect(vg, cursorX, imageY, EMOTE_SIZE, EMOTE_SIZE);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);
                    cursorX += EMOTE_SIZE + 4.0f;
                } else {
                    needsTextureRefresh = true;
                    nvgFillColor(vg, nvgRGB(244, 246, 250));
                    nvgText(
                        vg,
                        cursorX,
                        cursorY,
                        segment.text.c_str(),
                        nullptr);
                    cursorX += textWidth(segment.text);
                }
            } else {
                nvgFillColor(vg, nvgRGB(244, 246, 250));
                nvgText(
                    vg,
                    cursorX,
                    cursorY,
                    segment.text.c_str(),
                    nullptr);
                cursorX += textWidth(segment.text);
            }
        }
        nvgRestore(vg);

        if (needsTextureRefresh) {
            this->invalidate();
        }
    }

private:
    struct Segment {
        std::string text;
        std::string url;
        bool emote = false;
        brls::Image* image = nullptr;
    };

    std::string draft;
    std::vector<Segment> segments;
};

class PortraitEmoteCell : public RecyclingGridItem {
public:
    PortraitEmoteCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setPadding(5, 5, 5, 5);
        this->setCornerRadius(6.0f);
        this->setBackgroundColor(nvgRGB(31, 35, 46));

        picture = new brls::Image();
        picture->setWidth(44.0f);
        picture->setHeight(44.0f);
        picture->setImageFromRes("img/video-card-bg.png");
        this->addView(picture);

        name = new brls::Label();
        name->setFontSize(11.0f);
        name->setSingleLine(true);
        name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        name->setTextColor(nvgRGB(229, 231, 238));
        name->setMarginTop(3.0f);
        name->setWidthPercentage(100.0f);
        this->addView(name);
    }

    ~PortraitEmoteCell() override { Image::cancel(picture); }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setImageFromRes("img/video-card-bg.png");
        name->setText("");
    }

    void cacheForReuse() override { Image::cancel(picture); }

    void configure(const kick::UserEmote& emote) {
        name->setText(emote.name);
        if (!emote.imageUrl.empty()) Image::with(picture, emote.imageUrl);
    }

private:
    brls::Image* picture = nullptr;
    brls::Label* name = nullptr;
};

class PortraitEmoteDataSource : public RecyclingGridDataSource {
public:
    PortraitEmoteDataSource(
        std::vector<kick::UserEmote> emotes,
        std::function<void(const kick::UserEmote&)> selected)
        : emotes(std::move(emotes)), selected(std::move(selected)) {}

    size_t getItemCount() override { return emotes.size(); }

    RecyclingGridItem* cellForRow(
        RecyclingView* recycler,
        size_t index) override {
        auto* cell = dynamic_cast<PortraitEmoteCell*>(
            recycler->dequeueReusableCell("PortraitKickEmote"));
        cell->configure(emotes.at(index));
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (selected) selected(emotes.at(index));
    }

    void clearData() override { emotes.clear(); }

private:
    std::vector<kick::UserEmote> emotes;
    std::function<void(const kick::UserEmote&)> selected;
};

class PortraitChatComposer : public brls::Box {
public:
    using SendCallback = std::function<bool(const std::string&)>;

    PortraitChatComposer(std::string channel, SendCallback sendRequested)
        : channel(std::move(channel)),
          sendRequested(std::move(sendRequested)),
          emotes(kick::loadRecentEmotes()),
          recentEmotes(kick::loadRecentEmotes()) {
        this->setAxis(brls::Axis::COLUMN);
        this->setWidthPercentage(100.0f);
        this->setHeight(485.0f);
        this->setPadding(10, 12, 10, 12);
        this->setBackgroundColor(nvgRGB(10, 12, 18));
        this->setVisibility(brls::Visibility::GONE);

        auto* draftBox = new brls::Box();
        draftBox->setAxis(brls::Axis::COLUMN);
        draftBox->setWidthPercentage(100.0f);
        draftBox->setHeight(88.0f);
        draftBox->setPadding(9, 14, 8, 14);
        draftBox->setMarginBottom(8.0f);
        draftBox->setCornerRadius(6.0f);
        draftBox->setBackgroundColor(nvgRGB(28, 33, 43));
        draftBox->registerClickAction([this](brls::View*) {
            setEmotePanelVisible(!emotePanelVisible);
            return true;
        });
        draftBox->addGestureRecognizer(new brls::TapGestureRecognizer(draftBox));
        this->addView(draftBox);

        auto* draftHeader = new brls::Box();
        draftHeader->setAxis(brls::Axis::ROW);
        draftHeader->setWidthPercentage(100.0f);
        draftHeader->setHeight(22.0f);
        draftBox->addView(draftHeader);

        auto* caption = new brls::Label();
        caption->setText("Message");
        caption->setFontSize(14.0f);
        caption->setTextColor(nvgRGB(168, 178, 197));
        caption->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        caption->setGrow(1.0f);
        draftHeader->addView(caption);

        counter = new brls::Label();
        counter->setText("0 / 500");
        counter->setFontSize(13.0f);
        counter->setTextColor(nvgRGB(142, 151, 170));
        counter->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        draftHeader->addView(counter);

        draftPreview = new PortraitDraftPreview();
        draftPreview->setGrow(1.0f);
        draftPreview->setWidthPercentage(100.0f);
        draftBox->addView(draftPreview);

        keyboardContainer = new KickKeyboard(
            {
                [this](const std::string& value) { append(value); },
                [this]() {
                    eraseLastCodepoint();
                    refreshDraft();
                },
                [this]() { setEmotePanelVisible(true); },
                [this]() { send(); },
            },
            KickKeyboard::Options{});
        keyboardContainer->setGrow(1.0f);
        this->addView(keyboardContainer);

        buildEmotePanel();
        refreshDraft();
    }

    void setFrameSize(float width, float height) {
        this->setWidth(width);
        this->setHeight(height);
    }

    void setEmotePanelVisible(bool visible) {
        if (emotePanelVisible == visible) return;
        emotePanelVisible = visible;
        keyboardContainer->setVisibility(
            visible ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        emotePanel->setVisibility(
            visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        if (visible) {
            if (!emoteLoadStarted) loadEmotes();
            emoteGrid->invalidate();
            brls::sync([this]() {
                if (emotePanelVisible && !emotes.empty())
                    brls::Application::giveFocus(emoteGrid);
            });
        }
        this->invalidate();
    }

    bool isEmotePanelVisible() const { return emotePanelVisible; }

private:
    void append(const std::string& value) {
        if (draft.size() + value.size() > MAX_MESSAGE_BYTES) return;
        draft += value;
        refreshDraft();
    }

    void insertEmote(const kick::UserEmote& emote) {
        if (emote.name.empty()) return;
        std::string addition;
        if (!draft.empty() &&
            !std::isspace(static_cast<unsigned char>(draft.back())))
            addition += ' ';
        addition += emote.name;
        addition += ' ';
        if (draft.size() + addition.size() > MAX_MESSAGE_BYTES) {
            brls::Application::notify(
                "The Kick message limit is 500 characters");
            return;
        }
        draft += addition;
        kick::rememberRecentEmote(emote);
        recentEmotes = kick::loadRecentEmotes();
        refreshDraft();
    }

    void buildEmotePanel() {
        emotePanel = new brls::Box();
        emotePanel->setAxis(brls::Axis::COLUMN);
        emotePanel->setWidthPercentage(100.0f);
        emotePanel->setGrow(1.0f);
        emotePanel->setVisibility(brls::Visibility::GONE);
        this->addView(emotePanel);

        auto* headerBar = new brls::Box();
        headerBar->setAxis(brls::Axis::ROW);
        headerBar->setWidthPercentage(100.0f);
        headerBar->setHeight(38.0f);
        headerBar->setAlignItems(brls::AlignItems::CENTER);
        headerBar->setMarginBottom(8.0f);
        emotePanel->addView(headerBar);

        auto* backToKeyboard = new brls::Button();
        backToKeyboard->setText("< Keyboard");
        backToKeyboard->setFontSize(14.0f);
        backToKeyboard->setHeight(36.0f);
        backToKeyboard->setPadding(4, 12, 4, 12);
        backToKeyboard->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        backToKeyboard->registerClickAction([this](brls::View*) {
            setEmotePanelVisible(false);
            return true;
        });
        backToKeyboard->addGestureRecognizer(new brls::TapGestureRecognizer(backToKeyboard));
        headerBar->addView(backToKeyboard);

        btnRecentTab = new brls::Button();
        btnRecentTab->setText("Recent");
        btnRecentTab->setFontSize(14.0f);
        btnRecentTab->setHeight(36.0f);
        btnRecentTab->setPadding(4, 12, 4, 12);
        btnRecentTab->setMarginLeft(8.0f);
        btnRecentTab->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        headerBar->addView(btnRecentTab);

        btnAllTab = new brls::Button();
        btnAllTab->setText("All");
        btnAllTab->setFontSize(14.0f);
        btnAllTab->setHeight(36.0f);
        btnAllTab->setPadding(4, 12, 4, 12);
        btnAllTab->setMarginLeft(4.0f);
        btnAllTab->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        headerBar->addView(btnAllTab);

        btnRecentTab->registerClickAction([this](brls::View*) {
            emoteTab = EmoteTab::Recent;
            btnRecentTab->setStyle(&brls::BUTTONSTYLE_PRIMARY);
            btnAllTab->setStyle(&brls::BUTTONSTYLE_DEFAULT);
            refreshEmoteGrid();
            return true;
        });
        btnAllTab->registerClickAction([this](brls::View*) {
            emoteTab = EmoteTab::All;
            btnAllTab->setStyle(&brls::BUTTONSTYLE_PRIMARY);
            btnRecentTab->setStyle(&brls::BUTTONSTYLE_DEFAULT);
            refreshEmoteGrid();
            return true;
        });
        btnRecentTab->addGestureRecognizer(new brls::TapGestureRecognizer(btnRecentTab));
        btnAllTab->addGestureRecognizer(new brls::TapGestureRecognizer(btnAllTab));

        emoteStatus = new brls::Label();
        emoteStatus->setText("Loading emotes...");
        emoteStatus->setFontSize(13.0f);
        emoteStatus->setTextColor(nvgRGB(168, 178, 197));
        emoteStatus->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        emoteStatus->setGrow(1.0f);
        headerBar->addView(emoteStatus);

        emoteGrid = new RecyclingGrid();
        emoteGrid->setGrow(1.0f);
        emoteGrid->spanCount = 6;
        emoteGrid->estimatedRowHeight = 78.0f;
        emoteGrid->estimatedRowSpace = 6.0f;
        emoteGrid->preFetchLine = 2;
        emoteGrid->setPadding(2, 2, 2, 2);
        emoteGrid->registerCell(
            "PortraitKickEmote",
            []() { return new PortraitEmoteCell(); });
        emoteGrid->setIndexNavigationEnabled(true);
        emotePanel->addView(emoteGrid);
        refreshEmoteGrid();
    }

    void refreshEmoteGrid() {
        const auto displayList = (emoteTab == EmoteTab::Recent && !recentEmotes.empty())
            ? recentEmotes
            : emotes;

        if (emoteStatus) {
            emoteStatus->setText(
                loading
                    ? "Loading emotes..."
                    : displayList.empty()
                        ? (emoteTab == EmoteTab::Recent ? "No recent emotes" : "No emotes available")
                        : fmt::format("{} emotes", displayList.size()));
        }
        emoteGrid->setVisibility(brls::Visibility::GONE);
        emoteGrid->setDataSource(new PortraitEmoteDataSource(
            displayList,
            [this](const kick::UserEmote& emote) { insertEmote(emote); }));
        emoteGrid->setVisibility(brls::Visibility::VISIBLE);
        emoteGrid->invalidate();
    }

    void loadEmotes() {
        emoteLoadStarted = true;
        if (!kick::hasUserEmotesScope()) {
            loading = false;
            refreshEmoteGrid();
            return;
        }

        this->ptrLock();
        kick::loadUserEmotesAsync(
            channel,
            [this](kick::UserEmoteCatalogue catalogue) {
                emotes = std::move(catalogue.emotes);
                loading = false;
                refreshEmoteGrid();
                refreshDraft();
                this->ptrUnlock();
            },
            [this](const std::string& error) {
                loading = false;
                refreshEmoteGrid();
                emoteStatus->setText(error);
                this->ptrUnlock();
            });
    }

    void eraseLastCodepoint() {
        if (draft.empty()) return;

        // Check if draft ends with an emote
        std::string temp = draft;
        while (!temp.empty() && std::isspace(static_cast<unsigned char>(temp.back())))
            temp.pop_back();

        if (!temp.empty()) {
            size_t lastSpace = temp.find_last_of(" \t\n\r");
            size_t tokenStart = (lastSpace == std::string::npos) ? 0 : lastSpace + 1;
            std::string lastToken = temp.substr(tokenStart);

            bool isEmote = false;
            auto checkEmote = [&](const kick::UserEmote& e) {
                return !e.name.empty() && e.name == lastToken;
            };
            if (std::any_of(emotes.begin(), emotes.end(), checkEmote) ||
                std::any_of(recentEmotes.begin(), recentEmotes.end(), checkEmote)) {
                isEmote = true;
            }

            if (isEmote) {
                size_t eraseStart = (tokenStart > 0 && std::isspace(static_cast<unsigned char>(draft[tokenStart - 1])))
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

    void send() {
        if (draft.empty() || !sendRequested) return;
        std::unordered_map<std::string, const kick::UserEmote*> byName;
        for (const auto& emote : emotes) {
            if (!emote.name.empty() && !emote.id.empty())
                byName.emplace(emote.name, &emote);
        }
        std::string wireMessage;
        size_t pos = 0;
        while (pos < draft.size()) {
            const bool ws = std::isspace(static_cast<unsigned char>(draft[pos]));
            size_t end = pos + 1;
            while (end < draft.size() &&
                   static_cast<bool>(std::isspace(static_cast<unsigned char>(draft[end]))) == ws)
                ++end;
            const std::string token = draft.substr(pos, end - pos);
            if (!ws) {
                const auto found = byName.find(token);
                if (found != byName.end()) {
                    wireMessage += "[emote:" + found->second->id + ":" + found->second->name + "]";
                } else {
                    wireMessage += token;
                }
            } else {
                wireMessage += token;
            }
            pos = end;
        }
        if (sendRequested(wireMessage)) {
            draft.clear();
            keyboardContainer->resetShift();
            refreshDraft();
        }
    }

    void refreshDraft() {
        std::vector<kick::UserEmote> allKnown = emotes;
        for (const auto& r : recentEmotes) {
            if (!r.name.empty() &&
                std::none_of(allKnown.begin(), allKnown.end(), [&](const kick::UserEmote& e) {
                    return e.name == r.name;
                })) {
                allKnown.push_back(r);
            }
        }
        draftPreview->configure(draft, allKnown);
        counter->setText(
            fmt::format("{} / {}", draft.size(), MAX_MESSAGE_BYTES));
        this->invalidate();
    }

    static constexpr size_t MAX_MESSAGE_BYTES = 500;
    std::string channel;
    SendCallback sendRequested;
    std::string draft;
    PortraitDraftPreview* draftPreview = nullptr;
    brls::Label* counter = nullptr;
    KickKeyboard* keyboardContainer = nullptr;
    brls::Box* emotePanel = nullptr;
    brls::Label* emoteStatus = nullptr;
    RecyclingGrid* emoteGrid = nullptr;
    enum class EmoteTab { Recent, All };
    EmoteTab emoteTab = EmoteTab::Recent;
    brls::Button* btnRecentTab = nullptr;
    brls::Button* btnAllTab = nullptr;
    std::vector<kick::UserEmote> recentEmotes;
    std::vector<kick::UserEmote> emotes;
    bool loading = true;
    bool emoteLoadStarted = false;
    bool emotePanelVisible = false;
};

class KickChatPanel : public brls::Box {
public:
    KickChatPanel(
        bool docked,
        std::function<void()> sendRequested,
        std::function<bool()> backRequested)
        : docked(docked),
          sendRequested(std::move(sendRequested)),
          backRequested(std::move(backRequested)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setWidth(docked ? 330.0f : 360.0f);
        panelHeight = brls::Application::contentHeight;
        this->setHeight(panelHeight);
        if (!docked)
            this->setPositionType(brls::PositionType::ABSOLUTE);
        this->setPadding(18, 12, 12, 12);
        this->setVisibility(brls::Visibility::GONE);
        this->setFocusable(false);

        headerBox = new brls::Box();
        headerBox->setAxis(brls::Axis::ROW);
        headerBox->setAlignItems(brls::AlignItems::CENTER);
        headerBox->setMarginBottom(7);

        // Docked chat sits beside the video and follows the app theme; the
        // overlay is drawn over the video and keeps its light-on-dark look.
        const auto& palette = kicknx::theme::current();
        const NVGcolor primaryText = docked ? palette.textPrimary : nvgRGB(245, 246, 250);
        const NVGcolor mutedText = docked ? palette.textMuted : nvgRGB(173, 181, 199);
        const NVGcolor accent = docked ? palette.accent : nvgRGB(83, 252, 24);

        titlePrefix = new brls::Label();
        titlePrefix->setText("LIVE CHAT");
        titlePrefix->setFontSize(16);
        titlePrefix->setTextColor(primaryText);

        chattersCountLabel = new brls::Label();
        chattersCountLabel->setFontSize(16);
        chattersCountLabel->setTextColor(accent);
        chattersCountLabel->setVisibility(brls::Visibility::GONE);

        chattersSuffix = new brls::Label();
        chattersSuffix->setText(" active chatters");
        chattersSuffix->setFontSize(14);
        chattersSuffix->setTextColor(mutedText);
        chattersSuffix->setVisibility(brls::Visibility::GONE);

        headerBox->addView(titlePrefix);
        headerBox->addView(chattersCountLabel);
        headerBox->addView(chattersSuffix);
        this->addView(headerBox);

        status = new brls::Label();
        status->setText("");
        status->setFontSize(13);
        status->setTextColor(mutedText);
        status->setMarginBottom(7);
        status->setVisibility(brls::Visibility::GONE);
        this->addView(status);

        messagesBox = new brls::Box();
        messagesBox->setAxis(brls::Axis::COLUMN);
        messagesBox->setGrow(1.0f);
        this->addView(messagesBox);

        for (size_t index = 0; index < ROW_POOL; ++index) {
            auto* row = new KickChatMessageView();
            row->setThemed(docked);
            row->setVisibility(brls::Visibility::GONE);
            messagesBox->addView(row);
            rows.push_back(row);
        }

        composeButton = new brls::Button();
        composeButton->setText("Send message");
        composeButton->setFontSize(15);
        composeButton->setHeight(46);
        composeButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
        composeButton->setBorderThickness(2.0f);
        composeButton->setBorderColor(accent);
        composeButton->setBackgroundColor(docked ? palette.button : nvgRGB(24, 115, 36));
        composeButton->setCornerRadius(8);
        composeButton->setTextColor(docked ? palette.buttonText : nvgRGB(255, 255, 255));
        composeButton->setHideHighlightBackground(docked && palette.keepButtonFillOnFocus);
        composeButton->setMarginTop(8);
        composeButton->setVisibility(brls::Visibility::GONE);
        composeButton->registerClickAction(
            [this](brls::View*) {
                if (this->sendRequested) this->sendRequested();
                return true;
            });

        // The chat panel is outside VideoView's focus tree. If this button
        // receives focus after closing a composer/IME, route B to the same
        // playback back action instead of swallowing it.
        composeButton->registerAction(
            "Back",
            brls::BUTTON_B,
            [this](brls::View*) {
                return this->backRequested
                    ? this->backRequested()
                    : false;
            });

        this->addView(composeButton);
    }

    void apply(const kick::ChatPreferences& value) {
        preferences = value;
        panelWidth = static_cast<float>(
            docked ? value.dockedWidth : value.overlayWidth);
        this->setWidth(panelWidth);

        if (docked) {
            panelHeight = brls::Application::contentHeight;
            this->setHeight(panelHeight);
            this->setBackgroundColor(kicknx::theme::current().background);
        } else {
            const bool compact =
                value.overlaySize == kick::ChatOverlaySize::Compact;
            panelHeight = compact
                ? std::max(
                      280.0f,
                      brls::Application::contentHeight * 0.5f)
                : brls::Application::contentHeight;
            this->setHeight(panelHeight);

            const float inset = compact ? 12.0f : 0.0f;
            const bool onLeft =
                value.overlayPosition ==
                    kick::ChatOverlayPosition::TopLeft ||
                value.overlayPosition ==
                    kick::ChatOverlayPosition::BottomLeft;
            const bool onBottom =
                compact &&
                (value.overlayPosition ==
                     kick::ChatOverlayPosition::BottomRight ||
                 value.overlayPosition ==
                     kick::ChatOverlayPosition::BottomLeft);

            this->setPositionTop(
                onBottom ? brls::View::AUTO : inset);
            this->setPositionBottom(
                onBottom ? inset : brls::View::AUTO);
            this->setPositionLeft(
                onLeft ? inset : brls::View::AUTO);
            this->setPositionRight(
                onLeft ? brls::View::AUTO : inset);

            const int alpha =
                std::clamp(value.opacity * 255 / 100, 0, 255);
            this->setBackgroundColor(nvgRGBA(12, 14, 20, alpha));
        }

        composeButton->setVisibility(
            value.participation ==
                    kick::ChatParticipation::Interactive
                ? brls::Visibility::VISIBLE
                : brls::Visibility::GONE);

        refresh();
    }

    void setStatus(const std::string& value) {
        if (value.empty() ||
            value == "Live chat" ||
            value == "Chat disabled" ||
            value == "Connected") {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
            return;
        }

        status->setText(value);
        status->setVisibility(brls::Visibility::VISIBLE);
    }

    void setChatterCount(int count) {
        currentChatterCount = count;
        if (count > 0) {
            titlePrefix->setText("LIVE CHAT · ");
            std::string countStr;
            if (count >= 1000000) countStr = fmt::format("{:.1f}M", count / 1000000.0);
            else if (count >= 100000) countStr = fmt::format("{:.0f}K", count / 1000.0);
            else if (count >= 1000) countStr = fmt::format("{},{:03d}", count / 1000, count % 1000);
            else countStr = std::to_string(count);

            chattersCountLabel->setText(countStr);
            chattersCountLabel->setVisibility(brls::Visibility::VISIBLE);
            chattersSuffix->setText(count == 1 ? " active chatter" : " active chatters");
            chattersSuffix->setVisibility(brls::Visibility::VISIBLE);
        } else {
            titlePrefix->setText("LIVE CHAT");
            chattersCountLabel->setVisibility(brls::Visibility::GONE);
            chattersSuffix->setVisibility(brls::Visibility::GONE);
        }
    }

    void append(kick::ChatMessage message) {
        if (status->getVisibility() == brls::Visibility::VISIBLE) {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
        }
        messages.push_back(std::move(message));
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
        refresh();
    }

    void replaceMessages(
        const std::deque<kick::ChatMessage>& value) {
        if (!value.empty() && status->getVisibility() == brls::Visibility::VISIBLE) {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
        }
        messages = value;
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
        refresh();
    }

    const std::deque<kick::ChatMessage>& visibleMessages() const {
        return messages;
    }

    float width() const { return panelWidth; }

    void setPortraitFrame(float width, float height) {
        panelWidth = width;
        panelHeight = height;
        this->setWidth(width);
        this->setHeight(height);
        this->setBackgroundColor(kicknx::theme::current().background);
        composeButton->setVisibility(brls::Visibility::GONE);
        refresh();
    }

private:
    void refresh() {
        for (auto* row : rows) row->clear();
        if (messages.empty()) return;

        const float innerWidth =
            std::max(120.0f, panelWidth - 24.0f);
        const float reserved =
            18.0f + 28.0f +
            (status->getVisibility() == brls::Visibility::VISIBLE
                ? 28.0f
                : 0.0f) +
            (composeButton->getVisibility() == brls::Visibility::VISIBLE
                ? 58.0f
                : 0.0f);
        const float availableHeight =
            std::max(
                120.0f,
                panelHeight - reserved);

        std::vector<const kick::ChatMessage*> selected;
        float consumed = 0.0f;

        for (auto iterator = messages.rbegin();
             iterator != messages.rend() &&
             selected.size() < rows.size();
             ++iterator) {
            const float measuredHeight =
                KickChatMessageView::estimateHeightFor(
                *iterator,
                preferences,
                innerWidth,
                availableHeight);

            if (!selected.empty() &&
                consumed + measuredHeight > availableHeight)
                break;

            selected.push_back(&*iterator);
            consumed += measuredHeight;
        }

        std::reverse(selected.begin(), selected.end());
        std::deque<kick::ChatMessage> visibleMessages;
        for (size_t index = 0;
             index < selected.size() && index < rows.size();
             ++index) {
            rows[index]->configure(
                *selected[index],
                preferences,
                innerWidth,
                availableHeight);
            visibleMessages.push_back(*selected[index]);
        }

        messages = std::move(visibleMessages);
    }

    static constexpr size_t ROW_POOL = 20;
    static constexpr size_t MAX_MESSAGES = ROW_POOL;

    bool docked = false;
    float panelWidth = 330.0f;
    float panelHeight = brls::Application::contentHeight;
    kick::ChatPreferences preferences;
    std::function<void()> sendRequested;
    std::function<bool()> backRequested;
    std::deque<kick::ChatMessage> messages;
    brls::Box* headerBox = nullptr;
    brls::Label* titlePrefix = nullptr;
    brls::Label* chattersCountLabel = nullptr;
    brls::Label* chattersSuffix = nullptr;
    int currentChatterCount = 0;
    brls::Label* status = nullptr;
    brls::Box* messagesBox = nullptr;
    brls::Button* composeButton = nullptr;
    std::vector<KickChatMessageView*> rows;
};

struct ChatUiState {
    std::atomic_bool alive{true};
    KickChatPanel* activePanel = nullptr;
    std::deque<kick::ChatMessage> messages;
    std::string status;
    int chatterCount = 0;

    void append(kick::ChatMessage message) {
        status.clear();
        if (activePanel) {
            activePanel->setStatus("");
            activePanel->append(std::move(message));
            messages = activePanel->visibleMessages();
            return;
        }

        messages.push_back(std::move(message));
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
    }

    void setStatus(const std::string& value) {
        if (!messages.empty() && value.rfind("Connecting", 0) == 0) {
            return;
        }
        status = value;
        if (activePanel) activePanel->setStatus(value);
    }

    void setChatterCount(int count) {
        chatterCount = count;
        if (activePanel) activePanel->setChatterCount(count);
    }

    void activate(KickChatPanel* panel) {
        if (activePanel == panel) return;

        activePanel = panel;
        if (!activePanel) return;

        if (!messages.empty() && status.rfind("Connecting", 0) == 0) {
            status.clear();
        }

        activePanel->replaceMessages(messages);
        messages = activePanel->visibleMessages();
        activePanel->setStatus(status);
        activePanel->setChatterCount(chatterCount);
    }

private:
    static constexpr size_t MAX_MESSAGES = 20;
};


struct KickPlaybackRecoveryState {
    std::atomic_bool alive{true};
    std::atomic_bool recovering{false};
    std::atomic_bool retryScheduled{false};
    std::atomic_bool reconfigArmed{false};
    std::atomic_bool hardwareDecode{false};
    std::atomic_bool hybridSoftware{false};
    std::atomic_bool hybridRestoreEligible{false};
    std::atomic_bool decoderSwitching{false};
    // Hardware-only recovery must not replace the playlist until MPV has
    // confirmed that the old NVTEGRA-backed file has stopped. This prevents
    // a new load from racing an in-flight hardware decode job.
    std::atomic_bool waitingForHardwareStop{false};
    std::atomic_int pendingRecoveryAttempt{0};
    std::atomic_uint hardwareStopGeneration{0};
    std::atomic_int decoderMode{
        static_cast<int>(kick::DecoderMode::Software)};
    std::atomic_int attempt{0};
    std::atomic_int reconfigCount{0};
    std::atomic_uint restoreGeneration{0};
    std::chrono::steady_clock::time_point lastLoadedAt{};
    std::chrono::steady_clock::time_point lastReconfigRecovery{};
    // Flap protection: when a busy channel's playlist keeps ending, resolving
    // a brand-new playlist every time hands out a different edge host and
    // restarts the stream over and over. Remember the last playlist that
    // actually opened so recovery can retry it first, and back off when
    // interruptions cluster together.
    std::string lastUrl;
    std::chrono::steady_clock::time_point lastRecoveryAt{};
    int recentRecoveries = 0;
    int reuseStreak = 0;
    std::atomic_bool backoffDone{false};
    // Set once Kick has confirmed the streamer ended the broadcast; all
    // further recovery attempts are abandoned while the player exits.
    std::atomic_bool offlineHandled{false};
    std::atomic_bool offlineCheckRunning{false};
    std::atomic_int offlineStrikes{0};
    // Leaves the player once the stream has ended; set by the player, and
    // only called while `alive` is set.
    std::function<void()> exitPlayer;
    std::string channel;
    std::function<void(std::vector<kick::SubtitleTrack>)> onSubtitlesResolved;
};

constexpr int KICK_RECOVERY_MAX_ATTEMPTS = 5;
constexpr int HYBRID_RESTORE_STABLE_SECONDS = 8;
constexpr int HYBRID_REARM_SECONDS = 3;

void performKickPlaybackRecovery(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    int attempt);

void resolveKickReplacementPlaylist(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    int attempt);

void continueHardwareRecoveryAfterStop(
    const std::shared_ptr<KickPlaybackRecoveryState>& state);

void scheduleHybridHardwareRestore(
    const std::shared_ptr<KickPlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() ||
        !state->hybridSoftware.load() ||
        !state->hybridRestoreEligible.load())
        return;

    const unsigned int generation =
        state->restoreGeneration.fetch_add(1) + 1;

    brls::Logger::info(
        "kickNX Hybrid: normal presentation returned; waiting {} seconds "
        "before restoring hardware decoding",
        HYBRID_RESTORE_STABLE_SECONDS);

    ThreadPool::instance().submit(
        [state, generation](HTTP&) {
            for (int elapsed = 0;
                 elapsed < HYBRID_RESTORE_STABLE_SECONDS &&
                 state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, generation]() {
                if (!state->alive.load() ||
                    generation != state->restoreGeneration.load() ||
                    state->recovering.load() ||
                    kick::loadDecoderMode() !=
                        kick::DecoderMode::Hybrid ||
                    !state->hybridSoftware.load() ||
                    !state->hybridRestoreEligible.load())
                    return;

                state->decoderSwitching.store(true);
                state->reconfigArmed.store(false);
                state->hybridRestoreEligible.store(false);

                auto& mpv = MPVCore::instance();
                mpv.command("set", "vd-lavc-dr", "no");
                mpv.command(
                    "set",
                    "hwdec",
                    MPVCore::PLAYER_HWDEC_METHOD.c_str());
                mpv.command("set", "pause", "no");

                brls::Logger::warning(
                    "kickNX Hybrid: requested hardware decoder restoration");

                ThreadPool::instance().submit(
                    [state, generation](HTTP&) {
                        for (int elapsed = 0;
                             elapsed < HYBRID_REARM_SECONDS &&
                             state->alive.load();
                             ++elapsed) {
                            std::this_thread::sleep_for(
                                std::chrono::seconds(1));
                        }

                        if (!state->alive.load()) return;

                        brls::sync([state, generation]() {
                            if (!state->alive.load() ||
                                generation !=
                                    state->restoreGeneration.load() ||
                                kick::loadDecoderMode() !=
                                    kick::DecoderMode::Hybrid)
                                return;

                            auto& mpv = MPVCore::instance();
                            const std::string active =
                                mpv.getString("hwdec-current");

                            state->decoderSwitching.store(false);
                            state->lastLoadedAt =
                                std::chrono::steady_clock::now();

                            if (active.empty() || active == "no") {
                                state->hybridSoftware.store(true);
                                state->hardwareDecode.store(false);
                                state->reconfigArmed.store(true);
                                mpv.command("set", "hwdec", "no");
                                brls::Logger::warning(
                                    "kickNX Hybrid: hardware decoder did not "
                                    "reactivate; software remains active");
                                return;
                            }

                            state->hybridSoftware.store(false);
                            state->hardwareDecode.store(true);
                            state->reconfigArmed.store(true);
#if defined(KICKNX_PLAYBACK_PERF_DEBUG)
                            mpv.logHardwareDecoderState(
                                "hybrid-hardware-restored");
#endif
                        });
                    });
            });
        });
}

bool isAudioOnlyQuality(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c))
            normalized.push_back(
                static_cast<char>(std::tolower(c)));
    }
    return normalized == "audioonly";
}

void scheduleKickHardwareActivationCheck(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    kick::DecoderMode decoderMode) {
    if (!state || !state->alive.load() ||
        !kick::decoderUsesHardware(decoderMode) ||
        state->hybridSoftware.load())
        return;

    ThreadPool::instance().submit(
        [state, decoderMode](HTTP&) {
            for (int elapsed = 0;
                 elapsed < 8 && state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(
                    std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, decoderMode]() {
                if (!state->alive.load() ||
                    state->hybridSoftware.load() ||
                    kick::loadDecoderMode() != decoderMode)
                    return;

                auto& mpv = MPVCore::instance();
                const std::string codec = mpv.getString("video-codec");
                const int64_t width = mpv.getInt("video-params/w");
                const int64_t height = mpv.getInt("video-params/h");
                const std::string active =
                    mpv.getString("hwdec-current");

                if (isAudioOnlyQuality(kick::loadPreferredQuality())) {
                    brls::Logger::info(
                        "kickNX: skipping hardware activation warning for "
                        "explicit audio-only playback");
                    return;
                }

                if (codec.empty() || width <= 0 || height <= 0) {
                    brls::Logger::info(
                        "kickNX: hardware activation check postponed; "
                        "no video stream is active yet "
                        "(codec='{}' size={}x{})",
                        codec,
                        static_cast<long long>(width),
                        static_cast<long long>(height));
                    return;
                }

                if (active.empty() || active == "no") {
                    brls::Logger::warning(
                        "kickNX: Kick hardware decoder did not activate; "
                        "hwdec-current='{}'",
                        active.empty() ? "<empty>" : active);

                    if (decoderMode == kick::DecoderMode::Hybrid) {
                        state->hybridSoftware.store(true);
                        state->hardwareDecode.store(false);
                        mpv.command("set", "hwdec", "no");
                        brls::Logger::warning(
                            "kickNX Hybrid: hardware did not activate; "
                            "continuing in software");
                    } else {
                        brls::Logger::warning(
                            "kickNX: experimental hardware decoding did not activate");
                    }
                    return;
                }

                state->hardwareDecode.store(true);
                brls::Logger::info(
                    "kickNX: Kick hardware decoder active: {}",
                    active);
            });
        });
}

constexpr int KICK_OFFLINE_CONFIRMATIONS = 2;
constexpr long KICK_OFFLINE_EXIT_DELAY_MS = 3500;
constexpr long KICK_OFFLINE_RECHECK_MS = 3000;
constexpr long KICK_OFFLINE_POLL_MS = 20000;

void handleKickStreamOffline(
    const std::shared_ptr<KickPlaybackRecoveryState>& state) {
    if (!state || !state->alive.load()) return;
    if (state->offlineHandled.exchange(true)) return;

    state->retryScheduled.store(false);
    state->recovering.store(false);

    const std::string name = state->channel.empty() ? "The streamer" : state->channel;
    brls::Logger::info("kickNX: {} went offline; leaving the player", name);
    brls::Application::notify(name + " went offline. Returning to the previous screen…");

    // `alive` is cleared when the player is torn down, so a viewer who already
    // backed out never has an unrelated screen closed underneath them.
    brls::delay(KICK_OFFLINE_EXIT_DELAY_MS, [state]() {
        if (!state->alive.load()) return;
        if (state->exitPlayer) state->exitPlayer();
        else VideoView::close(true);
    });
}

// Called whenever playback is interrupted. Commercials and brief CDN drops
// leave the channel live, so the player is only closed after Kick reports no
// livestream on consecutive checks. A failed lookup never counts as offline.
void checkKickStreamOffline(
    const std::shared_ptr<KickPlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() || state->offlineHandled.load()) return;
    if (state->channel.empty()) return;

    bool expected = false;
    if (!state->offlineCheckRunning.compare_exchange_strong(expected, true)) return;

    kick::checkChannelLiveAsync(state->channel, [state](bool known, bool live) {
        state->offlineCheckRunning.store(false);
        if (!state->alive.load() || state->offlineHandled.load()) return;

        if (!known || live) {
            if (known) state->offlineStrikes.store(0);
            return;
        }

        if (state->offlineStrikes.fetch_add(1) + 1 >= KICK_OFFLINE_CONFIRMATIONS) {
            handleKickStreamOffline(state);
            return;
        }
        brls::delay(KICK_OFFLINE_RECHECK_MS, [state]() {
            checkKickStreamOffline(state);
        });
    });
}

// A stream that ends often just stalls without any playback event, so
// interruptions alone never noticed it. Ask Kick every 20 seconds as well,
// until the player closes or the stream is confirmed offline.
void watchKickStreamOffline(
    const std::shared_ptr<KickPlaybackRecoveryState>& state) {
    std::weak_ptr<KickPlaybackRecoveryState> weak = state;
    brls::delay(KICK_OFFLINE_POLL_MS, [weak]() {
        auto current = weak.lock();
        if (!current || !current->alive.load() || current->offlineHandled.load()) return;
        checkKickStreamOffline(current);
        watchKickStreamOffline(current);
    });
}

void finishKickRecoveryFailure(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    const std::string& error) {
    state->retryScheduled.store(false);
    state->recovering.store(false);
    state->attempt.store(0);

    if (state->alive.load()) {
        brls::Logger::warning("kickNX could not resume the live stream: {}", error);
        checkKickStreamOffline(state);
    }
}

void scheduleKickPlaybackRetry(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    const std::string& error) {
    if (!state || !state->alive.load() || state->offlineHandled.load()) return;
    checkKickStreamOffline(state);

    bool expected = false;
    if (!state->retryScheduled.compare_exchange_strong(expected, true))
        return;

    const int nextAttempt = state->attempt.fetch_add(1) + 1;
    if (nextAttempt > KICK_RECOVERY_MAX_ATTEMPTS) {
        finishKickRecoveryFailure(state, error);
        return;
    }

    const int delaySeconds = std::min(nextAttempt, 4);
    ThreadPool::instance().submit(
        [state, nextAttempt, delaySeconds](HTTP&) {
            for (int elapsed = 0;
                 elapsed < delaySeconds && state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, nextAttempt]() {
                if (!state->alive.load() ||
                    !state->recovering.load())
                    return;

                state->retryScheduled.store(false);
                performKickPlaybackRecovery(state, nextAttempt);
            });
        });
}

void useKickSoftwareDecoder(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    const std::string& reason) {
    auto& mpv = MPVCore::instance();
    mpv.command("set", "pause", "yes");
    mpv.command("drop-buffers");
    mpv.command("set", "vd-lavc-dr", "no");
    mpv.command("set", "hwdec", "no");

    if (state) {
        state->hybridSoftware.store(true);
        state->hardwareDecode.store(false);
        state->restoreGeneration.fetch_add(1);
    }

    brls::Logger::warning(
        "kickNX Hybrid: switched to software decoding ({})",
        reason);
}

void resolveKickReplacementPlaylist(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    int attempt) {
    if (!state || !state->alive.load() || !state->recovering.load())
        return;

    // Edge Flap Protection:
    // If we have the current playlist URL and haven't exhausted edge retries yet (attempt < 2),
    // try to reload the same edge URL first. This prevents switching CDN edge hosts
    // (e.g. from use12 to use14/use15) on temporary packet drops or brief segment delays.
    if (attempt < 2 && !state->lastUrl.empty()) {
        brls::Logger::info("kickNX: retrying the same edge server playlist (attempt {})", attempt);
        MPVCore::BOTTOM_BAR = false;
        MPVCore::instance().setUrl(state->lastUrl, kick::mpvExtra());
        return;
    }

    auto config = kick::loadConfig();
    config.channel = state->channel;

    kick::resolveLiveAsync(
        config,
        [state](kick::Resolution result) {
            if (!state->alive.load() || !state->recovering.load()) return;

            if (state->onSubtitlesResolved) {
                state->onSubtitlesResolved(result.subtitles);
            }

            MPVCore::BOTTOM_BAR = false;
            MPVCore::instance().setUrl(
                result.selected.url,
                kick::mpvExtra());

            // Recovery remains active until MPV_LOADED confirms that this
            // replacement playlist actually opened.
        },
        [state, attempt](const std::string& error) {
            if (!state->alive.load()) return;
            scheduleKickPlaybackRetry(state, error);
        });
}

void continueHardwareRecoveryAfterStop(
    const std::shared_ptr<KickPlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() || !state->recovering.load())
        return;

    bool expected = true;
    if (!state->waitingForHardwareStop.compare_exchange_strong(
            expected,
            false))
        return;

    const int attempt = state->pendingRecoveryAttempt.load();
    brls::Logger::warning(
        "kickNX: old hardware-decoded Kick file stopped; "
        "resolving a fresh playlist without toggling hwdec");
    resolveKickReplacementPlaylist(state, attempt);
}

void scheduleHardwareStopWatchdog(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    unsigned int generation) {
    ThreadPool::instance().submit(
        [state, generation](HTTP&) {
            for (int elapsed = 0; elapsed < 5 && state->alive.load(); ++elapsed)
                std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!state->alive.load()) return;

            brls::sync([state, generation]() {
                if (!state->alive.load() ||
                    !state->recovering.load() ||
                    !state->waitingForHardwareStop.load() ||
                    generation != state->hardwareStopGeneration.load())
                    return;

                if (MPVCore::instance().isStopped()) {
                    brls::Logger::warning(
                        "kickNX: hardware stop confirmation event was delayed; "
                        "continuing after MPV reported the file stopped");
                    continueHardwareRecoveryAfterStop(state);
                    return;
                }

                state->waitingForHardwareStop.store(false);
                finishKickRecoveryFailure(
                    state,
                    "the hardware decoder did not stop cleanly");
                brls::Logger::error(
                    "kickNX: aborted hardware recovery because MPV did not "
                    "confirm a clean stop within 5 seconds");
            });
        });
}

void performKickPlaybackRecovery(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    int attempt) {
    if (!state || !state->alive.load() || state->offlineHandled.load()) return;

    const kick::DecoderMode mode = kick::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    if (mode == kick::DecoderMode::Hardware) {
        auto& mpv = MPVCore::instance();
        state->hardwareDecode.store(true);
        state->hybridSoftware.store(false);
        state->reconfigArmed.store(false);
        state->pendingRecoveryAttempt.store(attempt);

        // Never toggle hwdec or discard decoder buffers while NVTEGRA may
        // still be waiting on an in-flight frame. Stop the old file first and
        // load the replacement only after MPV confirms MPV_STOP.
        if (mpv.isStopped()) {
            state->waitingForHardwareStop.store(false);
            brls::Logger::warning(
                "kickNX: hardware-decoded Kick file had already ended; "
                "resolving a fresh playlist");
            resolveKickReplacementPlaylist(state, attempt);
            return;
        }

        state->waitingForHardwareStop.store(true);
        const unsigned int generation =
            state->hardwareStopGeneration.fetch_add(1) + 1;

        brls::Logger::warning(
            "kickNX: stopping the old hardware-decoded Kick file before "
            "loading its replacement");
        mpv.stop();
        scheduleHardwareStopWatchdog(state, generation);
        return;
    }

    state->waitingForHardwareStop.store(false);
    state->hardwareStopGeneration.fetch_add(1);

    if (mode == kick::DecoderMode::Hybrid) {
        // The first dangerous Kick transition moves Hybrid to software.
        // Subsequent recovery events keep software active until the return to
        // the normal presentation is confirmed and stable.
        useKickSoftwareDecoder(
            state,
            state->hybridRestoreEligible.load()
                ? "return transition"
                : "commercial/presentation transition");
    } else {
        auto& mpv = MPVCore::instance();
        mpv.command("set", "pause", "yes");
        mpv.command("drop-buffers");
        state->hardwareDecode.store(false);
        state->hybridSoftware.store(false);
    }

    resolveKickReplacementPlaylist(state, attempt);
}

bool requestKickPlaybackRecovery(
    const std::shared_ptr<KickPlaybackRecoveryState>& state,
    MpvEventEnum event) {
#if defined(KICKNX_PLAYBACK_DEBUG)
    kicknx::debug::log(
        "TW_RECOVERY",
        "request event=%s(%d) state=%p alive=%d recovering=%d waiting_stop=%d switching=%d",
        kicknx::debug::playerEventName(static_cast<int>(event)),
        static_cast<int>(event),
        state.get(),
        state && state->alive.load() ? 1 : 0,
        state && state->recovering.load() ? 1 : 0,
        state && state->waitingForHardwareStop.load() ? 1 : 0,
        state && state->decoderSwitching.load() ? 1 : 0);
#endif
    if (!state || !state->alive.load()) return true;
    if (state->offlineHandled.load()) return true;

    if (state->decoderSwitching.load()) {
        brls::Logger::debug(
            "kickNX Hybrid: ignoring decoder-generated event {}",
            static_cast<int>(event));
        return true;
    }

    if (state->waitingForHardwareStop.load()) {
        brls::Logger::debug(
            "kickNX: ignoring event {} while waiting for the old "
            "hardware-decoded file to stop",
            static_cast<int>(event));
        return true;
    }

    // Decoder reset itself can generate VIDEO_RECONFIG. Ignore those while a
    // recovery is already in progress instead of creating recursive retries.
    if (event == MpvEventEnum::VIDEO_RECONFIG &&
        state->recovering.load())
        return true;

    const kick::DecoderMode mode = kick::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    // In Hardware mode, VIDEO_RECONFIG events are suppressed to avoid tearing down
    // NVTEGRA during in-flight frames. For actual stream dropouts (END_OF_FILE,
    // MPV_FILE_ERROR), we allow hardware recovery to run via performKickPlaybackRecovery
    // which cleanly stops the hardware file before reloading, preventing premature exit
    // to the home screen when a streamer's cache empties.
    if (mode == kick::DecoderMode::Hardware && event == MpvEventEnum::VIDEO_RECONFIG) {
        brls::Logger::warning(
            "kickNX Hardware: video reconfig recovery suppressed "
            "to avoid tearing down an active NVTEGRA frame");
        return true;
    }

    // A second transition while Hybrid is already in software is our signal
    // that Kick has moved away from the inserted presentation. Hardware is
    // restored only after the replacement stream has loaded and stayed stable.
    if (mode == kick::DecoderMode::Hybrid &&
        state->hybridSoftware.load()) {
        state->hybridRestoreEligible.store(true);
    }

    bool expected = false;
    if (state->recovering.compare_exchange_strong(expected, true)) {
        state->attempt.store(0);
        state->retryScheduled.store(false);
        state->restoreGeneration.fetch_add(1);

        const auto incidentTime = std::chrono::steady_clock::now();
        if (state->lastRecoveryAt.time_since_epoch().count() != 0 &&
            incidentTime - state->lastRecoveryAt < std::chrono::seconds(60)) {
            state->recentRecoveries++;
        } else {
            state->recentRecoveries = 1;
            state->reuseStreak = 0;
        }
        state->lastRecoveryAt = incidentTime;
        state->backoffDone.store(false);

        brls::Logger::warning(
            "kickNX: Kick live playback interrupted (event {}), "
            "resolving a fresh playlist",
            static_cast<int>(event));

        checkKickStreamOffline(state);
        performKickPlaybackRecovery(state, 0);
    } else {
        // If the replacement playlist fails before MPV_LOADED, schedule one
        // controlled retry rather than starting overlapping resolver jobs.
        scheduleKickPlaybackRetry(
            state,
            "the replacement playlist did not open");
    }

    return true;
}

void handleKickVideoReconfig(
    const std::shared_ptr<KickPlaybackRecoveryState>& state) {
#if defined(KICKNX_PLAYBACK_DEBUG)
    kicknx::debug::log(
        "TW_RECONFIG",
        "enter state=%p alive=%d switching=%d armed=%d recovering=%d count=%d",
        state.get(),
        state && state->alive.load() ? 1 : 0,
        state && state->decoderSwitching.load() ? 1 : 0,
        state && state->reconfigArmed.load() ? 1 : 0,
        state && state->recovering.load() ? 1 : 0,
        state ? state->reconfigCount.load() : -1);
#endif
    if (!state || !state->alive.load() ||
        state->decoderSwitching.load())
        return;

    const kick::DecoderMode mode = kick::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    if (mode == kick::DecoderMode::Software) return;

    const int count = state->reconfigCount.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now();

    // Ignore startup/reload reconfiguration. A later Kick presentation
    // change (such as live -> commercial or commercial -> live) is the
    // dangerous transition.
    if (!state->reconfigArmed.load() ||
        state->lastLoadedAt.time_since_epoch().count() == 0 ||
        now - state->lastLoadedAt < std::chrono::seconds(5)) {
        brls::Logger::debug(
            "kickNX: ignoring startup video reconfig {}",
            count);
        return;
    }

    if (state->lastReconfigRecovery.time_since_epoch().count() != 0 &&
        now - state->lastReconfigRecovery < std::chrono::seconds(8)) {
        brls::Logger::debug(
            "kickNX: throttling repeated video reconfig {}",
            count);
        return;
    }

    if (mode == kick::DecoderMode::Hardware) {
        // Diagnostic/safety behavior: do not stop, reload, drop buffers, or
        // change hwdec in response to VIDEO_RECONFIG. Any of those operations
        // can race a hardware frame currently being downloaded by MPV.
        brls::Logger::warning(
            "kickNX Hardware: Kick video reconfiguration {} detected; "
            "leaving MPV/NVTEGRA untouched",
            count);
        return;
    }

    state->lastReconfigRecovery = now;

    if (mode == kick::DecoderMode::Hybrid) {
        if (state->hybridSoftware.load()) {
            state->hybridRestoreEligible.store(true);
            brls::Logger::warning(
                "kickNX Hybrid: second Kick presentation transition "
                "detected; hardware restore will be attempted after stable "
                "software playback");
        } else {
            brls::Logger::warning(
                "kickNX Hybrid: Kick presentation transition detected; "
                "switching to software before consuming the new frames");
        }
    } else {
        brls::Logger::warning(
            "kickNX: post-startup Kick video reconfiguration detected; "
            "resetting hardware decoder before new frames are consumed");
    }

    requestKickPlaybackRecovery(
        state,
        MpvEventEnum::VIDEO_RECONFIG);
}

const char* configuredVideoRotation() {
    switch (MPVCore::VIDEO_ROTATION) {
        case 1: return "90";
        case 2: return "180";
        case 3: return "270";
        default: return "0";
    }
}

void resetPlayerVideoLayout() {
    auto& mpv = MPVCore::instance();
    mpv.command("set", "video-rotate", configuredVideoRotation());
    mpv.command("set", "video-margin-ratio-left", "0.0");
    mpv.command("set", "video-margin-ratio-right", "0.0");
    mpv.command("set", "video-margin-ratio-top", "0.0");
    mpv.command("set", "video-margin-ratio-bottom", "0.0");
    mpv.command("set", "video-zoom", "0.0");
    mpv.command("set", "video-pan-x", "0.0");
    mpv.command("set", "video-pan-y", "0.0");
    mpv.command("set", "video-align-x", "0");
    mpv.command("set", "video-align-y", "0");
    mpv.command("set", "video-recenter", "yes");
    mpv.setAspect(MPVCore::VIDEO_ASPECT);
}

std::atomic_uint64_t nextPlaybackSession{0};
std::atomic_uint64_t activePlaybackSession{0};

}  // namespace

class RemotePlayer : public brls::Box {
public:
    RemotePlayer(
        const remote::DirEntry& item,
        std::string kickChannel = "",
        std::string loadingNote = "",
        std::vector<kick::SubtitleTrack> kickSubtitles = {},
        int64_t uptimeSeconds = 0)
        : kickChannel(std::move(kickChannel)),
          kickSubtitles(std::move(kickSubtitles)),
          playbackSessionId(++nextPlaybackSession) {
        activePlaybackSession.store(playbackSessionId);
        kick::setLiveSubtitles({});
#if defined(KICKNX_PLAYBACK_DEBUG)
        kicknx::debug::log(
            "TW_PLAYER",
            "construct session=%llu channel=%s item_type=%d item_name=%s",
            static_cast<unsigned long long>(playbackSessionId),
            this->kickChannel.empty() ? "<non-kick>" : this->kickChannel.c_str(),
            static_cast<int>(item.type),
            item.name.c_str());
#endif
        MPVCore::LIVE_STREAM_RECOVERY = !this->kickChannel.empty();
        resetPlayerVideoLayout();
        float width = brls::Application::contentWidth;
        float height = brls::Application::contentHeight;
        view->setDimensions(width, height);
        view->setGrow(1.0f);
        view->setId("video");
        view->setTitie(item.name);
        view->hideVideoQuality();
        if (!loadingNote.empty()) {
            view->setLoadingNote(loadingNote);
        }
        view->showLoading();
        this->setDimensions(width, height);
        this->setAxis(brls::Axis::ROW);
        this->addView(view);

        VideoView::setGlobalCloseHandler([this]() {
            deactivatePlayback();
        });

        auto composeChat = [this]() {
            openChatComposer();
        };

        auto closePlayback = [this]() {
            deactivatePlayback();
            return VideoView::close(true);
        };

        this->registerAction(
            "hints/back"_i18n,
            brls::BUTTON_B,
            [this](brls::View*) {
                if (portraitLayout && portraitComposer && portraitComposer->isEmotePanelVisible()) {
                    portraitComposer->setEmotePanelVisible(false);
                    return true;
                }
                deactivatePlayback();
                return VideoView::close(true);
            },
            true);

        chatOverlayPanel =
            new KickChatPanel(
                false,
                composeChat,
                closePlayback);
        view->setContentOverlay(chatOverlayPanel);

        chatDockedPanel =
            new KickChatPanel(
                true,
                composeChat,
                closePlayback);
        this->addView(chatDockedPanel);

        if (!this->kickChannel.empty()) {
            portraitComposer = new PortraitChatComposer(
                this->kickChannel,
                [this](const std::string& message) {
                    return sendPortraitMessage(message);
                });
            this->addView(portraitComposer);
        }

        chatUi = std::make_shared<ChatUiState>();

        if (this->kickChannel.empty()) {
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            view->setDimensions(width, height);
        }

        if (!this->kickChannel.empty()) {
            view->setLiveMode(true, uptimeSeconds);
            MPVCore::LIVE_STREAM_RECOVERY = true;

            const kick::DecoderMode decoderMode =
                kick::loadDecoderMode();
            const bool hardwareDecode =
                kick::decoderUsesHardware(decoderMode);
#if defined(KICKNX_PLAYBACK_DEBUG)
            kicknx::debug::log(
                "TW_PLAYER",
                "session=%llu decoder_mode=%d hardware=%d",
                static_cast<unsigned long long>(playbackSessionId),
                static_cast<int>(decoderMode),
                hardwareDecode ? 1 : 0);
#endif
            if (hardwareDecode) {
                MPVCore::instance().command(
                    "set",
                    "hwdec",
                    MPVCore::PLAYER_HWDEC_METHOD.c_str());
                MPVCore::instance().command(
                    "set",
                    "vd-lavc-dr",
                    "no");
                brls::Logger::warning(
                    decoderMode == kick::DecoderMode::Hybrid
                        ? "kickNX: experimental Kick Hybrid decoding enabled"
                        : "kickNX: experimental Kick hardware decoding enabled");
            } else {
                MPVCore::instance().command("set", "hwdec", "no");
                brls::Logger::info(
                    "kickNX: stable Kick software decoding enabled");
            }

            playbackRecovery =
                std::make_shared<KickPlaybackRecoveryState>();
            playbackRecovery->channel = this->kickChannel;
            playbackRecovery->hardwareDecode.store(hardwareDecode);
            playbackRecovery->decoderMode.store(
                static_cast<int>(decoderMode));
            playbackRecovery->hybridSoftware.store(false);
            playbackRecovery->hybridRestoreEligible.store(false);
            playbackRecovery->onSubtitlesResolved = [this](std::vector<kick::SubtitleTrack> subs) {
                this->kickSubtitles = std::move(subs);
            };
            playbackRecovery->exitPlayer = [this]() { leaveEndedStream(); };
            watchKickStreamOffline(playbackRecovery);

            std::weak_ptr<KickPlaybackRecoveryState> weakRecovery =
                playbackRecovery;
            view->setPlaybackRecoveryHandler(
                [weakRecovery](MpvEventEnum event) {
                    auto state = weakRecovery.lock();
                    if (!state) return true;
                    return requestKickPlaybackRecovery(state, event);
                });

            chatPreferenceSubscribeID =
                kick::chatPreferencesEvent()->subscribe(
                    [this](kick::ChatPreferences preferences) {
                        applyChatPreferences(preferences);
                    });
            applyChatPreferences(
                kick::loadChatPreferences());

            auto& orientationController =
                kicknx::portrait::OrientationController::instance();
            orientationSubscribeID =
                orientationController.getOrientationChanged()->subscribe(
                    [this](kicknx::portrait::DisplayOrientation orientation) {
                        applyDisplayOrientation(orientation);
                    });
            applyDisplayOrientation(orientationController.orientation());

            chatModeAction = this->registerAction(
                "Chat layout",
                brls::BUTTON_START,
                [this](brls::View*) {
                    cycleChatMode();
                    return true;
                },
                false);

            channelSubscribeID =
                view->getChannelEvent()->subscribe(
                    [this]() {
                        openChannelPage();
                    });
            channelEventSubscribed = true;

            channelProfileCancel =
                std::make_shared<std::atomic_bool>(
                    false);
            this->ptrLock();
            kick::loadChannelProfileAsync(
                kick::loadConfig(),
                this->kickChannel,
                [this](kick::UserProfile profile) {
                    channelProfile =
                        std::move(profile);
                    view->showChannelButton(
                        channelProfile.profileImageUrl);
                    if (channelProfile.uptimeSeconds > 0) {
                        view->setStreamUptime(channelProfile.uptimeSeconds);
                    }
                    this->ptrUnlock();
                },
                [this](const std::string& error) {
                    brls::Logger::warning(
                        "kickNX: player channel avatar "
                        "could not be loaded: {}",
                        error);
                    view->hideChannelButton();
                    this->ptrUnlock();
                },
                channelProfileCancel);
        }

        if (item.type == remote::EntryType::PLAYLIST) {
            view->hideVideoProgressSlider();
        } else if (item.name.size() > 0) {
            titles.push_back(item.name);
        }

        auto& mpv = MPVCore::instance();
        eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
            if (playerShuttingDown ||
                activePlaybackSession.load() != playbackSessionId)
                return;

#if defined(KICKNX_PLAYBACK_DEBUG)
            kicknx::debug::log(
                "TW_EVENT",
                "session=%llu event=%s(%d) recovery=%p recovering=%d armed=%d "
                "hw=%d hybrid_sw=%d waiting_stop=%d",
                static_cast<unsigned long long>(playbackSessionId),
                kicknx::debug::playerEventName(static_cast<int>(event)),
                static_cast<int>(event),
                playbackRecovery.get(),
                playbackRecovery && playbackRecovery->recovering.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->reconfigArmed.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->hardwareDecode.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->hybridSoftware.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->waitingForHardwareStop.load() ? 1 : 0);
#endif

            auto& mpv = MPVCore::instance();
            switch (event) {
            case MpvEventEnum::MPV_LOADED: {
                if (!this->kickChannel.empty()) {
                    applyKickVideoLayout(currentChatPreferences);
                    if (playbackRecovery) {
                        const kick::DecoderMode decoderMode =
                            kick::loadDecoderMode();
                        const bool hardwareDecode =
                            kick::decoderUsesHardware(decoderMode) &&
                            !playbackRecovery->hybridSoftware.load();

                        playbackRecovery->decoderMode.store(
                            static_cast<int>(decoderMode));
                        playbackRecovery->hardwareDecode.store(
                            hardwareDecode);
                        playbackRecovery->lastLoadedAt =
                            std::chrono::steady_clock::now();
                        {
                            const std::string loadedPath =
                                mpv.getString("path");
                            if (!loadedPath.empty())
                                playbackRecovery->lastUrl = loadedPath;
                        }
                        playbackRecovery->waitingForHardwareStop.store(false);
                        playbackRecovery->hardwareStopGeneration.fetch_add(1);
                        playbackRecovery->reconfigArmed.store(
                            decoderMode != kick::DecoderMode::Software);

                        if (decoderMode == kick::DecoderMode::Software) {
                            playbackRecovery->hybridSoftware.store(false);
                            playbackRecovery->hybridRestoreEligible.store(false);
                            MPVCore::instance().command(
                                "set",
                                "hwdec",
                                "no");
                        } else if (decoderMode == kick::DecoderMode::Hybrid &&
                                   playbackRecovery->hybridSoftware.load()) {
                            MPVCore::instance().command(
                                "set",
                                "hwdec",
                                "no");
#if defined(KICKNX_PLAYBACK_PERF_DEBUG)
                            MPVCore::instance().logHardwareDecoderState(
                                "hybrid-software-transition");
#endif
                        } else {
#if defined(KICKNX_PLAYBACK_PERF_DEBUG)
                            MPVCore::instance().logHardwareDecoderState(
                                decoderMode == kick::DecoderMode::Hybrid
                                    ? "hybrid-hardware"
                                    : "kick-experimental-hardware");
#endif
                            scheduleKickHardwareActivationCheck(
                                playbackRecovery,
                                decoderMode);
                        }
                    }
                }

                const bool recoveryCompleted =
                    playbackRecovery &&
                    playbackRecovery->recovering.exchange(false);
                if (recoveryCompleted) {
                    playbackRecovery->retryScheduled.store(false);
                    playbackRecovery->attempt.store(0);
                    brls::Logger::info("kickNX: live stream resumed");
                }

                if (playbackRecovery &&
                    kick::loadDecoderMode() == kick::DecoderMode::Hybrid &&
                    playbackRecovery->hybridSoftware.load() &&
                    playbackRecovery->hybridRestoreEligible.load()) {
                    scheduleHybridHardwareRestore(playbackRecovery);
                }

                if (titles.empty()) this->loadList();
                mpv.command("set", "pause", "no");
                view->getProfile()->init("Local");
                // Every Kick caption language is its own live HLS feed, and an
                // added track keeps downloading for as long as it is loaded.
                // Adding all of them on each load starved the video cache, so
                // only the caption the viewer chose in player settings is
                // re-added after a load or recovery.
                kick::setLiveSubtitles(this->kickSubtitles);
                for (const auto& sub : this->kickSubtitles) {
                    if (kick::liveCaptionLanguage().empty() ||
                        sub.language != kick::liveCaptionLanguage())
                        continue;
                    mpv.command(
                        "sub-add",
                        sub.url.c_str(),
                        "select",
                        sub.name.c_str(),
                        sub.language.c_str());
#if defined(KICKNX_PLAYBACK_DEBUG)
                    kicknx::debug::log(
                        "TW_SUBTITLE",
                        "session=%llu sub-add name=%s lang=%s url=%s default=%d",
                        static_cast<unsigned long long>(playbackSessionId),
                        sub.name.c_str(),
                        sub.language.c_str(),
                        sub.url.c_str(),
                        sub.isDefault ? 1 : 0);
#endif
                    break;
                }
                const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
                for (auto& it : this->subtitles) {
                    mpv.command("sub-add", it.second.c_str(), flag, it.first.c_str());
                }
                break;
            }
            case MpvEventEnum::MPV_STOP:
                if (playbackRecovery &&
                    playbackRecovery->waitingForHardwareStop.load())
                    continueHardwareRecoveryAfterStop(playbackRecovery);
                break;
            case MpvEventEnum::VIDEO_RECONFIG:
                if (playbackRecovery)
                    handleKickVideoReconfig(playbackRecovery);
                break;
            default:;
            }
        });
        settingSubscribeID = view->getSettingEvent()->subscribe([this]() {
            brls::View* setting = new PlayerSetting(this->kickChannel);
            brls::Application::pushActivity(new brls::Activity(setting));
        });
    }

    void deactivatePlayback() {
        if (playerShuttingDown) return;
        playerShuttingDown = true;
        if (playerAlive) playerAlive->store(false);

        VideoView::setGlobalCloseHandler(nullptr);

        if (activePlaybackSession.load() == playbackSessionId)
            activePlaybackSession.store(0);

        MPVCore::LIVE_STREAM_RECOVERY = false;
        if (playbackRecovery) {
            playbackRecovery->alive.store(false);
            playbackRecovery->recovering.store(false);
            playbackRecovery->retryScheduled.store(false);
            playbackRecovery->reconfigArmed.store(false);
            playbackRecovery->decoderSwitching.store(false);
            playbackRecovery->restoreGeneration.fetch_add(1);
        }
        view->setPlaybackRecoveryHandler({});

        if (chatUi) chatUi->alive.store(false);
        stopChat();
        resetPlayerVideoLayout();

        auto& mpv = MPVCore::instance();
        mpv.stop();
        const char* configuredHwdec = MPVCore::HARDWARE_DEC
            ? MPVCore::PLAYER_HWDEC_METHOD.c_str()
            : "no";
        mpv.command("set", "hwdec", configuredHwdec);
    }

    void deactivatePlaybackForNavigation() {
        deactivatePlayback();
    }

    // Called once Kick confirms the stream ended. VideoView::close pops
    // whatever is on top, so with player settings, the chat composer or a
    // dialog open it closed that instead and left the dead player behind.
    // Close those first, one per pass, then the player itself.
    void leaveEndedStream() {
        if (playerShuttingDown) return;
        brls::Activity* own = this->getParentActivity();
        const auto stack = brls::Application::getActivitiesStack();
        if (!own || stack.empty()) return;

        if (stack.back() != own) {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            std::weak_ptr<std::atomic_bool> weakAlive = playerAlive;
            // Give the popped screen time to be removed before looking again;
            // popping twice in one go would pop the same screen twice.
            brls::delay(300, [this, weakAlive]() {
                auto alive = weakAlive.lock();
                if (!alive || !alive->load()) return;
                leaveEndedStream();
            });
            return;
        }

        deactivatePlayback();
        VideoView::close(true);
    }

    ~RemotePlayer() override {
        if (playerAlive) playerAlive->store(false);
        deactivatePlayback();
        if (asyncCancelToken)
            asyncCancelToken->store(true);
#if defined(KICKNX_PLAYBACK_DEBUG)
        kicknx::debug::log(
            "TW_PLAYER",
            "destruct session=%llu channel=%s owns=%d",
            static_cast<unsigned long long>(playbackSessionId),
            this->kickChannel.empty() ? "<non-kick>" : this->kickChannel.c_str(),
            activePlaybackSession.load() == playbackSessionId ? 1 : 0);
#endif
        playerShuttingDown = true;
        const bool ownsPlayback =
            activePlaybackSession.load() == playbackSessionId;
        if (ownsPlayback)
            activePlaybackSession.store(0);

        if (ownsPlayback && !this->kickChannel.empty()) {
            auto resetPreferences = currentChatPreferences;
            resetPreferences.mode = kick::ChatMode::Off;
            applyKickVideoLayout(resetPreferences);
        }

        if (ownsPlayback) {
            MPVCore::LIVE_STREAM_RECOVERY = false;
            resetPlayerVideoLayout();

            const char* configuredHwdec = MPVCore::HARDWARE_DEC
                ? MPVCore::PLAYER_HWDEC_METHOD.c_str()
                : "no";
            MPVCore::instance().command(
                "set",
                "hwdec",
                configuredHwdec);
        }

        if (playbackRecovery) {
            playbackRecovery->alive.store(false);
            playbackRecovery->recovering.store(false);
            playbackRecovery->retryScheduled.store(false);
            playbackRecovery->reconfigArmed.store(false);
            playbackRecovery->decoderSwitching.store(false);
            playbackRecovery->waitingForHardwareStop.store(false);
            playbackRecovery->hardwareStopGeneration.fetch_add(1);
            playbackRecovery->restoreGeneration.fetch_add(1);
        }
        view->setPlaybackRecoveryHandler({});

        if (chatSendAction != ACTION_NONE) {
            this->unregisterAction(chatSendAction);
            chatSendAction = ACTION_NONE;
        }

        if (chatModeAction != ACTION_NONE) {
            this->unregisterAction(chatModeAction);
            chatModeAction = ACTION_NONE;
        }

        if (chatUi) chatUi->alive.store(false);
        chatClient.reset();
        if (!this->kickChannel.empty()) {
            kick::chatPreferencesEvent()->unsubscribe(
                chatPreferenceSubscribeID);
            if (orientationSubscribeID) {
                kicknx::portrait::OrientationController::instance()
                    .getOrientationChanged()->unsubscribe(*orientationSubscribeID);
                orientationSubscribeID.reset();
            }
        }

        if (portraitLayout) {
            brls::Application::setContentOrientation(
                brls::ContentOrientation::LANDSCAPE);
            portraitLayout = false;
        }

        auto& mpv = MPVCore::instance();
        mpv.getEvent()->unsubscribe(eventSubscribeID);
        view->getPlayEvent()->unsubscribe(playSubscribeID);
        if (channelProfileCancel)
            channelProfileCancel->store(true);
        if (channelEventSubscribed)
            view->getChannelEvent()->unsubscribe(
                channelSubscribeID);
        view->getSettingEvent()->unsubscribe(settingSubscribeID);
        if (this->kickChannel.empty()) {
            mpv.command("write-watch-later-config");
        } else {
            brls::Logger::debug(
                "kickNX: skipped watch-later write for Kick live playback");
        }
    }

    void willDisappear(bool resetState) override {
        deactivatePlayback();
#ifdef ANDROID
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(235, 235, 235, 255));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(45, 45, 45, 255));
#else
        restoreClearColor();
#endif
    }

    void willAppear(bool resetState) override {
#ifdef ANDROID
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
#else
        // While MPV has no frame to present (initial load, live reconnects)
        // the window clear colour shows through. Keep it black instead of the
        // theme grey so buffering never flashes a grey screen.
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 255));
#endif
        if (!this->kickChannel.empty())
            restorePlaybackFocus();
    }

    static void restoreClearColor() {
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGB(235, 235, 235));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGB(45, 45, 45));
    }

    void setList(const DirList& list, size_t index, const std::string& extra) {
        // æ’­æ”¾åˆ—è¡¨
        DirList urls;
        for (size_t i = 1; i < list.size(); i++) {
            auto& it = list.at(i);
            if (it.type == remote::EntryType::VIDEO) {
                if (i == index) index = urls.size();
                titles.push_back(it.name);
                urls.push_back(it);
            }
        }
        if (titles.size() > 1) view->setList(titles, index);

        playSubscribeID = view->getPlayEvent()->subscribe([this, list, urls, extra](int index) {
            if (index < 0 || index >= (int)urls.size()) {
                return VideoView::close(true);
            }
            MPVCore::instance().reset();
            auto& item = urls.at(index);

            std::string name = item.name;
            auto pos = name.find_last_of(".");
            if (pos != std::string::npos) {
                name = name.substr(0, pos);
            }

            this->subtitles.clear();
            for (auto& s : list) {
                if (s.type == remote::EntryType::SUBTITLE) {
                    if (!s.name.rfind(name, 0)) {
                        this->subtitles.insert(std::make_pair(s.name.substr(pos), s.url()));
                    }
                }
            }
            this->url = item.url();
            MPVCore::instance().setUrl(this->url, extra);
            view->setTitie(name);
            return true;
        });

        view->getPlayEvent()->fire(index);
    }

    uint64_t getPlaybackSessionId() const { return playbackSessionId; }

    void setPlaybackCancelToken(std::shared_ptr<std::atomic_bool> cancel) {
        asyncCancelToken = std::move(cancel);
    }

    void setTitle(const std::string& title) {
        if (view) view->setTitie(title);
    }

    void showLoading(const std::string& note = "") {
        if (view) {
            if (!note.empty()) view->setLoadingNote(note);
            view->showLoading();
        }
    }

    void setKickSubtitles(std::vector<kick::SubtitleTrack> subs) {
        this->kickSubtitles = std::move(subs);
    }

    void setUrl(const std::string& path, const std::string& extra = "") {
        if (path.empty()) return;
        playSubscribeID = view->getPlayEvent()->subscribe([](int index) { return VideoView::close(true); });
        MPVCore::instance().command("set", "pause", "no");
        MPVCore::instance().setUrl(path, extra);
    }

    void loadList() {
        auto& mpv = MPVCore::instance();
        int64_t count = mpv.getInt("playlist-count");
        for (int64_t n = 0; n < count; n++) {
            auto key = fmt::format("playlist/{}/title", n);
            titles.push_back(mpv.getString(key));
        }
        if (titles.size() > 1) view->setList(titles, 0);
        view->setTitie(titles.front());

        playSubscribeID = view->getPlayEvent()->subscribe([this, &mpv](int index) {
            if (index < 0 || index >= (int)titles.size()) {
                return VideoView::close();
            }
            MPVCore::instance().reset();
            view->setTitie(titles.at(index));
            mpv.command("playlist-play-index", std::to_string(index).c_str());
            return true;
        });
    }


    void openChannelPage() {
        if (kickChannel.empty() || navigatingToChannel) return;
        navigatingToChannel = true;

        const std::string channel = kickChannel;
        deactivatePlaybackForNavigation();

        if (!brls::Application::popActivity(
                brls::TransitionAnimation::NONE)) {
            navigatingToChannel = false;
            return;
        }

        // Push on the next UI turn so the player activity and VideoView are
        // fully destroyed before the channel page becomes active.
        brls::sync([channel]() {
            brls::Application::pushActivity(
                new brls::Activity(
                    new KickChannelPage(channel)),
                brls::TransitionAnimation::NONE);
        });
    }

    void restorePlaybackFocus() {
        std::weak_ptr<std::atomic_bool> weakAlive = playerAlive;
        brls::sync([this, weakAlive]() {
            auto alive = weakAlive.lock();
            if (!alive || !alive->load()) return;
            if (!playerShuttingDown &&
                activePlaybackSession.load() == playbackSessionId)
                brls::Application::giveFocus(view);
        });
    }

    void cycleChatMode() {
        auto preferences =
            kick::loadChatPreferences();

        const char* description = "Off";
        switch (preferences.mode) {
        case kick::ChatMode::Off:
            preferences.mode =
                kick::ChatMode::Overlay;
            description = "Overlay";
            break;

        case kick::ChatMode::Overlay:
            preferences.mode =
                kick::ChatMode::RightPanel;
            description = "Right panel";
            break;

        case kick::ChatMode::RightPanel:
        default:
            preferences.mode =
                kick::ChatMode::Off;
            description = "Off";
            break;
        }

        if (!kick::saveChatPreferences(
                preferences)) {
            brls::Application::notify(
                "kickNX: could not change chat layout");
            return;
        }

        brls::Application::notify(
            std::string("Chat layout: ") +
            description);
    }

    void updateChatInteractionAction(
        const kick::ChatPreferences& preferences) {
        const bool interactive =
            preferences.participation ==
            kick::ChatParticipation::Interactive;

        if (interactive &&
            chatSendAction == ACTION_NONE) {
            chatSendAction = this->registerAction(
                "Chat",
                brls::BUTTON_LT,
                [this](brls::View*) {
                    openChatComposer();
                    return true;
                },
                false);
        } else if (!interactive &&
            chatSendAction != ACTION_NONE) {
            this->unregisterAction(chatSendAction);
            chatSendAction = ACTION_NONE;
        }
    }


    bool sendPortraitMessage(const std::string& message) {
        const auto preferences = kick::loadChatPreferences();
        if (preferences.participation !=
            kick::ChatParticipation::Interactive) {
            brls::Application::notify(
                "Enable Interactive under Chat participation first");
            return false;
        }
        if (!kick::hasChatWriteScope()) {
            brls::Application::notify(
                "Sign out and sign in again once to enable interactive chat");
            return false;
        }
        if (message.empty()) return false;

        kick::sendChatMessageAsync(
            kickChannel,
            message,
            [](const std::string&) {
                brls::Application::notify("kickNX: chat message sent");
            },
            [](const std::string& error) {
                brls::Application::notify("kickNX chat error: " + error);
            });
        return true;
    }

    void openChatComposer() {
        const auto preferences =
            kick::loadChatPreferences();

        if (preferences.participation !=
            kick::ChatParticipation::Interactive) {
            brls::Application::notify(
                "Enable Interactive under Chat participation first");
            return;
        }

        if (!kick::hasChatWriteScope()) {
            brls::Application::notify(
                "Sign out and sign in again once to enable "
                "interactive chat");
            return;
        }

        if (preferences.composerMode ==
            kick::ChatComposerMode::NintendoKeyboard) {
            auto* ime =
                brls::Application::getImeManager();
            if (!ime) {
                brls::Application::notify(
                    "The software keyboard is unavailable");
                return;
            }

            std::weak_ptr<std::atomic_bool> weakAlive = playerAlive;
            const std::string channelName = kickChannel;
            ime->openForText(
                [this, weakAlive, channelName](std::string message) {
                    // The keyboard can return after the stream was closed and
                    // this player destroyed; never touch it in that case.
                    auto alive = weakAlive.lock();
                    const bool playerExists = alive && alive->load();
                    if (playerExists) restorePlaybackFocus();

                    if (message.empty()) return;

                    brls::Application::notify(
                        "kickNX: sending chat message…");

                    kick::sendChatMessageAsync(
                        channelName,
                        message,
                        [](const std::string&) {
                            brls::Application::notify(
                                "kickNX: chat message sent");
                        },
                        [](const std::string& error) {
                            brls::Application::notify(
                                "kickNX chat error: " + error);
                        });
                },
                "Kick chat",
                "Send a message as your signed-in Kick account",
                500,
                "");
            return;
        }

        brls::Application::pushActivity(
            new brls::Activity(
                new KickChatComposer(
                    kickChannel)));
    }

    void stopChat() {
        chatClient.reset();
        if (chatUi) {
            chatUi->setStatus("Chat disabled");
            chatUi->setChatterCount(0);
        }
    }

    void startChat() {
        if (chatClient || kickChannel.empty()) return;

        std::weak_ptr<ChatUiState> weak = chatUi;
        chatClient = std::make_unique<kick::ChatClient>(
            kickChannel,
            [weak](kick::ChatMessage message) {
                brls::sync([weak, message = std::move(message)]() mutable {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->append(std::move(message));
                });
            },
            [weak](const std::string& status) {
                brls::sync([weak, status]() {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->setStatus(status);
                });
            },
            [weak](int count) {
                brls::sync([weak, count]() {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->setChatterCount(count);
                });
            });
    }

    void applyDisplayOrientation(
        kicknx::portrait::DisplayOrientation orientation) {
        portraitOrientation = orientation;
        portraitLayout =
            orientation != kicknx::portrait::DisplayOrientation::Landscape;

        brls::Application::setContentOrientation(
            orientation == kicknx::portrait::DisplayOrientation::PortraitClockwise
                ? brls::ContentOrientation::PORTRAIT_CLOCKWISE
                : orientation == kicknx::portrait::DisplayOrientation::PortraitCounterClockwise
                    ? brls::ContentOrientation::PORTRAIT_COUNTER_CLOCKWISE
                    : brls::ContentOrientation::LANDSCAPE);

        if (!portraitLayout && portraitComposer)
            portraitComposer->setEmotePanelVisible(false);

        applyChatPreferences(currentChatPreferences);

        if (!portraitLayout && !playerShuttingDown)
            brls::Application::giveFocus(view);
    }

    void applyKickVideoLayout(
        const kick::ChatPreferences& preferences) {
        const kick::ChatMode mode = preferences.mode;
        auto& mpv = MPVCore::instance();

        if (portraitLayout) {
            const float occupied = std::clamp(
                portraitVideoHeight /
                    std::max(1.0f, brls::Application::contentHeight),
                0.0f,
                1.0f);
            const float unused = 1.0f - occupied;
            const bool clockwise =
                portraitOrientation ==
                kicknx::portrait::DisplayOrientation::PortraitClockwise;

            const std::string left =
                fmt::format("{:.6f}", clockwise ? unused : 0.0f);
            const std::string right =
                fmt::format("{:.6f}", clockwise ? 0.0f : unused);

            mpv.command("set", "video-margin-ratio-left", left.c_str());
            mpv.command("set", "video-margin-ratio-right", right.c_str());
            mpv.command("set", "video-margin-ratio-top", "0.0");
            mpv.command("set", "video-margin-ratio-bottom", "0.0");
            mpv.command("set", "video-rotate", clockwise ? "90" : "270");
            mpv.command("set", "keepaspect", "yes");
            mpv.command("set", "keepaspect-window", "no");
            mpv.command("set", "video-aspect-override", "no");
            mpv.command("set", "panscan", "0.0");
            mpv.command("set", "video-zoom", "0.0");
            mpv.command("set", "video-pan-x", "0.0");
            mpv.command("set", "video-pan-y", "0.0");
            mpv.command("set", "video-align-x", "0");
            mpv.command("set", "video-align-y", "0");
            mpv.command("set", "video-recenter", "yes");
            return;
        }

        mpv.command("set", "video-rotate", configuredVideoRotation());

        // MPV renders directly into the full Switch framebuffer. Making the
        // Borealis VideoView narrower changes the OSD bounds, but does not by
        // itself constrain MPV's video rectangle. Reserve the docked chat area
        // through MPV's own window-relative video margin.
        const float contentWidth = brls::Application::contentWidth;
        const float rightMargin =
            mode == kick::ChatMode::RightPanel && contentWidth > 0.0f
                ? static_cast<float>(preferences.dockedWidth) /
                    contentWidth
                : 0.0f;

        const std::string rightMarginValue =
            fmt::format("{:.6f}", rightMargin);

        mpv.command(
            "set",
            "video-margin-ratio-left",
            "0.0");
        mpv.command(
            "set",
            "video-margin-ratio-right",
            rightMarginValue.c_str());
        mpv.command(
            "set",
            "video-margin-ratio-top",
            "0.0");
        mpv.command(
            "set",
            "video-margin-ratio-bottom",
            "0.0");

        mpv.command("set", "video-zoom", "0.0");
        mpv.command("set", "video-pan-x", "0.0");
        mpv.command("set", "video-pan-y", "0.0");
        mpv.command("set", "video-align-y", "0");
        mpv.command("set", "video-recenter", "yes");

        if (mode == kick::ChatMode::RightPanel) {
            // Fit the complete source image into the remaining area. Any
            // aspect-ratio mismatch becomes black letterboxing rather than
            // cropping the stream behind the chat panel.
            mpv.command("set", "keepaspect", "yes");
            mpv.command("set", "keepaspect-window", "no");
            mpv.command("set", "video-aspect-override", "no");
            mpv.command("set", "panscan", "0.0");
            mpv.command("set", "video-align-x", "-1");

            brls::Logger::info(
                "kickNX: docked video fit enabled, right margin ratio {}",
                rightMarginValue);
        } else {
            // Restore the user's normal player aspect choice when leaving the
            // dedicated docked layout.
            mpv.command("set", "video-align-x", "0");
            mpv.setAspect(MPVCore::VIDEO_ASPECT);

            brls::Logger::info(
                "kickNX: docked video fit disabled");
        }
    }

    void applyChatPreferences(const kick::ChatPreferences& preferences) {
        if (playerShuttingDown ||
            activePlaybackSession.load() != playbackSessionId ||
            kickChannel.empty() ||
            !chatOverlayPanel ||
            !chatDockedPanel)
            return;

        currentChatMode = preferences.mode;
        currentChatPreferences = preferences;

        chatOverlayPanel->apply(preferences);
        chatDockedPanel->apply(preferences);
        updateChatInteractionAction(preferences);

        const float width = brls::Application::contentWidth;
        const float height = brls::Application::contentHeight;

        if (portraitLayout) {
            this->setAxis(brls::Axis::COLUMN);
            this->setDimensions(width, height);
            portraitVideoHeight = std::min(width * 9.0f / 16.0f, height * 0.42f);
            const float composerHeight = std::min(485.0f, height * 0.42f);
            const float chatHeight = std::max(
                240.0f,
                height - portraitVideoHeight - composerHeight);
            view->setGrow(0.0f);
            view->setDimensions(width, portraitVideoHeight);
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::VISIBLE);
            chatDockedPanel->setPortraitFrame(width, chatHeight);
            chatUi->activate(chatDockedPanel);
            if (portraitComposer) {
                portraitComposer->setFrameSize(width, composerHeight);
                portraitComposer->setVisibility(brls::Visibility::VISIBLE);
            }
            startChat();
            applyKickVideoLayout(preferences);
            this->invalidate();
            return;
        }

        this->setAxis(brls::Axis::ROW);
        this->setDimensions(width, height);
        if (portraitComposer)
            portraitComposer->setVisibility(brls::Visibility::GONE);
        view->setGrow(1.0f);
        applyKickVideoLayout(preferences);

        switch (preferences.mode) {
        case kick::ChatMode::Off:
            chatUi->activate(nullptr);
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            view->setDimensions(width, height);
            stopChat();
            break;

        case kick::ChatMode::RightPanel:
            // TV layout: the complete video is scaled into the remaining
            // width, and chat occupies its own column rather than covering it.
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::VISIBLE);
            view->setDimensions(
                std::max(
                    640.0f,
                    width -
                        static_cast<float>(
                            preferences.dockedWidth)),
                height);
            chatUi->activate(chatDockedPanel);
            startChat();
            break;

        case kick::ChatMode::Overlay:
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            chatOverlayPanel->setVisibility(brls::Visibility::VISIBLE);
            view->setDimensions(width, height);
            chatUi->activate(chatOverlayPanel);
            startChat();
            break;
        }

        this->invalidate();
    }

private:
    VideoView* view = new VideoView();
    std::string url;
    std::string kickChannel;
    std::vector<std::string> titles;
    std::unordered_map<std::string, std::string> subtitles;
    std::vector<kick::SubtitleTrack> kickSubtitles;
    MPVEvent::Subscription eventSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    brls::VoidEvent::Subscription channelSubscribeID;
    bool channelEventSubscribed = false;
    HTTP::Cancel channelProfileCancel;
    kick::UserProfile channelProfile;
    brls::Event<kick::ChatPreferences>::Subscription chatPreferenceSubscribeID;
    std::optional<brls::Event<kicknx::portrait::DisplayOrientation>::Subscription>
        orientationSubscribeID;
    KickChatPanel* chatOverlayPanel = nullptr;
    KickChatPanel* chatDockedPanel = nullptr;
    PortraitChatComposer* portraitComposer = nullptr;
    std::shared_ptr<ChatUiState> chatUi;
    std::shared_ptr<KickPlaybackRecoveryState> playbackRecovery;
    std::unique_ptr<kick::ChatClient> chatClient;
    kick::ChatMode currentChatMode = kick::ChatMode::Off;
    kick::ChatPreferences currentChatPreferences;
    brls::ActionIdentifier chatSendAction = ACTION_NONE;
    brls::ActionIdentifier chatModeAction = ACTION_NONE;
    uint64_t playbackSessionId = 0;
    bool portraitLayout = false;
    float portraitVideoHeight = 405.0f;
    kicknx::portrait::DisplayOrientation portraitOrientation =
        kicknx::portrait::DisplayOrientation::Landscape;
    bool playerShuttingDown = false;
    // Shared with asynchronous callbacks (e.g. the Nintendo software keyboard)
    // so they can tell whether this player still exists before touching it.
    std::shared_ptr<std::atomic_bool> playerAlive =
        std::make_shared<std::atomic_bool>(true);
    bool navigatingToChannel = false;
    std::shared_ptr<std::atomic_bool> asyncCancelToken;
};

void RemoteView::play(
    const std::string& path,
    const std::string& name,
    const std::string& extra,
    const std::string& kickChannel,
    const std::string& loadingNote,
    const std::vector<kick::SubtitleTrack>& subtitles,
    int64_t uptimeSeconds) {
    // Every playback launch starts from a clean global MPV presentation. This
    // prevents a live chat margin or recovery session leaking into VODs/clips.
    MPVCore::LIVE_STREAM_RECOVERY = false;
    MPVCore::instance().stop();
    MPVCore::instance().command("set", "pause", "no");
    resetPlayerVideoLayout();

    RemotePlayer* view = new RemotePlayer(
        {remote::EntryType::VIDEO, name, path},
        kickChannel,
        loadingNote,
        subtitles,
        uptimeSeconds);
    brls::Application::pushActivity(new brls::Activity(view), brls::TransitionAnimation::NONE);
    if (!path.empty()) {
        view->setUrl(path, extra);
    }
}

void RemoteView::playLive(
    const kick::Config& config,
    int64_t uptimeSeconds,
    const std::string& streamTitle) {
    MPVCore::LIVE_STREAM_RECOVERY = true;
    MPVCore::instance().stop();
    resetPlayerVideoLayout();

    const std::string channel = config.channel;
    // Like VODs and clips, the OSD shows what is being watched.
    const std::string fullTitle = streamTitle.empty()
        ? "Kick - " + channel
        : channel + " · " + streamTitle;
    const std::string loadingNote = "";

    RemotePlayer* player = new RemotePlayer(
        {remote::EntryType::VIDEO, fullTitle, ""},
        channel,
        loadingNote,
        {},
        uptimeSeconds);
    brls::Application::pushActivity(new brls::Activity(player), brls::TransitionAnimation::NONE);

    auto cancel = std::make_shared<std::atomic_bool>(false);
    player->setPlaybackCancelToken(cancel);
    const uint64_t sessionId = player->getPlaybackSessionId();

    kick::resolveLiveAsync(
        config,
        [player, sessionId, fullTitle](kick::Resolution result) {
            if (activePlaybackSession.load() != sessionId) return;
            player->setTitle(fullTitle);
            player->setKickSubtitles(result.subtitles);
            MPVCore::BOTTOM_BAR = false;
            player->setUrl(result.selected.url, kick::mpvExtra());
        },
        [sessionId, channel](const std::string& error) {
            if (activePlaybackSession.load() != sessionId) return;
            auto* dialog = new brls::Dialog(
                "Failed to load stream for " + channel + ":\n" + error);
            dialog->addButton("hints/back"_i18n, []() {
                VideoView::close(true);
            });
            dialog->open();
        });
}

void RemoteView::playVod(
    const std::string& videoId,
    const std::string& title,
    const std::string& details) {
    MPVCore::LIVE_STREAM_RECOVERY = false;
    MPVCore::instance().stop();
    resetPlayerVideoLayout();

    const std::string fullTitle = "Kick VOD · " + title;
    const std::string loadingNote = "";

    RemotePlayer* player = new RemotePlayer(
        {remote::EntryType::VIDEO, fullTitle, ""},
        "",
        loadingNote,
        {},
        0);
    brls::Application::pushActivity(new brls::Activity(player), brls::TransitionAnimation::NONE);

    auto cancel = std::make_shared<std::atomic_bool>(false);
    player->setPlaybackCancelToken(cancel);
    const uint64_t sessionId = player->getPlaybackSessionId();

    kick::resolveVodAsync(
        videoId,
        [player, sessionId, fullTitle](kick::MediaPlayback playback) {
            if (activePlaybackSession.load() != sessionId) return;
            player->setTitle(fullTitle);
            player->setKickSubtitles(playback.subtitles);
            player->setUrl(playback.url, "");
        },
        [sessionId, details](const std::string& error) {
            if (activePlaybackSession.load() != sessionId) return;
            std::string msg = error;
            if (!details.empty()) {
                msg += "\n\n" + details;
            }
            auto* dialog = new brls::Dialog("Kick VOD playback error:\n" + msg);
            dialog->addButton("hints/back"_i18n, []() {
                VideoView::close(true);
            });
            dialog->open();
        },
        cancel);
}

void RemoteView::playClip(
    const std::string& slug,
    const std::string& title,
    const std::string& details) {
    MPVCore::LIVE_STREAM_RECOVERY = false;
    MPVCore::instance().stop();
    resetPlayerVideoLayout();

    const std::string fullTitle = "Kick clip · " + title;
    const std::string loadingNote = "";

    RemotePlayer* player = new RemotePlayer(
        {remote::EntryType::VIDEO, fullTitle, ""},
        "",
        loadingNote,
        {},
        0);
    brls::Application::pushActivity(new brls::Activity(player), brls::TransitionAnimation::NONE);

    auto cancel = std::make_shared<std::atomic_bool>(false);
    player->setPlaybackCancelToken(cancel);
    const uint64_t sessionId = player->getPlaybackSessionId();

    kick::resolveClipAsync(
        slug,
        [player, sessionId, fullTitle](kick::MediaPlayback playback) {
            if (activePlaybackSession.load() != sessionId) return;
            player->setTitle(fullTitle);
            player->setKickSubtitles(playback.subtitles);
            player->setUrl(playback.url, "");
        },
        [sessionId, details](const std::string& error) {
            if (activePlaybackSession.load() != sessionId) return;
            std::string msg = error;
            if (!details.empty()) {
                msg += "\n\n" + details;
            }
            auto* dialog = new brls::Dialog("Kick clip playback error:\n" + msg);
            dialog->addButton("hints/back"_i18n, []() {
                VideoView::close(true);
            });
            dialog->open();
        },
        cancel);
}
