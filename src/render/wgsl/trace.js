  const TRACE_SRC = ({ DDAW, FLAKEBLK, pickWGSL, POOL }) => /* wgsl */`
${POOL ? `    @group(0) @binding(1) var<storage, read> pool : array<u32>;    // ── PAGED BRICK POOL ── 512-byte payloads for OCCUPIED bricks only; the ~47% of the window that is empty sky has none
    @group(0) @binding(2) var<storage, read> bdesc : array<u32>;   // per-brick: 0 = all air, else slot+1 into pool. Replaces the occupancy bitmask - a nonzero descriptor IS the occupancy bit, which keeps TRACE at exactly 8 storage buffers (the WebGPU default cap)`
        : `    @group(0) @binding(1) var<storage, read> world : array<u32>;
    @group(0) @binding(2) var<storage, read> bricks : array<u32>;`}
    @group(0) @binding(3) var<storage, read> pal : array<vec4<f32>>;
    @group(0) @binding(4) var gAlbedo : texture_storage_2d<rgba8unorm, write>;
    @group(0) @binding(5) var gIrr : texture_storage_2d<rgba16float, write>;
    @group(0) @binding(7) var<storage, read> wbricks : array<u32>;   // water-only brick bits — skipW rays stride these
    @group(0) @binding(8) var slotOut : texture_storage_2d<r32uint, write>;   // ── DYNAMIC LIFE ── per-pixel creature id: bits 0-7 = drop slot + 1 (0 = terrain/sky), bits 8-10 = model-space hit axis*2+signBit (composite rebuilds the TRUE rotated normal from it)
    ${pickWGSL}
    ${DDAW}
    @group(0) @binding(6) var<storage, read> bricks2 : array<u32>;   // 32-voxel SUPER-brick occupancy (window origin is 32-aligned, so off>>5 is exact)
    const SNV : u32 = ${location.search.includes('flakedbg') ? LAVA_T : SNOW[0]}u;                                   // falling-flake voxel id — flakes are REAL primary hits (?flakedbg paints them as emissive lava to bisect trace-vs-downstream)
    const RNV : u32 = ${RAIN}u;                                                                       // falling-RAIN voxel id — the oak forest's weather. One palette id (see assets/palette.js): rain is a colour here and nothing else, because unlike SNOW it is never written into W
    // ── RAIN, THE TUNING, IN ONE PLACE ── every number the rain march uses. RAIN_MULT is the whole of "much
    // faster" (user): the drop lattice falls at RAIN_MULT x the flake lattice, i.e. 55 vox/s against snow's 11,
    // which is 5.5 m/s — real rain's terminal velocity is 5-9 m/s and a snowflake's is 1-1.5, so the ratio is
    // about right as well as being what was asked for. It multiplies the SAME accumulator the flakes use
    // (u.pickY.w, integrated on the CPU at a constant 11 vox/s) rather than integrating u.time here, so rain
    // and snow can never drift apart and no new uniform lane is needed.
    // RAIN_HW/RAIN_HH are the drop's half-extents in voxels: 6 cm wide, 21 cm tall. A cube falling five times
    // faster reads as HAIL, not rain — rain is legible because it is a streak — and the stretch is genuinely
    // cheap here: the drop does not spin (a raindrop does not tumble), so the two-stage conservative-slab +
    // rotate dance the flakes need collapses to ONE exact axis-aligned slab test, and the rain march is
    // cheaper per cell than the snow one despite being longer. RAIN_HH is capped at 1.05 for a hard reason,
    // not a taste one: the DDA walks 3-voxel cells and the jitter below keeps the whole drop INSIDE its own
    // cell, so a taller drop would straddle a cell boundary and grazing rays would slice pieces off it.
    const RAIN_MULT : f32 = 5.0; const RAIN_HW : f32 = 0.30; const RAIN_HH : f32 = 1.05;
    const RAIN_THR : f32 = 0.9755;                                                                   // SAME lattice occupancy as snow (~2.45%), so rain is exactly as dense as the snowfall it replaces and costs the same to march. A drop covers 2.1 x 0.6 voxels against a flake's 1 x 1, so it is ~25% more screen per drop and a great deal more legible; this is the one number to move if the storm wants to be heavier or lighter
    const RAIN_GATE : f32 = 350.0;                                                                   // how far past the oak BAND's own edge the camera still counts as "could this ray see the oak forest at all" (see oakNear below, which measures a wrapped DISTANCE and so needs no direction) — 150 for the flake march's own reach plus 200 for how far the border's meander can wander over the +-150 of z the same disc covers
    const LVT : u32 = ${LAVA_T}u; const LVB : u32 = ${LAVA_B}u; const LVR : u32 = ${LAVA_R}u; const LVY : u32 = ${LAVA_Y}u;
    // ── THE DESERT, ON THE GPU ── falling flakes are traced voxels in a world-space lattice, not particles the
    // CPU places, so the JS gate that keeps the BLANKET off the sand cannot reach them: without this it snows
    // over the desert and simply never settles. desertM is ported here rather than passed as a uniform because
    // it is pure (x, z) and the spawn it is anchored to is already fixed by the time this template runs
    // (build.js is manifest line 24, this is line 29). f32 instead of the CPU's f64 is fine — half a voxel of
    // disagreement at the border is invisible, and nothing downstream compares the two.
    fn dHash(x : i32, z : i32) -> f32 {                // the JS ihash, same mix, same constants
      var h : u32 = u32(x) * 374761393u + u32(z) * 668265263u;
      h = (h ^ (h >> 13u)) * 1274126177u;
      return f32(h ^ (h >> 16u)) / 4294967296.0;
    }
    fn dSstep(t : f32) -> f32 { return t * t * (3.0 - 2.0 * t); }
    fn dVnoise(x : f32, z : f32) -> f32 {
      let ix = i32(floor(x)); let iz = i32(floor(z));
      let fx = dSstep(x - floor(x)); let fz = dSstep(z - floor(z));
      return (dHash(ix, iz) * (1.0 - fx) + dHash(ix + 1, iz) * fx) * (1.0 - fz)
           + (dHash(ix, iz + 1) * (1.0 - fx) + dHash(ix + 1, iz + 1) * fx) * fz;
    }
    // ── THE BIOME CYCLE, ON THE GPU (user 2026-08-18: "make sure the landscapes keep cycling, endlessly") ──
    // world/window.js pwrap, ported bit for bit. floor(d/BIOP + 0.5) and NOT round(): WGSL's round breaks ties
    // to even and JS's Math.round breaks them upward, and the two sides of this border must agree — the snow
    // and rain marches here decide what FALLS while the CPU decides what SETTLES, so a disagreement is visible
    // as flakes falling on ground that refuses them.
    fn pwrapG(d : f32) -> f32 { return d - floor(d / f32(${BIOP}) + 0.5) * f32(${BIOP}); }
    fn desertMask(x : f32, z : f32) -> f32 {
      let c = f32(${SPWX + DESC - desWob(SPWZ)}) + (dVnoise(z * ${WOB_DES1} + 27.9, 83.1) - 0.5) * ${DESW}.0
                                 + (dVnoise(z * ${WOB_DES2} + 11.2, 51.7) - 0.5) * ${DESW * 0.35};   // the frequencies are INTERPOLATED from world/window.js (WOB_DES1/DES2/OAK/CH), never typed: a hand-copied one drifted to ~1.9x and put the falling snow on a different border from the blanket (see the note there)
      let t = 0.5 + (f32(${DESH}) - abs(pwrapG(x - c))) / ${DESB}.0;   // a BAND on a wrapped distance now, exactly as the JS desertM is
      if (t >= 1.0) { return 1.0; }
      if (t <= 0.0) { return 0.0; }
      return dSstep(t);
    }
    // ── THE BIRCH FOREST TAKES SNOW (user 2026-08-24) ── it used to have a birchMask here refusing flakes over
    // the band, the falling half of a two-gate rule whose settling half is in main/tick-snow.js. Both are gone.
    // The pairing itself is the thing worth keeping in mind: that file decides what SETTLES and this decides
    // what FALLS, so a band named in one and not the other either snows on ground that refuses to keep it, or
    // collects a blanket under an empty sky.
    // ── AND THE OAK FOREST, ON THE GPU, FOR THE SAME REASON ── read the note above desertMask first: it is
    // the precedent and this is the same move. A falling flake or drop is a traced voxel in a world-space
    // lattice, so the CPU's oakM cannot reach it — the JS gate in landSnowAt decides only what SETTLES. This
    // is oakM(x, z) from world/window.js ported bit-for-bit (oakWob = 0.6 of the desert's own meander plus
    // one independent octave; the mask runs the other way round, 1 = deep oak in the WEST), not a uniform,
    // because it is pure (x, z) and SPWX is already fixed by the time this template runs (build.js is
    // manifest line 24, this is line 29). f32 against the CPU's f64 is fine for the same reason: half a
    // voxel of disagreement at a 450-voxel blend band is invisible and nothing downstream compares the two.
    // It carries the whole of "it snows in the pines and rains in the oaks AT THE SAME TIME": the snow march
    // culls a flake where this says oak, the rain march culls a drop where it says pine, and both dither on
    // it, so the two weathers cross-fade across the border exactly as the canopies and the ground cover do.
    fn oakWobG(z : f32) -> f32 {                       // = the JS oakWob(z), which is deliberately part-shared with desWob so the two borders stay broadly parallel
      return ((dVnoise(z * ${WOB_DES1} + 27.9, 83.1) - 0.5) * ${DESW}.0
            + (dVnoise(z * ${WOB_DES2} + 11.2, 51.7) - 0.5) * ${DESW * 0.35}) * 0.6
           + (dVnoise(z * ${WOB_OAK} + 143.7, 61.3) - 0.5) * ${OAKW}.0;
    }
    // the band's CENTRE LINE at this z — its own function because the oakNear gate below has to measure the
    // camera's distance to the band, and a second copy of this expression is a second thing to keep in step.
    fn oakCentreG(z : f32) -> f32 { return f32(${SPWX + OAKC - oakWob(SPWZ)}) + oakWobG(z); }   // the wobble is pinned at the spawn's own z, exactly as the JS does, or how far spawn sits from the border is a per-session lottery
    fn oakMask(x : f32, z : f32) -> f32 {              // 1 = deep oak forest, 0 = pine forest — the mirror of desertMask
      let c = oakCentreG(z);
      let t = 0.5 + (f32(${OAKH}) - abs(pwrapG(x - c))) / ${OAKB}.0;   // a BAND, like the JS oakM: a half-plane has no west edge for the cycle to close against
      if (t >= 1.0) { return 1.0; }
      if (t <= 0.0) { return 0.0; }
      return dSstep(t);
    }
    // ── AND THE BLOSSOM BAND IS CUT BACK OUT OF IT, ON THE GPU TOO (user 2026-08-18: "in the cherry biome
    // make it snow like the pine forest") ── read the oakWeather note in world/window.js first; this is that
    // scalar ported the same way, and for the same reason oakMask itself is ported: a falling flake is a
    // traced voxel in a world-space lattice, so the CPU's masks cannot reach it and the JS gate in landSnowAt
    // decides only what SETTLES. Without this the band keeps oakMask's verdict, every flake over it is culled,
    // and the blanket the CPU now lays there appears under a clear sky — the two halves of the weather
    // disagreeing in the one way the desert note above exists to prevent.
    fn chWobG(z : f32) -> f32 {                        // = the JS chWob(z): 0.6 of the OAK meander (which already carries 0.36 of the desert's) plus one independent octave, so all three borders stay broadly parallel
      return oakWobG(z) * 0.6 + (dVnoise(z * ${WOB_CH} + 211.3, 97.7) - 0.5) * ${CHW}.0;
    }
    fn cherryMask(x : f32, z : f32) -> f32 {           // 1 = inside the blossom band, 0 = the oak forest either side of it. A DISTANCE from the centre line, not a side of it — that is what makes it a band rather than a half-plane
      let b = f32(${SPWX - CHOFF - oakWob(SPWZ)}) + oakWobG(z);   // oakWobG, NOT chWobG — the JS cherryM rides the OAK meander now (see CHHALF in world/window.js), and a stale copy here is the exact failure the desWob note at the top of this file is about: flakes culled on one border while the blanket settles on another. Still pinned at the spawn's own z exactly as the JS is
      let t = (${CHHALF + CHB}.0 - abs(pwrapG(x - b))) / ${CHB}.0;   // wrapped, or the blossom exists in the first period only and every later one falls back to plain oak weather
      if (t >= 1.0) { return 1.0; }
      if (t <= 0.0) { return 0.0; }
      return dSstep(t);
    }
    // ── THE WEATHER BORDER'S CONTRAST CURVE ── world/window.js wSharp, ported bit for bit. The blanket is
    // decided on the CPU and the falling flakes here, so if only one side sharpened its border the other would
    // disagree across the whole ramp — snow settling under a clear sky, or flakes falling on ground that
    // refuses them. Every weather test on this side goes through it, exactly as every one on that side does.
    fn wSharpG(m : f32) -> f32 {                       // 0.06..0.70 and LINEAR, in step with the JS — see world/window.js, which carries the reasoning. The two MUST match or the flakes and the blanket disagree across the band: this decides where a FLAKE is culled and wSharp decides where the BLANKET settles, so a stale copy here is snow lying under a clear sky
      if (m <= 0.06) { return 0.0; }
      if (m >= 0.70) { return 1.0; }
      return (m - 0.06) / 0.64;
    }
    fn cherryMaskW(x : f32, z : f32) -> f32 {          // the blossom band as the WEATHER sees it — cherryMask's twin on the wider CHBW ramp (world/window.js cherryW). Same centre and same CHHALF, so WHERE it snows is identical and only the fade length differs
      let b = f32(${SPWX - CHOFF - oakWob(SPWZ)}) + oakWobG(z);
      let t = (${CHWHALF + CHBW}.0 - abs(pwrapG(x - b))) / ${CHBW}.0;   // CHWHALF, matching the JS cherryW: the weather band kept its own half-width when the tree band was reshaped
      if (t >= 1.0) { return 1.0; }
      if (t <= 0.0) { return 0.0; }
      return dSstep(t);
    }
    fn oakWeather(x : f32, z : f32) -> f32 { return oakMask(x, z) * (1.0 - cherryMaskW(x, z)); }   // what every WEATHER site below asks. oakMask itself stays the faithful port of oakM, because the worldgen questions still want that one
    ${FLAKEBLK}
    fn onbT(n : vec3<f32>) -> vec3<f32> {
      return normalize(select(cross(n, vec3<f32>(0.0, 1.0, 0.0)), cross(n, vec3<f32>(1.0, 0.0, 0.0)), abs(n.y) > 0.9));
    }
    fn cosHemi(n : vec3<f32>, r1 : f32, r2 : f32) -> vec3<f32> {
      let a = 6.2831853 * r1; let r = sqrt(r2);
      let t = onbT(n); let b = cross(n, t);
      return t * (r * cos(a)) + b * (r * sin(a)) + n * sqrt(max(1.0 - r2, 0.0));
    }
    fn ign(p : vec2<f32>) -> f32 { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }
    @group(0) @binding(23) var<storage, read> bodyVox : array<u32>;  // ── RIGID BODIES ── dense per-body voxel grids (palette id per cell, 0 = empty), sub-allocated back to back
    ${UNI_FN}
    @group(0) @binding(9) var<storage, read> visb : array<u32>;      // ── DYNAMIC LIFE tile cull ── per-8×8-tile drop-slot visibility bitmask (4×u32/tile), computed ONCE per frame by the tiny VIS prepass and shared with the composite — replaces the per-workgroup recompute + workgroupBarrier that ran here even on creature-free frames
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>, @builtin(workgroup_id) wgid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let px = vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y);
      let rd = rayDir(px);
      let ro = u.camPos;                                             // window-relative
      ${!UNI_RAY ? '' : 'let tiS = (wgid.y * ((u32(u.res.x) + 7u) / 8u) + wgid.x) * 8u + 4u; let sg0 = visb[tiS]; let sg1 = visb[tiS + 1u]; let sg2 = visb[tiS + 2u]; let sg3 = visb[tiS + 3u]; let secN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);'}
      let skipW = (u32(u.fx) & 2u) != 0u;                            // camera underwater → water voxels are see-through
      let tcap = u.rdist.x / max(length(vec2<f32>(rd.x, rd.z)), 0.05);   // CIRCULAR render distance (slider) — the world ends at a radius, not a square edge
      var tEye = 0.0;                                                // CLIP-THROUGH FIX: the eye poking into canopy/terrain voxels made every ray hit unlit solid at t=0 — a full BLACK frame.
      {                                                              // Slide this ray's start past up to 3 voxels of solid; deeper stays dark (no x-ray through real walls). Water is
        let ec0 = vec3<i32>(floor(ro));                              // exempt on both ends — swimming already handles it via skipW.
        let v0 = voxAt(clamp(ec0, vec3<i32>(0), vec3<i32>(WX - 1, WY - 1, WZ - 1)));
        if (ec0.y > 0 && ec0.y < WY - 1 && v0 != 0u && v0 != WTv && v0 != WBv) {
          for (var q = 1; q <= 12; q++) {
            let tq = f32(q) * 0.25;
            let pq = vec3<i32>(floor(ro + rd * tq));
            if (pq.y <= 0 || pq.y >= WY - 1) { tEye = tq; break; }
            let vq = voxAt(clamp(pq, vec3<i32>(0), vec3<i32>(WX - 1, WY - 1, WZ - 1)));
            if (vq == 0u || vq == WTv || vq == WBv) { tEye = tq; break; }
          }
        }
      }
      if (FOLSKIP) {                                                 // ── FOLIAGE SEE-THROUGH ── if the eye sits inside a leaf voxel, make near foliage transparent to the PRIMARY ray only
        let ce = vec3<i32>(floor(ro));                               // (this whole block only exists in the see-through pipeline variant — the CPU picks it when the eye is within 1 voxel of foliage)
        if (ce.y > 0 && ce.y < WY - 1 && isFol(voxAt(clamp(ce, vec3<i32>(0), vec3<i32>(WX - 1, WY - 1, WZ - 1))))) { folSkipD = 30.0; }   // ~3 m: clears the crown you clipped into; distant canopy still renders normally
      }
      var h = trace(ro + rd * tEye, rd, min(4000.0, tcap) - tEye, skipW);
      folSkipD = 0.0;                                                 // reset before the sun / AO / lava / reflection traces below — they must still see the canopy as solid
      if (h.t >= 0.0) { h.t += tEye; }
      var flakeHit = false; var flakeFade = 1.0; var flakeUnder = 0u;                                        // set when the primary hit IS a falling flake — the lighting below gives those a scatter floor (see the sunV/skyV clamp)
      if ((u32(u.fx) & 16u) != 0u) {                               // FALLING SNOW — flakes are REAL VOXELS in the primary trace: grain, the jittered sun ray, AO, fog and the denoiser treat them exactly like the static world
        var rdS = rd;
        if (abs(rdS.x) < 1e-6) { rdS.x = 1e-6; } if (abs(rdS.y) < 1e-6) { rdS.y = 1e-6; } if (abs(rdS.z) < 1e-6) { rdS.z = 1e-6; }
        let inv = 1.0 / rdS;
        let winOf = vec3<f32>(u.winO.x, 0.0, u.winO.y);
        var fte = 1e9;
        var fnn = vec3<f32>(0.0); var fface = 0u; var fkAw = 1.0; var fkUw = 0u;   // …and the winner's fade + what is under it
        var fIsRain = false;                                         // the winner is a RAIN drop rather than a snow flake — the two marches compete on the same fte
        // ── CAN THIS RAY SEE THE OAK FOREST AT ALL? ── one mask sample, uniform-valued across the whole workgroup,
        // and it is what keeps rain free for the ~99% of the world that is pine forest and desert. The flake
        // march reaches at most 150 voxels, so a camera far enough EAST of the oak border cannot have a drop in
        // frame; RAIN_GATE pads that reach by how far the border's meander can wander over the same +-150 of z,
        // so the test is conservative (it can only ever turn rain ON early, never off late — a missing band of
        // weather at the edge of view would be the one failure mode worth avoiding). oakMask is monotone in x,
        // so sampling the disc's WEST extreme is the maximum over the disc and no second sample is needed.
        // When it is false the two blocks below are bit-identical to the snow march that shipped before rain.
        // RAIN_ON false => this interpolates to a literal false, so the snow cull below and the whole rain march
        // further down are dead code the WGSL compiler drops: the oak forest snows exactly like the pine
        // forest again and no ray pays for a second lattice.
        // Sampled if EITHER consumer is live - the flake cull (when snow is off in the oak forest) or the
        // rain march (when rain is on). With both off this interpolates to a literal false and the whole
        // oak weather path is dead code the compiler drops.
        // ── THIS GATE IS A CONSERVATIVE BOUND AND MUST NOT BE SHARPENED (user 2026-08-18: "you also seem to
        // make it snow in the oak forest ... the snow still instantly appears when transitioning biomes") ── it
        // is a CAMERA test that decides whether the per-flake cull below runs AT ALL, so it has to stay true
        // anywhere a visible flake might need culling. Wrapping it in wSharpG (which I did when the border was
        // tightened) made it binary and narrow, and broke the snow two ways at once:
        //   * stand in the blossom band and oakWeather is 0, so the gate went false and NOT ONE flake was
        //     culled anywhere in view — the whole field switched on the instant you crossed;
        //   * stand just inside the oak forest where oakWeather is still under 0.2 and the gate went false
        //     there too, so it snowed in the oak forest.
        // The RAW mask, with RAIN_GATE's margin, is the right bound: true across the whole blend and false only
        // well inside the biomes that never cull. The SMOOTH transition belongs on the per-flake test below,
        // which reads the FLAKE's own position — a gate on the camera can only ever be on or off.
        // ── AND IT IS A DISC, NOT A DIRECTION (user 2026-08-21: "its snowing in the pine forest, but is having the
        // snow fall in the oak forest … the snow just instantly appearing when switching biomes") ── the note above
        // is right about WHAT this gate must be and the old expression stopped being it, twice over. Both faults
        // are in the one line, and both were introduced by changes elsewhere that had no reason to look here:
        //   * "oakMask is monotone in x, so the WEST extreme is the maximum over the disc" was true of a HALF-PLANE.
        //     oakM became a BAND on 2026-08-18 so the cycle could close, and a band is not monotone in anything:
        //     sampling one side of the camera says nothing about the other.
        //   * BAND_MIRROR (2026-08-20) then put the oak forest EAST of the pines. minus-RAIN_GATE samples WEST, so on
        //     the border the player actually walks the margin now points away from the forest it exists to find.
        // WHAT THE PLAYER SAW. Standing in the pines, the sample landed in more pine, oakNear went false, and the
        // per-flake cull below did not run AT ALL — so it snowed across every oak column in view, out to the march's
        // own 150-voxel reach. Then walking east the gate flipped true at a fixed camera x (~125 voxels PAST the
        // treeline, inside the oak), and the whole flake field started being culled between one step and the next.
        // That is both halves of the report: too far out, and then a switch rather than a fade. The BLANKET was
        // never affected because tick-snow.js asks wSharp per settling column with no camera gate at all, which is
        // exactly why the ground snow reads as correct and the falling snow does not.
        // THE FIX IS THE BOUND THE NOTE ALREADY ASKS FOR, stated properly: is the camera within RAIN_GATE of the oak
        // BAND, measured on the wrapped distance to its centre line. Direction-free, so it cannot care which way
        // round BAND_MIRROR has the world; wrapped, so it stays true in every period of the cycle; and still one
        // mask sample, uniform across the workgroup, so the pine forest and the deep desert pay what they always did.
        // It uses oakM's own reach (OAKH + OAKB/2 — outside that the mask is exactly 0) rather than oakWeather's,
        // because oakWeather <= oakM everywhere and a bound that is too WIDE only costs a little work, while one
        // that is too narrow is the bug above.
        let oakNear = ${(!OAK_SNOW || RAIN_ON) ? 'abs(pwrapG(ro.x + winOf.x - oakCentreG(ro.z + winOf.z))) < f32(' + (OAKH + OAKB * 0.5) + ') + RAIN_GATE' : 'false'};
        {                                                            // near field: DDA over the 3-voxel flake lattice — a flake stays WHOLE from sky to ground.
          // A tile-binned producer pass was tried here (enumerate+bin flakes once, pixels read their tile): the TRACE
          // side dropped to +0.35 ms, but on this driver ANY producer dispatch with atomics OR workgroup barriers costs
          // ~3 ms flat (hash-only = 0.04 ms — measured either way), so the march wins on total. Its own order is
          // OPTIMIZED: hash gate → storm-band check → free conservative slab vs the spin bound → exact rotated hit →
          // flakeBlocked (~38 scattered loads) LAST, only for a ray that genuinely strikes the flake. All pure
          // predicates — the image is identical to the original ordering.
          let fall = vec3<f32>(-u.rdist.z + sin(u.time * 0.6) * 0.8, u.pickY.w, -u.rdist.w + cos(u.time * 0.5) * 0.8);   // u.pickY.w = integrated fall (doubles when standing still) + gusting wind
          // ── LARGE-COORDINATE PRECISION (user: "the snow clips into 2 sections") ── the old march ran on ro+winOf+fall
          // directly: at a far spawn (|world| ~1e5-4e5) f32 keeps only ~0.03 vox, and every slab/DDA test inherited that
          // error → flakes silently lost in view-dependent sectors. Split the big offset into an EXACT integer cell
          // offset (offC, fed to the hash) + a small [0,3) remainder (offR) and run the whole march in window-local
          // floats — bit-identical cells, full sub-voxel precision at ANY world position.
          let off = winOf + fall;
          let offC = vec3<i32>(floor(off * (1.0 / 3.0)));            // integer 3-vox-cell offset — exact (|off|/3 ≤ ~1.4e5, and f32 holds integers to 16.7M)
          let offR = off - vec3<f32>(offC) * 3.0;                    // remainder in [0,3) — small, continuous as the wind/fall drift
          let roP = ro + offR;                                       // window-local march origin (≤ ~2051 in magnitude → ~1e-4 vox precision)
          let maxNear = min(min(select(1e9, h.t, h.t >= 0.0), u.rdist.x), 150.0);
          var c = vec3<i32>(floor((roP + rd * 0.3) * (1.0 / 3.0)));  // exact third (0.3333 undershot by 1e-4 relative — harmless here, but keep it exact)
          let stp = vec3<i32>(i32(sign(rdS.x)), i32(sign(rdS.y)), i32(sign(rdS.z)));
          let tDel = abs(inv) * 3.0;
          var tMax = ((vec3<f32>(c) + max(vec3<f32>(stp), vec3<f32>(0.0))) * 3.0 - roP) * inv;
          for (var it = 0; it < 96; it++) {
            let h1 = ih3(c.x + offC.x, (c.y + offC.y) * 7, c.z + offC.z);   // hash on the WORLD cell (local + exact offset) — the same integers as before, so the flake set is unchanged
            if (h1 > 0.9755) {                                       // ~2.45% of cells (halved again per user)
              let ctr = (vec3<f32>(c) + vec3<f32>(0.17 + 0.66 * fract(h1 * 43758.5), 0.17 + 0.66 * fract(h1 * 12345.7), 0.17 + 0.66 * fract(h1 * 7777.3))) * 3.0;
              let cwy = ctr.y - offR.y;                              // storm-band pre-check — ctr is window-local, so minus the small remainder IS the world y (winOf.y = 0)
              if (cwy >= u.misc.y && cwy <= u.misc.z) {
              let fwp = ctr - offR + winOf;                          // window-local -> world, for the biome tests. HOISTED out of the slab block below because the oak cull on the next line needs it too
              // ── AND IT DOES NOT SNOW IN THE OAK FOREST (user: "instead of snow in the oak forest, make it rain") ──
              // the desert cull further down thins snow out into the sand; this does the same at the other border,
              // on the SAME dHash draw and the same salt, so a cell that refuses a flake is a cell the rain march
              // below is free to fill. Two things about where it sits. It is EARLY — ahead of the conservative
              // slab and the per-flake rotation the desert test deliberately sits behind — because in the oak
              // forest this culls essentially every flake, so paying 12 hashes to skip the trig is the cheap
              // direction, and it is what keeps the snow march nearly free while you stand in the rain. And it is
              // behind the oakNear gate, so in the pine forest and the desert not one instruction of it executes and the
              // snow that ships today is untouched, hash for hash.
              var oakCull = false;
              if (${!OAK_SNOW ? 'oakNear' : 'false'}) { oakCull = dHash((c.x + offC.x) * 5 + 17, (c.z + offC.z) * 7 + (c.y + offC.y) * 131 + 29) < wSharpG(oakWeather(fwp.x, fwp.z)); }
              if (!oakCull) {
              let roL0 = roP - ctr;
              let taC = (vec3<f32>(-0.708, -0.5, -0.708) - roL0) * inv;   // free conservative slab vs the flake's rotation bound (a spinning 1³ voxel about Y stays inside 0.708/0.5/0.708) — no trig yet
              let tbC = (vec3<f32>(0.708, 0.5, 0.708) - roL0) * inv;
              let tnC = min(taC, tbC); let tfC = max(taC, tbC);
              if (max(max(tnC.x, tnC.y), max(tnC.z, 0.3)) < min(min(min(tfC.x, tfC.y), tfC.z), maxNear)) {
              let spin = u.time * (2.0 + h1 * 2.5) + h1 * 47.0;      // each flake TWIRLS at its own rate and phase
              let cs = cos(spin); let sn = sin(spin);
              var rdL = vec3<f32>(rdS.x * cs - rdS.z * sn, rdS.y, rdS.x * sn + rdS.z * cs);   // ray → flake-local (rotate about Y by −spin)
              if (abs(rdL.x) < 1e-6) { rdL.x = 1e-6; } if (abs(rdL.z) < 1e-6) { rdL.z = 1e-6; }
              let invL = 1.0 / rdL;
              let roL = vec3<f32>(roL0.x * cs - roL0.z * sn, roL0.y, roL0.x * sn + roL0.z * cs);
              // ── IT FADES OUT, IT DOES NOT BLINK OUT ── a flake is an OPAQUE voxel, so the moment it fell past the
              // surface it lost the depth test and vanished between two frames; near the ground that reads as a flicker.
              // It keeps its FULL SIZE (user 2026-08-07: "do not shrink the snowflakes, just make them fade out") and
              // dissolves instead: over the last two voxels of the fall its colour is blended toward the ground directly
              // beneath it, which is what the pixel would show through a translucent flake anyway. ctr lives in roP space
              // and roP is ro + offR, so subtracting offR gives the window-local integers voxAt expects.
              var fkA = 1.0; var fkUnder = 0u;
              { let fb = vec3<i32>(floor(ctr - offR));
                let fy = fract(ctr.y - offR.y);
                let b1 = voxAt(vec3<i32>(fb.x, fb.y - 1, fb.z));
                if (b1 != 0u) { fkA = clamp(fy, 0.0, 1.0); fkUnder = b1; }
                else { let b2 = voxAt(vec3<i32>(fb.x, fb.y - 2, fb.z));
                  if (b2 != 0u) { fkA = clamp(0.5 + 0.5 * fy, 0.0, 1.0); fkUnder = b2; } } }
              let ta2 = (vec3<f32>(-0.5) - roL) * invL;              // FULL 10 cm voxel — size is never touched
              let tb2 = (vec3<f32>(0.5) - roL) * invL;
              let tn = min(ta2, tb2); let tf = max(ta2, tb2);
              let te = max(max(tn.x, tn.y), max(tn.z, 0.3));
              let tl = min(min(tf.x, tf.y), tf.z);
              // ── THE DITHER KEY IS THE FLAKE'S IDENTITY, NOT ITS CURRENT ADDRESS (user 2026-08-16: "the snow
              // seems to be flickering") ── this test thins the snowfall out across the desert blend band. It used to
              // key on fract(sin(floor(fwp.x) ... )) — a hash of the flake's WORLD POSITION. fwp is the lattice minus
              // the integrated fall/wind, so the world slides under a flake every frame: measured 15.7 vox/s of
              // horizontal drift, which steps floor(fwp.x)/floor(fwp.z) about 16 times a second. Each step was a fresh
              // uniform draw against desertMask, so every flake in the band flipped visible/culled ~8 times a second
              // (2*dm*(1-dm)*R at dm 0.5) and the whole snowfield sizzled. The lattice CELL is what identifies a flake
              // and it does not move with the wind, so hashing that gives each flake ONE key for its whole life: the
              // value it is compared against still slides, because the flake really is drifting into the sand, but only
              // across the 450-voxel band — so a flake crosses its threshold once and fades out instead of blinking.
              // Same dHash and the same salt family the ground blanket dithers on (landSnowAt in tick-snow.js); the two
              // are independent draws rather than one shared value, since a flake's lattice cell is not its landing
              // column, and it is the shared dm ramp - not a shared number - that keeps the two boundaries together.
              if (te > 1.0 && te < tl && te < maxNear && dHash((c.x + offC.x) * 5 + 17, (c.z + offC.z) * 7 + (c.y + offC.y) * 131 + 29) >= wSharpG(desertMask(fwp.x, fwp.z)) && !flakeBlocked(ctr - offR)) {   // ── NO SNOW OVER THE DESERT (user) ── ordered deliberately: after the cheap slab/rotation rejects, before flakeBlocked's scattered loads, so it only costs anything on a ray that already struck a flake   // te > 1: a flake that reaches the eye "hits your face" is culled; the open-air test runs LAST, only on a real strike. ctr is window-local now — minus the small remainder = the window position (exact at any world coordinate)
                fte = te; fkAw = fkA; fkUw = fkUnder;
                var nl = vec3<f32>(0.0);
                if (tn.x >= tn.y && tn.x >= tn.z) { nl.x = -sign(rdL.x); }
                else if (tn.y >= tn.z) { nl.y = -sign(rdL.y); }
                else { nl.z = -sign(rdL.z); }
                fnn = vec3<f32>(nl.x * cs + nl.z * sn, nl.y, -nl.x * sn + nl.z * cs);   // local normal → world (rotate back)
                if (abs(fnn.y) >= abs(fnn.x) && abs(fnn.y) >= abs(fnn.z)) { fface = select(2u, 3u, fnn.y < 0.0); }
                else if (abs(fnn.x) >= abs(fnn.z)) { fface = select(0u, 1u, fnn.x < 0.0); }
                else { fface = select(4u, 5u, fnn.z < 0.0); }
                break;
              }
              }
              }
              }
            }
            let tNext = min(tMax.x, min(tMax.y, tMax.z));
            if (tNext > maxNear) { break; }
            if (tMax.x <= tMax.y && tMax.x <= tMax.z) { tMax.x += tDel.x; c.x += stp.x; }
            else if (tMax.y <= tMax.z) { tMax.y += tDel.y; c.y += stp.y; }
            else { tMax.z += tDel.z; c.z += stp.z; }
          }
        }
        if (${RAIN_ON ? 'oakNear' : 'false'}) {
          // ── RAIN ── the oak forest's half of the same storm (user: "instead of snow in the oak forest, make it
          // rain … make the voxels that fall a light blue instead of white, and make them fall down much faster").
          // Same mechanics as the flakes by design: a DDA over a world-space 3-voxel lattice, ~2.45% of cells
          // occupied, a REAL primary hit so grain, the sun ray, AO, fog and the whole denoiser chain treat a drop
          // exactly like the static world. It runs off the SAME storm flag (u.fx bit 4), so one weather event
          // rains here and snows in the pines at the same moment, which is the whole point.
          //
          // WHY THIS IS A SECOND MARCH AND NOT A FLAG INSIDE THE FIRST ONE. The flake lattice does not move: what
          // moves is the RAY, via offR, and the lattice re-indexes by a whole cell (offC) every time offR wraps 3
          // so the slide is continuous and exact. That construction has room for exactly ONE fall speed — a second
          // speed is a second offset, i.e. a second lattice, whose cell boundaries sit somewhere else. Sharing one
          // DDA between them was worked through and costs either two candidate cells per step (drops straddle the
          // other lattice's boundaries) or a fixed within-cell phase that bands the rain into horizontal layers.
          // A separate march in its OWN lattice space is exact, leaves the tuned snow march untouched, and the
          // oakNear gate above means the pine forest and the desert never execute a line of it.
          let fallR = vec3<f32>(-u.rdist.z + sin(u.time * 0.6) * 0.8, u.pickY.w * RAIN_MULT, -u.rdist.w + cos(u.time * 0.5) * 0.8);   // the SAME horizontal wind as the flakes, RAIN_MULT x the fall: identical sideways drift over 1/5 the time is a five-times STEEPER streak, which is exactly how rain differs from snow in the same gust
          let off = winOf + fallR;                                   // same exact-integer-cell + small-remainder split the flake march uses — see the precision note there; without it a far spawn loses the drops in view-dependent sectors
          let offC = vec3<i32>(floor(off * (1.0 / 3.0)));
          let offR = off - vec3<f32>(offC) * 3.0;
          let roP = ro + offR;
          let maxNear = min(min(min(select(1e9, h.t, h.t >= 0.0), u.rdist.x), 150.0), fte);   // …and never past a flake already found: in the blend band both weathers exist and the nearer one wins
          var c = vec3<i32>(floor((roP + rd * 0.3) * (1.0 / 3.0)));
          let stp = vec3<i32>(i32(sign(rdS.x)), i32(sign(rdS.y)), i32(sign(rdS.z)));
          let tDel = abs(inv) * 3.0;
          var tMax = ((vec3<f32>(c) + max(vec3<f32>(stp), vec3<f32>(0.0))) * 3.0 - roP) * inv;
          for (var it = 0; it < 96; it++) {
            let h1 = ih3(c.x + offC.x, (c.y + offC.y) * 7, c.z + offC.z);
            if (h1 > RAIN_THR) {
              // The y jitter is DELIBERATELY narrow (0.35..0.65 of the cell against the flakes' 0.17..0.83): with a
              // half-height of RAIN_HH the drop then still ends inside its own 3-voxel cell, which is what makes
              // this DDA exact — a drop poking through a cell face would be sliced by any ray that clips that face
              // without entering the next cell. The range is derived from RAIN_HH so the two extremes touch the
              // cell faces EXACTLY at any drop height, i.e. the drop population covers the whole
              // cell height with nothing left over: no gap between one course of drops and the next to read as
              // horizontal banding, and no overhang for a grazing ray to clip.
              let ctr = vec3<f32>((f32(c.x) + 0.17 + 0.66 * fract(h1 * 43758.5)) * 3.0,
                                  (f32(c.y) + RAIN_HH / 3.0 + (1.0 - RAIN_HH * (2.0 / 3.0)) * fract(h1 * 12345.7)) * 3.0,   // = 0.35 + 0.30 * r at RAIN_HH 1.05; derived from RAIN_HH rather than written out so the in-cell invariant survives anyone retuning the drop
                                  (f32(c.z) + 0.17 + 0.66 * fract(h1 * 7777.3)) * 3.0);
              let cwy = ctr.y - offR.y;                              // storm-band pre-check, same as the flakes: the drops arrive and leave with the same front
              if (cwy >= u.misc.y && cwy <= u.misc.z) {
              // ONE exact slab test, no conservative bound and no trig: a raindrop does not tumble, so the box is
              // axis-aligned in world space and IS its own bound. That is what pays for the stretch.
              let roL = roP - ctr;
              let ta2 = (vec3<f32>(-RAIN_HW, -RAIN_HH, -RAIN_HW) - roL) * inv;
              let tb2 = (vec3<f32>(RAIN_HW, RAIN_HH, RAIN_HW) - roL) * inv;
              let tn = min(ta2, tb2); let tf = max(ta2, tb2);
              let te = max(max(tn.x, tn.y), max(tn.z, 0.3));
              let tl = min(min(tf.x, tf.y), tf.z);
              if (te > 1.0 && te < tl && te < maxNear) {
                let fwp = ctr - offR + winOf;                        // window-local -> world, for the biome test
                // ── IT ONLY RAINS IN THE OAK FOREST ── the exact mirror of the snow cull, and the reason the border
                // reads as weather rather than as a line. Keyed on the lattice CELL, not on the drop's world
                // position: fwp slides under the drop as the wind carries it, so a position hash re-draws several
                // times a second and the whole field sizzles (the 2026-08-16 desert-flicker bug — read the note at
                // the desert test above, it is the same trap). Its own salt, so the rain dither and the snow dither
                // are independent draws over the same ramp: across the band each cell rolls once for its whole
                // life, so a column fades from one weather to the other instead of flickering between them.
                if (dHash((c.x + offC.x) * 11 + 5, (c.z + offC.z) * 13 + (c.y + offC.y) * 197 + 41) < wSharpG(oakWeather(fwp.x, fwp.z)) && !flakeBlocked(ctr - offR)) {   // flakeBlocked LAST — ~38 scattered loads, and only a ray that genuinely struck a drop ever pays for them
                  // The drop DISSOLVES into what it is about to land on rather than blinking out of the depth test —
                  // the flakes' own fix, and it is what stands in for a splash: nothing lands, so the fade IS the
                  // landing. It runs over TWO voxels rather than the flakes' one because a drop is RAIN_HH tall
                  // and a flake is half a voxel: a one-voxel fade would still be near full opacity at the moment
                  // the bottom of the streak entered the ground. Reaching zero exactly as the centre meets the
                  // surface is what keeps the buried tail invisible.
                  var rkA = 1.0; var rkU = 0u;
                  { let fb = vec3<i32>(floor(ctr - offR));
                    let fy = fract(ctr.y - offR.y);
                    let b1 = voxAt(vec3<i32>(fb.x, fb.y - 1, fb.z));
                    if (b1 != 0u) { rkA = clamp(fy * 0.5, 0.0, 1.0); rkU = b1; }
                    else { let b2 = voxAt(vec3<i32>(fb.x, fb.y - 2, fb.z));
                      if (b2 != 0u) { rkA = clamp(0.5 + 0.5 * fy, 0.0, 1.0); rkU = b2; } } }
                  fte = te; fkAw = rkA; fkUw = rkU; fIsRain = true;
                  var nl = vec3<f32>(0.0);
                  if (tn.x >= tn.y && tn.x >= tn.z) { nl.x = -sign(rdS.x); }
                  else if (tn.y >= tn.z) { nl.y = -sign(rdS.y); }
                  else { nl.z = -sign(rdS.z); }
                  fnn = nl;                                          // already world-space — nothing to rotate back
                  if (abs(fnn.y) >= abs(fnn.x) && abs(fnn.y) >= abs(fnn.z)) { fface = select(2u, 3u, fnn.y < 0.0); }
                  else if (abs(fnn.x) >= abs(fnn.z)) { fface = select(0u, 1u, fnn.x < 0.0); }
                  else { fface = select(4u, 5u, fnn.z < 0.0); }
                  break;
                }
              }
              }
            }
            let tNext = min(tMax.x, min(tMax.y, tMax.z));
            if (tNext > maxNear) { break; }
            if (tMax.x <= tMax.y && tMax.x <= tMax.z) { tMax.x += tDel.x; c.x += stp.x; }
            else if (tMax.y <= tMax.z) { tMax.y += tDel.y; c.y += stp.y; }
            else { tMax.z += tDel.z; c.z += stp.z; }
          }
        }
        if (fte < 8e8 && (h.t < 0.0 || fte < h.t)) {                 // a flake or a drop beat the world hit — it IS that voxel from here on
          h.t = fte; h.vox = select(SNV, RNV, fIsRain); h.n = u.sunDir; h.face = fface; flakeHit = true; flakeFade = fkAw; flakeUnder = fkUw;   // SUN-FACING normal (user: "the snowflakes split into 2 chunks"): per-face cube shading tinted anti-sunward flakes grey vs white sunward ones — a visible tonal divide. A flake SCATTERS, so its lighting must not depend on face orientation; n = sunDir makes every flake's dot(n,sun) = 1 → one uniform field. (fface keeps the real face for grain.)
        }
      }
      // ── DYNAMIC LIFE ── every creature is a rigid voxel MODEL evaluated right here in the primary trace (the proven
      // snow-flake pattern, generalized): a model-space DDA with the slot's continuous sub-voxel transform. A nearer
      // creature OVERRIDES the hit, so the sun ray, the AO ray, the glow field, fog, water and the whole SVGF chain
      // treat it exactly like terrain — real contact AO and accumulated GI, no analytic stand-ins. Nothing is ever
      // written into any world grid: terrain corruption, stamp clearing and creature-overlap conflicts are impossible
      // by construction, and motion stays continuous (no grid snapping). Fireflies/sparks/dropped items keep the
      // composite path (emissive + translucency), as does any creature seen THROUGH a water surface (Beer–Lambert).
      var cSlot = 0u; var cCell = vec3<f32>(0.0); var cVc = vec3<i32>(0); var cAxis = 0u; var cN = vec3<f32>(0.0, 1.0, 0.0);
      var bHit = false; var bCol = vec3<f32>(0.0); var bN = vec3<f32>(0.0, 1.0, 0.0); var bVc = vec3<i32>(0); var bVox = 0u;   // rigid-body (felled chunk) hit   // …and its PALETTE ID, which bCol has already thrown away: h.vox is set to 0 for a body hit (see below), so the stone test for the sun sheen has nothing else to ask. A chopped-out boulder chunk is stone and must catch the sun exactly like the boulder it came off.
      var bestT = select(1e9, h.t, h.t >= 0.0);                      // nearest hit so far (world / flake) — SHARED by the creature loop and the rigid-body trace below, which is why it lives outside the dynamic-life gate
      if (ITEMN > 0 && u.lifeCfg.y > 0.5) {
        let ndc3 = (px / u.res) * 2.0 - 1.0;
        let dc3 = normalize(vec3<f32>(ndc3.x * u.tanH * u.aspect, -ndc3.y * u.tanH, 1.0));   // camera-space twin of rd — the drop transforms live in camera space
        let dropN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);
        // -- WHERE THE PARTICLE BAND REALLY ENDS (user 2026-08-24: "the bees seem to only have their wings
        // rendered, and nothing else ... I dont see any other forms of life at all") -- this loop used to skip
        // slots 5..24 as a LITERAL and treat 25+ as creatures. JS does not lay them out that way: the particle
        // band grows with the live spark pool and main/tick-life.js puts the flock and the creatures at
        // lifeSlotBase = 9 + sN, which it publishes in lifeCfg.w for exactly this reason. COMPOSITE was already
        // reading it; TRACE was not, and that split is the whole bug report:
        //   oak / pine  pollen and cherry petals fill the pool, base = 37, creatures sit ABOVE 24 and draw.
        //   BIRCH       neither exists here, base = 9, so every creature lands INSIDE 5..24 and this loop
        //               stepped straight over it. The body is trace-injected and vanished; the WINGS are a
        //               composite overlay and kept drawing, which is the bee with nothing but wings.
        // It also explains the rest of the report by construction: the flying flock sits at the same base, so
        // it disappeared with them; life reappears at the pine edge and over water because those carry enough
        // particles to push the base past 24; and the ducks FLICKER because the pool size oscillates around
        // that boundary, taking every creature in and out of the traced band with it.
        let lifeBase = i32(u.lifeCfg.w + 0.5);   // the ONE number, from the ONE writer - see main/tick-emit.js

        let tiV = (wgid.y * ((u32(u.res.x) + 7u) / 8u) + wgid.x) * ${VIS_W}u;   // this workgroup IS one 8×8 tile — read its four prepass mask words (under ?uni the stride is 8: words 0-3 primary, 4-7 the grown SECONDARY mask)
        let visM0 = visb[tiV]; let visM1 = visb[tiV + 1u]; let visM2 = visb[tiV + 2u]; let visM3 = visb[tiV + 3u];   // FOUR words now (128 slots) and all four stay in REGISTERS: re-fetching the word from storage per iteration measured 4× the per-slot cost
        for (var di = 4; di < dropN; di++) {                         // 4 = flying cardinal, lifeBase+ = live creatures; 0-3 drops and the spark/particle band between stay analytic
          if (di >= 5 && di < lifeBase) { di = lifeBase - 1; continue; }   // the sparks and the particle pool are analytic-only — step OVER the whole band, don't walk it. Bounded by lifeBase, never by a literal that goes stale the moment the pool changes size.
          { let mw = select(select(visM0, visM1, di >= 32), select(visM2, visM3, di >= 96), di >= 64); let mrem = mw >> (u32(di) & 31u); if (mrem == 0u) { di = i32(u32(di) | 31u); continue; } if ((mrem & 1u) == 0u) { di += i32(countTrailingZeros(mrem)) - 1; continue; } }   // ── TILE CULL, BIT-SCANNED ── the mask word says which slots can touch this 8×8 tile; jump straight to the next SET bit (or over an empty word entirely) instead of paying one loop iteration per slot. The iteration was the whole cost — an empty slot cost the same as a live one — so this is what makes the array size stop mattering. NOTE: it mutates di; a later edit that assumes di advances by one, or any drift between dropN and the mask, makes creatures vanish silently.
          if ((u32(lifeMotV(di).w + 0.5) & 1u) != 0u) { continue; } // analytic-only (firefly / empty)
          let dXv = dropV(di * 4 + 1);
          let dit = i32(dXv.w + 0.5);
          if (dit < 1) { continue; }
          let dA = dropV(di * 4); let dYv = dropV(di * 4 + 2); let dZv = dropV(di * 4 + 3);
          let it3 = clamp(dit - 1, 0, ITEMN - 1);
          let eW = ITEMD[it3].x; let eD = ITEMD[it3].y; let eH = ITEMD[it3].z; let eOff = ITEMD[it3].w;
          if (eW < 1) { continue; }
          let vsD = dA.w;
          let ew2 = f32(eW) * 0.5; let ed2 = f32(eD) * 0.5; let eh2 = f32(eH) * 0.5;
          let radD = vsD * (sqrt(ew2 * ew2 + ed2 * ed2 + eh2 * eh2) + 1.0);
          let tcD = dot(dA.xyz, dc3);
          if (tcD <= 0.0 || tcD - radD > bestT || length(dc3 * tcD - dA.xyz) > radD) { continue; }
          let roD = vec3<f32>(-dot(dA.xyz, dXv.xyz), -dot(dA.xyz, dYv.xyz), -dot(dA.xyz, dZv.xyz)) / vsD + vec3<f32>(ew2, ed2, eh2);
          var rdD = vec3<f32>(dot(dc3, dXv.xyz), dot(dc3, dYv.xyz), dot(dc3, dZv.xyz));
          if (abs(rdD.x) < 1e-6) { rdD.x = 1e-6; }
          if (abs(rdD.y) < 1e-6) { rdD.y = 1e-6; }
          if (abs(rdD.z) < 1e-6) { rdD.z = 1e-6; }
          let invD = 1.0 / rdD;
          let taD = -roD * invD;
          let tbD = (vec3<f32>(f32(eW), f32(eD), f32(eH)) - roD) * invD;
          let tnD = min(taD, tbD); let tfD = max(taD, tbD);
          let teD = max(max(tnD.x, tnD.y), max(tnD.z, 0.0));
          let tlD = min(min(tfD.x, tfD.y), tfD.z);
          if (teD >= tlD) { continue; }
          var vaxD = 0;
          if (tnD.y == teD) { vaxD = 1; } if (tnD.z == teD) { vaxD = 2; }
          var vcD = clamp(vec3<i32>(floor(roD + rdD * (teD + 1e-4))), vec3<i32>(0), vec3<i32>(eW - 1, eD - 1, eH - 1));
          let istD = vec3<i32>(sign(rdD));
          var vNxD = (vec3<f32>(vcD + max(istD, vec3<i32>(0))) - roD) * invD;
          var tHit = teD;
          var iMapD = eOff + vcD.x + vcD.y * eW + vcD.z * eW * eD;
          for (var i = 0; i < PICKSTEPS; i++) {
            let cell = ITEMMAP[u32(iMapD)];
            if (cell.w > 0.99) {                                     // OPAQUE only. ITEMMAP.w is per-voxel ALPHA, and a translucent voxel (the fly's wings) is deliberately INVISIBLE to the primary ray: the ray carries on to whatever is behind it, that surface lands in the g-buffer with its own normal/slot/motion, and the COMPOSITE blends the wing over the finished pixel. Blending here is not possible - this pass resolves ONE surface and its lighting - and dithering the wing away on a hash was the alternative, which the denoiser's history rejection turns into a crawling sparkle on a moving animal.
              if (tHit * vsD < bestT) {
                bestT = tHit * vsD;
                cSlot = u32(di + 1);
                cCell = cell.rgb; cVc = vcD;
                var nl = vec3<f32>(0.0);
                if (vaxD == 0) { nl.x = -f32(istD.x); } else if (vaxD == 1) { nl.y = -f32(istD.y); } else { nl.z = -f32(istD.z); }
                let nc = dXv.xyz * nl.x + dYv.xyz * nl.y + dZv.xyz * nl.z;
                cN = normalize(u.right * nc.x + u.up * nc.y + u.fwd * nc.z);   // TRUE rotated world normal — drives the sun ray + AO hemisphere (and the composite, via the axis bits)
                let sgn = select(0u, 1u, (vaxD == 0 && nl.x > 0.0) || (vaxD == 1 && nl.y > 0.0) || (vaxD == 2 && nl.z > 0.0));
                cAxis = u32(vaxD) * 2u + sgn;
              }
              break;
            }
            if (vNxD.x <= vNxD.y && vNxD.x <= vNxD.z) { tHit = vNxD.x; vNxD.x += abs(invD.x); vcD.x += istD.x; iMapD += istD.x; vaxD = 0; }
            else if (vNxD.y <= vNxD.z) { tHit = vNxD.y; vNxD.y += abs(invD.y); vcD.y += istD.y; iMapD += istD.y * eW; vaxD = 1; }
            else { tHit = vNxD.z; vNxD.z += abs(invD.z); vcD.z += istD.z; iMapD += istD.z * eW * eD; vaxD = 2; }
            if (any(vcD < vec3<i32>(0)) || any(vcD >= vec3<i32>(eW, eD, eH))) { break; }
          }
        }
      }                                                              // …end of the DYNAMIC-LIFE gate. The rigid-body trace below is deliberately OUTSIDE it.
      {                                                              // ── RIGID BODIES ── same traversal the shadow/AO rays use, so what you see and what the light sees can never disagree.
        // NOT gated on dynamic life (u.lifeCfg.y / ITEMN): a felled chunk has nothing to do with creature
        // trace-injection, and the secondary rays call traceAll unconditionally. While this sat inside that
        // gate, ?oldlife (or a failed .vox fetch leaving ITEMN == 0) made a chunk INVISIBLE to the camera while
        // it still cast a shadow, occluded AO and appeared in the water reflection — the exact disagreement the
        // line above says is impossible. Free when there is nothing to hit: bodyTrace returns on one compare
        // while physC.x is 0, which is every frame no tree has been felled.
        let bh2 = bodyTrace(ro, rd, bestT);
        if (bh2.t >= 0.0) { bestT = bh2.t; bHit = true; bCol = pal[bh2.vox].rgb; bVox = bh2.vox; bN = bh2.n;
          bVc = bh2.vc; }                                          // BODY-LOCAL cell (see Hit.vc): the grain rides with the wood instead of the trunk sliding through a world-anchored noise field
      }
      if (bHit) {                                                    // a felled chunk is the primary hit — terrain-identical shading from here on
        h.t = bestT; h.vox = 0u; h.n = bN;
        cSlot = 0u; cAxis = 0u;                                      // …and this pixel is NOT a creature any more, however the creature loop above left it. bodyTrace ran with maxT = bestT, so a chunk only wins by being strictly NEARER — but cSlot survived, and slotOut then told COMPOSITE to rebuild the normal from that animal's model axes and told DENOISE/TAA to reproject the pixel by the animal's motion. Fell a pine across a bird and the trunk pixels were lit with the bird's normal and smeared temporally.
        if (abs(bN.y) >= abs(bN.x) && abs(bN.y) >= abs(bN.z)) { h.face = select(2u, 3u, bN.y < 0.0); }
        else if (abs(bN.x) >= abs(bN.z)) { h.face = select(0u, 1u, bN.x < 0.0); }
        else { h.face = select(4u, 5u, bN.z < 0.0); }
      } else if (cSlot != 0u) {                                      // the creature IS the primary hit from here on
        h.t = bestT; h.vox = 0u; h.n = cN;                           // vox 0 → the water/lava id checks below can't misfire
        if (abs(cN.y) >= abs(cN.x) && abs(cN.y) >= abs(cN.z)) { h.face = select(2u, 3u, cN.y < 0.0); }
        else if (abs(cN.x) >= abs(cN.z)) { h.face = select(0u, 1u, cN.x < 0.0); }
        else { h.face = select(4u, 5u, cN.z < 0.0); }                // nearest-axis face: the denoiser's edge tests + a composite fallback; true shading normal comes from the axis bits
      }
      var albedo = vec3<f32>(0.0);
      var isRock = false;                                             // ── IS THIS PIXEL STONE ── rides out to slotOut bit 12 for the composite's sun sheen. Resolved inside the albedo branch below, not at the store, because that is the only place that knows WHICH id this pixel's colour came from: h.vox is authoritative for the static world only, a body hit carries its id in bVox, and a creature's h.vox is deliberately zeroed.
      var faceId = 7u; var t = -1.0;
      var hurtGlow = 0.0;                                            // >0 on a pixel inside the hit flash: it is emissive, so it must not be left to whatever light happens to reach it
      var sunV = 0.0; var skyV = 0.0;   // (bit 15 zeroes skyV at the end of the lighting block — the ambient/sky term, as opposed to the AO ray that modulates it)
      var creReact = 0.0;                                            // set inside the creature-shadow loop below: this pixel sits in a MOVING shadow's penumbra, so the reactive mask must cap its history (declared out here — the reactive mask is written well past the shading block's scope)
      if (h.t >= 0.0) {
        t = h.t; faceId = h.face;
        if ((h.vox == WTv || h.vox == WBv) && h.face == 2u) { faceId = 6u; }   // water top → reflective shading in the composite
        if (h.vox == LVT || h.vox == LVB || h.vox == LVR || h.vox == LVY) { faceId = 8u; }   // lava → emissive
        if (h.face == 2u && isSandV(h.vox)) { faceId = SANDF; }   // sand TOP face → the composite's sun glisten (see SANDF in PRE). Top only: a glinting vertical dune face or pit wall would read as wrong.
        let pos = ro + rd * t;
        let vcW = vec3<i32>(floor(pos - h.n * 0.01)) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));   // WORLD coords — grain must not swim when the window shifts
        if (bHit) { albedo = bCol * select(1.0, 0.88 + 0.24 * ih3(bVc.x, bVc.y, bVc.z), LG(9u)); isRock = isRockV(bVox); }   // felled chunk: palette colour + genuinely MODEL-LOCAL grain, at the SAME +/-12% amplitude static terrain uses, so a fallen trunk reads exactly like a standing one
        else if (cSlot != 0u) { albedo = cCell * select(1.0, 0.95 + 0.10 * ih3(cVc.x, cVc.y, cVc.z), LG(11u)); }   // creature: its cell color (baked self-AO included) + MODEL-LOCAL grain — GENTLE (±5%) so a model's authored colour transitions dominate the random per-voxel noise (user: adjacent whites read very differently); stable as it moves/rotates, matches the analytic path
        // ── AND THE GRAIN FADES OUT WITH DISTANCE (user 2026-08-20) ── the +/-12% per-voxel jitter is what makes
        // stone and bark read as MATERIAL up close. Past a few dozen voxels a voxel is smaller than a pixel, so
        // the same jitter stops being texture and becomes per-pixel noise — which is the speckled carpet the
        // mid-distance canopy turns into, and it is noise TAA then has to spend its history fighting. Faded
        // toward 1.0 (the plain palette colour) over 80..260 voxels: near-field material is untouched, the
        // treeline resolves into trees, and it costs one smoothstep on a value the branch already has.
        // It rides the same LG(10u) switch, so the debug bit still turns the whole thing off in one place.
        else { let gk = 1.0 - smoothstep(GRAIN_NEAR, GRAIN_FAR, t);
               albedo = pal[h.vox].rgb * select(1.0, mix(1.0, 0.88 + 0.24 * ivhash(vcW), gk), LG(10u)); isRock = isRockV(h.vox); }
        if (u.hurtB.w > 0.0) {                                       // ── HIT FLASH ── the animal just hit, blinking red (user)
          // WHOSE pixel is this? A trace-injected creature carries its dynamic-life slot in cSlot, so the
          // wounded animal is identified exactly, pixel for pixel, however it moves. The old box could
          // never do that: a worm renders OFF-GRID, so it had no stamped bounds and fell back to a
          // generous cube that slid around independently of the animal inside it (user).
          let isMe = (cSlot != 0u && cSlot == u32(u.hurtH.w + 0.5));
          // Grid-stamped animals (mammals, perched birds) are ordinary world voxels with no cSlot — but
          // hurtBox hands over their exact stamped bounds, so testing against those hugs the animal.
          let dHit = abs(pos - u.hurtB.xyz) / max(u.hurtH.xyz, vec3<f32>(0.001));
          // …and the voxel has to BE the animal: pal[].a carries the grid-stamped-creature flag (CREA_FLAG).
          // The box alone is an AABB, so it also holds the grass between the animal's legs and the ground
          // under its belly — painting those was the red square on the terrain (user). With the flag the
          // grid-stamped test is as exact as the trace-injected one: creature voxels only, terrain never.
          // …and a RAGDOLL is neither: on the killing blow the animal becomes a rigid BODY (bHit), which
          // carries no palette flag and — because the bHit branch above now clears it — no cSlot either
          // (it used to keep whatever slot the creature loop had just set). hurtBox tracks that body's live centre while it falls, so
          // testing a body pixel against the box is what keeps it red the whole way down (user 2026-08-05:
          // "red and rigid at the same time"). Only bodies INSIDE the box are touched, and the box is the
          // dead animal's own radius, so an ordinary chopped chunk lying elsewhere is never painted.
          if (isMe || (u.hurtH.w < 0.5 && bHit && all(dHit <= vec3<f32>(1.0)))
                   || (u.hurtH.w < 0.5 && !bHit && pal[h.vox].a > 0.5 && all(dHit <= vec3<f32>(1.0)))) {   // the WHOLE stamp, not an ellipsoid inside it (user)
            // NO SLACK (user: "the land mammals cast a red square on the terrain"). The box used to carry a
            // whole voxel of padding on every side, and everything in that shell — the ground under the
            // animal's feet, the grass beside it — was painted red too: a square of stained terrain around
            // the animal. hurtBox now hands over the animal's EXACT geometric bounds, so the only terrain
            // that can still be caught is the surface a neighbouring voxel shares with the animal's own
            // outer face, which the animal itself hides. Measuring the fade against those same bounds keeps
            // every voxel of the creature at mTrue <= 1, i.e. fully lit, with nothing left outside to fade.
            let hTrue = max(u.hurtH.xyz, vec3<f32>(0.001));
            let mTrue = max(abs(pos.x - u.hurtB.x) / hTrue.x, max(abs(pos.y - u.hurtB.y) / hTrue.y, abs(pos.z - u.hurtB.z) / hTrue.z));
            let fEdge = select(1.0 - smoothstep(1.0, 1.7, mTrue), 1.0, isMe);
            albedo = mix(albedo, HURT_RED, fEdge);    // DARKER (user): 9.0 saturated the red channel outright and tonemapped to a flat pale pink — this still reads as emissive but keeps its colour as RED
            hurtGlow = u.hurtB.w * fEdge;                            // …and it lights itself — see the sun/AO block
          }
          // Nothing else is touched here on purpose: the light this throws on its surroundings is the
          // point light the wound already puts in the fflies lane, which falls off smoothly. Painting a
          // second region by hand is what produced the hard-edged red square around it.
        }
        if (skipW && pos.y < WLF + 1.0 && LG(13u) && LG2(2u)) {      // swimming: caustic webs dance on everything below the surface. TWO gates: the old whole-scene caustics bit, and lgt.z bit 2, which is the water panel's caustics row — the panel has to be able to speak for all four caustic sites or the button is a lie. Moved out of a select(), which evaluated caust() either way.
          albedo = albedo * (1.0 + 1.3 * caust(floor(vec2<f32>(pos.x + u.winO.x, pos.z + u.winO.y)) + vec2<f32>(0.5)) * smoothstep(-0.02, 0.12, u.sunDir.y));
        }
        var foamK = 0.0;
        if (faceId == 6u && h.face == 2u && rd.y < -0.01 && u.pickZ.w < 0.4) {   // VOXEL OCEAN WAVES — still while the surface is more than half frozen
          let baseTop = pos.y;
          var tq = max(max(t - 3.8 / max(abs(rd.y), 0.08), t - 34.0), 0.0);
          let pW = ro + rd * tq;
          var cw = vec2<i32>(floor(vec2<f32>(pW.x, pW.z)));
          let sx2 = select(-1.0, 1.0, rd.x >= 0.0);
          let sz2 = select(-1.0, 1.0, rd.z >= 0.0);
          let adx = max(abs(rd.x), 1e-5); let adz = max(abs(rd.z), 1e-5);
          var tmx = ((f32(cw.x) + max(sx2, 0.0)) - pW.x) / (sx2 * adx) + tq;
          var tmz = ((f32(cw.y) + max(sz2, 0.0)) - pW.z) / (sz2 * adz) + tq;
          var wSide = false; var wCrest = -9.0; var whF = baseTop; var wRipF = 0.0;   // …wRipF = the ring foam at whichever column the march actually stops on
          for (var wi = 0; wi < 22; wi++) {
            let wxw = f32(cw.x) + u.winO.x; let wzw = f32(cw.y) + u.winO.y;
            var rip = vec2<f32>(0.0);
            if (LG2(4u)) { rip = ripHF(wxw, wzw); }                                                      // …plus any SPLASH or WAKE ring crossing this column (see ripHF in PRE; zero, and one compare, when nothing is rippling). lgt.z bit 4 is the panel's ripples row — off, the surface keeps its Gerstner swell and loses only the disturbances.
            let wv = gerstH(wxw, wzw) + rip.x;                                                           // GERSTNER height field (see PRE) — same sum the JS floater mirror rides
            // ── THE FOAM RING STANDS A VOXEL PROUD (user 2026-08-05) ── done HERE, in the surface march, not
            // after it. Lifting t once the hit was already found only moved that pixel's DEPTH: the foam
            // kept the silhouette of the flat water because the pixels it should have grown into were never
            // tested against the water at all. Raising the column's surface height BEFORE the intersection
            // test is what gives the band a real edge you can see standing above the swell.
            // 4 probes, not the shading pass's 8: this only decides WHICH columns are lifted, and the ±3 ring
            // is the same shoreline the foam itself is drawn on.
            var lift = 0.0;
            if (LG2(5u)) { let ciL = vec3<i32>(cw.x, i32(baseTop - 0.5), cw.y);   // lgt.z bit 5 — the panel's shore surf row. The LIFT goes with the foam it carries: leaving it on with the foam off would raise a proud band of ordinary water at every shoreline, which is a worse look than either state.
              for (var s3 = 0; s3 < 4; s3++) {
                var nb3 = ciL;
                if (s3 == 0) { nb3.x += 3; } else if (s3 == 1) { nb3.x -= 3; } else if (s3 == 2) { nb3.z += 3; } else { nb3.z -= 3; }
                let nv3 = voxAt(nb3);
                if (nv3 != 0u && nv3 != WTv && nv3 != WBv) { lift = 1.0; break; }
              } }
            let wh = baseTop + floor(wv + 0.5) + lift;
            let tNext = min(tmx, tmz);
            if (ro.y + rd.y * tq <= wh) { t = tq; wSide = true; wCrest = wv; whF = wh; wRipF = rip.y; break; }          // SIDE face of a wave step
            if (ro.y + rd.y * tNext <= wh) { t = (wh - ro.y) / rd.y; wCrest = wv; whF = wh; wRipF = rip.y; break; }     // TOP face within this column
            if (tmx < tmz) { tq = tmx; tmx += 1.0 / adx; cw.x += i32(sx2); }
            else { tq = tmz; tmz += 1.0 / adz; cw.y += i32(sz2); }
            if (tq > t + 38.0) { break; }
          }
          if (wCrest > -8.0) {
            let hp = ro + rd * t;
            let vcW2 = vec3<i32>(i32(floor(hp.x)), i32(whF - 0.5), i32(floor(hp.z))) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));
            albedo = pal[h.vox].rgb * (0.90 + 0.20 * ivhash(vcW2));                       // grain follows the WAVE voxel, not the flat plane
            if (wSide) { albedo *= 0.74; }                                                // darker step sides give the swell its silhouette
            var foam = 0.0;                                                                       // no mid-water whitecaps — foam only rings the shoreline
            let ci = vec3<i32>(i32(floor(hp.x)), i32(baseTop - 0.5), i32(floor(hp.z)));
            if (LG2(5u)) {                                                                // shore surf again — eight voxAt() fetches per water pixel, so the gate is a real saving as well as a switch
            for (var s2 = 0; s2 < 8; s2++) {                                              // probes at ±2 AND ±4 → a surf band twice as thick
              var nb = ci;
              let pr = select(2, 4, s2 >= 4);
              let a2 = s2 & 3;
              if (a2 == 0) { nb.x += pr; } else if (a2 == 1) { nb.x -= pr; } else if (a2 == 2) { nb.z += pr; } else { nb.z -= pr; }
              let nv = voxAt(nb);
              if (nv != 0u && nv != WTv && nv != WBv) {                                   // churned SURF ring wherever water meets land
                foam = max(foam, step(0.35, ivhash(ci + vec3<i32>(s2 * 13, 11, 5)) * (0.55 + 0.45 * sin(u.time * 1.6 + f32(ci.x * 7 + ci.z * 5) * 0.7))));
              }
            }
            }
            if (foam > 0.5) { let tFo = (whF + 2.0 - ro.y) / rd.y; if (tFo > 0.0) { t = min(t, tFo); } }   // …and the shaded surface rides on top of that raised column (the +1 now comes from the lift in the march above). GUARDED: this block only runs for rd.y < -0.01, so an eye BELOW that plane (whF is baseTop + floor(wv+0.5) + lift, i.e. up to ~6 voxels above the water — where the swim spring parks you) makes the quotient NEGATIVE and min() took it. A t behind the camera made TEMPORAL drop the pixel, COMPOSITE shade it as unlit water and the reflection ray start behind the eye: dark blotches trailing the shoreline foam ring while swimming. Below the plane there is nothing to clamp to — the ray is already under it.
            foam = max(foam, wRipF);                                                      // …and the crest of a SPLASH or WAKE ring is white for the same reason a whitecap is: it is water that has just been broken
            foamK = clamp(foam, 0.0, 1.0) * 0.8;
            albedo = mix(albedo, FOAM_C, foamK);
          } else {
            // ── AND IF THE MARCH RAN OUT, THE RING STILL HAS TO DRAW (user 2026-08-29: "I can't see the
            // splash rings and wakes at a distance") ── the loop above is capped at 22 column steps and it
            // starts up to 34 voxels SHORT of the hit, so on a grazing ray — which is every ray that reaches
            // far water — it gives up before it ever reaches this pixel's own column. wCrest stays at its
            // sentinel, the whole block above is skipped, and NOTHING on that surface is foamed: not a ring,
            // not a wake, not the shoreline surf. That is why the far half of a lake looked untouched while
            // a ring 40 voxels away was obvious.
            // Raising the cap would cost every water pixel in the frame a longer loop. Instead: when the
            // march fails, ask the ring field directly at the flat-plane hit. One evaluation, only on the
            // pixels that got nothing, and it is the same ripHF the march would have called.
            // Deliberately the RING only, not the shoreline probes — those are eight voxAt() fetches, and
            // foaming every distant shoreline is a change to the water's look that was not asked for.
            let hpF = ro + rd * t;
            var rfF = 0.0;
            if (LG2(4u)) { rfF = ripHF(hpF.x + u.winO.x, hpF.z + u.winO.y).y; }   // the same ripples bit — this is the SAME field the march would have sampled, so gating one and not the other would leave rings on far water only
            if (rfF > 0.0) {
              foamK = clamp(rfF, 0.0, 1.0) * 0.8;
              albedo = mix(albedo, FOAM_C, foamK);
            }
          }
        }
        // (the old 50%-translucent bed mix moved to the COMPOSITE: it now arrives via a REAL refracted ray with
        // Beer–Lambert absorption — the G-buffer albedo stays the pure surface color + foam.)
        var seed = ((gid.x * 1973u) ^ (gid.y * 9277u) ^ (u32(u.frame) * 26699u)) | 1u;
        let sp = pos + h.n * 0.02;
        if (${location.search.includes('nosun') ? 0 : 1} == 1 && (dot(h.n, u.sunDir) > 0.0 || (FOLBACK_URL && isFol(h.vox))) && u.sunDir.y > -0.04) {        // cone-jittered sun ray; skipped entirely at night (?nosun disables for A/B)
          let st = onbT(u.sunDir); let sb = cross(u.sunDir, st);
          let jitK = select(0.0, mix(0.028, 0.009, nightK()), LG(12u));                   // bit 12: sun PENUMBRA — off = a pin-sharp, perfectly hard shadow edge   // …and NIGHT BIT 0 tightens the cone toward the moon's own angular radius. 0.028 rad is a soft edge tuned for the SUN through a canopy; the same cone under a light a twentieth as bright gives a penumbra the denoiser cannot resolve, so a moon shadow arrived as a smudge. The mix rides nightK(), so noon is bit-identical.
          let sdir = normalize(u.sunDir + st * ((rand(&seed) * 2.0 - 1.0) * jitK) + sb * ((rand(&seed) * 2.0 - 1.0) * jitK));
          // -- BACK-LIT LEAF -- sp sits 0.02 along the NORMAL, which on a face the sun is BEHIND is the dark
          // side: the ray then walks straight back through the leaf's own voxel, hits it, and the pixel reads
          // as fully shadowed. sunV was therefore 0 on every pixel the transmission term targets, which is
          // exactly why it looked dead. Start on the sun side of the cell instead (1.25 voxels clears it), so
          // sunV answers the question transmission actually asks: does the sun reach the FAR face?
          // select() is false for every non-foliage surface here (the gate above only admits dot > 0 unless it
          // is a leaf), so ordinary shadows are bit-identical.
          // ══ AND THE BIAS SCALES WITH THE GRAZING ANGLE, OR A LOW SUN SPECKLES EVERY FLAT SURFACE ══ sp sits a
          // fixed 0.02 voxels along the normal. That clears the surface only for a ray leaving it steeply. As
          // the sun drops, dot(n, sunDir) on a flat top face goes to nothing and the jittered ray sets off
          // almost parallel to the ground — 0.02 of clearance is then not enough to get out of the CURRENT
          // cell, so the ray re-enters the voxel next door, reports occluded, and the pixel goes fully black.
          // sunV is binary here (select(1, 0, occ)), so there is no partial answer to soften it: neighbouring
          // pixels whose jitter tilts the other way come back fully lit, and the surface breaks into black
          // speckle. It follows the terrain's contours because that is where the one-voxel steps are, and it
          // CRAWLS, because the cone jitter is re-drawn every frame — which is the "random parts of terrain
          // flicker" this was reported as. Snow shows it worst: a black dot on white is maximum contrast.
          // The standard fix, and the reason it is not just a bigger constant: the clearance a ray needs is
          // proportional to 1/cos of its angle to the surface. Capped below one voxel so the offset can never
          // step the ray over a genuine one-voxel occluder and leak light through a thin wall.
          let ndl = max(dot(h.n, u.sunDir), 1e-3);
          let sunOrg = select(sp + h.n * clamp(0.03 / ndl, 0.03, 0.8), pos + u.sunDir * 1.25, dot(h.n, u.sunDir) <= 0.0);
          let ceilY = f32((u32(u.fx) >> 8u) & 31u) * 32.0;           // world ceiling (u.fx bits 8+): no solid above it → a climbing ray is clear once past it
          let sCap = select(1200.0, min(1200.0, (ceilY - sunOrg.y) / max(sdir.y, 1e-4)), sdir.y > 1e-4);
          if (sCap <= 0.0) { sunV = 1.0; }                           // already above everything and going up — full sun, no ray
          else {
          if (LG(0u)) { let shT = trace(sunOrg, sdir, sCap, skipW);   // ── OCCLUSION, NOT NEAREST HIT ── this ray only ever asks "is anything in the way".
                        var occ = shT.t >= 0.0;                            // traceAll walks the terrain AND then bodyTrace to find which of the two is CLOSER,
                        if (!occ) { let shB = bodyTraceX(sunOrg, sdir, sCap, true); occ = shB.t >= 0.0; }   // which this caller throws away. Once terrain has blocked the ray the answer
                        sunV = select(1.0, 0.0, occ); }                    // cannot change, so the body walk is pure waste. Bit-identical: occluded is occluded.
                        // (bodies included → a felled tree still casts a REAL shadow)
          else { sunV = 1.0; }                        // sun shadows OFF — every sunward surface fully lit
          }
          ${!(LIFE_UNI && (UNI_SEC & 1)) ? '' : 'if (sunV > 0.0) { let cs = creaSec(sp, sdir, SEC_R, sg0, sg1, sg2, sg3, secN); if (cs.x >= 0.0) { sunV = 0.0; creReact = max(creReact, cs.y); } }'}
          if (${UNI_CSHAD}) {   // CREATURE CAST SHADOWS — the sun ray tests the creature AABBs so they shadow ground/water. SKIPPED for a trace-injected creature pixel: its own box wraps the surface point and every sun ray would 'self-shadow' (cross-creature shadows are a lesser loss than a permanently dark body)
            for (var s = 0; s < 16; s++) {
              let CA = u.cshad[s * 2];
              if (CA.w < 0.5) { continue; }
              let d0 = sp - CA.xyz;                                  // both window-relative
              if (d0.x * d0.x + d0.z * d0.z > 1600.0) { continue; }  // a ground creature's shadow lands within a few metres
              let CB = u.cshad[s * 2 + 1];
              let he = vec3<f32>(CB.x, CB.y, CB.x);                  // half-extents (horizontal, vertical, horizontal) — axis-aligned box
              let inv2 = 1.0 / select(sdir, vec3<f32>(1e-5), abs(sdir) < vec3<f32>(1e-5));
              // GROWN box first. A pixel this ray only just misses is a pixel the shadow is about to
              // sweep over (or has just left), and it is exactly those pixels the 64-frame accumulator
              // smears. Flagging them reactive costs one extra slab test on the rare near-miss, and the
              // exact test below only runs when the grown one already hit — so the common case, a ray
              // nowhere near this creature, is unchanged.
              let heR = he * 1.6 + vec3<f32>(0.8);
              let taR = (CA.xyz - heR - sp) * inv2; let tbR = (CA.xyz + heR - sp) * inv2;
              let tnR = min(taR, tbR); let tfR = max(taR, tbR);
              if (max(max(tnR.x, tnR.y), tnR.z) >= min(min(tfR.x, tfR.y), tfR.z) || min(min(tfR.x, tfR.y), tfR.z) <= 0.05) { continue; }
              creReact = 1.0;                                        // moving shadow may touch this pixel → short history (see the reactive mask below)
              let ta2 = (CA.xyz - he - sp) * inv2; let tb2 = (CA.xyz + he - sp) * inv2;
              let tn2 = min(ta2, tb2); let tf2 = max(ta2, tb2);
              let teB = max(max(tn2.x, tn2.y), tn2.z);
              let tlB = min(min(tf2.x, tf2.y), tf2.z);
              if (teB < tlB && tlB > 0.05 && teB < 60.0) { sunV = 0.0; break; }   // the sun ray enters the creature box ahead → shadowed
            }
          }
        }
        if (${location.search.includes('noao') ? 0 : 1} == 1 && h.t < 500.0) {
          // Teardown AO: distance before collision ≈ indirect light (?noao disables for A/B). ONE ray per frame, not two — the profiler put the
          // white-noise 2-ray version at 4.3 ms (45% of the whole frame; incoherent directions thrash the DDA's L0 level
          // and diverge the warps). The temporal accumulator (maxHist 64) + the 12-tap spatial already average dozens of
          // frames, so the CONVERGED image is the sampler's mean either way — ray count only buys convergence speed. What
          // actually buys back the lost speed is stratification: an R2 low-discrepancy sequence over frames, IGN-offset
          // per pixel, covers the hemisphere evenly where rand() clumped, so 1 stratified ray converges about as fast as
          // the 2 white-noise rays it replaces. Same mean, ~half the trace cost.
          // ── AND BOTH LIGHTING TERMS STAY AT ONE RAY (user 2026-08-28: "theres still noise, especially when
          // looking at the ground") ── two attempts to buy that back with samples, both built, measured and
          // REVERTED the same day. The reasoning was sound and the arithmetic was right — at renderScale s only
          // s^2 of the pixels are traced, so 1/s^2 samples each costs what one per pixel costs at full res —
          // but neither survived measurement:
          //   AO rays scaled 1..4: cost 116 -> 88 fps at 0.6 and the user still saw the noise.
          //   SUN penumbra samples scaled the same way (the larger term — isolating it cut noise 59% against
          //     AO's 50%): 120 -> 78 fps and the noise metric got WORSE, 1.075 -> 1.265.
          // The honest comparison is the one that killed both: put them on the same noise-vs-fps curve as
          // simply raising the resolution slider, at a pinned time of day, and neither sits below it.
          // Whatever the player perceives as noise at low resolution, per-term sampling does not buy it more
          // cheaply than pixels do — so if this is revisited, START by finding a metric that actually tracks
          // the perception, because temporal frame-difference does not (it barely moves across 0.6/0.75/0.9).
          let fN = f32(u32(u.frame) & 1023u);
          let r1 = fract(ign(vec2<f32>(gid.xy)) + fN * 0.7548777);   // R2 sequence (plastic constants) — azimuth
          let r2 = fract(ign(vec2<f32>(gid.xy) + vec2<f32>(47.0, 17.0)) + fN * 0.5698403);   // decorrelated elevation lane
          let d = cosHemi(h.n, r1, r2);
          // ── AO RAY REACH ── physC.z, live-tunable via __vb.aoReach(n), default 24. It is BOTH the distance the
          // ray marches AND the value a miss returns, so the 0..1 range is preserved and only the span over
          // which occlusion is gathered changes. MEASURED at 24: AO is 2.17 ms — 45% of the trace and 27% of
          // the whole frame, the most expensive term in the renderer. Shorter = fewer DDA steps and a more
          // LOCAL occlusion: contact shadows survive, broad darkening under a canopy does not.
          // The select() fallback matters: a 0 here would make every surface fully lit, so any frame the lane
          // is not written still renders the default rather than a blown-out image.
          if (LG(1u)) { let aoR = select(24.0, u.physC.z, u.physC.z > 0.0);
                        let ah = traceAll(sp, d, aoR, skipW);      // bodies included → real contact AO and self-shadowing, no bake needed
                        var aT = select(aoR, ah.t, ah.t >= 0.0);
                        ${!(LIFE_UNI && (UNI_SEC & 2)) ? '' : '{ let cs2 = creaSec(sp, d, aT, sg0, sg1, sg2, sg3, secN); if (cs2.x >= 0.0) { aT = cs2.x; creReact = max(creReact, cs2.y); } }'}
                        skyV = clamp(aT / aoR, 0.0, 1.0); }
          else { skyV = 1.0; }                        // AO OFF — no contact darkening anywhere
        } else { skyV = 0.85; }                                      // past 50 m AO detail is sub-pixel — flat ambient, the ray saved on every far pixel
        if (flakeHit) {
          // ── A DROP IS NOT A FLAKE ── same idea, different numbers, and the split is REASONED rather than tuned
          // (this has not been A/B'd in the browser yet — these two lines are where to start if the rain reads
          // wrong). Snow is opaque and scatters, so it is white from every direction and takes a high floor on
          // BOTH terms; without that its anti-sunward faces shaded to sky luminance and vanished. A raindrop is a
          // lens instead: nearly everything you see in one is the sky behind it, which is what makes it read blue
          // at all. So its floor is almost entirely skyV and the sun term stays low — giving it snow's 0.6 sunV
          // would light the light blue toward white in open sun and undo the colour the whole change is about.
          // Both still scale with the sun/moon term downstream, so a night storm stays subtle.
          if (h.vox == RNV) { sunV = max(sunV, 0.25); skyV = max(skyV, 0.95); }
          else { sunV = max(sunV, 0.6); skyV = max(skyV, 0.85); }
          if (flakeFade < 1.0 && flakeUnder != 0u) { albedo = mix(pal[flakeUnder].rgb, albedo, flakeFade); }   // dissolving into what it is landing on
        }
        if (hurtGlow > 0.0) { sunV = max(sunV, hurtGlow * 0.55); skyV = max(skyV, hurtGlow * 0.75); }   // the flash glows in shadow too (user) — otherwise an animal hit under a canopy barely changed colour   // SOFTER (user): pinning both terms to FULL erased the creature's own sun/AO shading, so the whole animal went flat and blew out; lifting them part-way keeps its form readable while a wound in deep shade still reads

        if (!LG(15u)) { skyV = 0.0; }                              // bit 15: the SKY/AMBIENT term entirely — leaves only direct sun, so anything not in sunlight goes black   // FALLING FLAKES SCATTER — real snow is white from EVERY direction, so a flake never shows a dark shaded face. Without this floor, anti-sunward flakes shaded to ~sky luminance and vanished — the "snow band clipping" wedge the user saw (the void tracked the anti-sun azimuth, verified via the ?flakedbg emissive A/B). Scales with the sun/moon term downstream, so night stays subtle.
      }
      var lavaG = 0u;                                                  // soft light cast by nearby lava — deterministic probe, no flicker
      if (LG(3u)) {                                                    // LIGHT DEBUG bit 3: lava + firefly point lights
      // lava only exists at bedrock (y ≤ 8) and the probe reaches 18 down — pixels above y 28 can NEVER see glow, so 99.9% of pixels skip a whole ray
      if (t >= 0.0 && faceId != 8u && faceId != 7u && (ro.y + rd.y * t) < 28.0) {
        let pos2 = ro + rd * t + h.n * 0.02;
        let dh = trace(pos2, vec3<f32>(0.0, -1.0, 0.0), 18.0, true);
        if (dh.t >= 0.0 && (dh.vox == LVT || dh.vox == LVB || dh.vox == LVR || dh.vox == LVY)) {
          lavaG = u32(clamp((1.0 - dh.t / 18.0) * 14.0 + 0.5, 0.0, 14.0));
        }
      } else if (t >= 0.0 && faceId != 8u && faceId != 7u) {   // FIREFLY LIGHT — BAKED IN (user 2026-08-19: "bake in the firefly light and remove it from the panel"); it was night bit 4   // …NIGHT BIT 4 is the panel's switch for this and nothing else. LG bit 3 above already gates it, but that bit is shared with the LAVA probe, so a row labelled 'firefly light' driving it would have turned the bedrock glow off too. — same 4-bit glow field (lava lives below y 28, fireflies above: never both).
        let pos2 = ro + rd * t;                                        // Teardown-style AREA light (juandiegomontoya breakdown): jittered target on the glow sphere,
        var best = 0.0; var bi = -1;                                   // sphere-light falloff window (inner full → outer ZERO), temporal dither — TAA resolves the noise.
        for (var f = 0; f < 8; f++) {
          let F = u.fflies[f];
          if (F.w <= 0.0) { continue; }
          let dv = F.xyz - pos2;
          let d2 = dot(dv, dv);
          if (d2 > 484.0) { continue; }                                // 2.2 m reach
          let win = 1.0 - smoothstep(220.0, 484.0, d2);                // "outer radius where the intensity falls off to zero" — kills the hard circle edge
          let k = F.w * max(dot(h.n, normalize(dv)), 0.0) * win / (1.0 + d2 * 0.06);
          if (k > best) { best = k; bi = f; }
        }
        if (bi >= 0 && best > 0.015) {
          var s2 = ((gid.x * 2467u) ^ (gid.y * 8837u) ^ (u32(u.frame) * 15013u)) | 1u;
          let jit3 = vec3<f32>(rand(&s2), rand(&s2), rand(&s2)) * 1.8 - vec3<f32>(0.9);   // "a ray to a RANDOM POINT on the light's surface" — soft area-light penumbra via TAA
          let dv = u.fflies[bi].xyz + jit3 - (pos2 + h.n * 0.02);
          let dist = length(dv);
          let sh = traceAll(pos2 + h.n * 0.02, dv / max(dist, 1e-4), max(dist - 0.6, 0.0), true);
          if (sh.t < 0.0) {
            let ign = fract(52.9829189 * fract(0.06711056 * f32(gid.x) + 0.00583715 * f32(gid.y)) + f32(u32(u.frame) & 63u) * 0.618034);   // temporal IGN dither — TAA melts the 14 quantization steps into a smooth gradient
            lavaG = u32(clamp(sqrt(best) * 5.2 + ign, 0.0, 14.0));     // sqrt-encoded: extra levels in the dim tail, where banding rings were most visible
          }
        }
      }
      }                                                                // …end LIGHT DEBUG bit 3 (glow)
      textureStore(gAlbedo, vec2<i32>(gid.xy), vec4<f32>(sqrt(albedo), (f32(faceId) + f32(lavaG) * 16.0) / 255.0));
      // ── REACTIVE MASK ── the temporal pass blends 1/hist per frame with hist up to maxHist = 64, so a
      // pixel newly covered or uncovered by a MOVING shadow needs ~64 frames to catch up: the shadow
      // visibly lags and smears behind the body that casts it. Flag any pixel whose sun ray could have
      // interacted with a rigid body and let TEMPORAL cap that pixel's history. Cheap — it reuses the same
      // enclosing-sphere test bodyTrace already does. Teardown carries an equivalent reactive mask in its
      // motion G-buffer for exactly this reason.
      var reactive = select(0.0, creReact, LG(4u));
      if (LG(4u) && t >= 0.0 && u.physC.y > 0.004) {                           // only while something is actually moving (eased, not switched — see physC.y): a settled trunk must not pin every nearby pixel's history forever (see physC)
        let sp2 = ro + rd * t;
        let dc2 = u.physBound.xyz - sp2;
        let sdir2 = select(u.sunDir, vec3<f32>(0.0, 1.0, 0.0), u.sunDir.y <= 0.0);
        let tc2 = dot(dc2, sdir2);
        if (tc2 > -u.physBound.w && dot(dc2, dc2) - tc2 * tc2 < u.physBound.w * u.physBound.w) { reactive = max(reactive, u.physC.y); }
        // …and the AO ray, which the sun-cylinder test above misses entirely. AO reaches 24 voxels in
        // EVERY direction, so the contact darkening a moving trunk casts on the ground beside it was
        // still converging at maxHist and trailed the trunk by a good half second.
        let aoR = u.physBound.w + select(24.0, u.physC.z, u.physC.z > 0.0);   // tracks __vb.aoReach — a fixed 24 here would over- or under-cover the ray it exists to shadow
        if (dot(dc2, dc2) < aoR * aoR) { reactive = max(reactive, u.physC.y); }
      }
      textureStore(gIrr, vec2<i32>(gid.xy), vec4<f32>(sunV, skyV, t, reactive));
      textureStore(slotOut, vec2<i32>(gid.xy), vec4<u32>(cSlot | (cAxis << 8u) | (select(0u, 1u, t >= 0.0 && (isFol(h.vox) || isCactusV(h.vox))) << 11u) | (select(0u, 1u, isRock) << 12u), 0u, 0u, 0u));   // dynamic-life id + hit-axis bits — temporal identity/motion + composite true-normal reconstruction   // ...and bit 11 = IS THIS A LEAF. It rides here because the word had 21 unused bits and there is no spare g-buffer channel (gIrr is sun/sky/distance/history, gAlbedo is rgb + face); the composite cannot know a needle from bark otherwise. Every existing reader masks (& 255u, >> 8u & 7u), so this is invisible to them.   // …and bit 12 = IS THIS STONE, on the same argument and for the same reason (user 2026-08-16: a sun reflection on every rock). It needs all SIX faces, so a faceId could not carry it: gAlbedo.a has exactly six values left in its low nibble and spending every one of them on one material would be the last thing that channel ever did.
    }
  `;

  // strip scatter — copies a repacked 8-voxel X-strip from staging into its strided home in the world buffer
