  const VIS_SRC = () => /* wgsl */`
    ${pickWGSL}
    @group(0) @binding(1) var<storage, read_write> visb : array<u32>;   // 4×u32 bitmask per 8×8 screen tile: bit di = drop slot di's bounding sphere may touch this tile (128 slots = four words)
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {          // one invocation = one TILE (not one pixel) — the whole pass is ~1/64th of a screen pass
      let tX = (u32(u.res.x) + 7u) / 8u; let tY = (u32(u.res.y) + 7u) / 8u;
      if (gid.x >= tX || gid.y >= tY) { return; }
      var m0 = 0u; var m1 = 0u; var m2 = 0u; var m3 = 0u;
      ${UNI_RAY ? 'var g0 = 0u; var g1 = 0u; var g2 = 0u; var g3 = 0u;' : ''}
      if (ITEMN > 0) {                                                 // EXACT math of the old per-workgroup cull, cone half-angle hoisted out of the slot loop (it only depends on the tile)
        let tc = vec2<f32>(gid.xy) * 8.0 + vec2<f32>(4.0);
        let cdir = normalize(vec3<f32>((tc.x / u.res.x * 2.0 - 1.0) * u.tanH * u.aspect, -(tc.y / u.res.y * 2.0 - 1.0) * u.tanH, 1.0));
        var cosT = 1.0;                                                // tile half-angle = worst corner (±0.6 jitter margin baked into the ±4.6)
        for (var k = 0u; k < 4u; k++) {
          let co = tc + vec2<f32>(select(-4.6, 4.6, (k & 1u) != 0u), select(-4.6, 4.6, (k & 2u) != 0u));
          let cd = normalize(vec3<f32>((co.x / u.res.x * 2.0 - 1.0) * u.tanH * u.aspect, -(co.y / u.res.y * 2.0 - 1.0) * u.tanH, 1.0));
          cosT = min(cosT, dot(cd, cdir));
        }
        let tAng = acos(clamp(cosT, -1.0, 1.0));
        let dN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);
        for (var diC = 0; diC < dN; diC++) {
          let dXv = dropV(diC * 4 + 1);
          let dit = i32(dXv.w + 0.5);
          if (dit < 1) { continue; }
          let dA = dropV(diC * 4);
          let it3 = clamp(dit - 1, 0, ITEMN - 1);
          if (ITEMD[it3].x < 1) { continue; }
          let ex2 = vec3<f32>(f32(ITEMD[it3].x), f32(ITEMD[it3].y), f32(ITEMD[it3].z)) * 0.5;
          let radD = dA.w * (length(ex2) + 1.0);
          let dlen = length(dA.xyz);
          let cAng = acos(clamp(dot(dA.xyz / max(dlen, 1e-6), cdir), -1.0, 1.0));   // ONE acos per slot. The grown SECONDARY cone test below wants the SAME angle and used to recompute it from scratch; whenever that test runs, dlen > radS + 1 > radD + 1 > 1, so max() is the identity there and this is the identical value.
          var vis = 0u;
          if (dlen <= radD + 1.0) { vis = 1u; }                        // camera inside the sphere — always visible to this tile
          else if (cAng <= tAng + asin(clamp(radD / dlen, 0.0, 1.0)) + 0.01) { vis = 1u; }
          if (vis != 0u) { let bit = 1u << (u32(diC) & 31u); if (diC < 32) { m0 |= bit; } else if (diC < 64) { m1 |= bit; } else if (diC < 96) { m2 |= bit; } else { m3 |= bit; } }
          ${UNI_RAY ? 'var vg = 0u; let radS = radD + SEC_R; if (dlen <= radS + 1.0) { vg = 1u; } else if (cAng <= tAng + asin(clamp(radS / dlen, 0.0, 1.0)) + 0.01) { vg = 1u; } if (vg != 0u) { let bg = 1u << (u32(diC) & 31u); if (diC < 32) { g0 |= bg; } else if (diC < 64) { g1 |= bg; } else if (diC < 96) { g2 |= bg; } else { g3 |= bg; } }' : ''}
        }
      }
      let ti = (gid.y * tX + gid.x) * ${VIS_W}u;
      visb[ti] = m0; visb[ti + 1u] = m1; visb[ti + 2u] = m2; visb[ti + 3u] = m3;
      ${UNI_RAY ? 'visb[ti + 4u] = g0; visb[ti + 5u] = g1; visb[ti + 6u] = g2; visb[ti + 7u] = g3;' : ''}
    }`;

  // ── SHADER BUILD ── Every WGSL literal in render/wgsl/ is a FACTORY, and this is the
  // ONE place any of them is called. A function body evaluates when it is CALLED, so by
  // the time any `${…}` is read, every fragment above has finished running — which makes
  // the "a const read before its declaration inside an earlier shader literal" black
  // screen structurally impossible rather than something to remember. It cost a session.
  //
  // Shader-to-shader dependencies are passed EXPLICITLY. The destructured names shadow
  // the outer ones, so each shader body is byte-for-byte what it always was, while the
  // signature now states what that shader is composed from. A missing argument shows up
  // as the literal text "undefined" in the WGSL, which fails compilation loudly, instead
  // of a silent TDZ that only ever renders black.
  const PRE = PRE_SRC(), FLAKEBLK = FLAKEBLK_SRC(), DDAW = DDAW_SRC(), VIS = VIS_SRC();
  const TRACE = TRACE_SRC({ DDAW, FLAKEBLK, pickWGSL });
  const COMPOSITE = COMPOSITE_SRC({ DDAW, pickWGSL });
  const SCATTER = SCATTER_SRC(), PATCHW = PATCHW_SRC(), TEMPORAL = TEMPORAL_SRC(),
        SPATIAL = SPATIAL_SRC(), TAA = TAA_SRC(), BLIT = BLIT_SRC();

  const mkCompute = (code, fol) => device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: (PRE + code).replaceAll('§FOL§', fol ? 'true' : 'false') }), entryPoint: 'main' } });
  const pTraceV = [mkCompute(TRACE, 0), mkCompute(TRACE, 1)];          // FOLIAGE SPECIALIZATION: variant 0 = normal play (see-through check compiled OUT of the hot DDA loop), variant 1 = eye near foliage
  const pVis = mkCompute(VIS), pTemporal = mkCompute(TEMPORAL), pSpatial = mkCompute(SPATIAL), pComposite = mkCompute(COMPOSITE), pTaa = mkCompute(TAA);
  const pScatter = device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: SCATTER }), entryPoint: 'main' } });
  const bgScatter = device.createBindGroup({ layout: pScatter.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: scatBuf } }, { binding: 1, resource: { buffer: stagBuf } }, { binding: 2, resource: { buffer: worldBuf } }] });
  const pPatch = device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: PATCHW }), entryPoint: 'main' } });
  const bgPatch = device.createBindGroup({ layout: pPatch.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: patchCnt } }, { binding: 1, resource: { buffer: patchBuf } }, { binding: 2, resource: { buffer: worldBuf } }] });
  // Encode the staged patch into `enc`. Values are read from W32 HERE, so a word edited several times
  // this frame uploads its final state exactly once. Returns the pair count (0 = nothing to do).
  function patchEncode(enc) {
    brickFlush();                                      // the frame's accumulated brick/L2 bits, coalesced once — queued here so they land ahead of the trace that reads them
    if (!patchN) return 0;
    for (let i = 0; i < patchN; i++) { const w = patchIdx[i]; patchPairs[i * 2] = w; patchPairs[i * 2 + 1] = W32[w]; }
    device.queue.writeBuffer(patchBuf, 0, patchPairs.buffer, 0, patchN * 8);
    patchCntTmp[0] = patchN;
    device.queue.writeBuffer(patchCnt, 0, patchCntTmp);
    const p = enc.beginComputePass(); p.setPipeline(pPatch); p.setBindGroup(0, bgPatch);
    p.dispatchWorkgroups(Math.ceil(patchN / 64)); p.end();
    const n = patchN; patchN = 0; return n;
  }
  // Immediate flush — used when the stage overflows mid-frame, and by __vb.gpudiff() so a readback
  // never races staged-but-undispatched edits.
  function patchFlush() {
    if (!patchN) return 0;
    const enc = device.createCommandEncoder();
    const n = patchEncode(enc);
    device.queue.submit([enc.finish()]);
    return n;
  }
  const blitModule = device.createShaderModule({ code: PRE + BLIT });
  const pBlit = device.createRenderPipeline({ layout: 'auto', vertex: { module: blitModule, entryPoint: 'vs' }, fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
  const linSamp = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });

