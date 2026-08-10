  // ── WebGPU init ────────────────────────────────────────────────────────────
  if (!navigator.gpu) fail('WebGPU not available — use Chrome/Edge 113+');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) fail('no WebGPU adapter');
  const SIZE384 = 768 * 768 * 384, SIZE256 = 768 * 768 * 256;   // the minimum deep world wants 226 MB; fall back gracefully
  let lim = Math.min(adapter.limits.maxStorageBufferBindingSize, adapter.limits.maxBufferSize);
  // DEV: ?worldcap=<bytes> pretends the adapter is smaller, so the weak-device fallback ladder
  // (WYpick 384/256/192, WXZ 2048/1536/1280/1024/768) can actually be exercised and measured on a
  // big GPU. The game now ships to players' own machines, so those paths are shipping paths.
  { const m = /worldcap=(\d+)/.exec(location.search); if (m) lim = Math.min(lim, +m[1]); }
  const WYpick = lim >= SIZE384 ? 384 : (lim >= SIZE256 ? 256 : 192);
  let WXZ = 768;                                       // widest window the adapter can bind: 2048 (1.5 GB) → a TRUE 100 m view radius
  if (WYpick === 384) for (const c of [2048, 1536, 1280, 1024]) if (lim >= c * c * 384) { WXZ = c; break; }
  const WBYTES = WXZ * WXZ * WYpick;
  const PROF = adapter.features.has('timestamp-query');   // per-pass GPU timing (dev __vb.prof(); harmless when unread)
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
