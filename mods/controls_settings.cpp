// controls_settings.cpp - see controls_settings.h.
#include "controls_settings.h"

#include "mods_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
// The layout names come from the host (mods_controls_set_names), which must
// run before mods_controls_init because mods/ cannot depend on host/controls.
// Until the host calls it, this is what a fresh profile sees.
std::vector<std::string> g_names = {"pad", "keys", "pad+keys"};

// EDIT_ROW is not one of these: it is never declared or persisted.
constexpr int kStoredRowCount = CONTROLS_EDIT_ROW;
const char *const kKeys[kStoredRowCount] = {
    "layout", "size", "opacity", "haptics", "pad_with_controller", "snap"};
const char *const kLabels[kStoredRowCount] = {
    "Controls",       "Controls size",       "Controls opacity",
    "Button haptics", "Pad with controller", "Editor snapping"};
const int kMin[kStoredRowCount] = {0, 0, 20, 0, 0, 0};
// The layout row's true maximum is dynamic (g_names.size()); this entry is
// unused for it and only fills out the array.
const int kMax[kStoredRowCount] = {0, 2, 100, 1, 1, 1};

// Written by the settings page and the on-screen tab, read by the host loop
// that lays the controls out: each value is atomic on its own.
std::atomic<int> values[CONTROLS_ROW_COUNT] = {0, 1, 70, 1, 0, 1, 0};
std::atomic<uint32_t> hidden_groups{0};
std::atomic<bool> edit_request{false};
std::atomic<bool> initialized{false};

int layout_max() {
    return int(g_names.size()); // the extra index past the names is Hidden
}

int index_of(const std::string &name) {
    for (size_t i = 0; i < g_names.size(); ++i)
        if (g_names[i] == name)
            return int(i);
    return -1;
}
} // namespace

void mods_controls_set_names(std::vector<std::string> names) {
    g_names = std::move(names);
}

void mods_controls_refresh_names() {
    if (!initialized)
        return;
    mods_controls_set(CONTROLS_LAYOUT_ROW,
                      std::clamp(values[CONTROLS_LAYOUT_ROW].load(), 0, layout_max()));
}

void mods_controls_init(const char *default_layout) {
    if (initialized.exchange(true))
        return;

    const std::string want = default_layout ? default_layout : "";
    const int found = index_of(want);
    values[CONTROLS_LAYOUT_ROW] = want == "hidden" ? layout_max() : found >= 0 ? found : 0;
    values[CONTROLS_SIZE_ROW] = 1;
    values[CONTROLS_OPACITY_ROW] = 70;
    values[CONTROLS_HAPTICS_ROW] = 1;
    values[CONTROLS_PAD_WITH_CONTROLLER_ROW] = 0;
    values[CONTROLS_SNAP_ROW] = 1;
    hidden_groups = 0;

    // Migrate a saved keypad profile the first time host.controls/layout has
    // never been written. The keypad's own keys are left on disk untouched:
    // this only reads them, never declares or rewrites them.
    int64_t discard = 0;
    if (!mods_settings_stored_value("host.controls/layout", &discard)) {
        int64_t left = 1, right = 1, size = -1;
        const bool has_left = mods_settings_stored_value("host.keypad/left", &left);
        const bool has_right = mods_settings_stored_value("host.keypad/right", &right);
        if (has_left || has_right) {
            const int keys_index = index_of("keys");
            values[CONTROLS_LAYOUT_ROW] = keys_index >= 0 ? keys_index : 0;
            uint32_t bits = 0;
            if (has_left && left == 0)
                bits |= 1;
            if (has_right && right == 0)
                bits |= 2;
            hidden_groups = bits;
            if (mods_settings_stored_value("host.keypad/size", &size))
                values[CONTROLS_SIZE_ROW] = int(size);
        }
    }

    for (int i = 0; i < kStoredRowCount; ++i) {
        const int max = i == CONTROLS_LAYOUT_ROW ? layout_max() : kMax[i];
        mods_settings_declare(MODS_OWNER_RUNTIME, "host.controls", kKeys[i], kLabels[i],
                              POP_SETTING_INT, values[i], kMin[i], max);
        int64_t v = values[i].load();
        mods_settings_get(MODS_OWNER_RUNTIME, kKeys[i], &v);
        values[i] = int(std::clamp<int64_t>(v, kMin[i], max));
    }
    mods_settings_declare(MODS_OWNER_RUNTIME, "host.controls", "hidden", "hidden", POP_SETTING_INT,
                          hidden_groups.load(), 0, 0xffff);
    int64_t hv = hidden_groups.load();
    mods_settings_get(MODS_OWNER_RUNTIME, "hidden", &hv);
    hidden_groups = uint32_t(std::clamp<int64_t>(hv, 0, 0xffff));
}

void mods_controls_reset() {
    initialized = false;
    g_names = {"pad", "keys", "pad+keys"};
    values[CONTROLS_LAYOUT_ROW] = 0;
    values[CONTROLS_SIZE_ROW] = 1;
    values[CONTROLS_OPACITY_ROW] = 70;
    values[CONTROLS_HAPTICS_ROW] = 1;
    values[CONTROLS_PAD_WITH_CONTROLLER_ROW] = 0;
    values[CONTROLS_SNAP_ROW] = 1;
    values[CONTROLS_EDIT_ROW] = 0;
    hidden_groups = 0;
    edit_request = false;
}

int mods_controls_value(ControlsRow row) {
    return row >= 0 && row < CONTROLS_ROW_COUNT ? values[row].load() : 0;
}

std::string mods_controls_layout_name() {
    const int index = mods_controls_value(CONTROLS_LAYOUT_ROW);
    return index >= 0 && index < int(g_names.size()) ? g_names[index] : "";
}

PopModStatus mods_controls_set(ControlsRow row, int value) {
    if (row < 0 || row >= CONTROLS_ROW_COUNT)
        return POP_E_INVAL;
    if (row == CONTROLS_EDIT_ROW) {
        edit_request = true;
        return POP_OK;
    }
    const int max = row == CONTROLS_LAYOUT_ROW ? layout_max() : kMax[row];
    const int v = std::clamp(value, kMin[row], max);
    values[row] = v;
    if (!initialized)
        return POP_OK; // nothing declared yet: the value is applied when it is
    return mods_settings_set(MODS_OWNER_RUNTIME, kKeys[row], v);
}

// The layout choice and the on/off rows turn over in either direction, like
// the display rows; size and opacity stop at their ends. EDIT_ROW never
// carries a value: any nudge just asks the host to open the editor.
PopModStatus mods_controls_nudge(ControlsRow row, int delta) {
    if (row < 0 || row >= CONTROLS_ROW_COUNT)
        return POP_E_INVAL;
    if (row == CONTROLS_EDIT_ROW) {
        edit_request = true;
        return POP_OK;
    }
    const int current = mods_controls_value(row);
    int value;
    if (row == CONTROLS_LAYOUT_ROW) {
        const int count = layout_max() + 1;
        value = ((current + delta) % count + count) % count;
    } else if (row == CONTROLS_SIZE_ROW) {
        value = std::clamp(current + delta, 0, 2);
    } else if (row == CONTROLS_OPACITY_ROW) {
        value = std::clamp(current + delta * 10, 20, 100);
    } else { // haptics, pad_with_controller, snap: plain toggles
        value = current ? 0 : 1;
    }
    return mods_controls_set(row, value);
}

std::string mods_controls_line(ControlsRow row) {
    char buf[128];
    switch (row) {
    case CONTROLS_LAYOUT_ROW: {
        const std::string name = mods_controls_layout_name();
        snprintf(buf, sizeof buf, "%-22s %s", "Controls", name.empty() ? "hidden" : name.c_str());
        break;
    }
    case CONTROLS_SIZE_ROW: {
        static const char *const sizes[3] = {"Small", "Medium", "Large"};
        snprintf(buf, sizeof buf, "%-22s %s", "Controls size",
                 sizes[std::clamp(mods_controls_value(row), 0, 2)]);
        break;
    }
    case CONTROLS_OPACITY_ROW:
        snprintf(buf, sizeof buf, "%-22s %d%%", "Controls opacity", mods_controls_value(row));
        break;
    case CONTROLS_HAPTICS_ROW:
        snprintf(buf, sizeof buf, "%-22s %s", "Button haptics",
                 mods_controls_value(row) ? "on" : "off");
        break;
    case CONTROLS_PAD_WITH_CONTROLLER_ROW:
        snprintf(buf, sizeof buf, "%-22s %s", "Pad with controller",
                 mods_controls_value(row) ? "on" : "off");
        break;
    case CONTROLS_SNAP_ROW:
        snprintf(buf, sizeof buf, "%-22s %s", "Editor snapping",
                 mods_controls_value(row) ? "on" : "off");
        break;
    case CONTROLS_EDIT_ROW:
        snprintf(buf, sizeof buf, "%-22s %s", "Edit controls", ">");
        break;
    default:
        buf[0] = '\0';
    }
    return buf;
}

uint32_t mods_controls_hidden_groups() {
    return hidden_groups.load();
}

void mods_controls_set_hidden_groups(uint32_t bits) {
    const uint32_t v = bits & 0xffffu;
    hidden_groups = v;
    if (!initialized)
        return;
    mods_settings_set(MODS_OWNER_RUNTIME, "hidden", int64_t(v));
}

bool mods_controls_take_edit_request() {
    return edit_request.exchange(false);
}
