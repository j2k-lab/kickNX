#pragma once

#include "api/kick_rewards.hpp"

#include <string>

namespace kicknx {

/// The card shown after claiming Kick's Daily Reward.
void showClaimedRewardDialog(const kick::ClaimedReward& prize);

/// "5 hr 12 min" until the given Unix time (UTC); empty when unknown or past.
std::string timeUntil(int64_t unixSeconds);

}  // namespace kicknx
