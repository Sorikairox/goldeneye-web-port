/*
 * webnet.c — online multiplayer, game side (G6, docs/web/ONLINE.md). Adapted from the Conker web port's
 * port/ultra/pc_net.c (same model, same packed input, same ring and stats layout).
 *
 * Model: deterministic input lockstep. Every peer runs the same wasm on its own ROM from power-on; only
 * controller inputs travel (4 bytes per player per 50 Hz retrace). Retrace N runs only when every session
 * channel's input for N is in the ring below (the frame barrier, webkernel.c). The local player's input for
 * N + D is sampled at the start of retrace N (input delay D); frames 1..D are neutral for everyone.
 *
 * Everything here is inert unless the page calls ge_net_session() (or ge_rec_session()) before main().
 * The page side is port/web/net.js (WebRTC mesh, signaling, hash exchange).
 *
 * Determinism (playbook WEB_PORT_ORCHESTRATOR.md 10.2), what each source becomes under geNetDetOn:
 *  1. wall clock: osGetCount / osGetTime / timers read the det clock (geNetDetCount: retrace x one PAL
 *     retrace of counter ticks + 64 ticks per read, capped below half a retrace); boss.c seeds the RNG from
 *     it, so the boot seed is the same on every peer;
 *  2. scheduler budget: fibers run only inside a retrace and until all of them block; a busy-waiting fiber
 *     parks until the next retrace (not the next browser frame) (webkernel.c);
 *  3. audio timing: the AI length the game's audio regulator reads is a virtual queue drained by
 *     1764 bytes per retrace (22050 Hz x 4 bytes / 50), never the browser's ring (audio.c);
 *  4. input: controller reads return the ring's entry for the running retrace; the page never writes pad
 *     state into the game (libultra.c); the port's input adapters that write game state directly (mouse
 *     aim turn, free crouch, gun-sway feed, control-style forcing, gadget key) are off (input.c);
 *  5. controller presence: the session mask, no Rumble Pak (libultra.c);
 *  6. options: the host's ini (written by the page before main) plus the fixed values of
 *     geNetApplySessionConfig; nothing is saved during a session (config.c), the F10 overlay is refused;
 *  7. URL test flags: the page passes only whitelisted env variables in a session (net.js);
 *  8. save data: the session's EEPROM image (blank until G5's host-image seam), never persisted;
 *  9. GPU read-backs: none reach game memory in this port (fast3d draws to GL only);
 * 10. heap / stacks: fiber stacks are static slots (webkernel.c); no page-side malloc during a session;
 * 11. build / ROM: the page refuses peers with another wasm or ROM hash (net.js).
 *
 * State hash (desync check and determinism proof): H = the game's data (the game / libultra / asset
 * objects' .data/.bss and the port files that hold game state, listed by scripts/web/netregions.py from
 * the link map into ge007.netregions, loaded by the page) + the 8 MiB N64 DRAM arena, hashed in 4 KB
 * pages; S = the asyncify buffers of the game fibers (their saved wasm locals; webkernel.c).
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "webkernel.h"
#include "webnet.h"
#include "platform.h"
#include "system.h"

/* port/src/input.c, port/src/config.c (declared here: no game headers in this file) */
void inputUpdate(void);
unsigned inputComputePad(int idx, signed char *stick_x, signed char *stick_y);
int configSetValue(const char *key, double v);
extern int geNetPurePad;   /* input.c */

int geNetDetOn;
int geNetSessionOn;

/* ---------------------------------------------------------------- ring, stats */
#define NET_RING 256              /* frames; the page never runs more than D + 64 frames ahead of the slowest peer */
#define NET_HASHES 64             /* the last 64 desync-check hashes */
static struct {
    uint32_t input[NET_RING][4];  /* packed input of frame f in slot f % NET_RING, per channel */
    uint32_t frame[NET_RING][4];  /* the frame the slot holds (0 = empty) */
    uint32_t hash[NET_HASHES][3]; /* {retrace, H, S} every s_hashEvery retraces */
    int32_t localPad[4];          /* page-written local pad (unused by the C sampler; kept for the layout) */
    uint32_t stat[12];            /* see ge_net_addr */
} net;
static int s_mask;                /* channels in the session (= presence mask) */
static int s_localMask;           /* channels whose input this peer produces */
static unsigned s_delay = 3;      /* D */
static unsigned s_dropFrom[4];    /* a lost player's channel reads neutral from this frame on (0 = not dropped) */
static int s_hashEvery = 10;
static int s_script;              /* local input from the GE_NETPADn scripts (tests) */
static int s_hashLog;             /* GE_NETHASHLOG=1: log "[nethash] N H S" at every hash */
static unsigned s_retrace;        /* retraces started; during a retrace = the one running */
static unsigned s_done;           /* the last retrace completed */
int ge_rec_mode;                  /* 0 off, 1 record, 2 replay (recorder section) */
static uint32_t rec_input(unsigned f, uint32_t live);
static void rec_retrace_done(void);

static uint32_t net_pack(unsigned buttons, int sx, int sy)
{
    return (buttons & 0xFFFFu) << 16 | (uint32_t)(uint8_t)(int8_t)sx << 8 | (uint8_t)(int8_t)sy;
}

/* ---------------------------------------------------------------- scripted pads (tests)
 * GE_NETPAD0..3 = "<retrace>:<tok>[,<tok>...];..." per channel. Buttons A B Z START L R UP DOWN LEFT RIGHT
 * CUP CDOWN CLEFT CRIGHT pulse for 4 retraces; <button>HOLD / <button>REL hold / release one; stick tokens
 * SUP SDOWN SLEFT SRIGHT SNONE (or SX<n> / SY<n>) are sustained until changed. Entries in retrace order. */
#define PAD_MAX 1024
#define PAD_PULSE 4
typedef struct { unsigned f; unsigned pulse, hold, rel; int hasX, hasY, sx, sy; } PadEntry;
static PadEntry s_pad[4][PAD_MAX];
static int s_padN[4] = { -1, -1, -1, -1 };
static const struct { const char *k; unsigned v; } kBtn[] = {
    {"A",0x8000}, {"B",0x4000}, {"Z",0x2000}, {"START",0x1000}, {"UP",0x0800}, {"DOWN",0x0400},
    {"LEFT",0x0200}, {"RIGHT",0x0100}, {"L",0x0020}, {"R",0x0010}, {"CUP",0x0008}, {"CDOWN",0x0004},
    {"CLEFT",0x0002}, {"CRIGHT",0x0001},
};
static unsigned btnOf(const char *s, int n)
{
    for (size_t i = 0; i < sizeof kBtn / sizeof kBtn[0]; i++)
        if ((int)strlen(kBtn[i].k) == n && !strncasecmp(kBtn[i].k, s, n)) return kBtn[i].v;
    return 0;
}
static void padParse(int ch)
{
    static const char *names[4] = { "GE_NETPAD0", "GE_NETPAD1", "GE_NETPAD2", "GE_NETPAD3" };
    const char *p = getenv(names[ch]);
    s_padN[ch] = 0;
    while (p && *p && s_padN[ch] < PAD_MAX) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        if (end == p || *end != ':') { sysLogPrintf(LOG_WARNING, "%s: bad entry near '%s'", names[ch], p); break; }
        p = end + 1;
        PadEntry *e = &s_pad[ch][s_padN[ch]++];
        memset(e, 0, sizeof *e);
        e->f = (unsigned)f;
        while (*p && *p != ';') {
            const char *t = p;
            while (*p && *p != ',' && *p != ';') p++;
            int n = (int)(p - t);
            unsigned b;
            if ((b = btnOf(t, n))) e->pulse |= b;
            else if (n > 4 && !strncasecmp(t + n - 4, "HOLD", 4) && (b = btnOf(t, n - 4))) e->hold |= b;
            else if (n > 3 && !strncasecmp(t + n - 3, "REL", 3) && (b = btnOf(t, n - 3))) e->rel |= b;
            else if (n == 3 && !strncasecmp(t, "SUP", 3)) { e->hasY = 1; e->sy = 80; }
            else if (n == 5 && !strncasecmp(t, "SDOWN", 5)) { e->hasY = 1; e->sy = -80; }
            else if (n == 5 && !strncasecmp(t, "SLEFT", 5)) { e->hasX = 1; e->sx = -80; }
            else if (n == 6 && !strncasecmp(t, "SRIGHT", 6)) { e->hasX = 1; e->sx = 80; }
            else if (n == 5 && !strncasecmp(t, "SNONE", 5)) { e->hasX = e->hasY = 1; e->sx = e->sy = 0; }
            else if (n > 2 && (t[1] == 'X' || t[1] == 'x') && (t[0] == 'S' || t[0] == 's')) { e->hasX = 1; e->sx = atoi(t + 2); }
            else if (n > 2 && (t[1] == 'Y' || t[1] == 'y') && (t[0] == 'S' || t[0] == 's')) { e->hasY = 1; e->sy = atoi(t + 2); }
            else sysLogPrintf(LOG_WARNING, "%s: unknown token '%.*s'", names[ch], n, t);
            if (*p == ',') p++;
        }
        if (*p == ';') p++;
    }
    if (s_padN[ch]) sysLogPrintf(LOG_NOTE, "[net] %s: %d entries", names[ch], s_padN[ch]);
}
/* the scripted input of channel ch at frame f (pure function of the script) */
static uint32_t padScript(int ch, unsigned f)
{
    if (s_padN[ch] < 0) padParse(ch);
    unsigned b = 0, hold = 0;
    int sx = 0, sy = 0;
    for (int i = 0; i < s_padN[ch]; i++) {
        const PadEntry *e = &s_pad[ch][i];
        if (e->f > f) break;
        if (f - e->f < PAD_PULSE) b |= e->pulse;
        hold = (hold | e->hold) & ~e->rel;
        if (e->hasX) sx = e->sx;
        if (e->hasY) sy = e->sy;
    }
    return net_pack(b | hold, sx, sy);
}

/* ---------------------------------------------------------------- page API: session setup */
/* Before main(). present_mask = channels in the session (bit i = controller i + 1; the game sees exactly these
 * plugged in), local_mask = channels this peer feeds, delay = D (>= 1), hash_every = K (0 = no hash). */
EMSCRIPTEN_KEEPALIVE void ge_net_session(int present_mask, int local_mask, int delay, int hash_every)
{
    geNetDetOn = 1;
    geNetSessionOn = 1;
    geNetPurePad = 1;
    s_mask = present_mask & 0xF;
    s_localMask = local_mask & s_mask;
    s_delay = delay < 1 ? 1 : delay > 32 ? 32 : (unsigned)delay;
    s_hashEvery = hash_every < 0 ? 0 : hash_every;
    { const char *e = getenv("GE_NETSCRIPT"); s_script = e && *e && *e != '0'; }
    { const char *e = getenv("GE_NETHASHLOG"); s_hashLog = e && *e ? atoi(e) : 0; }
    sysLogPrintf(LOG_NOTE, "[net] session: channels %X, local %X, input delay %u, hash every %d retraces%s",
                 s_mask, s_localMask, s_delay, s_hashEvery, s_script ? ", local input from GE_NETPADn" : "");
}
/* 0 = input ring (u32 [256][4]), 1 = frame tags (u32 [256][4]), 2 = hash ring (u32 [64][3]), 3 = local pad,
 * 4 = stats (u32: 0 stalled ticks, 1 -, 2 last retrace run, 3 input delay, 4 hash every K, 5 channel mask,
 *     6 local mask, 7 ms slowed by the time sync, 8 retraces that had to wait for input, 9 ms waited) */
EMSCRIPTEN_KEEPALIVE uint32_t ge_net_addr(int which)
{
    net.stat[2] = s_done; net.stat[3] = s_delay; net.stat[4] = (uint32_t)s_hashEvery;
    net.stat[5] = (uint32_t)s_mask; net.stat[6] = (uint32_t)s_localMask;
    switch (which) {
    case 0: return (uint32_t)(uintptr_t)net.input;
    case 1: return (uint32_t)(uintptr_t)net.frame;
    case 2: return (uint32_t)(uintptr_t)net.hash;
    case 3: return (uint32_t)(uintptr_t)net.localPad;
    default: return (uint32_t)(uintptr_t)net.stat;
    }
}
EMSCRIPTEN_KEEPALIVE uint32_t ge_net_frame(void) { return s_done; }   /* the last retrace run */
EMSCRIPTEN_KEEPALIVE uint32_t ge_net_script_input(int ch, uint32_t f) { return padScript(ch & 3, f); }
EMSCRIPTEN_KEEPALIVE void ge_net_drop(int ch, uint32_t from)
{
    s_dropFrom[ch & 3] = from ? from : 1;
    sysLogPrintf(LOG_NOTE, "[net] channel %d neutral from retrace %u (player left)", ch & 3, from);
}

unsigned geNetRetrace(void) { return s_retrace; }
int geNetMask(void) { return s_mask; }

/* ---------------------------------------------------------------- det clock */
#define DET_Q 931050u       /* counter ticks per PAL retrace (46.5525 MHz / 50) */
static unsigned s_detReads;
static uint64_t detTicks(int bump)
{
    if (bump) s_detReads++;
    uint64_t sub = (uint64_t)s_detReads * 64u;
    if (sub > DET_Q / 2 - 1) sub = DET_Q / 2 - 1;
    return (uint64_t)s_retrace * DET_Q + sub;
}
uint32_t geNetDetCount(int bump) { return (uint32_t)detTicks(bump); }
uint64_t geNetDetUs(int bump) { return detTicks(bump) * 10000u / 465525u; }

/* ---------------------------------------------------------------- virtual AI queue */
#define AI_DRAIN_PER_RETRACE 1764u   /* 22050 Hz x 4 bytes / 50 */
static uint32_t s_aiQueued, s_aiLast;
void geNetAiPush(uint32_t len, int limitFrames)
{
    if (limitFrames > 0 && s_aiQueued / 4u >= (uint32_t)limitFrames) return;   /* the real queue would drop it */
    s_aiQueued += len;
    s_aiLast = len;
}
uint32_t geNetAiQueued(void) { return s_aiQueued; }
uint32_t geNetAiLast(void) { return s_aiLast; }

/* ---------------------------------------------------------------- the frame barrier */
static int net_has(unsigned f, int ch)
{
    return f <= s_delay || (s_dropFrom[ch] && f >= s_dropFrom[ch]) || net.frame[f % NET_RING][ch] == f;
}
EMSCRIPTEN_KEEPALIVE int ge_net_ready(uint32_t f)
{
    if (!geNetSessionOn || ge_rec_mode) return 1;
    for (int ch = 0; ch < 4; ch++)
        if ((s_mask >> ch & 1) && !net_has(f, ch)) return 0;
    return 1;
}
int geNetReady(unsigned f) { return ge_net_ready(f); }
static double s_waitSinceMs;
void geNetStalled(void)
{
    net.stat[0]++;
    if (!s_waitSinceMs) { s_waitSinceMs = emscripten_get_now(); net.stat[8]++; }
}

/* Time sync (GGPO-style frame advantage, Conker P339): net.js measures this peer's input margin against each
 * other player and asks it to slow down when it runs ahead; the main loop takes that wall time out of its 50 Hz
 * backlog, at most 8 % of the elapsed time. Only WHEN a retrace runs changes, never what it computes. */
#define NET_SLOW_MAX_RATE 0.08
static double s_slowLeftMs, s_slowTotalMs;
EMSCRIPTEN_KEEPALIVE void ge_net_slow(double ms) { s_slowLeftMs = ms > 0 ? ms : 0; }
double geNetTakeSlowMs(double elapsedMs)
{
    double cap = elapsedMs * NET_SLOW_MAX_RATE, take = s_slowLeftMs < cap ? s_slowLeftMs : cap;
    if (take < 0) take = 0;
    s_slowLeftMs -= take; s_slowTotalMs += take;
    net.stat[7] = (uint32_t)s_slowTotalMs;
    return take;
}

/* ---------------------------------------------------------------- per retrace */
/* start of retrace N (webkernel.c, before the VI post): sample the local input for N + D and send it */
void geNetRetraceStart(void)
{
    s_retrace++;
    s_detReads = 0;
    s_aiQueued = s_aiQueued > AI_DRAIN_PER_RETRACE ? s_aiQueued - AI_DRAIN_PER_RETRACE : 0;
    if (!geNetSessionOn) return;
    if (s_waitSinceMs) { net.stat[9] += (uint32_t)(emscripten_get_now() - s_waitSinceMs + 0.5); s_waitSinceMs = 0; }
    unsigned f = s_retrace + s_delay, slot = f % NET_RING;
    uint32_t live = 0;
    if (s_localMask && !s_script) {
        signed char sx = 0, sy = 0;
        inputUpdate();
        unsigned b = inputComputePad(0, &sx, &sy);   /* pure N64 pad (input.c geNetPurePad) */
        live = net_pack(b, sx, sy);
    }
    for (int ch = 0; ch < 4; ch++) {
        if (!(s_localMask >> ch & 1)) continue;
        uint32_t v = s_script ? padScript(ch, f) : live;
        net.input[slot][ch] = rec_input(f, v);
        net.frame[slot][ch] = f;
        if (ge_rec_mode == 1) EM_ASM({ if (Module['recInput']) Module['recInput']($0, $1 >>> 0); }, f, net.input[slot][ch]);
    }
    if (s_localMask && !ge_rec_mode) EM_ASM({ if (Module['netAfterTick']) Module['netAfterTick'](); });
}

/* libultra.c controller read: retrace N reads frame N's inputs */
int geNetPad(int ch, unsigned *buttons, int *sx, int *sy)
{
    *buttons = 0; *sx = *sy = 0;
    if (!(s_mask >> ch & 1)) return 0;
    unsigned f = s_retrace, slot = f % NET_RING;
    uint32_t v = f > s_delay && !(s_dropFrom[ch] && f >= s_dropFrom[ch]) && net.frame[slot][ch] == f ? net.input[slot][ch] : 0;
    *buttons = v >> 16; *sx = (int8_t)(v >> 8); *sy = (int8_t)v;
    return 1;
}

/* ---------------------------------------------------------------- state hash */
#define NET_MAX_REGIONS 4096
#define NET_MAX_PAGES   8192
#define NET_PAGE        4096u
#define NET_PAGEVECS    16
static struct { uint32_t addr, len; } s_reg[NET_MAX_REGIONS];
static int s_nreg, s_regsFinal;
static uint32_t s_pageAddr[NET_MAX_PAGES], s_pageLen[NET_MAX_PAGES];
static int s_npages;
static uint32_t s_pageVec[NET_PAGEVECS][NET_MAX_PAGES + 1];   /* + 1: S */
static uint32_t s_pageVecFrame[NET_PAGEVECS];
static uint32_t s_hashedBytes;

EMSCRIPTEN_KEEPALIVE int ge_net_region_add(uint32_t addr, uint32_t len)
{
    if (s_regsFinal || s_nreg >= NET_MAX_REGIONS || !len) return -1;
    s_reg[s_nreg].addr = addr; s_reg[s_nreg].len = len; s_nreg++;
    return s_nreg;
}
static void regionsFinal(void)
{
    if (s_regsFinal) return;
    s_regsFinal = 1;
    if (s_nreg < NET_MAX_REGIONS) { s_reg[s_nreg].addr = 0x80000000u; s_reg[s_nreg].len = 0x800000u; s_nreg++; }   /* N64 DRAM */
    for (int i = 0; i < s_nreg; i++)
        for (uint32_t o = 0; o < s_reg[i].len && s_npages < NET_MAX_PAGES; o += NET_PAGE) {
            s_pageAddr[s_npages] = s_reg[i].addr + o;
            s_pageLen[s_npages] = s_reg[i].len - o < NET_PAGE ? s_reg[i].len - o : NET_PAGE;
            s_hashedBytes += s_pageLen[s_npages];
            s_npages++;
        }
    sysLogPrintf(LOG_NOTE, "[nethash] %d regions (%d from ge007.netregions + DRAM), %d pages, %u bytes hashed",
                 s_nreg, s_nreg - 1, s_npages, s_hashedBytes);
}
uint32_t geNetHashBytes(const void *p, uint32_t n, uint32_t seed)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed << 32) ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w; memcpy(&w, b + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull; h ^= h >> 32;
    }
    for (; i < n; i++) { h = (h ^ b[i]) * 0x100000001B3ull; }
    h ^= h >> 29; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 32;
    return (uint32_t)h;
}
/* H and S now; also keeps the per-page vector for the desync report */
static void hashNow(uint32_t *H, uint32_t *S)
{
    regionsFinal();
    uint32_t *v = s_pageVec[(s_retrace / (s_hashEvery > 0 ? (unsigned)s_hashEvery : 1u)) % NET_PAGEVECS];
    for (int i = 0; i < s_npages; i++) v[i] = geNetHashBytes((const void *)(uintptr_t)s_pageAddr[i], s_pageLen[i], 0);
    v[s_npages] = webkernelStackHash();
    s_pageVecFrame[(s_retrace / (s_hashEvery > 0 ? (unsigned)s_hashEvery : 1u)) % NET_PAGEVECS] = s_retrace;
    *H = geNetHashBytes(v, (uint32_t)s_npages * 4u, 1);
    *S = v[s_npages];
}
/* desync report: the page vector of retrace f (still kept?) -> wasm address or 0; info: 0 pages, 1 = 1 (S) */
EMSCRIPTEN_KEEPALIVE uint32_t ge_net_page_hashes(uint32_t f)
{
    for (int i = 0; i < NET_PAGEVECS; i++) if (s_pageVecFrame[i] == f && f) return (uint32_t)(uintptr_t)s_pageVec[i];
    return 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t ge_net_page_info(int which, int i)
{
    switch (which) {
    case 0: return (uint32_t)s_npages;
    case 1: return 1;
    case 2: return i >= 0 && i < s_npages ? s_pageAddr[i] : 0;
    case 3: return i >= 0 && i < s_npages ? s_pageLen[i] : 0;
    default: return s_hashedBytes;
    }
}

/* end of retrace N (webkernel.c, after the fibers ran): desync test, hash, recorder */
void geNetRetraceDone(void)
{
    static int desyncAt = -1;
    s_done = s_retrace;
    if (!geNetDetOn) return;
    if (desyncAt < 0) { const char *e = getenv("GE_NETDESYNC"); desyncAt = e ? atoi(e) : 0; }
    if (desyncAt > 0 && s_retrace == (unsigned)desyncAt) {   /* test: flip one byte of game memory (one peer only) */
        volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)(0x80000000u + 0x200000u);
        *b ^= 1;
        sysLogPrintf(LOG_NOTE, "[net] test: flipped one byte of N64 DRAM (0x80200000) at retrace %u", s_retrace);
    }
    if (ge_rec_mode) rec_retrace_done();
    if (s_hashEvery <= 0 || s_retrace % (unsigned)s_hashEvery) return;
    uint32_t *e = net.hash[(s_retrace / (unsigned)s_hashEvery) % NET_HASHES];
    hashNow(&e[1], &e[2]);
    e[0] = s_retrace;
    if (s_hashLog) {
        extern uint32_t webkernelStackHashPart(int which);
        if (s_hashLog > 1) sysLogPrintf(LOG_NOTE, "[nethash] %u %08x %08x c %08x a %08x", s_retrace, e[1], e[2],
                                        webkernelStackHashPart(1), webkernelStackHashPart(2));
        else sysLogPrintf(LOG_NOTE, "[nethash] %u %08x %08x", s_retrace, e[1], e[2]);
    }
}

/* ---------------------------------------------------------------- EEPROM image */
static uint8_t s_eep[2048];
static int s_eepSet;
EMSCRIPTEN_KEEPALIVE uint32_t ge_net_eeprom_buf(void) { s_eepSet = 1; return (uint32_t)(uintptr_t)s_eep; }   /* page fills 2048 bytes */
int geNetEepromImage(uint8_t *dst, int len)
{
    if (len > (int)sizeof s_eep) len = (int)sizeof s_eep;
    memcpy(dst, s_eep, (size_t)len);   /* blank (zeros) unless the page wrote the host's image */
    sysLogPrintf(LOG_INFO, "eeprom: session image (%s), never persisted", s_eepSet ? "from the host" : "blank");
    return len;
}

/* ---------------------------------------------------------------- session options */
/* After configLoad() (the host's ini, written by the page): values every peer must share that the ini cannot
 * fix by itself. The window aspect (fullscreen, a different canvas) would change the game's projection, a
 * renderer classification would change draw / LOD distances, and the input adapters that write game state must
 * stay off (geNetPurePad does that in input.c; OG controls keep the game's own control style). */
void geNetApplySessionConfig(void)
{
    static const struct { const char *k; double v; } kFix[] = {
        { "Video.NativeWidescreen", 0 }, { "Video.WidescreenAuto", 0 },
        { "Video.LowEndConsidered", 1 }, { "Video.DeckPresetApplied", 1 },
        { "Input.ControlScheme", 1 }, { "Input.PdMouseAim", 0 },
        { "Game.AllUnlocked", 0 },
    };
    if (!geNetDetOn) return;
    for (size_t i = 0; i < sizeof kFix / sizeof kFix[0]; i++)
        if (configSetValue(kFix[i].k, kFix[i].v) != 0)
            sysLogPrintf(LOG_WARNING, "[net] session option %s not set", kFix[i].k);
    sysLogPrintf(LOG_NOTE, "[net] session options fixed (4:3 projection, OG controls, no port aim / crouch adapters)");
}

/* ---------------------------------------------------------------- page kick / background */
EMSCRIPTEN_KEEPALIVE void ge_net_kick(void) { webkernelKick(); }

/* ---------------------------------------------------------------- recorder (.gerec)
 * A recording is a one-player session of the machinery above with no input delay: the page calls
 * ge_rec_session(1, ...) before main(); retrace N's input is sampled at the start of retrace N from the local
 * pad (or GE_NETPAD0) and handed to the page (Module.recInput), which stores the list. Replay
 * (ge_rec_session(2, ...)) takes retrace N's input from the recording instead, optionally back to back (fast).
 * Checkpoints every check_every retraces: H / S (Module.recCheck). */
static uint32_t *s_recInputs;
static unsigned s_recN, s_recStopAt, s_recCheckEvery = 50;
int ge_rec_fast;
static int s_recEnded;
EMSCRIPTEN_KEEPALIVE void ge_rec_session(int mode, int check_every, uint32_t inputs, uint32_t n, int fast, uint32_t stop_at)
{
    ge_net_session(1, 1, 1, 0);
    s_delay = 0;
    ge_rec_mode = mode;
    s_recCheckEvery = check_every > 0 ? (unsigned)check_every : 50;
    s_recInputs = (uint32_t *)(uintptr_t)inputs; s_recN = n;
    ge_rec_fast = mode == 2 && fast;
    s_recStopAt = stop_at;
    sysLogPrintf(LOG_NOTE, "[rec] %s session: checkpoint every %u retraces%s", mode == 1 ? "recording" : "replay",
                 s_recCheckEvery, mode == 2 ? (ge_rec_fast ? ", fast" : ", real time") : "");
}
static uint32_t rec_input(unsigned f, uint32_t live)
{
    if (ge_rec_mode != 2) return live;
    return f >= 1 && f <= s_recN ? s_recInputs[f - 1] : 0;
}
static void rec_checkpoint(unsigned f, int kind)
{
    uint32_t h, s;
    hashNow(&h, &s);
    EM_ASM({ if (Module['recCheck']) Module['recCheck']($0, $1 >>> 0, $2 >>> 0, $3); }, f, h, s, kind);
}
EMSCRIPTEN_KEEPALIVE void ge_rec_checkpoint_now(void) { rec_checkpoint(s_done, 2); }
static void rec_retrace_done(void)
{
    unsigned f = s_retrace;
    if (f % s_recCheckEvery == 0) rec_checkpoint(f, 0);
    if (ge_rec_mode == 2 && f == s_recN) rec_checkpoint(f, 2);
    if (ge_rec_mode == 2 && !s_recEnded && f >= (s_recStopAt ? s_recStopAt : s_recN)) {
        s_recEnded = 1;
        webkernelHalt();
        sysLogPrintf(LOG_NOTE, "[rec] replay stopped after retrace %u", f);
        EM_ASM({ if (Module['recEnd']) Module['recEnd']($0); }, f);
    }
}
