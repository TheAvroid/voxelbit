  // ── PT_SRC ── THE SECOND RENDERER, on [Y] ─────────────────────────────────────────────────────────────
  // A brute-force Monte Carlo PATH TRACER over the same world the shipping renderer draws. It exists to be
  // compared against, not to replace anything: the shipping pipeline is a one-bounce estimator with a hand
  // built ambient/bounce model and an SVGF denoiser on top, and the honest way to know what that model costs
  // in accuracy is to stand a converging reference next to it and flip between the two.
  //
  // Technique ported from gnikoloff/webgpu-raytracer (MIT) — https://github.com/gnikoloff/webgpu-raytracer
  //   * multi-bounce path tracing written as a LOOP with an explicit throughput, because WGSL has no recursion
  //   * cosine-weighted hemisphere sampling at every diffuse bounce (the cosine cancels against the Lambert
  //     BRDF's own cosine, so the whole bounce is `throughput *= albedo`)
  //   * progressive ACCUMULATION into a storage buffer across frames, zeroed whenever the camera moves
  //   * a final tonemap/blit pass that divides the accumulator by its own sample count
  // What is NOT ported, and could not be: that renderer intersects TRIANGLE MESHES through a BVH. There are no
  // triangles here and no BVH to build — the brick pool IS the acceleration structure, and `trace()` /
  // `bodyTrace()` in render/wgsl/dda.js are the intersector. This shader takes DDAW as an argument for exactly
  // that reason: it is the SAME traversal the shipping tracer walks, so a difference between the two images is
  // a difference in the LIGHTING model and never in what the ray hit.
  //
  // ── BINDING BUDGET ── TRACE sits at exactly 8 storage buffers, the WebGPU default cap (see the bdesc note in
  // render/wgsl/trace.js), and a 9th fails pipeline creation silently. This pass spends SEVEN: pool, bdesc,
  // pal, bricks2, wbricks, bodyVox — the six DDAW needs — plus the accumulator. There is exactly one slot
  // spare, which is what the drop-slot creature intersector would cost if this ever grows to draw them.
  // Every binding here is READ by the shader: layout:'auto' prunes anything unused and then rejects the bind
  // group that still passes it, which shows up as a black canvas at absurd fps and one console line.
  const PT_SRC = ({ DDAW }) => /* wgsl */`
    @group(0) @binding(1) var<storage, read> pool : array<u32>;      // the paged brick pool — same buffer, same format, same reader as TRACE
    @group(0) @binding(2) var<storage, read> bdesc : array<u32>;
    @group(0) @binding(3) var<storage, read> pal : array<vec4<f32>>;
    @group(0) @binding(6) var<storage, read> bricks2 : array<u32>;
    @group(0) @binding(7) var<storage, read> wbricks : array<u32>;
    @group(0) @binding(23) var<storage, read> bodyVox : array<u32>;  // rigid bodies, so a felled tree paths exactly like a standing one
    // ── THE ACCUMULATOR ── rgb = the running sum of radiance, a = how many samples went into it. A BUFFER and
    // not an rgba32float texture because a read-write storage texture needs a device feature this game does not
    // request, and the pass has to read back what it wrote last frame.
    @group(0) @binding(10) var<storage, read_write> ptAcc : array<vec4<f32>>;
    // ── TEMPORAL HISTORY ── the previous frame's MEAN radiance and the primary hit distance that produced it.
    // Two TEXTURES ping-ponged rather than storage buffers: TRACE's 8-storage-buffer cap is the binding budget
    // this pass lives inside (see the note above), and storage TEXTURES are a separate, emptier limit.
    // rgba16float, not rgba32float: 32-bit float is not filterable in core WebGPU, and layout:'auto' infers a
    // filterable sample type for a texture_2d<f32>, so a 32F history fails bind-group validation. At 4000
    // voxels f16 still resolves ~2 voxels, far inside the 5% distance tolerance the reuse test wants.
    // ══ VOLUMETRIC CLOUDS, PORTED (user 2026-09-03) ══ the SAME cached density volume the shipping renderer
    // marches (render/wgsl/cloudgen.js fills it, composite.js marches it), bound here so the two renderers
    // cannot drift into two different skies. CG_CONSTS is that shared tuning block, so every number the
    // deck's shape depends on still has exactly one definition in the repo.
    // CLOUD_LO/CLOUD_HI and cgIGN/cgSample/cgPhase/cgLight are COPIES: they live inside composite.js and a
    // WGSL module cannot see another module's functions. They have to be kept in step with that file by hand.
    @group(0) @binding(24) var cgTex : texture_3d<f32>;
    @group(0) @binding(25) var cgSamp : sampler;
    @group(0) @binding(16) var ptHistIn  : texture_2d<f32>;
    @group(0) @binding(17) var ptHistOut : texture_storage_2d<rgba16float, write>;
    struct PTU {
      spp : f32,                                                     // paths per pixel per frame
      bounces : f32,                                                 // path length, including the primary ray
      reset : f32,                                                   // 1 = the camera moved, throw the accumulator away
      seq : f32,                                                     // a per-frame counter that decorrelates the sample sequence
      cap : f32,                                                     // stop accumulating past this many samples (0 = never)
      secR : f32,                                                    // how far a BOUNCE ray looks for geometry, in voxels (0 = the full render distance)
      reproj : f32,                                                  // 1 = reuse the previous frame through prevUVd instead of throwing the estimate away when the camera moves
      hist : f32,                                                    // how many samples the reused history is worth: the response of the exponential average is 1/(hist+1)
      folT : f32,                                                    // what fraction of a LEAF's albedo leaves through the far side (0 = opaque leaves, the strict reading of the geometry)
      dbg : f32,                                                     // bit 0 = skip next-event estimation (a measurement probe); bit 1 = white-noise jitter; bit 3 = no clouds
      neeMin : f32,                                                  // skip the sun shadow ray once the path's throughput falls below this — see the note at the NEE block
      fClamp : f32,                                                  // clamp a single sample's radiance to this — the firefly guard (0 = off)
      fog : f32,                                                     // distance-fog strength as a FRACTION of the shipping renderer's (composite.js worldFog); 0 = none
      reflK : f32,                                                   // scales the water's Fresnel reflectance — 1 = physical, 0.5 = half the mirror
      // ══ THE DEEP OPTIMISATION PASS, AND WHAT IT FOUND (2026-09-03) ══ the owner's bar was that a quality
      // trade must buy MORE THAN TEN TIMES its visual cost. Measured against that bar, with the sampling
      // noise floor subtracted in quadrature (this matters — see below), the whole frontier is:
      //   neeB 1  (sun only on the primary hit)  -32.3% frame   loss 1.48% open / 4.98% canopy   21.8x / 6.5x
      //   neeB 2  (sun on the first two bounces)  -2.7% frame   loss ~0%                          passes, but
      //                                                                                          the gain is
      //                                                                                          inside the
      //                                                                                          measurement
      //   shCap 400 (shadow ray 1200 -> 400)      -1.8% frame   loss 2.25%                        0.8x
      //   shCap 200                               -9.3% frame   loss 9.39%                        1.0x
      // NEE ON THE PRIMARY HIT ONLY IS THE ONE REAL LEVER — a third of the frame — and it is REJECTED, because
      // it passes in the open and fails under canopy at 6.5x, and a forest interior is where this renderer
      // spends its time. Sun-bounce is most of the indirect light exactly where the sky is occluded.
      // THE MEASUREMENT NOTE THAT MATTERS MORE THAN THE RESULT: two INDEPENDENTLY converged frames of the
      // SAME build differ by 3.0% in the open and 7.7% under canopy. That noise floor is larger than every
      // difference in the table, so an uncorrected A/B here measures nothing at all — the first pass of this
      // table said neeB 1 was 9.5x and rejected it for the wrong reason. Always converge the baseline twice.
      // ── AND neeB 1 IS THE SHIPPED DEFAULT, BY THE OWNER'S OVERRIDE OF THEIR OWN BAR (2026-09-03, asked and
      // answered: "take the NEE trade anyway") ── it buys 32% of the frame in the open and 20% under canopy
      // for a 1.5% / 5.0% visual loss. The canopy figure is 4.0x, below the ten-times bar, and it is taken
      // knowingly. __vbPT.set({neeB: 0}) restores the unbiased estimator for a reference image.
      neeB : f32,                                                    // fire the sun shadow ray only on bounces below this (0 = every bounce)
      shCap : f32,                                                   // how far a sun shadow ray looks for an occluder, in voxels (0 = the shipped 1200)
      // ══ THE BOUNCE RAYS ARE THE FRAME, AND THEIR COST IS DIVERGENCE ══ (measured 2026-09-03, 2195x885,
      // spp 1, __vb.profMin().trace, cap: 0 so it samples every frame). The table above spent itself on
      // the shadow rays because that is what the profiler said in the days before neeB 1 shipped. It is not
      // where the frame is any more, and the split now reads:
      //   primary ray + its NEE + sky/clouds (bounces 1)   3.4 ms
      //   the diffuse BOUNCE rays                         ~10   ms  = 72% of the pass
      //   every NEE shadow ray (dbg 1)                     1.5 ms  = 11%
      //   the cloud deck (dbg 8)                          ~0.0 ms
      // and by bounce count, 1/2/4/8 = 3.18 / 13.0 / 13.9 / 13.6 ms — the FIRST diffuse bounce is 9.8 ms of
      // it and bounces 3-8 together are 0.9, because Russian roulette has already killed those paths. So
      // bounces is not a lever and neither is the deck.
      // ── AND IT IS NOT TRAVERSAL WORK, IT IS WARP DIVERGENCE ── the probe that proves it seeds the path
      // from the lane BLOCK instead of the pixel, so 2^coh lanes draw identical bounce directions and
      // identical roulette outcomes: SAME ray count, SAME reach distribution, only coherence changes.
      //   per pixel (coh 0)  14.4 / 15.3 ms      4x4 (coh 2)   5.94 ms  -60%
      //   2x2      (coh 1)    8.76 ms  -41%      8x8 (coh 3)   4.82 ms  -67%
      // The control is bounces: 1, where the same probe moves nothing (3.38 vs 3.13): primary rays are
      // already coherent, so every bit of that gap is the secondary rays. Two thirds of this renderer is
      // lanes in a warp walking different bricks, not the walking itself — which is why every "make the
      // step cheaper" idea has washed out here. The steps are not the cost; the ARRANGEMENT of the rays is.
      // ── WHAT THE TRADE ACTUALLY COSTS ── not bias and not sharpness. The estimator stays unbiased (a
      // shared direction is still a fair cosine draw) and each pixel still gets its own path, so the mean
      // and the convergence RATE per pixel are unchanged. What changes is that the residual noise is
      // CORRELATED over the quad, so it reads as 2x2 blocks rather than as grain. Two things erode that:
      // pt.seq is in the seed, so the quad draws a fresh direction every frame, and the temporal
      // accumulator then averages those independent draws exactly as it always did.
      // Deliberately NOT jittering the block ORIGIN per frame, which would break the fixed 2x2 lattice: an
      // offset grid lets a quad straddle a workgroup boundary, where its four pixels are in different
      // dispatches and share nothing, and that gives the coherence back. Worth measuring, not worth
      // assuming.
      // coh: 0 restores the per-pixel estimator for a reference image, exactly as neeB: 0 does.
      coh : f32,                                                     // share one bounce direction across a 2^coh lane block (0 = per pixel, 1 = 2x2)
      bnoise : f32,                                                  // 1 = spatiotemporal blue noise on the primary hit's bounce and sun cone; 0 restores the white draws for an in-session A/B
    }
    @group(0) @binding(11) var<uniform> pt : PTU;
    // ── THE G-BUFFER OIDN WANTS ── colour is LINEAR HDR (the denoiser's hdr:true path applies OIDN's own PU
    // transfer and autoexposure, so it must be given real radiance, not a tonemapped image), and the two aux
    // planes are the primary hit's albedo and normal. The docs are explicit that aux must be NOISE-FREE — a
    // rasterised G-buffer, not the noisy beauty pass — which is exactly what a primary hit is.
    // FOUR storage textures is the WebGPU default cap for one stage, and this pass now uses all four.
    @group(0) @binding(4) var ptOut : texture_storage_2d<rgba16float, write>;
    @group(0) @binding(18) var ptAlb : texture_storage_2d<rgba16float, write>;
    @group(0) @binding(19) var ptNrm : texture_storage_2d<rgba16float, write>;
    ${DDAW}
    ${CG_CONSTS}
    const CLOUD_LO : f32 = 760.0;
    const CLOUD_HI : f32 = 1080.0;
    fn cgIGN(p : vec2<f32>, f : u32) -> f32 {
      let q = p + 5.588238 * f32(f % 64u);
      return fract(52.9829189 * fract(dot(q, vec2<f32>(0.06711056, 0.00583715))));
    }
    fn cgSample(p : vec3<f32>) -> f32 {
      let yn = (p.y - CLOUD_LO) / (CLOUD_HI - CLOUD_LO);
      let uvw = vec3<f32>(fract((p.x + u.cloudT.x * 9.0) / CG_TILE), clamp(yn, 0.0, 1.0), fract((p.z + u.cloudT.x * 3.5) / CG_TILE));
      return textureSampleLevel(cgTex, cgSamp, uvw, 0.0).r * CG_STORE;
    }
    fn cgPhase(cosT : f32, g : f32) -> f32 {
      let g2 = g * g;
      return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
    }
    fn cgLight(p : vec3<f32>) -> f32 {
      var sh = 0.0;
      for (var i = 1; i <= CG_LIGHT_STEPS; i++) {
        sh += cgSample(p + u.sunDir * (f32(i) * CG_LIGHT_STEP)) * CG_LIGHT_STEP_U;
      }
      return exp(-sh * CG_SHADOW);
    }
    // The deck over the sky for one primary ray. The structure is composite.js's march; what is dropped is
    // the rain coupling, the slice probe and the bloom hand-off, none of which this renderer has. The IGN
    // dither stays and is left INSIDE the accumulation on purpose: averaged over frames it resolves, where a
    // post-average composite would print it as a fixed pattern that never converges.
    fn ptClouds(sky : vec3<f32>, rd : vec3<f32>, px : vec2<f32>) -> vec3<f32> {
      if (rd.y <= 0.02 || (u32(pt.dbg) & 8u) != 0u) { return sky; }   // dbg bit 3 skips the deck, for an in-session cost A/B
      let camY = u.camPos.y;
      let tA = (CLOUD_LO - camY) / rd.y;
      let tB = (CLOUD_HI - camY) / rd.y;
      let ta = max(min(tA, tB), 0.0);
      let tb = min(max(tA, tB), 12000.0);
      if (tb <= ta) { return sky; }
      let roW = vec3<f32>(u.camPos.x + u.winO.x, camY, u.camPos.z + u.winO.y);
      let stepW = (tb - ta) / f32(CG_RAY_STEPS);
      let stepU = stepW / CG_U2W;
      let phase = mix(1.0, cgPhase(dot(rd, u.sunDir), CG_G), CG_PHASE_MIX);
      let sunC = sunTint() * mix(dayScale(), CG_NKEY, nightK());
      let ambC = mix(HORIZON, ZENITH, 0.4) * 0.55 * dayScale() + CG_NAMB * nightK();
      var tg = ta + stepW * cgIGN(px, u32(u.frame));
      var Tg = 1.0;
      var accG = vec3<f32>(0.0);
      for (var ci = 0; ci < CG_RAY_STEPS; ci++) {
        let d = cgSample(roW + rd * tg);
        if (d > CG_MIN_D) {
          let stTr = exp(-d * stepU);
          let wt = Tg * (1.0 - stTr);
          if (wt > CG_MIN_W) {
            accG = accG + wt * (sunC * (cgLight(roW + rd * tg) * phase * (1.0 - exp(-d))) * CG_SUN + ambC);
          } else {
            accG = accG + wt * ambC;
          }
          Tg = Tg * stTr;
          if (Tg < CG_T_CUT) { break; }
        }
        tg = tg + stepW;
      }
      let aG = 1.0 - Tg;
      if (aG <= 1e-3) { return sky; }
      // Distance moves the deck's COLOUR toward the horizon haze and leaves its ALPHA alone, and the sky is
      // occluded FASTER than the deck is dense — both exactly as composite.js argues them. The second is what
      // stops the sun disc showing through an opaque cloud.
      let cloudG = accG / max(aG, 1e-3);
      let hazeG = skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z)));
      let cG = mix(cloudG, hazeG, 1.0 - exp(-ta * 0.00035));
      // ── A MIX, NOT AN ADD ── written first as sky * (1 - aSky) + cG * aG, which is where the dark rim
      // around every cloud came from: CG_SKY_OCC is 1.85, so aSky is nearly DOUBLE aG, and those two terms
      // sum to (1 - 1.85 * aG) + aG = 1 - 0.85 * aG. That is less than one everywhere the deck is partly
      // transparent, i.e. exactly along every cloud edge, and the missing energy reads as a dark outline.
      // composite.js composites the two with a single mix by aSky and loses nothing; this is that line.
      let aSky = min(1.0, aG * CG_SKY_OCC);
      return mix(sky, cG, aSky);
    }
    // ── THE HELD TOOL WAS TRACED HERE AND REMOVED (user 2026-09-03: "revert the axe change. just remove
    // the tools from the hand") ── it worked: the viewmodel grid march from composite.js, intersected on the
    // primary ray and shaded by a short diffuse path with a real shadow ray into the world, matching the
    // terrain's own light (measured in a forest interior at 43.1 against the terrain's 38.7). Removed on
    // request, so this renderer draws no viewmodel at all — which is also what it did before.
    const PT_MAXB : i32 = 8;                                         // the loop bound the compiler unrolls against; pt.bounces is the runtime one and is always <= this
    // ── AN ORTHONORMAL BASIS AROUND n ── its own copy rather than TRACE's onbT, because these are separate
    // WGSL modules and neither can see the other. Same construction, same numbers.
    fn ptOnb(n : vec3<f32>) -> vec3<f32> {
      let a = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(n.x) > 0.9);
      return normalize(cross(a, n));
    }
    // COSINE-WEIGHTED hemisphere sampling (Malley's method: a uniform disc lifted onto the hemisphere). The
    // pdf is cos/pi and the Lambert BRDF is albedo/pi, so the estimator collapses to throughput *= albedo —
    // no cosine term appears anywhere in the loop below, and that is why it is not missing.
    // ── LOW-DISCREPANCY SAMPLING ── the estimator's cost is fixed; what is not fixed is how evenly its
    // samples land. White noise clumps and leaves gaps, so a given sample count buys less than it could.
    // R2 is the 2D generalisation of the golden ratio: successive terms are as far from each other as a
    // 2D sequence can be. Indexed by the FRAME (pt.seq) so the stratification happens along the axis the
    // accumulator actually averages over, and Cranley-Patterson rotated by a per-pixel offset so neighbouring
    // pixels do not all walk the same sequence and turn the noise into a visible lattice.
    fn r2seq(i : f32, cp : vec2<f32>) -> vec2<f32> {
      return fract(vec2<f32>(i * 0.7548776662, i * 0.5698402910) + cp);
    }
    fn ptCosHemi(n : vec3<f32>, r1 : f32, r2 : f32) -> vec3<f32> {
      let a = 6.2831853 * r1; let r = sqrt(r2);
      let t = ptOnb(n); let b = cross(n, t);
      return t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(max(1.0 - r2, 0.0));
    }
    // ── SPATIOTEMPORAL BLUE NOISE FOR THE FIRST BOUNCE ──────────────────────────────────────────────────
    // Interleaved gradient noise on the golden-ratio walk, the same construction and the same constants as
    // the cloud dither's cgIGN in render/wgsl/composite.js — deliberately, because that one is already the
    // answer to this exact complaint in another pass. IGN is near-optimal over a 2x2..8x8 neighbourhood, so
    // neighbours draw WELL-SPREAD directions instead of independent ones, and the walk decorrelates it over
    // frames so it cannot print a fixed pattern into a held-still image.
    // ── AND IT REDUCES NO VARIANCE AT ALL, WHICH IS THE POINT ── a blue-noise sequence draws from the same
    // distribution as the white one it replaces, so a single pixel's estimate is exactly as noisy either
    // way. What changes is WHERE the error lands: spread evenly at high spatial frequency instead of
    // clumping. The eye and the temporal average both handle that far better. It is also the one lever here
    // that CANNOT blur, because it changes which directions get sampled and never how pixels are combined —
    // which is what disqualified the a-trous filter and the bilinear history before it.
    fn ptIGN(p : vec2<f32>, f : u32) -> f32 {
      let q = p + 5.588238 * f32(f % 64u);
      return fract(52.9829189 * fract(dot(q, vec2<f32>(0.06711056, 0.00583715))));
    }
    // ── THE BLUE NOISE IS THE OFFSET, NOT THE SAMPLE ── R2 supplies the uniformity along the frame axis the
    // accumulator averages over, and the per-quad offset only ROTATES that sequence, which preserves its
    // uniformity exactly (Cranley-Patterson) while pushing neighbouring quads apart in the unit square.
    // Taking IGN as the sample PAIR was tried first and is wrong: both coordinates come off one lattice, so
    // the pairs lie near a line, the hemisphere is not covered evenly, and the converged image drifts — it
    // measured a consistent -1.5% mean shift while making the error CLUMP more than the white it replaced.
    fn ptSTBN(p : vec2<f32>, f : f32) -> vec2<f32> {
      let off = vec2<f32>(ptIGN(p, 0u), ptIGN(p + vec2<f32>(37.29, 11.73), 32u));
      return r2seq(f, off);
    }
    // ── ONE PATH ── returns the radiance carried back along the primary ray. Everything about it is ordinary
    // path tracing; the only voxel-specific parts are the intersector and the surface-offset epsilons, which
    // are in VOXEL units (one voxel = 10 cm) rather than the fractions of a scene-radius a mesh tracer uses.
    // ── STRATIFYING THE SUN CONE AND THE FIRST BOUNCE WAS TRIED AND REVERTED ── the obvious next dimensions
    // after the sub-pixel jitter, and measured in a three-way A/B in one session they are not worth having:
    // white 2.019, sub-pixel R2 1.285 (-36%), sub-pixel + sun + first bounce 1.278 (-37%). Half a percent of
    // noise for +2.2% of frame time (48.04 -> 49.10 ms), because the extra pairs have to be built for every
    // sample whether the path survives to use them or not. The sub-pixel jitter does essentially all of the
    // work; these dimensions are already well covered by the temporal average.
    fn ptPath(rd0 : vec3<f32>, seed : ptr<function, u32>, t0 : ptr<function, f32>, px : vec2<f32>,
              n0 : ptr<function, vec3<f32>>, a0 : ptr<function, vec3<f32>>) -> vec3<f32> {
      let uw = (u32(u.fx) & 2u) != 0u;                               // camera underwater: water voxels are see-through to the primary ray, exactly as in TRACE
      let sunC = sunTint();                                          // the illuminant, sun or moon — SHARED with the shipping renderer so the two images are lit by the same light
      let sunUp = u.sunDir.y > -0.04;
      let jitK = mix(0.028, 0.009, nightK());                        // the sun's angular radius as TRACE draws it — a real penumbra, sampled once per bounce instead of denoised
      let ceilY = f32((u32(u.fx) >> 8u) & 31u) * 32.0;               // world ceiling: a shadow ray already above it and climbing is clear without a walk
      var ro = u.camPos;
      var rd = rd0;
      var thr = vec3<f32>(1.0);
      var L = vec3<f32>(0.0);
      var inW = uw;                                                  // the path is INSIDE water: every later trace skips water voxels (see the water branch)
      var wJust = false;                                             // …and the segment that just entered it pays Beer-Lambert over its own length
      var spec = false;                                              // the ray currently being traced came off a SPECULAR event (water), so it is exempt from the bounce-distance cap below
      // ── THE BLUE NOISE IS ADDRESSED BY QUAD, NOT BY PIXEL ── coh already hands one bounce direction to a
      // whole 2x2 quad, and that sharing is worth 41% of this pass (see the coh note in PTU). Distributing
      // per pixel would quietly undo it. The quad is the finest grain that exists here, so it is the grain
      // the pattern is spread over: same rays, same cost, better-spread directions between neighbours.
      let cohP = u32(clamp(pt.coh, 0.0, 3.0) + 0.5);
      let quad = vec2<f32>(f32(u32(max(px.x, 0.0)) >> cohP), f32(u32(max(px.y, 0.0)) >> cohP));
      // A DYNAMIC bound on purpose. A literal one invites the compiler to unroll the loop and inline the
      // intersector eight times over, and the register pressure that produces costs more than the branch it
      // saves. PT_MAXB is the ceiling, not the count.
      let nb = min(i32(pt.bounces + 0.5), PT_MAXB);
      for (var b = 0; b < nb; b = b + 1) {
        let tcap = u.rdist.x / max(length(vec2<f32>(rd.x, rd.z)), 0.05);   // the same CIRCULAR render distance the shipping tracer uses, so the two end the world at the same radius
        // ── THE ONE APPROXIMATION IN THE TRANSPORT ── the PRIMARY ray goes the full render distance. A
        // DIFFUSE BOUNCE ray is stopped at pt.secR and, if it gets that far without hitting anything, is
        // shaded as if it had escaped to the sky. Primary rays are perfectly coherent — neighbouring pixels
        // walk neighbouring bricks and the pool stays in cache — while a cosine-sampled bounce ray is
        // maximally incoherent, and that is what the cap is buying back. How much it buys depends entirely
        // on how much geometry is in front of the camera: MEASURED at 2195x885, 4 bounces, secR 512 against
        // uncapped, 35.4 ms vs 42.9 ms over open water, and in a dense oak canopy an earlier build measured
        // 36.8 ms vs 281 ms. It is a floor under the worst case, not a saving on the average one.
        // ── AND IT IS FOR DIFFUSE BOUNCES ONLY ── a water reflection is a MIRROR: what it shows is the far
        // shore, and shortening it does not add a little noise, it deletes the reflection and replaces it
        // with sky. With specular rays capped too, the error against an uncapped reference was rms 17.9/255
        // over the whole frame; exempting them takes it to rms 3.08 with a mean of +1.06/255, against a
        // sampling noise floor of about 1.8 at the 512 samples both sides were measured at. So the remaining
        // bias is at the edge of measurable, and secR = 0 turns the cap off entirely for a strict reference.
        let lim = select(min(4000.0, tcap), min(select(4000.0, pt.secR, pt.secR > 0.0), tcap), b > 0 && !spec);
        let h = traceAll(ro, rd, lim, inW);
        if (b == 0) { *t0 = h.t; *n0 = h.n; *a0 = pal[h.vox].rgb; }   // the G-BUFFER: distance for reprojection, normal and albedo for OIDN's guided model. Both aux planes are noise-free by construction — they are the primary hit, not an estimate.                                   // the PRIMARY hit distance, which is the only depth the reprojection needs
        if (wJust) { thr *= exp(-WATER_SIG * select(60.0, h.t, h.t >= 0.0)); wJust = false; }   // the water column this segment crossed
        if (h.t < 0.0) {
          // ── THE MISS IS THE LIGHT ── skyColor() for the primary ray (sun disc, moon, stars, meteors — what
          // you actually see when you look up) and skyBase() for every bounce after it. That split is not a
          // shortcut: skyBase has no sun disc in it, and the direct sun is already estimated exactly by the
          // shadow ray below, so using the full sky on a bounce would count the sun TWICE.
          // An if, NOT select(): WGSL's select evaluates BOTH arms, and skyColor is the whole night sky —
          // stars, the milky way, the moon photo, eight meteor slots. Paying for it on every bounce that
          // escaped cost more than the bounce did.
          // The deck is composited on the PRIMARY miss only. A bounce ray that escapes is gathering ambient
          // sky, and marching a cloud volume for it would multiply the most expensive term in this renderer
          // by the bounce count for a contribution the eye cannot separate from the sky behind it.
          if (b == 0) { L += thr * ptClouds(skyColor(rd), rd, px); } else { L += thr * skyBase(rd); }
          break;
        }
        let p = ro + rd * h.t;
        let n = h.n;
        // ── WATER: A SMOOTH DIELECTRIC ── Schlick-weighted choice between a mirror reflection and straight-
        // through transmission. Not a refraction: the bend is small at the angles a lake is seen from and the
        // straight path keeps the branch to five lines. Once transmitted the path is flagged inW, so water is
        // invisible to it from then on and it reaches the bed in ONE more bounce instead of ricocheting inside
        // the volume until the budget runs out.
        if (h.vox == WTv || h.vox == WBv) {
          let ci = clamp(abs(dot(rd, n)), 0.0, 1.0);
          // ── THE MIRROR IS SCALED (user 2026-09-03: "lower the reflections by 50%") ── Schlick's reflectance
          // decides how often the path reflects rather than transmits, so scaling it here scales the mirror
          // directly: at reflK 0.5 half the rays that would have bounced off the surface go into the water
          // instead, and the lake shows half as much of the far shore and twice as much of its own bed.
          // Deliberately NOT energy-compensated. The physical answer is F, and this is a LOOK control.
          let F = (0.02 + 0.98 * pow(1.0 - ci, 5.0)) * pt.reflK;
          if (rand(seed) < F) { rd = reflect(rd, n); ro = p + n * 0.02; }
          else { inW = true; wJust = true; ro = p - n * 0.02; }
          spec = true;
          continue;
        }
        let alb = pal[h.vox].rgb;
        // ── A LEAF IS TWO-SIDED ── every renderer that draws a forest has to answer this, and the answer is
        // never "opaque". A real leaf reflects about a tenth of the visible light and TRANSMITS about as much
        // again, which is why a canopy interior is green and legible rather than black. These voxels are
        // opaque 10 cm cubes, so a strict path trace of the geometry puts a forest floor at near zero — and
        // that is a fact about the model, not about the light. pt.folT splits a leaf's albedo into a
        // reflected lobe (1 - folT) and a transmitted one (folT); the two together are exactly alb, so the
        // leaf conserves energy, and folT = 0 reduces every line below to the plain opaque surface it was.
        // The shipping renderer solves the same problem differently — a short AO ray that cannot see the
        // canopy overhead, plus the FOL_STR forward-lobe glow in TRACE — which is why its forest interiors
        // are so much brighter than this one's even at folT > 0.
        let fol = isFol(h.vox);
        let kT = select(0.0, pt.folT, fol);
        // ── NEXT EVENT ESTIMATION ── the sun is a tiny bright disc and a cosine-sampled bounce would find it
        // about once in 100,000 tries, so it is sampled EXPLICITLY every bounce. The cone jitter, the
        // grazing-angle offset and the ceiling escape are all lifted from the sun ray in render/wgsl/trace.js;
        // the difference is that this one is averaged over hundreds of frames instead of denoised.
        // ── AND IT IS SKIPPED ONCE THE PATH CANNOT PAY FOR IT ── measured, the four shadow rays are 15.5 ms
        // of a 35.6 ms frame: 43%, the single biggest line in this renderer. Their COST is flat — each walks
        // up to 1200 voxels — while their CONTRIBUTION is scaled by the path's throughput, which after three
        // diffuse bounces off ~0.15-albedo foliage is a few parts in a thousand. Gating on throughput spends
        // the shadow rays where they can still change a pixel and skips the ones that arithmetically cannot.
        // This is a bias, and a measured one rather than an argued one: see the sweep in the commit note.
        let thrMax = max(thr.r, max(thr.g, thr.b));
        if (sunUp && (u32(pt.dbg) & 1u) == 0u && thrMax > pt.neeMin && (pt.neeB <= 0.0 || f32(b) < pt.neeB)) {
          let st = ptOnb(u.sunDir); let sb = cross(u.sunDir, st);
          var su1 = rand(seed); var su2 = rand(seed);
          // …and the sun's penumbra too, on the primary hit: dappled canopy light is the other half of what
          // reads as static. Offset lattice and frame so it cannot correlate with the bounce above.
          if (b == 0 && pt.bnoise > 0.5) { let ss = ptSTBN(quad + vec2<f32>(91.0, 53.0), pt.seq + 16.0); su1 = ss.x; su2 = ss.y; }
          let sdir = normalize(u.sunDir + st * ((su1 * 2.0 - 1.0) * jitK) + sb * ((su2 * 2.0 - 1.0) * jitK));
          let ndl = dot(n, sdir);
          let front = ndl > 0.0;
          let kS = select(kT, 1.0 - kT, front);                      // which lobe the sun is on: the reflected one, or the transmitted one behind the leaf
          if (kS > 0.0 && abs(ndl) > 0.0) {
            let ns = select(-n, n, front);                           // …and the face the shadow ray has to leave from, or a back-lit leaf shadows itself and transmission reads as nothing
            // The clearance a ray needs off a face is proportional to 1/cos of its angle to that face. A fixed
            // epsilon speckles every flat surface under a low sun — see the long note at the same line in TRACE.
            let so = p + ns * clamp(0.03 / max(abs(dot(n, u.sunDir)), 1e-3), 0.03, 0.8);
            var shMax = 1200.0;
            if (pt.shCap > 0.0) { shMax = pt.shCap; }
            let sCap = select(shMax, min(shMax, (ceilY - so.y) / max(sdir.y, 1e-4)), sdir.y > 1e-4);
            var vis = true;
            if (sCap > 0.0) {
              vis = trace(so, sdir, sCap, inW).t < 0.0;
              if (vis) { vis = bodyTraceX(so, sdir, sCap, true).t < 0.0; }   // occlusion only, so ANY hit ends the walk
            }
            if (vis) { L += thr * alb * sunC * abs(ndl) * kS; }
          }
        }
        L += thr * alb * ambFloor();                                 // the same night/cave floor the composite adds, and for the same reason: with it gone a cave is pure black rather than dark
        thr *= alb;                                                  // ← the whole diffuse BRDF, see ptCosHemi
        // RUSSIAN ROULETTE from the second bounce on: kill low-throughput paths with probability 1-q and
        // scale the survivors by 1/q, which is unbiased — the estimator's expectation is untouched and only
        // its variance moves. Dark albedos are the common case in a forest, so a nominally 4-bounce path
        // usually stops sooner than four.
        if (b >= 1) {
          let q = clamp(max(thr.r, max(thr.g, thr.b)), 0.05, 0.95);
          if (rand(seed) > q) { break; }
          thr = thr / q;
        }
        // Pick a lobe with the same probability as its share of the albedo, so the estimator's weight is
        // (alb * kT) / kT = alb either way and nothing has to be re-weighted here.
        let bn = select(n, -n, kT > 0.0 && rand(seed) < kT);
        // The white draws still happen, so the seed stream is bit-identical either way and an A/B changes
        // the FIRST BOUNCE's direction and nothing else. Deeper bounces keep white noise: their error is
        // already spread over the hemisphere by everything above them, and correlating them across a
        // neighbourhood buys structure rather than quiet.
        var bu1 = rand(seed); var bu2 = rand(seed);
        if (b == 0 && pt.bnoise > 0.5) { let bs = ptSTBN(quad, pt.seq); bu1 = bs.x; bu2 = bs.y; }
        rd = ptCosHemi(bn, bu1, bu2);
        ro = p + bn * 0.02;
        spec = false;
      }
      return L;
    }
    // ── DISTANCE FOG ── composite.js worldFog, ported. That helper lives in COMPOSITE rather than PRE (a
    // WGSL module cannot see another's functions), so this is a copy and the constants have to be kept in
    // step by hand: the exponential is exp(-d * 0.0006) and the far-plane ramp is a fraction of rdist, both
    // exactly as they are there. pt.fog scales the density, so 0.25 is a quarter of the shipping haze.
    // Applied to the AVERAGED radiance rather than per sample: fog is a deterministic function of the primary
    // hit distance, so averaging first and fogging once is identical and costs one evaluation instead of one
    // per sample. It is applied AFTER the history store, so what accumulates stays the clean estimate.
    fn ptFog(col : vec3<f32>, rd : vec3<f32>, dist : f32) -> vec3<f32> {
      if (pt.fog <= 0.0 || dist <= 0.0) { return col; }
      var a = 1.0 - exp(-dist * 0.0006 * pt.fog);
      a = max(a, smoothstep(u.rdist.x * 0.62, u.rdist.x - 6.0, dist * length(vec2<f32>(rd.x, rd.z))) * pt.fog);
      return mix(col, skyBase(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), clamp(a, 0.0, 1.0));
    }
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      let RWu = u32(u.res.x);
      if (gid.x >= RWu || gid.y >= u32(u.res.y)) { return; }
      let idx = gid.y * RWu + gid.x;
      // ── THE CAP ── once the estimate has stopped visibly moving there is nothing left to buy, so the pass
      // still runs (it has to write ptOut) but takes no new samples. A stationary camera therefore converges
      // and then costs nearly nothing, which is what makes flipping to this renderer to LOOK at something
      // practical rather than a fan event.
      let accPrev = ptAcc[idx];
      let done = pt.cap > 0.5 && pt.reset < 0.5 && accPrev.a >= pt.cap;
      var sum = vec3<f32>(0.0);
      var n = 0.0;
      var t0 = -1.0;
      var n0 = vec3<f32>(0.0);
      var a0 = vec3<f32>(0.0);
      var rd0 = rayDir(vec2<f32>(f32(gid.x) + 0.5, f32(gid.y) + 0.5));
      if (!done) {
        // ── SEEDED FROM THE LANE BLOCK, NOT THE PIXEL ── see the coh note in PTU. This one line is the
        // whole optimisation: every random draw a path makes (the sun cone, the water Fresnel coin, the
        // roulette, the leaf lobe, the cosine hemisphere) comes off this seed, so sharing it across a quad
        // makes those four lanes take the SAME branches and walk the SAME bricks. The sub-pixel jitter is
        // deliberately NOT on it — cp below is hashed from gid directly, so the antialiasing stays per
        // pixel however wide this block gets. (dbg bit 1, the white-noise jitter probe, does read the seed
        // and so does share across the quad; it is a measurement path and not a shipped one.)
        var sgx = gid.x; var sgy = gid.y;
        let cohS = u32(clamp(pt.coh, 0.0, 3.0) + 0.5);
        if (cohS != 0u) { sgx = gid.x >> cohS; sgy = gid.y >> cohS; }
        var seed = ((sgx * 1973u) ^ (sgy * 9277u) ^ (u32(pt.seq) * 26699u)) | 1u;
        let sN = i32(pt.spp + 0.5);
        for (var s = 0; s < 4; s = s + 1) {
          if (s >= sN) { break; }
          // A NEW RANDOM SUB-PIXEL POSITION PER SAMPLE — this is the antialiasing. The shipping renderer gets
          // its edges from the TAA resolve's jitter history; here the pixel filter falls out of the estimator.
          // The per-pixel rotation. Hashed from the pixel, so it is fixed for this pixel across frames and the
          // sequence it rotates stays low-discrepancy in the frame index.
          let cp = vec2<f32>(f32((gid.x * 1664525u + gid.y * 1013904223u) & 0xffffu) / 65536.0,
                             f32((gid.y * 1664525u + gid.x * 22695477u) & 0xffffu) / 65536.0);
          // dbg bit 1 restores the old WHITE-NOISE jitter, so the two can be compared in ONE session — the
          // world reseeds on reload, and a cross-reload comparison is two different forests.
          var j = r2seq(pt.seq + f32(s), cp);
          if ((u32(pt.dbg) & 2u) != 0u) { j = vec2<f32>(rand(&seed), rand(&seed)); }   // dbg bit 1 restores white noise for an in-session A/B
          let px = vec2<f32>(f32(gid.x) + j.x, f32(gid.y) + j.y);
          let d = rayDir(px);
          var ts = -1.0;
          var pn = vec3<f32>(0.0); var pa = vec3<f32>(0.0);
          var c = ptPath(d, &seed, &ts, px, &pn, &pa);
          if (s == 0) { n0 = pn; a0 = pa; }
          // ── THE FIREFLY GUARD, AND IT IS OFF BY DEFAULT BECAUSE IT MEASURED AS NOTHING ── clamping looked
          // like a 31% noise win on a single capture, and a fuller protocol (two scenes, three reps each,
          // every configuration captured inside each rep) put it at zero once temporal reprojection is on:
          // scene 1 reproj -47/-47/-50% against clamp -45/-46/-50%, scene 2 -54/-57% against -56/-58%.
          // The reason is that the reprojection is ALREADY a strong temporal average, and the outliers this
          // targets are exactly what such an average removes first — the two are redundant. Kept switchable
          // because it is the right tool if the history is ever shortened or reprojection is turned off.
          // ── the guard itself ── a path that finds the sun through a one-voxel gap after three bounces
          // returns a radiance hundreds of times the pixel's mean. It is a legitimate sample and its
          // expectation is correct, but it lands as a single white speck that the temporal average then
          // smears over many frames, and a handful of them dominate what the eye calls noise. Clamping the
          // per-SAMPLE radiance loses the energy in that tail — it is a bias, and a visible one on genuinely
          // bright specular glints — so the threshold is a knob and the measurement is in the commit note.
          if (pt.fClamp > 0.0) {
            let m = max(c.r, max(c.g, c.b));
            if (m > pt.fClamp) { c = c * (pt.fClamp / m); }
          }
          sum += c;
          if (s == 0) { t0 = ts; rd0 = d; }                          // the first sample owns the depth this pixel reprojects by
          n += 1.0;
        }
      }
      // ══ TEMPORAL REPROJECTION ══ the estimate used to be thrown away the instant the camera moved, which is
      // why looking around dropped you to a 1-sample image. The world did not change — only where it is on the
      // screen — so the previous frame's radiance is still valid for the same SURFACE, just at a different
      // pixel. prevUVd (render/wgsl/pre.js) already projects a direction through the previous camera for the
      // shipping SVGF; feeding it (hitPoint - u.pPos) turns it into a full reprojection that handles walking
      // as well as looking, and it needs no new uniform because the previous camera is already published.
      // VALIDATED ON DISTANCE, or a disocclusion drags the wrong surface's colour across the screen: the
      // history is reused only where what it recorded is the same distance away as what this pixel just hit.
      // A SKY pixel is exempt and always valid — its reprojection is exact, being a pure direction.
      var prev = vec4<f32>(0.0);
      if (pt.reset < 0.5) { prev = accPrev; }
      else if (pt.reproj > 0.5) {
        let hp = u.camPos + rd0 * max(t0, 0.0);
        let uv = prevUVd(select(rd0, hp - u.pPos, t0 > 0.0));
        if (uv.x >= 0.0 && uv.x < 1.0 && uv.y >= 0.0 && uv.y < 1.0) {
          let hs = textureLoad(ptHistIn, vec2<i32>(uv * u.res), 0);
          let want = select(-1.0, length(hp - u.pPos), t0 > 0.0);
          // An if, not a select(). A select whose first two arguments both contain comparisons makes the WGSL
          // parser read the less-than and greater-than as a TEMPLATE LIST, and the whole module fails to
          // compile: a black canvas at absurd fps, reported only in __vbGpuErr and never in __vb.errLog().
          var ok = false;
          if (t0 > 0.0) { ok = hs.a > 0.0 && abs(hs.a - want) <= 0.05 * want + 1.0; }
          else { ok = hs.a < 0.0; }
          if (pt.reproj > 1.5) { ok = true; }          // DIAGNOSTIC (reproj = 2): accept unconditionally, to separate a plumbing fault from a validation one
          // ── AND IT IS ONE NEAREST TEXEL, NOT A BILINEAR FETCH (user 2026-09-03: "the blur is terrible now")
          // ── four validated bilinear taps were built here and REVERTED. Averaging the four neighbouring
          // history texels is a SPATIAL FILTER, and because the history feeds back into itself every frame
          // the softening COMPOUNDS: a little blur per frame becomes a lot of blur in a second. It measured
          // as a 37% error reduction against a converged reference, which is exactly the trap — that metric
          // cannot tell denoising from blurring, and rewards a blur every time. It is the same measurement
          // that recommended the a-trous filter the owner rejected in September, for the same reason.
          // A nearest tap keeps the image SHARP and leaves the motion noise; that trade is the owner's.
          // K is the history LENGTH, not a real sample count: reusing it as a fixed weight makes this an
          // exponential average with a 1/(K+1) response, which is what keeps a moving image both quiet and
          // able to follow a change instead of smearing it indefinitely.
          if (ok) { let K = max(pt.hist, 1.0); prev = vec4<f32>(hs.rgb * K, K); }
        }
      }
      let acc = prev + vec4<f32>(sum, n);
      ptAcc[idx] = acc;
      var col = acc.rgb / max(acc.a, 1.0);
      let colStore = col;
      textureStore(ptHistOut, vec2<i32>(gid.xy), vec4<f32>(colStore, t0));
      col = ptFog(col, rd0, t0);                                     // …after the store, so the accumulator never sees the haze
      // LINEAR out. The tonemap moved to the blit, because everything that may sit between them — the OIDN
      // pass — has to see real radiance. A sky pixel writes a zero normal and its own colour as albedo, which
      // is what OIDN expects for a background.
      textureStore(ptOut, vec2<i32>(gid.xy), vec4<f32>(col, 1.0));
      var albO = a0; if (t0 < 0.0) { albO = vec3<f32>(1.0); }
      // DIAGNOSTIC (dbg = 7): the accumulator's WEIGHT, not a colour — mean effective samples per pixel,
      // scaled by 1/64 so a probe of this plane reads back as a fraction. It is the one number that says
      // whether a moving image is noisy because the history is being REJECTED or because the estimate
      // behind it is simply that loud, and the two want opposite fixes.
      if (pt.dbg > 6.5) { albO = vec3<f32>(acc.a / 64.0); }
      textureStore(ptAlb, vec2<i32>(gid.xy), vec4<f32>(albO, 1.0));
      textureStore(ptNrm, vec2<i32>(gid.xy), vec4<f32>(n0, 1.0));
    }`;

  // ── PTBLIT_SRC ── the presentation pass: upscale the (renderScale-sized) path-traced target onto the
  // canvas. Deliberately NOT the shipping BLIT: that one draws the held viewmodel, the hurt flash, the lens
  // flare and the vignette, none of which this renderer produces, and reusing it would have put a lit axe in
  // front of an unlit world.
  // ── A SPATIAL DENOISER WAS BUILT HERE AND REMOVED (user 2026-09-03: "revert your recent denoising
  // changes, it blurred the image") ── an edge-aware a-trous filter, three passes at a doubling stride with
  // SVGF's depth/normal/luminance weights. It measured well on the metric it was tuned against — moving-image
  // high-frequency energy down 64-70% for no measurable frame cost — and the owner's eye rejected it, which
  // outranks the metric. A spatial filter buys quiet by spending sharpness, and at 1 spp there is not enough
  // signal for the guides to protect the detail that spending takes.
  // WHAT THAT LEAVES: temporal reprojection, which is the denoiser that does NOT trade sharpness, because it
  // borrows real samples of the same surface from the previous frame rather than averaging neighbours.
  // If this is ever revisited, the honest lesson is that the noise metric and the eye disagreed, and the
  // metric was the thing that was wrong.
  const PTBLIT_SRC = () => /* wgsl */`
    @group(0) @binding(1) var ptSrc : texture_2d<f32>;
    @group(0) @binding(2) var ptSamp : sampler;
    @vertex fn vs(@builtin(vertex_index) vi : u32) -> @builtin(position) vec4<f32> {
      var P = array<vec2<f32>, 3>(vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0), vec2<f32>(-1.0, 1.0));
      return vec4<f32>(P[vi], 0.0, 1.0);
    }
    @fragment fn fs(@builtin(position) fc : vec4<f32>) -> @location(0) vec4<f32> {
      var c = textureSampleLevel(ptSrc, ptSamp, fc.xy / u.canvasRes, 0.0).rgb;
      c = aces(c * 0.95);                                            // the composite's own tonemap and gamma — here rather than in the trace pass, so the denoiser sees linear light
      return vec4<f32>(pow(c, vec3<f32>(1.0 / 2.2)), 1.0);
    }`;
