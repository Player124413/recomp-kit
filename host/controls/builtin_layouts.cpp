// builtin_layouts.cpp - see builtin_layouts.h.
#include "builtin_layouts.h"

namespace controls {

namespace {

// The split on-screen keyboard. Pinned to host/keypad_layout.cpp's geometry
// (a fixed 8x5 grid of 32/36/40pt keys, 4pt gap, bottom-corner halves, with
// a HIDE/KEYS tab above each) by controls_tests.cpp's legacy oracle. The
// PAD tab (the next layout) sits at the bottom centre, leaving the top of
// the screen, where games keep their menus, to the game.
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
         "anchor": "bottom-right", "w": 64, "h": 20, "stack_on": "right"},
        {"kind": "toggle", "target": "next", "label": "PAD", "anchor": "bottom-center",
         "w": 64, "h": 20}
      ]
    }
  ]
})JSON";

// A DualSense-shaped pad: floating sticks in the bottom corners, the dpad
// beside the left one, the face diamond in the bottom-right corner,
// shoulders in the top corners and the system buttons at the bottom centre.
// L3 and R3 are not on screen by default. Translucent, unlike the keyboard.
const char *kPadTablet = R"JSON({
  "version": 1,
  "name": "pad",
  "opacity": 0.7,
  "groups": [
    {
      "id": "sticks",
      "controls": [
        {"kind": "stick", "stick": "left", "mode": "floating", "anchor": "bottom-left",
         "x": 24, "y": 24, "radius": 110},
        {"kind": "stick", "stick": "right", "mode": "floating", "anchor": "bottom-right",
         "x": 250, "y": 24, "radius": 90}
      ]
    },
    {
      "id": "buttons",
      "controls": [
        {"kind": "dpad", "anchor": "bottom-left", "x": 250, "y": 40, "size": 130},
        {"kind": "button", "button": "triangle", "anchor": "bottom-right", "x": 92, "y": 164, "size": 64},
        {"kind": "button", "button": "circle", "anchor": "bottom-right", "x": 24, "y": 96, "size": 64},
        {"kind": "button", "button": "cross", "anchor": "bottom-right", "x": 92, "y": 28, "size": 64},
        {"kind": "button", "button": "square", "anchor": "bottom-right", "x": 160, "y": 96, "size": 64},
        {"kind": "button", "button": "l1", "anchor": "top-left", "x": 24, "y": 24, "w": 110, "h": 44},
        {"kind": "button", "button": "l2", "anchor": "top-left", "x": 24, "y": 76, "w": 110, "h": 44},
        {"kind": "button", "button": "r1", "anchor": "top-right", "x": 24, "y": 24, "w": 110, "h": 44},
        {"kind": "button", "button": "r2", "anchor": "top-right", "x": 24, "y": 76, "w": 110, "h": 44},
        {"kind": "button", "button": "select", "anchor": "bottom-center", "x": -70, "y": 16, "w": 70, "h": 30},
        {"kind": "button", "button": "ps", "anchor": "bottom-center", "x": 0, "y": 12, "size": 40},
        {"kind": "button", "button": "start", "anchor": "bottom-center", "x": 70, "y": 16, "w": 70, "h": 30}
      ]
    },
    {
      "id": "tabs",
      "controls": [
        {"kind": "toggle", "target": "next", "label": "KEYS", "anchor": "top-center",
         "y": 8, "w": 72, "h": 28}
      ]
    }
  ]
})JSON";

// The pad+keys layout: the keyboard halves at 30pt keys, the two sticks at
// mid-height on either side, and a smaller face diamond above the right
// half (the pad layout's diamond at 52pt, raised by the half's 5 * 34pt
// height plus 12). Its NEXT tab stays where keys puts its PAD tab, at the
// bottom centre between the halves.
std::string pad_and_keys_tablet() {
    std::string s = kKeysTablet;
    const auto replace_all = [&s](const std::string &from, const std::string &to) {
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
            s.replace(at, from.size(), to);
    };
    replace_all("\"name\": \"keys\"", "\"name\": \"pad+keys\"");
    replace_all("\"key\": 36", "\"key\": 30");
    replace_all("\"label\": \"PAD\"", "\"label\": \"NEXT\"");
    const std::string tail = "\n  ]\n}";
    const size_t end = s.rfind(tail);
    if (end == std::string::npos)
        return std::string();
    s.replace(end, tail.size(), R"JSON(,
    {
      "id": "sticks",
      "controls": [
        {"kind": "stick", "stick": "left", "mode": "floating", "anchor": "center-left",
         "x": 24, "y": -90, "radius": 80},
        {"kind": "stick", "stick": "right", "mode": "floating", "anchor": "center-right",
         "x": 24, "y": -90, "radius": 80}
      ]
    },
    {
      "id": "face",
      "controls": [
        {"kind": "button", "button": "triangle", "anchor": "bottom-right", "x": 80, "y": 318, "size": 52},
        {"kind": "button", "button": "circle", "anchor": "bottom-right", "x": 24, "y": 262, "size": 52},
        {"kind": "button", "button": "cross", "anchor": "bottom-right", "x": 80, "y": 206, "size": 52},
        {"kind": "button", "button": "square", "anchor": "bottom-right", "x": 136, "y": 262, "size": 52}
      ]
    }
  ]
})JSON");
    return s;
}

} // namespace

const char *builtin_layout(const std::string &name, Form form) {
    if (form != Form::Tablet)
        return nullptr;
    if (name == "keys")
        return kKeysTablet;
    if (name == "pad")
        return kPadTablet;
    if (name == "pad+keys") {
        static const std::string text = pad_and_keys_tablet();
        return text.c_str();
    }
    return nullptr;
}

} // namespace controls
