// PROBE_EVAL expression (scripts/web/probe.mjs): is the page's Web Audio output running and non-silent?
// Taps the output node (the AudioWorklet of port/src/web/webaudio.c, or SDL's ScriptProcessor with
// &env=GE_SDLAUDIO=1) with an AnalyserNode for 1 s; returns path, context state, rate, peak level
// (and worklet underruns / queue depth).
(async () => {
  let ctx, node, path, extra = '';
  const W = window.Module && Module.geWebAudio;
  if (W) { ctx = W.ctx; node = W.node; path = 'worklet'; }
  else {
    const S = window.Module && Module.SDL2; if (!S || !S.audioContext) return 'no audio context';
    ctx = S.audioContext; node = S.audio && S.audio.scriptProcessorNode; path = 'scriptprocessor';
  }
  if (!node) return `${path}: context ${ctx.state}, no output node`;
  const an = ctx.createAnalyser(); an.fftSize = 2048; node.connect(an);
  const buf = new Float32Array(an.fftSize); let peak = 0;
  for (let i = 0; i < 20; i++) { await new Promise((r) => setTimeout(r, 50)); an.getFloatTimeDomainData(buf); for (const v of buf) peak = Math.max(peak, Math.abs(v)); }
  node.disconnect(an);
  if (W) extra = `, underruns ${W.under}, fifo ${W.rep} frames`;
  return `${path}: context ${ctx.state} ${ctx.sampleRate} Hz, t=${ctx.currentTime.toFixed(1)} s, peak ${peak.toFixed(3)}${extra}`;
})()
