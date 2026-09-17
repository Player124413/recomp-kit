/* native_seam.h - services a game's native overrides may call.
 *
 * A game's [translate] overrides header is compiled into the translation as C,
 * so everything here has C linkage and C types. None of it names a game.
 */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The size, in points, of the display the program's window is shown on - the
 * area a fullscreen window fills. Non-zero with both set, or zero
 * when the host has no display to report. Safe from any guest thread. */
int host_display_screen_size(int *w, int *h);

/* The host path a guest file would be written to in the writable overlay tier
 * (the player's profile). Zero, with nothing written, when there is no such
 * tier: a native override must never write into the game's own directory. */
int recomp_writable_path(const char *guest_path, char *out, size_t out_len);

/* The host path a guest file is read from, through every overlay tier and
 * then the game directory. Zero when the path cannot be resolved. */
int recomp_readable_path(const char *guest_path, char *out, size_t out_len);

/* Offer a DirectDraw display mode, as the host does for its own modes. */
int ddraw_add_mode(int w, int h, int bpp);

#ifdef __cplusplus
}
#endif
