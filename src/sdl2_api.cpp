// sendspin-steamlink: runtime SDL2 loader.
// Licensed under the Apache License, Version 2.0.

#include "sdl2_api.h"

#include <cstdio>
#include <dlfcn.h>

namespace sl {

Sdl2 sdl;

template <typename F>
static bool bind(void* lib, F& fn, const char* name, bool required) {
    fn = reinterpret_cast<F>(dlsym(lib, name));
    if (!fn && required) {
        fprintf(stderr, "SDL2: missing required symbol %s\n", name);
        return false;
    }
    return true;
}

bool Sdl2::load(const char* soname) {
    void* lib = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "SDL2: cannot load %s: %s\n", soname, dlerror());
        return false;
    }
    bool ok = true;
    ok &= bind(lib, Init, "SDL_Init", true);
    ok &= bind(lib, InitSubSystem, "SDL_InitSubSystem", true);
    ok &= bind(lib, QuitSubSystem, "SDL_QuitSubSystem", true);
    ok &= bind(lib, Quit, "SDL_Quit", true);
    ok &= bind(lib, GetError, "SDL_GetError", true);
    ok &= bind(lib, GetCurrentAudioDriver, "SDL_GetCurrentAudioDriver", false);
    ok &= bind(lib, OpenAudioDevice, "SDL_OpenAudioDevice", true);
    ok &= bind(lib, PauseAudioDevice, "SDL_PauseAudioDevice", true);
    ok &= bind(lib, CloseAudioDevice, "SDL_CloseAudioDevice", true);
    bind(lib, CreateWindow, "SDL_CreateWindow", false);
    bind(lib, CreateRenderer, "SDL_CreateRenderer", false);
    bind(lib, SetRenderDrawColor, "SDL_SetRenderDrawColor", false);
    bind(lib, RenderClear, "SDL_RenderClear", false);
    bind(lib, RenderPresent, "SDL_RenderPresent", false);
    bind(lib, ShowCursor, "SDL_ShowCursor", false);
    bind(lib, PumpEvents, "SDL_PumpEvents", false);
    return ok;
}

}  // namespace sl
