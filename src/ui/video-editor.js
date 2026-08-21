  // @module — screen recording, the clip timeline, red annotations, the export
  // @exports BASE_VIG, VE, renderDist, veAudioDest, vePanel, veTapEl, veToggleRec
  // ═══════════════ VIDEO EDITOR + SCREEN RECORDER ═══════════════ (user)
  // Capture the game canvas to a .webm with MediaRecorder, then trim / cut / delete / resize clips on a timeline and
  // export the result. Entirely self-contained DOM + MediaRecorder work — nothing here touches the WebGPU render loop.
  // The "recording" banner is a DOM element (never drawn on the canvas), so it can never appear in the capture/export.
  const VE = { clips: [], dur: 0, sel: -1, blobUrl: null, thumb: '', playing: false, lastPaint: 0, ac: null,
               capHz: 60, capMs: 1000 / 60 - 0.6, capEvery: 1, paintN: 0, recTrack: null,   // ── THE CAPTURE CADENCE ── on the object rather than as two module consts because tick-body.js reads it EVERY FRAME and the probe below rewrites it once: an exported `let` is a const snapshot taken at module-init, so the render loop would have kept the 60 forever (the linter catches exactly this)
               rec: null, chunks: [], recording: false, drag: null,
               strokes: [], undone: [], pen: false, drawing: false, exporting: false };   // strokes: red annotation polylines in NORMALISED video space; undone: redo stack for Ctrl+Z; exporting: suspends the game's render work
  const veBtnEl = $('veBtn'), vePanel = $('vePanel'), veStage = $('veStage'), veVideo = $('veVideo'),
        veTrack = $('veTrack'), vePlayhead = $('vePlayhead'), veTimeEl = $('veTime'), vePlayBtn = $('vePlay'),
        veRecEl = $('veRec'), veTimeline = $('veTimeline'), veRecBtn = $('veRecBtn');
  const veFmt = (s) => { s = Math.max(0, s || 0); const m = Math.floor(s / 60), q = Math.floor(s % 60); return (m < 10 ? '0' : '') + m + ':' + (q < 10 ? '0' : '') + q; };
  const vePickMime = () => { for (const m of ['video/webm;codecs=vp9,opus', 'video/webm;codecs=vp8,opus', 'video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm']) if (window.MediaRecorder && MediaRecorder.isTypeSupported(m)) return m; return ''; };
  const vePickExportMime = () => { for (const m of ['video/mp4;codecs=avc1.640028,mp4a.40.2', 'video/mp4;codecs=avc1.42E01E,mp4a.40.2', 'video/mp4;codecs=avc1', 'video/mp4']) if (window.MediaRecorder && MediaRecorder.isTypeSupported(m)) return m; return vePickMime(); };   // EXPORT as .mp4 (H.264/AAC) when the browser can record it (user); falls back to webm otherwise
  // H.264 FIRST. The export replays this recording in real time and captures presented frames, so any
  // frame the decoder cannot present in time is lost from the file for good. VP8 at this resolution is
  // software-decoded and could only sustain ~46 fps of a 58 fps recording -- measured, and NOT an encode
  // cost: halving the export resolution changed nothing. ?vecap=vp8 puts the old order back for an A/B.
  const veCaptureMime = () => { for (const m of (location.search.includes('vecap=vp8')
      ? ['video/webm;codecs=vp8,opus', 'video/webm;codecs=vp8', 'video/webm;codecs=vp9,opus', 'video/webm;codecs=vp9', 'video/mp4;codecs=avc1.42E01E,mp4a.40.2', 'video/mp4;codecs=avc1', 'video/webm']
      : ['video/mp4;codecs=avc1.42E01E,mp4a.40.2', 'video/mp4;codecs=avc1', 'video/webm;codecs=vp9,opus', 'video/webm;codecs=vp9', 'video/webm;codecs=vp8,opus', 'video/webm;codecs=vp8', 'video/webm'])) if (window.MediaRecorder && MediaRecorder.isTypeSupported(m)) return m; return ''; };   // LIVE-CAPTURE codec: VP8 webm FIRST. It fixes BOTH complaints — VP8 is 2-5× cheaper to encode than VP9 (no more render-loop lag) AND webm scrubs cleanly in the editor, whereas MediaRecorder's fragmented mp4 has no seek index → the export's frame-by-frame seek stalled and froze the output. mp4 stays only as a last resort for browsers with no webm recording (Safari). The export re-encodes to mp4 regardless, so live quality only needs to be decent.
  const veBitrate = (w, h) => Math.min(100e6, Math.max(28e6, Math.round((w || 1280) * (h || 720) * VE.capHz * 0.2)));   // …and the rate follows the CAPTURE rate (was a hard 60): at 120 Hz twice the frames share the bits, so a fixed number would have halved the quality per frame the moment the capture rate went up   // QUALITY (user: "download looks lower"): the export is a SECOND encode on top of the recording (trim+annotate needs a re-compress), so 16 Mbps compounded into visible loss. Scale the bitrate to the actual resolution × 60 fps at ~0.2 bpp so BOTH the recording and the re-encode stay near-transparent: ~28 Mbps at 1080p, ~44 at 1440p, capped at 100 for 4K/high-DPI. Bigger files, but the download now matches the preview. STILL the RECORDING rate — the master wants to be generous because the export re-encodes it. The EXPORT rate is veExportRate() below.

  // ── IS THE WEBCODECS EXPORT AVAILABLE? ── declared HERE, above its first use, because VE_OVER_KEY below calls it
  // during module init: a `const` read before its own declaration is a TDZ throw, and in this build that is a black
  // screen with one line in the console. Same class of bug as the const-order boot failure in game/index.html.
  const veWCReady = () => typeof VideoEncoder !== 'undefined' && typeof VideoDecoder !== 'undefined' && typeof VideoFrame !== 'undefined' && typeof EncodedVideoChunk !== 'undefined';

  // ── EXPORT SIZE BUDGET (user: 45 s of 3822×1890 came out a 906 MB download) ──
  // MediaRecorder treats videoBitsPerSecond as a SUGGESTION and consistently OVERSHOOTS it. Measured on this
  // content through this exact pipeline (video → canvas → captureStream(0) → requestFrame → recorder):
  //   requested 87 → 142 Mbps (1.64×) · 40 → 67.6 (1.69×) · 20 → 36.4 (1.82×) · HEVC 20 → 36.8 · AV1 20 → 34.3
  // and the real 906 MB export was 168.5 Mbps against an 86.7 Mbps request = 1.94×. So the overshoot GROWS as the
  // target falls, and any honest size estimate has to divide it out. 1.95 is the conservative end of that range:
  // a budget then lands UNDER rather than over, which is the correct direction to miss in.
  // What is NOT true (measured, don't re-derive): the request is otherwise respected — halving it halves the file,
  // linearly, across every codec. And at a FIXED target bitrate H.264/HEVC/AV1 all produce the SAME size; the codec
  // only buys quality-per-bit, never size, so switching codecs cannot shrink a budgeted export.
  // ...and because that factor is really a property of THIS machine's encoder, every finished export folds its own
  // measured ratio back in (veCalibrate below). 1.95 is only the seed; by the second export the estimate is this
  // user's own hardware, which is what makes a "under 500 MB" promise keepable rather than a guess.
  const VE_OVERSHOOT_SEED = 1.95;
  let veOvershoot = VE_OVERSHOOT_SEED;
  // ── THE KEY IS VERSIONED ── everything above was measured on MediaRecorder, which overshoots by ~1.95x. A WebCodecs
  // VideoEncoder honours its `bitrate` closely, so a stored 1.95 from the old path would halve the first WebCodecs
  // export's bitrate and quietly cost quality. `2` starts that measurement over; veCalibrate refines it as before.
  const VE_OVER_KEY = veWCReady() ? 'vb_ve_overshoot2' : 'vb_ve_overshoot';
  if (veWCReady()) veOvershoot = 1.05;
  try { const s = parseFloat(localStorage.getItem(VE_OVER_KEY)); if (isFinite(s) && s >= 1 && s <= 3) veOvershoot = s; } catch (e) {}
  const VE_EXPORT_BPP = 0.10;              // half the recording's 0.2 — this pass only has to preserve an already-compressed master, so the second generation needs far fewer bits than the first
  const VE_RATE_FLOOR = 1.5e6;             // below this a budget is unmeetable rather than merely lossy; the estimate turns amber instead of quietly lying
  const veSizeSel = $('veSize'), veEstEl = $('veEst');
  const veExportDur = () => VE.clips.reduce((a, c) => a + Math.max(0, c.e - c.s), 0);
  // Solve the requested bitrate for the chosen cap. No cap → pure quality target. With a cap, take whichever is
  // SMALLER: spending more than the quality target buys nothing, so a roomy budget still exports the smaller file.
  const veExportRate = (w, h, dur) => {
    const q = Math.min(100e6, Math.max(8e6, Math.round((w || 1280) * (h || 720) * 60 * VE_EXPORT_BPP)));
    const cap = (+veSizeSel.value || 0) * 1e6;
    if (!cap || dur <= 0) return { rate: q, capped: false, starved: false };
    const forBudget = (cap * 8 - 192000 * dur) / dur / veOvershoot;   // audio rides at a fixed 192 kbps and comes out of the same budget
    const rate = Math.min(q, forBudget);
    return { rate: Math.max(VE_RATE_FLOOR, Math.round(rate)), capped: rate < q, starved: rate < VE_RATE_FLOOR };
  };
  // Fold a finished export's REAL ratio back into the estimate. Half-weight EMA so one odd clip cannot swing it,
  // clamped to the range the encoder can plausibly occupy so a corrupt/aborted export can't poison future budgets.
  const veCalibrate = (bytes, dur, reqRate) => {
    if (!(bytes > 0 && dur > 0.5 && reqRate > 0)) return;
    const seen = (bytes * 8 - 192000 * dur) / dur / reqRate;
    if (!isFinite(seen) || seen < 0.5 || seen > 3) return;   // the floor is 0.5, not 1: a VideoEncoder can land UNDER its requested rate on easy content, and refusing to learn that would keep over-estimating forever
    veOvershoot = Math.round((veOvershoot + seen) / 2 * 1000) / 1000;
    try { localStorage.setItem(VE_OVER_KEY, String(veOvershoot)); } catch (e) {}
  };
  const veUpdateEst = () => {
    if (!veEstEl) return;
    const dur = veExportDur();
    if (dur <= 0) { veEstEl.textContent = ''; veEstEl.classList.remove('over'); return; }
    const vw = Math.round(veVideo.videoWidth || 1280) & ~1, vh = Math.round(veVideo.videoHeight || 720) & ~1;
    const r = veExportRate(vw, vh, dur);
    const mb = (r.rate * veOvershoot + 192000) * dur / 8 / 1e6;
    veEstEl.textContent = '≈ ' + (mb >= 1000 ? (mb / 1000).toFixed(1) + ' GB' : Math.round(mb) + ' MB');
    veEstEl.classList.toggle('over', r.starved);
    veEstEl.title = r.starved ? 'this clip is too long to fit that cap — exporting at the minimum bitrate instead'
      : 'estimated size at ' + Math.round(r.rate * veOvershoot / 1e6) + ' Mbps' + (r.capped ? ' (held down by the size cap)' : '');
  };
  try { veSizeSel.value = localStorage.getItem('vb_ve_size') || '0'; } catch (e) {}
  veSizeSel.addEventListener('click', (e) => e.stopPropagation());   // the panel sits over the game — don't let the click reach the world
  veSizeSel.addEventListener('change', () => { try { localStorage.setItem('vb_ve_size', veSizeSel.value); } catch (e) {} veUpdateEst(); });

  // ── AUDIO TAP ── every game sound registers through sndReg, so route them all into a MediaStream and mix that into
  // the capture — recordings then carry sound as well as picture. Built lazily on the first capture and wrapped in
  // try/catch, so any failure degrades to video-only rather than breaking playback. createMediaElementSource is
  // one-shot per element, so each is tapped once and also re-connected to the speakers so normal audio still plays.
  let veGameDest = null;                                 // VE.ac, not a `let`: see the CMD note in ui/console.js — an exported `let` is a dead snapshot outside the module
  const veTapEl = (a) => { if (!VE.ac || a._veTap) return;
    try {
      if (a._sfxOut) { a._sfxOut.connect(veGameDest); a._veTap = true; return; }   // already routed through an effect (see bassTap) — tap its OUTPUT, or the recorder would capture the dry element twice and the filtered one never
      const s = VE.ac.createMediaElementSource(a); s.connect(VE.ac.destination); s.connect(veGameDest); a._veTap = true;
    } catch (e) {} };
  const veAudioDest = () => {
    try {
      if (!VE.ac) { VE.ac = audioCtx(); if (!VE.ac) return null; veGameDest = VE.ac.createMediaStreamDestination(); }   // the SHARED context — see audioCtx
      if (typeof sndReg !== 'undefined') for (const s of sndReg) veTapEl(s.a);
      if (VE.ac.state === 'suspended') VE.ac.resume();
      return veGameDest;
    } catch (e) { console.warn('[vb] audio tap failed', e); return null; }
  };

  // ── SCREEN RECORDING ──
  // captureStream(60) samples on its own 16.67 ms schedule and timestamps by the WALL CLOCK of whatever
  // paint it lands on. The game paints every rAF - 8.3 ms at 120 Hz - so the two beat against each
  // other and the recording gets 8/16/17/25/33 ms intervals plus dropped frames (measured: 46 in 5 s).
  // That uneven spacing IS the judder. MediaRecorder ignores explicit VideoFrame timestamps (verified
  // by experiment), so it cannot be corrected downstream - the paint cadence has to be even at the
  // source, which is what VE.capMs does in the render loop.
  // ── AND THE CAPTURE RATE IS THE DISPLAY'S, NOT A HARD-CODED 60 (user 2026-08-19: "when recording, it cuts my
  // frames in half") ── the cadence argument above is right and stays, but it was pinned at 60: on a 120 Hz
  // monitor the gate in tick-body.js then dropped every OTHER paint for the length of the take, which is exactly
  // the halving the user saw — the recording was smooth and the GAME was not. The two are only in tension if the
  // capture rate is fixed. Sampling at the refresh rate instead satisfies both: one paint per slot, no paint
  // discarded, and the throttle becomes a no-op on any display the game can already keep up with.
  // MEASURED, not read off screen.refreshRate (which does not exist): the MINIMUM rAF delta over 40 frames, so a
  // few slow frames during boot cannot drag the estimate down — a 120 Hz panel that stutters twice still reports
  // 120. Snapped to a ladder because the raw number is never exactly 60/120, and left at 60 if the probe comes
  // back slower than 25 Hz (a machine that far behind wants the smaller encode anyway).
  // 144 is the ceiling ON THE CAPTURE, not on the game: above it the encoder is the thing that cannot keep up,
  // and MediaRecorder dropping frames would put the judder back while costing the file size of the frames it
  // dropped. A 240 Hz display is the one case still throttled, and it is throttled to 120 rather than to 60.
  const VE_HZ_LADDER = [60, 72, 75, 90, 100, 120, 144];   // VE.capHz / VE.capMs hold the result — one paint per capture slot, a hair under so a slot is never missed
  (function veProbeHz() {                              // one burst at boot; the result is cached for every take
    let n = 0, prev = 0, best = 1e9;
    const step = (t) => {
      if (prev) { const d = t - prev; if (d > 1 && d < best) best = d; }
      prev = t;
      if (++n < 40) { requestAnimationFrame(step); return; }
      if (!isFinite(best) || best > 40) return;        // 25 Hz or slower — keep the 60 default
      const hz = 1000 / best;
      let pick = VE_HZ_LADDER[0];
      for (const c of VE_HZ_LADDER) if (hz >= c - 4) pick = c;   // -4 of slack: a 120 Hz panel measures 119.88
      // ── THE PROBE SETS THE DISPLAY RATE AND NOTHING ELSE ── it used to write capHz/capMs directly, which is
      // wrong in two ways now that the capture rate is derived: it does not know the canvas size, and it lands
      // ASYNCHRONOUSLY — 40 rAF frames in — so on a recording started during boot it overwrote the rate
      // veStartRec had just computed while leaving capEvery untouched, and the two then disagreed (observed:
      // capHz 120 against capEvery 1 at a size whose budget allows 99). veStartRec owns the derivation.
      VE.refreshHz = pick;
    };
    requestAnimationFrame(step);
  })();
  // ── RELEASING A CANVAS CAPTURE, IN ONE PLACE ── VIDEO tracks only, for the reason veStopRec gives: the audio
  // tracks come from the shared veGameDest node and stopping them would silence every later recording. A live
  // canvas track nobody reads still copies the presented surface 60x a second (~29 MB a frame at this canvas),
  // so every failure path that created a stream has to come through here.
  const veReleaseCap = (s) => { if (!s) return; try { for (const t of s.getVideoTracks()) t.stop(); } catch (e) {} };
  // The recording UI in one place too, so start, stop and the error path cannot drift apart — they did: only
  // start and stop set it, and a recorder that died left the banner pulsing over a recorder that no longer was.
  const veRecUI = (on) => {
    veRecEl.classList.toggle('hidden', !on);           // ← the "recording" banner (DOM only → never captured)
    veBtnEl.classList.toggle('recording', on);
    veRecBtn.classList.toggle('on', on); veRecBtn.textContent = on ? '■ stop' : '● rec';
  };
  // ═══════════════ THE WEBCODECS RECORDER ═══════════════ (user 2026-08-21: "it should match the fps as the ingame playback")
  // The capture rate used to be held down by VE_PIX_BUDGET, and that budget was a property of MEDIARECORDER, not of
  // the machine. Measured on this hardware, at 3834x1658 off the live WebGPU canvas while the game was rendering:
  //   MediaRecorder budget (the old cap)   240 Mpixel/s   -> 30 fps at 3832x1902
  //   WebCodecs, one frame per paint       514 Mpixel/s   -> kept up with EVERY painted frame
  //   WebCodecs, fed as fast as it accepts  571 Mpixel/s
  // So the encoder was never the limit — the MediaStream plumbing in front of it was. Encoding the canvas directly
  // doubles the capture rate at 4K, 30 fps -> 60. (120 fps at that size would need 875 Mpixel/s, past what this
  // hardware does at all, so 60 is the honest ceiling rather than a tuning choice.)
  // `new VideoFrame(canvas)` reads the WebGPU canvas correctly — verified, 11 distinct pictures out of 12 paints.
  // Audio still goes through MediaRecorder, but an AUDIO-ONLY one: no pixels, so none of the cost that capped the
  // video, and it lands as AAC in a fragmented mp4 that ui/mp4-remux.js already knows how to read. Its samples and
  // its `stsd` are copied into the muxed take untouched.
  const VE_WC_PIX_BUDGET = 480e6;                         // conservative against the measured 571: the game has to keep rendering underneath, and a queue that grows is handled by dropping (see veWCPush) rather than by stalling the render loop
  const VE_WC_AMIME = 'audio/mp4;codecs=mp4a.40.2';
  const VE_A_TAIL_MS = 300;                               // how long the audio keeps running after the last frame, so its tail is captured rather than lost to stop()
  const veWCRecReady = () => veWCReady() && !!window.MediaRecorder && MediaRecorder.isTypeSupported(VE_WC_AMIME);
  // ?vehz=N forces the rate the capture cadence is derived from. Read HERE rather than inside veProbeHz, because
  // that probe returns early on any machine it cannot measure and the override went with it — so the flag did
  // nothing exactly when it was most needed. Applied at use, it cannot be skipped.
  const VE_HZ_FORCE = (() => { const m = /[?&]vehz=(\d+)/.exec(location.search); return m ? Math.max(24, Math.min(240, +m[1])) : 0; })();
  // ?veav=N shifts the recorded audio N ms LATER against the picture (negative = earlier). The lead below is
  // computed, not guessed, so this should sit at 0 — it exists because the capture path has latencies nothing in
  // here can observe (the tap runs at the audio graph, ahead of whatever the speakers add) and the only honest way
  // to settle the last few milliseconds is to watch a clip and dial it.
  const VE_AV_TRIM = (() => { const m = /[?&]veav=(-?\d+)/.exec(location.search); return m ? Math.max(-500, Math.min(500, +m[1])) : 0; })();

  // One frame per gated paint. Never blocks: if the encoder is behind, the frame is dropped and counted, because
  // stalling here would stall the GAME — and the export reconstructs a constant-rate timeline anyway.
  const veWCPush = (now) => {
    const R = VE.wc;
    if (!R || !R.enc || R.enc.state !== 'configured' || R.err) return;
    // ── PACE BY TIME, NOT BY A PAINT COUNT ── capEvery is refresh/capHz, which only gives capHz if the game paints
    // at exactly the refresh rate. It does not: this render loop has no v-sync. Measured, a window painting ~143 fps
    // against a probed 60 Hz refresh gave capEvery 1, so every paint was pushed — 143 frames a second, all labelled
    // 60. An 8 s take then came out spanning 19 s: SLOW MOTION. A wall-clock interval yields capHz whatever the
    // paint rate is. The 0.95 leaves room for jitter, so a paint arriving a hair early is not skipped outright.
    if (now - R.last < R.minMs) return;                   // not a drop — simply not this paint's turn
    R.last = now;
    if (R.enc.encodeQueueSize > 4) { R.dropped++; return; }
    if (R.t0 < 0) R.t0 = now;
    try {
      const f = new VideoFrame(canvas, { timestamp: Math.max(0, Math.round((now - R.t0) * 1000)), alpha: 'discard' });
      R.ts.push(f.timestamp);
      R.enc.encode(f, { keyFrame: (R.n % R.gop) === 0 });
      f.close(); R.n++;
    } catch (e) { R.err = e; console.warn('[vb] wc recorder push failed', e); }
  };

  // Returns true when it has taken the take over; false means "use the MediaRecorder path below".
  const veWCRecStart = async () => {
    if (!veWCRecReady()) return false;
    // Shrink the canvas to the trace resolution BEFORE reading its size — see veCaptureNative in render/targets.js.
    // Every failure path below has to put it back, or a refused take would leave the game rendering at capture size.
    veCaptureNative(true);
    const off = () => { veCaptureNative(false); return false; };
    const w = (canvas.width | 0) & ~1, h = (canvas.height | 0) & ~1;
    if (w < 16 || h < 16 || w > 4096) return off();       // hardware AVC encoders top out at 4096 wide; past that fall back rather than fail silently at export time
    const refresh = VE_HZ_FORCE || VE.refreshHz || 60;
    const maxFps = Math.max(12, VE_WC_PIX_BUDGET / Math.max(1, w * h));
    const capEvery = Math.max(1, Math.ceil(refresh / maxFps));
    const capHz = refresh / capEvery;
    let cfg = null;
    for (const codec of VE_ENC_TRY) {
      const c = { codec, width: w, height: h, bitrate: veBitrate(w, h), framerate: Math.max(1, Math.round(capHz)), avc: { format: 'avc' }, latencyMode: 'realtime' };
      const sup = await VideoEncoder.isConfigSupported(c).catch(() => null);
      if (sup && sup.supported) { cfg = c; break; }
    }
    if (!cfg || VE.recording) return off();               // …and re-check VE.recording: this awaited, so a second R press could have started a take underneath us
    const R = { enc: null, n: 0, t0: -1, gop: Math.max(1, Math.round(2 * capHz)), ts: [], samples: [], data: [],
                desc: null, dropped: 0, err: null, aRec: null, aChunks: [], w, h, capHz,
                last: -1e9, minMs: (1000 / capHz) * 0.95, aT0: -1 };
    try {
      R.enc = new VideoEncoder({
        output: (ch, meta) => {
          if (!R.desc && meta && meta.decoderConfig && meta.decoderConfig.description) R.desc = new Uint8Array(meta.decoderConfig.description);
          const b = new Uint8Array(ch.byteLength); ch.copyTo(b);
          R.samples.push({ size: b.length, dur: 0, cts: 0, sync: ch.type === 'key' });   // dur is filled at stop, from the timestamps actually captured
          R.data.push(b);
        },
        error: (e) => { R.err = e; console.warn('[vb] wc recorder', e); },
      });
      R.enc.configure(cfg);
    } catch (e) { console.warn('[vb] wc recorder configure failed — using MediaRecorder', e); return off(); }
    const adest = veAudioDest();
    if (adest) {
      try {
        R.aRec = new MediaRecorder(adest.stream, { mimeType: VE_WC_AMIME, audioBitsPerSecond: 192000 });
        R.aRec.ondataavailable = (e) => { if (e.data && e.data.size) R.aChunks.push(e.data); };
        R.aT0 = performance.now();                        // ── THE AUDIO CLOCK'S ORIGIN ── rAF timestamps and performance.now() share a time origin, so this is directly comparable with R.t0 below
        R.aRec.start(1000);                               // same one-second timeslice as the old path, for the same reason: a lost tab costs a second, not the take
      } catch (e) { console.warn('[vb] wc recorder: no audio', e); R.aRec = null; }
    }
    VE.wc = R;
    VE.capEvery = 1; VE.capHz = capHz; VE.capMs = 1000 / capHz - 0.6; VE.paintN = 0;   // capEvery 1: veWCPush does its own pacing (above), so the render loop hands it every paint and it decides
    VE.pushFrame = veWCPush;
    VE.recording = true;
    veRecUI(true);
    console.log('[vb] rec (webcodecs)', w + 'x' + h, '@', capHz.toFixed(1), 'fps (refresh', refresh + ', every', capEvery + ' paints)');
    /* ── AND IT NO LONGER STARTS THE MUSIC (user 2026-08-21: "remove the r playing the song function. keep the
       recorder on r") ── it did for one day. R is the recorder and nothing else again, and the soundtrack is
       back on its own clock — see ANTHEM_AT in ui/audio.js. */
    return true;
  };

  const veWCRecStop = async () => {
    const R = VE.wc; if (!R) return;
    VE.wc = null; VE.pushFrame = null; VE.recording = false; VE.recTrack = null;
    veRecUI(false);
    veCaptureNative(false);                               // back to the full-size canvas before anything below can throw
    try { if (R.enc && R.enc.state !== 'closed') await R.enc.flush(); } catch (e) {}
    try { if (R.enc && R.enc.state !== 'closed') R.enc.close(); } catch (e) {}
    // ── LET THE AUDIO OVERRUN, THEN CUT IT BACK ── MediaRecorder does not hand over the last fraction of a second
    // when it is stopped: measured, the audio covered 99.2-99.4% of the wall clock it ran for, a fixed ~130 ms
    // shortfall whatever the take's length, and that missing tail is the end of the last sound (user 2026-08-21:
    // "the audio seemed to cut off at the end"). Waiting here costs a quarter second before the editor opens and
    // guarantees there is more audio than picture; the excess is trimmed to the video's own length below, so the
    // two still end together.
    await new Promise((r) => setTimeout(r, VE_A_TAIL_MS));
    let aBlob = null;
    if (R.aRec) {
      try {
        aBlob = await new Promise((res) => {
          const done = () => res(R.aChunks.length ? new Blob(R.aChunks, { type: 'audio/mp4' }) : null);
          R.aRec.onstop = done; R.aRec.onerror = done;
          if (R.aRec.state === 'inactive') done(); else R.aRec.stop();
        });
      } catch (e) { aBlob = null; }
    }
    if (!R.samples.length || !R.desc) { console.warn('[vb] wc recorder produced nothing'); return; }
    // ── DURATIONS COME FROM THE CAPTURE TIMES, QUANTISED TO A FRAME GRID ── the encoder emits chunks in submission
    // order, so ts[i] belongs to samples[i]. The last frame has no successor and gets the median of the rest.
    // ── AND THE GRID IS NOT OPTIONAL ── the first version wrote raw microsecond durations against a 1,000,000
    // timescale. Every duration was then a different number (19700, 16800, 6200, 91600 …), no sane frame rate can
    // be derived from that, and the file came out declaring **r_frame_rate = 1000000/1** — one million fps. Players
    // and filters that conform to r_frame_rate then try to build a million frames a second: ffmpeg expanded a 252
    // frame take to 3,287,900, the editor preview stuttered, and playback showed GREEN frames (user 2026-08-21).
    // Rounding each duration to a whole number of capture slots, with one slot = 1000 ticks, makes the declared
    // rate exactly capHz while still keeping a real gap as a real gap. MediaRecorder's own takes always did this —
    // timescale 30000, r_frame_rate 60/1 — which is why this never came up before the recorder was replaced.
    const gaps = [];
    for (let i = 1; i < R.ts.length; i++) gaps.push(R.ts[i] - R.ts[i - 1]);
    const sorted = gaps.slice().sort((a, b) => a - b);
    // ── THE SLOT IS WHAT WAS ACTUALLY CAPTURED ── deriving it from capHz assumes the pacing hit its target, and
    // when it did not the whole take was rescaled. A LOW percentile, not the median: rounding each gap to a whole
    // number of slots can only round a short gap UP, so a coarse slot inflates every gap below it. Measured with a
    // median slot: gaps of 8-14 ms against a 13.7 ms slot stretched a 20.06 s take into 22.13 s of video, and the
    // audio — which was right — then ran progressively further ahead. That is the drift behind "the sound for the
    // eating plays much before". p10 sits under almost every real gap, so almost nothing is forced upwards.
    const pctl = (f) => sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * f))] : Math.round(1e6 / Math.max(1, R.capHz || 60));
    const period = Math.max(Math.round(1e6 / 240), Math.min(pctl(0.5), Math.max(1, pctl(0.10))));
    const med = pctl(0.5);
    const TS = Math.max(1, Math.round(1e6 / period * 1000));   // one slot = 1000 ticks, so gcd(durations) = 1000 and r_frame_rate lands on the real rate
    // ── AND THE ROUNDING IS CUMULATIVE, NOT PER-FRAME ── each frame's slot is chosen from the running REAL time,
    // so an error made on one frame is paid back by the next instead of accumulating. Rounding each gap on its own
    // lets a consistent bias run away over a long take, which is exactly how a few milliseconds a frame became
    // seconds of A/V drift.
    let realAcc = 0, slotAcc = 0;
    for (let i = 0; i < R.samples.length; i++) {
      realAcc += i + 1 < R.ts.length ? R.ts[i + 1] - R.ts[i] : med;
      const want = Math.round(realAcc / period);
      const dur = Math.max(1, want - slotAcc);
      slotAcc += dur;
      R.samples[i].dur = dur * 1000;
    }
    const tracks = [{ kind: 'video', timescale: TS, width: R.w, height: R.h, stsd: veAvcStsd(R.w, R.h, R.desc), samples: R.samples, data: R.data }];
    if (aBlob) {
      try {
        const AP = await veMp4Parse(aBlob);
        const at = AP && AP.live.find((t) => t.kind === 'audio');
        if (at && at.samples.length) {
          // ── THE TWO TRACKS DO NOT START AT THE SAME INSTANT ── the audio recorder starts inside veWCRecStart,
          // while video timestamp 0 is the FIRST PUSH, which cannot happen until the next frame has been rendered
          // — and veCaptureNative has just rebuilt every screen texture, so that frame is a slow one. Muxing both
          // from 0 then plays audio recorded BEFORE the first picture alongside it, i.e. every sound arrives early
          // (user 2026-08-21: "I'll eat an apple but the sound for the eating plays much before").
          // Both clocks are performance.now(), so the lead is directly measurable — drop that much audio off the
          // front. Trimming audio is the right side to correct: dropping video would throw away real frames, and
          // padding it would invent them.
          const lead = Math.max(0, ((R.t0 >= 0 && R.aT0 >= 0) ? R.t0 - R.aT0 : 0) + VE_AV_TRIM);
          const skip = lead * at.mediaTs / 1000;             // FRACTIONAL ticks — the remainder is not thrown away, see below
          const aS = [], aD = [];
          let acc = 0, dropped = 0, first = true;
          for (const q of at.samples) {
            if (acc + q.dur <= skip) { acc += q.dur; dropped++; continue; }
            let dur = q.dur;
            // ── AND THE SUB-FRAME REMAINDER ── dropping whole AAC frames can only correct in ~21 ms steps and
            // always rounds the trim DOWN, so it left an audio lead of 0-21 ms. Ears are far more sensitive to
            // audio arriving EARLY than late, and ~20 ms is already at the threshold — which is what was left
            // after the drift fix (user: "still slightly before the eating animation"). Holding the FIRST
            // surviving frame longer by the remainder pushes every later frame back by exactly it, so the
            // alignment lands on the microsecond instead of the nearest frame. Only that one frame is affected.
            if (first) { dur += Math.max(0, Math.round(skip - acc)); first = false; }
            aS.push({ size: q.size, dur, cts: 0, sync: true });
            aD.push(aBlob.slice(q.off, q.off + q.size));
            acc += q.dur;
          }
          // ── AND CUT THE OVERRUN ── the wait above deliberately captured more audio than picture. Keep one extra
          // frame past the video so nothing is clipped early, and drop the rest: a track running seconds past the
          // last frame is its own bug (it was what made audio look "longer" back when the video was inflated).
          const vidTicks = R.samples.reduce((k, q) => k + q.dur, 0) / TS * at.mediaTs;
          let keep = 0, used = 0;
          for (const q of aS) { if (used > vidTicks) break; used += q.dur; keep++; }
          if (keep < aS.length) { aS.length = keep; aD.length = keep; }
          // ── AND CHECK THE AUDIO ACTUALLY COVERS THE TAKE ── an audio track shorter than the wall clock it was
          // recorded over means MediaRecorder under-produced, and if that shortfall is spread through the take
          // rather than sitting at the end it is DRIFT: sound running further ahead of picture the longer it runs.
          const wallMs = R.aT0 >= 0 ? performance.now() - R.aT0 : 0;
          const audMs = at.samples.reduce((k, q) => k + q.dur, 0) / at.mediaTs * 1000;
          console.log('[vb] rec a/v lead', lead.toFixed(1), 'ms' + (VE_AV_TRIM ? ' (incl. ?veav=' + VE_AV_TRIM + ')' : '') + ' — dropped', dropped, 'off the front,',
                      at.samples.length - keep - dropped, 'off the tail, of', at.samples.length,
                      '| audio covered', audMs.toFixed(0), 'ms of', wallMs.toFixed(0),
                      'ms wall (' + (audMs / Math.max(1, wallMs) * 100).toFixed(1) + '%)');
          if (aS.length) tracks.push({ kind: 'audio', timescale: at.mediaTs, stsd: AP.moovRaw.slice(at.stsd.start, at.stsd.end), samples: aS, data: aD });
        }
      } catch (e) { console.warn('[vb] wc recorder: audio could not be muxed', e); }
    }
    const out = veMp4Mux(tracks, 1000);
    if (!out) { console.warn('[vb] wc recorder: mux failed'); return; }
    if (R.dropped) console.log('[vb] rec dropped', R.dropped, 'frames to encoder backpressure of', R.n + R.dropped);
    veLoadBlob(out);
  };

  const veStartRec = async () => {
    if (VE.recording || VE.starting) return;              // `starting` matters now that this awaits: R auto-repeats, and two overlapping starts used to corrupt a take
    VE.starting = true;
    try { if (await veWCRecStart()) return; } catch (e) { console.warn('[vb] wc recorder start failed — using MediaRecorder', e); }
    finally { VE.starting = false; }
    if (VE.recording) return;
    // ── NOT WHILE AN EXPORT IS RUNNING ── VE.exporting gates the whole render pass chain (tick-passes.js), so
    // the canvas is not being repainted: the take would be one frozen frame for the length of the export, and
    // VE.recording would then throttle the game's paint cadence the moment the export released it.
    if (VE.exporting) { console.warn('[vb] not starting a recording during an export — the canvas is not being painted'); return; }
    const mime = veCaptureMime(); if (!mime) { console.warn('[vb] MediaRecorder unsupported'); return; }   // hardware H.264 first → doesn't contend with the render loop the way software VP9 did
    // ── THE CAPTURE RATE IS BOUNDED BY THE ENCODER, NOT ONLY BY THE DISPLAY (user 2026-08-19: "it seems to lag
    // in the beginning of the clip and when the player eats the steak") ── sampling at the refresh rate fixed
    // the game being throttled, and then asked the encoder for something it cannot do: MEASURED on the user's
    // own 3822x1890 take, the file came back at an average 40 fps with gaps of 50-84 ms and one of 679 ms.
    // That is 7.2 MEGAPIXELS a frame; at 120 fps it is 867 Mpixel/s, which no hardware H.264 encoder sustains
    // while the game is also rendering. MediaRecorder does not block when it falls behind, it DROPS — and a
    // dropped frame is exactly the judder VE_CAP_MS exists to prevent, now arriving from the other end.
    // So the rate is min(refresh, budget / pixels), and 240 Mpixel/s is the budget: a shade under the ~289
    // that take actually achieved, so it sits inside what this machine demonstrably does rather than at the
    // edge of it. 1080p keeps the full refresh; 4K lands near 30.
    // AND IT SNAPS TO refresh/N, N AN INTEGER, which is the whole reason this stays smooth. The frames are
    // pushed BY THE RENDER LOOP (see capEvery in main/tick-body.js), one every Nth paint, so the spacing is
    // exact by construction instead of being a sampler's own clock beating against the paint rate — the
    // original judder this block was written for. captureStream(0) is manual-push mode, the same mode the
    // EXPORT path already uses for the same reason; it also means the game never skips a paint while
    // recording, so a 120 Hz game stays 120 Hz whatever the capture rate is.
    const VE_PIX_BUDGET = 240e6;
    { const px = Math.max(1, (canvas.width | 0) * (canvas.height | 0));
      const refresh = VE.refreshHz || 60;
      const maxFps = Math.max(12, VE_PIX_BUDGET / px);
      VE.capEvery = Math.max(1, Math.ceil(refresh / maxFps));
      VE.capHz = refresh / VE.capEvery;
      VE.capMs = 1000 / VE.capHz - 0.6;
      VE.paintN = 0; VE.lastPush = 0;                   // lastPush drives the time half of the capture gate in tick-body.js — stale from a previous take, the first push of this one would be skipped
      console.log('[vb] rec', canvas.width + 'x' + canvas.height, '@', VE.capHz.toFixed(1), 'fps (refresh', refresh + ', every', VE.capEvery + ' paints)'); }
    let stream; try { stream = canvas.captureStream(0); } catch (e) { console.warn('[vb] canvas.captureStream failed', e); return; }   // 0 = MANUAL push — the render loop drives it, see above
    VE.recTrack = stream.getVideoTracks()[0] || null;
    if (!VE.recTrack || !VE.recTrack.requestFrame) { console.warn('[vb] canvas capture has no requestFrame — falling back to the sampler'); veReleaseCap(stream); try { stream = canvas.captureStream(VE.capHz); } catch (e2) { return; } VE.recTrack = null; }
    VE.pushFrame = () => { try { VE.recTrack && VE.recTrack.requestFrame(); } catch (e) {} };   // the render loop calls VE.pushFrame and does not care which recorder is behind it
    VE.recStream = stream;                             // kept so veStopRec can release the canvas capture (see there)   // captures at VE.capHz → matches the game's cadence so playback isn't juddery (user), and no longer at the cost of the game's own frame rate
    const adest = veAudioDest(); if (adest) { try { adest.stream.getAudioTracks().forEach((t) => stream.addTrack(t)); } catch (e) {} }   // mix in the game audio (user) — video-only if it fails
    // ── THE CHUNK LIST IS PER-RECORDER, NOT A MODULE SINGLETON ── it was VE.chunks, and stop() queues
    // `dataavailable` and `stop` as SEPARATE tasks, so a second veStartRec landing between them corrupted the
    // take two ways: arriving first it pushed the finished file into the new take's list, and `new Blob([A, B])`
    // of two complete containers plays only A — the new footage silently discarded; arriving second it wiped the
    // array `onstop` was about to read, giving a 0-byte blob, a <video> that errors, veInitClips that never runs,
    // and the panel opening on the PREVIOUS take's timeline. Reachable by holding R, which auto-repeats.
    // A local closed over by this recorder's own handlers cannot be touched by any later call.
    const chunks = [];
    VE.chunks = chunks;                                // exposed for the debug hooks only; nothing reads it back
    let rec;
    try { rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: veBitrate(canvas.width, canvas.height), audioBitsPerSecond: 192000 }); }   // record at full canvas res + a resolution-scaled bitrate so encode #1 is high quality (the export re-encodes it)
    catch (e) { console.warn('[vb] MediaRecorder ctor failed', e); veReleaseCap(stream); VE.recStream = null; return; }   // release the capture: leaving it live orphaned a stream nobody could ever stop, because the next start overwrote VE.recStream
    const blobType = mime.indexOf('mp4') >= 0 ? 'video/mp4' : 'video/webm';   // blob container follows the codec actually recorded
    rec.ondataavailable = (e) => { if (e.data && e.data.size) chunks.push(e.data); };
    rec.onstop = () => veLoadBlob(new Blob(chunks, { type: blobType }));
    // ── A DEAD RECORDER MUST NOT LEAVE THE BANNER PULSING, AND MUST NOT EAT THE TAKE ── MediaRecorder fires
    // `error` and goes INACTIVE without ever firing `stop`, so with no handler here the failure was completely
    // silent: VE.recording stayed true, "recording" kept flashing, the game stayed throttled to the capture
    // cadence for a recorder that no longer existed, and the eventual stop press hit an InvalidStateError that
    // veStopRec's own catch swallowed — no blob, no warning, nothing. Salvage whatever has arrived (with the
    // timeslice below that is everything up to the last second) and put the UI back exactly as veStopRec would.
    rec.onerror = (ev) => {
      console.warn('[vb] MediaRecorder error - salvaging the take', (ev && ev.error) || ev);
      if (VE.rec !== rec) return;                      // a stale recorder's late error must not tear down a live one
      VE.rec = null; VE.recording = false; veRecUI(false);
      veReleaseCap(VE.recStream); VE.recStream = null;
      if (chunks.length) veLoadBlob(new Blob(chunks, { type: blobType }));
    };
    VE.lastPaint = performance.now() - VE.capMs;     // (kept for the fallback sampler path only)
    // ── A TIMESLICE, SO A LOST TAB IS NOT A LOST TAKE ── start() with no argument delivers ONE blob, at stop,
    // which means the whole recording sits inside the recorder until then: an OOM, an encoder fault or a
    // navigation lost ALL of it rather than its tail. veBitrate reads the DPR-SCALED buffer, not CSS pixels, so
    // on this display the capture runs ~650 MB/min and a two-minute take is already past a gigabyte — the
    // recording lengths this invites are exactly the ones that used to cost everything. 1000 ms is the coarsest
    // slice that still bounds the loss to one second; finer buys nothing and costs a blob per slice.
    VE.rec = rec;
    try { rec.start(1000); }
    catch (e) { console.warn('[vb] MediaRecorder start failed', e); VE.rec = null; veReleaseCap(stream); VE.recStream = null; return; }   // was unguarded: a throw here propagated into the keydown handler in ui/input.js and skipped every key below R
    VE.recording = true;
    veRecUI(true);
    /* ── AND IT NO LONGER STARTS THE MUSIC (user 2026-08-21: "remove the r playing the song function. keep the
       recorder on r") ── it did for one day. R is the recorder and nothing else again, and the soundtrack is
       back on its own clock — see ANTHEM_AT in ui/audio.js. */
  };

  const veStopRec = () => {
    if (!VE.recording) return;
    if (VE.wc) { veWCRecStop(); return; }                 // the WebCodecs take muxes itself and loads the result; everything below is MediaRecorder teardown
    try { VE.rec.stop(); } catch (e) {}
    // Release the canvas capture. Without this the compositor keeps feeding a stream nobody reads for
    // the rest of the session. VIDEO tracks only: the audio tracks come from the shared veGameDest node
    // and stopping them would silence every later recording.
    if (VE.recStream) { veReleaseCap(VE.recStream); VE.recStream = null; }
    VE.recording = false;
    VE.pushFrame = null;
    VE.recTrack = null;                              // the render loop tests this before pushing — a stale track would keep requestFrame firing into a stopped recorder
    veRecUI(false);
  };
  const veToggleRec = () => { VE.recording ? veStopRec() : veStartRec(); };

  // ── LOAD a recorded blob into the editor ──
  const veLoadBlob = (blob) => {
    VE.blob = blob;                                       // ── KEEP THE BLOB, NOT JUST ITS URL ── the WebCodecs export DEMUXES the take rather than replaying it (see veExportWC), so it needs the bytes. An object URL cannot be read back
    if (VE.blobUrl) URL.revokeObjectURL(VE.blobUrl);
    VE.blobUrl = URL.createObjectURL(blob);
    veVideo.src = VE.blobUrl;
    veVideo.onloadedmetadata = () => {
      if (!isFinite(veVideo.duration) || veVideo.duration === 0) {   // MediaRecorder webm reports Infinity until seeked past end
        veVideo.currentTime = 1e101;
        veVideo.ontimeupdate = () => { veVideo.ontimeupdate = null; veVideo.currentTime = 0; veInitClips(); };
      } else veInitClips();
    };
    if (vePanel.classList.contains('hidden')) veOpen();   // pop the editor open so a fresh capture is ready to trim
  };
  const veInitClips = () => {
    VE.dur = veVideo.duration || 0;
    if (veVideo.videoWidth && veVideo.videoHeight) veStage.style.aspectRatio = veVideo.videoWidth + ' / ' + veVideo.videoHeight;   // size the box to the clip → no letterbox bars (user)
    VE.clips = VE.dur > 0 ? [{ s: 0, e: VE.dur }] : [];
    VE.sel = VE.clips.length ? 0 : -1;
    veStage.classList.toggle('has-clip', VE.clips.length > 0);
    veRenderTimeline(); veUpdatePlayhead(); veUpdateTime(); veUpdateButtons();
  };
  const veCaptureThumb = () => {
    try { const c = document.createElement('canvas'); c.width = 160; c.height = 90;
      c.getContext('2d').drawImage(veVideo, 0, 0, 160, 90); VE.thumb = c.toDataURL('image/jpeg', 0.5); } catch (e) {}
  };
  veVideo.addEventListener('loadeddata', () => { veCaptureThumb(); veRenderTimeline(); veFitDraw(); });

  // ── TIMELINE (the scrubber box) ──
  const veXToTime = (clientX) => { const r = veTrack.getBoundingClientRect(); return Math.max(0, Math.min(VE.dur, ((clientX - r.left) / Math.max(1, r.width)) * VE.dur)); };
  const veRenderTimeline = () => {
    veTrack.innerHTML = '';
    if (VE.dur <= 0) return;
    const W = veTrack.clientWidth || 1;
    VE.clips.forEach((clip, i) => {
      const el = document.createElement('div'); el.className = 'veClip' + (i === VE.sel ? ' sel' : '');
      el.style.left = ((clip.s / VE.dur) * W) + 'px';
      el.style.width = Math.max(6, ((clip.e - clip.s) / VE.dur) * W) + 'px';
      if (VE.thumb) { const t = document.createElement('div'); t.className = 'veThumb'; t.style.backgroundImage = 'url(' + VE.thumb + ')'; el.appendChild(t); }
      const hl = document.createElement('div'); hl.className = 'veHandle l';
      const hr = document.createElement('div'); hr.className = 'veHandle r';
      el.appendChild(hl); el.appendChild(hr);
      el.addEventListener('mousedown', () => { VE.sel = i; veRenderTimeline(); veUpdateButtons(); });   // body → select
      hl.addEventListener('mousedown', (ev) => veHandleDown(ev, i, 'l'));                                // edges → trim
      hr.addEventListener('mousedown', (ev) => veHandleDown(ev, i, 'r'));
      veTrack.appendChild(el);
    });
  };
  const veHandleDown = (ev, i, side) => {
    if (VE.exporting) return;                          // a trim drag mid-export mutates the list veExport is walking
    ev.preventDefault(); ev.stopPropagation();
    VE.sel = i; VE.drag = { i, side }; veRenderTimeline(); veUpdateButtons();
    document.addEventListener('mousemove', veHandleMove); document.addEventListener('mouseup', veHandleUp);
  };
  const veHandleMove = (ev) => {
    if (!VE.drag) return;
    const c = VE.clips[VE.drag.i]; if (!c) return;
    const t = veXToTime(ev.clientX), prev = VE.clips[VE.drag.i - 1], next = VE.clips[VE.drag.i + 1];
    if (VE.drag.side === 'l') c.s = Math.max(prev ? prev.e : 0, Math.min(c.e - 0.05, t));   // clamped inside its neighbours → clips stay ordered, gaps reclaimable
    else c.e = Math.min(next ? next.s : VE.dur, Math.max(c.s + 0.05, t));
    veRenderTimeline(); veUpdateTime();
  };
  const veHandleUp = () => { VE.drag = null; document.removeEventListener('mousemove', veHandleMove); document.removeEventListener('mouseup', veHandleUp); };
  veTimeline.addEventListener('mousedown', (ev) => {                 // scrub ANYWHERE on the timeline, including over a clip (user) — the trim handles stopPropagation, so dragging an edge still trims instead of scrubbing
    if (VE.dur <= 0) return;
    const to = (e) => veSeek(veXToTime(e.clientX)); to(ev);
    const up = () => { document.removeEventListener('mousemove', to); document.removeEventListener('mouseup', up); };
    document.addEventListener('mousemove', to); document.addEventListener('mouseup', up);
  });

  // ── PLAYHEAD / SEEK / PLAYBACK (kept clips play in order, deleted gaps are skipped) ──
  const veUpdatePlayhead = () => { const W = veTrack.clientWidth || 1; vePlayhead.style.left = (8 + (VE.dur > 0 ? (veVideo.currentTime / VE.dur) * W : 0)) + 'px'; };
  const veUpdateTime = () => { const kept = VE.clips.reduce((a, c) => a + (c.e - c.s), 0); veTimeEl.textContent = veFmt(veVideo.currentTime) + ' / ' + veFmt(kept); };
  const veSeek = (t) => { veVideo.currentTime = Math.max(0, Math.min(VE.dur - 1e-3, t)); veUpdatePlayhead(); veUpdateTime(); };
  const vePlayLoop = () => {
    if (!VE.playing) return;
    const t = veVideo.currentTime;
    const cur = VE.clips.find((c) => t >= c.s - 1e-3 && t < c.e);
    if (!cur) { const nxt = VE.clips.find((c) => c.s > t); if (nxt) veVideo.currentTime = nxt.s; else return veStopAtEnd(); }
    else if (t >= cur.e - 0.03) { const nxt = VE.clips[VE.clips.indexOf(cur) + 1]; if (nxt) veVideo.currentTime = nxt.s; else return veStopAtEnd(); }
    veUpdatePlayhead(); veUpdateTime(); requestAnimationFrame(vePlayLoop);
  };
  const veStopAtEnd = () => { vePause(); if (VE.clips.length) veSeek(VE.clips[0].s); };
  const vePlay = () => {
    if (VE.dur <= 0 || !VE.clips.length) return;
    const t = veVideo.currentTime;
    if (!VE.clips.some((c) => t >= c.s - 1e-3 && t < c.e)) veVideo.currentTime = VE.clips[0].s;
    VE.playing = true; vePlayBtn.textContent = '❚❚'; veVideo.play().catch(() => {}); requestAnimationFrame(vePlayLoop);
  };
  const vePause = () => { VE.playing = false; vePlayBtn.textContent = '▶'; veVideo.pause(); };
  const veTogglePlay = () => { VE.playing ? vePause() : vePlay(); };

  // ── CUT / DELETE ──
  const veSplit = () => {
    if (VE.exporting) return;                          // splicing the live array mid-export drops a segment from the download
    const t = veVideo.currentTime, i = VE.clips.findIndex((c) => t > c.s + 0.02 && t < c.e - 0.02);
    if (i < 0) return;
    const c = VE.clips[i]; VE.clips.splice(i, 1, { s: c.s, e: t }, { s: t, e: c.e }); VE.sel = i + 1;
    veRenderTimeline(); veUpdateButtons();
  };
  const veDelete = () => {
    if (VE.exporting) return;                          // as veSplit: the export is walking this list
    if (VE.sel < 0 || VE.sel >= VE.clips.length) return;
    VE.clips.splice(VE.sel, 1); VE.sel = Math.min(VE.sel, VE.clips.length - 1);
    veStage.classList.toggle('has-clip', VE.clips.length > 0);
    veRenderTimeline(); veUpdateTime(); veUpdateButtons();
  };
  const veUpdateButtons = () => {
    // ── EVERYTHING IS DEAD WHILE AN EXPORT RUNS ── veExport iterates the clip list and drives veVideo's own
    // currentTime/play/pause, and every mutator below calls THIS, which used to re-enable the export button
    // mid-run: a second click then started a concurrent veExport, so two MediaRecorders and two rVFC pumps
    // drove one <video>, both downloads came out scrambled, and whichever finished first cleared VE.exporting
    // out from under the other. Gating here rather than at each call site covers the buttons; the guards on
    // veSplit/veDelete/veHandleDown cover the paths that do not go through a button.
    const has = VE.clips.length > 0 && !VE.exporting;
    vePlayBtn.disabled = !has; $('veSplit').disabled = !has; $('veExport').disabled = !has; $('veSnap').disabled = !has;
    $('veDel').disabled = VE.sel < 0 || !has;
    veSizeSel.disabled = !has;
    veUpdateEst();                       // trimming, splitting and deleting all change the exported DURATION, so the estimate has to follow every one of them
  };

  // ── EXPORT ── play the kept clips through the <video> and record ITS stream (picture + audio), so the trimmed export
  // carries sound too. The "recording" banner is a separate DOM element, never part of the element's stream. ──
  // ── RED ANNOTATION LAYER ── strokes are stored in NORMALISED video space (0..1), so they survive panel resizes and
  // land on the exact same pixels in the export whatever the stage size is.
  const veDraw = $('veDraw'), vePenBtn = $('vePen');
  const veFitDraw = () => { const r = veDraw.getBoundingClientRect();       // keep the backing store at CSS size for crisp lines
    const w = Math.max(2, Math.round(r.width)), h = Math.max(2, Math.round(r.height));
    if (veDraw.width !== w || veDraw.height !== h) { veDraw.width = w; veDraw.height = h; }
    veRedraw(); };
  const veVidBox = () => {                                 // where the VIDEO actually sits inside the stage (object-fit: contain letterboxes it)
    const cw = veDraw.width, ch = veDraw.height;
    const vw = veVideo.videoWidth || 16, vh = veVideo.videoHeight || 9;
    const s = Math.min(cw / vw, ch / vh);
    const w = vw * s, h = vh * s;
    return { x: (cw - w) / 2, y: (ch - h) / 2, w, h };
  };
  const veRedraw = () => {                                 // repaint every stroke from normalised space
    const g = veDraw.getContext('2d'); g.clearRect(0, 0, veDraw.width, veDraw.height);
    const b = veVidBox();
    veStrokePaint(g, b.x, b.y, b.w, b.h, Math.max(1.5, b.w * 0.004));
  };
  const veStrokePaint = (g, ox, oy, w, h, lw) => {         // shared by the on-screen layer AND the export compositor
    g.save(); g.strokeStyle = '#ff2d3f'; g.lineWidth = lw; g.lineCap = 'round'; g.lineJoin = 'round';
    for (const st of VE.strokes) { if (st.length < 1) continue;
      g.beginPath(); g.moveTo(ox + st[0].x * w, oy + st[0].y * h);
      if (st.length === 1) g.lineTo(ox + st[0].x * w + 0.01, oy + st[0].y * h);
      else for (let i = 1; i < st.length; i++) g.lineTo(ox + st[i].x * w, oy + st[i].y * h);
      g.stroke(); }
    g.restore();
  };
  const vePenPos = (e) => { const r = veDraw.getBoundingClientRect(), b = veVidBox();
    const px = (e.clientX - r.left) * (veDraw.width / Math.max(1, r.width)) - b.x;
    const py = (e.clientY - r.top) * (veDraw.height / Math.max(1, r.height)) - b.y;
    return { x: px / Math.max(1, b.w), y: py / Math.max(1, b.h) };   // normalised to the VIDEO, not the stage
  };
  veDraw.addEventListener('pointerdown', (e) => { if (!VE.pen) return; e.stopPropagation(); e.preventDefault();
    VE.drawing = true; VE.undone.length = 0; VE.strokes.push([vePenPos(e)]); veDraw.setPointerCapture(e.pointerId); veRedraw(); });   // a fresh stroke invalidates the redo stack
  veDraw.addEventListener('pointermove', (e) => { if (!VE.pen || !VE.drawing) return; e.stopPropagation();
    VE.strokes[VE.strokes.length - 1].push(vePenPos(e)); veRedraw(); });
  const veEndStroke = () => { VE.drawing = false; };
  veDraw.addEventListener('pointerup', veEndStroke); veDraw.addEventListener('pointercancel', veEndStroke);
  vePenBtn.addEventListener('click', (e) => { e.stopPropagation(); VE.pen = !VE.pen;
    vePenBtn.classList.toggle('on', VE.pen); veStage.classList.toggle('pen', VE.pen); });
  $('veErase').addEventListener('click', (e) => { e.stopPropagation(); VE.strokes.length = 0; VE.undone.length = 0; veRedraw(); });
  // ── UNDO / REDO the red strokes (user) ── Ctrl/⌘+Z pops the last stroke onto a redo stack; Ctrl/⌘+Shift+Z (or Ctrl+Y) puts it back. Only while the video editor is open; capture phase so it beats the game's key handling.
  window.addEventListener('keydown', (e) => {
    if (vePanel.classList.contains('hidden')) return;              // only inside the video editor
    if (!(e.ctrlKey || e.metaKey)) return;
    const k = e.key.toLowerCase();
    if (k === 'z' && !e.shiftKey) { e.preventDefault(); e.stopPropagation(); if (VE.strokes.length) { VE.undone.push(VE.strokes.pop()); veRedraw(); } }
    else if (k === 'y' || (k === 'z' && e.shiftKey)) { e.preventDefault(); e.stopPropagation(); if (VE.undone.length) { VE.strokes.push(VE.undone.pop()); veRedraw(); } }
  }, true);

  // ═══════════════ THE WEBCODECS EXPORT ═══════════════ (user 2026-08-21: "on playback the video is STILL stuttering")
  // The MediaRecorder export below replays the take in real time and re-records whatever the compositor happens to
  // present. That is the stutter, and it cannot be tuned out — see the measurement in ui/mp4-mux.js: every frame
  // interval in every shipped export is an integer multiple of the 8.33 ms refresh period, because MediaRecorder
  // stamps frames with the wall clock rather than with the time the frame actually belongs at.
  // This path never plays anything. It DEMUXES the recording, decodes every sample, redraws it with the annotations,
  // and hands each frame to a VideoEncoder with an EXACT timestamp on an even grid. Consequences worth stating:
  //   * frame pacing is perfect by construction — the output grid is arithmetic, not measured
  //   * nothing can be dropped: a slow decode makes the export take longer, it does not cost a frame
  //   * it is not bound to real time, so a 60 s take need not take 60 s
  //   * the audio is COPIED, not re-encoded — one whole generation of AAC loss removed
  //   * the file comes out flat, so it opens in an NLE without the remux
  // Returns true when it produced the download. False means "not applicable" and the caller falls through to the old
  // path — a webm recording, or a browser without WebCodecs. It must never throw its way out: the take is precious.
  const veCodecStr = (avcC) => 'avc1.' + [avcC[1], avcC[2], avcC[3]].map((b) => ('0' + b.toString(16)).slice(-2)).join('');
  const VE_ENC_TRY = ['avc1.640034', 'avc1.640033', 'avc1.640032', 'avc1.64002a', 'avc1.4d0034', 'avc1.42e034'];   // High 5.2 down to Baseline; the first the machine will actually configure wins
  const veTick = () => new Promise((r) => setTimeout(r, 0));
  const VE_FILL_MAX = 240;                                // ceiling on repeated frames for one gap: a pathological stall must not inflate the file without bound. 240 is 4 s at 60 fps — far past any real hitch
  // Pull samples through a sliding ~24 MB window, so a 900 MB take never lands in JS memory and the decoder is not fed
  // one blob read per frame.
  const veReader = (blob) => { let base = -1, buf = null;
    return async (off, size) => {
      if (base < 0 || off < base || off + size > base + buf.byteLength) {
        base = off; buf = await blob.slice(off, Math.min(blob.size, off + Math.max(24e6, size))).arrayBuffer();
      }
      return new Uint8Array(buf, off - base, size);
    }; };

  const veExportWC = async () => {
    // ── SAY WHY, ALWAYS ── these were bare `return false`s, so declining looked exactly like never being called and
    // the export quietly ran on the stuttering MediaRecorder path instead. That cost a whole debugging round when
    // the recorder started muxing FLAT mp4s and this parser still only understood fragmented ones.
    const bail = (why) => { console.warn('[vb] WebCodecs export unavailable —', why, '— using MediaRecorder'); return false; };
    if (!veWCReady()) return bail('no WebCodecs');
    if (!VE.blob) return bail('no recording blob');
    if (!VE.clips.length || veExportDur() <= 0) return bail('nothing on the timeline');
    let P = null;
    try { P = await veMp4Parse(VE.blob); } catch (e) { return bail('parse threw: ' + e); }
    if (!P) return bail('parse returned null — a webm take, or an mp4 shape this cannot read');
    const vt = P.live.find((t) => t.kind === 'video'), at = P.live.find((t) => t.kind === 'audio');
    if (!vt) return bail('no video track');
    if (!vt.avcC) return bail('no avcC in the video sample entry');
    if (!vt.samples.length) return bail('video track has no samples');

    // ── SELECT, THEN MEASURE, THEN DECODE ── the output frame duration depends on the total frame count, so every
    // clip's sample range is resolved up front. `from` walks back to the keyframe the first wanted frame needs.
    const tOf = (t, s) => (s.dts - t.t0) / t.mediaTs;
    const sel = [];
    for (const c of VE.clips) {
      const idx = [];
      for (let i = 0; i < vt.samples.length; i++) { const q = tOf(vt, vt.samples[i]); if (q >= c.s - 1e-4 && q < c.e - 1e-4) idx.push(i); }
      if (!idx.length) continue;
      let from = idx[0];
      while (from > 0 && !vt.samples[from].sync) from--;   // decoding may not start mid-GOP; the frames before the cut are decoded and thrown away
      sel.push({ c, first: idx[0], last: idx[idx.length - 1], from, n: idx.length, a: [] });   // the audio range is chosen below, once the frames have said how much of this clip they actually cover
    }
    const nFrames = sel.reduce((k, s) => k + s.n, 0);
    if (!nFrames) return bail('no frames fall inside the kept clips');
    const durSec = sel.reduce((k, s) => k + (s.c.e - s.c.s), 0);
    const VE_TS = 1000000;                                // a microsecond timescale: the grid step below then rounds to well under a millisecond of drift across a whole take
    // ── THE GRID STEP IS SIZED FROM THE VIDEO TRACK'S OWN SPAN ── NOT from durSec/nFrames. Those look equivalent
    // and are not: `durSec` comes off veVideo.duration, which is the longer of the two TRACKS, and the audio track
    // keeps running when the paint loop stalls. Measured on a stalled take: video timestamps spanned 3.01 s while
    // the container reported 7.94 s, so dividing gave a step 2.6x too wide and laid 3 s of video across 8 s — slow
    // motion, with the copied audio walking away from it. Summing what the FRAMES actually cover cannot drift that
    // way. The median gap only supplies the tail, since the last frame of a clip has no successor to measure against.
    const gaps = [];
    for (const S of sel) for (let i = S.first; i < S.last; i++) gaps.push((vt.samples[i + 1].dts - vt.samples[i].dts) / vt.mediaTs);
    gaps.sort((a, b) => a - b);
    const medGap = gaps.length ? gaps[gaps.length >> 1] : durSec / nFrames;
    // ── AUDIO FOLLOWS THE PICTURE ── each clip's audio is taken over what its FRAMES cover, not over the clip's
    // nominal length. The two are the same for a healthy take and diverge badly for a short-captured one: the audio
    // track is a real-time MediaStream and keeps running when the video track falls behind, so selecting on the
    // nominal range exported 4.9 s of sound against 1.3 s of picture. Sound now stops where the picture stops.
    for (const S of sel) {
      S.lo = tOf(vt, vt.samples[S.first]);
      S.span = tOf(vt, vt.samples[S.last]) - S.lo + medGap;
      S.a = at ? at.samples.filter((s) => { const q = tOf(at, s); return q >= S.lo - 1e-4 && q < S.lo + S.span - 1e-4; }) : [];
    }
    let baseSlot = 0;                                     // where each clip starts on the output grid; clips butt together
    for (const S of sel) { S.base = baseSlot; baseSlot += Math.round(S.span * 1e6 / Math.max(1, Math.round(medGap * VE_TS))); }
    // ── THE GRID STEP IS THE TAKE'S TYPICAL CADENCE (the median gap), NOT its average ── the average is dragged
    // down by every long gap, which then makes the base rate lower than the rate the game actually painted at and
    // forces most frames into a slot they do not belong in. The median is what "one frame" really cost, so a steady
    // stretch maps one frame to one slot and only a genuine hitch spans several.
    // The step is the 25th-percentile gap, floored at 144 fps and never coarser than the median. For a steady take
    // (a real capture is very nearly constant) p25, the median and the mean are the same number and this changes
    // nothing. For a BURSTY one it matters: a step at the median sends every gap shorter than typical into a slot
    // that is already taken, and those frames are then dropped. Measured on a bursty test take, a median step lost
    // 112 of 641 unique frames; p25 keeps them. Duplicates cost ~130 bytes, dropped frames cost content.
    const pct = (f) => gaps.length ? gaps[Math.min(gaps.length - 1, Math.floor(gaps.length * f))] : medGap;
    const frameDur = Math.max(Math.round(VE_TS / 144), Math.min(Math.round(medGap * VE_TS), Math.max(1, Math.round(pct(0.25) * VE_TS))));

    const btn = $('veExport'), label0 = btn.textContent;
    btn.disabled = true; btn.textContent = 'exporting…'; vePause();
    const VESC = (() => { const m = /[?&]vescale=([0-9.]+)/.exec(location.search); return m ? Math.max(0.25, Math.min(1, parseFloat(m[1]))) : 1; })();
    const vw = Math.round((veVideo.videoWidth || 1280) * VESC) & ~1, vh = Math.round((veVideo.videoHeight || 720) * VESC) & ~1;
    const cvs = document.createElement('canvas'); cvs.width = vw; cvs.height = vh;
    const g = cvs.getContext('2d', { alpha: false, willReadFrequently: false });   // ── NOT `desynchronized` ── that flag drops the internal double buffer, which is fine for MediaRecorder's requestFrame() but NOT for `new VideoFrame(canvas)`: with no stable back buffer the constructor kept handing the encoder a STALE picture, so the export encoded the same frame over and over. Measured: 89% of the exported frames were duplicates and the download played at 2.15 unique fps
    const lw = Math.max(2, vw * 0.004);
    const reqRate = veExportRate(vw, vh, durSec).rate;
    let dec = null, enc = null, failed = null, outIdx = 0, desc = null, drew = false;
    const encSamples = [], encData = [];
    VE.exporting = true; veUpdateButtons();
    try {
      let encCfg = null;
      for (const codec of VE_ENC_TRY) {
        const cfg = { codec, width: vw, height: vh, bitrate: Math.round(reqRate), framerate: VE_TS / frameDur, avc: { format: 'avc' }, latencyMode: 'quality' };
        const sup = await VideoEncoder.isConfigSupported(cfg).catch(() => null);
        if (sup && sup.supported) { encCfg = cfg; break; }
      }
      if (!encCfg) return bail('no AVC encoder configuration at ' + vw + 'x' + vh);
      enc = new VideoEncoder({
        output: (ch, meta) => {
          if (!desc && meta && meta.decoderConfig && meta.decoderConfig.description) desc = new Uint8Array(meta.decoderConfig.description);
          const b = new Uint8Array(ch.byteLength); ch.copyTo(b);
          encSamples.push({ size: b.length, dur: frameDur, cts: 0, sync: ch.type === 'key' });
          encData.push(b);
        },
        error: (e) => { failed = failed || e; },
      });
      enc.configure(encCfg);
      const gop = Math.max(1, Math.round(2 * VE_TS / frameDur));   // a keyframe every ~2 s so the file scrubs in an NLE instead of forcing long GOP walks
      const ready = [];
      dec = new VideoDecoder({ output: (f) => ready.push(f), error: (e) => { failed = failed || e; } });
      dec.configure({ codec: veCodecStr(vt.avcC), description: vt.avcC, codedWidth: veVideo.videoWidth || vw, codedHeight: veVideo.videoHeight || vh, optimizeForLatency: false });
      const read = veReader(VE.blob);
      let clip = null;
      const emit = async (f) => {                         // one decoded source frame → at most one encoded output frame
        const q = f.timestamp / 1e6;
        if (clip && q >= clip.c.s - 1e-4 && q < clip.c.e - 1e-4) {
          // ── REAL TIMING, EMITTED AS TRUE CFR ── each frame goes in the slot its OWN timestamp puts it in, and
          // any slot it skipped is filled by repeating the frame that was already on screen.
          // Why not an even grid (tried, and it was worse — user: "much worse than before"): the recording's
          // timestamps are already honest. The game paints on rAF, so a frame stamped at 16.67 ms really is the
          // world at 16.67 ms. The stutter never came from those times, it came from the export RE-RECORDING them
          // through MediaRecorder's wall clock a second time. Renumbering frames 0,1,2,… therefore does not remove
          // an error, it INTRODUCES one: it plays a frame that covered 50 ms for the same span as one that covered
          // 16, so motion speeds up and slows down. Honest times must survive the export.
          // Why fill rather than leave the gaps: a variable-frame-rate file plays in a browser and stutters or
          // freezes in downloaded players — the 2026-07-22 finding, and the user hit it again ("when the person
          // downloads the file, it's even worse"). Repeating the held frame keeps every slot occupied, so the file
          // is exactly constant-rate while showing each frame for as long as it truly lasted. A repeat costs a few
          // hundred bytes because nothing in it changed.
          const slot = clip.base + Math.max(0, Math.round((q - clip.lo) * 1e6 / frameDur));
          if (slot < outIdx) { f.close(); return; }        // ── DROP, DON'T SHOVE ── this frame lands in a slot already taken, i.e. the capture delivered two frames closer together than one frame period, so one of them is temporally redundant. Giving it the NEXT slot instead pushes every later frame late and the whole clip stretches (measured: 250 ms of drift over 4.4 s). Dropping is what -fps_mode cfr does, and it is what keeps the running time honest
          for (let gi = 0; drew && outIdx < slot && gi < VE_FILL_MAX; gi++) {   // hold the PREVIOUS frame — this runs before the new one is drawn, and `drew` keeps it from firing before there is anything to hold
            const dup = new VideoFrame(cvs, { timestamp: outIdx * frameDur, duration: frameDur, alpha: 'discard' });
            enc.encode(dup, { keyFrame: outIdx % gop === 0 });
            dup.close(); outIdx++;
          }
          g.drawImage(f, 0, 0, vw, vh);
          if (VE.strokes.length) veStrokePaint(g, 0, 0, vw, vh, lw);
          drew = true;
          const of = new VideoFrame(cvs, { timestamp: outIdx * frameDur, duration: frameDur, alpha: 'discard' });
          enc.encode(of, { keyFrame: outIdx % gop === 0 });
          of.close();
          if (++outIdx % 15 === 0) btn.textContent = 'exporting ' + Math.min(99, Math.round(outIdx * 100 / nFrames)) + '%';
        }
        f.close();                                        // MANDATORY: a VideoFrame holds a GPU surface, and 4K frames exhaust the pool within a second or two of leaking them
      };
      for (const S of sel) {
        clip = S;
        outIdx = Math.max(outIdx, S.base);                 // clips butt together on the output grid; never start one behind where the last ended
        for (let i = S.from; i <= S.last && !failed; i++) {
          const s = vt.samples[i];
          dec.decode(new EncodedVideoChunk({ type: s.sync ? 'key' : 'delta', timestamp: Math.round(tOf(vt, s) * 1e6),
                                             duration: Math.round(s.dur / vt.mediaTs * 1e6), data: await read(s.off, s.size) }));
          while (ready.length) await emit(ready.shift());
          while (!failed && (dec.decodeQueueSize > 8 || enc.encodeQueueSize > 8)) { await veTick(); while (ready.length) await emit(ready.shift()); }   // BACKPRESSURE, not a buffer: without it a 4K take queues thousands of frames and the tab is killed for memory
        }
        if (failed) break;
        await dec.flush();
        while (ready.length) await emit(ready.shift());
        outIdx = Math.max(outIdx, S.base + Math.round(S.span * 1e6 / frameDur));   // the next clip starts after everything this one covered, so a trimmed-out stretch does not leave a hole
      }
      if (failed) throw failed;
      await enc.flush();
      // every sample already carries frameDur: the output is constant-rate by construction, so stts is one row
      if (!desc || !encSamples.length) return bail('encoder produced no avcC/samples');
      const tracks = [{ kind: 'video', timescale: VE_TS, width: vw, height: vh, stsd: veAvcStsd(vw, vh, desc), samples: encSamples, data: encData }];
      if (at && at.stsd) {
        const aS = [], aD = [];
        for (const S of sel) for (const s of S.a) { aS.push({ size: s.size, dur: s.dur, cts: 0, sync: true }); aD.push(VE.blob.slice(s.off, s.off + s.size)); }   // ── AUDIO IS COPIED ── the AAC the recorder already made, passed through as Blob slices: no second encode, no quality loss, and no memory
        if (aS.length) tracks.push({ kind: 'audio', timescale: at.mediaTs, stsd: P.moovRaw.slice(at.stsd.start, at.stsd.end), samples: aS, data: aD });
      }
      const out = veMp4Mux(tracks, 1000);
      if (!out) return bail('mux failed');
      veCalibrate(out.size, durSec, reqRate);
      const url = URL.createObjectURL(out), a2 = document.createElement('a');
      a2.href = url; a2.download = 'voxelbit-clip.mp4'; document.body.appendChild(a2); a2.click(); a2.remove();
      setTimeout(() => URL.revokeObjectURL(url), 4000);
      veUpdateEst(); if (VE.clips.length) veSeek(VE.clips[0].s);
      return true;
    } catch (e) {
      console.warn('[vb] WebCodecs export failed — falling back to the recorder', e);
      return false;
    } finally {
      try { if (dec && dec.state !== 'closed') dec.close(); } catch (e) {}
      try { if (enc && enc.state !== 'closed') enc.close(); } catch (e) {}
      VE.exporting = false;
      btn.disabled = false; btn.textContent = label0;
      veUpdateButtons();
    }
  };

  // ── EXPORT ── COMPOSITED, not a raw element capture. The old path recorded veVideo.captureStream() while the clip
  // played in real time, so anything that hitched the page during those seconds — above all the game still rendering
  // full-tilt behind the panel — was burned into the file as dropped/long frames. That is why it looked fine in the
  // editor but juddered in the download. Now each presented video frame is drawn (plus the annotations) onto an
  // offscreen canvas and PUSHED to the recorder via requestFrame(), so the output carries exactly the frames the
  // decoder produced, and the game loop is suspended for the duration so nothing competes for the GPU.
  const veExport = async () => {
    if (!VE.clips.length || VE.dur <= 0) return;
    if (await veExportWC()) return;                       // ── THE DEFAULT PATH ── everything below is the MediaRecorder fallback, kept for webm takes and for any browser without WebCodecs. It is the one that stutters; see veExportWC
    const mime = vePickExportMime(); if (!mime) return;
    const ext = mime.indexOf('mp4') >= 0 ? 'mp4' : 'webm';   // download extension follows the container the recorder actually produced
    const btn = $('veExport'), label0 = btn.textContent; btn.disabled = true; btn.textContent = 'exporting…'; vePause();
    // ?vescale=N shrinks the ENCODE without touching the recording. The export replays the clip in real
    // time and captures presented frames, so if decode+encode cannot keep 60 fps the missing frames are
    // gone for good — this is the lever for finding out whether encode load is what costs them.
    const VESC = (() => { const m = /[?&]vescale=([0-9.]+)/.exec(location.search); return m ? Math.max(0.25, Math.min(1, parseFloat(m[1]))) : 1; })();
    const vw = Math.round((veVideo.videoWidth || 1280) * VESC) & ~1, vh = Math.round((veVideo.videoHeight || 720) * VESC) & ~1;
    const cvs = document.createElement('canvas'); cvs.width = vw; cvs.height = vh;
    const g = cvs.getContext('2d', { alpha: false, desynchronized: true });
    const lw = Math.max(2, vw * 0.004);
    const EXPORT_FPS = 60;
    let stream, track;
    try { stream = cvs.captureStream(0); track = stream.getVideoTracks()[0]; }   // 0 = MANUAL push, one per PRESENTED source frame (rVFC pump below) + a hold-last-frame watchdog; seeks spliced out via rec.pause()/resume(). Rejected alternatives: (1) captureStream(60) auto-sampling emits NOTHING while the canvas is static → ~1 s frozen gap at the head; (2) a wall-clock fixed-rate pump BEAT against the media clock → periodic dup/drop stutter; (3) an rAF media-clock pump RACED the decoder's presentation by ±1 frame → content dup/skip (the "still slightly jittery" bug). rVFC is safe now that EXPORT QUIET MODE stops the game from contending the decoder — the pre-gate rVFC attempt failed only because of that contention.
    catch (e) { console.warn('[vb] export captureStream failed', e); btn.disabled = false; btn.textContent = label0; return; }
    const wasMuted = veVideo.muted; veVideo.muted = false;
    let pump = null, alive = true, rec = null, aborted = false;   // hoisted ABOVE the try/finally below, so the teardown can always reach them however the run ends
    try {
      try { const a = veVideo.captureStream ? veVideo.captureStream() : (veVideo.mozCaptureStream ? veVideo.mozCaptureStream() : null);   // carry the ORIGINAL audio track across
        if (a) for (const t of a.getAudioTracks()) stream.addTrack(t); } catch (e) {}
      const expDur = veExportDur(), reqRate = veExportRate(vw, vh, expDur).rate;
      rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: reqRate, audioBitsPerSecond: 192000 });   // the FINAL encode — rate comes from the size cap in the toolbar, falling back to a quality target when the cap is "best quality"
      const chunks = []; rec.ondataavailable = (e) => { if (e.data && e.data.size) chunks.push(e.data); };
      const stopped = new Promise((res) => { rec.onstop = res; });
      const pushFrame = () => {                              // composite the CURRENT video frame + the red annotations, then PUSH it to the recorder
        g.drawImage(veVideo, 0, 0, vw, vh);
        if (VE.strokes.length) veStrokePaint(g, 0, 0, vw, vh, lw);
        if (track.requestFrame) track.requestFrame();
      };
      VE.exporting = true;                                   // suspends the game's render work (see the frame loop) so the pump runs uncontended and playback cannot hitch
      veUpdateButtons();                                     // …and greys the editor out, so nothing can mutate the clip list this run is walking (see veUpdateButtons)
      await veSeekAwait(VE.clips[0].s);
      pushFrame(); rec.start();
      let lastPush = 0;                                      // wall-clock of the last pushed frame — the watchdog's baseline
      const onVF = () => { if (!alive) return;               // ── PRESENTATION-LOCKED PUMP (user: "still slightly jittery — only after download") ── any rAF-timed pump samples the video on ITS OWN tick, racing the decoder's presentation by ±1 frame: sometimes it grabs frame N, sometimes still N-1 → content dup/skip in the file even with even timestamps. rVFC fires exactly ONCE per PRESENTED frame, so every push is the frame the decoder actually showed, on the compositor's even 60 Hz cadence. Safe now that the render loop is fully gated during export (EXPORT QUIET MODE) — the old rVFC rejection predates that gate, when the game contended the decoder into irregular presentation.
        g.drawImage(veVideo, 0, 0, vw, vh);
        if (VE.strokes.length) veStrokePaint(g, 0, 0, vw, vh, lw);
        if (!veVideo.paused && !veVideo.seeking && rec.state === 'recording' && track.requestFrame) { track.requestFrame(); lastPush = performance.now(); }
        veVideo.requestVideoFrameCallback(onVF); };
      const RVFC = !!veVideo.requestVideoFrameCallback;
      if (RVFC) veVideo.requestVideoFrameCallback(onVF);
      let clipBase = 0, clipPushed = 0;                      // fallback media-clock pump state (no-rVFC browsers only)
      const drive = () => { if (!alive) return;
        if (!veVideo.paused && !veVideo.seeking && rec.state === 'recording') {
          if (RVFC) { if (performance.now() - lastPush > 50 && track.requestFrame) { lastPush = performance.now(); track.requestFrame(); } }   // WATCHDOG: a gap recorded at CAPTURE time presents nothing during playback — re-push the held frame so the file never has a timestamp hole
          else { const target = Math.floor((veVideo.currentTime - clipBase) * EXPORT_FPS);
            if (target > clipPushed) { pushFrame(); clipPushed = target; } }   // no-rVFC fallback: push when the MEDIA clock crosses a 1/60 boundary; resync on a miss (a dropped frame is invisible, duplicate-then-catch-up is the stutter)
        }
        pump = requestAnimationFrame(drive); };
      pump = requestAnimationFrame(drive);
      for (const clip of VE.clips.slice()) {   // SNAPSHOT: a splice on the live array mid-iteration makes for..of skip the next clip outright, and the download silently omits a segment
        if (rec.state === 'recording') rec.pause();          // SPLICE the seek out of the file — the old pump filled every clip boundary with ~0.5 s of frozen duplicate frames; pausing the recorder stops its clock so the next clip butts on seamlessly
        await veSeekAwait(clip.s); clipBase = clip.s; clipPushed = 0;
        await veCap(veVideo.play().catch(() => {}), 3000, 'play()');
        if (rec.state === 'paused') rec.resume();
        if (!RVFC) pushFrame();                              // fallback only: land the clip's first frame now (the media clock hasn't crossed a boundary yet). On the rVFC path the seek's own presentation already drew it — pushing here could emit the PREVIOUS clip's stale canvas
        // …and TERMINATE THE MOMENT PLAYBACK STOPS. Nothing disables the close paths during an export: Escape, the ✕ and a
        // backdrop click all reach veClose → vePause → veVideo.pause(), and paused mid-clip `currentTime >= clip.e` can never
        // become true — the loop spun forever, VE.exporting was never cleared, and the frame loop skipped every frame after
        // that, so the game froze on its last presented frame until the page was reloaded.
        await veCap(new Promise((res) => { const chk = () => { if (!alive || veVideo.paused || veVideo.ended || veVideo.currentTime >= clip.e - 0.02) res(); else requestAnimationFrame(chk); }; requestAnimationFrame(chk); }), (clip.e - clip.s) * 1000 + 5000, 'clip end');   // the cap is the clip's own length plus slack: this wait is SUPPOSED to last the clip, so a fixed timeout would cut long ones short
        aborted = veVideo.paused && !veVideo.ended && veVideo.currentTime < clip.e - 0.02;   // stopped SHORT of the clip's end = someone closed the panel (or hit ❚❚) mid-run; reaching the end pauses too, hence the position test
        veVideo.pause();
        if (aborted) break;
      }
      alive = false;
      cancelAnimationFrame(pump); pump = null;
      rec.stop(); await veCap(stopped, 5000, 'recorder stop');
      if (aborted) return;                                 // closed mid-run: no truncated download the player never asked for, and no veCalibrate off a part-length file (the finally still releases VE.exporting and the button)
      let out = new Blob(chunks, { type: ext === 'mp4' ? 'video/mp4' : 'video/webm' });
      veCalibrate(out.size, expDur, reqRate);              // what the encoder ACTUALLY did → next export's estimate is this machine's, not a hardcoded guess. Measured on the RAW recorder output, BEFORE the remux below, because the encoder is what it is calibrating
      if (ext === 'mp4') { try { const flat = await veRemuxFmp4(out); if (flat) out = flat; } catch (e) { console.warn('[vb] remux failed — downloading the fragmented original', e); } }   // ── SO THE FILE OPENS IN AN NLE ── MediaRecorder only emits a FRAGMENTED mp4 and DaVinci Resolve refuses those outright (user 2026-08-21: dragging an export in did nothing). See ui/mp4-remux.js: it is a lossless table rebuild, 25 ms on a 360 MB export, and any failure falls through to the original rather than costing the user their take
      const url = URL.createObjectURL(out), a2 = document.createElement('a');
      a2.href = url; a2.download = 'voxelbit-clip.' + ext; document.body.appendChild(a2); a2.click(); a2.remove();
      setTimeout(() => URL.revokeObjectURL(url), 4000);
      veUpdateEst(); if (VE.clips.length) veSeek(VE.clips[0].s);
    } finally {                                        // ── ALWAYS RELEASE ── VE.exporting gates the ENTIRE frame loop, so any throw above (a MediaRecorder that refuses the mime, a lost blob, a device reset) left the game frozen on its last presented frame with the export button stuck disabled. Every exit path now lands here.
      alive = false; if (pump) cancelAnimationFrame(pump);
      try { if (rec && rec.state !== 'inactive') rec.stop(); } catch (e) {}
      try { for (const t of stream.getTracks()) t.stop(); } catch (e) {}   // release the export compositor's capture + the borrowed audio track
      veVideo.muted = wasMuted; VE.exporting = false;   // the two pieces of state that MUST NOT survive the call
      btn.disabled = false; btn.textContent = label0;
      veUpdateButtons();                               // …and hands the editor back. AFTER VE.exporting is cleared, or it would re-disable everything it just restored
    }
  };
  // ── NO AWAIT INSIDE THE PAUSE/RESUME WINDOW MAY BE UNBOUNDED ── the recorder is PAUSED across the seek at the
  // top of every clip, so any wait between rec.pause() and rec.resume() that never settles leaves it paused for
  // good: the file truncates there, MediaRecorder reports nothing at all, and VE.exporting stays latched — which
  // gates the entire render pass chain in tick-passes.js, so the game freezes on its last presented frame until
  // the page is reloaded. Three waits could do it and none was bounded: a play() promise that never settles on a
  // decode stall (.catch handles rejection, not non-resolution), the end-of-clip poll whose three conditions are
  // ALL false while the media clock is frozen (a stall is not paused, not ended and not advancing), and a
  // recorder wedged by a device reset that never fires onstop. Resolves rather than rejects on purpose: a
  // timed-out export should finish with the clips it has already written, not throw them away.
  const veCap = (pr, ms, what) => Promise.race([pr, new Promise((res) => setTimeout(() => {
    console.warn('[vb] export: ' + what + ' timed out after ' + Math.round(ms) + ' ms - continuing'); res(); }, ms))]);
  const veSeekAwait = (t) => new Promise((res) => {   // ── THE LAST UNBOUNDED WAIT ── every other await in veExport now terminates on pause/abort, and this one
    let done = false;                                //  must too: if the element never fires 'seeked' (a src swapped out from under it, a decode stall) the export's
    const fin = () => { if (done) return; done = true; clearTimeout(tm); veVideo.removeEventListener('seeked', fin); res(); };   //  try/finally never runs and VE.exporting stays latched, which freezes the frame loop for the session.
    const tm = setTimeout(fin, 3000);                 // 3 s is ~30x a normal seek on a local blob; giving up and pushing on beats hanging for ever
    veVideo.addEventListener('seeked', fin);
    veVideo.currentTime = Math.max(0, Math.min(VE.dur - 1e-3, t)); });

  // ── OPEN / CLOSE + BUTTON WIRING ──
  const veOpen = () => { vePanel.classList.remove('hidden'); lockEl.classList.add('hidden');   // the editor takes over the screen: hide the esc menu behind it and RELEASE the pointer so the mouse drives the UI, not the camera (the R-stop path leaves the game pointer-locked — user: "the mouse is still connected to the player camera in the background")
    if (document.pointerLockElement) document.exitPointerLock();
    veRenderTimeline(); veUpdatePlayhead(); veUpdateTime(); veUpdateButtons(); requestAnimationFrame(veFitDraw); };
  const veClose = () => { vePanel.classList.add('hidden'); vePause(); if (!locked && !dead) lockEl.classList.remove('hidden'); };   // hand the screen back to the esc menu so the player resumes / re-locks from there
  veBtnEl.addEventListener('click', (e) => { e.stopPropagation(); veOpen(); });   // opens the editor; recording is the ● rec button inside the panel
  veRecBtn.addEventListener('click', (e) => { e.stopPropagation(); veToggleRec(); });
  // ── SNAPSHOT (user 2026-08-19: "add a button to the recorder that lets me take a snapshot of the video as a
  // .png file") ── the frame sitting at the playhead, at the video's OWN resolution rather than at whatever size
  // the editor happens to be showing it, so a snapshot of a 4K capture is 4K. It reuses veStrokePaint — the same
  // painter the on-screen layer and the export compositor use — so the red annotations land in the .png exactly
  // where they are on screen: they live in normalised video space, so painting them at (0, 0, vw, vh) is the
  // whole of the mapping. Line width follows the frame width on the same 0.004 the editor uses, or a hairline
  // stroke authored on a small preview would come out invisible at full size.
  // NOT while an export is running: veExport drives veVideo's own currentTime through the clip list, so the
  // frame on screen mid-export belongs to the export, not to the playhead the player is looking at.
  const veSnapshot = () => {
    if (VE.exporting) return;
    const vw = veVideo.videoWidth | 0, vh = veVideo.videoHeight | 0;
    if (!vw || !vh) return;
    const c = document.createElement('canvas'); c.width = vw; c.height = vh;
    const g = c.getContext('2d');
    g.drawImage(veVideo, 0, 0, vw, vh);
    veStrokePaint(g, 0, 0, vw, vh, Math.max(1.5, vw * 0.004));
    c.toBlob((blob) => {
      if (!blob) return;
      const url = URL.createObjectURL(blob), a3 = document.createElement('a');
      a3.href = url; a3.download = 'voxelbit-frame.png'; document.body.appendChild(a3); a3.click(); a3.remove();
      setTimeout(() => URL.revokeObjectURL(url), 4000);   // the same grace the clip download gets
    }, 'image/png');
  };
  $('veSnap').addEventListener('click', (e) => { e.stopPropagation(); veSnapshot(); });
  $('veExport').addEventListener('click', (e) => { e.stopPropagation(); veExport(); });
  $('veClose').addEventListener('click', (e) => { e.stopPropagation(); veClose(); });
  vePlayBtn.addEventListener('click', (e) => { e.stopPropagation(); veTogglePlay(); });
  $('veSplit').addEventListener('click', (e) => { e.stopPropagation(); veSplit(); });
  $('veDel').addEventListener('click', (e) => { e.stopPropagation(); veDelete(); });
  vePanel.addEventListener('click', (e) => { e.stopPropagation(); if (e.target === vePanel) veClose(); });
  document.addEventListener('keydown', (e) => { if (e.code === 'Escape' && !vePanel.classList.contains('hidden')) { e.stopPropagation(); veClose(); } }, true);
  window.addEventListener('resize', () => { if (!vePanel.classList.contains('hidden')) { veRenderTimeline(); veUpdatePlayhead(); veFitDraw(); } });
  veUpdateButtons();

  const renderDist = RD_FIXED;                         // FIXED 100 m — the slider is gone (user), so nothing reassigns this
  const BASE_VIG = 0.26;                               // always-on vignette in normal play — well under the cinematic 0.55 so it frames the view without reading as an effect
