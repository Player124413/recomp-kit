// settings_tests.cpp - the per-profile settings store, including the
// transaction a failed mod init rolls back.
#include "mods_tests.h"
#include "../mods_internal.h"
#include "../keypad_settings.h"
#include "../display_settings.h"
#include "../../runtime/layout.h"
#include "../../runtime/win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "../../platform/os.h"

extern "C" bool mods_test_reset_loader(void);

namespace {
const char *PROFILE = nullptr; // this suite's own, from mod_test_dir
void fresh() {
    PROFILE = mod_test_dir("settings");
    os_setenv("RECOMP_PROFILE_DIR", PROFILE);
    mods_settings_reset();
    mods_overlay_set_profile_dir(PROFILE);
}
} // namespace

MOD_TEST_SUITE(settings_declare_and_round_trip) {
    fresh();
    mods_settings_declare(2, "a.mod", "ui_scale", "UI scale", POP_SETTING_INT, 2, 1, 4);
    mods_settings_declare(2, "a.mod", "shadows", "Shadows", POP_SETTING_BOOL, 1, 0, 1);

    int64_t v = 0;
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 2); // the declared default
    MOD_CHECK_EQ(mods_settings_set(2, "ui_scale", 4), POP_OK);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 4);
    MOD_CHECK_EQ(mods_settings_set(2, "ui_scale", 9), POP_E_RANGE);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 4); // a refused write changes nothing
    MOD_CHECK_EQ(mods_settings_get(2, "nope", &v), POP_E_NOTFOUND);
    // Another mod's key is not this mod's, even under the same name.
    MOD_CHECK_EQ(mods_settings_get(3, "ui_scale", &v), POP_E_NOTFOUND);

    // A boolean default written as `true` is 1, not 0.
    mods_settings_declare(3, "b.mod", "enabled", "Enabled", POP_SETTING_BOOL, 1, 0, 1);
    MOD_CHECK_EQ(mods_settings_get(3, "enabled", &v), POP_OK);
    MOD_CHECK_EQ(v, 1);

    // One JSON file per profile, reloaded next run, keyed by mod id and key.
    MOD_CHECK(std::string(mods_settings_path()).find(PROFILE) == 0);
    // No shutdown/save call: a successful settings change is durable already.
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "ui_scale", "UI scale", POP_SETTING_INT, 2, 1, 4);
    MOD_CHECK_EQ(mods_settings_get(2, "ui_scale", &v), POP_OK);
    MOD_CHECK_EQ(v, 4); // the persisted value beats the default
}

MOD_TEST_SUITE(settings_failed_save_keeps_previous_value) {
    fresh();
    mods_settings_declare(2, "a.mod", "volume", "Volume", POP_SETTING_INT, 5, 0, 10);
    MOD_CHECK_EQ(mods_settings_set(2, "volume", 7), POP_OK);
    const std::string path = mods_settings_path();
    // A file in place of the parent directory is an actual filesystem error,
    // independent of permissions (the test can also run as root).
    const std::string backup = std::string(PROFILE) + "-saved";
    MOD_CHECK_EQ(rename(PROFILE, backup.c_str()), 0);
    FILE *blocker = fopen(PROFILE, "wb");
    MOD_CHECK(blocker != nullptr);
    if (blocker)
        fclose(blocker);
    MOD_CHECK_EQ(mods_settings_set(2, "volume", 9), POP_E_STATE);
    int64_t value = 0;
    mods_settings_get(2, "volume", &value);
    MOD_CHECK_EQ(value, 7);
    MOD_CHECK_EQ(remove(PROFILE), 0);
    MOD_CHECK_EQ(rename(backup.c_str(), PROFILE), 0);
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(path.c_str()));
    mods_settings_declare(2, "a.mod", "volume", "Volume", POP_SETTING_INT, 5, 0, 10);
    mods_settings_get(2, "volume", &value);
    MOD_CHECK_EQ(value, 7);
}

MOD_TEST_SUITE(settings_transaction_rolls_back) {
    fresh();
    mods_settings_declare(2, "a.mod", "keep", "Keep", POP_SETTING_INT, 1, 0, 9);
    MOD_CHECK_EQ(mods_settings_set(2, "keep", 5), POP_OK);
    MOD_CHECK(mods_settings_save());

    // A mod that declares settings, changes an existing one and then fails.
    mods_settings_txn_begin();
    mods_settings_declare(3, "b.mod", "temp", "Temp", POP_SETTING_INT, 0, 0, 9);
    MOD_CHECK_EQ(mods_settings_set(3, "temp", 7), POP_OK);
    MOD_CHECK_EQ(mods_settings_set(2, "keep", 9), POP_OK);
    mods_settings_txn_rollback();
    mods_settings_remove_all(3);

    int64_t v = 0;
    // Its own declaration is gone, and the value it changed is back.
    MOD_CHECK_EQ(mods_settings_get(3, "temp", &v), POP_E_NOTFOUND);
    MOD_CHECK_EQ(mods_settings_get(2, "keep", &v), POP_OK);
    MOD_CHECK_EQ(v, 5);
    // The persisted file is not left holding the rolled-back value either.
    MOD_CHECK(mods_settings_save());
    mods_settings_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_settings_declare(2, "a.mod", "keep", "Keep", POP_SETTING_INT, 1, 0, 9);
    MOD_CHECK_EQ(mods_settings_get(2, "keep", &v), POP_OK);
    MOD_CHECK_EQ(v, 5);
    MOD_CHECK(!mods_settings_entry_count() || mods_settings_entry_count() == 1);
}

MOD_TEST_SUITE(settings_entries_are_ordered_and_labelled) {
    fresh();
    mods_settings_declare(3, "b.mod", "second", "Second", POP_SETTING_INT, 1, 0, 9);
    mods_settings_declare(2, "a.mod", "first", "First", POP_SETTING_BOOL, 0, 0, 1);
    MOD_CHECK_EQ(mods_settings_entry_count(), 2u);

    uint32_t owner = 0;
    const char *mod_id = nullptr, *key = nullptr, *label = nullptr;
    int32_t kind = 0;
    int64_t value = 0, min = 0, max = 0;
    // Ordered by owner (load order), not by declaration order.
    MOD_CHECK(mods_settings_entry(0, &owner, &mod_id, &key, &label, &kind, &value, &min, &max));
    MOD_CHECK_EQ(owner, 2u);
    MOD_CHECK_STR(mod_id, "a.mod");
    MOD_CHECK_STR(key, "first");
    MOD_CHECK_STR(label, "First");
    MOD_CHECK_EQ(kind, POP_SETTING_BOOL);
    MOD_CHECK(mods_settings_entry(1, &owner, &mod_id, &key, &label, &kind, &value, &min, &max));
    MOD_CHECK_EQ(owner, 3u);
    MOD_CHECK(!mods_settings_entry(2, &owner, &mod_id, &key, &label, &kind, &value, &min, &max));
}

MOD_TEST_SUITE(keypad_settings_round_trip) {
    fresh();
    mods_keypad_reset();
    mods_keypad_init(0);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_LEFT_ROW), 1);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_RIGHT_ROW), 1);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_SIZE_ROW), 1);
    MOD_CHECK_EQ(mods_keypad_set(KEYPAD_SIZE_ROW, 7), POP_OK); // clamped into the range
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_SIZE_ROW), 2);
    // Shown/hidden turns over either way; size stops at its ends.
    MOD_CHECK_EQ(mods_keypad_nudge(KEYPAD_RIGHT_ROW, +1), POP_OK);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_RIGHT_ROW), 0);
    MOD_CHECK_EQ(mods_keypad_nudge(KEYPAD_RIGHT_ROW, +1), POP_OK);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_RIGHT_ROW), 1);
    MOD_CHECK_EQ(mods_keypad_nudge(KEYPAD_SIZE_ROW, +1), POP_OK);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_SIZE_ROW), 2);
    MOD_CHECK_EQ(mods_keypad_nudge(KEYPAD_LEFT_ROW, -1), POP_OK);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_LEFT_ROW), 0);
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(MODS_OWNER_RUNTIME, "left", &v), POP_OK);
    MOD_CHECK_EQ(v, 0);
    MOD_CHECK(mods_keypad_line(KEYPAD_LEFT_ROW).find("hidden") != std::string::npos);
    MOD_CHECK(mods_keypad_line(KEYPAD_SIZE_ROW).find("Large") != std::string::npos);
    // The saved values come back on the next init, under the host.keypad/ keys.
    mods_settings_reset();
    mods_keypad_reset();
    MOD_CHECK(mods_settings_load(mods_settings_path()));
    mods_keypad_init(0);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_LEFT_ROW), 0);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_SIZE_ROW), 2);
    // A game that starts hidden does so only without a saved value.
    mods_settings_reset();
    mods_keypad_reset();
    mods_keypad_init(1);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_LEFT_ROW), 0);
    MOD_CHECK_EQ(mods_keypad_value(KEYPAD_RIGHT_ROW), 0);
}

// Settings suites run before any suite loads a symbol map. Exercise the
// fallback page itself, including its row metadata used for navigation.
MOD_TEST_SUITE(settings_page_without_symbols_shows_host_controls) {
    fresh();
    MOD_CHECK_EQ(mods_symbols_count(), 0u);
    mods_host_set_main_thread();
    mods_display_reset();
    mods_page_init();
    MOD_CHECK_EQ(mods_page_open(nullptr), POP_OK);
    const DisplayRow hidden[] = {DISPLAY_RENDERING,    DISPLAY_UI_SCALE, DISPLAY_WIDE,
                                 DISPLAY_CLASSIC_MODE, DISPLAY_TEXTURES, DISPLAY_FILTERING};
    const DisplayRow visible[] = {DISPLAY_WINDOW, DISPLAY_FPS, DISPLAY_OVERLAY};
    auto page_has_row = [](DisplayRow row) {
        const std::string label = mods_display_line(row);
        for (uint32_t i = 0; i < mods_page_line_count(); ++i)
            if (label == mods_page_line(i))
                return true;
        return false;
    };
    for (DisplayRow row : hidden) {
        MOD_CHECK(!mods_display_row_applies(row));
        MOD_CHECK(!page_has_row(row));
    }
    for (DisplayRow row : visible) {
        MOD_CHECK(mods_display_row_applies(row));
        MOD_CHECK(page_has_row(row));
    }
    MOD_CHECK_EQ(mods_page_line_count(), 3u + KEYPAD_ROW_COUNT);
    // The first visible row still targets window mode after filtering.
    mods_input_key(0xcd, 0, true);
    mods_input_key(0xcd, 0, false);
    MOD_CHECK_EQ(mods_display_value(DISPLAY_WINDOW), 1);
    mods_page_close();
    mods_input_remove_all(MODS_OWNER_RUNTIME);
    mods_display_reset();
    mods_keypad_reset();
    mods_settings_reset();
}

// Like the stub, this port can have no usable symbol table. The existing
// layout test seam makes that failure deterministic without touching a map.
MOD_TEST_SUITE(settings_load_before_missing_symbols) {
    mods_host_set_main_thread();
    MOD_CHECK(mods_test_reset_loader());
    fresh();
    mods_display_reset();
    MOD_CHECK_EQ(mods_symbols_count(), 0u);
    FILE *f = fopen(mods_settings_path(), "wb");
    MOD_CHECK(f != nullptr);
    if (f) {
        MOD_CHECK(fputs("{\"host.display/window\": 2}\n", f) >= 0);
        MOD_CHECK_EQ(fclose(f), 0);
    }
    host_layout_set_exe_path_for_test("/nowhere/at/all/exe");
    MOD_CHECK(!mods_load_all());
    MOD_CHECK_EQ(mods_symbols_count(), 0u);
    mods_display_init();
    int64_t v = -1;
    MOD_CHECK_EQ(mods_settings_get(MODS_OWNER_RUNTIME, "window", &v), POP_OK);
    MOD_CHECK_EQ(v, 2);
    MOD_CHECK_EQ(mods_display_value(DISPLAY_WINDOW), 2);
    MOD_CHECK(mods_test_reset_loader());
    host_layout_set_exe_path_for_test(nullptr);
    win32_set_file_ops(nullptr, nullptr);
    mods_display_reset();
    mods_settings_reset();
}
