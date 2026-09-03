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
  // ── SAFE MODE ── the same ladder, made PERSISTENT, and it exists because of a bug report the page could
  // not answer: "gpu device lost (unknown) — reload the page" on a gaming laptop, with no way to reach the
  // machine. `unknown` is the driver saying it reset the device or could not meet an allocation, and the only
  // lever a web page has in either case is TO ASK FOR LESS. vb_safe is a level 0-3 written by the banner's own
  // button (see core/telemetry.js), so a player who cannot hold the full window can step down to one that fits
  // without a console, a flag or a build:
  //   1  view distance x1 — keeps the 2048 window, pool 1.05 GB -> 402 MB. Halves the VRAM, costs 200 m -> 100 m.
  //   2  …and a 1536 window — pool 226 MB, view 75 m.
  //   3  …and a 1024 window + a render-scale ceiling (see render/targets.js) — the floor of the ladder.
  // NEVER ARMED AUTOMATICALLY. A device lost once to a driver update, an Optimus/MUX switch or a laptop waking
  // from sleep must not silently cost every later session its view distance; the player presses the button, and
  // __vb.safeMode(0) (or the banner's reset) hands the quality straight back.
  let SAFE = 0;
  try { SAFE = Math.max(0, Math.min(3, +localStorage.getItem('vb_safe') | 0)); } catch (e) {}
  if (SAFE >= 2) lim = Math.min(lim, SAFE >= 3 ? 402653184 : 905969664);   // 1024 and 1536 windows — the rungs of the WXZ ladder below, named as the byte caps it tests
  let WYpick = lim >= SIZE384 ? 384 : (lim >= SIZE256 ? 256 : 192);
  let WXZ = 768;                                       // widest window the adapter can bind: 2048 (1.5 GB) → a TRUE 100 m view radius
  if (WYpick === 384) for (const c of [2048, 1536, 1280, 1024]) if (lim >= c * c * 384) { WXZ = c; break; }
  // ══ AND THEN THE CEILING GOES UP, BUT ONLY WITH WHATEVER IS LEFT OVER (user 2026-09-03: the pine tips are
  // cut off) ══ the nine pines were re-baked from 151-152 voxels to 225-228, and the TREE RESERVE that pins
  // HMAX (world/window.js) was sized for the old ones: a trunk standing above WY - 228 has its crown written
  // straight past the top of the world array and comes out flat. Measured before this: 20.4% of pine columns
  // clipped, worst case 32 voxels off the tip.
  // WHY THE ORDER MATTERS, AND WHY THIS LOOP IS AFTER THE WXZ ONE. The obvious fix is a 512 rung in WYpick
  // above, and it is a trap: WYpick is chosen FIRST, so a taller world makes every rung of the WXZ ladder
  // harder and the window silently falls 2048 -> 1536. That is the view radius, 200 m -> 150 m, paid to fix
  // a treetop. Choosing WXZ first and spending only the REMAINDER on height means the window can never
  // regress -- a machine that cannot afford a taller world simply keeps the one it has.
  // WHY 480 AND NOT 512, WHICH IS THE NUMBER ANYONE WOULD REACH FOR: 2048*2048*512 is 2,147,483,648 bytes and
  // maxStorageBufferBindingSize on the measured adapter is 2,147,483,644. It misses by FOUR BYTES, and the
  // 2 GB cap is a common WebGPU ceiling rather than one machine's quirk. 480 fits at 1920 MB with headroom.
  // WHAT IT BUYS: at WY 480 the reserve can be the 234 the new pines need AND HMAX still lands at 246, so the
  // hills go 118 -> 138 voxels above water. This is not a trade against terrain height, it is more of both.
  // The rungs step by 32 (a multiple of the 8-voxel brick) so a machine that cannot carry 480 can still take
  // 448 or 416 and clip less rather than nothing.
  if (WYpick === 384) for (const y of [480, 448, 416]) if (lim >= WXZ * WXZ * y) { WYpick = y; break; }
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
  // ── 0.17 * 512 -> 0.30 * 272 (2026-09-03) ── a page is a 16-entry palette and 512 four-bit indices now,
  // so the same bytes hold 1.76x the bricks; see the PAGE FORMAT note in render/buffers.js. Both numbers
  // move together and BOTH must match it — the fraction AND the stride — or this asks the adapter for a
  // binding of the wrong size and the pool silently allocates short.
  const GPOOL2S = Math.ceil((WXZ * 2 >> 3) * (WYpick >> 3) * (WXZ * 2 >> 3) * 0.30);
  const GPOOL2 = GPOOL2S * 272 + Math.ceil(GPOOL2S * 0.025) * 512;   // the GMUL-2 pool, in bytes - must match POOL_FRAC_RING + PAGE_B + DENSE_PAGES in render/buffers.js
  // ── AND IT IS BACK ON, ADAPTER-GATED (2026-08-30) ── the far ring was switched off earlier the same day
  // because it collapsed under movement. Three bugs, all now fixed and re-measured under sustained flight:
  // the ring re-fetched every tile the near window had covered instead of ADOPTING its still-valid pages;
  // poolFlush early-returned whenever the near window was quiet, so the ring's DESCRIPTORS never reached the
  // GPU while its pages did; and STONE_ID pointed at a palette slot that is now cherry blossom, which is what
  // made the resulting stale sealed-page slab bright pink.
  // Measured after: the filled radius HOLDS at 1920 through sprint-speed flight (it used to fall to ~1050 and
  // oscillate), zero overflow, and the far terrain renders real ground all the way out.
  // ── `WYpick === 384` -> `>= 384` (2026-09-03) ── that clause was shorthand for "this machine is carrying
  // the full window", written when 384 was the only tall rung there was. The moment the ceiling could go to
  // 480 it started meaning the opposite: a machine that had MORE memory, not less, failed the test and had
  // its far ring switched off — GMUL fell 2 -> 1 and the view distance halved as a side effect of a fix that
  // was specifically trying to protect it. The real test is the one standing next to it, `lim >= GPOOL2*1.15`,
  // and it needs no help: GPOOL2 is sized off `WYpick >> 3`, so a taller world already asks for a
  // proportionally bigger ring pool (1281 MB at 480 against 1026 at 384) and a device that cannot bind it
  // still steps down on its own. WXZ === 2048 stays — a machine that fell down the WIDTH ladder has its own
  // reasons, and that is the case the original note is really about.
  let GMUL_PICK = (WXZ === 2048 && WYpick >= 384 && lim >= GPOOL2 * 1.15) ? 2 : 1;
  if (SAFE >= 1) GMUL_PICK = 1;                        // BEFORE the ?gmul override, so an explicit flag still wins for testing on a safe-moded profile
  { const m = /[?&]gmul=(\d+)/.exec(location.search); if (m) GMUL_PICK = Math.max(1, Math.min(2, +m[1] | 0)); }
  if (GMUL_PICK > 1) console.log('[vb] view distance x' + GMUL_PICK + ' - far ring pool', (GPOOL2 / 1048576) | 0, 'MB of', (lim / 1048576) | 0, 'MB bindable');
  const WBYTES = WXZ * WXZ * WYpick;
  // ── WHAT THIS MACHINE ACTUALLY PICKED ── on window, not just in the console, because the one machine that
  // needs it is somebody else's. The device-lost banner prints it, and a bug report that carries it says which
  // rung of the ladder was in play — which is the difference between "the driver reset" and "we asked for
  // 1.05 GB it did not have".
  window.__vbTier = { wxz: WXZ, wy: WYpick, gmul: GMUL_PICK, safe: SAFE, bindMB: (lim / 1048576) | 0,
    adapterMB: (Math.min(adapter.limits.maxStorageBufferBindingSize, adapter.limits.maxBufferSize) / 1048576) | 0 };
  // ── STEPPING DOWN THE LADDER BY ITSELF, AND ONLY AT BOOT ── the banner's button is for a player who saw the
  // crash; this is for the machine that never gets that far. A device that dies WHILE LOADING is not a driver
  // blip you can shrug off and carry on from — it is this machine saying it cannot hold what we just asked for,
  // every time, forever, and reloading unchanged asks for exactly the same thing again. There is no session to
  // lose and nothing to preserve, so it drops a rung and reloads itself.
  // MID-SESSION IS THE OPPOSITE CASE and must stay manual: a driver update, an Optimus/MUX switch or a laptop
  // waking from sleep kills a device that was running fine, and silently costing that player their view distance
  // for the rest of time would be the wrong read of a one-off.
  // BOUNDED BY sessionStorage, NOT localStorage: the counter has to survive the reload it is about to trigger
  // and die with the tab, or a machine that has one bad day boots at the bottom of the ladder for ever. Three
  // steps is the whole ladder; after that it gives up and lets the banner explain itself.
  const safeStepDown = (why) => {
    let n = 0;
    try { n = +sessionStorage.getItem('vb_safeAuto') || 0; } catch (e) {}
    if (SAFE >= 3 || n >= 3) return false;
    try { sessionStorage.setItem('vb_safeAuto', String(n + 1)); localStorage.setItem('vb_safe', String(SAFE + 1)); } catch (e) { return false; }
    console.warn('[vb] ' + why + ' - retrying at safe level ' + (SAFE + 1));
    try { loadMsgEl.textContent = 'not enough graphics memory - retrying smaller'; } catch (e) {}
    setTimeout(() => location.reload(), 1200);         // long enough to READ, short enough that it still feels like loading rather than a hang
    return true;
  };
  const PROF = adapter.features.has('timestamp-query');   // per-pass GPU timing (dev __vb.prof(); harmless when unread)
  // A device gets DEFAULT limits unless it asks for more, whatever the adapter can do. TRACE already
  // binds exactly 8 storage buffers and 8 is the WebGPU default cap, so a 9th (the paged-storage
  // descriptor table) fails pipeline creation SILENTLY: black frame, absurd fps, nothing on __vbErr.
  // ── AND IT ASKS FOR WHAT IT BINDS ── this used to require WBYTES, the size of the DENSE GPU WORLD, and there
  // has not been one since the paged brick pool replaced it (see the top of render/buffers.js). At a 2048 window
  // that asked every player's driver for a 1.61 GB binding when the largest buffer the game creates is the pool
  // at 1.04 GB - half a gigabyte of limit nothing was ever going to use, on the machines least able to spare it.
  // GPOOL1 mirrors POOL_FRAC in render/buffers.js exactly as GPOOL2 above mirrors POOL_FRAC_RING, and for the
  // same reason: the pool has to be SIZED before the fragment that sizes it runs. Both are noted there too.
  const GPOOL1S = Math.ceil((WXZ >> 3) * (WYpick >> 3) * (WXZ >> 3) * 0.44);
  const GPOOL1 = GPOOL1S * 272 + Math.ceil(GPOOL1S * 0.025) * 512;   // mirrors POOL_FRAC + PAGE_B, as GPOOL2 does
  const BIGBUF = Math.max(GMUL_PICK > 1 ? GPOOL2 : GPOOL1, 268435456);   // 256 MB floor: the default maxBufferSize, so this never asks for LESS than a device already gives
  const device = await adapter.requestDevice({
    requiredLimits: { maxStorageBufferBindingSize: BIGBUF, maxBufferSize: BIGBUF },
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
