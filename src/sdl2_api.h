// sendspin-steamlink: minimal SDL2 binding loaded at runtime with dlopen.
//
// The Steam Link firmware ships libSDL2-2.0.so.0 with a "marvell" audio driver. Loading it at
// runtime keeps this binary free of SDL headers and link-time version coupling: only the stable
// SDL2 C ABI below is used.
//
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>

namespace sl {

constexpr uint32_t SDL_INIT_AUDIO = 0x00000010u;
constexpr uint32_t SDL_INIT_VIDEO = 0x00000020u;
constexpr uint16_t AUDIO_S16LSB = 0x8010;
constexpr uint16_t AUDIO_S32LSB = 0x8020;
constexpr uint32_t SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u;
constexpr int SDL_WINDOWPOS_UNDEFINED = 0x1FFF0000;

using SDL_AudioCallback = void (*)(void* userdata, uint8_t* stream, int len);

struct SDL_AudioSpec {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    SDL_AudioCallback callback;
    void* userdata;
};

struct Sdl2 {
    int (*Init)(uint32_t) = nullptr;
    int (*InitSubSystem)(uint32_t) = nullptr;
    void (*QuitSubSystem)(uint32_t) = nullptr;
    void (*Quit)() = nullptr;
    const char* (*GetError)() = nullptr;
    const char* (*GetCurrentAudioDriver)() = nullptr;
    uint32_t (*OpenAudioDevice)(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int) = nullptr;
    void (*PauseAudioDevice)(uint32_t, int) = nullptr;
    void (*CloseAudioDevice)(uint32_t) = nullptr;
    // Video (optional): a black fullscreen window brings up the HDMI output the same way the
    // Steam Link's own interface does at boot.
    void* (*CreateWindow)(const char*, int, int, int, int, uint32_t) = nullptr;
    void* (*CreateRenderer)(void*, int, uint32_t) = nullptr;
    int (*SetRenderDrawColor)(void*, uint8_t, uint8_t, uint8_t, uint8_t) = nullptr;
    int (*RenderClear)(void*) = nullptr;
    void (*RenderPresent)(void*) = nullptr;
    int (*ShowCursor)(int) = nullptr;
    void (*PumpEvents)() = nullptr;

    bool load(const char* soname = "libSDL2-2.0.so.0");
    bool has_video() const {
        return CreateWindow && CreateRenderer && SetRenderDrawColor && RenderClear &&
               RenderPresent && PumpEvents;
    }
};

extern Sdl2 sdl;

}  // namespace sl
