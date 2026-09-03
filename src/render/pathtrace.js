  // ── THE PATH TRACER'S DRIVER ── pipelines, the accumulator, the [Y] toggle ──────────────────────────────
  // The shader is in render/wgsl/pathtrace.js; the attribution to gnikoloff/webgpu-raytracer (MIT) is there
  // too. This half owns everything that is not WGSL: the two pipelines, the screen-sized accumulator, when to
  // throw that accumulator away, and the one key that switches which renderer draws the frame.
  //
  // It sits DIRECTLY BELOW render/targets.js because that is where its dependencies land — RW/RH (the trace
  // resolution it must match), `resetHist`, and the pipelines' bind-group layouts all need the shader factories
  // from render/wgsl/vis.js and the buffers from render/buffers.js. It is above everything in sim/ and ui/,
  // which is fine: the only names it borrows from down there (ED, CMD, profQS) are read inside callbacks that
  // cannot run until the whole program has been evaluated.
  //
  // ── WHAT THIS DOES NOT DRAW, STATED UP FRONT ── creatures, dropped items and the held viewmodel are
  // trace-INJECTED through the drop-slot machinery (pickWGSL + itemMapBuf), and that costs a storage buffer
  // this pass does not have room for; falling snow and rain are a lattice march inside TRACE; clouds are a
  // volume march inside COMPOSITE. None of them appear in the path-traced image. Terrain, water, foliage and
  // rigid bodies do.
  // folT DEFAULTS TO 0 — OPAQUE LEAVES — BECAUSE THAT IS WHAT MEASURED BETTER. A two-sided leaf is the
  // standard answer to a dark canopy and it was built, tuned and then defaulted off: at folT 0.35 in a
  // dense oak interior the mean screen level went 13.5 -> 13.2 of 255, i.e. very slightly DOWN, because a
  // 4-bounce budget with Russian roulette cannot push light through six voxels of canopy however much each
  // leaf transmits, while the energy it takes out of the reflected lobe darkens the sunlit tops immediately.
  // The knob stays (__vbPT.set({folT})) because the question is worth being able to answer again.
  const PT = { on: false, spp: 1, bounces: 4, cap: 1024, secR: 512, folT: 0, reproj: 1, hist: 24, dbg: 0, neeMin: 0, fClamp: 0, fog: 0.25, reflK: 0.5, n: 0, seq: 0, w: 0, h: 0,
               acc: null, tex: null, hA: null, hB: null, bg: null, bgB: null, bgBlit: null, sig: null, par: 0 };
  const ptUni = device.createBuffer({ size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });   // 8 floats became 9 when `reproj` joined the struct; 48 keeps it 16-byte aligned with room for the next one
  const ptData = new Float32Array(16);   // [spp, bounces, reset, seq, cap, secR, folT, reproj, ...spare]
  // §FOL§ is DDAW's foliage-see-through specialization constant (see render/wgsl/dda.js). The path tracer
  // takes the 'false' variant unconditionally: the see-through hack exists to stop a clipped-into canopy
  // filling the screen in a renderer whose primary ray is the whole image, and a path tracer that made leaves
  // near the eye vanish would then light the scene through a hole that is not there.
  const ptMod = device.createShaderModule({ code: (PRE_SRC() + PT_SRC({ DDAW, pickWGSL })).replaceAll('§FOL§', 'false') });
  const pPath = device.createComputePipeline({ layout: 'auto', compute: { module: ptMod, entryPoint: 'main' } });
  const ptBlitMod = device.createShaderModule({ code: PRE_SRC() + PTBLIT_SRC() });
  const pPtBlit = device.createRenderPipeline({ layout: 'auto', vertex: { module: ptBlitMod, entryPoint: 'vs' },
    fragment: { module: ptBlitMod, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
  // ── THE ACCUMULATOR IS SCREEN-SIZED, so it is rebuilt whenever the trace target is ── a resize, a
  // render-scale step, or the recorder's capNative flip. Lazy rather than hooked into makeTargets: this
  // renderer is off by default and a player who never presses [Y] must not pay 12 MB for it, nor pay a
  // rebuild on every drag-resize.
  function ptSync() {
    if (PT.acc && PT.w === RW && PT.h === RH) return;
    const oldA = PT.acc, oldT = PT.tex;
    PT.w = RW; PT.h = RH; PT.n = 0; PT.sig = null;
    PT.acc = device.createBuffer({ size: RW * RH * 16, usage: GPUBufferUsage.STORAGE });   // vec4 per pixel: rgb sum + sample count
    PT.tex = device.createTexture({ size: [RW, RH], format: 'rgba8unorm',
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING });
    // ── THE TEMPORAL HISTORY, PING-PONGED ── one frame reads A and writes B, the next the other way round.
    // A single buffer cannot do this: pixels reproject to ARBITRARY neighbours, so reading and writing one
    // surface in the same dispatch is a read-write race across workgroups with no ordering to lean on.
    const mkH = () => device.createTexture({ size: [RW, RH], format: 'rgba16float',
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING });
    const oldHA = PT.hA, oldHB = PT.hB;
    PT.hA = mkH(); PT.hB = mkH();
    PT.bg = device.createBindGroup({ layout: pPath.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: { buffer: poolBuf } },
      { binding: 2, resource: { buffer: bdescBuf } }, { binding: 3, resource: { buffer: palBuf } },
      { binding: 4, resource: PT.tex.createView() }, { binding: 6, resource: { buffer: gb2Buf } },
      { binding: 7, resource: { buffer: gwbBuf } }, { binding: 10, resource: { buffer: PT.acc } },
      { binding: 11, resource: { buffer: ptUni } }, { binding: 14, resource: moonTex.createView() },
      { binding: 15, resource: linSamp }, { binding: 23, resource: { buffer: bodyBuf } },
      { binding: 16, resource: PT.hA.createView() }, { binding: 17, resource: PT.hB.createView() },
      { binding: 24, resource: cgView }, { binding: 25, resource: cgSamp },
      { binding: 13, resource: { buffer: itemMapBuf } }] });
    PT.bgB = device.createBindGroup({ layout: pPath.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: { buffer: poolBuf } },
      { binding: 2, resource: { buffer: bdescBuf } }, { binding: 3, resource: { buffer: palBuf } },
      { binding: 4, resource: PT.tex.createView() }, { binding: 6, resource: { buffer: gb2Buf } },
      { binding: 7, resource: { buffer: gwbBuf } }, { binding: 10, resource: { buffer: PT.acc } },
      { binding: 11, resource: { buffer: ptUni } }, { binding: 14, resource: moonTex.createView() },
      { binding: 15, resource: linSamp }, { binding: 23, resource: { buffer: bodyBuf } },
      { binding: 16, resource: PT.hB.createView() }, { binding: 17, resource: PT.hA.createView() },
      { binding: 24, resource: cgView }, { binding: 25, resource: cgSamp },
      { binding: 13, resource: { buffer: itemMapBuf } }] });
    PT.bgBlit = device.createBindGroup({ layout: pPtBlit.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: PT.tex.createView() },
      { binding: 2, resource: linSamp }] });
    if (oldA || oldT) device.queue.onSubmittedWorkDone().then(() => {          // released only once no submitted frame still references them
      try { if (oldA) oldA.destroy(); } catch (e) {}
      try { if (oldT) oldT.destroy(); } catch (e) {}
      try { if (oldHA) oldHA.destroy(); } catch (e) {}
      try { if (oldHB) oldHB.destroy(); } catch (e) {}
    }).catch(() => {});
  }
  // ── WHEN TO THROW THE ESTIMATE AWAY ── a progressive path tracer is only valid while the thing it is
  // averaging holds still, so the accumulator is zeroed the moment the camera or the world under it moves.
  // The signature is read out of UF, the staging copy of the uniform buffer, rather than from `cam`/`fwd`:
  // UF is BY DEFINITION what the GPU will see this frame, so the test can never drift out of step with the
  // rays it is guarding. Deliberately NOT in it: u.frame and u.time (floats 11 and 15, which change every
  // frame by construction) and u.sunDir — the day cycle turns the sun ~0.3 degrees a second, so resetting on
  // it would mean the image never converged at all. The cost of leaving it out is that shadows lag the sun by
  // however long you have been standing still, which is visible only in a timelapse.
  function ptSig() {
    let s = '';
    for (let i = 0; i <= 10; i++) s += UF[i] + ',';               // camPos + tanH, right + aspect, up
    for (let i = 12; i <= 14; i++) s += UF[i] + ',';              // fwd (15 is u.time)
    s += UF[31] + ',';                                            // u.fx — underwater, moon mode, the ceiling band
    for (let i = 44; i <= 47; i++) s += UF[i] + ',';              // winO + off: a window recentre moves every coordinate in the shader
    s += UF[UF_PHYSC + 1] + ',' + RW + 'x' + RH;                  // physC.y = reactive strength: nonzero while a rigid body is MOVING, and a moving body would otherwise smear across the accumulation
    return s;
  }
  // ── THE FRAME ── one compute dispatch that adds pt.spp paths per pixel to the accumulator and writes the
  // running mean into PT.tex, then one triangle that puts PT.tex on the canvas. That is the whole renderer:
  // no g-buffer, no temporal reprojection, no spatial filter, no TAA. The noise you see IS the estimator's
  // variance, and it goes down as 1/sqrt(samples) rather than being filtered away.
  function ptRender(enc) {
    ptSync();
    const sig = ptSig();
    if (resetHist || sig !== PT.sig) { PT.sig = sig; PT.n = 0; }
    PT.seq = (PT.seq + 1) & 0xffff;
    // ── THE ORDER HERE IS THE STRUCT'S ORDER, AND NOTHING CHECKS THAT FOR YOU ── PTU in the shader reads
    // spp, bounces, reset, seq, cap, secR, reproj, hist, folT. Inserting reproj/hist BEFORE folT in the
    // struct while still writing folT at index 6 handed the shader reproj = folT = 0 (so reprojection never
    // ran at all) and folT = hist = 24 (so every leaf became absurdly transmissive). It looked like a subtle
    // "reprojection barely helps" result for four measurements. Keep these two lists in the same order.
    ptData[0] = PT.spp; ptData[1] = PT.bounces; ptData[2] = PT.n === 0 ? 1 : 0; ptData[3] = PT.seq; ptData[4] = PT.cap; ptData[5] = PT.secR;
    // resetHist means the SHIPPING histories were invalidated (a toggle, a teleport, a resize) — there is no
    // previous frame to reproject from, so that one throws the estimate away as it always did. An ordinary
    // camera move does not: it reprojects.
    ptData[6] = (PT.reproj && !resetHist) ? PT.reproj : 0;
    ptData[7] = PT.hist;
    ptData[8] = PT.folT;
    ptData[9] = PT.dbg;
    ptData[10] = PT.neeMin;
    ptData[11] = PT.fClamp;
    ptData[12] = PT.fog;
    ptData[13] = PT.reflK;
    device.queue.writeBuffer(ptUni, 0, ptData.buffer, 0, 64);
    // Timestamps reuse the shipping query set when __vb.prof(true) has armed it, so the path tracer's own
    // cost lands in profEma[0] — printed under the name 'trace', which is exactly the pass it replaces.
    const cp = enc.beginComputePass(profQS ? { timestampWrites: { querySet: profQS, beginningOfPassWriteIndex: 0, endOfPassWriteIndex: 1 } } : {});
    PT.par ^= 1;
    cp.setPipeline(pPath); cp.setBindGroup(0, PT.par ? PT.bgB : PT.bg);
    cp.dispatchWorkgroups(Math.ceil(RW / 8), Math.ceil(RH / 8)); cp.end();
    if (profQS) {
      enc.resolveQuerySet(profQS, 0, 14, profRes, 0);
      if (!profBusy) { enc.copyBufferToBuffer(profRes, 0, profStg, 0, 112); profNew = true; }
    }
    const rp = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(),
      loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }],
      ...(profQS ? { timestampWrites: { querySet: profQS, beginningOfPassWriteIndex: 12, endOfPassWriteIndex: 13 } } : {}) });
    rp.setPipeline(pPtBlit); rp.setBindGroup(0, PT.bgBlit); rp.draw(3); rp.end();
    if (PT.cap <= 0 || PT.n < PT.cap) PT.n += PT.spp;
  }
  // ── THE TOGGLE ── [Y]. Free in play: the water panel that used to own it is retired (see the KeyY handler
  // in ui/hud.js, which returns on its first line), and the ASSET EDITOR still owns the key for its tree-size
  // panel (ui/input.js), so ED.on is the one guard that is not optional — two handlers on one key is the bug
  // that ate the L key in 2026-08-20. Switching back sets resetHist, because while the path tracer was up the
  // shipping renderer's temporal and TAA histories went stale and reprojecting onto them would ghost.
  function ptToggle(on) {
    const v = on === undefined ? !PT.on : !!on;
    if (v === PT.on) return PT.on;
    PT.on = v; PT.n = 0; PT.sig = null; resetHist = 1;
    console.log('[vb] renderer:', v ? 'PATH TRACED (progressive) — hold still to converge' : 'shipping');
    return PT.on;
  }
  addEventListener('keydown', (e) => {
    if (e.code !== 'KeyY' || e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
    try { if (CMD.open || ED.on) return; } catch (err) { return; }   // the command line owns the keyboard; the asset editor owns [Y]. try/catch because a key pressed before boot finishes would otherwise throw out of the listener
    const ae = document.activeElement; if (ae && /^(INPUT|TEXTAREA|SELECT)$/.test(ae.tagName)) return;
    ptToggle();
  });
  // A hook of its own rather than an entry in __vb: main/debug-api.js assigns window.__vb as one object
  // literal, so anything written there earlier would be overwritten.
  window.__vbPT = { on: (v) => ptToggle(v), stat: () => ({ on: PT.on, samples: PT.n, spp: PT.spp, bounces: PT.bounces, secR: PT.secR, folT: PT.folT, reproj: PT.reproj, hist: PT.hist, neeMin: PT.neeMin, fClamp: PT.fClamp, fog: PT.fog, reflK: PT.reflK, cap: PT.cap, w: PT.w, h: PT.h }),
                    set: (o) => { Object.assign(PT, o || {}); PT.n = 0; PT.sig = null; return PT.on; } };
