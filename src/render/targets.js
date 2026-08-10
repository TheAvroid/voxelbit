  // ── screen textures + bind groups (rebuilt on resize / scale change) ───────
  let renderScale = 0.8;                               // default resolution scale (user 2026-08-02: 80%, was 90%); the settings slider AND the [ ] keys both step 40..100% in 10s (persisted vb_scale). The 0.375 floor below is what an older build could persist and what __vb.res() still accepts — neither control offers it.
  try { const v = parseFloat(localStorage.getItem('vb_scale')); if (v >= 0.375 && v <= 1.0) renderScale = v; } catch (e) {}
  let RW = 0, RH = 0, CW = 0, CH = 0;
  let bgTrace, bgTemporal, bgSpatial, bgComposite, bgTaa, bgBlit, bgVis, visBuf = null;
  let eyeFolV = 0;                                       // which TRACE variant this frame: 1 = the foliage-see-through pipeline (eye within 1 voxel of leaves), 0 = the fast normal-play pipeline
  let resetHist = 1;
  let liveTargets = null;                              // textures the CURRENT bind groups reference — destroyed only after the GPU is done with them
  function makeTargets(force) {
    const nCW = Math.max(2, Math.floor(canvas.clientWidth * devicePixelRatio));
    const nCH = Math.max(2, Math.floor(canvas.clientHeight * devicePixelRatio));
    const nRW = Math.max(2, Math.round(nCW * renderScale)), nRH = Math.max(2, Math.round(nCH * renderScale));
    if (!force && liveTargets && nCW === CW && nCH === CH && nRW === RW && nRH === RH) return;   // a resize event that did not change the pixel size must not rebuild ten textures (and reset temporal history)
    if (CPROF) cpEvt |= 16;
    CW = nCW; CH = nCH; RW = nRW; RH = nRH;
    canvas.width = CW; canvas.height = CH;
    const tex = (fmt) => device.createTexture({ size: [RW, RH], format: fmt, usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING });
    const gAlbedo = tex('rgba8unorm'), gIrr = tex('rgba16float');
    const histA = tex('rgba16float'), histB = tex('rgba16float'), irrF = tex('rgba16float');
    const colorCur = tex('rgba8unorm'), colHistA = tex('rgba8unorm'), colHistB = tex('rgba8unorm');
    const dofTex = tex('rgba8unorm');                    // ── DEPTH OF FIELD ── TAA's resolved colour + the composite's circle of confusion. NOT ping-ponged: it is written and read inside the same frame and carries no history.
    const slotA = tex('r32uint'), slotB = tex('r32uint');   // ── DYNAMIC LIFE ── per-pixel creature id + hit-axis bits, ping-ponged (temporal needs LAST frame's ids for identity checks). cur for frame parity par = par ? slotB : slotA — mirrors the hist ping-pong.
    const v = (t) => t.createView();
    // ── LIFETIME ── the superseded set is released only after every submitted frame that still references it
    // has completed; destroying at swap time would pull textures out from under in-flight GPU work. Before this,
    // nothing was ever destroyed and each resize/scale change leaked a full screen-texture set until GC noticed.
    const oldT = liveTargets, oldVis = visBuf;
    liveTargets = [gAlbedo, gIrr, histA, histB, irrF, colorCur, colHistA, colHistB, slotA, slotB, dofTex];
    if (oldT || oldVis) device.queue.onSubmittedWorkDone().then(() => {
      if (oldT) for (const t of oldT) { try { t.destroy(); } catch (e) {} }
      if (oldVis) { try { oldVis.destroy(); } catch (e) {} }
    }).catch(() => {});
    visBuf = device.createBuffer({ size: Math.ceil(RW / 8) * Math.ceil(RH / 8) * (DROP_SLOTS >> 3) * (VIS_W >> 2), usage: GPUBufferUsage.STORAGE });   // DROP_SLOTS/32 u32 = DROP_SLOTS/8 BYTES of tile-cull bitmask per 8×8 tile — written by the VIS prepass, read by TRACE + COMPOSITE
    bgVis = device.createBindGroup({ layout: pVis.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: { buffer: visBuf } }] });
    bgTrace = pTraceV.map(pv => [0, 1].map(par => device.createBindGroup({ layout: pv.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: { buffer: worldBuf } },
      { binding: 2, resource: { buffer: brickBuf } }, { binding: 3, resource: { buffer: palBuf } },
      { binding: 4, resource: v(gAlbedo) }, { binding: 5, resource: v(gIrr) }, { binding: 6, resource: { buffer: brick2Buf } },
      { binding: 7, resource: { buffer: wbrickBuf } }, { binding: 9, resource: { buffer: visBuf } },
      { binding: 8, resource: v(par ? slotB : slotA) }, { binding: 13, resource: { buffer: itemMapBuf } },
      { binding: 23, resource: { buffer: bodyBuf } }] })));
    bgTemporal = [0, 1].map(par => device.createBindGroup({ layout: pTemporal.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: v(gIrr) },
      { binding: 2, resource: v(par ? histA : histB) }, { binding: 3, resource: linSamp },
      { binding: 4, resource: v(par ? histB : histA) },
      { binding: 5, resource: v(par ? slotB : slotA) }, { binding: 6, resource: v(par ? slotA : slotB) }] }));
    bgSpatial = [0, 1].map(par => device.createBindGroup({ layout: pSpatial.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: v(par ? histB : histA) },
      { binding: 2, resource: v(gAlbedo) }, { binding: 3, resource: v(irrF) }] }));
    bgComposite = [0, 1].map(par => device.createBindGroup({ layout: pComposite.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: v(gAlbedo) },
      { binding: 2, resource: v(irrF) }, { binding: 3, resource: v(colorCur) },
      { binding: 13, resource: { buffer: itemMapBuf } },
      { binding: 14, resource: moonTex.createView() }, { binding: 15, resource: linSamp },
      { binding: 16, resource: { buffer: worldBuf } }, { binding: 17, resource: { buffer: brickBuf } },
      { binding: 18, resource: { buffer: brick2Buf } }, { binding: 19, resource: { buffer: palBuf } },
      { binding: 20, resource: { buffer: wbrickBuf } },
      { binding: 21, resource: v(par ? slotB : slotA) }, { binding: 22, resource: { buffer: visBuf } },
      { binding: 23, resource: { buffer: bodyBuf } }] }));
    bgTaa = [0, 1].map(par => device.createBindGroup({ layout: pTaa.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: v(colorCur) },
      { binding: 2, resource: v(gIrr) }, { binding: 3, resource: v(par ? colHistA : colHistB) },
      { binding: 4, resource: linSamp }, { binding: 5, resource: v(par ? colHistB : colHistA) },
      { binding: 6, resource: v(par ? slotB : slotA) }, { binding: 7, resource: v(dofTex) }] }));
    bgBlit = [0, 1].map(par => device.createBindGroup({ layout: pBlit.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: v(par ? colHistB : colHistA) },
      { binding: 2, resource: linSamp }, { binding: 3, resource: v(dofTex) }] }));
    resetHist = 1;
  }
  makeTargets(true);
  let resizeT = 0;                                     // DEBOUNCE: a drag-resize fires dozens of events; each one rebuilt every target and reset temporal history
  addEventListener('resize', () => { clearTimeout(resizeT); resizeT = setTimeout(() => makeTargets(), 120); });

