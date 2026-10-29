#include "view/kick_loading_view.hpp"

namespace kick {

KickLoadingView::KickLoadingView() {
    this->setWidth(200.0f);
    this->setHeight(90.0f);
    logoImage = new brls::Image();
    logoImage->setImageFromRes("img/kicknx_logo.png");
    shineImage = new brls::Image();
    shineImage->setImageFromRes("img/kicknx_logo_shine.png");
}

KickLoadingView::~KickLoadingView() {
    if (logoImage) {
        delete logoImage;
        logoImage = nullptr;
    }
    if (shineImage) {
        delete shineImage;
        shineImage = nullptr;
    }
}

void KickLoadingView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    int tex = logoImage ? logoImage->getTexture() : 0;
    if (tex <= 0 && logoImage) {
        logoImage->setImageFromRes("img/kicknx_logo.png");
        tex = logoImage->getTexture();
    }

    const float cx = x + width * 0.5f;
    const float cy = y + height * 0.5f;

    // Smooth continuous animation cycle (1.4s loop)
    const uint64_t now = brls::getCPUTimeUsec();
    const float cycle = static_cast<float>(now % 1400000) / 1400000.0f;

    // Dimensions for the kickNX logo (natural aspect ~3.46)
    const float logoW = 160.0f;
    const float logoH = logoW * (186.0f / 644.0f); // ~46.2px
    const float logoX = cx - logoW * 0.5f;
    const float logoY = cy - logoH * 0.5f - 8.0f;

    if (tex > 0) {
        // Draw crisp 3D kickNX logo (white lettering with 3D black drop shadow)
        NVGpaint imgPaint = nvgImagePattern(vg, logoX, logoY, logoW, logoH, 0.0f, tex, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, logoX, logoY, logoW, logoH);
        nvgFillPaint(vg, imgPaint);
        nvgFill(vg);

        // Lime shimmer: the shine texture contains only the letter faces
        // (no shadow), so it lines up exactly with the text. A soft band is
        // built from thin scissor slices whose opacity follows a bell curve.
        const int shineTex = shineImage ? shineImage->getTexture() : 0;
        if (shineTex > 0) {
            const float bandW = logoW * 0.30f;
            const float bandCenter = (logoX - bandW) + cycle * (logoW + bandW * 2.0f);
            const int slices = 24;
            const float sliceW = bandW * 2.0f / slices;
            for (int i = 0; i < slices; i++) {
                const float sx = bandCenter - bandW + i * sliceW;
                const float t = ((i + 0.5f) / slices) * 2.0f - 1.0f;  // -1..1
                const float alpha = (1.0f - t * t) * (1.0f - t * t);  // smooth bell
                if (alpha <= 0.01f) continue;
                nvgSave(vg);
                nvgScissor(vg, sx, logoY, sliceW + 0.5f, logoH);
                nvgIntersectScissor(vg, logoX, logoY, logoW, logoH);
                NVGpaint shinePaint = nvgImagePattern(vg, logoX, logoY, logoW, logoH, 0.0f, shineTex, alpha);
                nvgBeginPath(vg);
                nvgRect(vg, logoX, logoY, logoW, logoH);
                nvgFillPaint(vg, shinePaint);
                nvgFill(vg);
                nvgRestore(vg);
            }
        }
    }

    // 3. Sleek neon buffering bar underneath the logo
    const float barW = 130.0f;
    const float barH = 3.5f;
    const float barX = cx - barW * 0.5f;
    const float barY = logoY + logoH + 11.0f;

    // Faint background track
    nvgBeginPath(vg);
    nvgRoundedRect(vg, barX, barY, barW, barH, barH * 0.5f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 25));
    nvgFill(vg);

    // Glowing active moving segment
    const float activeW = barW * 0.38f;
    const float activeX = (barX - activeW) + cycle * (barW + activeW * 2.0f);

    nvgSave(vg);
    nvgScissor(vg, barX, barY, barW, barH);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, activeX, barY, activeW, barH, barH * 0.5f);
    NVGpaint barPaint = nvgLinearGradient(
        vg,
        activeX, barY,
        activeX + activeW, barY,
        nvgRGBA(83, 252, 24, 50),
        nvgRGBA(83, 252, 24, 255));
    nvgFillPaint(vg, barPaint);
    nvgFill(vg);
    nvgRestore(vg);
}

brls::View* KickLoadingView::create() {
    return new KickLoadingView();
}

} // namespace kick
