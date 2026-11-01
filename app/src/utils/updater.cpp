#include "utils/updater.hpp"

#include "api/http.hpp"

#include <borealis.hpp>

#include <cstdio>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <streambuf>

#ifdef __SWITCH__
#include <switch.h>
// libnx's exit mode; borealis sets it through exitToHomeMode(), but its
// platform object is already gone when applyPending() runs.
extern "C" u32 __nx_applet_exit_mode;
#endif

namespace updater {

namespace {

std::string nroPath;
bool pending = false;

std::string newPath() { return nroPath + ".new"; }
std::string oldPath() { return nroPath + ".old"; }

/// Writes the download straight to the SD card, so a 35 MB NRO never sits in
/// memory, and reports progress as bytes arrive.
class FileProgressBuf : public std::streambuf {
public:
    FileProgressBuf(FILE* file, long long total, const std::function<void(int)>& progress)
        : file(file), total(total), progress(progress) {}

    bool failed = false;

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (std::fwrite(data, 1, count, file) != static_cast<size_t>(count)) {
            failed = true;
            return 0;
        }
        written += count;
        if (total > 0) {
            const int percent = static_cast<int>(written * 100 / total);
            if (percent != lastPercent) {
                lastPercent = percent;
                progress(percent);
            }
        }
        return count;
    }

    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::not_eof(ch);
        const char c = traits_type::to_char_type(ch);
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }

private:
    FILE* file;
    long long total;
    const std::function<void(int)>& progress;
    long long written = 0;
    int lastPercent = -1;
};

/// An NRO has "NRO0" at offset 0x10, and its total size at offset 0x18.
bool looksLikeNro(const std::string& path, long long expectedSize) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    unsigned char header[0x20] = {};
    const bool read = std::fread(header, 1, sizeof(header), file) == sizeof(header);
    std::fseek(file, 0, SEEK_END);
    const long long size = std::ftell(file);
    std::fclose(file);
    return read && std::memcmp(header + 0x10, "NRO0", 4) == 0 && size == expectedSize;
}

}  // namespace

bool supported() {
#ifdef __SWITCH__
    return !nroPath.empty();
#else
    return false;
#endif
}

void setNroPath(const char* path) {
    // The loader passes "sdmc:/..." paths; anything else is not a file we
    // can replace.
    if (path && std::strncmp(path, "sdmc:/", 6) == 0) nroPath = path;
}

void download(const std::string& url, long long expectedSize,
    const std::function<void(int)>& progress) {
    if (!supported()) throw std::runtime_error("self-update is not supported here");
    pending = false;

    const std::string target = newPath();
    FILE* file = std::fopen(target.c_str(), "wb");
    if (!file) throw std::runtime_error("can't write " + target);

    FileProgressBuf buffer(file, expectedSize, progress);
    std::ostream out(&buffer);
    try {
        // No overall time limit: a slow connection can take minutes. Only the
        // connection itself has to open quickly.
        HTTP http;
        http._get(url, &out);
    } catch (...) {
        std::fclose(file);
        std::remove(target.c_str());
        throw;
    }
    std::fclose(file);

    if (buffer.failed || !looksLikeNro(target, expectedSize)) {
        std::remove(target.c_str());
        throw std::runtime_error("the downloaded file is incomplete");
    }
    pending = true;
}

void applyPending() {
    if (!pending) return;
    pending = false;
#ifdef __SWITCH__
    // romfs keeps the running NRO open; close it before moving the file.
    romfsExit();

    std::remove(oldPath().c_str());
    if (std::rename(nroPath.c_str(), oldPath().c_str()) != 0) {
        brls::Logger::error("update: can't move the old NRO aside");
        std::remove(newPath().c_str());
        return;
    }
    if (std::rename(newPath().c_str(), nroPath.c_str()) != 0) {
        brls::Logger::error("update: can't move the new NRO into place");
        std::rename(oldPath().c_str(), nroPath.c_str());
        return;
    }
    std::remove(oldPath().c_str());

    // Ask hbloader to start the new NRO once this one exits. Some forwarders
    // can't do this; the viewer then opens kickNX again themselves.
    if (envHasNextLoad()) {
        const std::string argv = "\"" + nroPath + "\"";
        envSetNextLoad(nroPath.c_str(), argv.c_str());
        // Exiting straight to the HOME Menu would skip the loader.
        __nx_applet_exit_mode = 0;
    }
#endif
}

}  // namespace updater
