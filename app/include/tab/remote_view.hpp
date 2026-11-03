/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <api/kick.hpp>
#include <api/kick_playlist.hpp>

#include <cstdint>
#include <string>
#include <vector>

/// Entry points that open the full-screen Kick player (live, VOD and clip).
class RemoteView {
public:
    static void playLive(
        const kick::Config& config,
        int64_t uptimeSeconds = 0,
        const std::string& streamTitle = "");

    static void playVod(
        const std::string& videoId,
        const std::string& title = "",
        const std::string& details = "");

    static void playClip(
        const std::string& slug,
        const std::string& title = "",
        const std::string& details = "");
};
