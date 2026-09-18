// builtin_mods.cpp - the empty table a build without built-in plugins links.
// cmake/BuiltinMods.cmake generates the real one where a platform needs it;
// that definition is strong and replaces this one.
#include "builtin_mods.h"

#ifdef _WIN32
extern const RecompBuiltinMod recomp_builtin_mods[] = {{nullptr, nullptr, nullptr, nullptr}};
#else
__attribute__((weak)) extern const RecompBuiltinMod recomp_builtin_mods[] = {{nullptr, nullptr, nullptr, nullptr}};
#endif
