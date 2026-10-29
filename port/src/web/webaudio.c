/*
 * webaudio.c — audio output for the web build (GE_WEB) through an AudioWorklet.
 *
 * SDL2's Emscripten audio backend plays through a ScriptProcessorNode (deprecated; it runs on the
 * page's main thread, so a long game frame starves it). port/src/audio.c keeps its push model
 * (SDL_QueueAudio of 22050 Hz s16 stereo blocks + SDL_GetQueuedAudioSize for the game's AI_LEN
 * regulator, D204); on the web those three calls come here instead (audio.c, GE_WEB block):
 *
 *   webAudioOpen      AudioContext + an AudioWorkletNode whose processor keeps a FIFO of the
 *                     source-rate frames and resamples them (linear) to the context rate;
 *   webAudioQueue     converts a block to Float32 and posts it to the processor (transferred);
 *   webAudioQueuedBytes  FIFO depth estimate in source bytes (frames * 4), as SDL reported it: the
 *                     processor reports its depth every 8 render quanta; frames posted since are added,
 *                     frames played since (context time) are subtracted;
 *   webAudioPause     suspends / resumes the context (hidden tab).
 *
 * The context starts "suspended" until the page has a user gesture (autoplay policy); a pointerdown
 * or keydown resumes it. GE_SDLAUDIO=1 (page &env=) keeps SDL's ScriptProcessor path instead.
 * Blocks are copied into a static buffer first: the game's buffers live in the DRAM arena above
 * 2 GiB, which the MEMORY64=2 glue cannot index (PLAN 2.1).
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "platform.h"
#include "system.h"

EM_JS(int, wa_open, (int srcRate), {
  const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
  if (!AC) return 0;
  const ctx = new AC({ latencyHint: 'interactive' });
  if (!ctx.audioWorklet) return 0;
  const W = { ctx, node: null, pending: [], src: srcRate, posted: 0, rep: 0, repPosted: 0, repTime: 0, ready: false };
  Module.geWebAudio = W;
  const proc = `
class GeOut extends AudioWorkletProcessor {
  constructor(o) {
    super(); this.q = []; this.off = 0; this.len = 0; this.pos = 0; this.step = o.processorOptions.src / sampleRate;
    this.prevL = 0; this.prevR = 0; this.n = 0; this.under = 0;
    this.port.onmessage = (e) => { this.q.push(e.data); this.len += e.data.length >> 1; this.started = true; };
  }
  next() {               // one source frame, or silence (underrun)
    while (this.q.length && this.off >= this.q[0].length) { this.q.shift(); this.off = 0; }
    if (!this.q.length) { if (this.started) this.under++; return false; }   // starved frames after the first block
    const b = this.q[0]; this.curL = b[this.off]; this.curR = b[this.off + 1]; this.off += 2; this.len--; return true;
  }
  process(inp, out) {
    const L = out[0][0], R = out[0][1] || out[0][0];
    if (this.cur === undefined) { this.cur = true; this.curL = 0; this.curR = 0; }
    for (let i = 0; i < L.length; i++) {
      this.pos += this.step;
      while (this.pos >= 1) { this.pos -= 1; this.prevL = this.curL; this.prevR = this.curR; if (!this.next()) { this.curL = 0; this.curR = 0; } }
      L[i] = this.prevL + (this.curL - this.prevL) * this.pos;
      R[i] = this.prevR + (this.curR - this.prevR) * this.pos;
    }
    if ((++this.n & 7) === 0) this.port.postMessage([this.len, currentTime, this.under]);
    return true;
  }
}
registerProcessor('ge-out', GeOut);`;
  const url = URL.createObjectURL(new Blob([proc], { type: 'text/javascript' }));
  ctx.audioWorklet.addModule(url).then(() => {
    const node = new AudioWorkletNode(ctx, 'ge-out', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
                                                      processorOptions: { src: srcRate } });
    node.port.onmessage = (e) => { W.rep = e.data[0]; W.repTime = e.data[1]; W.repPosted = W.posted; W.under = e.data[2]; };
    node.connect(ctx.destination);
    W.node = node; W.ready = true;
    for (const b of W.pending) node.port.postMessage(b, [b.buffer]);
    W.pending = [];
  }).catch((e) => { console.log('[webaudio] worklet failed: ' + e); });
  const resume = () => { if (ctx.state === 'suspended' && !W.paused) ctx.resume(); };
  for (const ev of ['pointerdown', 'keydown', 'touchend']) document.addEventListener(ev, resume, true);
  resume();
  return ctx.sampleRate | 0;
});

EM_JS(void, wa_queue, (int ptr, int nsamples), {
  const W = Module.geWebAudio; if (!W) return;
  const f = new Float32Array(nsamples), h = HEAP16, base = ptr >> 1;
  for (let i = 0; i < nsamples; i++) f[i] = h[base + i] / 32768;
  W.posted += nsamples >> 1;
  if (W.ready) W.node.port.postMessage(f, [f.buffer]); else W.pending.push(f);
});

EM_JS(int, wa_queued_frames, (void), {
  const W = Module.geWebAudio; if (!W) return 0;
  if (!W.ready) return W.posted;
  let n = W.rep + (W.posted - W.repPosted);
  if (W.ctx.state === 'running') n -= Math.max(0, (W.ctx.currentTime - W.repTime) * W.src);
  return Math.max(0, Math.round(n));
});

EM_JS(void, wa_pause, (int paused), {
  const W = Module.geWebAudio; if (!W) return;
  W.paused = !!paused;
  if (paused) W.ctx.suspend(); else W.ctx.resume();
});

static int s_open;

/* Returns the context rate (> 0) or 0 when AudioWorklet is unavailable (caller falls back to SDL). */
int webAudioOpen(int srcRate)
{
    int rate = wa_open(srcRate);
    if (rate > 0) {
        s_open = 1;
        sysLogPrintf(LOG_INFO, "audio: AudioWorklet output, context %d Hz, source %d Hz", rate, srcRate);
    }
    return rate;
}

void webAudioQueue(const void *buf, uint32_t len)
{
    static int16_t staging[16384 * 2];
    if (!s_open || !buf || !len) return;
    if (len > sizeof(staging)) len = sizeof(staging);
    memcpy(staging, buf, len);
    wa_queue((int)(uintptr_t)staging, (int)(len / 2));
}

uint32_t webAudioQueuedBytes(void)
{
    return s_open ? (uint32_t)wa_queued_frames() * 4u : 0u;
}

void webAudioPause(int paused)
{
    if (s_open) wa_pause(paused);
}
