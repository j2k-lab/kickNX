#pragma once

#include <functional>
#include <string>

/// Self-update for the Switch build: download the new NRO next to the running
/// one, then swap the files and relaunch after the main loop has exited.
namespace updater {

/// True when this build can replace its own NRO (Switch only).
bool supported();

/// Remembers the running NRO's path (argv[0] from the homebrew loader).
void setNroPath(const char* path);

/// Downloads url to "<nro>.new" and checks it is a complete NRO of the
/// expected size. progress gets the percentage. Throws on failure.
void download(const std::string& url, long long expectedSize,
    const std::function<void(int)>& progress);

/// Call after the main loop: moves "<nro>.new" over the running NRO and asks
/// the loader to start it again. Does nothing when no update is waiting.
void applyPending();

}  // namespace updater
