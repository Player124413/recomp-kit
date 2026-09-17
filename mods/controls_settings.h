// controls_settings.h - the on-screen controls' persisted values, declared in
// the mod settings store like the display rows so they show on the F10 page
// and survive a relaunch. Replaces the old keypad settings (mods/keypad_
// settings.h); see docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once
#include "pop_mod_api.h"
#include <stdint.h>
#include <string>
#include <vector>

enum ControlsRow {
    CONTROLS_LAYOUT_ROW,
    CONTROLS_SIZE_ROW,
    CONTROLS_OPACITY_ROW,
    CONTROLS_HAPTICS_ROW,
    CONTROLS_PAD_WITH_CONTROLLER_ROW,
    CONTROLS_SNAP_ROW,
    CONTROLS_EDIT_ROW,
    CONTROLS_ROW_COUNT
};

// The host registers the layout names (LayoutStore::names()) before
// mods_page_init, because mods/ must not depend on host/controls. Until it
// does, the built-in defaults {"pad","keys","pad+keys"} apply. The layout
// setting's range is 0..names.size(); the last index is the Hidden choice.
void mods_controls_set_names(std::vector<std::string> names);
// Re-clamp the layout row after the names changed (Task 21).
void mods_controls_refresh_names();
// default_layout: RECOMP_CONTROLS_DEFAULT_LAYOUT, e.g. "pad", "keys",
// "pad+keys" or "hidden". Idempotent; mods_controls_reset() lets the tests
// start over. Migrates a saved host.keypad/* profile the first time it sees one.
void mods_controls_init(const char *default_layout);
void mods_controls_reset();
// layout: index into the names list (names.size() is Hidden); size 0..2;
// opacity 20..100; haptics/pad_with_controller/snap 0..1; EDIT_ROW: 0.
int mods_controls_value(ControlsRow row);
// names[index], or "" when the layout row is on the Hidden choice.
std::string mods_controls_layout_name();
PopModStatus mods_controls_set(ControlsRow row, int value);
// EDIT_ROW ignores the delta and requests the editor instead of a value change.
PopModStatus mods_controls_nudge(ControlsRow row, int delta);
std::string mods_controls_line(ControlsRow row);
// Hidden groups of the active layout, bit i = groups[i] hidden. Not shown on
// the settings page; the touch router (Task 7+) reads it directly.
uint32_t mods_controls_hidden_groups();
void mods_controls_set_hidden_groups(uint32_t bits);
// Set by the EDIT row; the host polls this and clears it.
bool mods_controls_take_edit_request();
