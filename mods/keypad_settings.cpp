// keypad_settings.cpp - see keypad_settings.h.
#include "keypad_settings.h"

#include "mods_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
const char *const kKeys[KEYPAD_ROW_COUNT] = {"left", "right", "size"};
const char *const kLabels[KEYPAD_ROW_COUNT] = {"Keypad left", "Keypad right", "Keypad size"};
const int kMax[KEYPAD_ROW_COUNT] = {1, 1, 2};
// Written by the settings page and the on-screen tab, read by the host loop
// that lays the keypad out: each value is atomic on its own.
std::atomic<int> values[KEYPAD_ROW_COUNT] = {1, 1, 1};
std::atomic<bool> initialized{false};
} // namespace

void mods_keypad_init(int default_hidden) {
    if (initialized.exchange(true))
        return;
    values[KEYPAD_LEFT_ROW] = default_hidden ? 0 : 1;
    values[KEYPAD_RIGHT_ROW] = default_hidden ? 0 : 1;
    values[KEYPAD_SIZE_ROW] = 1;
    for (int i = 0; i < KEYPAD_ROW_COUNT; ++i) {
        mods_settings_declare(MODS_OWNER_RUNTIME, "host.keypad", kKeys[i], kLabels[i],
                              POP_SETTING_INT, values[i], 0, kMax[i]);
        int64_t v = values[i].load();
        mods_settings_get(MODS_OWNER_RUNTIME, kKeys[i], &v);
        values[i] = int(std::clamp<int64_t>(v, 0, kMax[i]));
    }
}

void mods_keypad_reset() {
    initialized = false;
    for (auto &v : values)
        v = 1;
}

int mods_keypad_value(KeypadRow row) {
    return row >= 0 && row < KEYPAD_ROW_COUNT ? values[row].load() : 0;
}

PopModStatus mods_keypad_set(KeypadRow row, int value) {
    if (row < 0 || row >= KEYPAD_ROW_COUNT)
        return POP_E_INVAL;
    const int v = std::clamp(value, 0, kMax[row]);
    values[row] = v;
    if (!initialized)
        return POP_OK; // nothing declared yet: the value is applied when it is
    return mods_settings_set(MODS_OWNER_RUNTIME, kKeys[row], v);
}

// Shown/hidden turns over in either direction, like the display rows; size
// stops at its ends, like UI scale.
PopModStatus mods_keypad_nudge(KeypadRow row, int delta) {
    if (row < 0 || row >= KEYPAD_ROW_COUNT)
        return POP_E_INVAL;
    const int count = kMax[row] + 1, value = mods_keypad_value(row) + delta;
    return mods_keypad_set(row, row == KEYPAD_SIZE_ROW ? value : (value % count + count) % count);
}

std::string mods_keypad_line(KeypadRow row) {
    static const char *const sizes[3] = {"Small", "Medium", "Large"};
    char buf[128];
    const int v = mods_keypad_value(row);
    snprintf(buf, sizeof buf, "%-22s %s", row < KEYPAD_ROW_COUNT ? kLabels[row] : "",
             row == KEYPAD_SIZE_ROW ? sizes[std::clamp(v, 0, 2)]
             : v                    ? "shown"
                                    : "hidden");
    return buf;
}
