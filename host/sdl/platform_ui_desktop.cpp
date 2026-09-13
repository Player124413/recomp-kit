// platform_ui_desktop.cpp - the SDL host on macOS, Linux and Windows: a sized,
// resizable window, the game found on disk or picked by the user, no app
// lifecycle events to speak of.
#include "platform_ui.h"

#include "../game_path.h"

void platform_ui_init_hints() {
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
}

bool platform_ui_handle_lifecycle(const SDL_Event &) {
    return false;
}

bool platform_ui_touch_overlay_wanted() {
    return false;
}
