// sendspin-steamlink: finding a Sendspin server (Music Assistant) on the local network.
//
// A one-shot mDNS query (RFC 6762) for _sendspin-server._tcp.local, sent from an ordinary port so
// responders answer us directly ("legacy unicast"). Music Assistant's Sendspin provider announces
// itself this way with its port and path.
//
// Licensed under the Apache License, Version 2.0.

#include "discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace sl {

namespace {

constexpr const char* kService = "_sendspin-server._tcp.local";
constexpr uint16_t kTypeA = 1, kTypePTR = 12, kTypeTXT = 16, kTypeSRV = 33;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads a (possibly compressed) DNS name at off; advances off past it. Empty on malformed input.
std::string read_name(const uint8_t* p, size_t len, size_t& off) {
    std::string out;
    size_t pos = off;
    bool jumped = false;
    int hops = 0;
    while (pos < len) {
        uint8_t l = p[pos];
        if (l == 0) {
            if (!jumped) off = pos + 1;
            return out;
        }
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > 16) return {};
            size_t target = ((l & 0x3F) << 8) | p[pos + 1];
            if (!jumped) off = pos + 2;
            jumped = true;
            pos = target;
            continue;
        }
        if (pos + 1 + l > len) return {};
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(p + pos + 1), l);
        pos += 1 + l;
    }
    return {};
}

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

std::vector<uint8_t> build_query() {
    std::vector<uint8_t> q(12, 0);
    q[5] = 1;  // one question
    std::string name = kService;
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        q.push_back(static_cast<uint8_t>(dot - start));
        q.insert(q.end(), name.begin() + static_cast<long>(start), name.begin() + static_cast<long>(dot));
        start = dot + 1;
    }
    q.push_back(0);
    q.push_back(0);
    q.push_back(kTypePTR);
    q.push_back(0x80);  // class IN, unicast response requested
    q.push_back(1);
    return q;
}

struct Instance {
    std::string target;
    uint16_t port = 0;
    std::string path = "/sendspin";
    std::string source_ip;
};

}  // namespace

std::optional<std::string> find_sendspin_server(int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return std::nullopt;
    sockaddr_in any{};
    any.sin_family = AF_INET;
    if (bind(fd, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0) {
        close(fd);
        return std::nullopt;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);
    auto query = build_query();

    std::map<std::string, Instance> instances;     // instance name -> details
    std::map<std::string, std::string> addresses;  // host name -> IPv4
    const std::string service = lower(kService);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto next_send = std::chrono::steady_clock::now();
    int sends = 0;
    uint8_t buf[9000];
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        if (sends < 3 && now >= next_send) {
            sendto(fd, query.data(), query.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
            ++sends;
            next_send = now + std::chrono::milliseconds(700);
        }
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::min(deadline, next_send) - now).count();
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(std::max<long long>(wait, 1))) <= 0) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 12) continue;
        char fromip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, fromip, sizeof fromip);

        size_t len = static_cast<size_t>(n), off = 12;
        uint16_t qd = u16(buf + 4);
        uint16_t rr = static_cast<uint16_t>(u16(buf + 6) + u16(buf + 8) + u16(buf + 10));
        bool bad = false;
        for (uint16_t i = 0; i < qd && !bad; ++i) {
            if (read_name(buf, len, off).empty() || off + 4 > len) bad = true;
            off += 4;
        }
        for (uint16_t i = 0; i < rr && !bad; ++i) {
            std::string name = lower(read_name(buf, len, off));
            if (name.empty() || off + 10 > len) break;
            uint16_t type = u16(buf + off);
            uint16_t rdlen = u16(buf + off + 8);
            off += 10;
            if (off + rdlen > len) break;
            size_t rd = off;
            off += rdlen;
            if (type == kTypePTR && name == service) {
                size_t t = rd;
                std::string inst = lower(read_name(buf, len, t));
                if (!inst.empty() && instances[inst].source_ip.empty()) instances[inst].source_ip = fromip;
            } else if (type == kTypeSRV && rdlen >= 7) {
                size_t t = rd + 6;
                Instance& in = instances[name];
                in.port = u16(buf + rd + 4);
                in.target = lower(read_name(buf, len, t));
                if (in.source_ip.empty()) in.source_ip = fromip;
            } else if (type == kTypeTXT) {
                size_t t = rd;
                while (t < rd + rdlen) {
                    uint8_t l = buf[t++];
                    if (t + l > rd + rdlen) break;
                    std::string kv(reinterpret_cast<const char*>(buf + t), l);
                    t += l;
                    if (lower(kv.substr(0, 5)) == "path=") instances[name].path = kv.substr(5);
                }
            } else if (type == kTypeA && rdlen == 4) {
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, buf + rd, ip, sizeof ip);
                addresses[name] = ip;
            }
        }
        // Done as soon as one complete Sendspin server is known.
        for (auto& [key, in] : instances) {
            if (key.size() > service.size() && key.ends_with(service) && in.port != 0) {
                deadline = now;  // finish after this packet
            }
        }
    }
    close(fd);

    for (auto& [key, in] : instances) {
        if (key.size() <= service.size() || !key.ends_with(service) || in.port == 0) continue;
        std::string ip = in.source_ip;
        if (auto it = addresses.find(in.target); it != addresses.end()) ip = it->second;
        if (ip.empty()) continue;
        std::string path = in.path.empty() || in.path[0] != '/' ? "/" + in.path : in.path;
        return "ws://" + ip + ":" + std::to_string(in.port) + path;
    }
    return std::nullopt;
}

std::string mac_suffix() {
    // Prefer the interface that's in use (Wi-Fi or Ethernet), then any with an address.
    for (bool need_up : {true, false}) {
        for (const char* ifc : {"mlan0", "wlan0", "eth0"}) {
            std::string base = std::string("/sys/class/net/") + ifc + "/";
            std::string state, mac;
            std::ifstream(base + "operstate") >> state;
            if (need_up && state != "up") continue;
            std::ifstream f(base + "address");
            if (f >> mac && mac.size() == 17 && mac != "00:00:00:00:00:00") {
                std::string s = mac.substr(12, 2) + mac.substr(15, 2);
                for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                return s;
            }
        }
    }
    return {};
}

}  // namespace sl
