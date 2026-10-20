// sendspin-steamlink: finding a Sendspin server (Music Assistant) on the local network.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <optional>
#include <string>

namespace sl {

/// Asks the local network over mDNS for a Sendspin server (_sendspin-server._tcp, which Music
/// Assistant announces) and returns its WebSocket URL, e.g. "ws://192.168.1.20:8927/sendspin".
std::optional<std::string> find_sendspin_server(int timeout_ms = 3000);

/// The last four hex digits of this device's network MAC address ("CDEF"), or "" if unknown.
std::string mac_suffix();

}  // namespace sl
