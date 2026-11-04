/*
 * host.h - PC preview harness control API (implemented in plat_host.c)
 *
 * The gfx.h software backend owns the two 400x240 / 320x240 framebuffers and
 * can dump them to PNG files.  This small side-channel lets the scripted
 * driver (main.c) decide *which* frames get captured without polluting the
 * game-facing gfx.h contract.
 *
 * Typical driver use per frame:
 *
 *     if (capture_this_frame)
 *         host_capture_request(frame_index);
 *     app_draw(&app);              // gfx_begin_frame ... gfx_present
 *     host_capture_finish();       // safety net if app_draw never presented
 */
#ifndef HOST_PREVIEW_H
#define HOST_PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configure capture: outdir directory (created by driver beforehand), and
 * whether capture is allowed at all.  Call once at startup. */
void host_capture_configure(const char *outdir, int enabled);

/* When defer != 0, gfx_present() does NOT write PNGs; the driver must call
 * host_capture_finish() after app_draw().  Default is 0 (capture in present,
 * which is what the gfx.h contract describes). */
void host_capture_set_defer(int defer);

/* Arm capture for frame number `frame_no`.  Call before app_draw(). */
void host_capture_request(int frame_no);

/* Write both PNGs for `frame_no` if they have not been written yet.
 * Returns 1 if this call wrote them, 0 otherwise. */
int host_capture_finish(int frame_no);

/* Number of gfx_present() calls seen so far (diagnostics; also tells the
 * driver whether app_draw presents once per frame). */
int host_present_count(void);

/* Last frame number written to disk, or -1. */
int host_last_captured_frame(void);

/* Diagnostics for "is this screenshot actually non-blank?" checks.
 * screen: 0 = top (400x240), 1 = bottom (320x240).
 * host_screen_signature: 32-bit FNV-1a hash of the current framebuffer.
 * host_screen_distinct_colors: number of distinct RGB values in it. */
uint32_t host_screen_signature(int screen);
int host_screen_distinct_colors(int screen);

#ifdef __cplusplus
}
#endif

#endif /* HOST_PREVIEW_H */
