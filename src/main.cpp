// sendspin-steamlink: a headless Sendspin player for the original Valve Steam Link.
//
// Audio goes out over HDMI through the firmware's own SDL2 ("marvell" driver). The Steam Link
// interface must not be running at the same time; the start script takes care of that.
//
// Builds against two sendspin-cpp generations:
//   SENDSPIN_LEGACY_API  sendspin-cpp v0.8.x, unencrypted, for Music Assistant 2.10.x
//   (default)            sendspin-cpp >= 0.9, encrypted Sendspin 1.0, for newer servers
//
// Usage: sendspin-steamlink [options]
//   -n NAME   Player name shown in Music Assistant (default "Steam Link XXXX", from the MAC)
//   -u URL    Dial this Sendspin server, e.g. ws://192.168.1.10:8927/sendspin. Without it, or
//             when it hasn't answered for a minute, the player finds Music Assistant on the
//             network itself (mDNS).
//   -p PORT   Listen port for servers that connect to us (default 8928)
//   -s FILE   State file (identity, pairing records / saved delay)
//   -a        Allow playback without pairing            (encrypted build only)
//   -t FILE   Write the pairing token to FILE            (encrypted build only)
//   -d MS     Fixed output delay in ms (+ = play earlier to compensate a slow receiver)
//   -L MS     Extra output latency estimate for the HDMI/receiver pipeline
//   -W        Open a black fullscreen window (off by default: it crashes while the
//             Steam Link interface owns the display). -V is accepted and ignored.
//   -C STEPS  Control the AV receiver's volume over HDMI-CEC instead of scaling the audio:
//             STEPS volume presses for the whole 0-100 range (0 = off, the default). Uses the
//             slcec helper next to this program.
//   -l LEVEL  none|error|warn|info|debug|verbose
//
// Licensed under the Apache License, Version 2.0.

#include "cec_volume.h"
#include "discovery.h"
#include "sdl2_api.h"
#include "sdl_sink.h"
#include "sendspin/client.h"
#include "sendspin/config.h"
#include "sendspin/metadata_role.h"
#include "sendspin/player_role.h"
#ifndef SENDSPIN_LEGACY_API
#include "file_persistence_provider.h"
#endif

#include <getopt.h>
#include <unistd.h>

#ifndef SL_BUILD_STAMP
#define SL_BUILD_STAMP "dev"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>

using namespace sendspin;

static std::atomic<bool> running{true};
static void on_signal(int) { running.store(false); }


static bool parse_level(const char* s, LogLevel& out) {
    struct { const char* n; LogLevel l; } t[] = {
        {"none", LogLevel::NONE}, {"error", LogLevel::ERROR}, {"warn", LogLevel::WARN},
        {"info", LogLevel::INFO}, {"debug", LogLevel::DEBUG}, {"verbose", LogLevel::VERBOSE}};
    for (auto& e : t) {
        if (strcmp(s, e.n) == 0) { out = e.l; return true; }
    }
    return false;
}

static void usage(const char* p) {
    fprintf(stderr,
            "Usage: %s [-n NAME] [-u URL] [-p PORT] [-s STATEFILE] [-a] [-t TOKENFILE]\n"
            "          [-d DELAY_MS] [-L LATENCY_MS] [-C STEPS] [-W] [-l LEVEL]\n",
            p);
}

#ifdef SENDSPIN_LEGACY_API
// The pre-1.0 protocol needs a caller-chosen client_id. Use the network MAC so it is stable
// across reboots and reinstalls; fall back to a random id kept in a file.
static std::string legacy_client_id(const std::string& state_file) {
    for (const char* ifc : {"eth0", "wlan0", "mlan0"}) {
        std::ifstream f(std::string("/sys/class/net/") + ifc + "/address");
        std::string mac;
        if (f >> mac && mac.size() == 17 && mac != "00:00:00:00:00:00") {
            std::string id = "steamlink-";
            for (char c : mac) if (c != ':') id += c;
            return id;
        }
    }
    std::string path = state_file + ".id";
    std::string id;
    if (std::ifstream in(path); in >> id && !id.empty()) return id;
    std::random_device rd;
    char buf[32];
    snprintf(buf, sizeof buf, "steamlink-%08x%04x", rd(), rd() & 0xffff);
    id = buf;
    std::ofstream(path) << id << "\n";
    return id;
}

// v0.8 persistence: just the server-adjustable static delay, in a small text file.
struct LegacyPersistence : SendspinPersistenceProvider {
    std::string path;
    explicit LegacyPersistence(std::string p) : path(std::move(p)) {}
    bool save_static_delay(uint16_t ms) override {
        std::ofstream f(path);
        f << ms << "\n";
        return static_cast<bool>(f);
    }
    std::optional<uint16_t> load_static_delay() override {
        std::ifstream f(path);
        unsigned v;
        if (f >> v && v < 65536) return static_cast<uint16_t>(v);
        return std::nullopt;
    }
    // Which server last played here, so a reconnecting server is recognized after a restart.
    bool save_last_server_hash(uint32_t hash) override {
        std::ofstream f(path + ".server");
        f << hash << "\n";
        return static_cast<bool>(f);
    }
    std::optional<uint32_t> load_last_server_hash() override {
        std::ifstream f(path + ".server");
        unsigned long v;
        if (f >> v && v <= 0xffffffffUL) return static_cast<uint32_t>(v);
        return std::nullopt;
    }
};
#endif

int main(int argc, char* argv[]) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    std::string name;
    std::string url;
    std::string state_file = "./sendspin-state.json";
    std::string token_file;
    uint16_t port = SendspinClientConfig::DEFAULT_SERVER_PORT;
    bool unpaired = false;
    bool video = false;
    int delay_ms = 0;
    int latency_ms = 0;
    int cec_steps = 0;
    LogLevel level = LogLevel::INFO;

    int opt;
    while ((opt = getopt(argc, argv, "n:u:p:as:t:d:L:C:VWl:h")) != -1) {
        switch (opt) {
            case 'n': name = optarg; break;
            case 'u': url = optarg; break;
            case 'p': port = static_cast<uint16_t>(atoi(optarg)); break;
            case 'a': unpaired = true; break;
            case 's': state_file = optarg; break;
            case 't': token_file = optarg; break;
            case 'd': delay_ms = atoi(optarg); break;
            case 'L': latency_ms = atoi(optarg); break;
            case 'C': cec_steps = atoi(optarg); break;
            case 'V': video = false; break;
            case 'W': video = true; break;
            case 'l':
                if (!parse_level(optarg, level)) { usage(argv[0]); return 2; }
                break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    SendspinClient::set_log_level(level);
    // Accept a bare address ("192.168.1.20", "192.168.1.20:8927") as well as a full URL.
    if (!url.empty() && url.find("://") == std::string::npos) {
        std::string host = url;
        if (host.find(':') == std::string::npos) host += ":8927";
        if (host.find('/') == std::string::npos) host += "/sendspin";
        url = "ws://" + host;
    }
    if (name.empty()) {
        std::string suffix = sl::mac_suffix();
        name = suffix.empty() ? "Steam Link" : "Steam Link " + suffix;
    }
#ifdef SENDSPIN_LEGACY_API
    (void)unpaired;
    fprintf(stderr, "sendspin-steamlink: legacy protocol build (Music Assistant 2.10.x), built %s\n",
            SL_BUILD_STAMP);
#else
    fprintf(stderr, "sendspin-steamlink: Sendspin 1.0 encrypted build, built %s\n",
            SL_BUILD_STAMP);
#endif

    // ---- SDL
    if (!sl::sdl.load()) return 1;
    void* window = nullptr;
    if (video && sl::sdl.has_video()) {
        if (sl::sdl.Init(sl::SDL_INIT_VIDEO) == 0) {
            window = sl::sdl.CreateWindow("Sendspin", sl::SDL_WINDOWPOS_UNDEFINED,
                                          sl::SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                                          sl::SDL_WINDOW_FULLSCREEN_DESKTOP);
            if (window) {
                if (sl::sdl.ShowCursor) sl::sdl.ShowCursor(0);
                if (void* r = sl::sdl.CreateRenderer(window, -1, 0)) {
                    sl::sdl.SetRenderDrawColor(r, 0, 0, 0, 255);
                    sl::sdl.RenderClear(r);
                    sl::sdl.RenderPresent(r);
                }
            } else {
                fprintf(stderr, "video: no window (%s), continuing audio-only\n", sl::sdl.GetError());
            }
        } else {
            fprintf(stderr, "video: init failed (%s), continuing audio-only\n", sl::sdl.GetError());
        }
    }
    if (sl::sdl.InitSubSystem(sl::SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: SDL audio init failed: %s\n", sl::sdl.GetError());
        return 1;
    }
    if (sl::sdl.GetCurrentAudioDriver) {
        const char* drv = sl::sdl.GetCurrentAudioDriver();
        fprintf(stderr, "audio: SDL driver %s\n", drv ? drv : "(none)");
    }

    sl::SdlSink sink;
    sink.extra_latency_us = static_cast<int64_t>(latency_ms) * 1000;

    // The volume Music Assistant last set, so the player reports it on connecting (the library
    // would report 0) and the receiver's position is known after a restart.
    const std::string volume_file = state_file + ".volume";
    // First start: half-way for a receiver (so the slider can go both ways), full level otherwise.
    const int default_volume = cec_steps > 0 ? 50 : 100;
    int saved_volume = default_volume;
    if (std::ifstream vf(volume_file); !(vf >> saved_volume) || saved_volume < 0 ||
                                       saved_volume > 100) {
        saved_volume = default_volume;
    }

    // Receiver volume over HDMI: the audio stays at full level and the receiver's own volume
    // follows Music Assistant. Mute stays in the player, where it can't get out of step.
    std::unique_ptr<sl::CecVolume> cec;
    if (cec_steps > 0) {
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string dir = ".";
        if (n > 0) {
            exe[n] = 0;
            dir = exe;
            dir = dir.substr(0, dir.rfind('/'));
        }
        std::string helper = dir + "/slcec";
        if (access(helper.c_str(), X_OK) == 0) {
            cec = std::make_unique<sl::CecVolume>(helper, cec_steps > 200 ? 200 : cec_steps,
                                                  saved_volume);
            fprintf(stderr, "sendspin: receiver volume over HDMI, %d steps for 0-100%%\n", cec_steps);
        } else {
            fprintf(stderr, "sendspin: %s not found; using the player's own volume\n", helper.c_str());
        }
    }

    // ---- Sendspin client
    SendspinClientConfig config;
    config.name = name;
    config.product_name = "Steam Link";
    config.manufacturer = "Valve";
    config.software_version = "sendspin-steamlink 1.0";
    config.server_port = port;
#ifdef SENDSPIN_LEGACY_API
    config.client_id = legacy_client_id(state_file);
    LegacyPersistence persistence(state_file + ".delay");
#else
    FilePersistenceProvider persistence(state_file);
#endif

    SendspinClient client(std::move(config));
    client.set_persistence_provider(&persistence);
#ifndef SENDSPIN_LEGACY_API
    client.set_unpaired_access_enabled(unpaired);
#endif

    PlayerRoleConfig pc;
    // Plain PCM first so the server does the decoding/resampling, not the Steam Link's CPU.
    pc.audio_formats = {
        {SendspinCodecFormat::PCM, 2, 48000, 16},
        {SendspinCodecFormat::PCM, 2, 44100, 16},
        {SendspinCodecFormat::FLAC, 2, 48000, 16},
        {SendspinCodecFormat::FLAC, 2, 44100, 16},
    };
    pc.fixed_delay_us = static_cast<int32_t>(delay_ms) * 1000;
    auto& player = client.add_player(std::move(pc));
    player.update_volume(static_cast<uint8_t>(saved_volume));
    if (!cec) sink.set_volume(static_cast<uint8_t>(saved_volume));
#ifdef SENDSPIN_LEGACY_API
    player.set_static_delay_adjustable(true);
#else
    player.set_output_delay_adjustable(true);
#endif
    auto& metadata = client.add_metadata();

    struct PlayerL : PlayerRoleListener {
        sl::SdlSink& sink;
        PlayerRole& player;
        sl::CecVolume* cec;
        std::string volume_file;
        PlayerL(sl::SdlSink& s, PlayerRole& p, sl::CecVolume* c, std::string vf)
            : sink(s), player(p), cec(c), volume_file(std::move(vf)) {}
        size_t on_audio_write(uint8_t* data, size_t len, uint32_t timeout_ms) override {
            return sink.write(data, len, timeout_ms);
        }
        void on_stream_start() override {
            auto& p = player.get_current_stream_params();
            if (p.sample_rate && p.channels && p.bit_depth) {
                fprintf(stderr, ">>> stream start %u Hz %u ch %u-bit\n", *p.sample_rate,
                        *p.channels, *p.bit_depth);
                sink.configure(*p.sample_rate, *p.channels, *p.bit_depth);
            }
        }
        void on_stream_end() override {
            fprintf(stderr, ">>> stream end\n");
            sink.clear();
        }
        void on_volume_changed(uint8_t v) override {
            if (cec) cec->set_volume(v);
            else sink.set_volume(v);
            std::ofstream(volume_file) << static_cast<int>(v) << "\n";
        }
        void on_mute_changed(bool m) override { sink.set_muted(m); }
    };
    struct MetaL : MetadataRoleListener {
        std::string last;
        void on_metadata(const ServerMetadataStateObject& md) override {
            std::string now = md.artist.value_or("") + " - " + md.title.value_or("");
            if (md.title && now != last) {
                fprintf(stderr, ">>> now playing: %s\n", now.c_str());
                last = now;
            }
        }
    };
    struct ClientL : SendspinClientListener {
#ifndef SENDSPIN_LEGACY_API
        void on_trust_changed(ConnectionTrust t) override {
            fprintf(stderr, ">>> connected (%s)\n",
                    t == ConnectionTrust::USER ? "paired" : "unpaired");
        }
        void on_pairing_started(const std::string&) override { fprintf(stderr, ">>> pairing...\n"); }
        void on_pairing_succeeded(const std::string&) override {
            fprintf(stderr, ">>> pairing succeeded\n");
        }
        void on_pairing_failed(const std::string&, SendspinPairAbortReason r) override {
            fprintf(stderr, ">>> pairing failed (reason %d)\n", static_cast<int>(r));
        }
#endif
    };
    struct NetL : SendspinNetworkProvider {
        bool is_network_ready() override { return true; }
    };

    PlayerL player_l(sink, player, cec.get(), volume_file);
    MetaL meta_l;
    ClientL client_l;
    NetL net;
    sink.on_frames_played = [&player](uint32_t frames, int64_t ts) {
        player.notify_audio_played(frames, ts);
    };
    player.set_listener(&player_l);
    metadata.set_listener(&meta_l);
    client.set_listener(&client_l);
    client.set_network_provider(&net);

    if (!client.start()) {
        fprintf(stderr, "sendspin: client failed to start\n");
        return 1;
    }
#ifdef SENDSPIN_LEGACY_API
    fprintf(stderr, "sendspin: '%s' listening on port %u\n", name.c_str(), port);
#else
    fprintf(stderr, "sendspin: '%s' listening on port %u (unpaired access %s)\n", name.c_str(),
            port, unpaired ? "on" : "off");
    if (auto tok = client.pairing_token()) {
        fprintf(stderr, "sendspin: pairing token %s\n", tok->c_str());
        if (!token_file.empty()) std::ofstream(token_file) << *tok << "\n";
    }
#endif

    // Open the device right away with the preferred format, so HDMI audio is live (and the
    // receiver locked) before the first song starts.
    sink.configure(48000, 2, 16);

    // Finding Music Assistant. A configured address is dialed first and again every 15 s while
    // disconnected. Without one, or once the configured address hasn't worked for a minute (Music
    // Assistant moved, e.g. a new DHCP lease), the network is searched over mDNS every 30 s and
    // whatever is found there is used instead.
    const std::string configured = url;
    auto try_discover = [&] {
        if (auto found = sl::find_sendspin_server()) {
            if (*found != url) {
                fprintf(stderr, "sendspin: found Music Assistant at %s\n", found->c_str());
                url = *found;
            }
            client.connect_to(url);
            return true;
        }
        return false;
    };
    if (configured.empty()) {
        if (!try_discover()) fprintf(stderr, "sendspin: no Music Assistant found yet; still looking\n");
    } else {
        fprintf(stderr, "sendspin: dialing %s\n", url.c_str());
        client.connect_to(url);
    }

    using clock = std::chrono::steady_clock;
    auto last_dial = clock::now();
    auto disconnected_since = clock::now();
    bool was_connected = false;
    int tick = 0;
    auto last_report = clock::now();
    int reports = 0;
    while (running.load()) {
        client.loop();
        if (window && (++tick % 10) == 0) sl::sdl.PumpEvents();
        bool connected = client.is_connected();
        if (connected != was_connected) {
            fprintf(stderr, ">>> %s\n", connected ? "server connected" : "server disconnected");
            was_connected = connected;
            if (!connected) disconnected_since = clock::now();
        }
        if (!connected) {
            auto now = clock::now();
            bool search = configured.empty() || now - disconnected_since > std::chrono::seconds(60);
            if (search && now - last_dial > std::chrono::seconds(30)) {
                if (!try_discover() && !url.empty()) client.connect_to(url);
                last_dial = clock::now();
            } else if (!search && now - last_dial > std::chrono::seconds(15)) {
                client.connect_to(url);
                last_dial = now;
            }
        }
        // Audio timing in the log: every minute for the first ten, then every ten minutes.
        auto since = clock::now() - last_report;
        if (since > std::chrono::minutes(reports < 10 ? 1 : 10)) {
            fprintf(stderr, "%s\n", sink.take_timing_report().c_str());
            last_report = clock::now();
            ++reports;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Say goodbye as "restart", not "shutdown": the server then keeps this player in its group
    // for a few minutes and puts it straight back into a running stream when it reconnects
    // (after a reboot, or the supervisor restarting it). "shutdown" makes the server forget
    // the group, so playback would have to be started again by hand.
    fprintf(stderr, "sendspin: shutting down\n");
    if (client.is_connected()) {
        client.disconnect(SendspinGoodbyeReason::RESTART);
        for (int i = 0; i < 30; ++i) {
            client.loop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    client.stop();
    sink.close();
    sl::sdl.Quit();
    return 0;
}
