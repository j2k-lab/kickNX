#pragma once

#include <borealis.hpp>
#include "utils/qrcodegen.hpp"
#include <string>
#include <vector>
#include <algorithm>

namespace kick {

class QRCodeView : public brls::View {
public:
    QRCodeView(const std::string& text = "", float size = 260.0f)
        : text(text), qrSize(size) {
        setDimensions(size, size);
        generate();
    }

    void setText(const std::string& newText) {
        text = newText;
        generate();
        invalidate();
    }

    void setQrSize(float size) {
        qrSize = size;
        setDimensions(size, size);
        invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override {
        // Draw white rounded background card with subtle border
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 12.0f);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);

        if (matrixSize <= 0 || modules.empty()) return;

        // Draw black QR code modules inside quiet zone
        const float padding = 16.0f;
        const float drawArea = std::min(width, height) - (padding * 2.0f);
        if (drawArea <= 0.0f) return;

        const float cellSize = drawArea / static_cast<float>(matrixSize);
        const float startX = x + (width - drawArea) * 0.5f;
        const float startY = y + (height - drawArea) * 0.5f;

        nvgBeginPath(vg);
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        for (int row = 0; row < matrixSize; row++) {
            for (int col = 0; col < matrixSize; col++) {
                if (modules[row * matrixSize + col]) {
                    nvgRect(vg, startX + col * cellSize, startY + row * cellSize, cellSize, cellSize);
                }
            }
        }
        nvgFill(vg);
    }

private:
    void generate() {
        if (text.empty()) {
            modules.clear();
            matrixSize = 0;
            return;
        }
        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            matrixSize = qr.getSize();
            modules.resize(matrixSize * matrixSize);
            for (int row = 0; row < matrixSize; row++) {
                for (int col = 0; col < matrixSize; col++) {
                    modules[row * matrixSize + col] = qr.getModule(col, row);
                }
            }
        } catch (...) {
            modules.clear();
            matrixSize = 0;
        }
    }

    std::string text;
    float qrSize;
    int matrixSize = 0;
    std::vector<bool> modules;
};

} // namespace kick
