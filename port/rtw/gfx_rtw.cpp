// gfx_rtw.cpp — fast3d window backend of the retrowser.app build (GE_RTW; replaces gfx_sdl2.cpp there).
//
// The game runs in a Web Worker and draws with WebGL2 straight onto the OffscreenCanvas the host hands to
// the module factory (Module.canvas; context made in port/rtw/rtw_lib.js gertw_gl_create). No SDL window,
// no page listeners. The drawing buffer belongs to the module (spec §4.5): its size follows rtw_resize and
// the Resolution option (port/rtw/rtw.c geRtwBufferSize), applied between frames in handle_events. A worker
// commits WebGL frames at the end of each task, so there is no explicit swap. Exports the same symbols as
// gfx_sdl2.cpp (gfx_sdl, gfx_sdl_* helpers, gfx_pre_swap_hook) so video.c is unchanged.
#include <stdint.h>
#include <stdbool.h>
#include <emscripten.h>
#include "../fast3d/gfx_window_manager_api.h"
#include "system.h"

extern "C" int gertw_gl_create(int w, int h);       // rtw_lib.js
extern "C" void gertw_canvas_size(int w, int h);    // rtw_lib.js
extern "C" void geRtwBufferSize(int *w, int *h);    // rtw.c
extern "C" int geRtwTakeResize(int *w, int *h);     // rtw.c
extern "C" int geRtwTakeShotRequest(void);          // rtw.c (extra button Screenshot)
extern "C" void geRtwShotWritten(const char *ppmPath);
extern "C" void geRtwPump(void);                    // rtw.c: options overlay requests, inGame option changes
extern "C" bool gfx_opengl_dump_bound_fbo(uint32_t width, uint32_t height, const char *path);   // gfx_opengl.cpp

static int s_w = 640, s_h = 480;
static int s_ctx;
static int s_target_fps;

extern "C" void (*gfx_pre_swap_hook)(void) = NULL;

static void rtw_init(const struct GfxWindowInitSettings *set) {
    (void)set;
    geRtwTakeResize(&s_w, &s_h);
    s_ctx = gertw_gl_create(s_w, s_h);
    if (!s_ctx) {
        sysFatalError("Could not create a WebGL2 context on the host's canvas");
    }
    sysLogPrintf(LOG_NOTE, "rtw: WebGL2 context on the OffscreenCanvas, %dx%d", s_w, s_h);
}
static void rtw_close(void) {}
static int rtw_get_display_mode(int modenum, int *w, int *h) { if (modenum) return 0; *w = s_w; *h = s_h; return 1; }
static int rtw_get_current_display_mode(int *w, int *h) { *w = s_w; *h = s_h; return 1; }
static int rtw_get_num_display_modes(void) { return 1; }
static int32_t rtw_get_fullscreen_state(void) { return 0; }
static void rtw_set_fullscreen_changed_callback(void (*cb)(bool)) { (void)cb; }
static void rtw_set_fullscreen(bool e) { (void)e; }
static void rtw_set_fullscreen_exclusive(bool e) { (void)e; }
static void rtw_set_fullscreen_flag(int32_t m) { (void)m; }
static int32_t rtw_get_fullscreen_flag_mode(void) { return 0; }
static int32_t rtw_get_maximized_state(void) { return 0; }
static void rtw_set_maximize(bool e) { (void)e; }
static void rtw_get_refresh_rate(uint32_t *r) { *r = 60; }
static void rtw_set_cursor_visibility(bool v) { (void)v; }
static void rtw_set_closest_resolution(int32_t w, int32_t h, bool c) { (void)w; (void)h; (void)c; }
static void rtw_set_dimensions(uint32_t w, uint32_t h, int32_t x, int32_t y) { (void)w; (void)h; (void)x; (void)y; }
static void rtw_get_dimensions(uint32_t *w, uint32_t *h, int32_t *x, int32_t *y) {
    if (w) *w = (uint32_t)s_w;
    if (h) *h = (uint32_t)s_h;
    if (x) *x = 0;
    if (y) *y = 0;
}
static void rtw_get_centered_positions(int32_t w, int32_t h, int32_t *x, int32_t *y) { (void)w; (void)h; *x = 0; *y = 0; }
static void rtw_handle_events(void) {
    geRtwPump();
    int w, h;
    if (geRtwTakeResize(&w, &h) && (w != s_w || h != s_h)) {
        s_w = w; s_h = h;
        gertw_canvas_size(w, h);
    }
}
static bool rtw_start_frame(void) { return true; }
static void rtw_swap_buffers_begin(void) {
    if (gfx_pre_swap_hook) gfx_pre_swap_hook();
    // the composited frame is still in the drawing buffer (committed when this task ends)
    if (geRtwTakeShotRequest() && gfx_opengl_dump_bound_fbo((uint32_t)s_w, (uint32_t)s_h, "/rtwshot.ppm")) {
        geRtwShotWritten("/rtwshot.ppm");
    }
}
static void rtw_swap_buffers_end(void) {}
static double rtw_get_time(void) { return emscripten_get_now() / 1000.0; }
static int32_t rtw_get_target_fps(void) { return s_target_fps; }
static void rtw_set_target_fps(int fps) { s_target_fps = fps; }   // pacing is the browser frame + webkernel retraces
static bool rtw_can_disable_vsync(void) { return false; }
static void *rtw_get_window_handle(void) { return NULL; }
static void rtw_set_window_title(const char *t) { (void)t; }
static int rtw_get_swap_interval(void) { return 1; }
static bool rtw_set_swap_interval(int i) { (void)i; return true; }

extern "C" void gfx_sdl_make_context_current(void) {}   // one context, always current in this worker
extern "C" void gfx_sdl_park_for_exit(void) {}
extern "C" void gfx_sdl_release_context(void) {}
extern "C" void gfx_sdl_update_cached_size(void) {}

extern "C" struct GfxWindowManagerAPI gfx_sdl = {
    rtw_init,
    rtw_close,
    rtw_get_display_mode,
    rtw_get_current_display_mode,
    rtw_get_num_display_modes,
    rtw_get_fullscreen_state,
    rtw_set_fullscreen_changed_callback,
    rtw_set_fullscreen,
    rtw_set_fullscreen_exclusive,
    rtw_set_fullscreen_flag,
    rtw_get_fullscreen_flag_mode,
    rtw_get_maximized_state,
    rtw_set_maximize,
    rtw_get_refresh_rate,
    rtw_set_cursor_visibility,
    rtw_set_closest_resolution,
    rtw_set_dimensions,
    rtw_get_dimensions,
    rtw_get_centered_positions,
    rtw_handle_events,
    rtw_start_frame,
    rtw_swap_buffers_begin,
    rtw_swap_buffers_end,
    rtw_get_time,
    rtw_get_target_fps,
    rtw_set_target_fps,
    rtw_can_disable_vsync,
    rtw_get_window_handle,
    rtw_set_window_title,
    rtw_get_swap_interval,
    rtw_set_swap_interval,
};
