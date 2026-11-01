/*
 * webtouch.c — on-screen touch controls (GE_WEB; page side: port/web/touch.js).
 *
 * The page draws an N64 pad over / under the picture on phones and tablets (stick, A, B, Z, R, L, the four
 * C buttons, Start) and hands its state here on every change: ge_web_touch(on, buttons, stick_x, stick_y),
 * buttons in the N64 OSContPad bit layout, the stick in -80..80 (the port's STICK_MAX).
 *
 * input.c merges it into controller 0 at the end of the pad computation (geWebTouchMerge), exactly where a
 * keyboard key or a gamepad button lands: the touch buttons are OR'd in, a moved touch stick replaces the
 * stick. So it travels every path controller 0 travels: the offline read (libultra.c contSnapshotFromKeyboard),
 * the online lockstep sample (webnet.c geNetRetraceStart -> inputComputePad(0)) and the recorder.
 *
 * While the layer is on (on = 1) and the port's Modern controls force a control style (offline), seat 0 gets
 * the game's default 1.1 HONEY roles (stick = walk + turn, C = look / strafe) instead of the 1.2 SOLITARE roles
 * the port forces for a keyboard + mouse (stick = look): on a touch screen the stick has to walk. Online (OG
 * controls) the game's own style stands, which is 1.1 unless the player changed it in the watch.
 * Off (layer hidden, Off setting, or no touch screen): ge_web_touch(0, 0, 0, 0) and nothing changes.
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>

int geWebTouchOn = 0;          /* the touch layer is shown (input.c stickSolitare) */
static unsigned s_buttons;     /* N64 bits held on the touch layer */
static int s_sx, s_sy;         /* touch stick, -80..80 */

EMSCRIPTEN_KEEPALIVE void ge_web_touch(int on, int buttons, int sx, int sy)
{
    geWebTouchOn = on ? 1 : 0;
    if (!geWebTouchOn) buttons = sx = sy = 0;
    s_buttons = (unsigned)buttons & 0xffffu;
    s_sx = sx < -80 ? -80 : sx > 80 ? 80 : sx;
    s_sy = sy < -80 ? -80 : sy > 80 ? 80 : sy;
}

/* input.c inputComputePadSlot, controller 0 only, after keyboard / gamepad / port adapters */
void geWebTouchMerge(unsigned *button, int *sx, int *sy)
{
    if (!geWebTouchOn) return;
    *button |= s_buttons;
    if (s_sx || s_sy) { *sx = s_sx; *sy = s_sy; }
}
