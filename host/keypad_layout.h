// keypad_layout.h - the split on-screen keyboard: what the keys are and where
// they sit, in drawable pixels. Two halves of 8x5 cells park in the bottom
// corners; a tab above each half's outer corner hides it to a corner tab.
// The same numbers place the drawing and answer the hit test. Pure: no SDL,
// no GPU. Design: docs/superpowers/specs/2026-09-13-keypad-design.md.
#pragma once

enum KeypadSide { KEYPAD_LEFT = 0, KEYPAD_RIGHT = 1 };

// SDL_Scancode values, spelled out so this file needs no SDL header.
// keypad_tests.cpp asserts they match SDL's enumerators.
enum KeypadScan {
    kScanA = 4,
    kScanB,
    kScanC,
    kScanD,
    kScanE,
    kScanF,
    kScanG,
    kScanH,
    kScanI,
    kScanJ,
    kScanK,
    kScanL,
    kScanM,
    kScanN,
    kScanO,
    kScanP,
    kScanQ,
    kScanR,
    kScanS,
    kScanT,
    kScanU,
    kScanV,
    kScanW,
    kScanX,
    kScanY,
    kScanZ, // 29
    kScan1 = 30,
    kScan2,
    kScan3,
    kScan4,
    kScan5,
    kScan6,
    kScan7,
    kScan8,
    kScan9,
    kScan0, // 39
    kScanReturn = 40,
    kScanEscape = 41,
    kScanBackspace = 42,
    kScanTab = 43,
    kScanSpace = 44,
    kScanMinus = 45,
    kScanEquals = 46,
    kScanLeftBracket = 47,
    kScanRightBracket = 48,
    kScanBackslash = 49,
    kScanSemicolon = 51,
    kScanApostrophe = 52,
    kScanGrave = 53,
    kScanComma = 54,
    kScanPeriod = 55,
    kScanSlash = 56,
    kScanF1 = 58,
    kScanF2,
    kScanF3,
    kScanF4,
    kScanF5,
    kScanF6,
    kScanF7,
    kScanF8,
    kScanF9,
    kScanF10,
    kScanF11,
    kScanF12, // 69
    kScanInsert = 73,
    kScanHome = 74,
    kScanPageUp = 75,
    kScanDelete = 76,
    kScanEnd = 77,
    kScanPageDown = 78,
    kScanRight = 79,
    kScanLeft = 80,
    kScanDown = 81,
    kScanUp = 82,
    kScanLCtrl = 224,
    kScanLShift = 225,
    kScanLAlt = 226,
};

struct KeypadKey {
    const char *label; // drawn, at most 5 glyphs
    int scancode;      // KeypadScan value sent while the key is held
    int col, row;      // cell position in the half's grid
    int span;          // width in cells
};

struct KeypadRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const {
        return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
    }
};

// What the presenter draws and the hit test consults.
struct KeypadView {
    bool wanted = false;            // draw and hit-test anything at all (no hardware keyboard)
    bool left = true, right = true; // half shown (else only its KEYS tab)
    int size = 1;                   // 0 small, 1 medium, 2 large
    unsigned lit = 0;               // keypad_modifier_bit() bits: latched or locked
    double scale = 1.0;             // drawable pixels per window point
};

struct KeypadHit {
    enum Kind { None, Key, Toggle } kind = None;
    KeypadSide side = KEYPAD_LEFT;
    int scancode = 0; // Key only; 0 for a gap between keys
};

constexpr int kKeypadCols = 8, kKeypadRows = 5;
constexpr int kKeypadKeyPt[3] = {32, 36, 40};
constexpr int kKeypadGapPt = 4;
constexpr int kKeypadTabWPt = 64, kKeypadTabHPt = 20;

// The keys of one half; *count receives how many.
const KeypadKey *keypad_keys(KeypadSide side, int *count);
// Cell pitch (key plus gap) and gap, in drawable pixels, for a size step.
int keypad_pitch_px(int size, double scale);
int keypad_gap_px(double scale);
// The half's rectangle, bottom-aligned in its corner, for a drawable of dw x dh.
KeypadRect keypad_half_rect(KeypadSide side, int size, double scale, int dw, int dh);
// The tab: above the half's outer corner while shown, in the corner while hidden.
KeypadRect keypad_tab_rect(KeypadSide side, bool shown, int size, double scale, int dw, int dh);
// One key's rectangle (the cell minus the gap).
KeypadRect keypad_key_rect(KeypadSide side, const KeypadKey &key, int size, double scale, int dw,
                           int dh);
// What is under (px, py): a key, a tab, or nothing (the game's). A point inside
// a shown half but between keys is a Key hit with scancode 0, so the gap never
// clicks through to the game. Nothing at all while the view is not wanted.
KeypadHit keypad_hit(const KeypadView &view, int dw, int dh, double px, double py);
bool keypad_is_modifier(int scancode);
// The lit bit for a modifier scancode (0 for other keys).
unsigned keypad_modifier_bit(int scancode);
