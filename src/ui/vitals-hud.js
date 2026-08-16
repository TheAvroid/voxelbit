  // ── HURT FLASH ── all that is left of the vitals HUD. The five hunger and five heart voxels were built
  // here and then removed at the user's request (2026-08-15): "remove the hunger/hearts ui. keep the
  // mechanics, just remove the ui." The mechanics are untouched and still tick in sim/vitals.js — health,
  // hunger, saturation, exhaustion, regen and starvation all run exactly as before, they are simply not
  // drawn. `__vb.vit()` is now the only way to read them, which is what the tests use.
  const hurtEl = $('hurtFx');
  const hurtCtx = hurtEl ? hurtEl.getContext('2d') : null;

  // ── THE HURT SCREEN ── drawn at 64x36 and stretched over the window by CSS. That is what makes it VOXELISED:
  // every block is one canvas pixel blown up to ~30 px on screen, so the edges are genuinely hard. Painting a
  // full-resolution red vignette and calling it pixel art would have given soft edges at any window size.
  const HURT_W = 64, HURT_H = 36;
  function hurtScreen() {
    if (!hurtCtx) return;
    if (hurtEl.width !== HURT_W) { hurtEl.width = HURT_W; hurtEl.height = HURT_H; }
    hurtCtx.clearRect(0, 0, HURT_W, HURT_H);
    // A vignette in BLOCKS: distance from centre decides how likely a block is to be painted at all, so the
    // red gathers at the edges and breaks up into scattered voxels as it reaches inward — a dither, not a fade.
    for (let y = 0; y < HURT_H; y++) {
      for (let x = 0; x < HURT_W; x++) {
        const nx = (x / (HURT_W - 1)) * 2 - 1, ny = (y / (HURT_H - 1)) * 2 - 1;
        const d = Math.sqrt(nx * nx * 0.85 + ny * ny);
        // STARTS AT 0.78, NOT 0.42. The first cut painted from 42% of the way out, which on a wide window is
        // most of the screen — a screenshot showed the scene almost entirely behind red blocks. A hurt cue has
        // to frame the view, never obscure it, so only the outer fifth paints and it stays under half opacity.
        const a = (d - 0.78) / 0.5;
        if (a <= 0) continue;
        if (Math.random() > Math.min(1, a * a * 1.1)) continue;
        hurtCtx.fillStyle = 'rgba(' + (150 + ((Math.random() * 60) | 0)) + ',10,14,' + Math.min(0.5, 0.12 + a * 0.42).toFixed(3) + ')';
        hurtCtx.fillRect(x, y, 1, 1);
      }
    }
    hurtEl.classList.remove('hidden');
    // RESTART the animation: dropping the class, forcing a reflow, then re-adding it is what makes a repeat
    // hit replay the flash. Without the reflow the browser sees no class change at all and nothing happens.
    hurtEl.classList.remove('on');
    void hurtEl.offsetWidth;
    hurtEl.classList.add('on');
  }
