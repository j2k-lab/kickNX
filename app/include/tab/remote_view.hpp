/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <client/client.hpp>
#include <api/kick.hpp>
#include <api/kick_playlist.hpp>

#include <cstdint>
#include <string>
#include <vector>

using DirList = std::vector<remote::DirEntry>;

/// Entry points that open the full-screen Kick player (live, VOD and clip).
class RemoteView {
public:
    static void play(
        const std::string& path,
        const std::string& name = "",
        const std::string& extra = "",
        const std::string& kickChannel = "",
        const std::string& loadingNote = "",
        const std::vector<kick::SubtitleTrack>& subtitles = {},
        int64_t uptimeSeconds = 0);

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
