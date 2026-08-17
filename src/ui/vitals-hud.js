  // ── HURT FLASH ── all that is left of the vitals HUD. The five hunger and five heart voxels were built
  // here and then removed at the user's request (2026-08-15): "remove the hunger/hearts ui. keep the
  // mechanics, just remove the ui." The mechanics are untouched and still tick in sim/vitals.js — health,
  // hunger, saturation, exhaustion, regen and starvation all run exactly as before, they are simply not
  // drawn. `__vb.vit()` is now the only way to read them, which is what the tests use.
  // ── THE HURT FLASH LIVES IN THE RENDER NOW (user 2026-08-16: "when recording a video on playback, the
  // red pixels on the ui dont show up. make them showup on playback when recording the scree.") ──
  // It was a DOM <canvas id="hurtFx"> laid over the game at 64x36 and blown up by image-rendering:pixelated,
  // faded by a CSS keyframe. That is correct on screen and INVISIBLE IN A RECORDING: veStartRec captures the
  // WebGPU canvas with canvas.captureStream(60), and a DOM element stacked on top of that canvas is simply not
  // part of the captured surface. Nor could it be composited in — drawImage of this canvas reads back all zero
  // (the reason every screenshot in testing goes through CDP). The ONLY place a screen effect can be both seen
  // and recorded is inside the image itself, so the same 64x36 grid, the same dither, the same reds and the same
  // 0.42 s fade are now drawn by BLIT (see the hurt-flash block there). This file keeps the TIMING and the SEED,
  // which is all that is left of it on the JS side; tick-camera publishes both into u.hurtV every frame.
  const HURTV = { t0: -1e9, seed: 1 };
  const HURTV_MS = 420;                      // the CSS keyframe's own 0.42 s, kept to the millisecond
  const HURTV_STEPS = 10;                    // …stepped to 24 fps like every other animation in this game: 0.42 s x 24 = 10 frames
  // Restarting is a bare timestamp write, so a hit landing mid-flash simply starts the fade again — which is what
  // the remove -> reflow -> add class dance existed to force out of CSS. The bug that trick was fixing (the flash
  // firing on the FIRST hit only, because two class toggles inside two frames coalesce into no change at all)
  // cannot come back here: there is no style engine to coalesce anything.
  function hurtScreen() { HURTV.t0 = performance.now(); HURTV.seed = 1 + ((Math.random() * 4093) | 0); }   // a fresh dither per hit, exactly as the old canvas re-painted itself per hit
  // 1 at the hit, 0 at 0.42 s, STEPPED to 24 fps on the way down.
  const hurtVig = () => { const k = 1 - (performance.now() - HURTV.t0) / HURTV_MS;
    return k <= 0 ? 0 : Math.round(Math.min(1, k) * HURTV_STEPS) / HURTV_STEPS; };
