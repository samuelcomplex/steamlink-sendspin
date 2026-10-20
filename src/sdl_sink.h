// sendspin-steamlink: SDL2 audio output for the sendspin-cpp player role.
//
// Modeled on sendspin-cpp's examples/common/portaudio_sink (Apache 2.0): a single-producer /
// single-consumer byte ring between the library's sync task (write) and SDL's audio callback
// (read), with playback feedback reported from the callback.
//
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace sl {

class SdlSink {
public:
    SdlSink();
    ~SdlSink();

    /// Called from the SDL audio thread with the frames just handed to the device and the
    /// client-clock time (us) at which the last of them is expected to leave the speaker.
    std::function<void(uint32_t frames, int64_t finish_us)> on_frames_played;

    /// Fixed extra output latency (us) added to every finish estimate: the Marvell/HDMI/receiver
    /// pipeline beyond SDL's own buffer.
    int64_t extra_latency_us{0};

    /// Opens (or keeps) the device for this format. Safe to call on every stream start.
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

    /// Lets go of the audio device and SDL's audio subsystem, e.g. while the Steam Link
    /// interface sets HDMI up again. Writes wait (as for a full device) until resume().
    void suspend();
    /// Re-initializes SDL audio and reopens the device in the last configured format.
    bool resume();
    bool suspended() const { return suspended_.load(); }
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms);
    void clear();
    void close();

    void set_volume(uint8_t volume);
    void set_muted(bool muted);

private:
    static void sdl_callback(void* userdata, uint8_t* stream, int len);
    size_t ring_write_(const uint8_t* data, size_t len);
    size_t ring_read_(uint8_t* dest, size_t len);
    size_t ring_used_() const;
    size_t ring_free_() const { return capacity_ - 1 - ring_used_(); }
    void ring_reset_();
    void update_volume_();

    std::vector<uint8_t> buffer_;
    size_t capacity_;
    std::atomic<size_t> read_pos_{0};
    std::atomic<size_t> write_pos_{0};
    std::atomic<bool> clear_requested_{false};

    std::mutex write_mutex_;
    std::condition_variable write_cv_;
    std::atomic<bool> abort_write_{false};

    std::recursive_mutex config_mutex_;  // configure / suspend / resume from different threads
    std::atomic<bool> suspended_{false};
    uint32_t want_rate_{48000};
    uint8_t want_channels_{2};
    uint8_t want_bits_{16};

    uint32_t device_{0};
    uint32_t sample_rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    size_t bytes_per_frame_{0};
    int64_t device_buffer_us_{0};

    uint8_t volume_{100};
    bool muted_{false};
    std::atomic<uint64_t> volume_q32_{uint64_t{1} << 32};
};

}  // namespace sl
