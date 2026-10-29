// PROBE_EVAL expression (scripts/web/probe.mjs): is SDL's Web Audio output running and non-silent?
// Taps the ScriptProcessor with an AnalyserNode for 1 s; returns context state, sample rate and peak level.
(async () => {
  const S = window.Module && Module.SDL2; if (!S || !S.audioContext) return 'no SDL2 audio context';
  const ctx = S.audioContext, node = S.audio && S.audio.scriptProcessorNode;
  if (!node) return `context ${ctx.state}, no output node`;
  const an = ctx.createAnalyser(); an.fftSize = 2048; node.connect(an);
  const buf = new Float32Array(an.fftSize); let peak = 0;
  for (let i = 0; i < 20; i++) { await new Promise((r) => setTimeout(r, 50)); an.getFloatTimeDomainData(buf); for (const v of buf) peak = Math.max(peak, Math.abs(v)); }
  node.disconnect(an);
  return `context ${ctx.state} ${ctx.sampleRate} Hz, t=${ctx.currentTime.toFixed(1)} s, peak ${peak.toFixed(3)}`;
})()
