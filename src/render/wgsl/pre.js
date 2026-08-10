  // ── WGSL ────────────────────────────────────────────────────────────────────
  const PRE_SRC = () => /* wgsl */`
    struct U {
      camPos : vec3<f32>, tanH : f32,
      right  : vec3<f32>, aspect : f32,
      up     : vec3<f32>, frame : f32,
      fwd    : vec3<f32>, time : f32,
      pPos   : vec3<f32>, pTanH : f32,
      pRight : vec3<f32>, pAspect : f32,
      pUp    : vec3<f32>, maxHist : f32,
      pFwd   : vec3<f32>, fx : f32,
      sunDir : vec3<f32>, reset : f32,
      res    : vec2<f32>, jit : vec2<f32>,
      pJit   : vec2<f32>, canvasRes : vec2<f32>,
      winO   : vec2<f32>, off : vec2<f32>,
      pickA  : vec4<f32>,                                            // held pick: anchor xyz (camera space) + voxel size
      pickX  : vec4<f32>, pickY : vec4<f32>, pickZ : vec4<f32>,      // item local axes in camera space; pickX.w = show flag
      rdist  : vec4<f32>,                                            // x = render distance in voxels (menu slider)
      drops  : array<vec4<f32>, 256>,                                // drop slots 0..63 (the other 64 are in dropsB at the end of this struct): 4 dropped items + cardinal (slot 4) + 20 death-burst sparks/smoke (5-24) + the drawn flock + trace-injected creatures × {anchor.xyz+voxelScale, X.xyz+itemId, Y.xyz+glow, Z.xyz} in camera space, voxel units
      pick2A : vec4<f32>,                                            // LEFT hand (dual-wield rock): anchor xyz (camera space) + voxel size
      pick2X : vec4<f32>, pick2Y : vec4<f32>, pick2Z : vec4<f32>,    // left-hand item axes; pick2X.w = show id (0 = hidden)
      fflies : array<vec4<f32>, 8>,                                  // glowing FIREFLY point lights: window-coord xyz + intensity (0 = empty slot); traced in TRACE, applied via the lavaG glow field
      cshad  : array<vec4<f32>, 32>,                                 // CREATURE SHADOW boxes (16 × 2 vec4): [center.xyz(window) + active] [halfXZ, halfY, 0, 0] — the sun ray tests these so moving lilies/ducks/worms CAST shadows on the ground/water like static voxels
      misc   : vec4<f32>,                                            // x = CINEMATIC vignette depth; y/z = snow storm LEADING/TRAILING edge world-y (flakes exist only between them); w = EYE-INSIDE-A-VOXEL fill: the packed sRGB of the voxel the camera is buried in, PLUS 1 so that 0 means 'not buried' (written in tick-camera, read by COMPOSITE). NOT spare — take this lane for something else and the whole screen becomes the buried-in-rock fill.
      lifeMot : array<vec4<f32>, 64>,                                // ── DYNAMIC LIFE ── per drop-slot rigid MOTION + flags: xyz = world-space delta this frame (anchorNow − anchorPrev, window units — origins cancel), w = flags bitfield: 1 = analytic-only (fireflies/drops/sparks/empty — never trace-injected), 2 = anim frame changed (reject irradiance history), 4 = slot occupant changed / spawned / teleported (reject history)
      lifeCfg : vec4<f32>,                                           // x = life debug view (0 off, 1 slot ids, 2 history confidence, 3 motion vectors, 4 raw AO), y = trace-injection enabled (0 under ?oldlife → full analytic fallback), z/w spare
      physB : array<vec4<f32>, 80>,                                  // ── RIGID BODIES ── 16 x 5 vec4, appended LAST so no existing offset moves:
                                                                     // [0] anchor.xyz (camera space, at the COM) + voxel scale · [1] local X axis in camera space + dimX
                                                                     // [2] local Y axis + dimY · [3] local Z axis + dimZ · [4] comLocal.xyz + buffer offset
      physC : vec4<f32>,                                             // x = ACTIVE body count (0 = the whole path is one compare), y = REACTIVE STRENGTH 0..1 (1 while a body is moving, eased to 0 over ~0.45 s after it stops — a binary here made a settling trunk pulse its own shadow noise on and off), z/w spare. Was: how many are AWAKE (the reactive mask keys off y; tracing keys off x)
      physBound : vec4<f32>,                                         // xyz = centre (window coords) of a sphere enclosing EVERY body, w = its radius — one test rejects a ray that cannot touch any of them
      heldCfg : vec4<f32>,                                           // x = SUN visibility at the player (gates the held item's DIRECT term), y = its SKY visibility (gates the ambient + ground bounce, standing in for the irr.g the world gets), z = STACKBADGE: how many of the held item you are carrying, drawn beside the hand by BLIT (NOT spare — see UF_HELDCFG), w spare but ACTIVELY ZEROED every frame by the tick-camera line that writes x/y/z, so a value written to it anywhere else is gone by the time the GPU sees it
      // ── APPENDED TAIL ── these MUST stay in the same order as the UF writes at the end of the frame
      // (heldCfg 1860, lgt 1864, hurtB 1868, hurtH 1872). WGSL lays a struct out in declaration order,
      // so re-ordering these silently feeds each field its neighbour's numbers instead of erroring.
      lgt : vec4<f32>,                                               // ── LIGHT DEBUG ── x = bitmask of which lighting terms are enabled (see LG / the top-right panel). All bits set = the normal image.
      hurtB : vec4<f32>,                                             // ── HIT FLASH ── xyz = centre (window coords) of the animal the knife just wounded, w = flash strength (0 = nothing showing, the usual case)
      hurtH : vec4<f32>,                                             // …and its half-extents. Snug: taken from the voxels the animal actually stamped, so the ground it stands on stays its own colour.                                           // x = SUN VISIBILITY at the player (0 = the tool in your hands is in shade, 1 = open sun). Held items had no visibility term at all and were lit as if always in the open.
      // ── SECOND HALF OF THE DROP ARRAY (slots 64..127) ── APPENDED, never inserted. Every JS write past
      // 'drops' is a hardcoded float index (1092…1875): growing it in place would have shifted all of
      // them and silently fed each field its neighbour's numbers. So the extra capacity lands here at the
      // very end, and dropV()/lifeMotV() below stitch the two halves into one logical 128-slot array.
      dropsB : array<vec4<f32>, ${(DROP_SLOTS - DROP_HALF) * 4}>,                                // drop slots 64..127, same 4-vec4 layout as 'drops' above
      lifeMotB : array<vec4<f32>, ${DROP_SLOTS - DROP_HALF}>,                               // …and their lifeMot entries
      // ── DEPTH OF FIELD ── appended at the VERY end, after both drop halves, for exactly the reason they are:
      // every JS write past 'drops' is a hardcoded float index, so a field inserted anywhere above would shift
      // them all and silently feed each one its neighbour's numbers. See UF_DOF on the JS side.
      dof : vec4<f32>,                                               // x = FOCUS distance in voxels (0 = the effect is OFF — one compare skips the whole gather), y = max circle-of-confusion radius in CANVAS pixels, z = gather taps per pixel of radius, w spare
    }
    @group(0) @binding(0) var<uniform> u : U;
    ${UNI_CONST}
    const DROP_N : i32 = ${DROP_SLOTS};                                        // total drop slots (25 fixed + the flock + traced creatures). 128 = exactly four 32-bit tile-mask words; a 129th needs a fifth.
    fn dropV(i : i32) -> vec4<f32> { if (i < ${DROP_HALF * 4}) { return u.drops[i]; } return u.dropsB[i - ${DROP_HALF * 4}]; }        // one logical drops[] over the two halves. The index is the loop counter, workgroup-uniform, so this is a scalar branch — and after the bit-scan it only runs for slots that actually touch the tile.
    fn lifeMotV(i : i32) -> vec4<f32> { if (i < ${DROP_HALF}) { return u.lifeMot[i]; } return u.lifeMotB[i - ${DROP_HALF}]; }   // …and one logical lifeMot[]
    const WX : i32 = ${WX}; const WY : i32 = ${WY}; const WZ : i32 = ${WZ};
    const BX : i32 = ${BX}; const BY : i32 = ${BY}; const BZ : i32 = ${BZ};
    const SUN_COL : vec3<f32> = vec3<f32>(3.60, 3.24, 2.74);
    const ZENITH  : vec3<f32> = vec3<f32>(0.118, 0.302, 0.663);   // deep clear blue
    const HORIZON : vec3<f32> = vec3<f32>(0.402, 0.567, 0.769);   // blue haze, not grey
    const BOUNCE  : vec3<f32> = vec3<f32>(0.238, 0.207, 0.156);
    // ── WATER EXTINCTION ── per voxel (10 cm), for EVERY through-water path. It is a named constant because
    // it was two separate literals: the bed was scaled to 0.8 on 2026-08-06 ("20% more transparent") and the
    // creature path was missed, so a fish faded 25% faster than the lakebed directly behind it.
    const WATER_SIG : vec3<f32> = vec3<f32>(0.240, 0.0920, 0.0416);
    const HURT_RED : vec3<f32> = vec3<f32>(1.0, 0.07, 0.055);   // the wound red — used by BOTH the hit flash on the animal and the blood voxels it throws, so they always match exactly (user 2026-08-05). It has to stay <= 1 in every channel for that claim to hold: the animal path (TRACE) writes sqrt(albedo) into gAlbedo, which is rgba8unorm, so any component above 1 is CLAMPED there and decodes as 1.0 — while the blood path (COMPOSITE) multiplies this constant directly and kept the full value. At 2.1 the flash showed 1.0 and the spray carried 2.1x the red, which read as a brighter material in shade. 1.0 is what the flash has always actually displayed, so the animal's pixels are unchanged and the blood now matches them.
    fn rayDir(px : vec2<f32>) -> vec3<f32> {
      let ndc = (px / u.res) * 2.0 - 1.0;
      return normalize(u.fwd + u.right * (ndc.x * u.tanH * u.aspect) - u.up * (ndc.y * u.tanH));
    }
    fn prevUVd(d : vec3<f32>) -> vec2<f32> {
      let z = dot(d, u.pFwd);
      if (z <= 0.01) { return vec2<f32>(-1.0); }
      let x = dot(d, u.pRight) / (z * u.pTanH * u.pAspect);
      let y = dot(d, u.pUp) / (z * u.pTanH);
      let uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
      return (uv * u.res - u.pJit) / u.res;
    }
    // LIGHT DEBUG — is term n enabled? Bits, in panel order: 0 sun shadow, 1 ambient occlusion,
    // 2 creature shadow boxes, 3 firefly/lava glow, 4 SVGF REACTIVE MASK (TRACE caps the history of pixels a
    // moving body/shadow touches — NOT water; water reflect/refract are bits 18/19 below), 5 fog,
    // 6 irradiance history, 7 spatial filter, 8 TAA. Everything on = the normal image.
    // The authoritative list is the terms map in debug-api's lgt().
    const FOAM_C = vec3<f32>(${FOAM_RGB[0]}, ${FOAM_RGB[1]}, ${FOAM_RGB[2]});   // shoreline foam AND the splash droplet — one constant so the two can never drift apart (see FOAM_RGB)
    fn LG(b : u32) -> bool { return (u32(u.lgt.x + 0.5) & (1u << b)) != 0u; }
    // ── SECOND MASK (u.lgt.z) ── lgt.x is FULL: it carries 24 terms and an f32 holds integers exactly only
    // to 2^24, so a 25th bit there would start rounding and flip its neighbours. Extra switches live here.
    fn LG2(b : u32) -> bool { return (u32(u.lgt.z + 0.5) & (1u << b)) != 0u; }
    // ── BACK-LIT FOLIAGE ── BAKED IN at 30% of the swept strength (user 2026-08-08), so these are constants
    // and not a uniform: no slider, no keybind, no per-frame float. FOL_LOBE is the forward-lobe exponent and
    // it matters more than the strength does — it sets how wide an arc around the sun the glow fires over, and
    // dropping it 4 → 2 did more than doubling strength did. ?nofol compiles the whole thing out for an A/B.
    const FOLBACK : bool = ${location.search.includes('nofol') ? 'false' : 'true'};
    const FOL_STR : f32 = 1.2;                                      // 30% of the tuned 4.0
    const FOL_LOBE : f32 = 2.0;
    // 9 body grain (felled chunks), 10 terrain grain, 11 creature grain, 12 sun penumbra,
    // 13 caustics, 14 bounce light, 15 sky/ambient, 16 held-item shading, 17 volumetric light,
    // 18 water reflect, 19 water refract, 20 water foam, 21 water ice, 22 water glisten, 23 water waves.
    fn rand(s : ptr<function, u32>) -> f32 {
      *s = *s * 747796405u + 2891336453u;
      let w = ((*s >> ((*s >> 28u) + 4u)) ^ *s) * 277803737u;
      return f32((w >> 22u) ^ w) / 4294967296.0;
    }
    fn faceN(id : u32) -> vec3<f32> {
      var FN = array<vec3<f32>, 6>(vec3<f32>(1.0,0.0,0.0), vec3<f32>(-1.0,0.0,0.0), vec3<f32>(0.0,1.0,0.0), vec3<f32>(0.0,-1.0,0.0), vec3<f32>(0.0,0.0,1.0), vec3<f32>(0.0,0.0,-1.0));
      return FN[min(id, 5u)];
    }
    fn ih3(x : i32, y : i32, z : i32) -> f32 {
      var h = u32(x) * 374761393u + u32(y) * 3266489917u + u32(z) * 668265263u;
      h = (h ^ (h >> 13u)) * 1274126177u;
      return f32(h ^ (h >> 16u)) / 4294967296.0;
    }
    fn isMoon() -> bool { return (u32(u.fx) & 8u) != 0u; }          // at night u.sunDir carries the MOON direction
    fn dayScale() -> f32 {                                           // global light level — SMOOTH through the sun↔moon swap (user: no abrupt jump at dusk 18:13 / dawn 5:46)
      let sy = select(u.sunDir.y, -u.sunDir.y, isMoon());            // TRUE sun elevation: in moon mode u.sunDir is the moon (opposite point), so −u.sunDir.y is the real sun — continuous across the swap
      let dayVal = 0.025 + 0.975 * smoothstep(-0.10, 0.28, sy);      // daytime curve
      return mix(0.0135, dayVal, smoothstep(-0.25, -0.05, sy));      // ease from the moon floor (0.0135) up to the day curve across twilight — no step where moonMode flips
    }
    fn sunTint() -> vec3<f32> {                                      // sun goes amber at the horizon; the moon is cool silver-blue
      if (isMoon()) { return vec3<f32>(0.40, 0.50, 0.78) * 0.198 * smoothstep(0.0, 0.15, u.sunDir.y); }   // moonlight follows the darker nights: 0.44 → 0.352 → 0.264 → 0.198
      let warm = mix(vec3<f32>(1.0, 0.42, 0.18), vec3<f32>(1.0, 1.0, 1.0), smoothstep(0.02, 0.38, u.sunDir.y));
      return SUN_COL * warm * smoothstep(-0.05, 0.10, u.sunDir.y);
    }
    @group(0) @binding(14) var moonTex : texture_2d<f32>;            // sampled only by skyColor — pipelines that never call it drop these bindings
    @group(0) @binding(15) var moonSamp : sampler;
    fn skyBase(rd : vec3<f32>) -> vec3<f32> {
      let y = clamp(rd.y, -1.0, 1.0);
      var c = mix(HORIZON, ZENITH, pow(max(y, 0.0), 0.38));        // fast falloff — blue takes over just above the horizon
      c = mix(c, HORIZON * 0.72, smoothstep(0.0, -0.28, y));
      c *= dayScale();
      let sy = select(u.sunDir.y, -u.sunDir.y, isMoon());                       // TRUE sun elevation + direction (moon = opposite point) — the sunset band stays in the WEST and fades smoothly instead of vanishing when moonMode flips
      let sdir = select(u.sunDir, -u.sunDir, isMoon());
      let sethue = smoothstep(0.30, 0.04, abs(sy)) * smoothstep(-0.22, 0.02, sy);
      let sdh = normalize(vec3<f32>(sdir.x, 1e-4, sdir.z));
      c += vec3<f32>(0.62, 0.24, 0.08) * sethue * pow(max(dot(rd, sdh), 0.0), 3.0) * clamp(1.0 - abs(rd.y) * 2.2, 0.0, 1.0);   // sunrise/sunset band
      return c;
    }
    fn skyColor(rd : vec3<f32>) -> vec3<f32> {
      var c = skyBase(rd);
      let realSun = select(u.sunDir, -u.sunDir, isMoon());            // the ACTUAL sun (u.sunDir carries the up-body; −it is the down-body). Both are continuous across the dusk/dawn swap.
      let realMoon = -realSun;                                         // the moon is the opposite point — so as the sun sets in the west, the moon RISES in the east, both drawn from their OWN elevation
      { let s = dot(rd, realSun); let up = smoothstep(-0.03, 0.06, realSun.y);   // ── SUN disc: only while the sun is above the horizon; it sinks below the map at dusk and rises at dawn (no pop) ──
        c += SUN_COL * 6.0 * smoothstep(0.999939, 0.999962, s) * up;  // hard-edged disc, halved
        c += SUN_COL * 0.5 * pow(max(s, 0.0), 2600.0) * up; }         // GLARE — a tight corona hugging the disc
      { let s = dot(rd, realMoon); let up = smoothstep(-0.03, 0.06, realMoon.y);   // ── MOON disc: a real NASA photograph, only while the moon is above the horizon — rises as the sun sets ──
        let md = smoothstep(0.999742, 0.999787, s);
        if (md > 0.001) {
          let T = normalize(cross(realMoon, vec3<f32>(0.0, 1.0, 0.0)));
          let B = cross(realMoon, T);
          let uv = vec2<f32>(dot(rd, T), dot(rd, B)) * (0.5 / 0.0217) + vec2<f32>(0.5);
          let mt = textureSampleLevel(moonTex, moonSamp, clamp(uv, vec2<f32>(0.0), vec2<f32>(1.0)), 0.0).rgb;
          let du = uv * 2.0 - vec2<f32>(1.0);                        // disc-local coords → sphere normal for the PHASE terminator
          let nz = sqrt(max(0.0, 1.0 - du.x * du.x - du.y * du.y));
          let ph = u.rdist.y * 6.2831853;                            // 0 = new, pi = full
          let lam = max(dot(vec3<f32>(du.x, du.y, nz), vec3<f32>(sin(ph), 0.0, -cos(ph))), 0.0);
          c += mt * 4.5 * md * up * (0.035 + 0.965 * lam);           // lambert-lit lunar sphere + a whisper of earthshine on the dark side
        }
      }
      let night = 1.0 - smoothstep(-0.16, 0.02, realSun.y);          // stars fade in on the real sun elevation — continuous across the swap (no pop)
      if (night > 0.02 && rd.y > 0.02) {                             // stars
        let cc = vec3<i32>(rd * 480.0);
        let hh = ih3(cc.x, cc.y, cc.z);
        if (hh > 0.9985) { c += vec3<f32>(0.9, 0.93, 1.0) * (hh - 0.9985) * 500.0 * night; }
      }
      return c;
    }
    fn aces(x : vec3<f32>) -> vec3<f32> {
      return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));
    }
    const WLF : f32 = ${WL}.0;
    fn gerstH(wx : f32, wz : f32) -> f32 {                           // GERSTNER height (4-wave sum) — the voxel wave columns + JS floaters quantize THIS
      var h_ = 0.0;
      ${GERSTH_WGSL}
      return h_;
    }
    fn gerstN(wx : f32, wz : f32) -> vec3<f32> {                     // GERSTNER shading normal — the Q term pinches crests (GPU-Gems form)
      var nx_ = 0.0; var nz_ = 0.0; var ny_ = 1.0;
      ${GERSTN_WGSL}
      return normalize(vec3<f32>(nx_, max(ny_, 0.30), nz_));
    }
    fn vn2(p : vec2<f32>) -> f32 {
      let f = floor(p); let i = vec2<i32>(f);
      var w = p - f; w = w * w * (3.0 - 2.0 * w);
      let a = mix(ih3(i.x, i.y, 7), ih3(i.x + 1, i.y, 7), w.x);
      let b = mix(ih3(i.x, i.y + 1, 7), ih3(i.x + 1, i.y + 1, 7), w.x);
      return mix(a, b, w.y);
    }
    fn caust(p : vec2<f32>) -> f32 {                                 // CAUSTICS — two drifting noise fields; their coincidence lines are the bright webs
      let n1 = vn2(p * 0.09 + vec2<f32>(u.time * 0.275, u.time * 0.17));    // caustic drift halved with the waves
      let n2 = vn2(p * 0.11 + vec2<f32>(-u.time * 0.235, u.time * 0.145));
      return pow(clamp(1.0 - abs(n1 - n2) * 2.6, 0.0, 1.0), 5.0);
    }
    fn ivhash(c : vec3<i32>) -> f32 {
      var x = u32(c.x) * 374761393u + u32(c.y) * 668265263u + u32(c.z) * 2246822519u;
      x = (x ^ (x >> 13u)) * 1274126177u;
      return f32((x ^ (x >> 16u)) & 1023u) / 1023.0;
    }
  `;

