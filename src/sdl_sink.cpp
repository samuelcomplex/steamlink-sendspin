// sendspin-steamlink: SDL2 audio output for the sendspin-cpp player role.
// Licensed under the Apache License, Version 2.0.

#include "sdl_sink.h"

#include "sdl2_api.h"
#include "sendspin/client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace sl {

// ~170 ms at 48 kHz / 16-bit / stereo: room for scheduling jitter on a single 1 GHz core.
static constexpr size_t RING_CAPACITY = 32768;
// Frames per SDL callback. 1024 frames is ~21 ms at 48 kHz.
static constexpr uint16_t SDL_BUFFER_FRAMES = 1024;

SdlSink::SdlSink() : buffer_(RING_CAPACITY), capacity_(RING_CAPACITY) {}

SdlSink::~SdlSink() {
    close();
}

// ---------------------------------------------------------------- ring (SPSC)

size_t SdlSink::ring_used_() const {
    size_t w = write_pos_.load(std::memory_order_acquire);
    size_t r = read_pos_.load(std::memory_order_acquire);
    return (w - r + capacity_) % capacity_;
}

size_t SdlSink::ring_write_(const uint8_t* data, size_t len) {
    size_t w = write_pos_.load(std::memory_order_relaxed);
    size_t n = std::min(len, ring_free_());
    if (n == 0) return 0;
    size_t first = std::min(n, capacity_ - w);
    std::memcpy(&buffer_[w], data, first);
    if (n > first) std::memcpy(&buffer_[0], data + first, n - first);
    write_pos_.store((w + n) % capacity_, std::memory_order_release);
    return n;
}

size_t SdlSink::ring_read_(uint8_t* dest, size_t len) {
    if (clear_requested_.load(std::memory_order_acquire)) {
        clear_requested_.store(false, std::memory_order_relaxed);
        read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
        std::memset(dest, 0, len);
        return 0;
    }
    size_t r = read_pos_.load(std::memory_order_relaxed);
    size_t n = std::min(len, ring_used_());
    if (n > 0) {
        size_t first = std::min(n, capacity_ - r);
        std::memcpy(dest, &buffer_[r], first);
        if (n > first) std::memcpy(dest + first, &buffer_[0], n - first);
        read_pos_.store((r + n) % capacity_, std::memory_order_release);
    }
    if (n < len) std::memset(dest + n, 0, len - n);
    return n;
}

void SdlSink::ring_reset_() {
    clear_requested_.store(false, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
    write_pos_.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------- device

bool SdlSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    if (device_ != 0 && sample_rate == sample_rate_ && channels == channels_ &&
        bits_per_sample == bits_) {
        clear();
        abort_write_.store(false, std::memory_order_release);
        return true;
    }
    close();

    uint16_t format;
    if (bits_per_sample == 16) {
        format = AUDIO_S16LSB;
    } else if (bits_per_sample == 32) {
        format = AUDIO_S32LSB;
    } else {
        fprintf(stderr, "audio: unsupported bit depth %u\n", bits_per_sample);
        return false;
    }

    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = static_cast<int>(sample_rate);
    want.format = format;
    want.channels = channels;
    want.samples = SDL_BUFFER_FRAMES;
    want.callback = &SdlSink::sdl_callback;
    want.userdata = this;

    // allowed_changes = 0: SDL converts to whatever the hardware runs at, so the ring always
    // carries exactly the stream format.
    ring_reset_();
    sample_rate_ = sample_rate;
    channels_ = channels;
    bits_ = bits_per_sample;
    bytes_per_frame_ = static_cast<size_t>(channels) * (bits_per_sample / 8);

    device_ = sdl.OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device_ == 0) {
        fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", sdl.GetError());
        return false;
    }
    // One device buffer is queued ahead of the one currently playing.
    uint16_t frames = have.samples ? have.samples : SDL_BUFFER_FRAMES;
    device_buffer_us_ = static_cast<int64_t>(frames) * 1000000 / sample_rate;

    fprintf(stderr, "audio: opened %u Hz, %u ch, %u-bit (device buffer %u frames, %lld us)\n",
            sample_rate, channels, bits_per_sample, frames,
            static_cast<long long>(device_buffer_us_));
    abort_write_.store(false, std::memory_order_release);
    sdl.PauseAudioDevice(device_, 0);
    return true;
}

void SdlSink::close() {
    abort_write_.store(true, std::memory_order_release);
    write_cv_.notify_all();
    if (device_ != 0) {
        sdl.CloseAudioDevice(device_);  // joins the SDL audio thread
        device_ = 0;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    ring_reset_();
    sample_rate_ = 0;
}

void SdlSink::clear() {
    clear_requested_.store(true, std::memory_order_release);
}

size_t SdlSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    size_t total = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (total < length) {
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (abort_write_.load(std::memory_order_acquire) || device_ == 0) break;
            size_t remaining = length - total;
            size_t n = std::min(remaining, ring_free_());
            // Keep the ring frame-aligned (see portaudio_sink.cpp for the reasoning).
            if (n < remaining && bytes_per_frame_ > 0) n -= n % bytes_per_frame_;
            if (n > 0) total += ring_write_(data + total, n);
        }
        if (total < length) {
            std::unique_lock<std::mutex> lock(write_mutex_);
            size_t min_space = bytes_per_frame_ > 0 ? bytes_per_frame_ : 1;
            if (!write_cv_.wait_until(lock, deadline, [this, min_space] {
                    return ring_free_() >= min_space ||
                           abort_write_.load(std::memory_order_acquire);
                })) {
                break;
            }
        }
    }
    return total;
}

static int64_t client_now_us() {
#ifndef SENDSPIN_HAS_NOW_US
    // Library releases without SendspinClient::now_us() read steady_clock directly.
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#else
    return sendspin::SendspinClient::now_us();
#endif
}

void SdlSink::sdl_callback(void* userdata, uint8_t* stream, int len) {
    int64_t now_us = client_now_us();
    auto* self = static_cast<SdlSink*>(userdata);
    size_t want = static_cast<size_t>(len);
    size_t got = self->ring_read_(stream, want);

    uint64_t vol = self->volume_q32_.load(std::memory_order_relaxed);
    if (vol < (uint64_t{1} << 32) && self->bits_ == 16) {
        auto* s = reinterpret_cast<int16_t*>(stream);
        size_t count = want / 2;
        int64_t scale = static_cast<int64_t>(vol);
        for (size_t i = 0; i < count; ++i) {
            s[i] = static_cast<int16_t>((static_cast<int64_t>(s[i]) * scale + (int64_t{1} << 31)) >> 32);
        }
    } else if (vol < (uint64_t{1} << 32) && self->bits_ == 32) {
        auto* s = reinterpret_cast<int32_t*>(stream);
        size_t count = want / 4;
        int64_t scale = static_cast<int64_t>(vol);
        for (size_t i = 0; i < count; ++i) {
            s[i] = static_cast<int32_t>((static_cast<int64_t>(s[i]) * scale + (int64_t{1} << 31)) >> 32);
        }
    }

    self->write_cv_.notify_one();

    if (got > 0 && self->bytes_per_frame_ > 0 && self->on_frames_played) {
        uint32_t frames = static_cast<uint32_t>(got / self->bytes_per_frame_);
        // The buffer SDL asked for starts playing once the one already queued has drained, and
        // the frames we wrote sit at its start.
        int64_t finish = now_us + self->device_buffer_us_ +
                         static_cast<int64_t>(frames) * 1000000 / self->sample_rate_ +
                         self->extra_latency_us;
        self->on_frames_played(frames, finish);
    }
}

// ---------------------------------------------------------------- volume

void SdlSink::set_volume(uint8_t volume) {
    volume_ = volume > 100 ? 100 : volume;
    update_volume_();
}

void SdlSink::set_muted(bool muted) {
    muted_ = muted;
    update_volume_();
}

void SdlSink::update_volume_() {
    constexpr uint64_t ONE = uint64_t{1} << 32;
    if (muted_ || volume_ == 0) {
        volume_q32_.store(0, std::memory_order_relaxed);
    } else if (volume_ >= 100) {
        volume_q32_.store(ONE, std::memory_order_relaxed);
    } else {
        uint64_t v = volume_;
        volume_q32_.store(v * v * ONE / 10000, std::memory_order_relaxed);  // perceptual curve
    }
}

}  // namespace sl
