#include "utils/network_state.hpp"

#include <mutex>
#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace kicknx::network {

#if defined(__SWITCH__)
namespace {

// Closed by its destructor, which runs before libnx shuts nifm down.
struct HeldRequest {
    NifmRequest request{};
    bool created = false;
    std::mutex mutex;
    std::chrono::steady_clock::time_point lastCheck{};

    ~HeldRequest() {
        if (created) nifmRequestClose(&request);
    }
};

HeldRequest& held() {
    static HeldRequest value;
    return value;
}

// A request the system dropped (a lost connection, Airplane mode) no longer
// counts, so it is submitted again.
void renewRequest() {
    HeldRequest& state = held();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.created) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - state.lastCheck < std::chrono::seconds(5)) return;
    state.lastCheck = now;

    NifmRequestState current = NifmRequestState_Invalid;
    if (R_FAILED(nifmGetRequestState(&state.request, &current))) return;
    if (current == NifmRequestState_Available || current == NifmRequestState_OnHold) return;
    const Result result = nifmRequestSubmit(&state.request);
}

}  // namespace

void holdConnection() {
    HeldRequest& state = held();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.created) return;
    const Result created = nifmCreateRequest(&state.request, true);
    if (R_FAILED(created)) return;
    state.created = true;
    const Result submitted = nifmRequestSubmit(&state.request);
}

bool online() {
    renewRequest();
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
}
#else
void holdConnection() {}
bool online() { return true; }
#endif

bool waitUntilOnline(std::chrono::seconds limit, const std::atomic_bool* stop) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!online()) {
        if ((stop && stop->load()) || std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;
}

}  // namespace kicknx::network
