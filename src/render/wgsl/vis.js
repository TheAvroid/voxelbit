  // @module — the tile visibility-cull WGSL, and the one place every shader factory is built into a pipeline
  // @exports CG_BAND, CG_DIM, CG_STRIDE, CG_SWEEPS, DDAW, FLAKEBLK, bgCloudGen, cgBuf, cgData, cgSamp, cgState, cgView, linSamp, pCloudGen, pBlit, pComposite, pSpatial, pTaa, pTemporal, pTraceV, pVis, worldFlush
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
  // ── THE DENSE ARRAY HAS NO GPU READER LEFT ── DDAW is built ONCE, on the pool, and TRACE and COMPOSITE
  // share it. There is no dense variant to fall back to and no POOL_ON to pick between them: the 1.5 GB
  // world buffer is not created at all any more (see render/buffers.js). W stays on the CPU as the source
  // of truth and the pool is its derived cache; nothing on the GPU sees the flat array.
  const PRE = PRE_SRC(), FLAKEBLK = FLAKEBLK_SRC(), DDAW = DDAW_SRC(1), VIS = VIS_SRC();
  const TRACE = TRACE_SRC({ DDAW, FLAKEBLK, pickWGSL, POOL: 1 });
  const COMPOSITE = COMPOSITE_SRC({ DDAW, pickWGSL });
  const TEMPORAL = TEMPORAL_SRC(),
        SPATIAL = SPATIAL_SRC(), TAA = TAA_SRC(), BLIT = BLIT_SRC();

  const mkCompute = (code, fol) => device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: (PRE + code).replaceAll('§FOL§', fol ? 'true' : 'false') }), entryPoint: 'main' } });
  const pTraceV = [mkCompute(TRACE, 0), mkCompute(TRACE, 1)];          // FOLIAGE SPECIALIZATION: variant 0 = normal play (see-through check compiled OUT of the hot DDA loop), variant 1 = eye near foliage
  const pVis = mkCompute(VIS), pTemporal = mkCompute(TEMPORAL), pSpatial = mkCompute(SPATIAL), pComposite = mkCompute(COMPOSITE), pTaa = mkCompute(TAA);
  // ── ONE FLUSH, NO DISPATCH ── this used to encode a (wordIndex, value) list into a compute pass that
  // wrote the dense GPU array. The pool replaced that: brickFlush drains the dirty-brick queue as whole
  // pages, so the only thing left to do is call it at the right moment in the frame — ahead of the trace
  // that reads what it wrote. `all` drains the queue completely rather than stopping at the per-frame
  // budget, which is what a readback needs before it can trust what it reads.
  function worldFlush(all) { brickFlush(all); return 0; }
  const blitModule = device.createShaderModule({ code: PRE + BLIT });
  const pBlit = device.createRenderPipeline({ layout: 'auto', vertex: { module: blitModule, entryPoint: 'vs' }, fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
  const linSamp = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });
  // ══ CLOUD DENSITY CACHE (jeantimex/procedural-clouds, MIT) ══ created ONCE, deliberately outside makeTargets:
  // it is a world-space volume and has nothing to do with the screen, so a resize or a render-scale change must
  // not throw it away and pay the refill again.
  // rgba8unorm because it has to be BOTH storage-capable (the compute pass writes it) and FILTERABLE (the march
  // samples it linearly). r32float is storage-capable and unfilterable, which WebGPU rejects at bind-group creation
  // with nothing but a console line and a black frame; r8unorm is filterable and not storage-capable. This is the
  // only core format that is both, and at 4 bytes it costs exactly what r32float would have.
  const CG_DIM = [160, 24, 160];                       // 2.46 MB. Anisotropic on purpose: the deck is wide and thin, so y carries a sixth of the resolution and every texel still lands finer than a march step
  const cgTex3 = device.createTexture({ size: CG_DIM, dimension: '3d', format: 'rgba8unorm',
    usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING });
  const cgView = cgTex3.createView();
  const cgSamp = device.createSampler({ magFilter: 'linear', minFilter: 'linear',   // REPEAT in x/z: the volume is a tile of an endless deck, and clamp-to-edge would put a one-texel seam on every tile boundary. y clamps, because the deck has a real top and bottom
    addressModeU: 'repeat', addressModeV: 'clamp-to-edge', addressModeW: 'repeat' });
  const cgBuf = device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const pCloudGen = device.createComputePipeline({ layout: 'auto', compute: { module: device.createShaderModule({ code: CLOUDGEN_SRC() }), entryPoint: 'main' } });
  const bgCloudGen = device.createBindGroup({ layout: pCloudGen.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: cgBuf } }, { binding: 1, resource: cgView }] });
  const cgData = new Float32Array(8);
  const CG_BAND = 1;                                   // y slices per fill…
  const CG_STRIDE = 4;                                 // …and a fill only every 4th frame, so a full sweep of the volume takes 96 frames — about 1.6 s.
  const CG_SWEEPS = 2;                                 // …and then it STOPS. With the evolution clock pinned the fill is idempotent, so once the volume has been
  // written through twice there is nothing left for it to compute: the deck's motion is a lookup offset and the
  // shape does not change. Steady-state cost of the whole cache is therefore ZERO, not "amortised". Re-armed by
  // There is nothing left to re-arm it for: the deck used to be switchable on [Y] and a toggle had to rebuild
  // the volume, but it is the only deck now and the fill runs unconditionally from boot (user 2026-08-28).
  // THAT IS NOT A COMPROMISE, IT IS THE POINT. The deck's visible MOTION is wind, and the march applies wind as
  // a lookup offset, so it costs the cache exactly nothing. The only thing a refill carries is the slow
  // evolution of cloud SHAPE, which in the sky takes minutes. At 4 slices every frame this pass was ~17M hash
  // evaluations a frame and cost more than the march it was supposed to be saving; at one slice every fourth
  // frame it is ~1M, and nothing about the image can tell the difference.
  const cgState = { slice: 0, fills: 0 };                        // …and which band is next. An OBJECT, not an exported `let`: a module exports a const snapshot taken at init, so a later write from tick-passes would be invisible here (lint-vb.py catches exactly this)

