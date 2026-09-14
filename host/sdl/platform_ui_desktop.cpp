// platform_ui_desktop.cpp - the SDL host on desktop and Android. Desktop
// uses a resizable window and file picker; Android uses fullscreen touch.
#include "platform_ui.h"

#include "../game_path.h"

#ifdef __ANDROID__
#include "../audio.h"
#include "../present.h"
#include <stdlib.h>
#endif

void platform_ui_init_hints() {
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // The shared touch mapper generates mouse events itself.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif
    // A click that brings the window forward reaches the game in the same
    // event, rather than being swallowed as the activating click.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
}

GamePath platform_ui_resolve_game(const char *flag, std::string *error) {
    if (error)
        error->clear();
    return game_path_resolve(flag); // an empty result means: show the picker
}

SDL_Window *platform_ui_create_window(const char *title, int mode_w, int mode_h, int scale,
                                      SDL_WindowFlags surface_flag, int *window_mode) {
#ifdef __ANDROID__
    // App lifecycle events go to watchers even when SDL cannot pump while
    // backgrounded. Match the mobile host's audio/presenter suspension seam.
    static bool watching = false;
    if (!watching) {
        SDL_AddEventWatch(
            [](void *, SDL_Event *event) {
                platform_ui_handle_lifecycle(*event);
                return true;
            },
            nullptr);
        watching = true;
    }
    if (window_mode)
        *window_mode = 2;
    return SDL_CreateWindow(title, 0, 0,
                            surface_flag | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#else
    SDL_Window *w = SDL_CreateWindow(title, mode_w * scale, mode_h * scale,
                                     surface_flag | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!w)
        return nullptr;
    SDL_SetWindowMinimumSize(w, mode_w, mode_h);
    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (window_mode)
        *window_mode = 0;
    return w;
#endif
}

bool platform_ui_handle_lifecycle(const SDL_Event &event) {
#ifdef __ANDROID__
    switch (event.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_TERMINATING:
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        host_audio_pause(false);
        host_present_suspend(false);
        return true;
    default:
        break;
    }
#else
    (void)event;
#endif
    return false;
}

bool platform_ui_keypad_wanted() {
#ifdef __ANDROID__
    return !SDL_HasKeyboard();
#else
    return false;
#endif
}

bool platform_ui_pointer_capture_supported() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}

int platform_ui_default_overlay() {
#ifdef __ANDROID__
    return 0;
#else
    return 2;
#endif
}

void platform_ui_process_exit(int code) {
#ifdef __ANDROID__
    // main has called SDL_Quit and destroyed its window. End the process:
    // minimizing here would leave a dead game in the activity's task.
    SDL_Log("[android] the game exited (%d); ending the app", code);
    exit(code);
#else
    (void)code;
    // main() returns; the process ends the ordinary way.
#endif
}
