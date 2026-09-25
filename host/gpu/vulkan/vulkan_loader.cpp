// vulkan_loader.cpp - find and load the Vulkan loader through volk. Homebrew's
// loader on macOS is outside the dynamic linker's default search, so a few
// known paths and RECOMP_VULKAN_LIBRARY are tried before giving up.
#include "vulkan_device.h"

#include <mutex>
#include <stdlib.h>
#include <string>
#ifndef _WIN32
#include <dlfcn.h>
#include <dirent.h>
#include <vector>
#include "../../../platform/os.h"
#endif

#ifdef __ANDROID__
extern "C" const char *SDL_GetAndroidInternalStoragePath(void);
#endif

namespace gpu {

static std::once_flag g_once;
static bool g_loaded = false;
static std::string g_path;

#ifndef _WIN32
static bool try_path(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"));
    if (!gipa)
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vk_icdGetInstanceProcAddr"));
    if (!gipa) {
        dlclose(h);
        return false;
    }
    volkInitializeCustom(gipa);
    g_path = path;
    return true;
}
#endif

bool vulkan_load() {
    std::call_once(g_once, [] {
#ifndef _WIN32
        const char *env = recomp_env("VULKAN_LIBRARY");
        if (env && *env && try_path(env)) {
            g_loaded = true;
            return;
        }
#endif
#ifdef __ANDROID__
        // 1. Check custom driver from profile/vulkan_driver.txt
        const char *prof = recomp_env("PROFILE_DIR");
        if (prof && *prof) {
            std::string pfile = std::string(prof) + "/vulkan_driver.txt";
            FILE *f = fopen(pfile.c_str(), "r");
            if (f) {
                char buf[512] = {};
                if (fgets(buf, sizeof(buf) - 1, f)) {
                    size_t len = strlen(buf);
                    while (len > 0 &&
                           (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' '))
                        buf[--len] = '\0';
                    if (len > 0 && try_path(buf)) {
                        fprintf(stderr, "gpu/vulkan: loaded custom driver from profile: %s\n", buf);
                        g_loaded = true;
                        fclose(f);
                        return;
                    }
                }
                fclose(f);
            }
        }

        // 2. Check internal app storage (<internal>/driver/)
        const char *internal_dir = SDL_GetAndroidInternalStoragePath();
        if (internal_dir && *internal_dir) {
            std::string ddir = std::string(internal_dir) + "/driver";
            std::string active_file = ddir + "/active_driver.txt";
            FILE *f = fopen(active_file.c_str(), "r");
            std::string active_so;
            if (f) {
                char buf[256] = {};
                if (fgets(buf, sizeof(buf) - 1, f)) {
                    size_t len = strlen(buf);
                    while (len > 0 &&
                           (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' '))
                        buf[--len] = '\0';
                    if (len > 0)
                        active_so = buf;
                }
                fclose(f);
            }

            // Preload auxiliary libraries in driver directory
            DIR *d = opendir(ddir.c_str());
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != nullptr) {
                    std::string ename = ent->d_name;
                    if (ename.size() > 3 && ename.substr(ename.size() - 3) == ".so" &&
                        ename != "libvulkan_freedreno.so" && ename != "vulkan.adreno.so") {
                        std::string aux_path = ddir + "/" + ename;
                        dlopen(aux_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
                    }
                }
                closedir(d);
            }

            std::vector<std::string> candidates;
            if (!active_so.empty()) {
                if (active_so[0] == '/')
                    candidates.push_back(active_so);
                else
                    candidates.push_back(ddir + "/" + active_so);
            }
            candidates.push_back(ddir + "/libvulkan_freedreno.so");
            candidates.push_back(ddir + "/vulkan.adreno.so");
            candidates.push_back(ddir + "/turnip.so");
            candidates.push_back(ddir + "/libvulkan.so");

            for (const auto &c : candidates) {
                if (try_path(c.c_str())) {
                    fprintf(stderr, "gpu/vulkan: loaded custom Turnip driver: %s\n", c.c_str());
                    g_loaded = true;
                    return;
                }
            }
        }

        if (try_path("libvulkan.so")) {
            g_loaded = true;
            return;
        }
#endif
        if (volkInitialize() == VK_SUCCESS) {
            g_loaded = true;
            return;
        }
#ifdef __APPLE__
        const char *candidates[] = {"/opt/homebrew/lib/libvulkan.1.dylib",
                                    "/usr/local/lib/libvulkan.1.dylib",
                                    "/opt/homebrew/lib/libMoltenVK.dylib"};
        for (const char *c : candidates)
            if (try_path(c)) {
                g_loaded = true;
                return;
            }
#endif
    });
    return g_loaded;
}

bool vulkan_available() {
    return vulkan_load();
}

const char *vulkan_loader_path_impl() {
    vulkan_load();
    return g_path.empty() ? nullptr : g_path.c_str();
}

} // namespace gpu
