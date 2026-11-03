#pragma once

#include <string>
#include <stdint.h>
#include <borealis/core/event.hpp>

typedef enum MpvEventEnum {
    MPV_LOADED,
    MPV_PAUSE,
    MPV_RESUME,
    MPV_STOP,
    LOADING_START,
    LOADING_END,
    UPDATE_DURATION,
    UPDATE_PROGRESS,
    START_FILE,
    END_OF_FILE,
    VIDEO_SPEED_CHANGE,
    VIDEO_MUTE,
    VIDEO_UNMUTE,
    MPV_FILE_ERROR,
    VIDEO_RECONFIG,
    RESET,
} MpvEventEnum;

typedef brls::Event<MpvEventEnum> MPVEvent;
