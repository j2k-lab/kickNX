#include "view/reward_dialog.hpp"

#include "utils/image.hpp"

#include <borealis.hpp>

#include <cctype>
#include <ctime>

namespace kicknx {

void showClaimedRewardDialog(const kick::ClaimedReward& prize) {
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setAlignItems(brls::AlignItems::CENTER);
    content->setPadding(30, 30, 20, 30);

    auto* title = new brls::Label();
    title->setText(prize.duplicate ? "You already had this one" : "You got a reward!");
    title->setFontSize(24);
    title->setMarginBottom(16);
    content->addView(title);

    if (!prize.cardUrl.empty()) {
        auto* card = new brls::Image();
        card->setDimensions(220, 220);
        card->setScalingType(brls::ImageScalingType::FIT);
        card->setMarginBottom(14);
        Image::with(card, prize.cardUrl);
        content->addView(card);
    }

    std::string details = prize.name;
    if (!prize.rarity.empty()) {
        std::string rarity = prize.rarity;
        rarity[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rarity[0])));
        details = details.empty() ? rarity : rarity + " · " + details;
    }
    if (prize.watchTimeMinutes > 0) {
        const std::string bonus = fmt::format("+{} min watch time", prize.watchTimeMinutes);
        details = details.empty() ? bonus : details + "\n" + bonus;
    }
    if (details.empty()) details = "Find it in your Collectibles emotes.";

    auto* label = new brls::Label();
    label->setText(details);
    label->setFontSize(18);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    content->addView(label);

    auto* dialog = new brls::Dialog(content);
    dialog->addButton("OK", []() {});
    dialog->open();
}

std::string timeUntil(int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const int64_t left = unixSeconds - static_cast<int64_t>(std::time(nullptr));
    if (left <= 0) return {};
    const int64_t minutes = (left + 59) / 60;
    const int64_t hours = minutes / 60;
    if (hours == 0) return fmt::format("{} min", minutes);
    return fmt::format("{} hr {} min", hours, minutes % 60);
}

}  // namespace kicknx
