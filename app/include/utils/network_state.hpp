#pragma once

#include <atomic>
#include <chrono>

namespace kicknx::network {

/// Tells the Switch this app needs the internet, for as long as it runs. The
/// app's own use of sockets does not do that. Failure is harmless.
void holdConnection();

/// Whether the Switch currently reports a working internet connection.
bool online();

/// Waits until online, for at most `limit` or until `stop` is set. Returns
/// whether the Switch is online.
bool waitUntilOnline(std::chrono::seconds limit, const std::atomic_bool* stop = nullptr);

}  // namespace kicknx::network
