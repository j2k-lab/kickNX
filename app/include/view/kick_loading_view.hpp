#pragma once

#include <borealis.hpp>

namespace kick {

class KickLoadingView : public brls::View {
public:
    KickLoadingView();
    ~KickLoadingView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;

    static brls::View* create();

private:
    brls::Image* logoImage = nullptr;
    brls::Image* shineImage = nullptr;
};

} // namespace kick
