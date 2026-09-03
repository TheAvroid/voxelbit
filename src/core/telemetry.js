  // ── FREEZE FORENSICS ── (user: "sometimes the game freezes") every way the render loop can die is now caught,
  // logged with a stack (window.__vbErr — paste it to be fixed), and where possible the loop is restarted instead
  // of hanging. A LOST GPU DEVICE (driver reset/timeout — the classic mid-walk freeze) shows a visible banner.
  window.__vbErr = null;
  // ── AND A RING, NOT JUST THE LAST ONE (user 2026-08-18: an intermittent freeze/crash minutes into play) ──
  // __vbErr is overwritten by every subsequent error, so a fault that fires once and is then followed by any
  // other noise leaves nothing behind. That is exactly the wrong shape for a bug you cannot reproduce on demand:
  // by the time anybody looks, the thing that actually broke is gone. This keeps the last 32 with a wall-clock
  // stamp and the uptime at which each happened, which is what turns "it froze at some point" into a timeline.
  // Read it with __vb.errLog(). Bounded so it cannot itself become the leak it is here to help find.
  window.__vbErrLog = [];
  const vbNoteErr = (tag, e) => {
    const msg = tag + ': ' + ((e && (e.stack || e.message)) || e);
    window.__vbErr = msg;
    window.__vbErrLog.push({ t: new Date().toISOString().slice(11, 19), up: +(performance.now() / 1000).toFixed(1), tag, msg: String(msg).slice(0, 400) });
    if (window.__vbErrLog.length > 32) window.__vbErrLog.shift();
    console.error('[vb]', tag, e); };
  // ── CPU PHASE TELEMETRY ── (dev) armed with __vb.cprof(true). When disarmed this costs exactly one
  // predictable `if (CPROF)` test per phase boundary — no performance.now() calls, no allocation — so it
  // is safe to leave compiled into normal play. Armed, it EMAs the ms spent in each tickBody phase.
  let CPROF = 0, cpLast = 0;
  const CP_NAMES = ['physics', 'stream', 'weather', 'snowvox', 'camera', 'life', 'emit', 'encode'];
  const cpEma = new Float64Array(CP_NAMES.length);
  const cpCur = new Float64Array(CP_NAMES.length);     // THIS frame's phase costs (the EMA hides spikes)
  // ── …AND ONE LEVEL DOWN, INSIDE THE SNOW TICK ── 'snowvox' is a single cprof bucket, and during a storm it
  // is the largest thing snow adds to the CPU, but it covers three jobs that are optimised in completely
  // different ways: placing flakes, draining the blanket, and handing the frame's edits to the GPU. Same
  // idiom and the same arming flag as cpMark, so it is one predictable branch per section when disarmed.
  // ── AND ONE LEVEL DOWN INSIDE 'encode', WHICH IS THE PHASE THAT SPIKES ── the bucket covers three jobs with
  // nothing in common: draining the world's dirty bricks into the pool, recording the frame's compute passes,
  // and handing the encoder to the driver. Only the last of those can block on the GPU, so telling them apart
  // is the difference between "we are doing too much work" and "we are waiting for the last frame to finish".
  // `swap` is getCurrentTexture, kept separate because that is where a present-paced stall actually lands.
  const EN_NAMES = ['world', 'passes', 'swap', 'submit'];
  const enEma = new Float64Array(EN_NAMES.length);
  const enMax = new Float64Array(EN_NAMES.length);
  let enLast = 0;
  const enMark = (i) => { const t = performance.now(), d = t - enLast; enEma[i] += (d - enEma[i]) * 0.08; if (d > enMax[i]) enMax[i] = d; enLast = t; };
  const SN_NAMES = ['land', 'melt', 'patch'];
  const snEma = new Float64Array(SN_NAMES.length);
  let snLast = 0;
  const snMark = (i) => { const t = performance.now(); snEma[i] += ((t - snLast) - snEma[i]) * 0.08; snLast = t; };
  const cpMark = (i) => { const t = performance.now(); cpCur[i] = t - cpLast; cpEma[i] += (cpCur[i] - cpEma[i]) * 0.08; cpLast = t; };
  // ── FRAME-TIME RING ── always on (two float stores per frame) so 1% lows / spikes can be read at any moment.
  // FT = the wall gap between frames; FTB = time actually spent INSIDE tickBody. The pair is the whole
  // diagnosis: FT >> FTB means the stall is outside our code (GPU pacing / present / GC), FT ≈ FTB means it is ours.
  const FTR = 4096;                                  // ~13 s at 300 fps — a 1024 ring covered only 3.4 s, so ft() and spikes() reported different windows
  const FT = new Float32Array(FTR), FTB = new Float32Array(FTR); let ftI = 0, ftN = 0;
  let heapPrev = 0, heapDrops = 0, heapAlloc = 0;    // GC discrimination: a frame where the JS heap SHRINKS is a collection
  // ── SPIKE LOG ── worst frames with their phase breakdown + which heavy subsystems ran that frame.
  const CPE_NAMES = ['census', 'band', 'upBricks', 'recenter', 'targets', 'snowPatch', 'stampPatch', 'heapShrank', 'navFlush'];
  let cpEvt = 0, cpSpikeTh = 12;
  const cpSpikes = [];
  // ── GPU UPLOAD ACCOUNTING ── installed only while armed: writeBuffer is wrapped to tally calls + bytes.
  let upN = 0, upB = 0, upWrapped = null, cpUpN = 0, cpUpB = 0, tbT0 = 0;
  function cprofArm(on) {
    if (on && !upWrapped) { const q = device.queue, orig = q.writeBuffer.bind(q);
      upWrapped = orig;
      q.writeBuffer = function (buf, off, data, dOff, size) {
        upN++; upB += (size !== undefined ? size * (data.BYTES_PER_ELEMENT || 1) : (data.byteLength || 0) - (dOff || 0) * (data.BYTES_PER_ELEMENT || 1));
        return dOff === undefined ? orig(buf, off, data) : (size === undefined ? orig(buf, off, data, dOff) : orig(buf, off, data, dOff, size)); };
    } else if (!on && upWrapped) { device.queue.writeBuffer = upWrapped; upWrapped = null; }
    CPROF = on ? 1 : 0;
  }
  window.addEventListener('error', (e) => vbNoteErr('uncaught', e.error || e.message));
  window.addEventListener('unhandledrejection', (e) => vbNoteErr('rejection', e.reason));
  // ── DEVICE LOST ── the picture freezes at the last frame with no error anywhere, so this banner is the only
  // thing that ever tells anybody what happened. It used to print the REASON alone, and for every interesting
  // case that string is 'unknown': WebGPU has exactly two reasons, 'destroyed' (we called destroy(), expected)
  // and 'unknown' (literally everything else), so the reason discriminates NOTHING and info.MESSAGE is where
  // the driver says what it actually did. That was logged to a console the reporter was never going to open and
  // dropped from the one surface they could see. A real report (2026-09-02, an Asus TUF A16) came back as those
  // four words and nothing else, and there was no way to tell a driver timeout from an allocation the laptop
  // could not meet, which are opposite bugs with opposite fixes.
  // So it now carries the whole picture: the driver's own message, which adapter we got and which rung of the
  // world ladder that bought, how long the session had been up, and any GPU errors already recorded. Selectable,
  // with a COPY button, because the entire point is that it ends up in a bug report.
  const gpuLostBanner = (info) => {
    vbNoteErr('gpu device lost', info.reason + ' — ' + info.message);
    // ── LOST BEFORE THE GAME EVER STARTED ── window.__vb is the last thing boot publishes, so its absence means
    // the device died DURING LOAD. That is not a driver blip: it is this machine failing to hold what we asked
    // for, and it will do it again on the next reload and the one after. Drop a rung and retry instead of
    // showing a crash screen whose only honest advice is "try something smaller". See safeStepDown in
    // core/gpu.js for why this is boot-only and how it is bounded against a reload loop.
    if (info.reason !== 'destroyed' && !window.__vb && safeStepDown('device lost while loading')) return;
    try { document.exitPointerLock(); } catch (e) {}     // the pointer is locked to a canvas that will never draw again; nothing below is clickable until it is released
    try { loadEl.classList.add('hidden'); } catch (e) {}
    // TDZ-SAFE READS. This callback can fire DURING boot — an allocation that fails while the pool is being
    // built is exactly the case worth reporting — and in that case the fragments below core/ have not run yet.
    // `typeof` does NOT help here: these are let/const in the SAME scope, so touching one inside its temporal
    // dead zone throws rather than reporting 'undefined'. Hence a try/catch per read. Getting this wrong would
    // have thrown inside a .then() whose .catch() swallows it — a lost device and a BLANK SCREEN.
    const q = (f, d) => { try { const v = f(); return (v === undefined || v === null) ? d : v; } catch (e) { return d; } };
    const tier = q(() => window.__vbTier, null);
    const ad = q(() => window.__vbAdapter, null);
    const gerr = q(() => window.__vbGpuErr, []) || [];
    const report = [
      'reason   ' + info.reason,
      'message  ' + (info.message || '(none)'),
      'adapter  ' + (ad ? ([ad.vendor, ad.architecture, ad.device, ad.description].filter(Boolean).join(' ') || '(unnamed)') : '(unknown)'),
      'world    ' + (tier ? tier.wxz + 'x' + tier.wy + '  view x' + tier.gmul + '  bindable ' + tier.bindMB + ' MB of ' + tier.adapterMB + '  safe ' + tier.safe : '(not reached)'),
      'screen   ' + q(() => RW + 'x' + RH + ' @ ' + Math.round(renderScale * 100) + '%', '(not reached)'),
      'pool     ' + q(() => (((POOL_SLOTS * 512) / 1048576) | 0) + ' MB', '(not reached)'),
      'uptime   ' + (performance.now() / 1000).toFixed(1) + ' s',
      'gpuerr   ' + (gerr.length ? gerr.map((e) => e.msg + (e.n > 1 ? ' x' + e.n : '')).join(' | ') : 'none'),
      'log      ' + (window.__vbErrLog.length ? window.__vbErrLog.slice(-6).map((e) => e.up + 's ' + e.tag).join(', ') : 'none'),
      'agent    ' + navigator.userAgent,
    ].join('\n');
    const b = document.createElement('div');
    b.style.cssText = 'position:fixed;inset:0;z-index:99;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px;padding:24px;box-sizing:border-box;background:rgba(8,10,14,0.94);color:#ff8a95;font:10px "px3",Consolas,monospace;letter-spacing:2px;text-align:center;line-height:2';
    const h = document.createElement('div');
    h.textContent = 'gpu device lost (' + info.reason + ')';
    const sub = document.createElement('div');
    sub.style.cssText = 'color:#c9d2dd;opacity:0.8;letter-spacing:1px';
    sub.textContent = info.reason === 'destroyed' ? 'the page released the gpu' : 'the graphics driver reset, or ran out of memory';
    // The driver's message, big enough to read and SELECTABLE — the banner sits over a canvas that has had
    // user-select off all session, so it has to turn it back on for itself or the text cannot even be dragged.
    const pre = document.createElement('pre');
    pre.style.cssText = 'margin:0;max-width:min(760px,92vw);max-height:38vh;overflow:auto;text-align:left;white-space:pre-wrap;word-break:break-word;background:rgba(0,0,0,0.45);border:1px solid rgba(255,255,255,0.14);border-radius:6px;padding:12px 14px;color:#c9d2dd;font:11px/1.6 Consolas,monospace;letter-spacing:0;user-select:text;-webkit-user-select:text';
    pre.textContent = report;
    const row = document.createElement('div');
    row.style.cssText = 'display:flex;gap:10px;flex-wrap:wrap;justify-content:center';
    const mk = (label, bg, fn) => { const el = document.createElement('button');
      el.style.cssText = 'cursor:pointer;padding:10px 16px;border:1px solid rgba(255,255,255,0.22);border-radius:6px;background:' + bg + ';color:#0d1014;font:9px "px3",Consolas,monospace;letter-spacing:2px';
      el.textContent = label; el.onclick = fn; row.appendChild(el); return el; };
    mk('reload', '#b8ff6e', () => location.reload());
    mk('copy report', '#c9d2dd', function () {
      const self = this;
      const bad = () => { self.textContent = 'select it instead'; };   // clipboard access is refused outright over plain http and on an unfocused page, and it REJECTS rather than throwing — a bare try/catch would have reported success on the one path most likely to fail
      try { const pr = navigator.clipboard.writeText(report); self.textContent = 'copied'; if (pr && pr.catch) pr.catch(bad); } catch (e) { bad(); } });
    // ── THE ONE THING A PLAYER CAN ACTUALLY DO ── if the driver reset under load, or could not hold what we
    // asked for, then reloading unchanged asks it for exactly the same thing again. This steps the world down
    // one rung (SAFE in core/gpu.js) and reloads into it. Hidden on 'destroyed', which is us shutting the page
    // down and never a machine problem, and at the bottom of the ladder there is nothing left to give up.
    const VIEW = ['200 m view', '100 m view', '75 m view', '55 m view, half res'];
    const lvl = tier ? (tier.safe | 0) : 0;
    if (info.reason !== 'destroyed' && lvl < 3) mk('reload smaller (' + VIEW[lvl] + ' -> ' + VIEW[lvl + 1] + ')', '#ffd76a', () => {
      try { localStorage.setItem('vb_safe', String(lvl + 1)); } catch (e) {}
      location.reload(); });
    if (lvl > 0) mk('reset quality', '#8fb7ff', () => { try { localStorage.removeItem('vb_safe'); } catch (e) {} location.reload(); });
    b.append(h, sub, pre, row);
    document.body.appendChild(b);
  };
  window.__vbGpuLost = gpuLostBanner;                   // so the crash screen can be SEEN without crashing a gpu: __vbGpuLost({reason:'unknown', message:'...'}) draws exactly what a player would get. A screen nobody can render is a screen nobody checks
  device.lost.then(gpuLostBanner).catch(() => {});
  const canvas = $('c');
  const ctx = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: 'opaque' });
  let moonTex = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
  device.queue.writeTexture({ texture: moonTex }, new Uint8Array([205, 210, 220, 255]), {}, [1, 1]);   // flat fallback if the photo is missing
  try {                                                // REAL moon — a full-disc photograph mapped onto the disc (user supplied moon.webp and then moon.png 2026-08-19, replacing moon.jpg, which was a WANING CRESCENT with its terminator and earthshine baked into the pixels and needed a high-pass in the shader to pass for full. Measured on the new file: 1600x1600, 68.8% of it above 40/255 and the four quadrants within 92-113 of each other, i.e. evenly lit — so the shader samples it straight)
    const mimg = await createImageBitmap(await (await fetch('assets/moon.png')).blob());
    const moonFallback = moonTex;                      // the 1x1 placeholder is superseded here — destroy it once the real photo is in place
    moonTex = device.createTexture({ size: [mimg.width, mimg.height], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
    device.queue.copyExternalImageToTexture({ source: mimg }, { texture: moonTex }, [mimg.width, mimg.height]);
    mimg.close();                                      // release the decoded bitmap immediately — the texture owns the pixels now
    try { moonFallback.destroy(); } catch (e) {}       // no bind group has been built yet (makeTargets runs later), so nothing references it
  } catch (e) { console.warn('[vb] moon.png missing — flat moon disc', e); }

