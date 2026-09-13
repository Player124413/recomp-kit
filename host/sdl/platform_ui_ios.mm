// platform_ui_ios.mm - the SDL host on iPadOS: one fullscreen Metal window, the
// game seeded from the bundle into Documents/game (the bundle is read-only and
// the guest writes its saves next to its data), and the app lifecycle mapped
// onto audio and presentation.
#include "platform_ui.h"

#include "../../platform/os.h"
#include "../audio.h"
#include "../present.h"
#include "game_config.h"

#import <Foundation/Foundation.h>

#include <filesystem>
#include <fstream>
#include <stdio.h>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string read_stamp(const fs::path &dir) {
    std::ifstream in(dir / ".stamp");
    std::string s;
    std::getline(in, s);
    return s;
}

std::string bundle_dir() {
    char path[4096];
    if (os_exe_path(path, sizeof path) != 0)
        return "";
    return fs::path(path).parent_path().string();
}

std::string documents_dir() {
    NSArray *paths =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return paths.count ? std::string([paths[0] fileSystemRepresentation]) : "";
}

} // namespace

void platform_ui_init_hints() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    // The mapper turns fingers into mouse and key events itself.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
}

GamePath platform_ui_resolve_game(const char *, std::string *error) {
    GamePath g;
    const fs::path bundled = fs::path(bundle_dir()) / "game";
    const fs::path docs = fs::path(documents_dir()) / "game";
    std::error_code ec;
    if (!fs::is_regular_file(bundled / RECOMP_EXECUTABLE, ec)) {
        if (error)
            *error =
                "the app bundle has no game/" RECOMP_EXECUTABLE
                "; build with tools/build.py --target ios from a checkout with the game installed";
        return g;
    }
    const std::string want = read_stamp(bundled);
    if (!fs::is_regular_file(docs / RECOMP_EXECUTABLE, ec) || read_stamp(docs) != want) {
        fs::remove_all(docs, ec);
        fs::create_directories(docs, ec);
        fs::copy(bundled, docs, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        if (ec) {
            fs::remove_all(docs);
            if (error)
                *error = "copying the game into Documents failed: " + ec.message();
            return g;
        }
    }
    g.exe = (docs / RECOMP_EXECUTABLE).string();
    g.source = GamePathSource::Saved;
    return g;
}

namespace {
// SDL3 does not queue the app lifecycle events; it hands them to event
// watchers on the UIKit callstack that raised them (SDL_SendAppEvent). The
// host's event loop therefore never sees them, and this watcher is the only
// place the background transition can be acted on before iOS freezes the
// process.
bool lifecycle_watch(void *, SDL_Event *e) {
    platform_ui_handle_lifecycle(*e);
    return true;
}
} // namespace

SDL_Window *platform_ui_create_window(const char *title, int, int, int,
                                      SDL_WindowFlags surface_flag, int *window_mode) {
    static bool watching = false;
    if (!watching) {
        SDL_AddEventWatch(lifecycle_watch, nullptr);
        watching = true;
    }
    SDL_Window *w = SDL_CreateWindow(
        title, 0, 0, surface_flag | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_mode)
        *window_mode = 2;
    return w;
}

bool platform_ui_handle_lifecycle(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        fprintf(stderr, "[ios] will enter background: suspending presentation and audio\n");
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        fprintf(stderr, "[ios] did enter background\n");
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        fprintf(stderr, "[ios] will enter foreground\n");
        return true;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        fprintf(stderr, "[ios] did enter foreground: resuming audio and presentation\n");
        host_audio_pause(false);
        host_present_suspend(false);
        return true;
    case SDL_EVENT_TERMINATING:
        fprintf(stderr, "[ios] terminating\n");
        host_present_suspend(true);
        host_audio_pause(true);
        return true;
    default:
        return false;
    }
}

bool platform_ui_keypad_wanted() {
    return !SDL_HasKeyboard();
}

bool platform_ui_pointer_capture_supported() {
    return false;
}

int platform_ui_default_overlay() {
    return 0;
}
