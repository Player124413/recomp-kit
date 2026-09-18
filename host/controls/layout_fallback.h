// layout_fallback.h - which layout the host actually loads: the one for the
// screen's form factor, or, when a name has no layout for it, its tablet one.
// Also the clamp the per-name hidden-group bits need when the forms of one
// layout name differ in how many groups they have.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once

#include "layout_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace controls {

// store.load(name, form), then, for a phone form that found nothing,
// store.load(name, Form::Tablet). *fell_back says whether the tablet layout
// was used; *problem is the first parse failure either load reported.
inline bool load_with_tablet_fallback(const LayoutStore &store, const std::string &name, Form form,
                                      Layout *out, std::string *problem, bool *fell_back) {
    *fell_back = false;
    if (store.load(name, form, out, problem))
        return true;
    if (form == Form::Tablet)
        return false;
    std::string tablet_problem;
    if (!store.load(name, Form::Tablet, out, &tablet_problem)) {
        if (problem->empty())
            *problem = tablet_problem;
        return false;
    }
    *fell_back = true;
    return true;
}

// The groups the player has hidden, as the settings row stores them: bit i
// is groups[i], for the first kHiddenBits groups.
constexpr size_t kHiddenBits = 16;

// The stored bits as they apply to a layout with `groups` groups. The bits
// belong to a layout *name*, and one name's forms need not have the same
// groups (portrait "keys" is one block where landscape is two halves), so a
// bit past the last group would otherwise hide a group the layout does not
// have, or come back as a phantom difference every pump.
inline uint32_t clamp_hidden_bits(uint32_t bits, size_t groups) {
    const size_t n = groups < kHiddenBits ? groups : kHiddenBits; // at most 16
    return bits & ((1u << n) - 1u);
}

// Which of the stored bits apply to `l`: those past its last group are
// dropped, and so is any bit on a group holding a cycle toggle (target
// "next"). A name's forms can disagree about what group i is -- portrait
// "keys" is one board where landscape is two halves -- and a bit that landed
// on the group holding the cycle tab would leave no on-screen way to any
// other layout, which is the one state a player cannot get out of. A group
// with no cycle toggle can still be hidden whole by an inherited bit; the F10
// page is then the way back, so nothing (the editor included) may treat this
// as a promise that every group stays reachable on screen.
inline uint32_t hidden_bits_for(const Layout &l, uint32_t stored) {
    uint32_t bits = clamp_hidden_bits(stored, l.groups.size());
    for (size_t i = 0; i < l.groups.size() && i < kHiddenBits; ++i)
        for (const Control &c : l.groups[i].controls)
            if (c.kind == Kind::Toggle && c.target == "next") {
                bits &= ~(1u << i);
                break;
            }
    return bits;
}

} // namespace controls
