#pragma once

#include <borealis.hpp>
#include "api/http.hpp"
#include "config.hpp"

/// Whether the bytes are a WebP file (RIFF....WEBP).
bool isWebpData(const std::string& data);

/// Decodes an animated WebP into canvas-sized RGBA frames and per-frame
/// delays in milliseconds. Fails without decoding anything when the canvas is
/// larger than maxDimension, and stops (failing) once the frames would pass
/// maxFrames or maxBytes. Always fails in builds without WebP support.
bool decodeWebpAnimation(
    const std::string& data,
    int maxDimension,
    int maxFrames,
    size_t maxBytes,
    std::vector<uint8_t>& frames,
    std::vector<int>& delaysMs,
    int& width,
    int& height);

class Image {
    using Ref = std::shared_ptr<Image>;

public:
    Image();
    Image(const Image&) = delete;

    virtual ~Image();

    /// @brief 设置要加载内容的图片组件。此函数需要工作在主线程。
    static void with(brls::Image* view, const std::string& url);

    /// @brief 取消请求，并清空图片。此函数需要工作在主线程。
    static void cancel(brls::Image* view);

private:
    void doRequest(HTTP& s);

    static void clear(
        brls::Image* view,
        const HTTP::Cancel& expectedRequest = nullptr);

private:
    std::string url;
    brls::Image* image;
    HTTP::Cancel isCancel;

    /// 对象池
    inline static std::list<Ref> pool;
    inline static std::mutex requestMutex;
    inline static std::unordered_map<brls::Image*, Ref> requests;
};