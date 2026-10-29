/*
 * webpage.c — page integration for the web build (GE_WEB): tab visibility and page unload.
 *
 * Unload: SDL2's Emscripten backend registers a beforeunload callback that returns "" (a char *).
 * With -sMEMORY64=2 the glue gets that pointer back as a BigInt and passes it to UTF8ToString, which
 * mixes BigInt and Number and throws "TypeError: Cannot mix BigInt and other types" while the page
 * closes. SDL's callback had already sent SDL_APP_TERMINATING, but nothing could act on it. This file
 * replaces SDL's callback with one that returns NULL (no confirmation dialog, nothing for the glue to
 * convert), raises SDL_APP_TERMINATING for event watches and runs the native clean-exit work that can
 * run synchronously (main.c portAtExit: stop pad rumble, write the config). EEPROM saves are written
 * when the game saves (geEepromRW), not at exit.
 *
 * Visibility: SDL turns visibilitychange into SDL_WINDOWEVENT_HIDDEN / _SHOWN. An event watch (runs
 * synchronously inside the browser event) pauses the scheduler and the console clock
 * (webkernelSetPaused) and the audio device while the tab is hidden, and resumes both when it is shown.
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <stdint.h>
/* not <emscripten/html5.h>: it pulls in pthread.h -> the port's sched.h shim -> game headers whose
 * `typedef s32 bool` clashes with <stdbool.h>. The one html5 call used here, declared as in html5.h
 * (thread 1 = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD): */
typedef const char *(*web_beforeunload_cb)(int eventType, const void *reserved, void *userData);
int emscripten_set_beforeunload_callback_on_thread(void *userData, web_beforeunload_cb cb, uintptr_t thread);
#define emscripten_set_beforeunload_callback(ud, cb) emscripten_set_beforeunload_callback_on_thread((ud), (cb), 1)
#include <SDL.h>

#include "webkernel.h"
#include "platform.h"
#include "system.h"
/* declared here rather than via the port headers (kept free of game headers, see above) */
void audioSetPaused(int paused);
void configSave(void);
void inputRumbleStopAll(void);

static const char *webBeforeUnload(int eventType, const void *reserved, void *userData)
{
    (void)eventType; (void)reserved; (void)userData;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_APP_TERMINATING;
    SDL_PushEvent(&e);           /* event watches see it now; the queue is never pumped again */
    inputRumbleStopAll();
    configSave();
    return NULL;                 /* NULL: no "leave page?" dialog, and no pointer for the glue */
}

static int SDLCALL webEventWatch(void *userData, SDL_Event *ev)
{
    (void)userData;
    if (ev->type == SDL_WINDOWEVENT) {
        if (ev->window.event == SDL_WINDOWEVENT_HIDDEN) {
            webkernelSetPaused(1);
            audioSetPaused(1);
        } else if (ev->window.event == SDL_WINDOWEVENT_SHOWN) {
            audioSetPaused(0);
            webkernelSetPaused(0);
        }
    }
    return 1;
}

/* After videoInit (SDL's own page handlers are registered by then). */
void webPageInit(void)
{
    emscripten_set_beforeunload_callback(NULL, NULL);            /* removes SDL's handler */
    emscripten_set_beforeunload_callback(NULL, webBeforeUnload);
    SDL_AddEventWatch(webEventWatch, NULL);
}

/* Page test hook (main.js ?pausetest / probe): pause or resume as a hidden tab would. */
EMSCRIPTEN_KEEPALIVE void webPageSetHidden(int hidden)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_WINDOWEVENT;
    e.window.event = hidden ? SDL_WINDOWEVENT_HIDDEN : SDL_WINDOWEVENT_SHOWN;
    webEventWatch(NULL, &e);
}
