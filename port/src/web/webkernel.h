/*
 * webkernel.h — cooperative libultra threads for the web build (GE_WEB).
 *
 * The native port runs every game thread as a real pthread (port/src/libultra.c).
 * A browser page has one main thread that must return to the event loop every
 * frame, so on the web each game thread is an Emscripten fiber (ASYNCIFY) and a
 * small priority scheduler runs them from the requestAnimationFrame callback.
 *
 * libultra.c keeps its own queue/thread code; on the web its blocking points
 * map onto this API (see the GE_WEB block at the top of libultra.c):
 *   pthread_cond_wait   -> webkernelWait()    block until some queue changes
 *   pthread_cond_signal -> webkernelSignal()  a queue changed (wakes waiters,
 *                                             preempts like libultra would)
 *   pthread_create      -> webkernelSpawn()
 *   the VI tick thread  -> the per-frame retrace callback (webkernelMainLoop)
 */
#ifndef PORT_WEB_WEBKERNEL_H
#define PORT_WEB_WEBKERNEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start a fiber for a game thread. prio points at the OSThread's priority
 * (read live, so osSetThreadPri is honoured). */
void webkernelSpawn(void (*entry)(void *), void *arg, const int *prio, int id);
/* Block the current fiber until webkernelSignal() is called (by anyone).
 * `what` identifies the wait for diagnostics (libultra.c passes the queue's
 * condition variable; portWebDescribeWait() turns it back into a queue). */
void webkernelWait(const void *what);
/* Something a blocked thread may wait for changed. Preempts the current fiber
 * if a higher-priority thread was waiting (libultra osSendMesg semantics). */
void webkernelSignal(void);
/* osYieldThread: let other ready threads of the same or higher priority run. */
void webkernelYield(void);
/* Called from osGetCount(): a thread that polls the clock many times in a row
 * without touching a queue is busy-waiting (frametiming.c waitForNextFrame);
 * park it until the next browser frame so the page does not hang. */
void webkernelSpinCheck(void);
/* 1 when running on a game fiber (not the browser's main context). */
int webkernelInFiber(void);
/* Install the main loop and return (main() then returns to the browser).
 * Every animation frame: pump(), then retrace() once per due console retrace
 * (period_us, catch-up capped), then run the ready fibers until all block. */
void webkernelMainLoop(void (*pump)(void), void (*retrace)(void), unsigned period_us);

/* libultra.c: a gfx task finished (pacing report). */
void webkernelFrameRendered(void);

/* Debug trace point: logged from frame GE_WKTRACE on, always kept in a ring. */
void webkernelTrace(const char *tag, uintptr_t arg);

/* Provided by libultra.c: describe a wait object for the stall report. */
void portWebDescribeWait(const void *what, char *buf, int len);

#ifdef __cplusplus
}
#endif

#endif
