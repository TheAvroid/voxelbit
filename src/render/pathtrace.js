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
  const PT = { on: false, spp: 1, bounces: 4, cap: 1024, secR: 512, folT: 0, reproj: 1, hist: 24, dbg: 0, neeMin: 0, fClamp: 0, fog: 0.25, reflK: 0.5, neeB: 1, shCap: 0, coh: 1, oidn: 1, n: 0, seq: 0, w: 0, h: 0,
               acc: null, tex: null, alb: null, nrm: null, dnTex: null, dnSig: null, dnBusy: false, hA: null, hB: null, bg: null, bgB: null, bgBlit: null, sig: null, par: 0 };
  const ptUni = device.createBuffer({ size: 80, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });   // 8 floats became 9 when `reproj` joined the struct; 48 keeps it 16-byte aligned with room for the next one
  const ptData = new Float32Array(20);   // [spp, bounces, reset, seq, cap, secR, folT, reproj, ...spare]
  // §FOL§ is DDAW's foliage-see-through specialization constant (see render/wgsl/dda.js). The path tracer
  // takes the 'false' variant unconditionally: the see-through hack exists to stop a clipped-into canopy
  // filling the screen in a renderer whose primary ray is the whole image, and a path tracer that made leaves
  // near the eye vanish would then light the scene through a hole that is not there.
  const ptMod = device.createShaderModule({ code: (PRE_SRC() + PT_SRC({ DDAW })).replaceAll('§FOL§', 'false') });
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
    // rgba16float, not rgba8unorm: this is LINEAR HDR radiance now, because OIDN's hdr path applies its own
    // PU transfer and autoexposure and must be handed real light. The blit tonemaps at the end of the chain.
    const mkF = () => device.createTexture({ size: [RW, RH], format: 'rgba16float',
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC });
    PT.tex = mkF();
    // ── THE TEMPORAL HISTORY, PING-PONGED ── one frame reads A and writes B, the next the other way round.
    // A single buffer cannot do this: pixels reproject to ARBITRARY neighbours, so reading and writing one
    // surface in the same dispatch is a read-write race across workgroups with no ordering to lean on.
    const mkH = () => device.createTexture({ size: [RW, RH], format: 'rgba16float',
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING });
    const oldHA = PT.hA, oldHB = PT.hB, oldAl = PT.alb, oldNr = PT.nrm, oldDn = PT.dnTex;
    PT.hA = mkH(); PT.hB = mkH();
    PT.alb = mkF(); PT.nrm = mkF();                                  // the noise-free aux planes OIDN's guided model wants
    PT.dnTex = mkF(); PT.dnSig = null;                               // …and where its result lands, owned by us so the blit can sample it
    PT.bg = device.createBindGroup({ layout: pPath.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: { buffer: poolBuf } },
      { binding: 2, resource: { buffer: bdescBuf } }, { binding: 3, resource: { buffer: palBuf } },
      { binding: 4, resource: PT.tex.createView() }, { binding: 6, resource: { buffer: gb2Buf } },
      { binding: 7, resource: { buffer: gwbBuf } }, { binding: 10, resource: { buffer: PT.acc } },
      { binding: 11, resource: { buffer: ptUni } }, { binding: 14, resource: moonTex.createView() },
      { binding: 15, resource: linSamp }, { binding: 23, resource: { buffer: bodyBuf } },
      { binding: 16, resource: PT.hA.createView() }, { binding: 17, resource: PT.hB.createView() },
      { binding: 24, resource: cgView }, { binding: 25, resource: cgSamp },
      { binding: 18, resource: PT.alb.createView() }, { binding: 19, resource: PT.nrm.createView() }] });
    PT.bgB = device.createBindGroup({ layout: pPath.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: { buffer: poolBuf } },
      { binding: 2, resource: { buffer: bdescBuf } }, { binding: 3, resource: { buffer: palBuf } },
      { binding: 4, resource: PT.tex.createView() }, { binding: 6, resource: { buffer: gb2Buf } },
      { binding: 7, resource: { buffer: gwbBuf } }, { binding: 10, resource: { buffer: PT.acc } },
      { binding: 11, resource: { buffer: ptUni } }, { binding: 14, resource: moonTex.createView() },
      { binding: 15, resource: linSamp }, { binding: 23, resource: { buffer: bodyBuf } },
      { binding: 16, resource: PT.hB.createView() }, { binding: 17, resource: PT.hA.createView() },
      { binding: 24, resource: cgView }, { binding: 25, resource: cgSamp },
      { binding: 18, resource: PT.alb.createView() }, { binding: 19, resource: PT.nrm.createView() }] });
    const mkBlit = (t) => device.createBindGroup({ layout: pPtBlit.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniBuf } }, { binding: 1, resource: t.createView() },
      { binding: 2, resource: linSamp }] });
    PT.bgBlit = mkBlit(PT.tex); PT.bgBlitDn = mkBlit(PT.dnTex);      // raw estimate, or the denoised one when it is current
    if (oldA || oldT) device.queue.onSubmittedWorkDone().then(() => {          // released only once no submitted frame still references them
      try { if (oldA) oldA.destroy(); } catch (e) {}
      try { if (oldT) oldT.destroy(); } catch (e) {}
      try { if (oldHA) oldHA.destroy(); } catch (e) {}
      try { if (oldHB) oldHB.destroy(); } catch (e) {}
      try { if (oldAl) oldAl.destroy(); } catch (e) {}
      try { if (oldNr) oldNr.destroy(); } catch (e) {}
      try { if (oldDn) oldDn.destroy(); } catch (e) {}
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
  // ══ OIDN (pmndrs/denoiser, MIT; Intel Open Image Denoise weights, Apache-2.0) ══ a learned U-Net, which is
  // the denoiser this renderer actually wanted: it reconstructs detail where the a-trous filter averaged it
  // away, and the a-trous is exactly what the owner rejected for blurring.
  // IT RUNS ON OUR DEVICE. The library normally has ORT create the GPUDevice and hands it back for you to
  // adopt — which would not work here, because core/gpu.js asks for a raised maxStorageBufferBindingSize to
  // bind the ~1 GB brick pool. Setting ort.env.webgpu.device BEFORE Denoiser.create makes its engine adopt
  // ours instead (it checks exactly that, and skips its own device creation when it is already set), so the
  // textures below are shared with no copy and no second device.
  // AND IT IS NOT A PER-FRAME PASS. The published figure is ~104 ms at 1080p and our trace target is about
  // that, against a 15-22 ms frame — so denoising every frame would cost five times the frame it cleans.
  // It runs ONCE per settled estimate instead: move, and you see the raw progressive image; hold still and
  // the clean one replaces it. That is the shape the cost allows and it is where the noise mattered least.
  let dnP = null;
  const dnLoad = () => dnP || (dnP = (async () => {
    // core/gpu.js already built it when the game booted with ?oidn / vb_oidn, and BECAUSE it did, this
    // device is the denoiser's own — the only arrangement in which it can read our textures at all.
    if (window.__vbDN) { PT.dn = window.__vbDN; return window.__vbDN; }
    // ── AND WITHOUT IT WE DO NOT BUILD ONE ── creating a Denoiser here would succeed, report itself ready,
    // and run: onnxruntime makes its own GPUDevice, and every voxelbit texture it reads across that
    // boundary comes back ZERO rather than throwing. The blit would then show a black canvas at a
    // convergence-capped 400+ fps with no error anywhere. A denoiser we cannot feed is worse than none.
    PT.oidn = 0;
    console.warn('[vb] path tracer: OIDN needs the game booted on its device — reload with ?oidn (or set vb_oidn=1) to enable it');
    return null;
  })().catch((e) => { PT.oidn = 0; console.warn('[vb] OIDN unavailable, path tracer stays raw:', e); return null; }));
  const DN_MIN = 12;                                                 // samples before it is worth denoising — below this the estimate is still moving fast enough that the result is stale by the time it lands
  function dnMaybe() {
    if (!PT.oidn || PT.dnBusy || PT.n < DN_MIN || PT.dnSig === PT.sig) { return; }
    PT.dnBusy = true;
    const sig = PT.sig;
    dnLoad().then((d) => {
      if (!d) { PT.dnBusy = false; return null; }
      return d.denoiseTextures({ color: PT.tex, albedo: PT.alb, normal: PT.nrm, hdr: true, output: PT.dnTex });
    }).then((t) => { PT.dnRet = t ? (t === PT.dnTex ? 'ours' : 'engine') : 'none'; PT.dnStats = (PT.dn && PT.dn.stats) || null;
      // a run that aborted returns undefined and leaves the output texture untouched — showing it would
      // blit an empty frame while every status flag still claimed the denoiser was current
      if (t && PT.sig === sig) { PT.dnSig = sig; } PT.dnBusy = false; })
      .catch((e) => { PT.dnBusy = false; console.warn('[vb] OIDN run failed:', e); });
  }
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
    ptData[14] = PT.neeB;
    ptData[15] = PT.shCap;
    // ── THE BOUNCE-COHERENCE BLOCK ── 1 = a 2x2 quad shares one bounce direction, which is 41% of this
    // pass (see the long note at `coh` in the PTU struct). 0 restores the per-pixel estimator.
    ptData[16] = PT.coh;
    device.queue.writeBuffer(ptUni, 0, ptData.buffer, 0, 80);
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
    // the denoised image only while it belongs to THIS estimate — the moment the camera moves, sig changes
    // and the raw progressive frame comes back rather than a clean picture of where you used to be looking
    rp.setPipeline(pPtBlit); rp.setBindGroup(0, (PT.dnSig === PT.sig && PT.dnSig !== null) ? PT.bgBlitDn : PT.bgBlit); rp.draw(3); rp.end();
    if (PT.cap <= 0 || PT.n < PT.cap) PT.n += PT.spp;
    dnMaybe();                                                       // …after the submit, never inside the encoder: this is async and runs its own passes
  }
  // ── THE TOGGLE ── [Y], and the 'graphics' row in the settings panel. Free in play: the water panel that used to own it is retired (see the KeyY handler
  // in ui/hud.js, which returns on its first line), and the ASSET EDITOR still owns the key for its tree-size
  // panel (ui/input.js), so ED.on is the one guard that is not optional — two handlers on one key is the bug
  // that ate the L key in 2026-08-20. Switching back sets resetHist, because while the path tracer was up the
  // shipping renderer's temporal and TAA histories went stale and reprojecting onto them would ghost.
  // ── AND THE SETTINGS PANEL THROWS THE SAME SWITCH ── the 'graphics low/high' row in ui/settings.js calls
  // ptToggle rather than writing PT.on, so the accumulator reset and the resetHist flush a renderer swap needs
  // happen in exactly one place however the swap was asked for. ptBtnSync is the callback that repaints (and
  // persists) that row, and it is declared HERE, above every caller, because ui/settings.js sits BELOW this
  // fragment in the manifest — a `let` down there would be in the dead zone for the [Y] handler above. It stays
  // the no-op stub if the row is never wired, which is what makes the panel optional to this file.
  let ptBtnSync = () => {};
  function ptToggle(on) {
    const v = on === undefined ? !PT.on : !!on;
    if (v === PT.on) return PT.on;
    PT.on = v; PT.n = 0; PT.sig = null; resetHist = 1;
    console.log('[vb] renderer:', v ? 'PATH TRACED (progressive) — hold still to converge' : 'shipping');
    ptBtnSync();                                     // [Y], the settings row and __vbPT.on() all land here, so the row can never read 'low' while the path tracer is on screen
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
  // ── A 64x64 READBACK OF ANY STAGE ── a black frame has three candidate causes (the estimate, the aux
  // planes, the denoised result) and the canvas shows only the last of them, so it cannot tell them apart.
  // rgba16float is 8 B/px, so a 64-wide row is exactly 512 B and already meets the 256 B row alignment.
  async function ptProbe(which) {
    const t = ({ tex: PT.tex, alb: PT.alb, nrm: PT.nrm, dn: PT.dnTex })[which];
    if (!t) { return { err: 'no texture ' + which }; }
    const W = 64, H = 64, bpr = W * 8;
    const buf = device.createBuffer({ size: bpr * H, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: t, origin: { x: Math.max(0, (PT.w >> 1) - 32), y: Math.max(0, (PT.h >> 1) - 32) } },
      { buffer: buf, bytesPerRow: bpr }, { width: W, height: H });
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const u = new Uint16Array(buf.getMappedRange().slice(0));
    buf.unmap(); buf.destroy();
    const h2f = (v) => { const sg = (v & 0x8000) ? -1 : 1, e = (v >> 10) & 31, f = v & 1023;
      if (e === 0) { return sg * f * 5.9604644775390625e-8; }
      if (e === 31) { return f ? NaN : sg * Infinity; }
      return sg * Math.pow(2, e - 15) * (1 + f / 1024); };
    let mx = 0, sum = 0, nz = 0;
    for (let i = 0; i < u.length; i++) { if (i % 4 === 3) { continue; } const v = h2f(u[i]); if (v > mx) { mx = v; } sum += v; if (u[i]) { nz++; } }
    return { which, max: +mx.toFixed(4), mean: +(sum / (u.length * 0.75)).toFixed(5), nonzero: nz };
  }
  window.__vbPT = { probe: (w) => ptProbe(w), dev: () => device, texs: () => ({ tex: PT.tex, alb: PT.alb, nrm: PT.nrm, dn: PT.dnTex }), inst: () => PT.dn || null, dbg: () => ({ dnRet: PT.dnRet || null, dnStats: PT.dnStats || null }), on: (v) => ptToggle(v), stat: () => ({ on: PT.on, samples: PT.n, spp: PT.spp, bounces: PT.bounces, secR: PT.secR, folT: PT.folT, reproj: PT.reproj, hist: PT.hist, neeMin: PT.neeMin, fClamp: PT.fClamp, fog: PT.fog, reflK: PT.reflK, neeB: PT.neeB, shCap: PT.shCap, coh: PT.coh, oidn: PT.oidn, oidnReady: !!PT.dn, oidnSig: PT.dnSig === PT.sig, cap: PT.cap, w: PT.w, h: PT.h }),
                    set: (o) => { Object.assign(PT, o || {}); PT.n = 0; PT.sig = null; PT.dnSig = null; return PT.on; },
                    denoise: () => { PT.dnSig = null; dnMaybe(); return { busy: PT.dnBusy, samples: PT.n }; } };
