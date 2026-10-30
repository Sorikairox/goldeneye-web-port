/*
 * webnet.h — online multiplayer, game side (G6, docs/web/ONLINE.md).
 *
 * Deterministic input lockstep: every peer runs the same wasm on its own ROM from power-on and only
 * controller inputs travel. This header is the port-side seam: webkernel.c (retrace loop, barrier),
 * libultra.c (det clock, controller reads, EEPROM sandbox), audio.c (virtual AI queue), input.c
 * (pure N64 pad), config.c / optionsoverlay.c / webpage.c (session rules). With no session every
 * function here is inert and the callers take their original paths (geNetDetOn == 0).
 * Kept free of game headers (see webpage.c for why).
 */
#ifndef PORT_WEB_WEBNET_H
#define PORT_WEB_WEBNET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while a lockstep session (or an offline det run / recording) runs: set by the page before main(). */
extern int geNetDetOn;       /* deterministic mode: det clock, retrace-indexed fibers, virtual AI queue */
extern int geNetSessionOn;   /* controller input comes from the input ring (implies geNetDetOn) */

/* det clock: retrace x quantum + a small tick per read (osGetCount / osGetTime) */
uint32_t geNetDetCount(int bump);
uint64_t geNetDetUs(int bump);

/* webkernel.c, around every retrace: sample the local input for N + D / hash the state */
void geNetRetraceStart(void);
void geNetRetraceDone(void);
unsigned geNetRetrace(void);          /* the retrace running (or last run) */
int geNetReady(unsigned f);           /* the frame barrier: may retrace f run? */
void geNetStalled(void);
double geNetTakeSlowMs(double elapsedMs);   /* time sync: wall time to take out of the 50 Hz backlog */

/* libultra.c controller read: this retrace's pad of channel ch; returns 1 if ch is in the session */
int geNetPad(int ch, unsigned *buttons, int *stickX, int *stickY);
int geNetMask(void);

/* libultra.c EEPROM: the session's image (host's, or blank); never persisted */
int geNetEepromImage(uint8_t *dst, int len);

/* audio.c: virtual AI queue drained by retrace (the game's AI_LEN never depends on the browser) */
void geNetAiPush(uint32_t len, int limitFrames);
uint32_t geNetAiQueued(void);
uint32_t geNetAiLast(void);

/* main.c, after configLoad(): the session's fixed option values (see webnet.c) */
void geNetApplySessionConfig(void);

#ifdef __cplusplus
}
#endif

#endif
