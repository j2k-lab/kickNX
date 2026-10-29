/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <view/auto_tab_frame.hpp>
#include <client/client.hpp>
#include <api/kick.hpp>
#include <api/kick_playlist.hpp>
#include <utils/ums.hpp>

class RecyclingGrid;

using DirList = std::vector<remote::DirEntry>;

class RemoteView : public AttachedView {
public:
    using Client = std::shared_ptr<remote::Client>;

    RemoteView(Client c);
    ~RemoteView() override;

    brls::View* getDefaultFocus() override;

    void push(const std::string& path);

    void dismiss(std::function<void(void)> cb = [] {}) override;

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
        int64_t uptimeSeconds = 0);

    static void playVod(
        const std::string& videoId,
        const std::string& title = "",
        const std::string& details = "");

    static void playClip(
        const std::string& slug,
        const std::string& title = "",
        const std::string& details = "");

protected:
    void setContent(RecyclingGrid* view);
    RecyclingGrid* newRecycler();

    std::vector<RecyclingGrid*> stack;
    RecyclingGrid* recycler;
    Client client;
};

class UmsView : public RemoteView {
public:
    UmsView();
    ~UmsView() override;

private:
    Ums::DeviceEvent::Subscription deviceSubscribeID;
};
