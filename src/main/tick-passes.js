    // ── passes ──
    // ── EXPORT QUIET MODE ── while the video editor is writing a file the TRACE is skipped: it runs at 200+ fps
    // behind the panel and its GPU/CPU contention is what made the exported clip judder (the editor preview looked
    // fine because dropped frames only show up in the recorded timeline). The loop keeps pumping so playback,
    // seeking and the recorder all continue normally. ONLY the render passes are skipped — this used to be a
    // mid-body return right here, which is precisely the exit tick-body.js warns about. It left xStripPending
    // set, so stream.js parked its band generator on its wait-for-scatter yield loop and stepShifts burned
    // its whole 3-18 ms budget busy-waiting every frame with terrain streaming frozen; and it skipped patchEncode,
    // which is what zeroes patchN, so staged words climbed to PATCHMAX (well under a second with snow falling) and
    // patchFlush then submitted a patch while a scatter was still pending — inverting the very ordering below.
    const veQuiet = VE.exporting;
    const par = frame & 1;
    if (CPROF) enLast = performance.now();
    const enc = device.createCommandEncoder();
    // ── NOTHING WRITES VOXELS ON THE GPU ANY MORE ── the strip scatter and the voxel-patch dispatch both
    // existed to push CPU edits into the dense GPU array without a big writeBuffer. There is no dense GPU
    // array: the pool is a derived cache of W, and every path that mutates W already poolTouch()es the bricks
    // it changed, so the same edits land as whole 512-byte pages inside brickFlush below. That also retires
    // the ordering hazard those two dispatches had with each other — a pre-band strip snapshot can no longer
    // stomp this frame's creature and snow words, because there is no snapshot.
    worldFlush();
    recTick();                                       // ── FLIGHT RECORDER ── one ring-buffer row a frame (render/buffers.js); F9 dumps the last ~12 s
    if (CPROF) enMark(0);
    // ══ CLOUD DENSITY CACHE FILL (jeantimex/procedural-clouds, MIT) ══ a BAND of y slices per frame, not the
    // whole volume. The full fill is ~614k texels of fractal Voronoi and would be a visible hitch if it landed
    // in one frame; spread four slices at a time it is 102k texels and finishes a full sweep every six frames.
    // The source solves the same problem with a ping-pong pair and a cross-fade. This does not need one: the
    // only thing that changes between sweeps is the slow evolution clock, so a half-refreshed volume differs
    // from itself by less than a texel of shape and there is no seam to hide — which also saves the second
    // volume's memory AND the second texture fetch on every march step and every light step.
    // AHEAD of the render below, so the frame that samples the cache sees this frame's slices.
    // ── THE FIRST FILL DOES THE WHOLE VOLUME (user 2026-08-28: "when I refresh the game and spawn in for
    // the first time, the sky is empty at first, and then fills in with clouds") ── and that was not a bug in
    // the deck, it was this loop's own schedule showing through. One slice every fourth frame over 24 slices
    // is 96 frames for a single sweep, ~1.6 s, and the march can only draw what has been written — so the
    // player watched the sky populate. Amortising is right for the STEADY state and wrong for the first
    // frame, where there is nothing on screen to protect: prime the whole volume in one dispatch, then let
    // the existing schedule carry the (idempotent) second sweep.
    // It is ~614k texels in one go, which is the hitch the amortisation exists to avoid — but it lands on the
    // first rendered frame, behind the loading overlay, where a hitch costs nothing and a visibly empty sky
    // costs everything.
    const cgBand = cgState.fills === 0 ? CG_DIM[1] : CG_BAND;        // …the whole deck first, one slice thereafter
    if (!veQuiet && (cgState.fills === 0 || (frame % CG_STRIDE) === 0) && cgState.fills < CG_DIM[1] * CG_SWEEPS) {                // UNGATED (user 2026-08-28: keep the new deck, drop the Y switch) — the march in COMPOSITE is unconditional now, so a gate here would starve it of the volume it samples and the sky would simply have no clouds in it. The fill still stops on its own after CG_SWEEPS, so steady-state cost is zero either way
      cgData[0] = 0; cgData[1] = 0; cgData[2] = 0;
      cgData[3] = 0.0;                        // the evolution clock, in days x 240 — slow on purpose: the deck's MOTION is wind, which the march applies as a lookup offset and which therefore costs the cache nothing
      cgData[4] = cgState.slice; cgData[5] = cgBand;
      device.queue.writeBuffer(cgBuf, 0, cgData.buffer, 0, 32);
      const cp = enc.beginComputePass();
      cp.setPipeline(pCloudGen); cp.setBindGroup(0, bgCloudGen);
      cp.dispatchWorkgroups(Math.ceil(CG_DIM[0] / 8), Math.ceil(cgBand / 4), Math.ceil(CG_DIM[2] / 8));
      cp.end();
      cgState.slice = (cgState.slice + cgBand) % CG_DIM[1]; cgState.fills += cgBand;   // fills counts SLICES, not dispatches, so the priming dispatch accounts for all 24 of them and the CG_SWEEPS bound still means what it says
    }
    // ── WHICH RENDERER DRAWS THIS FRAME ── [Y], and the settings panel's 'graphics low/high' row, flip
    // between the shipping pipeline (below, = low) and the progressive path tracer in render/pathtrace.js
    // (= high). PT.on is false by default and the else-if leaves
    // the shipping branch below byte-for-byte what it was, so an untoggled frame is the frame it always was.
    if (!veQuiet && PT.on) ptRender(enc);
    else if (!veQuiet) {                               // ── the render, and only the render ── everything above and below this brace is frame bookkeeping the export must not skip
      const gw = Math.ceil(RW / 8), gh = Math.ceil(RH / 8);
      let qi = 0;
      const run = (pipe, bg) => {
        const p = enc.beginComputePass(profQS ? { timestampWrites: { querySet: profQS, beginningOfPassWriteIndex: qi++, endOfPassWriteIndex: qi++ } } : {});
        p.setPipeline(pipe); p.setBindGroup(0, bg); p.dispatchWorkgroups(gw, gh); p.end(); };
      { const p = enc.beginComputePass(profQS ? { timestampWrites: { querySet: profQS, beginningOfPassWriteIndex: 10, endOfPassWriteIndex: 11 } } : {});   // ── VIS prepass ── one thread per 8×8 TILE (~1/64th of a screen pass): the creature/drop tile-cull bitmask, computed once and shared by TRACE + COMPOSITE
        p.setPipeline(pVis); p.setBindGroup(0, bgVis);
        p.dispatchWorkgroups(Math.ceil(gw / 8), Math.ceil(gh / 8)); p.end(); }
      run(pTraceV[eyeFolV], bgTrace[eyeFolV][par]);
      run(pTemporal, bgTemporal[par]);
      run(pSpatial, bgSpatial[par]);
      run(pComposite, bgComposite[par]);
      run(pTaa, bgTaa[par]);
      if (profQS) {                                    // resolve + read back pass timings (only while __vb.prof polling has it armed)
        enc.resolveQuerySet(profQS, 0, 14, profRes, 0);
        if (!profBusy) { enc.copyBufferToBuffer(profRes, 0, profStg, 0, 112); profNew = true; }
      }
      if (CPROF) enMark(1);
      const swapView = ctx.getCurrentTexture().createView();   // …can block on the presentation queue, so it is timed on its own
      if (CPROF) enMark(2);
      const rp = enc.beginRenderPass({ colorAttachments: [{ view: swapView, loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }], ...(profQS ? { timestampWrites: { querySet: profQS, beginningOfPassWriteIndex: 12, endOfPassWriteIndex: 13 } } : {}) });
      rp.setPipeline(pBlit); rp.setBindGroup(0, bgBlit[par]); rp.draw(3); rp.end();
    }
    device.queue.submit([enc.finish()]);
    if (CPROF) enMark(3);                              // a quiet frame still submits: the scatter + patch above are the encoder's whole payload, and they must land in THIS frame's order
    if (profNew && !profBusy) {
      profNew = false; profBusy = true;
      profStg.mapAsync(GPUMapMode.READ).then(() => {
        const q = new BigInt64Array(profStg.getMappedRange());
        for (let i = 0; i < 7; i++) { const ms = Number(q[i * 2 + 1] - q[i * 2]) / 1e6; if (ms >= 0 && ms < 100) { profEma[i] = profEma[i] * 0.92 + ms * 0.08; if (ms < profMin[i]) profMin[i] = ms; } }
        profSamp++;
        profStg.unmap(); profBusy = false;
      }).catch(() => { profBusy = false; });
    }

    if (CPROF) { cpMark(7); cpUpN += (upN - cpUpN) * 0.08; cpUpB += (upB - cpUpB) * 0.08;
      const tb = performance.now() - tbT0; FTB[ftI] = tb;
      if (dt * 1000 > cpSpikeTh || tb > cpSpikeTh) {   // log the worst frames with full attribution
        if (cpSpikes.length >= 40) cpSpikes.shift();
        cpSpikes.push({ dt: +(dt * 1000).toFixed(2), tick: +tb.toFixed(2),
          ph: Array.from(cpCur, (v) => +v.toFixed(2)), ev: cpEvt, upN, upKB: +(upB / 1024).toFixed(1) });
      }
    }
    ftI = (ftI + 1) % FTR; if (ftN < FTR) ftN++;
    // ── save prev cam (world coords) ──
    prevCam.pos = cam; prevCam.right = right; prevCam.up = up; prevCam.fwd = fwd;
    prevCam.tanH = tanH; prevCam.aspect = aspect; prevCam.jit = j;
    // Counted: a frame that consumed a SET flag threw away the temporal history. Standing still it should be
    // zero — anything else and nothing on screen can converge. __vb.histStat().
    if (resetHist) { HIST_DBG.resets = (HIST_DBG.resets | 0) + 1; HIST_DBG.lastAt = frame; }
    HIST_DBG.frames = (HIST_DBG.frames | 0) + 1;
    if (!veQuiet) resetHist = 0;                       // the TRACE is what consumes the flag, so a quiet frame must NOT clear it — a teleport during an export would otherwise lose its history flush and come back ghosting
    // ── ONE SLICE OF THE BIRCH PERCH SWEEP ── beside frame++ because that is the one call site this file
    // documents as NEVER SKIPPED, and the sweep needs exactly that. It cannot live in main/tick-nav.js's
    // buildCardCand (that only runs when findPineCrown asks for a perch, and nothing asks until the sweep
    // has produced its first candidates — it advanced one slice and stopped, leaving the birch forest with
    // zero perched songbirds); nor in main/tick-body.js, which loads BEFORE tick-nav.js, so the call threw
    // "Cannot access 'birchScanStep' before initialization" inside tickBody every frame, silently; nor at
    // the lifeSlotBase line in main/tick-life.js, which turns out not to run unconditionally either. Three
    // wrong homes, each found by probing the sweep's own cursor rather than by reading.
    birchScanStep();
    frame++;                                           // never skipped: tick-creatures reads ((frame + wk) & 63) to recycle off-water slots, and a frozen counter makes that fire every frame for one slot in 64 and never for the rest

    // ── dual-wield overlays ── the shift+left-click hint shows until the discovery is earned
    if (achHideT && now > achHideT) { achEl.style.opacity = 0; achHideT = 0; }
    clashHintEl.classList.toggle('hidden', !(locked && !vbAch.sharpEdge && dualRocks() && now - clashT0 > 620));

    // ── hud ──
    hudT += dt;
    if (hudT > 0.25) {
      hudT = 0;
      if (!locked) {
        const mins = Math.floor(tday * 1440), th = Math.floor(mins / 60); let th12 = th % 12; if (th12 === 0) th12 = 12;   // 12-hour + am/pm in the settings clock too (user)
        todLabel.textContent = th12 + ':' + String(mins % 60).padStart(2, '0') + ' ' + (th < 12 ? 'am' : 'pm');
        if (!todDrag) { todSlider.value = mins; sliderFill(todSlider); }   // keep the green fill line tracking the clock as the day advances
      }
      const h24 = Math.floor(tday * 24), mn = Math.floor((tday * 24 % 1) * 60);   // 12-hour clock with AM/PM (user)
      let h12 = h24 % 12; if (h12 === 0) h12 = 12;                                // 0/12/24 → 12
      const clock = h12 + ':' + String(mn).padStart(2, '0') + ' ' + (h24 < 12 ? 'am' : 'pm');
      const hudParts = [];                               // each section is toggleable (fps/coords/time) and joined by a BLANK LINE for spacing (user)
      if (showRes) hudParts.push(`${CW}×${CH}  (${RW}×${RH})`);   // RESOLUTION readout (user: toggled in settings): display/canvas res, then the path-traced render target (CW×CH × renderScale) in parens
      if (showFps) hudParts.push(`${fpsEma.toFixed(0)} fps`);
      if (showCoords) hudParts.push(`x ${(P.x / 10).toFixed(1)}\ny ${(P.y / 10).toFixed(1)}\nz ${(P.z / 10).toFixed(1)}`);   // coords in metres, one axis per line
      if (showTime) hudParts.push(`${clock}${P.fly ? '  FLY' : ''}${P.crouch ? '  crouch' : ''}${cycleSpeed !== 1 ? (cycleSpeed < 0 ? '  <<x' + (-cycleSpeed).toFixed(1) : '  x' + cycleSpeed.toFixed(1)) : ''}`);
      hudEl.textContent = hudParts.join('\n\n');
    }
    // ── HAND THE FRAME TO THE RECORDER, AFTER IT IS DRAWN ── this used to sit at the TOP of tickBody, capturing
    // whatever was presented LAST tick. That was right for MediaRecorder, whose requestFrame() asks the COMPOSITOR
    // for the last composited image. It is wrong for the WebCodecs recorder, which reads the canvas itself with
    // `new VideoFrame(canvas)`: before this frame's passes have run there is nothing valid in the WebGPU canvas to
    // read, and the take opened with EMPTY frames — measured, indices 0-4 and 9 came out pure green, 130 bytes
    // each, and green is what a zeroed YUV plane looks like (user 2026-08-21: "theres green screens now").
    // Here the frame has been submitted, so what is read is the picture that was just rendered.
    if (VE.recording && VE.pushFrame) {
      VE.paintN = (VE.paintN | 0) + 1;
      if (VE.paintN % (VE.capEvery || 1) === 0) VE.pushFrame(now);
    }
    // FRAME PACING: off the PREVIOUS frame's GPU completion — up to 2 frames in flight, so the CPU's frame
    // prep (worldgen slices, uniforms, JS) overlaps GPU execution instead of serializing with it. This used to
    // be the v-sync-off half of a branch; v-sync was removed (user 2026-08-08) and it defaulted off anyway, so
    // this is now the only pacing mode and a hidden tab is the sole reason to fall back to rAF.
    if (document.hidden || veQuiet) { paceWaited = false; tickReq = true; requestAnimationFrame(tick); }   // hidden tab — or an export in progress — always idles via rAF: a quiet frame submits only a patch, so the GPU-completion pace would resolve instantly and spin the sim at thousands of fps, which is the contention the quiet mode exists to remove
    else { const wait = gpuPrevDone; gpuPrevDone = device.queue.onSubmittedWorkDone(); paceTs = performance.now();   // ONE pacing mode — no threshold flip-flop, so gen throughput is CONSISTENT
      tickReq = true;
      wait.then(paceOk, paceFail); }   // HOISTED handlers (see above): the inline arrow pair allocated two closures EVERY frame — the top entry in the steady-state heap profile
  }
  try { const nb = nvBoot(); console.log('[vb] navfield', (NV_BYTES / 1048576).toFixed(2) + ' MB', NVN + ' cells', 'boot ' + nb.toFixed(1) + ' ms', NAVARB ? 'arbiter ON' : 'arbiter OFF'); }
  catch (e) { vbNoteErr('navBoot', e); }               // the field is an accelerator, never a dependency — if it fails to build, every consumer falls back to the point probes it used before
  tick(performance.now());
