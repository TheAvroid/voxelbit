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
  device.lost.then((info) => {                         // device lost = the picture freezes at the last frame with no error anywhere — now it says so
    vbNoteErr('gpu device lost', info.reason + ' — ' + info.message);
    loadEl.classList.add('hidden');
    const b = document.createElement('div');
    b.style.cssText = 'position:fixed;inset:0;z-index:99;display:flex;align-items:center;justify-content:center;background:rgba(8,10,14,0.88);color:#ff8a95;font:10px "px3",Consolas,monospace;letter-spacing:2px;text-align:center;line-height:2';
    b.textContent = 'gpu device lost (' + info.reason + ') — reload the page';
    document.body.appendChild(b);
  }).catch(() => {});
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

