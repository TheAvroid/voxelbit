  // @module — screen recording, the clip timeline, red annotations, the export
  // @exports BASE_VIG, VE, VE_CAP_MS, renderDist, veAC, veAudioDest, vePanel, veTapEl, veToggleRec
  // ═══════════════ VIDEO EDITOR + SCREEN RECORDER ═══════════════ (user)
  // Capture the game canvas to a .webm with MediaRecorder, then trim / cut / delete / resize clips on a timeline and
  // export the result. Entirely self-contained DOM + MediaRecorder work — nothing here touches the WebGPU render loop.
  // The "recording" banner is a DOM element (never drawn on the canvas), so it can never appear in the capture/export.
  const VE = { clips: [], dur: 0, sel: -1, blobUrl: null, thumb: '', playing: false, lastPaint: 0,
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
  const veBitrate = (w, h) => Math.min(100e6, Math.max(28e6, Math.round((w || 1280) * (h || 720) * 60 * 0.2)));   // QUALITY (user: "download looks lower"): the export is a SECOND encode on top of the recording (trim+annotate needs a re-compress), so 16 Mbps compounded into visible loss. Scale the bitrate to the actual resolution × 60 fps at ~0.2 bpp so BOTH the recording and the re-encode stay near-transparent: ~28 Mbps at 1080p, ~44 at 1440p, capped at 100 for 4K/high-DPI. Bigger files, but the download now matches the preview. STILL the RECORDING rate — the master wants to be generous because the export re-encodes it. The EXPORT rate is veExportRate() below.

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
  try { const s = parseFloat(localStorage.getItem('vb_ve_overshoot')); if (isFinite(s) && s >= 1 && s <= 3) veOvershoot = s; } catch (e) {}
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
    if (!isFinite(seen) || seen < 1 || seen > 3) return;
    veOvershoot = Math.round((veOvershoot + seen) / 2 * 1000) / 1000;
    try { localStorage.setItem('vb_ve_overshoot', String(veOvershoot)); } catch (e) {}
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
  let veAC = null, veGameDest = null;
  const veTapEl = (a) => { if (!veAC || a._veTap) return;
    try {
      if (a._sfxOut) { a._sfxOut.connect(veGameDest); a._veTap = true; return; }   // already routed through an effect (see bassTap) — tap its OUTPUT, or the recorder would capture the dry element twice and the filtered one never
      const s = veAC.createMediaElementSource(a); s.connect(veAC.destination); s.connect(veGameDest); a._veTap = true;
    } catch (e) {} };
  const veAudioDest = () => {
    try {
      if (!veAC) { veAC = audioCtx(); if (!veAC) return null; veGameDest = veAC.createMediaStreamDestination(); }   // the SHARED context — see audioCtx
      if (typeof sndReg !== 'undefined') for (const s of sndReg) veTapEl(s.a);
      if (veAC.state === 'suspended') veAC.resume();
      return veGameDest;
    } catch (e) { console.warn('[vb] audio tap failed', e); return null; }
  };

  // ── SCREEN RECORDING ──
  // captureStream(60) samples on its own 16.67 ms schedule and timestamps by the WALL CLOCK of whatever
  // paint it lands on. The game paints every rAF - 8.3 ms at 120 Hz - so the two beat against each
  // other and the recording gets 8/16/17/25/33 ms intervals plus dropped frames (measured: 46 in 5 s).
  // That uneven spacing IS the judder. MediaRecorder ignores explicit VideoFrame timestamps (verified
  // by experiment), so it cannot be corrected downstream - the paint cadence has to be even at the
  // source, which is what VE_CAP_MS does in the render loop.
  const VE_CAP_MS = 1000 / 60 - 0.6;                   // one paint per capture slot, a hair under so a slot is never missed
  const veStartRec = () => {
    if (VE.recording) return;
    const mime = veCaptureMime(); if (!mime) { console.warn('[vb] MediaRecorder unsupported'); return; }   // hardware H.264 first → doesn't contend with the render loop the way software VP9 did
    let stream; try { stream = canvas.captureStream(60); } catch (e) { console.warn('[vb] canvas.captureStream failed', e); return; }
    VE.recStream = stream;                             // kept so veStopRec can release the canvas capture (see there)   // 60 fps → matches the game's cadence so playback isn't juddery (user)
    const adest = veAudioDest(); if (adest) { try { adest.stream.getAudioTracks().forEach((t) => stream.addTrack(t)); } catch (e) {} }   // mix in the game audio (user) — video-only if it fails
    VE.chunks = [];
    try { VE.rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: veBitrate(canvas.width, canvas.height), audioBitsPerSecond: 192000 }); }   // record at full canvas res + a resolution-scaled bitrate so encode #1 is high quality (the export re-encodes it)
    catch (e) { console.warn('[vb] MediaRecorder ctor failed', e); return; }
    VE.rec.ondataavailable = (e) => { if (e.data && e.data.size) VE.chunks.push(e.data); };
    VE.rec.onstop = () => veLoadBlob(new Blob(VE.chunks, { type: mime.indexOf('mp4') >= 0 ? 'video/mp4' : 'video/webm' }));   // blob container follows the codec actually recorded (mp4 for H.264, else webm) — a mismatched type made the editor <video> refuse H.264 chunks
    VE.lastPaint = performance.now() - VE_CAP_MS;     // first frame paints immediately
    VE.rec.start();
    VE.recording = true;
    veRecEl.classList.remove('hidden');                // ← the "recording" banner (DOM only → never captured)
    veBtnEl.classList.add('recording');
    veRecBtn.classList.add('on'); veRecBtn.textContent = '■ stop';
  };
  const veStopRec = () => {
    if (!VE.recording) return;
    try { VE.rec.stop(); } catch (e) {}
    // Release the canvas capture. Without this the compositor keeps feeding a stream nobody reads for
    // the rest of the session. VIDEO tracks only: the audio tracks come from the shared veGameDest node
    // and stopping them would silence every later recording.
    if (VE.recStream) { try { for (const t of VE.recStream.getVideoTracks()) t.stop(); } catch (e) {} VE.recStream = null; }
    VE.recording = false;
    veRecEl.classList.add('hidden');
    veBtnEl.classList.remove('recording');
    veRecBtn.classList.remove('on'); veRecBtn.textContent = '● rec';
  };
  const veToggleRec = () => { VE.recording ? veStopRec() : veStartRec(); };

  // ── LOAD a recorded blob into the editor ──
  const veLoadBlob = (blob) => {
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
    const t = veVideo.currentTime, i = VE.clips.findIndex((c) => t > c.s + 0.02 && t < c.e - 0.02);
    if (i < 0) return;
    const c = VE.clips[i]; VE.clips.splice(i, 1, { s: c.s, e: t }, { s: t, e: c.e }); VE.sel = i + 1;
    veRenderTimeline(); veUpdateButtons();
  };
  const veDelete = () => {
    if (VE.sel < 0 || VE.sel >= VE.clips.length) return;
    VE.clips.splice(VE.sel, 1); VE.sel = Math.min(VE.sel, VE.clips.length - 1);
    veStage.classList.toggle('has-clip', VE.clips.length > 0);
    veRenderTimeline(); veUpdateTime(); veUpdateButtons();
  };
  const veUpdateButtons = () => {
    const has = VE.clips.length > 0;
    vePlayBtn.disabled = !has; $('veSplit').disabled = !has; $('veExport').disabled = !has;
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

  // ── EXPORT ── COMPOSITED, not a raw element capture. The old path recorded veVideo.captureStream() while the clip
  // played in real time, so anything that hitched the page during those seconds — above all the game still rendering
  // full-tilt behind the panel — was burned into the file as dropped/long frames. That is why it looked fine in the
  // editor but juddered in the download. Now each presented video frame is drawn (plus the annotations) onto an
  // offscreen canvas and PUSHED to the recorder via requestFrame(), so the output carries exactly the frames the
  // decoder produced, and the game loop is suspended for the duration so nothing competes for the GPU.
  const veExport = async () => {
    if (!VE.clips.length || VE.dur <= 0) return;
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
    try { const a = veVideo.captureStream ? veVideo.captureStream() : (veVideo.mozCaptureStream ? veVideo.mozCaptureStream() : null);   // carry the ORIGINAL audio track across
      if (a) for (const t of a.getAudioTracks()) stream.addTrack(t); } catch (e) {}
    const expDur = veExportDur(), reqRate = veExportRate(vw, vh, expDur).rate;
    const rec = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: reqRate, audioBitsPerSecond: 192000 });   // the FINAL encode — rate comes from the size cap in the toolbar, falling back to a quality target when the cap is "best quality"
    const chunks = []; rec.ondataavailable = (e) => { if (e.data && e.data.size) chunks.push(e.data); };
    const stopped = new Promise((res) => { rec.onstop = res; });
    let pump = null, alive = true;
    const pushFrame = () => {                              // composite the CURRENT video frame + the red annotations, then PUSH it to the recorder
      g.drawImage(veVideo, 0, 0, vw, vh);
      if (VE.strokes.length) veStrokePaint(g, 0, 0, vw, vh, lw);
      if (track.requestFrame) track.requestFrame();
    };
    VE.exporting = true;                                   // suspends the game's render work (see the frame loop) so the pump runs uncontended and playback cannot hitch
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
    for (const clip of VE.clips) {
      if (rec.state === 'recording') rec.pause();          // SPLICE the seek out of the file — the old pump filled every clip boundary with ~0.5 s of frozen duplicate frames; pausing the recorder stops its clock so the next clip butts on seamlessly
      await veSeekAwait(clip.s); clipBase = clip.s; clipPushed = 0;
      await veVideo.play().catch(() => {});
      if (rec.state === 'paused') rec.resume();
      if (!RVFC) pushFrame();                              // fallback only: land the clip's first frame now (the media clock hasn't crossed a boundary yet). On the rVFC path the seek's own presentation already drew it — pushing here could emit the PREVIOUS clip's stale canvas
      await new Promise((res) => { const chk = () => { if (veVideo.currentTime >= clip.e - 0.02 || veVideo.ended) res(); else requestAnimationFrame(chk); }; requestAnimationFrame(chk); });
      veVideo.pause();
    }
    alive = false;
    cancelAnimationFrame(pump);
    rec.stop(); await stopped; veVideo.muted = wasMuted; VE.exporting = false;
    try { for (const t of stream.getTracks()) t.stop(); } catch (e) {}   // release the export compositor's capture + the borrowed audio track
    const out = new Blob(chunks, { type: ext === 'mp4' ? 'video/mp4' : 'video/webm' });
    veCalibrate(out.size, expDur, reqRate);              // what the encoder ACTUALLY did → next export's estimate is this machine's, not a hardcoded guess
    const url = URL.createObjectURL(out), a2 = document.createElement('a');
    a2.href = url; a2.download = 'voxelbit-clip.' + ext; document.body.appendChild(a2); a2.click(); a2.remove();
    setTimeout(() => URL.revokeObjectURL(url), 4000);
    btn.disabled = false; btn.textContent = label0; veUpdateEst(); if (VE.clips.length) veSeek(VE.clips[0].s);
  };
  const veSeekAwait = (t) => new Promise((res) => { const on = () => { veVideo.removeEventListener('seeked', on); res(); }; veVideo.addEventListener('seeked', on); veVideo.currentTime = Math.max(0, Math.min(VE.dur - 1e-3, t)); });

  // ── OPEN / CLOSE + BUTTON WIRING ──
  const veOpen = () => { vePanel.classList.remove('hidden'); lockEl.classList.add('hidden');   // the editor takes over the screen: hide the esc menu behind it and RELEASE the pointer so the mouse drives the UI, not the camera (the R-stop path leaves the game pointer-locked — user: "the mouse is still connected to the player camera in the background")
    if (document.pointerLockElement) document.exitPointerLock();
    veRenderTimeline(); veUpdatePlayhead(); veUpdateTime(); veUpdateButtons(); requestAnimationFrame(veFitDraw); };
  const veClose = () => { vePanel.classList.add('hidden'); vePause(); if (!locked && !dead) lockEl.classList.remove('hidden'); };   // hand the screen back to the esc menu so the player resumes / re-locks from there
  veBtnEl.addEventListener('click', (e) => { e.stopPropagation(); veOpen(); });   // opens the editor; recording is the ● rec button inside the panel
  veRecBtn.addEventListener('click', (e) => { e.stopPropagation(); veToggleRec(); });
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
