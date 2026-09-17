// controls_host.cpp - see controls_host.h.
#include "controls_host.h"

#include "../../mods/controls_settings.h"
#include "../../mods/mods_internal.h"
#include "../../platform/os.h"
#include "../present.h"
#include "binding.h"
#include "game_config.h"
#include "layout_fallback.h"
#include "layout_store.h"
#include "overlay.h"
#include "router.h"
#include "vpad.h"

#include <stdio.h>
#include <string>

namespace controls {

namespace {

// The settings' key sizes: small, medium (the layout's own) and large.
constexpr double kSizePt[3] = {32, 36, 40};

HostHooks g_hooks;
LayoutStore g_store;
Layout g_layout;
Router g_router;
Screen g_screen;
bool g_keyboard_absent = false;

// The mapped binding: RECOMP_CONTROLS_PAD == 1 drives it from the shared
// virtual pad every pump; the other pad modes (off, native) leave it unused.
Binding g_binding;

// Reads a whole file as text; false (leaving *out alone) when it cannot be
// opened, same contract as layout_store.cpp's own copy.
bool read_text_file(const std::string &path, std::string *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out->append(buf, n);
    fclose(f);
    return true;
}

// RECOMP_CONTROLS_MAPPED, then <profile>/controls/binding.txt if present. A
// missing override file is fine; a malformed one (either source) logs once
// and is ignored as a whole (parse_mapped leaves *table unchanged on error).
MappedTable load_mapped_table() {
    MappedTable table;
    std::string error;
    if (!parse_mapped(RECOMP_CONTROLS_MAPPED, &table, &error))
        fprintf(stderr, "[controls] bad RECOMP_CONTROLS_MAPPED: %s\n", error.c_str());
    std::string text;
    const std::string path = std::string(mods_overlay_profile_dir()) + "/controls/binding.txt";
    if (read_text_file(path, &text) && !parse_mapped(text, &table, &error))
        fprintf(stderr, "[controls] bad %s: %s\n", path.c_str(), error.c_str());
    return table;
}

// What g_layout was loaded and sized for.
bool g_loaded = false;      // a load has been attempted
bool g_have_layout = false; // g_layout holds a layout the router drives
std::string g_loaded_name;
Form g_loaded_form = Form::Tablet;
double g_file_scale = 1.0;
int g_size = -1;
bool g_enabled = false;

bool g_published = false;
bool g_published_wanted = false;
uint64_t g_published_revision = 0;

// The groups the player has hidden, as the settings row stores them: bit i
// is groups[i], for the first kHiddenBits groups.
constexpr size_t kHiddenBits = 16;

uint32_t hidden_bits(const Layout &l) {
    uint32_t bits = 0;
    for (size_t i = 0; i < l.groups.size() && i < kHiddenBits; ++i)
        if (!l.groups[i].visible)
            bits |= 1u << i;
    return bits;
}

void apply_hidden_bits(Layout &l, uint32_t bits) {
    for (size_t i = 0; i < l.groups.size() && i < kHiddenBits; ++i)
        l.groups[i].visible = (bits & (1u << i)) == 0;
}

class HostSink : public ControlsSink {
  public:
    void key(int scancode, bool down) override {
        if (g_hooks.key)
            g_hooks.key(scancode, down);
    }
    void action(const std::string &name) override {
        if (name == "settings" && g_hooks.open_settings)
            g_hooks.open_settings();
        else if (name == "system_keyboard" && g_hooks.system_keyboard)
            g_hooks.system_keyboard();
        // "edit_layout" arrives with the editor (Task 20).
    }
    // "next" steps to the following layout name, wrapping and never landing
    // on the Hidden choice; any other target selects that name.
    void switch_layout(const std::string &target) override {
        const std::vector<std::string> names = g_store.names();
        if (names.empty())
            return;
        int index = -1;
        if (target == "next") {
            const int current = mods_controls_value(CONTROLS_LAYOUT_ROW);
            index =
                current >= 0 && current < int(names.size()) ? (current + 1) % int(names.size()) : 0;
        } else {
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == target)
                    index = int(i);
        }
        if (index >= 0)
            (void)mods_controls_set(CONTROLS_LAYOUT_ROW, index);
    }
    void group_visibility_changed() override {
        mods_controls_set_hidden_groups(hidden_bits(g_layout));
    }
    void tap() override {} // haptics arrive in Task 13
};

HostSink g_sink;

// Loads `name` for `form` into g_layout, releasing whatever the router held
// against the old one first. "" (the Hidden choice) or a failed load leaves
// no layout.
void reload(const std::string &name, Form form) {
    g_router.set_layout(nullptr, g_sink); // releases against the old layout
    g_loaded = true;
    g_loaded_name = name;
    g_loaded_form = form;
    g_have_layout = false;
    g_layout = Layout();
    g_size = -1;
    if (name.empty())
        return;
    Layout fresh;
    std::string problem;
    bool fell_back = false;
    if (!load_with_tablet_fallback(g_store, name, form, &fresh, &problem, &fell_back)) {
        fprintf(stderr, "[controls] no %s layout \"%s\"%s%s\n", form_name(form), name.c_str(),
                problem.empty() ? "" : ": ", problem.c_str());
        return;
    }
    if (!problem.empty())
        fprintf(stderr, "[controls] skipped %s\n", problem.c_str());
    static bool fallback_logged = false;
    if (fell_back && !fallback_logged) {
        fallback_logged = true;
        fprintf(stderr, "[controls] no %s layout \"%s\"; using the tablet one\n", form_name(form),
                name.c_str());
    }
    g_layout = std::move(fresh);
    g_file_scale = g_layout.scale;
    g_have_layout = true;
    g_router.set_layout(&g_layout, g_sink);
}

void publish() {
    const bool wanted = g_have_layout && g_enabled;
    ControlsView view;
    if (wanted)
        view = make_view(g_layout, g_router, g_screen,
                         g_layout.opacity * mods_controls_value(CONTROLS_OPACITY_ROW) / 100.0);
    view.wanted = wanted;
    if (g_published && g_published_wanted == wanted &&
        (!wanted || g_published_revision == view.revision))
        return;
    g_published = true;
    g_published_wanted = wanted;
    g_published_revision = view.revision;
    host_present_set_controls(view);
}

} // namespace

void host_init(const HostHooks &hooks) {
    g_hooks = hooks;
    // The game's bundled layouts directory has no accessor yet, so only the
    // player's copies and the kit's built-ins are searched.
    g_store.set_dirs(std::string(mods_overlay_profile_dir()) + "/controls", "");
    mods_controls_set_names(g_store.names());
    g_binding.set_table(load_mapped_table());
}

void host_set_screen(const Screen &s, const Rect &game, int safe_bottom) {
    g_screen = s;
    g_screen.controls_area = controls_area_below(s.dw, s.dh, game, safe_bottom);
    g_router.set_screen(g_screen);
    g_router.set_claim_area(g_screen.controls_area);
    // The binding works in window points; s.scale is drawable pixels per point.
    g_binding.set_bounds(s.scale > 0 ? s.dw / s.scale : 0, s.scale > 0 ? s.dh / s.scale : 0);
}

void host_pointer_moved(double x, double y) {
    g_binding.set_cursor(x, y);
}

void host_set_wanted(bool keyboard_absent, bool controller_present) {
    (void)controller_present; // Task 12
    static const bool force = recomp_env("KEYPAD") != nullptr;
    g_keyboard_absent = keyboard_absent || force;
}

bool host_finger_down(int64_t id, double px, double py, uint64_t now) {
    return g_router.finger_down(id, px, py, now, g_sink);
}

bool host_finger_motion(int64_t id, double px, double py, uint64_t now) {
    return g_router.finger_motion(id, px, py, now, g_sink);
}

bool host_finger_up(int64_t id, uint64_t now) {
    return g_router.finger_up(id, now, g_sink);
}

bool host_finger_cancel(int64_t id) {
    return g_router.finger_cancel(id, g_sink);
}

void host_release_all() {
    g_router.cancel_all(g_sink);
    std::vector<TouchAction> actions;
    g_binding.release_all(&actions);
    if (!actions.empty() && g_hooks.touch_actions)
        g_hooks.touch_actions(actions);
    publish();
}

void host_pump(uint64_t now) {
    // The layout row means nothing until the settings are loaded (the first
    // presented frame runs mods_page_init); draw nothing before then.
    if (!mods_controls_initialized()) {
        publish();
        return;
    }
    const std::string name = mods_controls_layout_name();
    const Form form = g_screen.dw > 0 && g_screen.dh > 0
                          ? form_for(g_screen.dw, g_screen.dh, g_screen.scale)
                          : g_loaded_form;
    if (!g_loaded || name != g_loaded_name || form != g_loaded_form)
        reload(name, form);

    if (g_have_layout) {
        int size = mods_controls_value(CONTROLS_SIZE_ROW);
        size = size < 0 ? 0 : size > 2 ? 2 : size;
        if (size != g_size) {
            g_size = size;
            g_layout.scale = g_file_scale * kSizePt[size] / 36.0;
        }
        // The hidden groups follow the row whoever set it: a toggle writes it
        // (group_visibility_changed), and a settings load or reset may too.
        const uint32_t bits = mods_controls_hidden_groups();
        if (bits != hidden_bits(g_layout))
            apply_hidden_bits(g_layout, bits);
    }

    const bool enabled = g_keyboard_absent && !name.empty() && g_have_layout;
    if (enabled != g_enabled || g_router.enabled() != enabled) {
        g_enabled = enabled;
        g_router.set_enabled(enabled, g_sink);
    }
    vpad().set_source(kPadSourceTouch, g_router.pad());

#if RECOMP_CONTROLS_PAD == 1
    // The mapped binding turns the merged pad into keys/mouse; other pad
    // modes (off, native) leave the virtual pad for host_pad_* to read
    // directly.
    {
        std::vector<TouchAction> actions;
        std::vector<std::string> names;
        g_binding.tick(vpad().state(), now, &actions, &names);
        if (!actions.empty() && g_hooks.touch_actions)
            g_hooks.touch_actions(actions);
        for (const std::string &name : names)
            g_sink.action(name);
    }
#endif

    publish();
}

} // namespace controls
