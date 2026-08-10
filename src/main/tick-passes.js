    // ── passes ──
    if (VE.exporting) { tickReq = true; paceWaited = false; requestAnimationFrame(tick); return; }   // ── EXPORT QUIET MODE ── while the video editor is writing a file, skip the frame entirely: the
    // tracer runs at 200+ fps behind the panel and its GPU/CPU contention is what made the exported clip judder
    // (the editor preview looked fine because dropped frames only show up in the recorded timeline). The loop keeps
    // pumping so playback, seeking and the recorder all continue normally.
    const par = frame & 1;
    const enc = device.createCommandEncoder();
    if (xStripPending >= 0) {                          // strip scatter runs before this frame's trace
      const p = enc.beginComputePass(); p.setPipeline(pScatter); p.setBindGroup(0, bgScatter);
      p.dispatchWorkgroups(Math.ceil(STRIPW / 256)); p.end();
      xStripPending = -1;
    }
    // ⚠ ORDERING: the voxel patch goes AFTER the strip scatter. The scatter carries a PRE-band snapshot of a
    // whole x-strip; applying creature/snow/edit words first would let it stomp them with stale wrapped terrain.
    patchEncode(enc);
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
    if (profQS) {                                      // resolve + read back pass timings (only while __vb.prof polling has it armed)
      enc.resolveQuerySet(profQS, 0, 14, profRes, 0);
      if (!profBusy) { enc.copyBufferToBuffer(profRes, 0, profStg, 0, 112); profNew = true; }
    }
    const rp = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }], ...(profQS ? { timestampWrites: { querySet: profQS, beginningOfPassWriteIndex: 12, endOfPassWriteIndex: 13 } } : {}) });
    rp.setPipeline(pBlit); rp.setBindGroup(0, bgBlit[par]); rp.draw(3); rp.end();
    device.queue.submit([enc.finish()]);
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
    resetHist = 0;
    frame++;

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
      if (showTime) hudParts.push(`${clock}${P.fly ? '  FLY' : ''}${P.crouch ? '  crouch' : ''}${cycleSpeed !== 1 ? '  x' + cycleSpeed.toFixed(1) : ''}`);
      hudEl.textContent = hudParts.join('\n\n');
    }
    // FRAME PACING: off the PREVIOUS frame's GPU completion — up to 2 frames in flight, so the CPU's frame
    // prep (worldgen slices, uniforms, JS) overlaps GPU execution instead of serializing with it. This used to
    // be the v-sync-off half of a branch; v-sync was removed (user 2026-08-08) and it defaulted off anyway, so
    // this is now the only pacing mode and a hidden tab is the sole reason to fall back to rAF.
    if (document.hidden) { paceWaited = false; tickReq = true; requestAnimationFrame(tick); }   // hidden tab always idles via rAF
    else { const wait = gpuPrevDone; gpuPrevDone = device.queue.onSubmittedWorkDone(); paceTs = performance.now();   // ONE pacing mode — no threshold flip-flop, so gen throughput is CONSISTENT
      tickReq = true;
      wait.then(paceOk, paceFail); }   // HOISTED handlers (see above): the inline arrow pair allocated two closures EVERY frame — the top entry in the steady-state heap profile
  }
  try { const nb = nvBoot(); console.log('[vb] navfield', (NV_BYTES / 1048576).toFixed(2) + ' MB', NVN + ' cells', 'boot ' + nb.toFixed(1) + ' ms', NAVARB ? 'arbiter ON' : 'arbiter OFF'); }
  catch (e) { vbNoteErr('navBoot', e); }               // the field is an accelerator, never a dependency — if it fails to build, every consumer falls back to the point probes it used before
  tick(performance.now());
