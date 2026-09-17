// builtin_layouts.cpp - see builtin_layouts.h.
#include "builtin_layouts.h"

namespace controls {

namespace {

// The split on-screen keyboard. Pinned to host/keypad_layout.cpp's geometry
// (a fixed 8x5 grid of 32/36/40pt keys, 4pt gap, bottom-corner halves, with
// a HIDE/KEYS tab above each) by controls_tests.cpp's legacy oracle.
const char *kKeysTablet = R"JSON({
  "version": 1,
  "name": "keys",
  "safe_inset": false,
  "groups": [
    {
      "id": "left",
      "grid": {"cols": 8, "rows": 5, "key": 36, "gap": 4},
      "anchor": "bottom-left",
      "controls": [
        {"kind": "key", "scancode": "Escape", "label": "Esc", "col": 0, "row": 0},
        {"kind": "key", "scancode": "F1", "col": 1, "row": 0},
        {"kind": "key", "scancode": "F2", "col": 2, "row": 0},
        {"kind": "key", "scancode": "F3", "col": 3, "row": 0},
        {"kind": "key", "scancode": "F4", "col": 4, "row": 0},
        {"kind": "key", "scancode": "F5", "col": 5, "row": 0},
        {"kind": "key", "scancode": "F6", "col": 6, "row": 0},
        {"kind": "key", "scancode": "Insert", "label": "Ins", "col": 7, "row": 0},
        {"kind": "key", "scancode": "Grave", "label": "`", "col": 0, "row": 1},
        {"kind": "key", "scancode": "1", "col": 1, "row": 1},
        {"kind": "key", "scancode": "2", "col": 2, "row": 1},
        {"kind": "key", "scancode": "3", "col": 3, "row": 1},
        {"kind": "key", "scancode": "4", "col": 4, "row": 1},
        {"kind": "key", "scancode": "5", "col": 5, "row": 1},
        {"kind": "key", "scancode": "6", "col": 6, "row": 1},
        {"kind": "key", "scancode": "Home", "col": 7, "row": 1},
        {"kind": "key", "scancode": "Tab", "col": 0, "row": 2},
        {"kind": "key", "scancode": "Q", "col": 1, "row": 2},
        {"kind": "key", "scancode": "W", "col": 2, "row": 2},
        {"kind": "key", "scancode": "E", "col": 3, "row": 2},
        {"kind": "key", "scancode": "R", "col": 4, "row": 2},
        {"kind": "key", "scancode": "T", "col": 5, "row": 2},
        {"kind": "key", "scancode": "LeftBracket", "label": "[", "col": 6, "row": 2},
        {"kind": "key", "scancode": "PageUp", "label": "PgUp", "col": 7, "row": 2},
        {"kind": "key", "scancode": "LShift", "label": "Shift", "col": 0, "row": 3},
        {"kind": "key", "scancode": "A", "col": 1, "row": 3},
        {"kind": "key", "scancode": "S", "col": 2, "row": 3},
        {"kind": "key", "scancode": "D", "col": 3, "row": 3},
        {"kind": "key", "scancode": "F", "col": 4, "row": 3},
        {"kind": "key", "scancode": "G", "col": 5, "row": 3},
        {"kind": "key", "scancode": "RightBracket", "label": "]", "col": 6, "row": 3},
        {"kind": "key", "scancode": "PageDown", "label": "PgDn", "col": 7, "row": 3},
        {"kind": "key", "scancode": "LCtrl", "label": "Ctrl", "col": 0, "row": 4},
        {"kind": "key", "scancode": "LAlt", "label": "Alt", "col": 1, "row": 4},
        {"kind": "key", "scancode": "Z", "col": 2, "row": 4},
        {"kind": "key", "scancode": "X", "col": 3, "row": 4},
        {"kind": "key", "scancode": "C", "col": 4, "row": 4},
        {"kind": "key", "scancode": "V", "col": 5, "row": 4},
        {"kind": "key", "scancode": "B", "col": 6, "row": 4},
        {"kind": "key", "scancode": "Backslash", "label": "\\", "col": 7, "row": 4}
      ]
    },
    {
      "id": "right",
      "grid": {"cols": 8, "rows": 5, "key": 36, "gap": 4},
      "anchor": "bottom-right",
      "controls": [
        {"kind": "key", "scancode": "F7", "col": 0, "row": 0},
        {"kind": "key", "scancode": "F8", "col": 1, "row": 0},
        {"kind": "key", "scancode": "F9", "col": 2, "row": 0},
        {"kind": "key", "scancode": "F10", "col": 3, "row": 0},
        {"kind": "key", "scancode": "F11", "col": 4, "row": 0},
        {"kind": "key", "scancode": "F12", "col": 5, "row": 0},
        {"kind": "key", "scancode": "Delete", "label": "Del", "col": 6, "row": 0},
        {"kind": "key", "scancode": "End", "col": 7, "row": 0},
        {"kind": "key", "scancode": "7", "col": 0, "row": 1},
        {"kind": "key", "scancode": "8", "col": 1, "row": 1},
        {"kind": "key", "scancode": "9", "col": 2, "row": 1},
        {"kind": "key", "scancode": "0", "col": 3, "row": 1},
        {"kind": "key", "scancode": "Minus", "label": "-", "col": 4, "row": 1},
        {"kind": "key", "scancode": "Equals", "label": "=", "col": 5, "row": 1},
        {"kind": "key", "scancode": "Backspace", "label": "Bksp", "col": 6, "row": 1, "span": 2},
        {"kind": "key", "scancode": "Y", "col": 0, "row": 2},
        {"kind": "key", "scancode": "U", "col": 1, "row": 2},
        {"kind": "key", "scancode": "I", "col": 2, "row": 2},
        {"kind": "key", "scancode": "O", "col": 3, "row": 2},
        {"kind": "key", "scancode": "P", "col": 4, "row": 2},
        {"kind": "key", "scancode": "Semicolon", "label": ";", "col": 5, "row": 2},
        {"kind": "key", "scancode": "Apostrophe", "label": "'", "col": 6, "row": 2},
        {"kind": "key", "scancode": "Return", "label": "Enter", "col": 7, "row": 2},
        {"kind": "key", "scancode": "H", "col": 0, "row": 3},
        {"kind": "key", "scancode": "J", "col": 1, "row": 3},
        {"kind": "key", "scancode": "K", "col": 2, "row": 3},
        {"kind": "key", "scancode": "L", "col": 3, "row": 3},
        {"kind": "key", "scancode": "Comma", "label": ",", "col": 4, "row": 3},
        {"kind": "key", "scancode": "Period", "label": ".", "col": 5, "row": 3},
        {"kind": "key", "scancode": "Slash", "label": "/", "col": 6, "row": 3},
        {"kind": "key", "scancode": "Up", "label": "^", "col": 7, "row": 3},
        {"kind": "key", "scancode": "Space", "col": 0, "row": 4, "span": 3},
        {"kind": "key", "scancode": "N", "col": 3, "row": 4},
        {"kind": "key", "scancode": "M", "col": 4, "row": 4},
        {"kind": "key", "scancode": "Left", "label": "<", "col": 5, "row": 4},
        {"kind": "key", "scancode": "Down", "label": "v", "col": 6, "row": 4},
        {"kind": "key", "scancode": "Right", "label": ">", "col": 7, "row": 4}
      ]
    },
    {
      "id": "tabs",
      "controls": [
        {"kind": "toggle", "target": "left", "label": "HIDE", "label_off": "KEYS",
         "anchor": "bottom-left", "w": 64, "h": 20, "stack_on": "left"},
        {"kind": "toggle", "target": "right", "label": "HIDE", "label_off": "KEYS",
         "anchor": "bottom-right", "w": 64, "h": 20, "stack_on": "right"}
      ]
    }
  ]
})JSON";

} // namespace

const char *builtin_layout(const std::string &name, Form form) {
    if (name == "keys" && form == Form::Tablet)
        return kKeysTablet;
    return nullptr;
}

} // namespace controls
