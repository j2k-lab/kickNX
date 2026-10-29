#include "activity/kick_channel.hpp"
#include "activity/kicknx_home.hpp"

#include "api/kick.hpp"
#include "api/kick_helix.hpp"
#include "tab/remote_view.hpp"
#include "utils/image.hpp"
#include "view/h_recycling.hpp"
#include "view/recycling_grid.hpp"

#include <borealis/views/dialog.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

std::string compactNumber(int value) {
    if (value >= 1000000)
        return fmt::format(
            "{:.1f}M",
            value / 1000000.0);
    if (value >= 1000)
        return fmt::format(
            "{:.1f}K",
            value / 1000.0);
    return std::to_string(value);
}

std::string joinTags(
    const std::vector<std::string>& tags) {
    std::string result;
    const size_t count =
        std::min<size_t>(tags.size(), 6);
    for (size_t index = 0;
         index < count;
         ++index) {
        if (!result.empty())
            result += "  ·  ";
        result += tags[index];
    }
    return result;
}

bool isSelfChannel(
    const std::string& channelLogin,
    const std::string& profileLogin = "",
    const std::string& profileId = "") {
    const auto session = kick::loadOAuth();
    if (!session.signedIn()) return false;

    auto normalize = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c != '@' && !std::isspace(static_cast<unsigned char>(c)))
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };

    const std::string myLogin = normalize(session.login);
    if (!myLogin.empty()) {
        if (!channelLogin.empty() && myLogin == normalize(channelLogin)) return true;
        if (!profileLogin.empty() && myLogin == normalize(profileLogin)) return true;
    }
    if (!session.userId.empty() && !profileId.empty() && session.userId == profileId) {
        return true;
    }
    return false;
}

class CircularAvatar : public brls::View {
public:
    CircularAvatar() {
        loader = new brls::Image();
        loader->setFreeTexture(false);
    }

    ~CircularAvatar() override {
        Image::cancel(loader);
        delete loader;
    }

    void setUrl(const std::string& url) {
        Image::cancel(loader);
        currentUrl = url;
        if (!currentUrl.empty())
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
        const float size =
            std::min(width, height);
        const float cx =
            x + width * 0.5f;
        const float cy =
            y + height * 0.5f;
        const float radius =
            size * 0.5f - 2.0f;

        nvgSave(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius);
        nvgFillColor(
            vg,
            nvgRGB(24, 115, 36));
        nvgFill(vg);

        if (loader->getTexture() > 0) {
            NVGpaint paint =
                nvgImagePattern(
                    vg,
                    cx - radius,
                    cy - radius,
                    radius * 2.0f,
                    radius * 2.0f,
                    0,
                    loader->getTexture(),
                    1.0f);
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, radius);
            nvgFillPaint(vg, paint);
            nvgFill(vg);
        }

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius);
        nvgStrokeColor(
            vg,
            nvgRGBA(255, 255, 255, 100));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
        nvgRestore(vg);
    }

private:
    brls::Image* loader = nullptr;
    std::string currentUrl;
};

enum class MediaKind { Details, Vod, Clip };

struct MediaCard {
    MediaKind kind = MediaKind::Details;
    std::string id;
    std::string title;
    std::string subtitle;
    std::string thumbnail;
    std::string details;
};

class MediaCell : public RecyclingGridItem {
public:
    MediaCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(
            nvgRGB(31, 35, 46));
        this->setPadding(0, 0, 10, 0);

        image = new brls::Image();
        image->setHeight(122);
        image->setWidthPercentage(100);
        image->setScalingType(brls::ImageScalingType::FILL);
        image->setImageFromRes(
            "img/video-card-bg.png");
        this->addView(image);

        title = new brls::Label();
        title->setFontSize(16);
        title->setSingleLine(true);
        title->setMarginTop(7);
        title->setMarginLeft(9);
        title->setMarginRight(9);
        title->setTextColor(
            nvgRGB(245, 246, 250));
        this->addView(title);

        subtitle = new brls::Label();
        subtitle->setFontSize(13);
        subtitle->setSingleLine(true);
        subtitle->setMarginTop(3);
        subtitle->setMarginLeft(9);
        subtitle->setMarginRight(9);
        subtitle->setTextColor(
            nvgRGB(164, 173, 192));
        this->addView(subtitle);
    }

    ~MediaCell() override {
        Image::cancel(image);
    }

    void prepareForReuse() override {
        Image::cancel(image);
        image->setImageFromRes(
            "img/video-card-bg.png");
        title->setText("");
        subtitle->setText("");
    }

    void cacheForReuse() override {
        Image::cancel(image);
    }

    std::function<brls::View*()> upTarget;
    std::function<brls::View*()> downTarget;

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::UP && upTarget) {
            brls::View* target = upTarget();
            if (target)
                return target;
        }
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            brls::View* target = downTarget();
            if (target)
                return target;
        }
        return RecyclingGridItem::getNextFocus(direction, currentView);
    }

    brls::Image* image = nullptr;
    brls::Label* title = nullptr;
    brls::Label* subtitle = nullptr;
};

class MediaDataSource :
    public RecyclingGridDataSource {
public:
    using Selection =
        std::function<void(const MediaCard&)>;

    MediaDataSource(
        std::vector<MediaCard> items,
        Selection selected,
        std::function<brls::View*()> upTarget = nullptr,
        std::function<brls::View*()> downTarget = nullptr)
        : items(std::move(items)),
          selected(std::move(selected)),
          upTarget(std::move(upTarget)),
          downTarget(std::move(downTarget)) {}

    size_t getItemCount() override {
        return items.size();
    }

    RecyclingGridItem* cellForRow(
        RecyclingView* recycler,
        size_t index) override {
        auto* cell =
            dynamic_cast<MediaCell*>(
                recycler->dequeueReusableCell(
                    "ChannelMedia"));
        const auto& item = items.at(index);
        cell->title->setText(item.title);
        cell->subtitle->setText(
            item.subtitle);
        if (!item.thumbnail.empty())
            Image::with(
                cell->image,
                item.thumbnail);

        cell->upTarget = this->upTarget;
        cell->downTarget = this->downTarget;
        return cell;
    }

    void onItemSelected(
        brls::Box*,
        size_t index) override {
        if (selected)
            selected(items.at(index));
    }

    void clearData() override {
        items.clear();
    }

private:
    std::vector<MediaCard> items;
    Selection selected;
    std::function<brls::View*()> upTarget;
    std::function<brls::View*()> downTarget;
};

ChannelActionButton* makeButton(
    const std::string& text,
    brls::Label** output = nullptr) {
    auto* button = new ChannelActionButton();
    button->setAxis(brls::Axis::ROW);
    button->setFocusable(true);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(
        nvgRGB(24, 115, 36));
    button->setMarginRight(12);
    button->addGestureRecognizer(
        new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(18);
    label->setTextColor(
        nvgRGB(250, 250, 252));
    button->addView(label);

    if (output)
        *output = label;
    return button;
}

brls::Box* createMediaSection(
    const std::string& titleText,
    const std::vector<MediaCard>& cards,
    const std::string& emptyText,
    std::function<void(const MediaCard&)> selected,
    HRecyclerFrame** outRecycler = nullptr,
    std::function<brls::View*()> upTarget = nullptr,
    std::function<brls::View*()> downTarget = nullptr) {
    if (outRecycler)
        *outRecycler = nullptr;

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setMarginTop(20);

    auto* header = new brls::Header();
    header->setTitle(titleText);
    header->setHeight(38);
    section->addView(header);

    if (cards.empty()) {
        auto* empty = new brls::Label();
        empty->setText(emptyText);
        empty->setFontSize(16);
        empty->setTextColor(
            nvgRGB(165, 174, 193));
        empty->setMarginTop(14);
        empty->setMarginBottom(14);
        empty->setMarginLeft(4);
        empty->setMarginRight(4);
        section->addView(empty);
        return section;
    }

    auto* row = new HRecyclerFrame();
    row->setHeight(190);
    row->estimatedRowWidth = 285;
    row->estimatedRowSpace = 16;
    row->setPadding(0, 4, 0, 4);
    row->registerCell(
        "ChannelMedia",
        []() {
            return new MediaCell();
        });
    row->setDataSource(
        new MediaDataSource(
            cards,
            std::move(selected),
            std::move(upTarget),
            std::move(downTarget)));
    section->addView(row);
    if (outRecycler)
        *outRecycler = row;
    return section;
}

class ChannelCategoryCell : public RecyclingGridItem {
public:
    ChannelCategoryCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setCornerRadius(8);
        this->setBackgroundColor(nvgRGB(32, 35, 45));
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
        title->setTextColor(nvgRGB(245, 246, 250));
        this->addView(title);
    }

    ~ChannelCategoryCell() override {
        Image::cancel(picture);
    }

    void prepareForReuse() override {
        Image::cancel(picture);
        picture->setScalingType(brls::ImageScalingType::FILL);
        picture->setImageFromRes("img/category-card-bg.png");
        title->setText("");
    }

    void cacheForReuse() override {
        Image::cancel(picture);
    }

    std::function<brls::View*()> upTarget;
    std::function<brls::View*()> downTarget;

    brls::View* getNextFocus(
        brls::FocusDirection direction,
        brls::View* currentView) override {
        if (direction == brls::FocusDirection::UP && upTarget) {
            brls::View* target = upTarget();
            if (target)
                return target;
        }
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            brls::View* target = downTarget();
            if (target)
                return target;
        }
        return RecyclingGridItem::getNextFocus(direction, currentView);
    }

    brls::Image* picture = nullptr;
    brls::Label* title = nullptr;
};

class ChannelCategoryDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const kick::Category&)>;

    ChannelCategoryDataSource(
        std::vector<kick::Category> items,
        Selection selected,
        std::function<brls::View*()> upTarget = nullptr,
        std::function<brls::View*()> downTarget = nullptr)
        : items(std::move(items)),
          selected(std::move(selected)),
          upTarget(std::move(upTarget)),
          downTarget(std::move(downTarget)) {}

    size_t getItemCount() override {
        return items.size();
    }

    RecyclingGridItem* cellForRow(
        RecyclingView* recycler,
        size_t index) override {
        auto* cell =
            dynamic_cast<ChannelCategoryCell*>(
                recycler->dequeueReusableCell(
                    "ChannelCategory"));
        const auto& item = items.at(index);
        cell->title->setText(item.name);
        if (!item.boxArtUrl.empty() &&
            (item.boxArtUrl.rfind("http://", 0) == 0 || item.boxArtUrl.rfind("https://", 0) == 0 || item.boxArtUrl.rfind("//", 0) == 0)) {
            cell->picture->setScalingType(brls::ImageScalingType::FIT);
            Image::with(
                cell->picture,
                kick::categoryThumbnail(item.boxArtUrl, 188, 250));
        } else {
            cell->picture->setScalingType(brls::ImageScalingType::FILL);
            Image::cancel(cell->picture);
            cell->picture->setImageFromRes("img/category-card-bg.png");
        }

        cell->upTarget = this->upTarget;
        cell->downTarget = this->downTarget;
        return cell;
    }

    void onItemSelected(
        brls::Box*,
        size_t index) override {
        if (selected)
            selected(items.at(index));
    }

    void clearData() override {
        items.clear();
    }

private:
    std::vector<kick::Category> items;
    Selection selected;
    std::function<brls::View*()> upTarget;
    std::function<brls::View*()> downTarget;
};

brls::Box* createCategorySection(
    const std::string& titleText,
    const std::vector<kick::Category>& categories,
    const std::string& emptyText,
    std::function<void(const kick::Category&)> selected,
    HRecyclerFrame** outRecycler = nullptr,
    std::function<brls::View*()> upTarget = nullptr,
    std::function<brls::View*()> downTarget = nullptr) {
    if (outRecycler)
        *outRecycler = nullptr;

    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setMarginTop(20);

    auto* header = new brls::Header();
    header->setTitle(titleText);
    header->setHeight(38);
    section->addView(header);

    if (categories.empty()) {
        auto* empty = new brls::Label();
        empty->setText(emptyText);
        empty->setFontSize(16);
        empty->setTextColor(
            nvgRGB(165, 174, 193));
        empty->setMarginTop(14);
        empty->setMarginBottom(14);
        empty->setMarginLeft(4);
        empty->setMarginRight(4);
        section->addView(empty);
        return section;
    }

    auto* row = new HRecyclerFrame();
    row->setHeight(285);
    row->estimatedRowWidth = 175;
    row->estimatedRowSpace = 18;
    row->setPadding(0, 4, 0, 4);
    row->registerCell(
        "ChannelCategory",
        []() {
            return new ChannelCategoryCell();
        });
    row->setDataSource(
        new ChannelCategoryDataSource(
            categories,
            std::move(selected),
            std::move(upTarget),
            std::move(downTarget)));
    section->addView(row);
    if (outRecycler)
        *outRecycler = row;
    return section;
}

brls::View* getRecyclerFocus(HRecyclerFrame* recycler) {
    if (!recycler)
        return nullptr;
    brls::View* focus = recycler->getDefaultFocus();
    if (focus)
        return focus;
    for (auto* child : recycler->getChildren()) {
        if (!child)
            continue;
        if (child->isFocusable() &&
            child->getVisibility() == brls::Visibility::VISIBLE)
            return child;
        if (auto* box = dynamic_cast<brls::Box*>(child)) {
            for (auto* grandchild : box->getChildren()) {
                if (grandchild && grandchild->isFocusable() &&
                    grandchild->getVisibility() == brls::Visibility::VISIBLE)
                    return grandchild;
            }
        }
    }
    return nullptr;
}

}  // namespace

KickChannelPage::KickChannelPage(
    std::string channelLogin)
    : channelLogin(std::move(channelLogin)),
      requestCancel(
          std::make_shared<std::atomic_bool>(
              false)) {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    this->setBackgroundColor(
        nvgRGB(0, 0, 0));

    scroll = new brls::ScrollingFrame();
    scroll->setDimensions(
        brls::Application::contentWidth,
        brls::Application::contentHeight);
    scroll->setScrollingBehavior(
        brls::ScrollingBehavior::CENTERED);
    scroll->setFocusable(false);
    scroll->setBackgroundColor(
        nvgRGB(0, 0, 0));
    this->addView(scroll);

    content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100);
    content->setBackgroundColor(
        nvgRGB(0, 0, 0));
    content->setPadding(0, 0, 0, 0);
    scroll->setContentView(content);

    headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::COLUMN);
    headerBox->setWidthPercentage(100);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setBackgroundColor(
        nvgRGB(16, 18, 24));
    headerBox->setPadding(14, 42, 24, 42);
    content->addView(headerBox);

    auto* topBar = new brls::Box();
    topBar->setAxis(brls::Axis::ROW);
    topBar->setWidthPercentage(100);
    topBar->setAlignItems(brls::AlignItems::CENTER);
    topBar->setMarginBottom(12);
    headerBox->addView(topBar);

    backButton = new ChannelActionButton();
    backButton->setId("channel/back");
    backButton->setAxis(brls::Axis::ROW);
    backButton->setFocusable(true);
    backButton->setPadding(8, 18, 8, 18);
    backButton->setCornerRadius(8);
    backButton->setBackgroundColor(nvgRGB(24, 115, 36));
    backButton->addGestureRecognizer(new brls::TapGestureRecognizer(backButton));
    backButton->registerClickAction([this](brls::View*) {
        markClosing();
        return brls::Application::popActivity();
    });
    backButton->downTarget = [this]() {
        return getActionFocus();
    };

    auto* backLabel = new brls::Label();
    backLabel->setText("<  Back");
    backLabel->setFontSize(16);
    backLabel->setTextColor(nvgRGB(255, 255, 255));
    backButton->addView(backLabel);
    topBar->addView(backButton);

    offlineImage = new brls::Image();
    offlineImage->setHeight(215);
    offlineImage->setWidthPercentage(100);
    offlineImage->setCornerRadius(8);
    offlineImage->setMarginTop(0);
    offlineImage->setImageFromRes(
        "img/video-card-bg.png");
    offlineImage->setVisibility(
        brls::Visibility::GONE);
    headerBox->addView(offlineImage);

    identity = new brls::Box();
    identity->setAxis(brls::Axis::COLUMN);
    identity->setAlignItems(brls::AlignItems::CENTER);
    identity->setJustifyContent(brls::JustifyContent::CENTER);
    identity->setWidthPercentage(100);
    identity->setMarginTop(16);
    headerBox->addView(identity);

    avatar = new CircularAvatar();
    avatar->setWidth(116);
    avatar->setHeight(116);
    avatar->setMarginBottom(12);
    identity->addView(avatar);

    name = new brls::Label();
    name->setText(channelLogin);
    name->setFontSize(32);
    name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    name->setTextColor(
        nvgRGB(245, 246, 250));
    identity->addView(name);

    liveState = new brls::Label();
    liveState->setText("Checking status…");
    liveState->setFontSize(17);
    liveState->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    liveState->setTextColor(
        nvgRGB(172, 180, 199));
    liveState->setMarginTop(6);
    identity->addView(liveState);

    status = new brls::Label();
    status->setText("Loading channel…");
    status->setFontSize(15);
    status->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status->setTextColor(
        nvgRGB(164, 173, 192));
    status->setMarginTop(4);
    identity->addView(status);

    headline = new brls::Label();
    headline->setFontSize(22);
    headline->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    headline->setTextColor(
        nvgRGB(238, 239, 244));
    headline->setMarginTop(14);
    headline->setWidthPercentage(90);
    headerBox->addView(headline);

    metadata = new brls::Label();
    metadata->setFontSize(16);
    metadata->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    metadata->setTextColor(
        nvgRGB(166, 176, 195));
    metadata->setMarginTop(8);
    headerBox->addView(metadata);

    description = new brls::Label();
    description->setFontSize(16);
    description->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    description->setTextColor(
        nvgRGB(199, 204, 216));
    description->setMarginTop(12);
    description->setWidthPercentage(85);
    headerBox->addView(description);

    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    actions->setJustifyContent(brls::JustifyContent::CENTER);
    actions->setAlignItems(brls::AlignItems::CENTER);
    actions->setMarginTop(18);
    headerBox->addView(actions);

    playButton =
        makeButton(
            "Watch live",
            &playButtonLabel);
    playButton->setId("channel/watch-live");
    playButton->setMarginLeft(8);
    playButton->setMarginRight(8);
    playButton->setVisibility(
        brls::Visibility::GONE);
    playButton->registerClickAction(
        [this](brls::View*) {
            playLive();
            return true;
        });
    playButton->upTarget = [this]() {
        return backButton;
    };
    actions->addView(playButton);

    followButton = makeButton("Follow", &followButtonLabel);
    followButton->setId("channel/follow");
    followButton->setMarginLeft(8);
    followButton->setMarginRight(8);
    followButton->registerClickAction([this](brls::View*) {
        toggleFollow();
        return true;
    });
    followButton->upTarget = [this]() {
        return backButton;
    };
    if (isSelfChannel(channelLogin)) {
        followButton->setVisibility(brls::Visibility::GONE);
    }
    actions->addView(followButton);

    refreshButton =
        makeButton("Refresh");
    refreshButton->setId("channel/refresh");
    refreshButton->setMarginLeft(8);
    refreshButton->setMarginRight(8);
    refreshButton->registerClickAction(
        [this](brls::View*) {
            load();
            return true;
        });
    refreshButton->upTarget = [this]() {
        return backButton;
    };
    actions->addView(refreshButton);

    bodyBox = new brls::Box();
    bodyBox->setAxis(brls::Axis::COLUMN);
    bodyBox->setWidthPercentage(100);
    bodyBox->setBackgroundColor(
        nvgRGB(0, 0, 0));
    bodyBox->setPadding(8, 42, 36, 42);
    content->addView(bodyBox);

    videosHolder = new brls::Box();
    videosHolder->setAxis(brls::Axis::COLUMN);
    bodyBox->addView(videosHolder);

    clipsHolder = new brls::Box();
    clipsHolder->setAxis(brls::Axis::COLUMN);
    bodyBox->addView(clipsHolder);

    categoriesHolder = new brls::Box();
    categoriesHolder->setAxis(
        brls::Axis::COLUMN);
    bodyBox->addView(categoriesHolder);

    this->registerAction(
        "Back",
        brls::BUTTON_B,
        [this](brls::View*) {
            markClosing();
            return brls::Application::popActivity();
        });

    load();
}

KickChannelPage::~KickChannelPage() {
    closing = true;
    if (requestCancel)
        requestCancel->store(true);
    Image::cancel(offlineImage);
}

void KickChannelPage::markClosing() {
    closing = true;
    if (requestCancel)
        requestCancel->store(true);
}

void KickChannelPage::willDisappear(bool resetState) {
    // resetState is true only when the activity is being popped for good.
    if (resetState)
        markClosing();
    brls::Box::willDisappear(resetState);
}

void KickChannelPage::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (status) {
        status->setText("");
    }
}

brls::View* KickChannelPage::getActionFocus() {
    if (data.live && playButton &&
        playButton->getVisibility() ==
            brls::Visibility::VISIBLE)
        return playButton;
    if (followButton &&
        followButton->getVisibility() ==
            brls::Visibility::VISIBLE)
        return followButton;
    return refreshButton;
}

brls::View* KickChannelPage::getFirstRowFocus() {
    if (auto* v = getRecyclerFocus(videosRecycler))
        return v;
    if (auto* c = getRecyclerFocus(clipsRecycler))
        return c;
    if (auto* cat = getRecyclerFocus(categoriesRecycler))
        return cat;
    return nullptr;
}

brls::View* KickChannelPage::getDefaultFocus() {
    return getActionFocus();
}

void KickChannelPage::load() {
    followPending = false;
    if (requestCancel)
        requestCancel->store(true);
    requestCancel =
        std::make_shared<std::atomic_bool>(
            false);

    status->setText("Loading channel…");
    this->ptrLock();

    // Each request owns its cancel token. A superseded (refresh) or abandoned
    // (page left) request must never write to the UI, so its callbacks only
    // release the pointer lock.
    auto token = requestCancel;
    kick::loadChannelPageAsync(
        kick::loadConfig(),
        channelLogin,
        [this, token](kick::ChannelPageData loaded) {
            if (closing || (token && token->load())) {
                this->ptrUnlock();
                return;
            }
            data = std::move(loaded);
            showData(data);
            this->ptrUnlock();
        },
        [this, token](const std::string& error) {
            if (closing || (token && token->load())) {
                this->ptrUnlock();
                return;
            }
            status->setText(
                "Channel error: " + error);
            brls::Application::notify(
                "kickNX: " + error);
            this->ptrUnlock();
        },
        requestCancel);
}

void KickChannelPage::showData(
    kick::ChannelPageData loaded) {
    // Dynamic sections own recyclable cells. Move focus to a permanent
    // control before clearing them so Borealis never retains a detached view.
    if (refreshButton)
        brls::Application::giveFocus(refreshButton);

    const std::string displayName =
        loaded.profile.displayName.empty()
            ? loaded.profile.login
            : loaded.profile.displayName;

    name->setText(displayName);
    static_cast<CircularAvatar*>(avatar)->setUrl(
        loaded.profile.profileImageUrl);

    if (!loaded.profile.offlineImageUrl.empty()) {
        offlineImage->setVisibility(
            brls::Visibility::VISIBLE);
        Image::with(
            offlineImage,
            loaded.profile.offlineImageUrl);
        if (identity)
            identity->setMarginTop(16);
    } else {
        Image::cancel(offlineImage);
        offlineImage->setVisibility(
            brls::Visibility::GONE);
        if (identity)
            identity->setMarginTop(28);
    }

    if (loaded.live) {
        liveState->setText(
            "LIVE · " +
            compactNumber(
                loaded.liveStream.viewerCount) +
            " viewers");
        liveState->setTextColor(
            nvgRGB(83, 252, 24));
        headline->setText(
            loaded.liveStream.title.empty()
                ? loaded.information.title
                : loaded.liveStream.title);
        playButton->setVisibility(
            brls::Visibility::VISIBLE);
    } else {
        liveState->setText("OFFLINE");
        liveState->setTextColor(
            nvgRGB(177, 183, 198));
        headline->setText(
            loaded.information.title);
        playButton->setVisibility(
            brls::Visibility::GONE);
    }

    std::string meta =
        loaded.information.gameName;
    if (!loaded.information
             .broadcasterLanguage.empty()) {
        if (!meta.empty())
            meta += "  ·  ";
        meta += loaded.information
                    .broadcasterLanguage;
    }

    const std::string tags =
        joinTags(loaded.information.tags);
    if (!tags.empty()) {
        if (!meta.empty())
            meta += "\n";
        meta += tags;
    }
    metadata->setText(meta);

    description->setText(
        loaded.profile.description.empty()
            ? "This channel has no description."
            : loaded.profile.description);

    status->setText("");

    const bool isSelf = isSelfChannel(channelLogin, loaded.profile.login, loaded.profile.id);
    if (followButton) {
        followButton->setVisibility(isSelf ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    if (!isSelf) {
        following = loaded.isFollowing;
        updateFollowButton(following);
    }

    videosHolder->clearViews();
    clipsHolder->clearViews();
    categoriesHolder->clearViews();
    videosRecycler = nullptr;
    clipsRecycler = nullptr;
    categoriesRecycler = nullptr;

    auto resetScrollTop = [this]() {
        if (scroll) scroll->setContentOffsetY(0.0f, false);
    };

    auto actionDown = [this]() -> brls::View* {
        return getFirstRowFocus();
    };
    if (playButton) {
        playButton->downTarget = actionDown;
        playButton->getFocusEvent()->subscribe([resetScrollTop](brls::View*) { resetScrollTop(); });
    }
    if (followButton) {
        followButton->downTarget = actionDown;
        followButton->getFocusEvent()->subscribe([resetScrollTop](brls::View*) { resetScrollTop(); });
    }
    if (refreshButton) {
        refreshButton->downTarget = actionDown;
        refreshButton->getFocusEvent()->subscribe([resetScrollTop](brls::View*) { resetScrollTop(); });
    }

    auto videosUp = [this, resetScrollTop]() -> brls::View* {
        resetScrollTop();
        return getActionFocus();
    };
    auto videosDown = [this]() -> brls::View* {
        if (auto* c = getRecyclerFocus(clipsRecycler))
            return c;
        if (auto* cat = getRecyclerFocus(categoriesRecycler))
            return cat;
        return nullptr;
    };

    auto clipsUp = [this, resetScrollTop]() -> brls::View* {
        if (auto* v = getRecyclerFocus(videosRecycler))
            return v;
        resetScrollTop();
        return getActionFocus();
    };
    auto clipsDown = [this]() -> brls::View* {
        if (auto* cat = getRecyclerFocus(categoriesRecycler))
            return cat;
        return nullptr;
    };

    auto categoriesUp = [this, resetScrollTop]() -> brls::View* {
        if (auto* c = getRecyclerFocus(clipsRecycler))
            return c;
        if (auto* v = getRecyclerFocus(videosRecycler))
            return v;
        resetScrollTop();
        return getActionFocus();
    };
    auto categoriesDown = []() -> brls::View* {
        return nullptr;
    };

    std::vector<MediaCard> videos;
    for (const auto& video :
         loaded.videos) {
        MediaCard card;
        card.kind = MediaKind::Vod;
        card.id = video.id;
        card.title = video.title;
        card.subtitle =
            video.duration +
            "  ·  " +
            compactNumber(video.viewCount) +
            " views";
        card.thumbnail =
            kick::mediaThumbnail(
                video.thumbnailUrl,
                320,
                180);
        card.details =
            video.title +
            "\n\nDuration: " +
            video.duration +
            "\nViews: " +
            std::to_string(video.viewCount) +
            "\nPublished: " +
            video.publishedAt +
            "\n\nPress A to play this broadcast.";
        videos.push_back(std::move(card));
    }

    videosHolder->addView(
        createMediaSection(
            "Recent broadcasts",
            videos,
            loaded.videosError.empty()
                ? "No recent broadcasts were returned."
                : "Recent broadcasts are unavailable.",
            [this](const MediaCard& card) {
                if (card.kind == MediaKind::Vod)
                    playVod(card.id, card.title, card.details);
                else
                    showMediaDetails(card.title, card.details);
            },
            &videosRecycler,
            videosUp,
            videosDown));

    std::vector<MediaCard> clips;
    for (const auto& clip :
         loaded.clips) {
        MediaCard card;
        card.kind = MediaKind::Clip;
        card.id = clip.id;
        card.title = clip.title;
        card.subtitle =
            compactNumber(clip.viewCount) +
            " views  ·  " +
            clip.creatorName;
        card.thumbnail =
            clip.thumbnailUrl;
        card.details =
            clip.title +
            "\n\nCreator: " +
            clip.creatorName +
            "\nViews: " +
            std::to_string(clip.viewCount) +
            "\nDuration: " +
            fmt::format(
                "{:.1f}s",
                clip.duration) +
            "\nCreated: " +
            clip.createdAt +
            "\n\nPress A to play this clip.";
        clips.push_back(std::move(card));
    }

    clipsHolder->addView(
        createMediaSection(
            "Clips",
            clips,
            loaded.clipsError.empty()
                ? "No recent clips were returned."
                : "Clips are unavailable.",
            [this](const MediaCard& card) {
                if (card.kind == MediaKind::Clip)
                    playClip(card.id, card.title, card.details);
                else
                    showMediaDetails(card.title, card.details);
            },
            &clipsRecycler,
            clipsUp,
            clipsDown));

    categoriesHolder->addView(
        createCategorySection(
            "Channel category",
            loaded.categories,
            "No current category information.",
            [this](const kick::Category& category) {
                if (auto* home = KickNXHome::getInstance()) {
                    markClosing();
                    brls::Application::popActivity();
                    home->openCategoryByName(category);
                } else {
                    showMediaDetails(
                        category.name,
                        category.name);
                }
            },
            &categoriesRecycler,
            categoriesUp,
            categoriesDown));

    content->invalidate();
    scroll->setContentOffsetY(0, false);

    brls::Application::giveFocus(getActionFocus());
}

void KickChannelPage::playLive() {
    if (!data.live) {
        brls::Application::notify(
            "This channel is currently offline");
        return;
    }

    auto config = kick::loadConfig();
    config.channel =
        data.profile.login.empty()
            ? channelLogin
            : data.profile.login;
    config.preferredQuality =
        kick::loadPreferredQuality();
    if (config.preferredQuality.empty())
        config.preferredQuality = "1080p60";

    RemoteView::playLive(config, data.liveStream.uptimeSeconds);
}


void KickChannelPage::playVod(
    const std::string& id,
    const std::string& title,
    const std::string& details) {
    RemoteView::playVod(id, title, details);
}

void KickChannelPage::playClip(
    const std::string& slug,
    const std::string& title,
    const std::string& details) {
    RemoteView::playClip(slug, title, details);
}

void KickChannelPage::showMediaDetails(
    const std::string& title,
    const std::string& details) {
    auto* dialog = new brls::Dialog(
        details.empty()
            ? title
            : details);
    dialog->addButton("OK", []() {});
    dialog->open();
}

void KickChannelPage::updateFollowButton(bool isFollowing) {
    if (!followButton || !followButtonLabel) return;
    if (isFollowing) {
        followButtonLabel->setText("Following");
        followButton->setBackgroundColor(nvgRGB(50, 56, 70));
    } else {
        followButtonLabel->setText("Follow");
        followButton->setBackgroundColor(nvgRGB(24, 115, 36));
    }
}

void KickChannelPage::toggleFollow() {
    if (followPending) return;
    if (isSelfChannel(channelLogin, data.profile.login, data.profile.id)) return;
    const auto session = kick::loadOAuth();
    if (!session.signedIn()) {
        brls::Application::notify("Sign in to follow channels on Kick.");
        return;
    }

    followPending = true;
    followButtonLabel->setText(following ? "Unfollowing…" : "Following…");

    const bool newFollow = !following;
    this->ptrLock();
    kick::setChannelFollowAsync(
        kick::loadConfig(),
        channelLogin,
        newFollow,
        [this, newFollow](bool confirmed) {
            if (closing) {
                this->ptrUnlock();
                return;
            }
            followPending = false;
            following = confirmed;
            updateFollowButton(following);
            brls::Application::notify(
                confirmed ? ("Followed " + channelLogin) : ("Unfollowed " + channelLogin));
            this->ptrUnlock();
        },
        [this](const std::string& error) {
            if (closing) {
                this->ptrUnlock();
                return;
            }
            followPending = false;
            updateFollowButton(following);
            brls::Application::notify("Could not update follow: " + error);
            this->ptrUnlock();
        });
}
