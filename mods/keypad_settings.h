// keypad_settings.h - the on-screen keypad's three persisted values, declared
// in the mod settings store like the display rows so they show on the F10
// page and survive a relaunch. Design: docs/superpowers/specs/2026-09-13-keypad-design.md.
#pragma once
#include "pop_mod_api.h"
#include <string>

enum KeypadRow { KEYPAD_LEFT_ROW, KEYPAD_RIGHT_ROW, KEYPAD_SIZE_ROW, KEYPAD_ROW_COUNT };

// default_hidden: 1 starts both halves hidden (game.toml [touch] keypad = "hidden").
// Idempotent; mods_keypad_reset() lets the tests start over.
void mods_keypad_init(int default_hidden);
void mods_keypad_reset();
int mods_keypad_value(KeypadRow row); // left/right: 0 hidden, 1 shown; size 0..2
PopModStatus mods_keypad_set(KeypadRow row, int value);
PopModStatus mods_keypad_nudge(KeypadRow row, int delta);
std::string mods_keypad_line(KeypadRow row);
