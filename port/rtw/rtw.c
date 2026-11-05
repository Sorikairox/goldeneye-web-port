/*
 * rtw.c — the retrowser.app v1 module interface of the web build (GE_RTW; docs/web/RTW.md).
 *
 * retrowser.app runs the game in a dedicated Web Worker inside a sandboxed iframe (spec: ~/Code/retrowser
 * spec/retrowser-v1.md): no DOM, no network, no AudioContext. The host hands over the ROM, the native save
 * and the input, and pulls audio, saves and log lines back through the rtw_* exports below. The Emscripten
 * glue is a MODULARIZE factory (createGoldenEye) built with -sENVIRONMENT=worker; port/rtw/rtw_lib.js holds
 * the JS side (env.rtw_* imports wired to Module.rtw, the WebGL2 context on the host's OffscreenCanvas, and
 * the start-up: ROM checks, template fill, sidecar conversion with its IndexedDB cache, main()).
 *
 * In this build the page-facing files of the web build are replaced (CMake GE_RTW): webaudio.c (AudioWorklet)
 * -> the pull ring here; webpage.c (beforeunload, visibility) -> rtw_pause; websave.c (IndexedDB via the page)
 * -> the native save image here; port/fast3d/gfx_sdl2.cpp (SDL window) -> port/rtw/gfx_rtw.cpp. SDL2 stays
 * linked for its plain-C parts (atomics, timers, keyboard / mouse state); nothing initialises its video,
 * audio or joystick drivers, which need a document. The few SDL calls that would reach them are wrapped
 * (-Wl,--wrap, CMake GE_RTW) at the end of this file.
 *
 * Online play (retrowser standard 1.2, spec §4.12, manifest "net": { "players": 4, "model": "lockstep" }): the
 * host owns the room, the WebRTC mesh and the lockstep session boot (every peer from power-on with the room host's
 * options and save). The game side stays port/src/web/webnet.c (input ring, frame barrier, input delay, state
 * hashes); the packet logic of the web page's net.js lives in the worker glue port/rtw/rtw_net.js (--pre-js),
 * which the rtw_net_* exports below feed, and which sends through the env.rtw_net_send / rtw_net_stat imports.
 *
 * Off (the normal web build) none of this is compiled.
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <SDL.h>
#include "realzlib.h"   /* host zlib by absolute path (CMake-generated): PNG screenshots */

#include "platform.h"
#include "system.h"
#include "../src/web/webkernel.h"

/* ---- host imports (env.rtw_*, port/rtw/rtw_lib.js -> Module.rtw) --------------------------------------- */
extern int rtw_log(const char *ptr, int len);
extern int rtw_build_id(void *ptr);
extern int rtw_rumble(int port, int on);
extern int rtw_export_file(const char *name, const char *mime, const void *ptr, int len);
extern int rtw_pointer_lock(int on);
extern int rtw_net_send(int peer, int channel, const void *ptr, int len);   /* [1.2] a packet to a session peer */
extern int rtw_net_stat(int peer, int kind);                                /* [1.2] round trips in ms */
/* JS helpers (rtw_lib.js; not host imports, so not named rtw_*) */
extern void gertw_boot(void *rom, int len);
extern void gertw_net_session(int players, int me);                     /* rtw_net.js rtwNet.begin */
extern void gertw_net_peer(int peer, int state);                        /* rtwNet.peerState */
extern void gertw_net_recv(int peer, int channel, uint32_t ptr, int len);   /* rtwNet.recv */

/* ---- port functions used here (declared locally: the port headers pull game headers) ------------------ */
extern int configSetValue(const char *key, double v);
extern void videoRequestLiveConfigForKey(const char *key);
extern void portAllUnlockedApply(void);
extern void audioSetPaused(int paused);
extern void inputNotifyClick(void);
extern int ge_web_eeprom_load(uint32_t addr, uint32_t len);
extern uint32_t ge_web_eeprom_image(void);
extern void ge_web_touch(int on, int buttons, int sx, int sy);
extern void fileGenerateCRC(uint8_t *from, uint8_t *to, void *saveData);   /* src/game/crc.c: writes chksum1/2 */
extern int geNetSessionOn;                    /* port/src/web/webnet.c: a lockstep session runs */
extern void geNetApplySessionConfig(void);    /* webnet.c: the option values every peer of a session must share */

static void rtwLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rtwLog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    rtw_log(buf, n);
}

static int s_started;      /* main() has run */
static int s_pausePending; /* rtw_pause(1) before main() */
static int s_bootReady;    /* the template is filled and the option keys are valid (ge_rtw_before_main) */
static void rtwConfigReplay(void);

/* ---- start-up ------------------------------------------------------------------------------------------ */
static uint8_t *s_rom;
static int s_romLen;

/* §4.3 step 2: a buffer for the ROM (big-endian .z64, the host converts .v64/.n64). The JS start-up copies it
 * into the Emscripten FS where romdata.c loads it, fills the template binary and converts the sidecars from
 * it, then frees it (ge_rtw_rom_free). */
EMSCRIPTEN_KEEPALIVE void *rtw_rom_buffer(int size)
{
    if (size <= 0 || size > 64 * 1024 * 1024) return NULL;
    free(s_rom);
    s_rom = (uint8_t *)malloc((size_t)size);
    s_romLen = s_rom ? size : 0;
    return s_rom;
}
EMSCRIPTEN_KEEPALIVE void ge_rtw_rom_free(void) { free(s_rom); s_rom = NULL; }

/* ---- saves: the cart's native 4 Kbit EEPROM image --------------------------------------------------- */
/* GoldenEye's cart has a 4 Kbit EEPROM: 64 blocks of 8 bytes = 512 bytes (the game's libultra osEepromProbe
 * only reports the 4K type and osEepromWrite refuses blocks above EEPROM_MAXBLOCKS = 64; file2.c uses block 0
 * for the 32-byte smallSave and blocks 4..63 for the five 96-byte save_data slots, exactly 512 bytes). The PC
 * port's image is 2048 bytes (16 Kbit, the tail unused) with the checksum words and save_data.options (u16 @12,
 * inside the CRC range) in little-endian host order (D492); the N64 writes the same structs big-endian.
 * Per checksummed region (block 0 + five slots):
 *   native -> port: eepimport.c's D514 rules: a region valid only in native order is converted and re-stamped;
 *   port -> native: a port-valid region gets options swapped and both checksums re-stamped with the game's
 *   own CRC over the native bytes, big-endian; any other region is copied as it is (invalid on both sides:
 *   the game wipes it, as the N64 does). */
#define RTW_EEP_NATIVE 512
#define RTW_EEP_PORT   2048
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void eepCrc(uint8_t *r, int len, uint32_t *c1, uint32_t *c2)
{
    int32_t calc[24] = {0};   /* a save_data: only chksum1 / chksum2 are written */
    fileGenerateCRC(r + 8, r + len, calc);
    *c1 = (uint32_t)calc[0];
    *c2 = (uint32_t)calc[1];
}
static int eepValid(uint8_t *r, int len, int bigEndian)
{
    uint32_t c1, c2;
    eepCrc(r, len, &c1, &c2);
    return bigEndian ? (be32(r) == c1 && be32(r + 4) == c2) : (le32(r) == c1 && le32(r + 4) == c2);
}
static void put32(uint8_t *p, uint32_t v, int bigEndian)
{
    for (int i = 0; i < 4; i++) p[bigEndian ? i : 3 - i] = (uint8_t)(v >> (24 - 8 * i));
}
#define EEP_REGION_OFF(i) ((i) == 0 ? 0 : 32 + ((i) - 1) * 96)
#define EEP_REGION_LEN(i) ((i) == 0 ? 32 : 96)
static void eepConvert(uint8_t *img, int toBigEndian)
{
    for (int i = 0; i < 6; i++) {
        uint8_t *r = img + EEP_REGION_OFF(i);
        const int len = EEP_REGION_LEN(i);
        if (eepValid(r, len, toBigEndian)) continue;    /* already in the target order */
        if (!eepValid(r, len, !toBigEndian)) continue;  /* invalid in both: left as it is */
        if (len == 96) { uint8_t t = r[12]; r[12] = r[13]; r[13] = t; }
        uint32_t c1, c2;
        eepCrc(r, len, &c1, &c2);
        put32(r, c1, toBigEndian);
        put32(r + 4, c2, toBigEndian);
    }
}
static uint8_t s_native[RTW_EEP_NATIVE];
static uint8_t s_portImg[RTW_EEP_PORT];
static int s_haveSave, s_saveGen;

EMSCRIPTEN_KEEPALIVE void *rtw_save_ptr(void) { return s_native; }
EMSCRIPTEN_KEEPALIVE int rtw_save_size(void) { return RTW_EEP_NATIVE; }
EMSCRIPTEN_KEEPALIVE int rtw_save_generation(void) { return s_saveGen; }

/* §4.3 step 3: the host copied the stored image to rtw_save_ptr(). Taken over before main() (the game reads
 * its EEPROM once at boot). A 2048-byte emulator file arrives cut to its first 512 bytes, which is all of it. */
EMSCRIPTEN_KEEPALIVE int rtw_save_load(void *ptr, int size)
{
    if (size < 0) return -1;
    if (size > RTW_EEP_NATIVE) size = RTW_EEP_NATIVE;
    if (ptr != (void *)s_native) memmove(s_native, ptr, (size_t)size);
    memset(s_native + size, 0, RTW_EEP_NATIVE - (size_t)size);
    s_haveSave = size > 0;
    if (s_started) rtwLog("rtw: save handed over after start; used at the next start");
    return 0;
}

/* websave.c replacement. what: 1 = the EEPROM image was stored (libultra.c geEepromStore), 2 = ge007.ini
 * (config.c configSave; the config is not kept: options come from rtw_option). */
int webSaveSandboxed(void) { return 0; }
void webSaveWritten(int what)
{
    if (what != 1) return;
    uint8_t n[RTW_EEP_NATIVE];
    memcpy(n, (const void *)(uintptr_t)ge_web_eeprom_image(), sizeof(n));
    eepConvert(n, 1);
    if (memcmp(n, s_native, sizeof(n)) != 0) {
        memcpy(s_native, n, sizeof(n));
        s_saveGen++;
    }
}

/* ---- input ---------------------------------------------------------------------------------------------- */
/* Host pads: N64 OSContPad bits (active-high), stick -127..127 (up positive), extra buttons in bits 16+.
 * Pad 0 (keyboard + touch + first gamepad on the host) goes into controller 0 through the page's touch-pad
 * seam (webtouch.c ge_web_touch -> input.c geWebTouchMerge), so it travels every path controller 0 travels
 * and the port's mouse look stays available; with it on, seat 0 uses the game's 1.1 roles (stick walks).
 * Pads 1-3 are plain N64 pads, present while the host keeps sending them (libultra.c geRtwPad). */
#define RTW_EXTRA_RELOAD  (1u << 16)   /* port action Reload (key R) */
#define RTW_EXTRA_CROUCH  (1u << 17)   /* port action Crouch (key Left Ctrl) */
#define RTW_EXTRA_SHOT    (1u << 18)   /* screenshot -> env.rtw_export_file (PNG) */
#define RTW_STICK_MAX 80               /* the port's / a real pad's stick range */
static unsigned s_extra;
static int s_shotReq;
int geRtwTakeShotRequest(void) { int r = s_shotReq; s_shotReq = 0; return r; }   /* gfx_rtw.cpp, before the commit */
static unsigned s_padB[4];
static int s_padX[4], s_padY[4];
static double s_padSeen[4];

static int stickScale(int v)
{
    if (v < -127) v = -127;
    if (v > 127) v = 127;
    return v * RTW_STICK_MAX / 127;
}

EMSCRIPTEN_KEEPALIVE int rtw_input(int port, int buttons, int sx, int sy)
{
    if (port < 0 || port > 3) return 0;
    const unsigned b = (unsigned)buttons;
    const int x = stickScale(sx), y = stickScale(sy);
    if (port == 0) {
        static unsigned last; static int lastX, lastY, logged;
        if ((b != last || x != lastX || y != lastY) && logged < 60) {   /* test hook: the first changes */
            logged++;
            rtwLog("rtw: pad0 buttons %08x stick %d,%d", b, x, y);
        }
        last = b; lastX = x; lastY = y;
        ge_web_touch(1, (int)(b & 0xffffu), x, y);
        const unsigned rise = (b & ~s_extra);
        s_extra = b & 0xffff0000u;
        if ((rise & RTW_EXTRA_SHOT) && s_started) s_shotReq = 1;
    } else {
        s_padB[port] = b & 0xffffu;
        s_padX[port] = x;
        s_padY[port] = y;
        s_padSeen[port] = emscripten_get_now();
    }
    return 0;
}

/* pads 1-3: present while the host sent them in the last 2 s (the libultra wraps below) */
static int geRtwPad(int port, unsigned *b, int *sx, int *sy)
{
    if (port < 1 || port > 3 || !s_padSeen[port] || emscripten_get_now() - s_padSeen[port] > 2000.0) return 0;
    *b = s_padB[port];
    *sx = s_padX[port];
    *sy = s_padY[port];
    return 1;
}

/* Mouse: relative motion while the host has the pointer captured (it captures after rtw_pointer_lock(1) and
 * a click). Fed to the port's mouse look through the wrapped SDL mouse calls below. */
static int s_mdx, s_mdy;
static unsigned s_mbuttons;
static int s_relative;

EMSCRIPTEN_KEEPALIVE int rtw_mouse(int dx, int dy, int buttons)
{
    s_mdx += dx;
    s_mdy += dy;
    s_mbuttons = (unsigned)buttons & 7u;
    if (s_started && buttons) inputNotifyClick();   /* a click with the pointer captured (re)arms the grab */
    return 0;
}

static Uint32 sdlButtons(void)
{
    /* the host sends on change only, and buttons 0 when the capture ends */
    Uint32 m = 0;
    if (s_mbuttons & 1) m |= SDL_BUTTON_LMASK;
    if (s_mbuttons & 2) m |= SDL_BUTTON_RMASK;
    if (s_mbuttons & 4) m |= SDL_BUTTON_MMASK;
    return m;
}

/* ---- rumble (input.c inputRumbleSupported / inputRumble) ------------------------------------------------ */
static int s_rumbleOpt = 1;
static int s_rumbleOn[4];
static int geRtwRumbleSupported(int port)
{
    if (!s_rumbleOpt || port < 0 || port > 3) return 0;
    if (port == 0) return 1;
    return s_padSeen[port] != 0;
}
static void geRtwRumble(int port, int on)
{
    if (port < 0 || port > 3) return;
    on = on && s_rumbleOpt;
    if (s_rumbleOn[port] == on) return;
    s_rumbleOn[port] = on;
    rtw_rumble(port, on);
}

/* ---- video size (port/rtw/gfx_rtw.cpp) --------------------------------------------------------------- */
static int s_cssW = 640, s_cssH = 480, s_dpr = 100;
static int s_resOpt = 1;          /* option 16: 0 = 480 lines, 1 = screen size, 2 = screen x devicePixelRatio */
static int s_resizeDirty = 1;

EMSCRIPTEN_KEEPALIVE int rtw_resize(int w, int h, int dpr)
{
    if (w < 1 || h < 1) return 0;
    s_cssW = w; s_cssH = h; s_dpr = dpr > 0 ? dpr : 100;
    s_resizeDirty = 1;
    return 0;
}

void geRtwBufferSize(int *outW, int *outH)
{
    double w = s_cssW, h = s_cssH;
    if (s_resOpt == 0) { w = w * 480.0 / h; h = 480.0; }
    else if (s_resOpt == 2) { w = w * s_dpr / 100.0; h = h * s_dpr / 100.0; }
    /* bounds: at most 2560 on the long side and 2560x1440 pixels, at least 320x240 */
    double k = 1.0;
    if (w > 2560.0) k = 2560.0 / w;
    if (h * k > 2560.0) k = 2560.0 / h;
    if (w * h * k * k > 2560.0 * 1440.0) k = __builtin_sqrt(2560.0 * 1440.0 / (w * h));
    w *= k; h *= k;
    if (w < 320.0) w = 320.0;
    if (h < 240.0) h = 240.0;
    *outW = (int)(w + 0.5);
    *outH = (int)(h + 0.5);
}

int geRtwTakeResize(int *w, int *h)
{
    if (!s_resizeDirty) return 0;
    s_resizeDirty = 0;
    geRtwBufferSize(w, h);
    return 1;
}

/* ---- audio: pull model (§4.8), replaces webaudio.c ------------------------------------------------------ */
/* audio.c keeps its push model (blocks of 22050 Hz s16 stereo + a queue depth for the game's AI_LEN
 * regulator, D204). Blocks go into a ring the host drains once per display frame; the depth the regulator
 * sees is a virtual DAC that plays at real time (frames queued minus wall time x rate, stopped while
 * paused), which is what the N64's audio interface did. The host does its own rate control. */
#define RTW_AUDIO_RATE 22050
#define RTW_RING 32768                  /* frames, ~1.5 s */
static int16_t s_ring[RTW_RING * 2];
static unsigned s_ringHead, s_ringCount;
static int16_t s_audioOut[RTW_RING * 2];
static int s_audioOpen, s_audioPaused;
static double s_vq, s_vqAt;
static unsigned s_audioDropped;

static void vqUpdate(void)
{
    const double now = emscripten_get_now();
    if (s_vqAt && !s_audioPaused) {
        s_vq -= (now - s_vqAt) * RTW_AUDIO_RATE / 1000.0;
        if (s_vq < 0) s_vq = 0;
    }
    s_vqAt = now;
}

int webAudioOpen(int srcRate)
{
    if (srcRate != RTW_AUDIO_RATE)
        sysLogPrintf(LOG_WARNING, "rtw: audio opened at %d Hz, rtw_audio_rate reports %d", srcRate, RTW_AUDIO_RATE);
    s_audioOpen = 1;
    sysLogPrintf(LOG_INFO, "audio: retrowser pull output, %d Hz s16 stereo", srcRate);
    return srcRate > 0 ? srcRate : RTW_AUDIO_RATE;
}

void webAudioQueue(const void *buf, uint32_t len)
{
    if (!s_audioOpen || !buf || len < 4) return;
    const unsigned frames = len / 4;
    const int16_t *src = (const int16_t *)buf;
    vqUpdate();
    s_vq += frames;
    for (unsigned i = 0; i < frames; i++) {
        unsigned at = (s_ringHead + s_ringCount) % RTW_RING;
        if (s_ringCount == RTW_RING) {   /* host not draining (paused page, slow start): drop the oldest */
            s_ringHead = (s_ringHead + 1) % RTW_RING;
            s_ringCount--;
            s_audioDropped++;
        }
        s_ring[at * 2] = src[i * 2];
        s_ring[at * 2 + 1] = src[i * 2 + 1];
        s_ringCount++;
    }
}

uint32_t webAudioQueuedBytes(void)
{
    if (!s_audioOpen) return 0;
    vqUpdate();
    return (uint32_t)s_vq * 4u;
}

void webAudioPause(int paused)
{
    vqUpdate();
    s_audioPaused = paused ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int rtw_audio_rate(void) { return RTW_AUDIO_RATE; }
EMSCRIPTEN_KEEPALIVE void *rtw_audio_buffer(void) { return s_audioOut; }
EMSCRIPTEN_KEEPALIVE int rtw_audio_drain(void)
{
    const unsigned n = s_ringCount;
    for (unsigned i = 0; i < n; i++) {
        const unsigned at = (s_ringHead + i) % RTW_RING;
        s_audioOut[i * 2] = s_ring[at * 2];
        s_audioOut[i * 2 + 1] = s_ring[at * 2 + 1];
    }
    s_ringHead = (s_ringHead + n) % RTW_RING;
    s_ringCount = 0;
    return (int)n;
}

/* ---- pause (§4.4): replaces webpage.c's hidden-tab pause --------------------------------------------- */
EMSCRIPTEN_KEEPALIVE int rtw_pause(int on)
{
    if (!s_started) { s_pausePending = on ? 1 : 0; return 0; }
    webkernelSetPaused(on ? 1 : 0);   /* stops the main loop and the console clock */
    audioSetPaused(on ? 1 : 0);
    return 0;
}

/* main.c calls this after videoInit; the web build's version hooks beforeunload and visibility, which do
 * not exist here (the host pauses through rtw_pause). */
void webPageInit(void) {}

/* ---- options (§4.9): manifest ids -> the port's ini keys (docs/web/RTW.md) ------------------------------ */
typedef struct { int id; const char *key; int kind; } RtwOpt;   /* kind 0: value as is, 1: bool inverted */
enum { OPT_RES = 16, OPT_RUMBLE = 10, OPT_ONLINE = 17, OPT_NETDELAY = 18 };   /* 17, 18: online play (rtw_net.js) */
static const RtwOpt kOpts[] = {
    { 1,  "Video.AspectMode", 0 },         /* 0 fill (Hor+ widescreen), 1 original 4:3, 2 16:9, 3 21:9 */
    { 2,  "Game.SkipIntro", 0 },
    { 3,  "Input.MouseSensitivity", 0 },
    { 4,  "Input.MouseInvertY", 0 },
    { 5,  "Video.FovScale", 0 },
    { 6,  "Video.DrawDistance", 0 },
    { 7,  "Video.TextureFilter", 0 },
    { 8,  "Game.AllUnlocked", 0 },
    { 9,  "Input.ControlScheme", 0 },
    { 11, "Game.ScreenShakeIntensity", 2 },   /* percent -> factor */
    { 12, "Game.NoHitFlash", 0 },
    { 13, "Game.HudScale", 0 },
    { 14, "Video.CrosshairHide", 0 },
    { 15, "Video.DisplayFPS", 0 },
};
#define RTW_MAX_OPT 32
static int s_optSet[RTW_MAX_OPT], s_optVal[RTW_MAX_OPT];

/* Online play (standard 1.2): every peer boots with the room host's option values (the host dictates them), so a
 * session starts from the same configuration everywhere. On top, webnet.c's geNetApplySessionConfig pins what the
 * web build's sessions pin (OG controls, no all-unlocked, 4:3 projection, no machine-dependent classification):
 * options 8 (All levels unlocked) and 9 (Control style) are not applied in a session. After the start the options
 * stay as they are for the whole session (the host's menu may still send rtw_option); only the ones that change
 * the picture alone (1 aspect of the output rectangle, 7 texture filter, 15 FPS counter, 16 resolution) follow it. */
static int rtwSessionFixed(int id) { return geNetSessionOn && (id == 8 || id == 9); }
static int rtwSessionLocal(int id) { return id == 1 || id == 7 || id == 15 || id == 16; }

static void optApply(int id, int value)
{
    if (id == OPT_ONLINE || id == OPT_NETDELAY) return;   /* read by rtw_net.js at rtw_net_session (ge_rtw_option) */
    if (rtwSessionFixed(id)) return;
    if (id == OPT_RES) { s_resOpt = value < 0 ? 0 : value > 2 ? 2 : value; s_resizeDirty = 1; return; }
    if (id == OPT_RUMBLE) {
        s_rumbleOpt = value != 0;
        configSetValue("Input.RumbleScale", s_rumbleOpt ? 0.5 : 0.0);
        if (!s_rumbleOpt) for (int p = 0; p < 4; p++) geRtwRumble(p, 0);
        return;
    }
    for (size_t i = 0; i < sizeof(kOpts) / sizeof(kOpts[0]); i++) {
        if (kOpts[i].id != id) continue;
        double v = kOpts[i].kind == 2 ? value / 100.0 : kOpts[i].kind == 1 ? !value : value;
        if (!configSetValue(kOpts[i].key, v)) sysLogPrintf(LOG_WARNING, "rtw: option %d: %s not registered", id, kOpts[i].key);
        if (s_started) {
            videoRequestLiveConfigForKey(kOpts[i].key);
            if (id == 8) portAllUnlockedApply();
        }
        return;
    }
    sysLogPrintf(LOG_WARNING, "rtw: unknown option %d", id);
}

EMSCRIPTEN_KEEPALIVE int rtw_option(int id, int value)
{
    if (s_started && geNetSessionOn && !rtwSessionLocal(id)) {
        rtwLog("rtw: option %d = %d ignored: an online session keeps the room host's options", id, value);
        return 0;
    }
    if (id >= 0 && id < RTW_MAX_OPT) { s_optSet[id] = 1; s_optVal[id] = value; }
    if (s_bootReady) optApply(id, value);   /* before that: applied by ge_rtw_before_main */
    return 0;
}

/* the value the host gave option id (rtw_net.js reads 17 / 18 at rtw_net_session), or def when it sent none */
EMSCRIPTEN_KEEPALIVE int ge_rtw_option(int id, int def)
{
    return id >= 0 && id < RTW_MAX_OPT && s_optSet[id] ? s_optVal[id] : def;
}

/* ---- start (§4.3 step 6) ----------------------------------------------------------------------------- */
/* Returns at once; rtw_lib.js gertw_boot runs the asynchronous part (ROM check, template fill, sidecars from
 * the sandbox origin's IndexedDB or converted in wasm) and then main(), which installs the browser main
 * loop (webkernel.c) and returns: loop kind "mainloop". */
EMSCRIPTEN_KEEPALIVE int rtw_start(void)
{
    if (!s_rom || !s_romLen) { rtwLog("rtw: rtw_start without a ROM"); return -1; }
    gertw_boot(s_rom, s_romLen);
    return 0;
}

/* gertw_boot: right before main() */
EMSCRIPTEN_KEEPALIVE void ge_rtw_before_main(void)
{
    rtwConfigReplay();   /* the option registrations the constructors made, now with filled key strings */
    s_bootReady = 1;
    /* logged only now: string literals that also occur in the ROM are zeroed in the template until the fill */
    uint8_t id[32];
    rtw_build_id(id);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", id[i]);
    rtwLog("rtw: GoldenEye 007 (EU) retrowser build, build id %.16s", hex);
    if (s_haveSave) {
        memset(s_portImg, 0, sizeof(s_portImg));
        memcpy(s_portImg, s_native, RTW_EEP_NATIVE);
        eepConvert(s_portImg, 0);
        ge_web_eeprom_load((uint32_t)(uintptr_t)s_portImg, RTW_EEP_PORT);
    }
    for (int id = 0; id < RTW_MAX_OPT; id++) if (s_optSet[id]) optApply(id, s_optVal[id]);
    ge_web_touch(1, 0, 0, 0);   /* host pad 0 = controller 0 with the N64 roles (see rtw_input) */
}

/* gertw_boot: right after main() returned (the main loop is installed) */
EMSCRIPTEN_KEEPALIVE void ge_rtw_after_main(void)
{
    s_started = 1;
    /* the boot's video / input defaults ran (D546 migrations, low-end check): the host's options win */
    for (int id = 0; id < RTW_MAX_OPT; id++) if (s_optSet[id]) optApply(id, s_optVal[id]);
    for (int id = 0; id < RTW_MAX_OPT; id++) if (s_optSet[id] && id != OPT_RES && id != OPT_RUMBLE && !rtwSessionFixed(id)) {
        for (size_t i = 0; i < sizeof(kOpts) / sizeof(kOpts[0]); i++)
            if (kOpts[i].id == id) videoRequestLiveConfigForKey(kOpts[i].key);
    }
    if (geNetSessionOn) geNetApplySessionConfig();   /* online: the session's fixed values stand after the boot's defaults */
    /* Click-to-lock is the host's job (it captures on a click after rtw_pointer_lock(1)): arm the port's grab
     * so it asks for the pointer whenever a stage is running. */
    inputNotifyClick();
    if (s_pausePending) rtw_pause(1);
    rtwLog("rtw: game started (save %s)", s_haveSave ? "from the host" : "new");
}

/* ---- screenshots: an extra button -> PNG -> env.rtw_export_file ----------------------------------------- */
static void pngChunk(uint8_t **p, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t *o = *p;
    o[0] = (uint8_t)(len >> 24); o[1] = (uint8_t)(len >> 16); o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)len;
    memcpy(o + 4, type, 4);
    if (len) memcpy(o + 8, data, len);
    uint32_t c = (uint32_t)crc32(0L, o + 4, len + 4);
    o[8 + len] = (uint8_t)(c >> 24); o[9 + len] = (uint8_t)(c >> 16); o[10 + len] = (uint8_t)(c >> 8); o[11 + len] = (uint8_t)c;
    *p = o + 12 + len;
}

/* gfx_rtw.cpp dumped the frame (gfx_opengl_dump_bound_fbo: a P6 PPM, top row first) into the MEMFS before
 * the frame was committed: encode it as an RGB PNG. */
void geRtwShotWritten(const char *ppmPath)
{
    static int shot;
    FILE *f = fopen(ppmPath, "rb");
    if (!f) return;
    unsigned w = 0, h = 0, mx = 0;
    if (fscanf(f, "P6 %u %u %u", &w, &h, &mx) != 3 || mx != 255 || !w || !h || w > 8192 || h > 8192) { fclose(f); return; }
    fgetc(f);
    const size_t stride = (size_t)w * 3;
    uint8_t *raw = (uint8_t *)malloc((stride + 1) * h);
    uint8_t *row = (uint8_t *)malloc(stride);
    if (!raw || !row) { free(raw); free(row); fclose(f); return; }
    for (unsigned y = 0; y < h; y++) {
        raw[y * (stride + 1)] = 0;   /* filter: none */
        if (fread(raw + y * (stride + 1) + 1, 1, stride, f) != stride) { free(raw); free(row); fclose(f); return; }
    }
    fclose(f);
    remove(ppmPath);
    uLongf zlen = compressBound((uLong)((stride + 1) * h));
    uint8_t *png = (uint8_t *)malloc(zlen + 128);
    uint8_t *z = (uint8_t *)malloc(zlen);
    if (png && z && compress2(z, &zlen, raw, (uLong)((stride + 1) * h), 6) == Z_OK) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                             (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                             8, 2, 0, 0, 0 };
        uint8_t *p = png;
        memcpy(p, sig, 8); p += 8;
        pngChunk(&p, "IHDR", ihdr, 13);
        pngChunk(&p, "IDAT", z, (uint32_t)zlen);
        pngChunk(&p, "IEND", NULL, 0);
        char name[64];
        snprintf(name, sizeof(name), "goldeneye-shot-%03d.png", shot++);
        rtw_export_file(name, "image/png", png, (int)(p - png));
        sysLogPrintf(LOG_INFO, "rtw: screenshot %ux%u exported as %s", w, h, name);
    }
    free(z); free(png); free(raw); free(row);
}

/* ---- SDL calls that would need SDL's Emscripten video / audio / joystick drivers (-Wl,--wrap) ----------- */
/* GL entry points come from Emscripten's WebGL2 context (gfx_rtw.cpp made it current); the profile is
 * reported as ES like SDL's Emscripten context, so gfx_opengl.cpp takes the same paths as the web build. */
void *__wrap_SDL_GL_GetProcAddress(const char *proc)
{
    extern void *emscripten_webgl_get_proc_address(const char *name);
    return emscripten_webgl_get_proc_address(proc);
}
int __wrap_SDL_GL_GetAttribute(SDL_GLattr attr, int *value)
{
    if (!value) return -1;
    switch (attr) {
    case SDL_GL_CONTEXT_PROFILE_MASK: *value = SDL_GL_CONTEXT_PROFILE_ES; return 0;
    case SDL_GL_CONTEXT_MAJOR_VERSION: *value = 3; return 0;
    case SDL_GL_CONTEXT_MINOR_VERSION: *value = 0; return 0;
    case SDL_GL_DEPTH_SIZE: *value = 24; return 0;
    case SDL_GL_STENCIL_SIZE: *value = 8; return 0;
    case SDL_GL_DOUBLEBUFFER: *value = 1; return 0;
    default: *value = 0; return 0;
    }
}
/* input.c asks for the game-controller subsystem (pads come from the host instead) and audio.c for the audio
 * subsystem (output is the ring above, through audio.c's web routing). */
int __real_SDL_InitSubSystem(Uint32 flags);
int __wrap_SDL_InitSubSystem(Uint32 flags)
{
    if (flags & (SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_SENSOR)) {
        SDL_SetError("retrowser: pads come from the host");
        return -1;
    }
    if (flags & SDL_INIT_AUDIO) return 0;
    if (flags & SDL_INIT_VIDEO) { SDL_SetError("retrowser: no SDL video in a worker"); return -1; }
    return __real_SDL_InitSubSystem(flags);
}
/* Mouse look: relative mode = the port wants the pointer -> env.rtw_pointer_lock. */
int __wrap_SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    const int on = enabled ? 1 : 0;
    if (on != s_relative) {
        s_relative = on;
        s_mdx = s_mdy = 0;
        rtw_pointer_lock(on);
    }
    return 0;
}
SDL_bool __wrap_SDL_GetRelativeMouseMode(void) { return s_relative ? SDL_TRUE : SDL_FALSE; }
Uint32 __wrap_SDL_GetRelativeMouseState(int *x, int *y)
{
    if (x) *x = s_mdx;
    if (y) *y = s_mdy;
    s_mdx = s_mdy = 0;
    return sdlButtons();
}
/* No absolute pointer in the sandbox (motion only while captured): an untouched mouse at 0,0, as SDL reports
 * before the first motion event. */
Uint32 __wrap_SDL_GetMouseState(int *x, int *y)
{
    if (x) *x = 0;
    if (y) *y = 0;
    return sdlButtons();
}
/* Keyboard: only the extra buttons, as the keys the port binds to those actions by default. */
const Uint8 *__wrap_SDL_GetKeyboardState(int *numkeys)
{
    static Uint8 keys[SDL_NUM_SCANCODES];
    memset(keys, 0, sizeof(keys));
    if (s_started) {
        if (s_extra & RTW_EXTRA_RELOAD) keys[SDL_SCANCODE_R] = 1;
        if (s_extra & RTW_EXTRA_CROUCH) keys[SDL_SCANCODE_LCTRL] = 1;
    }
    if (numkeys) *numkeys = SDL_NUM_SCANCODES;
    return keys;
}

/* ---- port functions reached through -Wl,--wrap (CMake GE_RTW): the shared port sources stay unchanged ---- */
/* Rumble (input.c; called from libultra.c's Rumble Pak shims): every present host pad has a Rumble Pak. */
int __wrap_inputRumbleSupported(int idx) { return geRtwRumbleSupported(idx); }
void __wrap_inputRumble(int idx, float strength, float time) { (void)time; geRtwRumble(idx, strength > 0.f); }

/* Controllers 1-3 (libultra.c, called by the game's joy.c): the host's further gamepads as plain N64 pads.
 * Mirrors of OSContPad / OSContStatus (include/PR/os.h). */
typedef struct { uint16_t button; int8_t stick_x, stick_y; uint8_t err; } RtwContPad;
typedef struct { uint16_t type; uint8_t status; uint8_t err; } RtwContStatus;
#define RTW_CONT_TYPE_NORMAL 0x0005
#define RTW_CONT_CARD_ON 0x01
#define RTW_MAXCONTROLLERS 4
void __real_osContGetReadData(RtwContPad *pad);
void __wrap_osContGetReadData(RtwContPad *pad)
{
    __real_osContGetReadData(pad);
    if (geNetSessionOn) return;   /* online: every controller comes from the session's input ring */
    for (int i = 1; i < RTW_MAXCONTROLLERS; i++) {
        unsigned b; int sx, sy;
        if (!pad || !geRtwPad(i, &b, &sx, &sy)) continue;
        pad[i].button = (uint16_t)b;
        pad[i].stick_x = (int8_t)sx;
        pad[i].stick_y = (int8_t)sy;
        pad[i].err = 0;
    }
}
static void rtwStatus(RtwContStatus *st)
{
    if (geNetSessionOn) return;   /* online: presence = the session's mask (libultra.c) */
    for (int i = 1; i < RTW_MAXCONTROLLERS; i++) {
        unsigned b; int sx, sy;
        if (!st || !geRtwPad(i, &b, &sx, &sy)) continue;
        st[i].type = RTW_CONT_TYPE_NORMAL;
        st[i].status = geRtwRumbleSupported(i) ? RTW_CONT_CARD_ON : 0;
        st[i].err = 0;
    }
}
static uint8_t rtwMask(void)
{
    uint8_t m = 0;
    for (int i = 1; i < RTW_MAXCONTROLLERS; i++) { unsigned b; int x, y; if (geRtwPad(i, &b, &x, &y)) m |= (uint8_t)(1 << i); }
    return m;
}
void __real_osContGetQuery(RtwContStatus *status);
void __wrap_osContGetQuery(RtwContStatus *status) { __real_osContGetQuery(status); rtwStatus(status); }
int32_t __real_osContReset(void *mq, RtwContStatus *status);
int32_t __wrap_osContReset(void *mq, RtwContStatus *status) { int32_t r = __real_osContReset(mq, status); rtwStatus(status); return r; }
int32_t __real_osContInit(void *mq, uint8_t *bitpattern, RtwContStatus *data);
int32_t __wrap_osContInit(void *mq, uint8_t *bitpattern, RtwContStatus *data)
{
    int32_t r = __real_osContInit(mq, bitpattern, data);
    if (bitpattern && !geNetSessionOn) *bitpattern |= rtwMask();
    return r;
}

/* Option registrations (config.c). The port registers its ini keys from C constructors, which run when the
 * module is instantiated, before the start-up fills the template (docs/web/ROMDATA.md); configRegister* copy
 * the key, and key literals that also occur in the ROM are still zeroed then (the merged string pool), so
 * they would be registered broken. Until the fill (ge_rtw_before_main) the calls are queued and replayed in
 * order; afterwards they pass straight through. */
void __real_configRegisterInt(const char *key, int *value, int min, int max);
void __real_configRegisterUInt(const char *key, unsigned int *value, unsigned int min, unsigned int max);
void __real_configRegisterFloat(const char *key, float *value, float min, float max);
void __real_configRegisterString(const char *key, char *value, int bufSize);
void __real_configSetOptionMeta(const char *key, const char *label, double step, const char *const *enumNames);
typedef struct { int kind; const char *key; void *value; double a, b; const char *label; const char *const *names; } RtwReg;
static RtwReg s_reg[512];
static int s_nreg;
static int rtwQueue(int kind, const char *key, void *value, double a, double b, const char *label, const char *const *names)
{
    if (s_bootReady) return 0;
    if (s_nreg >= (int)(sizeof(s_reg) / sizeof(s_reg[0]))) return 0;   /* cannot happen (~110 options) */
    s_reg[s_nreg++] = (RtwReg){ kind, key, value, a, b, label, names };
    return 1;
}
void __wrap_configRegisterInt(const char *key, int *value, int min, int max)
{ if (!rtwQueue(0, key, value, min, max, NULL, NULL)) __real_configRegisterInt(key, value, min, max); }
void __wrap_configRegisterUInt(const char *key, unsigned int *value, unsigned int min, unsigned int max)
{ if (!rtwQueue(1, key, value, min, max, NULL, NULL)) __real_configRegisterUInt(key, value, min, max); }
void __wrap_configRegisterFloat(const char *key, float *value, float min, float max)
{ if (!rtwQueue(2, key, value, min, max, NULL, NULL)) __real_configRegisterFloat(key, value, min, max); }
void __wrap_configRegisterString(const char *key, char *value, int bufSize)
{ if (!rtwQueue(3, key, value, bufSize, 0, NULL, NULL)) __real_configRegisterString(key, value, bufSize); }
void __wrap_configSetOptionMeta(const char *key, const char *label, double step, const char *const *enumNames)
{ if (!rtwQueue(4, key, NULL, step, 0, label, enumNames)) __real_configSetOptionMeta(key, label, step, enumNames); }
static void rtwConfigReplay(void)
{
    for (int i = 0; i < s_nreg; i++) {
        const RtwReg *r = &s_reg[i];
        switch (r->kind) {
        case 0: __real_configRegisterInt(r->key, (int *)r->value, (int)r->a, (int)r->b); break;
        case 1: __real_configRegisterUInt(r->key, (unsigned int *)r->value, (unsigned int)r->a, (unsigned int)r->b); break;
        case 2: __real_configRegisterFloat(r->key, (float *)r->value, (float)r->a, (float)r->b); break;
        case 3: __real_configRegisterString(r->key, (char *)r->value, (int)r->a); break;
        case 4: __real_configSetOptionMeta(r->key, r->label, r->a, r->names); break;
        }
    }
    s_nreg = 0;
}

/* ---- online play (standard 1.2, spec §4.12) ---------------------------------------------------------------- */
/* The host's lockstep boot calls rtw_net_session + rtw_net_peer after rtw_option and before rtw_start; the glue
 * (rtw_net.js) picks the input delay and starts webnet.c's session right before main() (gertwBootAsync), so the
 * game is in the session from its first retrace. Packets arrive between the module's own steps: the host asks for a
 * buffer (at most 64 KiB, channel 1's limit), copies the packet in and calls rtw_net_recv; the glue copies it out at
 * once (it never runs the game from inside the call: an Asyncify fiber switch must not unwind through it). Sends
 * go through ge_rtw_net_send from a static buffer (no malloc during a session), so both host imports are real
 * env.rtw_* imports of the wasm. Everything here is port-only state (scripts/web/netregions.py leaves it out of
 * the state hash). */
#define RTW_NET_MAX 65536
static uint8_t s_netRx[RTW_NET_MAX], s_netTx[RTW_NET_MAX];
EMSCRIPTEN_KEEPALIVE int rtw_net_session(int players, int me) { gertw_net_session(players, me); return 0; }
EMSCRIPTEN_KEEPALIVE int rtw_net_peer(int peer, int state) { gertw_net_peer(peer, state); return 0; }
EMSCRIPTEN_KEEPALIVE void *rtw_net_buffer(int size) { return size >= 0 && size <= RTW_NET_MAX ? s_netRx : NULL; }
EMSCRIPTEN_KEEPALIVE int rtw_net_recv(int peer, int channel, int len)
{
    if (len < 0 || len > RTW_NET_MAX) return 0;
    gertw_net_recv(peer, channel, (uint32_t)(uintptr_t)s_netRx, len);
    return 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t ge_rtw_net_txbuf(void) { return (uint32_t)(uintptr_t)s_netTx; }
EMSCRIPTEN_KEEPALIVE int ge_rtw_net_send(int peer, int channel, int len)
{
    if (len < 0 || len > RTW_NET_MAX) return -1;
    return rtw_net_send(peer, channel, s_netTx, len);
}
EMSCRIPTEN_KEEPALIVE int ge_rtw_net_stat(int peer, int kind) { return rtw_net_stat(peer, kind); }
