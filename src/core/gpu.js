  // ── WebGPU init ────────────────────────────────────────────────────────────
  if (!navigator.gpu) fail('WebGPU not available — use Chrome/Edge 113+');
  // ?igpu — boot this instance on the INTEGRATED adapter (UHD 770) instead of the discrete card, so an AI/CDP test
  // browser stops competing with the played game for the 4070. Whole-flag match, never includes(): ?igpu would
  // otherwise also fire on a future ?noigpu, which is the exact trap the ?uni/?nouni note in held-items.js records.
  const IGPU_ADAPTER = /[?&]igpu\b/.test(location.search);
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: IGPU_ADAPTER ? 'low-power' : 'high-performance' });
  if (!adapter) fail('no WebGPU adapter');
  // powerPreference is only a HINT. If the hint is ignored the test silently runs on the 4070 and the whole
  // point is lost, so record what we actually got — assertable from a test, the way __vbGpuErr is.
  window.__vbAdapter = Object.assign({ requested: IGPU_ADAPTER ? 'low-power' : 'high-performance' }, adapter.info ? { vendor: adapter.info.vendor, architecture: adapter.info.architecture, device: adapter.info.device, description: adapter.info.description } : {});
  if (IGPU_ADAPTER) console.warn('[vb] ?igpu → adapter:', JSON.stringify(window.__vbAdapter));
  const SIZE384 = 768 * 768 * 384, SIZE256 = 768 * 768 * 256;   // the minimum deep world wants 226 MB; fall back gracefully
  let lim = Math.min(adapter.limits.maxStorageBufferBindingSize, adapter.limits.maxBufferSize);
  // DEV: ?worldcap=<bytes> pretends the adapter is smaller, so the weak-device fallback ladder
  // (WYpick 384/256/192, WXZ 2048/1536/1280/1024/768) can actually be exercised and measured on a
  // big GPU. The game now ships to players' own machines, so those paths are shipping paths.
  { const m = /worldcap=(\d+)/.exec(location.search); if (m) lim = Math.min(lim, +m[1]); }
  const WYpick = lim >= SIZE384 ? 384 : (lim >= SIZE256 ? 256 : 192);
  let WXZ = 768;                                       // widest window the adapter can bind: 2048 (1.5 GB) → a TRUE 100 m view radius
  if (WYpick === 384) for (const c of [2048, 1536, 1280, 1024]) if (lim >= c * c * 384) { WXZ = c; break; }
  // ── HOW MUCH WIDER THE GPU WINDOW IS THAN THE CPU ONE ── the full argument is at TWO WINDOWS in
  // world/window.js. Chosen HERE because it is an adapter question, alongside WXZ and WYpick, and because
  // world/window.js sizes its arrays off it and runs after this fragment.
  // DEFAULT 1 UNTIL THE RING IS MEASURED, not because 2 does not work: at GMUL 2 the far ring is three times
  // the near window's area, and the pool has to be SIZED for it up front (a storage buffer cannot grow), so
  // the allocation is the number that decides whether a machine can run it — not the fraction of it that ends
  // up occupied. ?gmul=2 is how that gets exercised and measured before it is anyone's default.
  // THE TEST IS THE POOL'S OWN ALLOCATION, because that is the thing that actually has to bind. At GMUL 2 the
  // GPU grid is four times the bricks and the pool is sized for it up front (a storage buffer cannot grow):
  // ~1.05 GB against 402 MB at GMUL 1, measured. So the requirement is a device that can bind that with room
  // to spare AND is already carrying the full 2048x384 window - a machine that fell down the WXZ ladder has
  // its own reasons and must not then be handed four times the memory for a longer view it cannot afford.
  // A LADDER, NOT A CONSTANT: capable machines get 200 m, everything else keeps the 100 m it has today, the
  // same way WXZ already walks 2048/1536/1280/1024/768. ?gmul=1 forces it off, ?gmul=2 forces it on.
  const GPOOL2 = Math.ceil((WXZ * 2 >> 3) * (WYpick >> 3) * (WXZ * 2 >> 3) * 0.17) * 512;   // the GMUL-2 pool, in bytes - must match POOL_FRAC_RING in render/buffers.js
  // ── AND IT IS BACK ON, ADAPTER-GATED (2026-08-30) ── the far ring was switched off earlier the same day
  // because it collapsed under movement. Three bugs, all now fixed and re-measured under sustained flight:
  // the ring re-fetched every tile the near window had covered instead of ADOPTING its still-valid pages;
  // poolFlush early-returned whenever the near window was quiet, so the ring's DESCRIPTORS never reached the
  // GPU while its pages did; and STONE_ID pointed at a palette slot that is now cherry blossom, which is what
  // made the resulting stale sealed-page slab bright pink.
  // Measured after: the filled radius HOLDS at 1920 through sprint-speed flight (it used to fall to ~1050 and
  // oscillate), zero overflow, and the far terrain renders real ground all the way out.
  let GMUL_PICK = (WXZ === 2048 && WYpick === 384 && lim >= GPOOL2 * 1.15) ? 2 : 1;
  { const m = /[?&]gmul=(\d+)/.exec(location.search); if (m) GMUL_PICK = Math.max(1, Math.min(2, +m[1] | 0)); }
  if (GMUL_PICK > 1) console.log('[vb] view distance x' + GMUL_PICK + ' - far ring pool', (GPOOL2 / 1048576) | 0, 'MB of', (lim / 1048576) | 0, 'MB bindable');
  const WBYTES = WXZ * WXZ * WYpick;
  const PROF = adapter.features.has('timestamp-query');   // per-pass GPU timing (dev __vb.prof(); harmless when unread)
  // A device gets DEFAULT limits unless it asks for more, whatever the adapter can do. TRACE already
  // binds exactly 8 storage buffers and 8 is the WebGPU default cap, so a 9th (the paged-storage
  // descriptor table) fails pipeline creation SILENTLY: black frame, absurd fps, nothing on __vbErr.
  const device = await adapter.requestDevice({
    ...(WBYTES > (1 << 27) ? { requiredLimits: { maxStorageBufferBindingSize: WBYTES, maxBufferSize: Math.max(WBYTES, 268435456) } } : {}),
    ...(PROF ? { requiredFeatures: ['timestamp-query'] } : {}) });
  if (WYpick < 384 || WXZ < 2048) console.warn('[vb] adapter caps buffers at', lim, '— window', WXZ, '×', WYpick);
  // GPU errors are also RECORDED, not just logged. A shader that fails to compile makes every command
  // buffer it is encoded into invalid, so the whole frame is silently dropped — the picture keeps moving
  // (stale swapchain) while the world quietly stops updating. Console-only reporting let exactly that hide;
  // __vbGpuErr makes it assertable from a test. Deduped + capped so a per-frame error can't grow unbounded.
  window.__vbGpuErr = [];
  device.addEventListener('uncapturederror', (e) => {
    const m = e.error.message;
    console.error('[vb] gpu error:', m);
    const k = m.slice(0, 160), hit = window.__vbGpuErr.find((q) => q.msg === k);
    if (hit) hit.n++; else if (window.__vbGpuErr.length < 24) window.__vbGpuErr.push({ msg: k, n: 1 });
  });
