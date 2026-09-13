// platform_ui.h - what the SDL host does differently per platform: hints
// before SDL_Init, where the game comes from, the one window, and the app
// lifecycle. platform_ui_desktop.cpp is macOS, Linux and Windows;
// platform_ui_ios.mm is iPadOS. Everything else in the host is shared.
#pragma once
#include "../game_path.h"

#include <SDL3/SDL.h>

#include <string>

// Hints that must be set before SDL_Init. Desktop: click-through focus, no
// screensaver. iOS: landscape orientations, no synthesized touch->mouse events.
void platform_ui_init_hints();

// The game executable to load. Desktop: --exe, POP_RECOMP_EXE, checkout, saved
// path (may be empty: the caller shows the picker). iOS: Documents/game/<exe>,
// seeded from the bundle; empty with *error set when the bundle has no game or
// the copy failed.
GamePath platform_ui_resolve_game(const char *flag, std::string *error);

// The one window. Desktop: mode_w*scale x mode_h*scale, resizable, centered,
// hidden until shown. iOS: fullscreen. *window_mode receives 0 (desktop
// window) or 2 (fullscreen).
SDL_Window *platform_ui_create_window(const char *title, int mode_w, int mode_h, int scale,
                                      SDL_WindowFlags surface_flag, int *window_mode);

// True when `e` was an app lifecycle event this platform consumed
// (background: pause audio and presentation; foreground: resume). Desktop:
// always false.
bool platform_ui_handle_lifecycle(const SDL_Event &e);
