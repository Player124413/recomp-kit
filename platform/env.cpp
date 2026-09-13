// env.cpp - the kit's environment switches, RECOMP_<NAME>.
//
// One reader, so every switch is spelled the same way and a listing of them
// is a search for `recomp_env(`.  The prefix names the kit, not a game.
#include "platform/os.h"

#include <stdlib.h>
#include <string.h>

extern "C" {

const char *recomp_env(const char *name) {
    char buf[160];
    if (strlen(name) + sizeof "RECOMP_" > sizeof buf)
        return NULL;
    strcpy(buf, "RECOMP_");
    strcat(buf, name);
    return getenv(buf);
}

} // extern "C"
