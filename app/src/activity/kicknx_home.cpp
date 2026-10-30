#include "activity/kicknx_home.hpp"
#include "utils/theme.hpp"
#include "view/theme_header.hpp"
#include "activity/kick_channel.hpp"
#include "api/kick.hpp"
#include "api/kick_auth.hpp"
#include "api/kick_helix.hpp"
#include "tab/remote_view.hpp"
#include "utils/image.hpp"
#include "utils/config.hpp"
#include "view/h_recycling.hpp"
#include "view/mpv_core.hpp"
#include "view/qr_code_view.hpp"

#include <borealis/core/cache_helper.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <fmt/format.h>
#include <memory>
#include <utility>
#include <vector>

namespace {

class AboutScrollingFrame : public brls::ScrollingFrame {
public:
    void setFocusProxy(brls::View* view) { focusProxy = view; }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override {
        // ScrollingFrame normally reads the right stick only while it or one
        // of its descendants owns focus. The About page keeps focus on the
        // fixed Back button, which sits outside the scroll frame. Bridge that
        // one local focus state during drawing so the stock right-stick
        // scrolling behavior remains available without changing global/home
        // navigation.
        const bool proxyIsFocused =
            focusProxy && brls::Application::getCurrentFocus() == focusProxy;
        const bool previousChildFocused = childFocused;
        if (proxyIsFocused) childFocused = true;

        brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);

        childFocused = previousChildFocused;
    }

private:
    brls::View* focusProxy = nullptr;
};

class KickNXActionButton : public brls::Box {
public:
    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            if (auto* target = downTarget()) return target;
        }
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (auto* target = upTarget()) return target;
        }
        return brls::Box::getNextFocus(direction, currentView);
    }

    std::function<brls::View*()> downTarget;
    std::function<brls::View*()> upTarget;
};

class AvatarImageLoader : public brls::Image {
public:
    explicit AvatarImageLoader(std::function<void()> onLoaded)
        : onLoaded(std::move(onLoaded)) {}

    void innerSetImage(int texture) override {
        brls::Image::innerSetImage(texture);
        if (onLoaded) onLoaded();
    }

private:
    std::function<void()> onLoaded;
};

class KickNXProfileAvatar : public brls::View {
public:
    KickNXProfileAvatar() {
        loader = new AvatarImageLoader([this]() {
            this->invalidate();
        });
        loader->setFreeTexture(false);
        this->setFocusable(true);
        this->setCornerRadius(38);
        this->setHighlightCornerRadius(38);
        this->setHideHighlightBackground(true);
        this->setHideHighlightBorder(true);
        this->setVisibility(brls::Visibility::GONE);
        this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    ~KickNXProfileAvatar() override {
        Image::cancel(loader);
        delete loader;
    }

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            if (auto* target = downTarget()) return target;
        }
        return brls::View::getNextFocus(direction, currentView);
    }

    std::function<brls::View*()> downTarget;

    void setProfileImage(const std::string& url) {
        if (url == currentUrl) {
            this->setVisibility(
                url.empty()
                    ? brls::Visibility::GONE
                    : brls::Visibility::VISIBLE);
            return;
        }

        currentUrl = url;
        Image::cancel(loader);

        if (currentUrl.empty()) {
            this->setVisibility(brls::Visibility::GONE);
            return;
        }

        this->setVisibility(brls::Visibility::VISIBLE);
        Image::with(loader, currentUrl);
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
        const float size = std::min(width, height);
        const float centerX = x + width * 0.5f;
        const float centerY = y + height * 0.5f;
        const float radius = size * 0.5f - 2.0f;

        nvgSave(vg);

        // Kick-toned placeholder while the image is downloading.
        nvgBeginPath(vg);
        nvgCircle(vg, centerX, centerY, radius);
        nvgFillColor(vg, kicknx::theme::current().button);
        nvgFill(vg);

        if (loader && loader->getTexture() > 0) {
            const float imageX = centerX - radius;
            const float imageY = centerY - radius;
            const float imageSize = radius * 2.0f;

            NVGpaint paint = nvgImagePattern(
                vg,
                imageX,
                imageY,
                imageSize,
                imageSize,
                0,
                loader->getTexture(),
                1.0f);

            nvgBeginPath(vg);
            nvgCircle(vg, centerX, centerY, radius);
            nvgFillPaint(vg, paint);
            nvgFill(vg);
        }

        const bool focused = isFocused();
        nvgBeginPath(vg);
        nvgCircle(vg, centerX, centerY, radius);
        if (focused) {
            // Bright Kick green ring on hover/focus (#53FC18)
            nvgStrokeColor(vg, kicknx::theme::current().accent);
            nvgStrokeWidth(vg, 4.0f);
        } else {
            nvgStrokeColor(vg, kicknx::theme::current().outline);
            nvgStrokeWidth(vg, 2.0f);
        }
        nvgStroke(vg);

        nvgRestore(vg);
    }

private:
    brls::Image* loader = nullptr;
    std::string currentUrl;
};

brls::Box* makeButton(const std::string& text, brls::Label** outLabel = nullptr) {
    auto* button = new KickNXActionButton();
    button->setFocusable(true);
    button->setAxis(brls::Axis::ROW);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(kicknx::theme::current().button);
    button->setHideHighlightBackground(kicknx::theme::current().keepButtonFillOnFocus);
    button->setMarginRight(12);
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(19);
    label->setTextColor(kicknx::theme::current().buttonText);
    button->addView(label);
    if (outLabel) *outLabel = label;
    return button;
}

std::string viewersText(int viewers) {
    if (viewers >= 1000000) return fmt::format("{:.1f}M viewers", viewers / 1000000.0);
    if (viewers >= 1000) return fmt::format("{:.1f}K viewers", viewers / 1000.0);
    return std::to_string(viewers) + " viewers";
}

std::string followersText(int followers) {
    if (followers >= 1000000) return fmt::format("{:.1f}M followers", followers / 1000000.0);
    if (followers >= 1000) return fmt::format("{:.1f}K followers", followers / 1000.0);
    return std::to_string(followers) + " followers";
}

std::string streamKey(const kick::Stream& item) {
    if (!item.userLogin.empty()) return item.userLogin;
    if (!item.userId.empty()) return item.userId;
    return item.id;
}

std::string categoryKey(const kick::Category& item) {
    return item.id.empty() ? item.name : item.id;
}

std::string eventKey(const kick::LiveEvent& item) {
    return item.id;
}

std::string eventSubtitle(const kick::LiveEvent& item) {
    std::string detail = item.categoryName;
    if (item.liveStreamCount > 0) {
        if (!detail.empty()) detail += " \u00b7 ";
        detail += std::to_string(item.liveStreamCount) +
            (item.liveStreamCount == 1 ? " live stream" : " live streams");
    }
    return detail.empty() ? std::string("Kick event") : detail;
}

std::string clipKey(const kick::ChannelClip& item) {
    return item.id;
}

std::string viewsText(int views) {
    if (views >= 1000000) return fmt::format("{:.1f}M views", views / 1000000.0);
    if (views >= 1000) return fmt::format("{:.1f}K views", views / 1000.0);
    return std::to_string(views) + " views";
}

std::string clipDurationText(float seconds) {
    const int total = static_cast<int>(seconds + 0.5f);
    return fmt::format("{}:{:02}", total / 60, total % 60);
}

std::string joinErrors(const kick::HomeData& data) {
    std::vector<std::string> errors;
    if (!data.followedError.empty()) errors.push_back("followed channels");
    if (!data.offlineFollowedError.empty()) errors.push_back("offline followed channels");
    if (!data.popularError.empty()) errors.push_back("popular streams");
    if (!data.popularClipsError.empty()) errors.push_back("popular clips");
    if (!data.categoriesError.empty()) errors.push_back("categories");

    std::string result;
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index) result += index + 1 == errors.size() ? " and " : ", ";
        result += errors[index];
    }
    return result;
}

class StreamCell : public RecyclingGridItem {
public:
    StreamCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(kicknx::theme::current().surface);
        this->setPadding(0, 0, 10, 0);

        picture = new brls::Image();
        picture->setHeight(142);
        picture->setWidthPercentage(100);
        picture->setImageFromRes("img/video-card-bg.png");
        this->addView(picture);

        title = new brls::Label();
        title->setFontSize(18);
        title->setSingleLine(true);
        title->setMarginTop(8);
        title->setMarginLeft(10);
        title->setMarginRight(10);
        title->setTextColor(kicknx::theme::current().textPrimary);
        this->addView(title);

        subtitle = new brls::Label();
        subtitle->setFontSize(15);
        subtitle->setSingleLine(true);
        subtitle->setMarginTop(3);
        subtitle->setMarginLeft(10);
        subtitle->setMarginRight(10);
        subtitle->setTextColor(kicknx::theme::current().textMuted);
        this->addView(subtitle);

        this->getFocusEvent()->subscribe([this](brls::View*) {
            if (onFocused) onFocused();
        });
    }

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (auto* target = upTarget()) return target;
        }
        return RecyclingGridItem::getNextFocus(direction, currentView);
    }

    ~StreamCell() override { Image::cancel(picture); }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setImageFromRes("img/video-card-bg.png");
        title->setText("");
        subtitle->setText("");
        onFocused = nullptr;
        upTarget = nullptr;
    }

    void cacheForReuse() override {
        Image::cancel(picture);
        onFocused = nullptr;
        upTarget = nullptr;
    }

    brls::Image* picture = nullptr;
    brls::Label* title = nullptr;
    brls::Label* subtitle = nullptr;
    std::function<void()> onFocused;
    std::function<brls::View*()> upTarget;
};

class CategoryCell : public RecyclingGridItem {
public:
    CategoryCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(kicknx::theme::current().surface);
        this->setPadding(0, 0, 10, 0);

        picture = new brls::Image();
        picture->setHeight(205);
        picture->setWidthPercentage(100);
        picture->setScalingType(brls::ImageScalingType::FILL);
        picture->setImageFromRes("img/category-card-bg.png");
        this->addView(picture);

        title = new brls::Label();
        title->setFontSize(17);
        title->setSingleLine(true);
        title->setMarginTop(8);
        title->setMarginLeft(8);
        title->setMarginRight(8);
        title->setTextColor(kicknx::theme::current().textPrimary);
        this->addView(title);

        this->getFocusEvent()->subscribe([this](brls::View*) {
            if (onFocused) onFocused();
        });
    }

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (auto* target = upTarget()) return target;
        }
        return RecyclingGridItem::getNextFocus(direction, currentView);
    }

    ~CategoryCell() override { Image::cancel(picture); }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setScalingType(brls::ImageScalingType::FILL);
        picture->setImageFromRes("img/category-card-bg.png");
        title->setText("");
        onFocused = nullptr;
        upTarget = nullptr;
    }

    void cacheForReuse() override {
        Image::cancel(picture);
        onFocused = nullptr;
        upTarget = nullptr;
    }

    brls::Image* picture = nullptr;
    brls::Label* title = nullptr;
    std::function<void()> onFocused;
    std::function<brls::View*()> upTarget;
};

class StreamDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const kick::Stream&)>;
    using Focus = std::function<void(const kick::Stream&, size_t)>;

    StreamDataSource(
        std::vector<kick::Stream> data,
        Selection selected,
        Focus focused,
        std::function<brls::View*()> up)
        : items(std::move(data)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(up) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Stream"));
        const auto& item = items.at(index);
        cell->upTarget = upTarget;
        cell->title->setText(item.userName.empty() ? item.userLogin : item.userName);
        std::string detail = item.gameName;
        if (item.isLive) {
            if (!detail.empty()) detail += " · ";
            detail += viewersText(item.viewerCount);
        } else {
            if (item.followersCount > 0) {
                if (!detail.empty()) detail += " · ";
                detail += followersText(item.followersCount);
            } else {
                if (!detail.empty()) detail += " · ";
                detail += "Offline";
            }
        }
        cell->subtitle->setText(detail);
        if (!item.thumbnailUrl.empty()) {
            Image::with(cell->picture, kick::streamThumbnail(item.thumbnailUrl));
        }

        const kick::Stream focusedItem = item;
        const Focus focusedCallback = onFocused;
        cell->onFocused = [focusedItem, index, focusedCallback]() {
            if (focusedCallback) focusedCallback(focusedItem, index);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    size_t append(std::vector<kick::Stream> incoming) {
        size_t added = 0;
        for (auto& item : incoming) {
            const std::string key = streamKey(item);
            const auto duplicate = std::find_if(items.begin(), items.end(), [&](const kick::Stream& current) {
                return streamKey(current) == key;
            });
            if (duplicate != items.end()) continue;
            items.push_back(std::move(item));
            ++added;
        }
        return added;
    }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (streamKey(items[index]) == key) return index;
        }
        return items.size();
    }

private:
    std::vector<kick::Stream> items;
    Selection onSelected;
    Focus onFocused;
    std::function<brls::View*()> upTarget;
};

// Clips reuse StreamCell (thumbnail, title, subtitle); only the data differs.
class ClipDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const kick::ChannelClip&)>;
    using Focus = std::function<void(const kick::ChannelClip&, size_t)>;

    ClipDataSource(
        std::vector<kick::ChannelClip> data,
        Selection selected,
        Focus focused,
        std::function<brls::View*()> up)
        : items(std::move(data)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(up) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Clip"));
        const auto& item = items.at(index);
        cell->upTarget = upTarget;
        cell->title->setText(item.title);

        std::string detail = item.channelName.empty() ? item.creatorName : item.channelName;
        if (!detail.empty()) detail += " · ";
        detail += viewsText(item.viewCount);
        if (item.duration > 0.0f) detail += " · " + clipDurationText(item.duration);
        cell->subtitle->setText(detail);
        if (!item.thumbnailUrl.empty()) Image::with(cell->picture, item.thumbnailUrl);

        const kick::ChannelClip focusedItem = item;
        const Focus focusedCallback = onFocused;
        cell->onFocused = [focusedItem, index, focusedCallback]() {
            if (focusedCallback) focusedCallback(focusedItem, index);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    size_t append(std::vector<kick::ChannelClip> incoming) {
        size_t added = 0;
        for (auto& item : incoming) {
            const std::string key = clipKey(item);
            const auto duplicate = std::find_if(items.begin(), items.end(), [&](const kick::ChannelClip& current) {
                return clipKey(current) == key;
            });
            if (duplicate != items.end()) continue;
            items.push_back(std::move(item));
            ++added;
        }
        return added;
    }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (clipKey(items[index]) == key) return index;
        }
        return items.size();
    }

private:
    std::vector<kick::ChannelClip> items;
    Selection onSelected;
    Focus onFocused;
    std::function<brls::View*()> upTarget;
};

// Events reuse StreamCell: event thumbnail, title, and category/stream count.
class EventDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const kick::LiveEvent&)>;
    using Focus = std::function<void(const kick::LiveEvent&, size_t)>;

    EventDataSource(
        std::vector<kick::LiveEvent> data,
        Selection selected,
        Focus focused,
        std::function<brls::View*()> up)
        : items(std::move(data)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(up) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<StreamCell*>(recycler->dequeueReusableCell("Event"));
        const auto& item = items.at(index);
        cell->upTarget = upTarget;
        cell->title->setText(item.title);
        cell->subtitle->setText(eventSubtitle(item));
        if (!item.thumbnailUrl.empty()) Image::with(cell->picture, item.thumbnailUrl);

        const kick::LiveEvent focusedItem = item;
        const Focus focusedCallback = onFocused;
        cell->onFocused = [focusedItem, index, focusedCallback]() {
            if (focusedCallback) focusedCallback(focusedItem, index);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (eventKey(items[index]) == key) return index;
        }
        return items.size();
    }

private:
    std::vector<kick::LiveEvent> items;
    Selection onSelected;
    Focus onFocused;
    std::function<brls::View*()> upTarget;
};

class CategoryDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const kick::Category&)>;
    using Focus = std::function<void(const kick::Category&, size_t)>;

    CategoryDataSource(
        std::vector<kick::Category> data,
        Selection selected,
        Focus focused,
        std::function<brls::View*()> up)
        : items(std::move(data)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(up) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<CategoryCell*>(recycler->dequeueReusableCell("Category"));
        const auto& item = items.at(index);
        cell->upTarget = upTarget;
        cell->title->setText(item.name);
        if (!item.boxArtUrl.empty()) {
            cell->picture->setScalingType(brls::ImageScalingType::FIT);
            Image::with(cell->picture, kick::categoryThumbnail(item.boxArtUrl));
        } else {
            cell->picture->setScalingType(brls::ImageScalingType::FILL);
            cell->picture->setImageFromRes("img/category-card-bg.png");
        }

        const kick::Category focusedItem = item;
        const Focus focusedCallback = onFocused;
        cell->onFocused = [focusedItem, index, focusedCallback]() {
            if (focusedCallback) focusedCallback(focusedItem, index);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    size_t append(std::vector<kick::Category> incoming) {
        size_t added = 0;
        for (auto& item : incoming) {
            const std::string key = categoryKey(item);
            const auto duplicate = std::find_if(items.begin(), items.end(), [&](const kick::Category& current) {
                return categoryKey(current) == key;
            });
            if (duplicate != items.end()) continue;
            items.push_back(std::move(item));
            ++added;
        }
        return added;
    }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (categoryKey(items[index]) == key) return index;
        }
        return items.size();
    }

private:
    std::vector<kick::Category> items;
    Selection onSelected;
    Focus onFocused;
    std::function<brls::View*()> upTarget;
};

}  // namespace

class KickRow : public brls::Box {
public:
    enum class Kind { Streams, Categories, Clips, Events };
    using FocusCallback = std::function<void(const std::string&, const std::string&)>;
    using StreamFocusCallback = std::function<void(const kick::Stream&)>;

    KickRow(std::string identifier, const std::string& text, Kind rowKind, FocusCallback focus)
        : id(std::move(identifier)), kind(rowKind), focusCallback(std::move(focus)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setMarginBottom(18);

        header = new ThemeHeader();
        header->setTitle(text);
        header->setHeight(38);
        this->addView(header);

        recycler = new HRecyclerFrame();
        const bool cardRow = kind != Kind::Categories;
        recycler->setHeight(cardRow ? 225 : 285);
        recycler->estimatedRowWidth = cardRow ? 315 : 175;
        recycler->estimatedRowSpace = 18;
        recycler->setPadding(0, 4, 0, 4);
        if (kind == Kind::Streams) {
            recycler->registerCell("Stream", []() { return new StreamCell(); });
        } else if (kind == Kind::Clips) {
            recycler->registerCell("Clip", []() { return new StreamCell(); });
        } else if (kind == Kind::Events) {
            recycler->registerCell("Event", []() { return new StreamCell(); });
        } else {
            recycler->registerCell("Category", []() { return new CategoryCell(); });
        }
        this->addView(recycler);

        emptyBox = new brls::Box();
        emptyBox->setAxis(brls::Axis::ROW);
        emptyBox->setHeight(70);
        emptyBox->setPadding(14, 18, 14, 18);
        emptyBox->setCornerRadius(8);
        emptyBox->setBackgroundColor(kicknx::theme::current().surfaceAlt);
        emptyLabel = new brls::Label();
        emptyLabel->setFontSize(17);
        emptyLabel->setTextColor(kicknx::theme::current().textMuted);
        emptyBox->addView(emptyLabel);
        emptyBox->setVisibility(brls::Visibility::GONE);
        this->addView(emptyBox);

        this->setVisibility(brls::Visibility::GONE);
    }

    void setTitle(const std::string& text) { header->setTitle(text); }
    void onStreamFocused(StreamFocusCallback callback) { streamFocusCallback = std::move(callback); }

    void setStreams(
        std::vector<kick::Stream> items,
        std::function<void(const kick::Stream&)> selected,
        const std::string& emptyText,
        std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        streamSource = new StreamDataSource(std::move(items), std::move(selected),
            [this](const kick::Stream& stream, size_t) {
                if (focusCallback) focusCallback(id, streamKey(stream));
                if (streamFocusCallback) streamFocusCallback(stream);
            }, upTarget);
        categorySource = nullptr;
        clipSource = nullptr;
        eventSource = nullptr;
        recycler->setDataSource(streamSource);
        showResult(count > 0, emptyText);
    }

    void setClips(
        std::vector<kick::ChannelClip> items,
        std::function<void(const kick::ChannelClip&)> selected,
        const std::string& emptyText,
        std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        clipSource = new ClipDataSource(std::move(items), std::move(selected),
            [this](const kick::ChannelClip& clip, size_t) {
                if (focusCallback) focusCallback(id, clipKey(clip));
            }, upTarget);
        streamSource = nullptr;
        categorySource = nullptr;
        eventSource = nullptr;
        recycler->setDataSource(clipSource);
        showResult(count > 0, emptyText);
    }

    void setEvents(
        std::vector<kick::LiveEvent> items,
        std::function<void(const kick::LiveEvent&)> selected,
        const std::string& emptyText,
        std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        eventSource = new EventDataSource(std::move(items), std::move(selected),
            [this](const kick::LiveEvent& event, size_t) {
                if (focusCallback) focusCallback(id, eventKey(event));
            }, upTarget);
        streamSource = nullptr;
        categorySource = nullptr;
        clipSource = nullptr;
        recycler->setDataSource(eventSource);
        showResult(count > 0, emptyText);
    }

    size_t appendClips(std::vector<kick::ChannelClip> items) {
        if (!clipSource) return 0;
        const size_t added = clipSource->append(std::move(items));
        count += added;
        if (added) recycler->notifyDataChanged();
        updateSubtitle();
        return added;
    }

    void setCategories(
        std::vector<kick::Category> items,
        std::function<void(const kick::Category&)> selected,
        const std::string& emptyText,
        std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        categorySource = new CategoryDataSource(std::move(items), std::move(selected),
            [this](const kick::Category& category, size_t) {
                if (focusCallback) focusCallback(id, categoryKey(category));
            }, upTarget);
        streamSource = nullptr;
        clipSource = nullptr;
        eventSource = nullptr;
        recycler->setDataSource(categorySource);
        showResult(count > 0, emptyText);
    }

    size_t appendStreams(std::vector<kick::Stream> items) {
        if (!streamSource) return 0;
        const size_t added = streamSource->append(std::move(items));
        count += added;
        if (added) recycler->notifyDataChanged();
        updateSubtitle();
        return added;
    }

    size_t appendCategories(std::vector<kick::Category> items) {
        if (!categorySource) return 0;
        const size_t added = categorySource->append(std::move(items));
        count += added;
        if (added) recycler->notifyDataChanged();
        updateSubtitle();
        return added;
    }

    void setHasMore(bool value) {
        hasMore = value;
        updateSubtitle();
    }

    void onNextPage(bool enabled, const std::function<void()>& callback) {
        recycler->onNextPage(enabled ? callback : std::function<void()>{});
    }

    void clear() {
        this->setVisibility(brls::Visibility::GONE);
        recycler->onNextPage(std::function<void()>{});
        recycler->clearData();
        streamSource = nullptr;
        categorySource = nullptr;
        clipSource = nullptr;
        eventSource = nullptr;
        count = 0;
        hasMore = false;
    }

    brls::View* defaultFocus() { return recycler->getDefaultFocus(); }

    bool focusKey(const std::string& key) {
        size_t index = count;
        if (streamSource) index = streamSource->find(key);
        if (categorySource) index = categorySource->find(key);
        if (clipSource) index = clipSource->find(key);
        if (eventSource) index = eventSource->find(key);
        if (index >= count) return false;
        recycler->selectRowAt(index, false);
        if (auto* focus = recycler->getDefaultFocus()) {
            brls::Application::giveFocus(focus);
            return true;
        }
        return false;
    }

private:
    void showResult(bool hasItems, const std::string& emptyText) {
        this->setVisibility(brls::Visibility::VISIBLE);
        recycler->setVisibility(hasItems ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        emptyLabel->setText(emptyText);
        emptyBox->setVisibility(hasItems ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        updateSubtitle();
    }

    void updateSubtitle() {
        header->setSubtitle(std::to_string(count) + (hasMore ? "+" : ""));
    }

    std::string id;
    Kind kind;
    FocusCallback focusCallback;
    StreamFocusCallback streamFocusCallback;
    ThemeHeader* header = nullptr;
    HRecyclerFrame* recycler = nullptr;
    brls::Box* emptyBox = nullptr;
    brls::Label* emptyLabel = nullptr;
    StreamDataSource* streamSource = nullptr;
    CategoryDataSource* categorySource = nullptr;
    ClipDataSource* clipSource = nullptr;
    EventDataSource* eventSource = nullptr;
    size_t count = 0;
    bool hasMore = false;
};

KickNXHome::KickNXHome() {
    instance = this;
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(kicknx::theme::current().background);

    // Borealis already uses an LRU texture cache. Tighten its absolute runtime
    // cap for this thumbnail-heavy client so stale channel/category artwork is
    // evicted instead of accumulating throughout a long browsing session.
    brls::LRUCache<std::string, size_t>::DEFAULT_CAPACITY = 0;
    brls::TextureCache::instance().cache.setCapacity(192);

    auto* top = new brls::Box();
    top->setAxis(brls::Axis::COLUMN);
    top->setWidthPercentage(100);
    top->setBackgroundColor(kicknx::theme::current().header);
    top->setPadding(24, 48, 12, 48);
    kicknx::theme::decorateHeader(top);
    this->addView(top);

    auto* headingRow = new brls::Box();
    headingRow->setAxis(brls::Axis::ROW);
    headingRow->setAlignItems(brls::AlignItems::FLEX_START);
    headingRow->setWidthPercentage(100);
    top->addView(headingRow);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    headingRow->addView(headingText);

    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    headingText->addView(titleRow);

    auto* titleLogo = new brls::Image();
    titleLogo->setImageFromRes("img/kicknx_logo.png");
    titleLogo->setScalingType(brls::ImageScalingType::FIT);
    titleLogo->setHeight(44);
    titleLogo->setWidth(44.0f * 644.0f / 186.0f);
    titleRow->addView(titleLogo);

    auto* title = new brls::Label();
    title->setText("· Kick for Nintendo Switch");
    title->setFontSize(33);
    title->setTextColor(kicknx::theme::current().headerAccent);
    title->setMarginLeft(14);
    // Keep the title on one line: during abrupt screen transitions the label
    // can be laid out while its parent is momentarily narrow, which wrapped
    // "Switch" onto a second line.
    title->setSingleLine(true);
    title->setShrink(0.0f);
    titleRow->addView(title);

    accountLabel = new brls::Label();
    accountLabel->setFontSize(18);
    accountLabel->setTextColor(kicknx::theme::current().headerTextSecondary);
    accountLabel->setMarginTop(5);
    headingText->addView(accountLabel);

    profileAvatar = new KickNXProfileAvatar();
    profileAvatar->setWidth(104);
    profileAvatar->setHeight(104);
    profileAvatar->setMarginLeft(24);
    profileAvatar->registerClickAction([this](brls::View*) {
        const auto session = kick::loadOAuth();
        if (session.signedIn() && !session.login.empty()) {
            brls::Application::pushActivity(
                new brls::Activity(
                    new KickChannelPage(session.login)));
            return true;
        }
        return false;
    });
    headingRow->addView(profileAvatar);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(17);
    statusLabel->setTextColor(kicknx::theme::current().headerTextMuted);
    statusLabel->setMarginTop(6);
    statusLabel->setMarginBottom(14);
    top->addView(statusLabel);

    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    top->addView(actions);

    loginButton = makeButton("Sign in", &loginButtonLabel);
    loginButton->registerClickAction([this](brls::View*) {
        if (kick::loadOAuth().signedIn()) logout(); else beginLogin();
        return true;
    });
    actions->addView(loginButton);

    searchButton = makeButton("Search");
    searchButton->registerClickAction([this](brls::View*) {
        search();
        return true;
    });
    actions->addView(searchButton);

    refreshButton = makeButton("Refresh");
    refreshButton->registerClickAction([this](brls::View*) {
        refreshHome();
        return true;
    });
    actions->addView(refreshButton);

    aboutButton = makeButton("About");
    aboutButton->registerClickAction([this](brls::View*) {
        showAbout();
        return true;
    });
    actions->addView(aboutButton);

    themeButton = makeButton("Theme");
    themeButton->registerClickAction([this](brls::View*) {
        showThemePicker();
        return true;
    });
    actions->addView(themeButton);

    auto* avatar = static_cast<KickNXProfileAvatar*>(profileAvatar);
    avatar->downTarget = [this]() -> brls::View* {
        return aboutButton ? aboutButton : loginButton;
    };
    static_cast<KickNXActionButton*>(themeButton)->upTarget = [this]() -> brls::View* {
        return (profileAvatar && profileAvatar->getVisibility() == brls::Visibility::VISIBLE) ? profileAvatar : nullptr;
    };
    static_cast<KickNXActionButton*>(aboutButton)->upTarget = [this]() -> brls::View* {
        return (profileAvatar && profileAvatar->getVisibility() == brls::Visibility::VISIBLE) ? profileAvatar : nullptr;
    };
    static_cast<KickNXActionButton*>(refreshButton)->upTarget = [this]() -> brls::View* {
        return (profileAvatar && profileAvatar->getVisibility() == brls::Visibility::VISIBLE) ? profileAvatar : nullptr;
    };
    static_cast<KickNXActionButton*>(searchButton)->upTarget = [this]() -> brls::View* {
        return (profileAvatar && profileAvatar->getVisibility() == brls::Visibility::VISIBLE) ? profileAvatar : nullptr;
    };
    static_cast<KickNXActionButton*>(loginButton)->upTarget = [this]() -> brls::View* {
        return (profileAvatar && profileAvatar->getVisibility() == brls::Visibility::VISIBLE) ? profileAvatar : nullptr;
    };

    errorBox = new brls::Box();
    errorBox->setAxis(brls::Axis::ROW);
    errorBox->setAlignItems(brls::AlignItems::CENTER);
    errorBox->setPadding(12, 16, 12, 16);
    errorBox->setMarginTop(12);
    errorBox->setCornerRadius(8);
    errorBox->setBackgroundColor(kicknx::theme::current().errorBackground);
    errorBox->setVisibility(brls::Visibility::GONE);

    errorLabel = new brls::Label();
    errorLabel->setGrow(1.0f);
    errorLabel->setFontSize(16);
    errorLabel->setTextColor(kicknx::theme::current().errorText);
    errorBox->addView(errorLabel);

    retryButton = makeButton("Retry");
    retryButton->setMarginRight(0);
    retryButton->registerClickAction([this](brls::View*) {
        if (retryAction) retryAction();
        return true;
    });
    errorBox->addView(retryButton);
    top->addView(errorBox);

    homeScroll = new brls::ScrollingFrame();
    auto* scroll = homeScroll;
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusable(false);
    scroll->setGrow(1.0f);
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(4, 48, 26, 48);
    scroll->setContentView(content);
    this->addView(scroll);

    heroBox = new brls::Box();
    heroBox->setAxis(brls::Axis::ROW);
    heroBox->setHeight(278);
    heroBox->setCornerRadius(12);
    heroBox->setBackgroundColor(kicknx::theme::current().surfaceAlt);
    heroBox->setMarginBottom(20);
    heroBox->setVisibility(brls::Visibility::GONE);

    auto* heroInfo = new brls::Box();
    heroInfo->setAxis(brls::Axis::COLUMN);
    heroInfo->setWidthPercentage(44);
    heroInfo->setHeightPercentage(100);
    heroInfo->setPadding(24, 26, 24, 26);
    heroInfo->setBackgroundColor(kicknx::theme::current().heroOverlay);
    heroBox->addView(heroInfo);

    heroLiveLabel = new brls::Label();
    heroLiveLabel->setFontSize(16);
    heroLiveLabel->setTextColor(kicknx::theme::current().accent);
    heroLiveLabel->setMarginBottom(8);
    heroInfo->addView(heroLiveLabel);

    heroChannelLabel = new brls::Label();
    heroChannelLabel->setFontSize(27);
    heroChannelLabel->setTextColor(kicknx::theme::current().textPrimary);
    heroChannelLabel->setSingleLine(true);
    heroChannelLabel->setMarginBottom(8);
    heroInfo->addView(heroChannelLabel);

    heroTitleLabel = new brls::Label();
    heroTitleLabel->setFontSize(20);
    heroTitleLabel->setTextColor(kicknx::theme::current().textPrimary);
    heroTitleLabel->setMarginBottom(12);
    heroInfo->addView(heroTitleLabel);

    heroMetaLabel = new brls::Label();
    heroMetaLabel->setFontSize(17);
    heroMetaLabel->setTextColor(kicknx::theme::current().textMuted);
    heroInfo->addView(heroMetaLabel);

    heroPreview = new brls::Image();
    heroPreview->setGrow(1.0f);
    heroPreview->setHeightPercentage(100);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    heroBox->addView(heroPreview);

    content->addView(heroBox);

    auto remember = [this](const std::string& row, const std::string& key) {
        rememberFocus(row, key);
    };
    followedRow = new KickRow("followed", "Followed channels", KickRow::Kind::Streams, remember);
    offlineFollowedRow = new KickRow("offline-followed", "Followed channels — offline", KickRow::Kind::Streams, remember);
    popularRow = new KickRow("popular", "Popular live streams", KickRow::Kind::Streams, remember);
    popularClipsRow = new KickRow("popular-clips", "Popular clips", KickRow::Kind::Clips, remember);
    eventsRow = new KickRow("events", "Live events", KickRow::Kind::Events, remember);
    categoryStreamsRow = new KickRow("category-streams", "Category streams", KickRow::Kind::Streams, remember);
    categoriesRow = new KickRow("categories", "Top categories", KickRow::Kind::Categories, remember);
    searchRow = new KickRow("search", "Search results", KickRow::Kind::Streams, remember);

    // The hero is the visual context for the followed-channel row. Keep the
    // page pinned to its absolute top while moving horizontally between those
    // cards, so changing the selected channel never crops the hero banner.
    auto updateFollowedStream = [this](const kick::Stream& stream) {
        updateHero(stream);

        if (homeScroll) {
            // The first stream row belongs visually to the hero at the top of
            // the page. CENTERED scrolling moves that row underneath the fixed
            // account/actions bar; the content is clipped, but Borealis draws
            // its focus border globally, making the cyan cursor overlap the
            // bar. Disable automatic centering while this row owns focus and
            // pin the page to the true top instead.
            homeScroll->setScrollingBehavior(
                brls::ScrollingBehavior::NATURAL);
            homeScroll->setContentOffsetY(0.0f, false);
        }

        // Focus propagation can finish later in the same UI pass. Reassert the
        // top offset after it completes so an already-started animation cannot
        // leave the highlight partially behind the fixed header.
        brls::sync([this]() {
            if (homeScroll)
                homeScroll->setContentOffsetY(0.0f, false);
        });
    };

    auto updateFocusedStream =
        [this](const kick::Stream& stream) {
            if (homeScroll)
                homeScroll->setScrollingBehavior(
                    brls::ScrollingBehavior::CENTERED);
            updateHero(stream);
        };

    followedRow->onStreamFocused(updateFollowedStream);
    offlineFollowedRow->onStreamFocused(updateFocusedStream);
    popularRow->onStreamFocused(updateFocusedStream);
    categoryStreamsRow->onStreamFocused(updateFocusedStream);
    searchRow->onStreamFocused(updateFocusedStream);

    content->addView(followedRow);
    content->addView(popularRow);
    content->addView(popularClipsRow);
    content->addView(eventsRow);
    content->addView(categoryStreamsRow);
    content->addView(categoriesRow);
    content->addView(searchRow);
    content->addView(offlineFollowedRow);

    auto firstVisibleContentFocus = [this]() -> brls::View* {
        for (auto* row : {
                 followedRow,
                 popularRow,
                 popularClipsRow,
                 eventsRow,
                 categoryStreamsRow,
                 categoriesRow,
                 searchRow,
                 offlineFollowedRow}) {
            if (auto* focus = row->defaultFocus()) return focus;
        }
        return nullptr;
    };

    for (auto* button : {loginButton, searchButton, refreshButton, aboutButton, themeButton}) {
        if (auto* actionButton = dynamic_cast<KickNXActionButton*>(button))
            actionButton->downTarget = firstVisibleContentFocus;
    }

    this->registerAction("Search", brls::BUTTON_Y, [this](brls::View*) {
        search();
        return true;
    });
    this->registerAction("Refresh", brls::BUTTON_X, [this](brls::View*) {
        refreshHome();
        return true;
    });
    this->registerAction("Configured channel", brls::BUTTON_RB, [this](brls::View*) {
        playConfiguredChannel();
        return true;
    });

    updateAccount();
    refreshHome();
    brls::sync([this]() {
        if (isActiveActivity()) brls::Application::giveFocus(refreshButton);
    });
}

KickNXHome::~KickNXHome() {
    if (instance == this) instance = nullptr;
    if (activeRequest) activeRequest->store(true);
    if (heroPreview) Image::cancel(heroPreview);
}

void KickNXHome::updateHero(const kick::Stream& stream) {
    if (!heroBox || !heroPreview) return;

    Image::cancel(heroPreview);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    if (!stream.thumbnailUrl.empty())
        Image::with(heroPreview, kick::streamThumbnail(stream.thumbnailUrl, 820, 461));

    const std::string channel = stream.userName.empty() ? stream.userLogin : stream.userName;
    heroLiveLabel->setText(stream.isLive ? "LIVE · " + viewersText(stream.viewerCount)
                                         : (stream.followersCount > 0 ? "OFFLINE · " + followersText(stream.followersCount)
                                                                      : "OFFLINE"));
    heroChannelLabel->setText(channel.empty() ? "Kick" : channel);
    heroTitleLabel->setText(stream.title.empty() ? "Live on Kick" : stream.title);

    std::string meta = stream.gameName;
    if (!stream.language.empty()) {
        if (!meta.empty()) meta += " · ";
        meta += stream.language;
    }
    heroMetaLabel->setText(meta.empty() ? "Select a stream to watch" : meta);
    heroBox->setVisibility(brls::Visibility::VISIBLE);
}

void KickNXHome::clearHero() {
    if (!heroBox || !heroPreview) return;
    Image::cancel(heroPreview);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    heroBox->setVisibility(brls::Visibility::GONE);
}

void KickNXHome::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (!busy && statusLabel) {
        statusLabel->setText("");
    }
    if (kick::isFollowedChannelsDirty()) {
        kick::clearFollowedChannelsDirty();
        refreshHome();
    }
    // A theme chosen while another screen was on top is applied on return.
    if (themeRebuildPending)
        brls::delay(THEME_REBUILD_DELAY_MS, []() { KickNXHome::rebuildForTheme(); });
}

namespace {

// Full-screen dimmed backdrop. Translucent, so Borealis keeps drawing Home
// underneath and the picker reads as a pop-up rather than a new page.
class ThemePickerOverlay : public brls::Box {
public:
    bool isTranslucent() override { return true; }
};

// A miniature Home drawn in a preset's own colours, so each card previews the
// theme it applies rather than the one currently active.
brls::Box* makeThemePreview(const kicknx::theme::Palette& preset) {
    auto* preview = new brls::Box();
    preview->setAxis(brls::Axis::COLUMN);
    preview->setWidthPercentage(100);
    preview->setHeight(150);
    preview->setCornerRadius(8);
    preview->setBackgroundColor(preset.background);

    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::ROW);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setHeight(40);
    bar->setPadding(0, 14, 0, 14);
    bar->setCornerRadius(8);
    bar->setBackgroundColor(preset.header);
    kicknx::theme::decorateHeader(bar, preset);
    preview->addView(bar);

    auto* logo = new brls::Label();
    logo->setText("kickNX");
    logo->setFontSize(17);
    logo->setTextColor(preset.headerAccent);
    bar->addView(logo);

    auto* cards = new brls::Box();
    cards->setAxis(brls::Axis::ROW);
    cards->setPadding(14, 14, 14, 14);
    cards->setGrow(1.0f);
    preview->addView(cards);

    for (int index = 0; index < 2; ++index) {
        auto* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setGrow(1.0f);
        card->setCornerRadius(6);
        card->setPadding(10, 10, 10, 10);
        card->setBackgroundColor(preset.surface);
        if (index == 0) card->setMarginRight(10);
        cards->addView(card);

        auto* thumb = new brls::Box();
        thumb->setHeight(36);
        thumb->setCornerRadius(4);
        thumb->setBackgroundColor(preset.surfaceAlt);
        card->addView(thumb);

        auto* name = new brls::Label();
        name->setText(index == 0 ? "Streamer" : "Category");
        name->setFontSize(14);
        name->setMarginTop(6);
        name->setTextColor(preset.textPrimary);
        card->addView(name);

        auto* meta = new brls::Label();
        meta->setText(index == 0 ? "12.4K viewers" : "Just Chatting");
        meta->setFontSize(12);
        meta->setTextColor(preset.textMuted);
        card->addView(meta);
    }
    return preview;
}

void chooseTheme(kicknx::theme::Preset preset) {
    brls::Application::popActivity();
    if (preset == kicknx::theme::current().preset) return;
    kicknx::theme::set(preset);
    KickNXHome::scheduleThemeRebuild();
}

}  // namespace

void KickNXHome::scheduleThemeRebuild() {
    // The picker restores focus to this Home as it closes, so rebuild only
    // after it is gone; deleting Home earlier would leave focus pointing at a
    // destroyed view.
    themeRebuildPending = true;
    brls::delay(THEME_REBUILD_DELAY_MS, []() { KickNXHome::rebuildForTheme(); });
}

void KickNXHome::showThemePicker() {
    const auto& palette = kicknx::theme::current();

    auto* page = new ThemePickerOverlay();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    page->setJustifyContent(brls::JustifyContent::CENTER);
    page->setAlignItems(brls::AlignItems::CENTER);
    page->setBackgroundColor(nvgRGBA(0, 0, 0, 160));

    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(std::min(1040.0f, brls::Application::contentWidth - 96.0f));
    panel->setCornerRadius(16);
    panel->setBorderThickness(2.0f);
    panel->setBorderColor(palette.outline);
    panel->setBackgroundColor(palette.background);
    page->addView(panel);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(20, 28, 16, 28);
    heading->setWidthPercentage(100);
    heading->setCornerRadius(16);
    heading->setBackgroundColor(palette.header);
    kicknx::theme::decorateHeader(heading);
    panel->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* title = new brls::Label();
    title->setText("Theme");
    title->setFontSize(34);
    title->setTextColor(palette.headerAccent);
    headingText->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText("Choose how kickNX looks. Video and the chat overlay stay dark.");
    subtitle->setFontSize(17);
    subtitle->setTextColor(palette.headerTextMuted);
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([](brls::View*) {
        return brls::Application::popActivity();
    });
    heading->addView(backButton);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setPadding(24, 28, 28, 28);
    panel->addView(row);

    brls::View* currentCard = nullptr;
    const auto& all = kicknx::theme::presets();
    for (size_t index = 0; index < all.size(); ++index) {
        const auto& preset = all[index];
        const bool active = preset.preset == palette.preset;

        auto* card = new KickNXActionButton();
        card->setFocusable(true);
        card->setAxis(brls::Axis::COLUMN);
        card->setGrow(1.0f);
        card->setPadding(14, 14, 16, 14);
        card->setCornerRadius(12);
        card->setBackgroundColor(palette.surface);
        if (index + 1 < all.size()) card->setMarginRight(24);
        card->addGestureRecognizer(new brls::TapGestureRecognizer(card));
        card->upTarget = [backButton]() -> brls::View* { return backButton; };
        const kicknx::theme::Preset choice = preset.preset;
        card->registerClickAction([choice](brls::View*) {
            chooseTheme(choice);
            return true;
        });
        row->addView(card);

        card->addView(makeThemePreview(preset));

        auto* name = new brls::Label();
        name->setText(preset.name);
        name->setFontSize(20);
        name->setMarginTop(12);
        name->setTextColor(palette.textPrimary);
        card->addView(name);

        auto* state = new brls::Label();
        state->setText(active ? "Current theme" : "Press A to apply");
        state->setFontSize(15);
        state->setMarginTop(4);
        state->setTextColor(active ? palette.accent : palette.textMuted);
        card->addView(state);

        if (active) currentCard = card;
    }

    static_cast<KickNXActionButton*>(backButton)->downTarget = [currentCard]() -> brls::View* {
        return currentCard;
    };

    page->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        return brls::Application::popActivity();
    });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::View* initialFocus = currentCard ? currentCard : backButton;
    brls::sync([initialFocus]() {
        brls::Application::giveFocus(initialFocus);
    });
}

void KickNXHome::rebuildForTheme() {
    KickNXHome* old = instance;
    if (!old || !themeRebuildPending) return;
    brls::Activity* activity = old->getParentActivity();
    const auto& stack = brls::Application::getActivitiesStack();
    // Only swap while Home is the visible screen; otherwise willAppear retries.
    if (!activity || stack.empty() || stack.back() != activity) return;
    // Wait for in-flight browsing requests so none completes into a deleted Home.
    if (old->busy) {
        brls::delay(THEME_REBUILD_DELAY_MS, []() { KickNXHome::rebuildForTheme(); });
        return;
    }

    themeRebuildPending = false;
    auto* fresh = new KickNXHome();
    // Move focus into the new Home before setContentView deletes the old one.
    brls::Application::giveFocus(fresh->refreshButton);
    activity->setContentView(fresh);
}

void KickNXHome::setBusy(bool value, const std::string& message) {
    busy = value;
    if (!message.empty()) statusLabel->setText(message);
}

void KickNXHome::updateAccount() {
    const auto session = kick::loadOAuth();
    if (session.signedIn()) {
        accountLabel->setText("Signed in as " +
            (session.displayName.empty() ? session.login : session.displayName));
        loginButtonLabel->setText("Sign out");
        if (profileAvatar) profileAvatar->setFocusable(true);
        if (!session.profileImageUrl.empty()) {
            updateProfileImage(session.profileImageUrl);
        }
    } else {
        accountLabel->setText("Guest mode · Public Kick streams & live chat enabled");
        loginButtonLabel->setText("Sign in");
        if (profileAvatar) profileAvatar->setFocusable(false);
        updateProfileImage("");
    }
}

void KickNXHome::updateProfileImage(const std::string& url) {
    if (!profileAvatar) return;
    static_cast<KickNXProfileAvatar*>(
        profileAvatar)->setProfileImage(url);
}

uint64_t KickNXHome::beginBrowsingRequest(const std::string& message) {
    // A browsing refresh/search may replace an older browsing operation. Login
    // and playback resolution remain non-replaceable because they use separate
    // protocols and must complete their own lifecycle cleanly.
    if (busy && !activeRequest) return 0;
    if (activeRequest) activeRequest->store(true);

    activeRequest = std::make_shared<std::atomic_bool>(false);
    const uint64_t id = ++requestGeneration;
    setBusy(true, message);
    this->ptrLock();
    return id;
}

bool KickNXHome::finishBrowsingRequest(uint64_t requestId) {
    this->ptrUnlock();
    if (requestId != requestGeneration) return false;
    activeRequest.reset();
    busy = false;
    return true;
}

void KickNXHome::cancelBrowsingRequest() {
    if (!activeRequest) return;
    activeRequest->store(true);
    activeRequest.reset();
    ++requestGeneration;
    busy = false;
}

void KickNXHome::showRequestError(const std::string& message, std::function<void()> retry) {
    errorLabel->setText(message);
    retryAction = std::move(retry);
    retryButton->setVisibility(retryAction ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    errorBox->setVisibility(brls::Visibility::VISIBLE);
}

void KickNXHome::clearRequestError() {
    retryAction = nullptr;
    errorBox->setVisibility(brls::Visibility::GONE);
}

void KickNXHome::rememberFocus(const std::string& row, const std::string& key) {
    lastFocusedRow = row;
    lastFocusedKey = key;
}

bool KickNXHome::isActiveActivity() {
    const auto& stack = brls::Application::getActivitiesStack();
    return !stack.empty() && (stack.back() == this->getParentActivity() || stack.back()->getContentView() == this);
}

void KickNXHome::restoreContentFocus() {
    if (!isActiveActivity()) return;
    if (lastFocusedRow == "followed" && followedRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "popular" && popularRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "popular-clips" && popularClipsRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "events" && eventsRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "category-streams" && categoryStreamsRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "categories" && categoriesRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "search" && searchRow->focusKey(lastFocusedKey)) return;
    if (lastFocusedRow == "offline-followed" && offlineFollowedRow->focusKey(lastFocusedKey)) return;
    focusFirstContentRow();
}

void KickNXHome::focusFirstContentRow() {
    if (!isActiveActivity()) return;
    if (auto* focus = followedRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = popularRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = popularClipsRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = eventsRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = categoriesRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    if (auto* focus = offlineFollowedRow->defaultFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    brls::Application::giveFocus(refreshButton);
}

void KickNXHome::beginLogin() {
    if (busy) return;
    clearRequestError();

    struct LoginDialogState {
        kick::TVLinkSession tvSession;
        kick::QRCodeView* qrView = nullptr;
        brls::Label* pairCodeLabel = nullptr;
        brls::Label* statusText = nullptr;
        brls::RepeatingTimer timer;
        HTTP::Cancel cancel = std::make_shared<std::atomic_bool>(false);
        std::function<void()> pollFunc;
        bool submitting = false;
        bool polling = false;

        ~LoginDialogState() {
            timer.stop();
            if (cancel) cancel->store(true);
        }
    };

    auto state = std::make_shared<LoginDialogState>();

    auto* page = new brls::Box();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    const auto& palette = kicknx::theme::current();
    // With full-screen artwork (Kick theme) the heading sits on the dark
    // pattern and the two cards take the page colour so they stand out.
    const bool artwork = !palette.pageImage.empty();
    const NVGcolor cardColor = artwork ? palette.background : palette.surfaceAlt;
    page->setBackgroundColor(palette.header);
    kicknx::theme::decoratePage(page);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(20, 48, 14, 48);
    heading->setWidthPercentage(100);
    page->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* title = new brls::Label();
    title->setText("Sign in to Kick");
    title->setFontSize(32);
    title->setTextColor(artwork ? palette.headerAccent : palette.textPrimary);
    headingText->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText(
        "Scan with the Kick mobile app to pair your account");
    subtitle->setFontSize(16);
    subtitle->setTextColor(artwork ? palette.headerAccent : palette.textMuted);
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([state](brls::View*) {
        if (state->submitting) return true;
        state->timer.stop();
        if (state->cancel) state->cancel->store(true);
        return brls::Application::popActivity();
    });
    heading->addView(backButton);

    // Full-screen two-panel hero layout for instant QR & TV code sign-in
    auto* body = new brls::Box();
    body->setAxis(brls::Axis::ROW);
    body->setGrow(1.0f);
    body->setWidthPercentage(100);
    body->setPadding(10, 48, 28, 48);
    body->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    body->setAlignItems(brls::AlignItems::STRETCH);
    page->addView(body);

    // ==========================================
    // Left: Centered & Enlarged Hero QR Card
    // ==========================================
    auto* qrCard = new brls::Box();
    qrCard->setAxis(brls::Axis::COLUMN);
    qrCard->setAlignItems(brls::AlignItems::CENTER);
    qrCard->setJustifyContent(brls::JustifyContent::CENTER);
    qrCard->setWidth(480);
    qrCard->setCornerRadius(18);
    qrCard->setBackgroundColor(cardColor);
    qrCard->setPadding(24, 24, 24, 24);
    body->addView(qrCard);

    state->qrView = new kick::QRCodeView("", 350.0f);
    qrCard->addView(state->qrView);

    auto* qrHint = new brls::Label();
    qrHint->setText("Point your phone camera at this QR code");
    qrHint->setFontSize(16);
    qrHint->setTextColor(kicknx::theme::current().textSecondary);
    qrHint->setMarginTop(18);
    qrCard->addView(qrHint);

    // ==========================================
    // Right: Instructions, 6-Digit Code & Actions
    // ==========================================
    auto* infoCard = new brls::Box();
    infoCard->setAxis(brls::Axis::COLUMN);
    infoCard->setGrow(1.0f);
    infoCard->setMarginLeft(32);
    infoCard->setCornerRadius(18);
    infoCard->setBackgroundColor(cardColor);
    infoCard->setPadding(32, 40, 28, 40);
    infoCard->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    body->addView(infoCard);

    auto* infoTop = new brls::Box();
    infoTop->setAxis(brls::Axis::COLUMN);
    infoCard->addView(infoTop);

    auto* qrHeading = new brls::Label();
    qrHeading->setText("Instant QR & TV Code Sign-In");
    qrHeading->setFontSize(26);
    qrHeading->setTextColor(kicknx::theme::current().textPrimary);
    infoTop->addView(qrHeading);

    auto* step1 = new brls::Label();
    step1->setText("1. Open the Kick app on your phone (signed into your account).");
    step1->setFontSize(16);
    step1->setTextColor(kicknx::theme::current().textSecondary);
    step1->setMarginTop(16);
    infoTop->addView(step1);

    auto* step2 = new brls::Label();
    step2->setText("2. Point your phone camera or QR scanner at the code on the left.");
    step2->setFontSize(16);
    step2->setTextColor(kicknx::theme::current().textSecondary);
    step2->setMarginTop(10);
    infoTop->addView(step2);

    auto* step3 = new brls::Label();
    step3->setText("3. Confirm that this 6-digit verification code matches in the app:");
    step3->setFontSize(16);
    step3->setTextColor(kicknx::theme::current().textSecondary);
    step3->setMarginTop(10);
    infoTop->addView(step3);

    auto* codeBox = new brls::Box();
    codeBox->setAxis(brls::Axis::ROW);
    codeBox->setAlignItems(brls::AlignItems::CENTER);
    codeBox->setJustifyContent(brls::JustifyContent::CENTER);
    codeBox->setPadding(14, 32, 14, 32);
    codeBox->setCornerRadius(12);
    codeBox->setBackgroundColor(artwork ? palette.background : palette.header);
    // On a card of the same green, an outline keeps the code box visible.
    if (artwork) {
        codeBox->setBorderThickness(2.0f);
        codeBox->setBorderColor(palette.textPrimary);
    }
    codeBox->setMarginTop(18);
    codeBox->setWidth(320);
    infoTop->addView(codeBox);

    state->pairCodeLabel = new brls::Label();
    state->pairCodeLabel->setFontSize(44);
    state->pairCodeLabel->setTextColor(kicknx::theme::current().accent);
    codeBox->addView(state->pairCodeLabel);

    auto* infoBottom = new brls::Box();
    infoBottom->setAxis(brls::Axis::COLUMN);
    infoCard->addView(infoBottom);

    state->statusText = new brls::Label();
    state->statusText->setText("Waiting for confirmation in the Kick mobile app…");
    state->statusText->setFontSize(16);
    state->statusText->setTextColor(kicknx::theme::current().accent);
    state->statusText->setMarginBottom(16);
    infoBottom->addView(state->statusText);

    auto* qrActionRow = new brls::Box();
    qrActionRow->setAxis(brls::Axis::ROW);
    qrActionRow->setAlignItems(brls::AlignItems::CENTER);
    infoBottom->addView(qrActionRow);

    auto* refreshQrBtn = makeButton("Refresh Code");
    qrActionRow->addView(refreshQrBtn);

    auto* cancelBtn = makeButton("Cancel");
    cancelBtn->setMarginLeft(16);
    cancelBtn->registerClickAction([state](brls::View*) {
        if (state->submitting) return true;
        state->timer.stop();
        if (state->cancel) state->cancel->store(true);
        return brls::Application::popActivity();
    });
    qrActionRow->addView(cancelBtn);

    auto refreshSession = [this, state]() {
        if (state->cancel) state->cancel->store(true);
        state->cancel = std::make_shared<std::atomic_bool>(false);
        state->polling = false;
        state->tvSession = kick::createTVLinkSession();
        if (state->qrView) state->qrView->setText(state->tvSession.verificationUrl);
        if (state->pairCodeLabel) {
            std::string c = state->tvSession.code;
            if (c.size() == 6) c = c.substr(0, 3) + " " + c.substr(3);
            state->pairCodeLabel->setText(c);
        }
        if (state->statusText) {
            state->statusText->setText("Waiting for confirmation in the Kick mobile app…");
        }
    };
    refreshSession();

    refreshQrBtn->registerClickAction([refreshSession](brls::View*) {
        refreshSession();
        return true;
    });

    state->pollFunc = [this, state]() {
        if (state->submitting || state->polling || (state->cancel && state->cancel->load())) return;
        state->polling = true;
        kick::pollTVLinkAuthAsync(
            kick::loadConfig(),
            state->tvSession,
            [this, state](kick::OAuthSession session) {
                state->timer.stop();
                state->polling = false;
                state->submitting = false;
                setBusy(false, "Signed in to Kick as " +
                    (session.displayName.empty() ? session.login : session.displayName));
                updateAccount();
                updateProfileImage(session.profileImageUrl);
                brls::Application::notify("kickNX: Signed in as " +
                    (session.displayName.empty() ? session.login : session.displayName));
                this->ptrUnlock();
                brls::Application::popActivity();
                refreshHome();
            },
            [state](bool pending, const std::string& message) {
                state->polling = false;
                if (state->cancel && state->cancel->load()) return;
                if (state->statusText) {
                    state->statusText->setText(message);
                }
            },
            state->cancel);
    };

    state->timer.setCallback([state]() {
        if (state->pollFunc) state->pollFunc();
    });
    state->timer.start(2500);

    page->registerAction("Back", brls::BUTTON_B, [state](brls::View*) {
        if (state->submitting) return true;
        state->timer.stop();
        if (state->cancel) state->cancel->store(true);
        return brls::Application::popActivity();
    });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::sync([refreshQrBtn]() {
        brls::Application::giveFocus(refreshQrBtn);
    });
}

void KickNXHome::logout() {
    if (busy && !activeRequest) return;
    cancelBrowsingRequest();
    clearRequestError();
    setBusy(true, "Signing out of Kick…");
    this->ptrLock();
    kick::logoutAsync(
        kick::loadConfig(),
        [this]() {
            updateProfileImage("");
            updateAccount();
            followedRow->clear();
            offlineFollowedRow->clear();
            setBusy(false, "Signed out. Browsing Kick in guest mode.");
            brls::Application::notify("kickNX: Signed out of Kick");
            this->ptrUnlock();
            refreshHome();
        });
}

void KickNXHome::refreshHome() {
    const uint64_t requestId = beginBrowsingRequest("Loading Kick home…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    kick::loadHomeAsync(
        kick::loadConfig(),
        [this, requestId](kick::HomeData data) {
            if (!finishBrowsingRequest(requestId)) return;
            showHomeData(std::move(data));
            restoreContentFocus();
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Home error: " + error);
            showRequestError("Could not refresh Kick home: " + error,
                [this]() { refreshHome(); });
            brls::Application::notify("kickNX: " + error);
        },
        requestCancel);
}

void KickNXHome::showHomeData(kick::HomeData data) {
    updateAccount();
    std::string profileUrl = data.profile.profileImageUrl;
    if (profileUrl.empty()) {
        const auto session = kick::loadOAuth();
        if (session.signedIn()) profileUrl = session.profileImageUrl;
    }
    if (!profileUrl.empty()) {
        updateProfileImage(profileUrl);
    }
    auto play = [this](const kick::Stream& stream) { playChannel(stream); };

    if (!data.followed.items.empty()) updateHero(data.followed.items.front());
    else if (!data.popular.items.empty()) updateHero(data.popular.items.front());
    else clearHero();

    followedCursor = data.followed.loaded ? data.followed.cursor : "";
    popularCursor = data.popular.loaded ? data.popular.cursor : "";
    popularClipsCursor = data.popularClips.loaded ? data.popularClips.cursor : "";
    categoriesCursor = data.categories.loaded ? data.categories.cursor : "";

    const bool signedIn = data.account.signedIn();
    const bool followedHasItems = signedIn && !data.followed.items.empty();
    const bool offlineFollowedHasItems = signedIn && !data.offlineFollowed.items.empty();
    const bool popularHasItems = !data.popular.items.empty();
    const bool popularClipsHaveItems = !data.popularClips.items.empty();
    std::vector<kick::LiveEvent> liveEvents;
    for (auto& event : data.events.items) {
        if (event.isLive()) liveEvents.push_back(std::move(event));
    }
    const bool eventsHaveItems = !liveEvents.empty();
    const bool categoriesHaveItems = !data.categories.items.empty();

    auto focusTopActions = [this]() -> brls::View* {
        if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
        return refreshButton;
    };

    std::function<brls::View*()> followedUpTarget;
    std::function<brls::View*()> offlineFollowedUpTarget;
    std::function<brls::View*()> popularUpTarget;
    std::function<brls::View*()> popularClipsUpTarget;
    std::function<brls::View*()> eventsUpTarget;
    std::function<brls::View*()> categoriesUpTarget;

    if (followedHasItems) {
        followedUpTarget = focusTopActions;
    } else if (popularHasItems) {
        popularUpTarget = focusTopActions;
    } else if (popularClipsHaveItems) {
        popularClipsUpTarget = focusTopActions;
    } else if (eventsHaveItems) {
        eventsUpTarget = focusTopActions;
    } else if (categoriesHaveItems) {
        categoriesUpTarget = focusTopActions;
    } else if (offlineFollowedHasItems) {
        offlineFollowedUpTarget = focusTopActions;
    }

    if (signedIn) {
        followedRow->setStreams(std::move(data.followed.items), play,
            data.followedError.empty()
                ? "None of your followed Kick channels are live right now."
                : "Followed channels could not be loaded. Use Retry to try again.",
            followedUpTarget);
        offlineFollowedRow->setStreams(std::move(data.offlineFollowed.items), play,
            data.offlineFollowedError.empty()
                ? "No offline followed channels were returned."
                : "Offline followed channels could not be loaded. Use Retry to try again.",
            offlineFollowedUpTarget);
    } else {
        followedRow->clear();
        offlineFollowedRow->clear();
    }

    popularRow->setStreams(std::move(data.popular.items), play,
        data.popularError.empty()
            ? "Kick returned no featured live streams."
            : "Popular streams could not be loaded. Use Retry to try again.",
        popularUpTarget);
    popularClipsRow->setClips(std::move(data.popularClips.items),
        [](const kick::ChannelClip& clip) {
            const std::string channel = clip.channelName.empty() ? clip.creatorName : clip.channelName;
            RemoteView::playClip(
                clip.id,
                clip.title,
                channel + " · " + viewsText(clip.viewCount));
        },
        data.popularClipsError.empty()
            ? "Kick returned no popular clips."
            : "Popular clips could not be loaded. Use Retry to try again.",
        popularClipsUpTarget);
    // Events are new and undocumented; when none are live, or the request
    // failed, the row is hidden rather than showing an error.
    if (eventsHaveItems) {
        eventsRow->setEvents(std::move(liveEvents),
            [this](const kick::LiveEvent& event) { openEvent(event); },
            "No Kick events are live right now.",
            eventsUpTarget);
    } else {
        eventsRow->clear();
    }
    categoriesRow->setCategories(std::move(data.categories.items),
        [this](const kick::Category& category) { openCategory(category); },
        data.categoriesError.empty()
            ? "Kick returned no top categories."
            : "Top categories could not be loaded. Use Retry to try again.",
        categoriesUpTarget);

    followedRow->setHasMore(!followedCursor.empty());
    popularRow->setHasMore(!popularCursor.empty());
    popularClipsRow->setHasMore(!popularClipsCursor.empty());
    categoriesRow->setHasMore(!categoriesCursor.empty());
    followedRow->onNextPage(!followedCursor.empty(), [this]() { loadMoreFollowed(); });
    popularRow->onNextPage(!popularCursor.empty(), [this]() { loadMorePopular(); });
    popularClipsRow->onNextPage(!popularClipsCursor.empty(), [this]() { loadMorePopularClips(); });
    categoriesRow->onNextPage(!categoriesCursor.empty(), [this]() { loadMoreCategories(); });

    categoryStreamsRow->clear();
    searchRow->clear();
    categoryStreamsCursor.clear();
    searchCursor.clear();
    currentCategoryId.clear();
    currentCategoryName.clear();
    currentSearchQuery.clear();

    if (data.hasErrors()) {
        const std::string sections = joinErrors(data);
        setBusy(false, "Home updated, but " + sections + " could not be loaded.");
        showRequestError("Some Kick sections failed to load: " + sections + ".",
            [this]() { refreshHome(); });
    } else {
        clearRequestError();
        setBusy(false, "Kick home updated.");
    }
}

void KickNXHome::showAbout() {
    auto* page = new brls::Box();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    page->setBackgroundColor(kicknx::theme::current().background);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(24, 48, 18, 48);
    heading->setWidthPercentage(100);
    heading->setBackgroundColor(kicknx::theme::current().header);
    kicknx::theme::decorateHeader(heading);
    page->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    headingText->addView(titleRow);

    auto* title = new brls::Label();
    title->setText("About");
    title->setFontSize(34);
    title->setTextColor(kicknx::theme::current().headerAccent);
    titleRow->addView(title);

    auto* titleLogo = new brls::Image();
    titleLogo->setImageFromRes("img/kicknx_logo.png");
    titleLogo->setScalingType(brls::ImageScalingType::FIT);
    titleLogo->setHeight(44);
    titleLogo->setWidth(44.0f * 644.0f / 186.0f);
    titleLogo->setMarginLeft(14);
    titleRow->addView(titleLogo);

    auto* subtitle = new brls::Label();
    subtitle->setText(
        "Version " + AppVersion::getVersion() +
        " · Kick for Nintendo Switch");
    subtitle->setFontSize(17);
    subtitle->setTextColor(kicknx::theme::current().headerTextMuted);
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([](brls::View*) {
        return brls::Application::popActivity();
    });
    heading->addView(backButton);

    auto* scroll = new AboutScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setFocusable(false);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusProxy(backButton);
    page->addView(scroll);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(6, 48, 42, 48);
    scroll->setContentView(content);

    auto addSection = [content](const std::string& text) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(25);
        label->setTextColor(kicknx::theme::current().textPrimary);
        label->setMarginTop(18);
        label->setMarginBottom(8);
        content->addView(label);
    };

    auto addBody = [content](
                       const std::string& text,
                       NVGcolor color = kicknx::theme::current().textSecondary) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(18);
        label->setTextColor(color);
        label->setMarginBottom(8);
        content->addView(label);
    };

    auto* creditCard = new brls::Box();
    creditCard->setAxis(brls::Axis::COLUMN);
    creditCard->setPadding(18, 22, 18, 22);
    creditCard->setCornerRadius(10);
    creditCard->setBackgroundColor(kicknx::theme::current().surfaceAlt);
    creditCard->setMarginBottom(8);
    content->addView(creditCard);

    auto* createdBy = new brls::Label();
    createdBy->setText("Created by j2k");
    createdBy->setFontSize(24);
    createdBy->setTextColor(kicknx::theme::current().accent);
    creditCard->addView(createdBy);

    auto* projectDescription = new brls::Label();
    projectDescription->setText(
        "kickNX is a controller-first, native Kick client for "
        "Nintendo Switch homebrew, forked from twiNX.");
    projectDescription->setFontSize(18);
    projectDescription->setTextColor(kicknx::theme::current().textSecondary);
    projectDescription->setMarginTop(7);
    creditCard->addView(projectDescription);

    addSection("Features");
    addBody(
        "• Kick sign-in (official TV QR code mobile pairing) and guest-mode browsing\n"
        "• Followed channels row for live and offline streams\n"
        "• Featured live streams, subcategories and live search by channel or game with viewer counts\n"
        "• Detailed channel pages with past broadcasts (VODs), clips and categories\n"
        "• Live AWS IVS HLS, VOD and clip playback with quality selection\n"
        "• Selectable Software, Hardware and Hybrid playback modes\n"
        "• Automatic portrait mode using attached Joy-Con motion sensors, with either rotation supported\n"
        "• Smartphone-style portrait player combining video, live Pusher chat, a persistent message draft, inline touch keyboard and emote sheet\n"
        "• Real-time Pusher WebSocket chat with static/animated Kick emotes and message composer\n"
        "• Touch controls for switching between the portrait keyboard and emote sheet\n"
        "• Adjustable touch-keyboard haptics for portrait mode typing\n"
        "• Docked and overlay chat, compact mode and four-corner placement\n"
        "• Controller-first navigation designed for portrait, handheld and TV play");

    addSection("Notes");
    addBody(
        "• Subtitles status: Subtitle tracks in Player Settings remain inactive until Kick "
        "updates their Amazon IVS backend to serve live transcode tracks (Kick's web player "
        "currently relies on client-side browser speech recognition).");

    addSection("Changelog");
    addBody(
        "0.5.0 (Current)\n"
        "• Colour themes: OLED (default), Dark Grey and Kick, chosen from a pop-up on Home.\n"
        "• Kick theme: muted green pages, grey cards and a dark K pattern on headers and sign-in.\n"
        "• Live events row and event pages with live streams and popular clips.\n"
        "• In-app keyboard for landscape chat, with Caps Lock and a green Emotes key.\n"
        "• Stream title shown at the top of the live player.\n"
        "• Larger profile picture on Home.\n"
        "• Kick-only app: Jellyfin, media library, USB browsing and danmaku removed.\n"
        "• Offline streams show a message and return to the previous screen.\n"
        "• Live playback: adaptive quality, twiNX cache settings.\n"
        "• OSD wakes with the stick or D-pad without taking focus from the chat composer.\n"
        "• Emote backspace removes a whole emote.\n"
        "• Fixed a crash after closing a stream quickly, and sign-in/playback errors "
        "caused by too many open connections.\n\n"
        "0.4.0\n"
        "• Channel follow/unfollow button on streamer profiles with live status & sync.\n"
        "• Clickable user profile avatar navigating directly to your own channel page.\n"
        "• Custom 3D kickNX buffering indicator with lime-green text shimmer & neon bar.\n"
        "• Kick bright green theme highlights, section divider lines, and accent tabs.\n"
        "• Transparent 3D kickNX brand logo headers on Home and About pages.\n"
        "• OLED true black background on streamer profiles, About page, and playback.\n"
        "• Dedicated vertical category card placeholder (350x410).\n"
        "• Fixed stuck chat connecting banner, scrub bar exit crash, and popular streams showing offline.\n"
        "• Emote composer closes on rotation; OLED black chat panels; simplified playback settings.\n\n"
        "0.3.2\n"
        "• Kick green live playback bar with real-time stream uptime.\n"
        "• OLED true black home screen background.\n"
        "• Restored top-right profile avatar for signed-in accounts.\n"
        "• Kick logo card placeholder & clip/category thumbnail fixes.\n"
        "• Channel category click returns home and loads that category.\n\n"
        "0.3.1\n"
        "• Instant transition & loading indicator for VODs and clips.\n"
        "• Real-time active chatter counter in the live chat header.\n"
        "• Multi-language WebVTT subtitle track selection.\n\n"
        "0.3.0\n"
        "• Official Kick TV mobile QR code login (kick.com/tv/login).\n"
        "• 60 FPS smooth video playback presentation.\n"
        "• Fast single-variant VOD streaming & native quality selector.\n"
        "• Custom FFmpeg 7.1 hardware acceleration & stream tuning.\n\n"
        "0.2.0\n"
        "• Live Pusher chat with static/animated emotes and composer.\n"
        "• Channel pages with past broadcasts, clips, and category info.\n"
        "• Followed channels row and global broadcaster/category search.\n\n"
        "0.1.0\n"
        "• Initial kickNX native Nintendo Switch release.\n"
        "• Kick stream discovery, guest browsing, and Joy-Con portrait mode.");

    addSection("Backend notice");
    addBody(
        "Kick may change or update their backend at any time, which can affect app functionality.");

    addSection("Credits");
    addBody(
        "Project author & direction: j2k\n"
        "AI co-development: Gemini and Claude\n"
        "Original foundation: twiNX\n\n"
        "Built with Kick v1/v2 APIs, Centrifugo & Pusher WebSockets, mpv, FFmpeg, Borealis, "
        "libcurl, libnx, and devkitPro. Special thanks to the twiNX project, Gemini, Claude, and the "
        "Nintendo Switch homebrew and open-source communities that made kickNX possible.");

    addSection("Project note");
    addBody(
        "kickNX is an independent homebrew project. It is not affiliated "
        "with, endorsed by or sponsored by Kick Streaming Pty Ltd or Nintendo.",
        kicknx::theme::current().textFaint);

    page->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        return brls::Application::popActivity();
    });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::sync([backButton]() {
        brls::Application::giveFocus(backButton);
    });
}

void KickNXHome::search() {
    if (busy && !activeRequest) return;
    brls::Application::getImeManager()->openForText(
        [this](std::string query) {
            query.erase(query.begin(), std::find_if(query.begin(), query.end(), [](unsigned char c) {
                return !std::isspace(c);
            }));
            while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back()))) {
                query.pop_back();
            }
            if (!query.empty()) searchFor(query);
        },
        "Search Kick", "Enter a Kick channel slug or stream query", 100, "");
}

void KickNXHome::searchFor(const std::string& query) {
    const uint64_t requestId = beginBrowsingRequest("Searching Kick for “" + query + "”…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    kick::searchChannelsPageAsync(
        kick::loadConfig(), query, "",
        [this, requestId, query](kick::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            currentSearchQuery = query;
            searchCursor = page.cursor;
            searchRow->setTitle("Search results · " + query);
            searchRow->setStreams(std::move(page.items),
                [this](const kick::Stream& stream) { playChannel(stream); },
                "No Kick channels matched “" + query + "”.");
            searchRow->setHasMore(!searchCursor.empty());
            searchRow->onNextPage(!searchCursor.empty(), [this]() { loadMoreSearch(); });
            clearRequestError();
            setBusy(false, "Search complete.");
            if (isActiveActivity()) {
                if (auto* focus = searchRow->defaultFocus()) brls::Application::giveFocus(focus);
            }
        },
        [this, requestId, query](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Search error: " + error);
            showRequestError("Search failed: " + error,
                [this, query]() { searchFor(query); });
            brls::Application::notify("kickNX: " + error);
        },
        requestCancel);
}

void KickNXHome::openCategory(const kick::Category& category) {
    const uint64_t requestId = beginBrowsingRequest("Loading " + category.name + " streams…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    kick::loadCategoryStreamsPageAsync(
        kick::loadConfig(), category.id, "",
        [this, requestId, category](kick::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            currentCategoryId = category.id;
            currentCategoryName = category.name;
            categoryStreamsCursor = page.cursor;
            if (!page.items.empty()) updateHero(page.items.front());
            categoryStreamsRow->setTitle("Live in " + category.name);
            categoryStreamsRow->setStreams(std::move(page.items),
                [this](const kick::Stream& stream) { playChannel(stream); },
                "No live channels are currently streaming " + category.name + ".");
            categoryStreamsRow->setHasMore(!categoryStreamsCursor.empty());
            categoryStreamsRow->onNextPage(!categoryStreamsCursor.empty(), [this]() { loadMoreCategoryStreams(); });
            clearRequestError();
            setBusy(false, "Category loaded.");
            if (isActiveActivity()) {
                if (auto* focus = categoryStreamsRow->defaultFocus()) brls::Application::giveFocus(focus);
            }
        },
        [this, requestId, category](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Category error: " + error);
            showRequestError("Could not load " + category.name + ": " + error,
                [this, category]() { openCategory(category); });
            brls::Application::notify("kickNX: " + error);
        },
        requestCancel);
}

void KickNXHome::loadMoreFollowed() {
    if (followedCursor.empty() || busy) return;
    const std::string cursor = followedCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more followed channels…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    kick::loadFollowedPageAsync(kick::loadConfig(), cursor,
        [this, requestId](kick::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = followedRow->appendStreams(std::move(page.items));
            followedCursor = page.cursor;
            followedRow->setHasMore(!followedCursor.empty());
            followedRow->onNextPage(!followedCursor.empty(), [this]() { loadMoreFollowed(); });
            clearRequestError();
            setBusy(false, added ? "More followed channels loaded." : "No additional followed channels.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more followed channels: " + error,
                [this]() { loadMoreFollowed(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void KickNXHome::loadMorePopular() {
    if (popularCursor.empty() || busy) return;
    const std::string cursor = popularCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more popular streams…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    kick::loadPopularPageAsync(kick::loadConfig(), cursor,
        [this, requestId](kick::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = popularRow->appendStreams(std::move(page.items));
            popularCursor = page.cursor;
            popularRow->setHasMore(!popularCursor.empty());
            popularRow->onNextPage(!popularCursor.empty(), [this]() { loadMorePopular(); });
            clearRequestError();
            setBusy(false, added ? "More popular streams loaded." : "No additional popular streams.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more popular streams: " + error,
                [this]() { loadMorePopular(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void KickNXHome::loadMorePopularClips() {
    if (popularClipsCursor.empty() || busy) return;
    const std::string cursor = popularClipsCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more popular clips…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    kick::loadPopularClipsPageAsync(kick::loadConfig(), cursor,
        [this, requestId](kick::ClipPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = popularClipsRow->appendClips(std::move(page.items));
            popularClipsCursor = page.cursor;
            popularClipsRow->setHasMore(!popularClipsCursor.empty());
            popularClipsRow->onNextPage(!popularClipsCursor.empty(), [this]() { loadMorePopularClips(); });
            clearRequestError();
            setBusy(false, added ? "More popular clips loaded." : "No additional popular clips.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more popular clips: " + error,
                [this]() { loadMorePopularClips(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void KickNXHome::loadMoreCategories() {
    if (categoriesCursor.empty() || busy) return;
    const std::string cursor = categoriesCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more categories…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    kick::loadCategoriesPageAsync(kick::loadConfig(), cursor,
        [this, requestId](kick::CategoryPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = categoriesRow->appendCategories(std::move(page.items));
            categoriesCursor = page.cursor;
            categoriesRow->setHasMore(!categoriesCursor.empty());
            categoriesRow->onNextPage(!categoriesCursor.empty(), [this]() { loadMoreCategories(); });
            clearRequestError();
            setBusy(false, added ? "More categories loaded." : "No additional categories.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more categories: " + error,
                [this]() { loadMoreCategories(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void KickNXHome::loadMoreSearch() {
    if (searchCursor.empty() || currentSearchQuery.empty() || busy) return;
    const std::string cursor = searchCursor;
    const std::string query = currentSearchQuery;
    const uint64_t requestId = beginBrowsingRequest("Loading more search results…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    kick::searchChannelsPageAsync(kick::loadConfig(), query, cursor,
        [this, requestId](kick::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = searchRow->appendStreams(std::move(page.items));
            searchCursor = page.cursor;
            searchRow->setHasMore(!searchCursor.empty());
            searchRow->onNextPage(!searchCursor.empty(), [this]() { loadMoreSearch(); });
            clearRequestError();
            setBusy(false, added ? "More search results loaded." : "No additional search results.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more search results: " + error,
                [this]() { loadMoreSearch(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void KickNXHome::loadMoreCategoryStreams() {
    if (categoryStreamsCursor.empty() || currentCategoryId.empty() || busy) return;
    const std::string cursor = categoryStreamsCursor;
    const std::string gameId = currentCategoryId;
    const uint64_t requestId = beginBrowsingRequest("Loading more " + currentCategoryName + " streams…");
    if (!requestId) return;
    const HTTP::Cancel requestCancel = activeRequest;
    kick::loadCategoryStreamsPageAsync(kick::loadConfig(), gameId, cursor,
        [this, requestId](kick::StreamPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = categoryStreamsRow->appendStreams(std::move(page.items));
            categoryStreamsCursor = page.cursor;
            categoryStreamsRow->setHasMore(!categoryStreamsCursor.empty());
            categoryStreamsRow->onNextPage(!categoryStreamsCursor.empty(), [this]() { loadMoreCategoryStreams(); });
            clearRequestError();
            setBusy(false, added ? "More category streams loaded." : "No additional category streams.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more category streams: " + error,
                [this]() { loadMoreCategoryStreams(); });
            setBusy(false, "Pagination error: " + error);
        }, requestCancel);
}

void KickNXHome::openChannelPage(
    const kick::Stream& stream) {
    const std::string login =
        stream.userLogin.empty()
            ? stream.userName
            : stream.userLogin;

    if (login.empty()) {
        statusLabel->setText(
            "This result did not contain a "
            "Kick channel slug.");
        return;
    }

    brls::Application::pushActivity(
        new brls::Activity(
            new KickChannelPage(login)));
}

namespace {

// Root of the event page. Cancels its stream request when the page closes so
// a late reply never touches views that were already deleted.
class EventPageView : public brls::Box {
public:
    EventPageView() : cancel(std::make_shared<std::atomic_bool>(false)) {}
    ~EventPageView() override { cancel->store(true); }

    HTTP::Cancel cancel;
};

// "2026-09-08T16:00:00Z" -> "2026-09-08 16:00 UTC". Kick sends UTC times.
std::string eventTimeText(const std::string& iso) {
    if (iso.size() < 16 || iso[10] != 'T') return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

std::string eventPhaseText(const kick::LiveEvent& event) {
    if (event.isLive()) return "LIVE NOW";
    if (event.phase == "pre") return "Upcoming";
    if (event.phase == "post") return "Ended";
    return "Event";
}

}  // namespace

void KickNXHome::openEvent(const kick::LiveEvent& event) {
    const auto& palette = kicknx::theme::current();

    auto* page = new EventPageView();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    page->setBackgroundColor(palette.background);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(24, 48, 18, 48);
    heading->setWidthPercentage(100);
    heading->setBackgroundColor(palette.header);
    kicknx::theme::decorateHeader(heading);
    page->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* title = new brls::Label();
    title->setText(event.title);
    title->setFontSize(30);
    title->setSingleLine(true);
    title->setTextColor(palette.headerTextPrimary);
    headingText->addView(title);

    std::string meta = eventPhaseText(event);
    if (!event.categoryName.empty()) meta += " · " + event.categoryName;
    if (!event.organiserName.empty()) meta += " · Organised by " + event.organiserName;
    if (!event.startTime.empty()) meta += " · " + eventTimeText(event.startTime);
    auto* metaLabel = new brls::Label();
    metaLabel->setText(meta);
    metaLabel->setFontSize(16);
    metaLabel->setMarginTop(4);
    metaLabel->setTextColor(event.isLive() ? palette.headerAccent : palette.headerTextMuted);
    headingText->addView(metaLabel);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([](brls::View*) {
        return brls::Application::popActivity();
    });
    heading->addView(backButton);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    page->addView(scroll);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(18, 48, 36, 48);
    scroll->setContentView(content);

    if (!event.bannerUrl.empty()) {
        auto* banner = new brls::Image();
        banner->setWidthPercentage(100);
        banner->setHeight(230);
        banner->setCornerRadius(12);
        banner->setScalingType(brls::ImageScalingType::FILL);
        banner->setImageFromRes("img/video-card-bg.png");
        Image::with(banner, event.bannerUrl);
        content->addView(banner);
    }

    if (!event.description.empty()) {
        auto* description = new brls::Label();
        description->setText(event.description);
        description->setFontSize(17);
        description->setMarginTop(14);
        description->setTextColor(palette.textSecondary);
        content->addView(description);
    }

    auto* status = new brls::Label();
    status->setFontSize(17);
    status->setMarginTop(18);
    status->setTextColor(palette.textMuted);
    content->addView(status);

    auto* streamsRow = new KickRow("event-streams", "Live streams", KickRow::Kind::Streams,
        [](const std::string&, const std::string&) {});
    streamsRow->setMarginTop(14);
    content->addView(streamsRow);

    // Most-viewed clips from the event, shown once Kick returns any; ended
    // events often have clips even with no one live.
    auto* clipsRow = new KickRow("event-clips", "Popular clips", KickRow::Kind::Clips,
        [](const std::string&, const std::string&) {});
    clipsRow->setMarginTop(14);
    clipsRow->setVisibility(brls::Visibility::GONE);
    content->addView(clipsRow);

    // Shared with the async callbacks below; they bail out once the page closes.
    struct EventPageState {
        bool streamsShown = false;
        bool clipsLoading = false;
        std::string clipsCursor;
    };
    auto state = std::make_shared<EventPageState>();

    static_cast<KickNXActionButton*>(backButton)->downTarget = [state, streamsRow, clipsRow]() -> brls::View* {
        if (state->streamsShown) return streamsRow->defaultFocus();
        if (clipsRow->getVisibility() == brls::Visibility::VISIBLE) return clipsRow->defaultFocus();
        return nullptr;
    };

    page->registerAction("Back", brls::BUTTON_B, [](brls::View*) {
        return brls::Application::popActivity();
    });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::sync([backButton]() { brls::Application::giveFocus(backButton); });

    const HTTP::Cancel cancel = page->cancel;
    const std::string eventId = event.id;
    const auto playClip = [](const kick::ChannelClip& clip) {
        const std::string channel = clip.channelName.empty() ? clip.creatorName : clip.channelName;
        RemoteView::playClip(clip.id, clip.title, channel + " · " + viewsText(clip.viewCount));
    };
    const auto clipsUp = [state, streamsRow, backButton]() -> brls::View* {
        if (state->streamsShown) return streamsRow->defaultFocus();
        return backButton;
    };

    // Next pages load as the row is scrolled to its end. The row's grid owns
    // the loader; the loader keeps only a weak reference to itself, so there
    // is no ownership cycle.
    auto loadMoreClips = std::make_shared<std::function<void()>>();
    *loadMoreClips = [cancel, state, clipsRow, eventId, weakMore = std::weak_ptr<std::function<void()>>(loadMoreClips)]() {
        if (state->clipsLoading || state->clipsCursor.empty()) return;
        state->clipsLoading = true;
        kick::loadEventClipsAsync(kick::loadConfig(), eventId, state->clipsCursor,
            [cancel, state, clipsRow, weakMore](kick::ClipPage result) {
                if (cancel->load()) return;
                state->clipsLoading = false;
                clipsRow->appendClips(std::move(result.items));
                state->clipsCursor = result.cursor;
                clipsRow->setHasMore(!state->clipsCursor.empty());
                if (auto more = weakMore.lock())
                    clipsRow->onNextPage(!state->clipsCursor.empty(), [more]() { (*more)(); });
            },
            [cancel, state](const std::string&) {
                if (cancel->load()) return;
                state->clipsLoading = false;
            },
            cancel);
    };

    kick::loadEventClipsAsync(kick::loadConfig(), eventId, "",
        [cancel, state, clipsRow, playClip, clipsUp, loadMoreClips](kick::ClipPage result) {
            if (cancel->load() || result.items.empty()) return;
            state->clipsCursor = result.cursor;
            clipsRow->setClips(std::move(result.items), playClip,
                "This event has no clips yet.", clipsUp);
            clipsRow->setHasMore(!state->clipsCursor.empty());
            clipsRow->onNextPage(!state->clipsCursor.empty(), [loadMoreClips]() { (*loadMoreClips)(); });
        },
        // Clips are extra; on failure the row simply stays hidden.
        [](const std::string&) {},
        cancel);

    if (!event.isLive() && event.liveStreamCount <= 0) {
        status->setText(event.phase == "pre"
            ? "This event hasn't started yet."
            : "This event has ended.");
        streamsRow->setVisibility(brls::Visibility::GONE);
        return;
    }

    status->setText("Loading live streams…");
    kick::loadEventStreamsAsync(kick::loadConfig(), eventId,
        [this, cancel, state, status, streamsRow, backButton](kick::StreamPage result) {
            if (cancel->load()) return;
            status->setVisibility(brls::Visibility::GONE);
            state->streamsShown = !result.items.empty();
            streamsRow->setStreams(std::move(result.items),
                [this](const kick::Stream& stream) { playChannel(stream); },
                "No one is streaming this event right now.",
                [backButton]() -> brls::View* { return backButton; });
            if (auto* focus = streamsRow->defaultFocus()) brls::Application::giveFocus(focus);
        },
        [cancel, status](const std::string& error) {
            if (cancel->load()) return;
            status->setText("Live streams could not be loaded: " + error);
        },
        cancel);
}

void KickNXHome::playChannel(const kick::Stream& stream) {
    if (busy) return;
    if (!stream.isLive) {
        openChannelPage(stream);
        return;
    }

    auto config = kick::loadConfig();
    config.channel = stream.userLogin;
    config.preferredQuality = kick::loadPreferredQuality();
    if (config.preferredQuality.empty()) config.preferredQuality = "auto";
    if (config.channel.empty()) {
        statusLabel->setText("This result did not contain a Kick channel slug.");
        return;
    }

    RemoteView::playLive(config, stream.uptimeSeconds, stream.title);
}

void KickNXHome::playConfiguredChannel() {
    auto config = kick::loadConfig();
    if (config.channel.empty()) {
        statusLabel->setText("No Kick channel is selected");
        return;
    }
    config.preferredQuality = kick::loadPreferredQuality();
    if (config.preferredQuality.empty()) config.preferredQuality = "auto";
    RemoteView::playLive(config);
}

void KickNXHome::clearRows() {
    clearHero();
    followedRow->clear();
    offlineFollowedRow->clear();
    popularRow->clear();
    popularClipsRow->clear();
    eventsRow->clear();
    categoryStreamsRow->clear();
    categoriesRow->clear();
    searchRow->clear();
    followedCursor.clear();
    popularCursor.clear();
    popularClipsCursor.clear();
    categoriesCursor.clear();
    searchCursor.clear();
    categoryStreamsCursor.clear();
}
