#pragma once
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Internal, unpinned host/mod seam. No AppKit or frame pointers cross it. */
int32_t host_display_anchor(uint64_t id, int8_t h, int8_t v, int clear);
uint32_t host_display_elements(uint64_t *ids, uint32_t max);
float host_display_aspect(void);
uint64_t host_display_epoch(void);
int host_display_offer_mode(int w, int h, int bpp);
void host_display_request_window(int mode); /* queued; 0 windowed, 1 borderless, 2 fullscreen */
int host_display_take_window(void);         /* main thread drains; -1 when unchanged */
void mods_display_transition(uint64_t epoch, int screen_class);
int mods_display_classic(void);
int mods_display_scale(void);
int mods_display_wide(void);
void mods_display_scene_domain(int w, int h);
int mods_display_scene_width(int guest_w, int guest_h);
int mods_display_fps(void);       /* 0 original, otherwise requested new-frame limit */
int mods_display_overlay(void);   /* 0 off, 1 counters, 2 frame-time graph */
int mods_display_textures(void);  /* 0 original, 1 HD pack */
int mods_display_filtering(void); /* 0 original, 1 trilinear, 2 4x, 3 8x, 4 16x */
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Synchronous owned-pixel snapshot: the host copies ARGB before returning.
void host_display_present_window(const uint32_t *argb, int w, int h);
// Seconds to the presenter's next refresh boundary, `intervals` refreshes
// on - when a Present with a sync interval returns. Zero on a headless
// presenter. The caller waits in the scheduler, never in the host.
double host_present_refresh_delay(int intervals);
bool ddraw_gdi_primary_active(void);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
uint32_t ddraw_gdi_begin_primary(void);
void ddraw_gdi_end_primary(uint32_t dc);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
// An external software presenter owns one display layer above the window
// canvases. The runtime copies pixels synchronously and keeps the last frame
// across GDI refreshes; owner identifies which presenter may retire it.
void gdi_present_surface(uint32_t owner, uint32_t hwnd, const uint32_t *argb, int w, int h,
                         bool fullscreen);
void gdi_forget_surface(uint32_t owner);
#ifdef __cplusplus
}
#endif
