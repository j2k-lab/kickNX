#pragma once

#include "api/kick_helix.hpp"

#include <borealis.hpp>

#include <string>

class KickNXDetails : public brls::Box {
public:
    explicit KickNXDetails(kick::Stream stream);
    ~KickNXDetails() override;

private:
    void chooseQuality();
    void updateQualityLabel();
    void watchLive();
    void setBusy(bool value, const std::string& message = "");

    kick::Stream stream;
    std::string preferredQuality;
    bool busy = false;

    brls::Image* preview = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* qualityLabel = nullptr;
    brls::Box* watchButton = nullptr;
};
