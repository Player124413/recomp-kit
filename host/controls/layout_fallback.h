// layout_fallback.h - which layout the host actually loads: the one for the
// screen's form factor, or, while a name has no phone layout anywhere (the
// phone built-ins are still to come), its tablet one.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md.
#pragma once

#include "layout_store.h"

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

} // namespace controls
