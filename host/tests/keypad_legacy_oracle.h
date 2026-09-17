// keypad_legacy_oracle.h - a frozen, renamed copy of today's split-keypad
// geometry (host/keypad_layout.cpp), used only to pin the built-in "keys"
// controls layout to that exact geometry in controls_tests.cpp. This is a
// regression oracle, not a mirror: it must never be updated to track future
// changes to keypad_layout.cpp. Reuses KeypadKey/KeypadRect/KeypadSide from
// keypad_layout.h (header only; that .cpp is not linked into controls_tests).
#pragma once

#include "../keypad_layout.h"

// The keys of one half, in the same order as keypad_layout.cpp's tables;
// *count receives how many.
const KeypadKey *legacy_keypad_keys(KeypadSide side, int *count);
// The half's rectangle, bottom-aligned in its corner, for a drawable of dw x dh.
KeypadRect legacy_keypad_half_rect(KeypadSide side, int size, double scale, int dw, int dh);
// The tab: above the half's outer corner while shown, in the corner while hidden.
KeypadRect legacy_keypad_tab_rect(KeypadSide side, bool shown, int size, double scale, int dw,
                                  int dh);
// One key's rectangle (the cell minus the gap).
KeypadRect legacy_keypad_key_rect(KeypadSide side, const KeypadKey &key, int size, double scale,
                                  int dw, int dh);
