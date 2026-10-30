/*
 * webkernel.c — cooperative libultra threads on Emscripten fibers (see webkernel.h).
 *
 * Model (same as the Conker web port's pc_sched.c, simplified):
 *  - each game thread is an emscripten_fiber_t with static C + asyncify stacks
 *    (fixed addresses: their contents end up in game memory, and fixed slots
 *    keep two browsers' layouts identical for later lockstep work);
 *  - the scheduler is the browser's main context; it resumes the highest
 *    priority READY fiber until no fiber is ready, then returns to the browser;
 *  - a blocked fiber (WAITING) becomes READY again whenever any queue changes
 *    (webkernelSignal bumps a counter); it re-checks its own condition in
 *    libultra.c's loop and waits again if it was not for it;
 *  - a busy-waiting fiber (SPIN) is resumed on the next browser frame only.
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <emscripten/fiber.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "webkernel.h"
#include "webnet.h"
#include "platform.h"
#include "system.h"

#define WK_MAX_THREADS 8
#define WK_C_STACK     (1024 * 1024)
#define WK_A_STACK     (1024 * 1024)
#define WK_MAX_SWITCHES_PER_FRAME 20000
#define WK_SPIN_LIMIT  64

enum { WK_FREE, WK_READY, WK_RUNNING, WK_WAITING, WK_SPIN, WK_DEAD };

typedef struct {
    emscripten_fiber_t fiber;
    void (*entry)(void *);
    void *arg;
    const int *prio;
    int id;
    int state;
    unsigned waitSeq;      /* signal counter value when it blocked */
    unsigned order;        /* FIFO among equal priorities */
    unsigned switches;
    const void *what;      /* what it waits on (diagnostics) */
} WkThread;

static WkThread s_threads[WK_MAX_THREADS];
static char s_cstack[WK_MAX_THREADS][WK_C_STACK] __attribute__((aligned(16)));
static char s_astack[WK_MAX_THREADS][WK_A_STACK] __attribute__((aligned(16)));
static emscripten_fiber_t s_mainFiber;
static char s_mainAstack[WK_A_STACK] __attribute__((aligned(16)));
static WkThread *s_cur = NULL;
static unsigned s_signalSeq = 1;
static unsigned s_orderSeq = 0;
static unsigned s_spinCalls = 0;
static int s_inited = 0;
static unsigned s_lastRunFrame = 0;   /* last frame any fiber ran */
static int s_stallReported = 0;

static void (*s_pump)(void);
static void (*s_retrace)(void);
static double s_periodMs;
static double s_nextRetraceMs;
static unsigned s_frames;

/* Debug trace (GE_WKTRACE=<frame>, page: ?env=GE_WKTRACE=800): from that browser frame on, log every
 * fiber switch and every webkernelTrace() point, so the last lines before a hang name the thread and
 * the place. Every event also goes into a small ring (ge_web_wk_ring) that a debugger can read. */
static unsigned s_traceFrom = 0;
typedef struct { unsigned frame; int tid; const char *tag; uintptr_t arg; } WkEvent;
#define WK_RING 256
WkEvent ge_web_wk_ring[WK_RING];
unsigned ge_web_wk_ringPos;

void webkernelTrace(const char *tag, uintptr_t arg)
{
    WkEvent *e = &ge_web_wk_ring[ge_web_wk_ringPos++ % WK_RING];
    e->frame = s_frames; e->tid = s_cur ? s_cur->id : -1; e->tag = tag; e->arg = arg;
    if (s_traceFrom && s_frames >= s_traceFrom)
        sysLogPrintf(LOG_NOTE, "wk %u t%d %s 0x%llx", s_frames, e->tid, tag, (unsigned long long)arg);
}

static void wkInit(void)
{
    if (s_inited) return;
    s_inited = 1;
    { const char *e = getenv("GE_WKTRACE"); if (e) s_traceFrom = (unsigned)strtoul(e, NULL, 0); }
    emscripten_fiber_init_from_current_context(&s_mainFiber, s_mainAstack, sizeof s_mainAstack);
}

static int wkPrio(const WkThread *t) { return t->prio ? *t->prio : 0; }

static int wkRunnable(const WkThread *t)
{
    return t->state == WK_READY ||
           (t->state == WK_WAITING && t->waitSeq != s_signalSeq);
}

static WkThread *wkPick(void)
{
    WkThread *best = NULL;
    for (int i = 0; i < WK_MAX_THREADS; i++) {
        WkThread *t = &s_threads[i];
        if (!wkRunnable(t)) continue;
        if (!best || wkPrio(t) > wkPrio(best) ||
            (wkPrio(t) == wkPrio(best) && t->order < best->order))
            best = t;
    }
    return best;
}

static void wkSwitchOut(void)
{
    WkThread *me = s_cur;
    emscripten_fiber_swap(&me->fiber, &s_mainFiber);
}

static void wkFiberEntry(void *arg)
{
    WkThread *t = (WkThread *)arg;
    t->entry(t->arg);
    sysLogPrintf(LOG_NOTE, "webkernel: thread %d returned", t->id);
    t->state = WK_DEAD;
    wkSwitchOut();   /* never resumed */
}

void webkernelSpawn(void (*entry)(void *), void *arg, const int *prio, int id)
{
    wkInit();
    for (int i = 0; i < WK_MAX_THREADS; i++) {
        WkThread *t = &s_threads[i];
        if (t->state != WK_FREE) continue;
        memset(t, 0, sizeof *t);
        t->entry = entry;
        t->arg = arg;
        t->prio = prio;
        t->id = id;
        t->state = WK_READY;
        t->order = ++s_orderSeq;
        emscripten_fiber_init(&t->fiber, wkFiberEntry, t, s_cstack[i], WK_C_STACK,
                              s_astack[i], WK_A_STACK);
        sysLogPrintf(LOG_NOTE, "webkernel: thread %d (prio %d) in slot %d", id, wkPrio(t), i);
        return;
    }
    sysFatalError("webkernel: out of thread slots");
}

int webkernelInFiber(void) { return s_cur != NULL; }

void webkernelWait(const void *what)
{
    s_spinCalls = 0;
    if (!s_cur) {
        sysFatalError("webkernel: blocking wait outside a game thread");
        return;
    }
    s_cur->state = WK_WAITING;
    s_cur->waitSeq = s_signalSeq;
    s_cur->what = what;
    webkernelTrace("wait", (uintptr_t)what);
    wkSwitchOut();
}

void webkernelSignal(void)
{
    s_spinCalls = 0;
    s_signalSeq++;
    if (!s_cur) return;
    /* libultra: a send that wakes a higher-priority thread switches to it now. */
    WkThread *n = wkPick();
    if (n && n != s_cur && wkPrio(n) > wkPrio(s_cur)) {
        s_cur->state = WK_READY;
        s_cur->order = ++s_orderSeq;
        wkSwitchOut();
    }
}

void webkernelYield(void)
{
    s_spinCalls = 0;
    if (!s_cur) return;
    s_cur->state = WK_READY;
    s_cur->order = ++s_orderSeq;
    wkSwitchOut();
}

void webkernelSpinCheck(void)
{
    if (!s_cur) return;
    if (++s_spinCalls < WK_SPIN_LIMIT) return;
    s_spinCalls = 0;
    webkernelTrace("spin", 0);
    s_cur->state = WK_SPIN;
    wkSwitchOut();
}

static void wkRunReady(void)
{
    for (int n = 0; n < WK_MAX_SWITCHES_PER_FRAME; n++) {
        WkThread *t = wkPick();
        if (!t) return;
        s_cur = t;
        t->state = WK_RUNNING;
        t->switches++;
        s_spinCalls = 0;
        s_lastRunFrame = s_frames;
        webkernelTrace("run", t->id);
        emscripten_fiber_swap(&s_mainFiber, &t->fiber);
        webkernelTrace("back", (uintptr_t)t->state);
        s_cur = NULL;

    }
    sysLogPrintf(LOG_WARNING, "webkernel: %d switches in one frame; back to the browser",
                 WK_MAX_SWITCHES_PER_FRAME);
}

static const char *wkStateName(int st)
{
    static const char *n[] = { "free", "ready", "running", "waiting", "spin", "dead" };
    return st >= 0 && st <= WK_DEAD ? n[st] : "?";
}

/* Every thread blocked for ~2 s of frames: report who waits on what, once. */
static void wkStallCheck(void)
{
    if (s_frames - s_lastRunFrame < 120 || s_stallReported) return;
    s_stallReported = 1;
    sysLogPrintf(LOG_ERROR, "webkernel: STALL, no thread ran for %u frames:", s_frames - s_lastRunFrame);
    for (int i = 0; i < WK_MAX_THREADS; i++) {
        WkThread *t = &s_threads[i];
        char buf[160] = "";
        if (t->state == WK_FREE) continue;
        if (t->state == WK_WAITING) portWebDescribeWait(t->what, buf, sizeof buf);
        sysLogPrintf(LOG_ERROR, "  thread %d prio %d %s switches %u %s", t->id, wkPrio(t),
                     wkStateName(t->state), t->switches, buf);
    }
}

/* Pacing report every 10 s of wall time: console retraces posted (should be 50/s PAL), browser
 * frames, game frames rendered, retraces dropped by the catch-up cap. Cheap; always on. */
static unsigned s_retraces, s_rendered, s_dropped;
static double s_statT0;
static unsigned s_statRetr, s_statFrames, s_statRend, s_statDrop;
void webkernelFrameRendered(void) { s_rendered++; }

static void wkPacingReport(double now)
{
    if (s_statT0 == 0) {   /* first call, or after a resume: open a new window */
        s_statT0 = now; s_statRetr = s_retraces; s_statFrames = s_frames; s_statRend = s_rendered;
        s_statDrop = s_dropped;
        return;
    }
    double dt = (now - s_statT0) / 1000.0;
    if (dt < 10.0) return;
    sysLogPrintf(LOG_NOTE, "pacing: %.1f s: retraces %.2f/s, browser frames %.1f/s, game frames %.2f/s, "
                 "dropped retraces %u", dt, (s_retraces - s_statRetr) / dt, (s_frames - s_statFrames) / dt,
                 (s_rendered - s_statRend) / dt, s_dropped - s_statDrop);
    s_statT0 = now; s_statRetr = s_retraces; s_statFrames = s_frames; s_statRend = s_rendered;
    s_statDrop = s_dropped;
}

/* G6 det mode (geNetDetOn: an online session, an offline det run, a recording; webnet.c). Everything the
 * game computes is indexed by retrace, never by browser frame or wall time:
 *  - fibers run only inside a retrace, until every one of them blocks (no run on frames without a retrace);
 *  - a busy-waiting fiber (SPIN) is parked until the next retrace, not the next browser frame;
 *  - retrace N runs only when every session channel's input for N is there (geNetReady, the frame barrier);
 *    otherwise the tick ends and the backlog is clamped to one period;
 *  - the loop runs on a 4 ms timer (webkernelMainLoop), and the page can run a tick at once when the input a
 *    stalled game waits for arrives (webkernelKick), or from a worker while the page is hidden.
 * Wall time only decides WHEN a retrace runs (50 Hz pacing, time sync), never what it computes. */
static int s_det, s_inFrame, s_halted;
static double s_lastTickMs;
static void wkDetRetrace(void)
{
    geNetRetraceStart();
    for (int i = 0; i < WK_MAX_THREADS; i++)
        if (s_threads[i].state == WK_SPIN) s_threads[i].state = WK_READY;
    s_retraces++;
    if (s_retrace) s_retrace();
    wkRunReady();
    geNetRetraceDone();
}
static void wkFrameDet(void)
{
    if (s_pump) s_pump();   /* SDL events: local devices and the window, no game state */
    double now = emscripten_get_now();
    if (s_nextRetraceMs == 0) s_nextRetraceMs = now;
    if (s_lastTickMs) s_nextRetraceMs += geNetTakeSlowMs(now - s_lastTickMs);   /* time sync */
    s_lastTickMs = now;
    int due = 0;
    extern int ge_rec_fast;
    if (ge_rec_fast) {   /* replay at full speed: retraces back to back for ~12 ms per tick */
        while (!s_halted && emscripten_get_now() - now < 12.0) wkDetRetrace();
        s_nextRetraceMs = now;
    }
    while (!s_halted && now >= s_nextRetraceMs && due < 4) {
        if (!geNetReady(geNetRetrace() + 1)) {
            geNetStalled();
            if (s_nextRetraceMs < now - s_periodMs) s_nextRetraceMs = now - s_periodMs;
            break;
        }
        s_nextRetraceMs += s_periodMs;
        due++;
        wkDetRetrace();
    }
    if (now - s_nextRetraceMs > 4 * s_periodMs) {
        s_dropped += (unsigned)((now - s_nextRetraceMs) / s_periodMs);
        s_nextRetraceMs = now - s_periodMs;
    }
    wkPacingReport(now);
}

static void wkFrame(void)
{
    s_frames++;
    if (s_det) {
        if (s_inFrame) return;
        s_inFrame = 1;
        wkFrameDet();
        s_inFrame = 0;
        return;
    }

    /* Busy-waiting threads get another go this frame. */
    for (int i = 0; i < WK_MAX_THREADS; i++)
        if (s_threads[i].state == WK_SPIN) s_threads[i].state = WK_READY;

    if (s_pump) s_pump();

    /* Console retraces due by wall time (PAL 50 Hz / NTSC 60 Hz), fixed step,
     * at most 3 per animation frame (a hidden tab or a slow frame skips the
     * backlog instead of fast-forwarding). */
    double now = emscripten_get_now();
    if (s_nextRetraceMs == 0) s_nextRetraceMs = now;
    int due = 0;
    while (now >= s_nextRetraceMs && due < 3) {
        s_nextRetraceMs += s_periodMs;
        due++;
        s_retraces++;
        if (s_retrace) s_retrace();
        wkRunReady();
    }
    if (now - s_nextRetraceMs > 4 * s_periodMs) {
        s_dropped += (unsigned)((now - s_nextRetraceMs) / s_periodMs);
        s_nextRetraceMs = now + s_periodMs;
    }
    wkPacingReport(now);
    if (!due) wkRunReady();   /* spinning threads (frame pacing) poll the clock */
    wkStallCheck();
}

/* Hidden-tab pause (webpage.c). Browsers stop requestAnimationFrame for a hidden tab anyway; pausing
 * explicitly also stops the console clock: the paused span is subtracted from sysGetMicroseconds()
 * (osGetCount, osGetTime, timers), so after resume the game continues from the frame it was on instead
 * of seeing one huge frame delta, and the retrace schedule restarts from "now" (no backlog). */
static int s_paused;
static double s_pauseStartMs;
static uint64_t s_pausedTotalUs;
int webkernelPaused(void) { return s_paused; }
uint64_t webkernelPausedUs(void)
{
    uint64_t us = s_pausedTotalUs;
    if (s_paused) us += (uint64_t)((emscripten_get_now() - s_pauseStartMs) * 1000.0);
    return us;
}
void webkernelSetPaused(int paused)
{
    if (s_det) return;   /* G6: a session never pauses silently; net.js ticks it from a worker while hidden */
    if (!!paused == s_paused) return;
    double now = emscripten_get_now();
    if (paused) {
        s_pauseStartMs = now;
        s_paused = 1;
        emscripten_pause_main_loop();
    } else {
        s_pausedTotalUs += (uint64_t)((now - s_pauseStartMs) * 1000.0);
        s_paused = 0;
        s_nextRetraceMs = 0;     /* restart the retrace schedule from now */
        s_statT0 = 0;            /* pacing report: new 10 s window */
        emscripten_resume_main_loop();
    }
    sysLogPrintf(LOG_NOTE, "webkernel: %s (paused total %.1f s)", paused ? "paused" : "resumed",
                 s_pausedTotalUs / 1e6);
}

void webkernelMainLoop(void (*pump)(void), void (*retrace)(void), unsigned period_us)
{
    wkInit();
    s_pump = pump;
    s_retrace = retrace;
    s_periodMs = period_us / 1000.0;
    s_det = geNetDetOn;
    sysLogPrintf(LOG_NOTE, "webkernel: main loop, retrace every %.3f ms%s", s_periodMs,
                 s_det ? " (det: retrace-indexed fibers, 4 ms timer loop)" : "");
    emscripten_set_main_loop(wkFrame, 0, 0);
    if (s_det) emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 4);
}

/* G6: run a det tick now (the page got the input a stalled retrace waits for; or a worker tick while hidden). */
void webkernelKick(void) { if (s_det) wkFrame(); }
/* G6: stop running retraces (end of a recording's replay); the page reads the result. */
void webkernelHalt(void) { s_halted = 1; }

/* G6 state hash S: the live part of a game fiber's stacks (C stack from its saved stack pointer to the top,
 * asyncify buffer -- the saved wasm locals of its call chain -- from its start to the saved position). Called
 * between retraces: every fiber is suspended. */
uint32_t geNetHashBytes(const void *p, uint32_t n, uint32_t seed);
uint32_t webkernelStackHashPart(int which)   /* 1 = C stacks only, 2 = asyncify buffers only, 3 = both */
{
    uint32_t h = 0;
    for (int i = 0; i < WK_MAX_THREADS; i++) {
        WkThread *t = &s_threads[i];
        if (t->state == WK_FREE || t->state == WK_DEAD) continue;
        uintptr_t sp = (uintptr_t)t->fiber.stack_ptr, top = (uintptr_t)t->fiber.stack_base;
        uintptr_t a0 = (uintptr_t)s_astack[i], a1 = (uintptr_t)t->fiber.asyncify_data.stack_ptr;
        if ((which & 1) && sp >= (uintptr_t)s_cstack[i] && sp <= top) h = geNetHashBytes((const void *)sp, (uint32_t)(top - sp), h ^ (uint32_t)i);
        if ((which & 2) && a1 >= a0 && a1 <= a0 + WK_A_STACK) h = geNetHashBytes((const void *)a0, (uint32_t)(a1 - a0), h ^ 0x100u ^ (uint32_t)i);
        h ^= (uint32_t)t->state * 0x9E3779B1u + (uint32_t)t->id;
    }
    return h;
}
/* S = the asyncify buffers only: the C (shadow) stacks' live frames keep uninitialised slack written by earlier
 * deeper calls (log formatting with wall-clock numbers, the renderer), which differs run to run without being game
 * state (G6: two identical det runs differed in C stacks at 98 % of retraces, never in the asyncify buffers). */
uint32_t webkernelStackHash(void) { return webkernelStackHashPart(2); }

/* Guard (linked with -Wl,--wrap=emscripten_sleep): an ASYNCIFY sleep from inside a game fiber unwinds
 * the fiber as if it were the main loop and breaks the scheduler (G2's post-logo hang came from SDL's
 * SwapWindow doing emscripten_sleep(0)). In a fiber the sleep is refused and reported once; the
 * browser frame boundary is the scheduler's job. Outside fibers (never used today) it passes through. */
void __real_emscripten_sleep(unsigned int ms);
void __wrap_emscripten_sleep(unsigned int ms)
{
    static int reported;
    if (s_cur) {
        if (!reported++)
            sysLogPrintf(LOG_ERROR, "webkernel: emscripten_sleep(%u) from fiber %d refused (would break "
                         "the fiber scheduler); find the caller and route it through webkernel", ms, s_cur->id);
        webkernelTrace("emscripten_sleep refused", ms);
        return;
    }
    __real_emscripten_sleep(ms);
}

/* Busy-waits on state another thread changes (linked with -Wl,--wrap=alCSPGetState).
 * music.c musicTrack{1,2,3}Play: `while (alCSPGetState(seqp)) ;` waits for the audio thread to
 * process the stop event the game just queued. With real threads (native, N64) the audio thread
 * preempts the loop; with cooperative fibers the loop never gives it a turn and the page freezes
 * (G2's hang at the intro music start, after the Rareware logo). The wrapper counts the poll like
 * osGetCount does: after a run of polls the fiber parks until the next browser frame, the audio
 * thread runs on the next retrace, and the loop sees AL_STOPPED. Same value, same game code. */
struct ALCSPlayer_s;
int __real_alCSPGetState(struct ALCSPlayer_s *seqp);
int __wrap_alCSPGetState(struct ALCSPlayer_s *seqp)
{
    webkernelSpinCheck();
    return __real_alCSPGetState(seqp);
}
