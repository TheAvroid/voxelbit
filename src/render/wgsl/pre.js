  // ── WGSL ────────────────────────────────────────────────────────────────────
  // ── THE SUN'S ANGULAR RADIUS, ONCE ── the WGSL below needs both the cosine (for the cheap disc test) and
  // the angle itself (for the moon's disc coordinates). Deriving one from the other in JS is the only way
  // they cannot drift; a hand-typed second literal is a bug waiting for the next size change.
  const SUN_COS_R = 0.999756245;
  const SUN_ANG_R = Math.acos(SUN_COS_R);
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
      lifeCfg : vec4<f32>,                                           // x = life debug view (0 off, 1 slot ids, 2 history confidence, 3 motion vectors, 4 raw AO), y = trace-injection enabled (0 under ?oldlife → full analytic fallback), z = the standing heart level the vitals ring reads, w = the RAIN SKY scalar (storm ramp x oakM at the
                                                                   // camera; 0 in fair weather and outside the oak forest). NEITHER IS SPARE - hurtV.w is the last float of the
                                                                   // whole uniform buffer, which is why it was safe to take: nothing sits below it to shift
      physB : array<vec4<f32>, ${PHYS_MAX * 5}>,                       // ── RIGID BODIES ── PHYS_MAX x 5 vec4 (24 today, was a hardcoded 16) — the length is INTERPOLATED from the JS constant so the shader can never disagree with the offsets buffers.js derives:
                                                                     // [0] anchor.xyz (camera space, at the COM) + voxel scale · [1] local X axis in camera space + dimX
                                                                     // [2] local Y axis + dimY · [3] local Z axis + dimZ · [4] comLocal.xyz + buffer offset
      physC : vec4<f32>,                                             // x = ACTIVE body count (0 = the whole path is one compare), y = REACTIVE STRENGTH 0..1 (1 while a body is moving, eased to 0 over ~0.45 s after it stops — a binary here made a settling trunk pulse its own shadow noise on and off), z/w spare. Was: how many are AWAKE (the reactive mask keys off y; tracing keys off x)
      physBound : vec4<f32>,                                         // xyz = centre (window coords) of a sphere enclosing EVERY body, w = its radius — one test rejects a ray that cannot touch any of them
      heldCfg : vec4<f32>,                                           // x = SUN visibility at the player (gates the held item's DIRECT term), y = its SKY visibility (gates the ambient + ground bounce, standing in for the irr.g the world gets), z = STACKBADGE: how many of the held item you are carrying, drawn beside the hand by BLIT (NOT spare — see UF_HELDCFG), w spare but ACTIVELY ZEROED every frame by the tick-camera line that writes x/y/z, so a value written to it anywhere else is gone by the time the GPU sees it
      // ── APPENDED TAIL ── these MUST stay in the same order as the UF writes at the end of the frame
      // (UF_HELDCFG, UF_LGT, UF_HURTB, UF_HURTH — all derived from PHYS_MAX). WGSL lays a struct out in declaration order,
      // so re-ordering these silently feeds each field its neighbour's numbers instead of erroring.
      lgt : vec4<f32>,                                               // ── LIGHT DEBUG ── x = bitmask of which lighting terms are enabled (see LG / the top-right panel). All bits set = the normal image.
      hurtB : vec4<f32>,                                             // ── HIT FLASH ── xyz = centre (window coords) of the animal the knife just wounded, w = flash strength (0 = nothing showing, the usual case)
      hurtH : vec4<f32>,                                             // …and its half-extents. Snug: taken from the voxels the animal actually stamped, so the ground it stands on stays its own colour.                                           // x = SUN VISIBILITY at the player (0 = the tool in your hands is in shade, 1 = open sun). Held items had no visibility term at all and were lit as if always in the open.
      // ── SECOND HALF OF THE DROP ARRAY (slots 64..127) ── APPENDED, never inserted. Every JS write past
      // 'drops' is a hardcoded float index (1092…UF_HURTH+3): growing it in place would have shifted all of
      // them and silently fed each field its neighbour's numbers. So the extra capacity lands here at the
      // very end, and dropV()/lifeMotV() below stitch the two halves into one logical 128-slot array.
      dropsB : array<vec4<f32>, ${(DROP_SLOTS - DROP_HALF) * 4}>,                                // drop slots 64..127, same 4-vec4 layout as 'drops' above
      lifeMotB : array<vec4<f32>, ${DROP_SLOTS - DROP_HALF}>,                               // …and their lifeMot entries
      // ── DEPTH OF FIELD ── appended at the VERY end, after both drop halves, for exactly the reason they are:
      // every JS write past 'drops' is a hardcoded float index, so a field inserted anywhere above would shift
      // them all and silently feed each one its neighbour's numbers. See UF_DOF on the JS side.
      dof : vec4<f32>,                                               // x = FOCUS distance in voxels (0 = the effect is OFF — one compare skips the whole gather), y = max circle-of-confusion radius in CANVAS pixels, z = gather taps per pixel of radius, w spare
      // ── FLOATING HEARTS ── appended after dof for the same reason dof was appended after the drop halves.
      heart  : vec4<f32>,                                            // xyz = the FIRST heart's anchor in CAMERA space (voxel units, the same space the held item's pickA lives in), w = its voxel scale at full health
      heartC : vec4<f32>,                                            // x = the heart model's item id (0 = single.vox never loaded → the whole block compiles to one compare and draws nothing), y = health in HEARTS (hp / 4, so 5.0 is a full bar), z = the gap between two hearts in the same camera units, w = HURT KICK 0..1 (VIT.hurtT — the row swells and brightens for the half second after a hit)
      // ── THE HURT FLASH ── the red vignette that fires on every hit. It used to be a DOM <canvas id="hurtFx">
      // laid over the game and faded by a CSS keyframe. That works on screen and is INVISIBLE IN A RECORDING:
      // veStartRec captures the WebGPU canvas with canvas.captureStream(60), and a DOM element sitting on top of
      // that canvas is not part of the captured surface (user 2026-08-16: "the red pixels on the ui dont show up").
      // Compositing the two canvases into a third is not open either — drawImage of this canvas reads back all
      // zero. So the flash is now DRAWN INTO THE IMAGE, in BLIT, and the recording gets it because it is the image.
      hurtV  : vec4<f32>,                                            // x = flash strength 0..1 (0 = the whole block is one compare), y = the PER-HIT dither seed (fixed for the length of one flash, so the blocks fade rather than sizzle), z = the standing heart level, w = the RAIN SKY scalar (see UF_RAINK) — NOT spare
      // ── THE STACK BADGE'S PLACEMENT (user 2026-08-17: "give me some sliders to adjust the positioning of
      // the x(#)") ── appended at the VERY end, after hurtV, which is the rule every field back here follows:
      // the JS writes this buffer at fixed float indices, so a lane inserted anywhere above silently feeds
      // every one below it its neighbour's numbers. hurtV was genuinely full (see UF_RAINK), so this is a
      // fresh vec4 rather than a borrowed lane. Written every frame in main/tick-camera.js from the sliders
      // in the held-item panel (ui/hud.js), so it is never the zero a cold buffer would hand BLIT.
      badge  : vec4<f32>,                                            // x/y = where the badge STARTS, in canvas PIXELS — the held model's own projected top-right corner plus the panel's nudge, computed in main/tick-camera.js. z = glyph size multiplier, w = tilt in radians
      // ── HUNGER (user 2026-08-19) ── x = 0..4, the same 0..4 hurtV.z carries for health, so BLIT draws ONE
      // effect twice in two colours rather than two effects. Appended at the very end for the reason badge was.
      vitG   : vec4<f32>,
      // ── THE CRAFT PREVIEW ── a third hand, same layout as pick2 (see render/buffers.js UF_PICK3). It hangs
      // between the two real hands while the STONE AGE bench is open and is hidden (pick3X.w = 0) otherwise.
      pick3A : vec4<f32>,
      pick3X : vec4<f32>, pick3Y : vec4<f32>, pick3Z : vec4<f32>,
      // ── THE OFF-HAND'S STACK BADGE ── same layout as the right hand's badge lane; its COUNT rides in vitG.y (see render/buffers.js).
      badge2 : vec4<f32>,                                            // x = hunger level 0 (full stomach) .. 4 (empty, and starving). y/z/w spare.
      // ── RIGID-BODY GROUP SPHERES ── xyz = centre (window coords), w = radius, over PHYS_GRP consecutive bodies;
      // w <= 0 means the group is empty. Appended at the VERY end, same rule as every field above it.
      physG : array<vec4<f32>, ${PHYS_NG}>,
      // ── THE CLOUD CLOCK ── x = seconds of CLOUD time, which is not wall time: it advances with the
      // day/night cycle speed so the deck drifts in step with the sun (user 2026-08-28: "make the clouds move
      // with the time of the sun … when I speed up the day night cycle, also speed up the cloud movement").
      // At 1x it equals u.time exactly, so the deck's normal drift is unchanged. Appended at the VERY end for
      // the reason every field back here is. y/z/w spare.
      cloudT : vec4<f32>,
      // ── SURFACE DISTURBANCES ── xy = world XZ of the ring's centre, z = birth on u.time's clock, w = strength
      // (0 = empty). Compacted, so the readers stop at the first empty slot. Appended at the VERY end, for the
      // reason every field back here is: each JS write past 'drops' is a hardcoded float index.
      ripple : array<vec4<f32>, ${RIP_N}>,
    }
    @group(0) @binding(0) var<uniform> u : U;
    ${UNI_CONST}
    const DROP_N : i32 = ${DROP_SLOTS};                                        // total drop slots (25 fixed + the flock + traced creatures). 128 = exactly four 32-bit tile-mask words; a 129th needs a fifth.
    const PHYS_GRP : i32 = ${PHYS_GRP};                                        // bodies per group sphere in u.physG — bodyTrace culls a whole slab of the debris on one compare (see render/buffers.js)
    fn dropV(i : i32) -> vec4<f32> { if (i < ${DROP_HALF * 4}) { return u.drops[i]; } return u.dropsB[i - ${DROP_HALF * 4}]; }        // one logical drops[] over the two halves. The index is the loop counter, workgroup-uniform, so this is a scalar branch — and after the bit-scan it only runs for slots that actually touch the tile.
    fn lifeMotV(i : i32) -> vec4<f32> { if (i < ${DROP_HALF}) { return u.lifeMot[i]; } return u.lifeMotB[i - ${DROP_HALF}]; }   // …and one logical lifeMot[]
    const WX : i32 = ${WX}; const WY : i32 = ${WY}; const WZ : i32 = ${WZ};
    const BX : i32 = ${BX}; const BY : i32 = ${BY}; const BZ : i32 = ${BZ};
    const SUN_COL : vec3<f32> = vec3<f32>(3.60, 3.24, 2.74);
    // ── THE SUN'S ANGULAR RADIUS ── the MOON's own outer threshold, so the two discs are the same size
    // (user 2026-08-28: "make the sun as big as the moon"). Real sun and moon are both ~0.53 degrees and
    // very nearly equal, so matching them is the physical answer as well as the asked-for one.
    const SUN_COSR : f32 = ${SUN_COS_R};
    const SUN_ANGR : f32 = ${SUN_ANG_R};                              // …the same radius as an ANGLE, for the moon's disc coordinates                              // cos(1.2651 deg) -> a 2.5302 deg disc. Size lives here as an ANGLE, so scaling by k is cos(k * acos(edge)), worked out rather than eyeballed. Running total (user 2026-08-28): moon-matched 0.999742 (2.6031 deg) -> 25% smaller 0.999854872 (1.9523) -> 20% bigger 0.999791018 (2.3428) -> 35% bigger 0.999619141 (3.1627) -> 20% smaller, this (2.5302). The flare in blit.js is sized off this now, so a size change finally carries the glare with it instead of being pinned by it. It is no longer the moon's number, so the two discs are free to differ. SUN_SOFT below needs no adjustment: it is expressed in rr, which is relative to THIS radius, so the soft edge scales with the disc on its own
    const SUN_DISC : f32 = 0.6;                                      // the disc's peak, as a multiple of SUN_COL. Was 6.0; see the note at the draw for why the compositor sets a ceiling on it
    const SUN_CORE : f32 = 0.55;                                     // ── WHERE THE FALLOFF STARTS, in rr — inside this the disc is flat ──
    const SUN_FALL : f32 = 2.2;                                      // ── AND ITS DECAY, which is the ONLY thing that sets how soft the edge looks ──
    // A smoothstep cannot do this job however it is placed, and three passes of moving it proved that. The
    // disc is SUN_COL * 6 = 21.6 linear, so all that is ever VISIBLE of the profile is where it crosses
    // 1/21.6 = 0.046 — deep in its tail. smoothstep is STEEP there, so the whole transition from white to
    // nothing was squeezed into ~2 px no matter where the curve was put: SUN_EXT moved the edge outward and
    // left it every bit as hard. An exponential has a constant log-slope, so the width of that crossing is
    // set directly by SUN_FALL and nothing else. MEASURED at the same edge position: 2.2 px of fade before,
    // 9.5 px after. Same apparent size, four times the softness.
    const SUN_MUFLOOR : f32 = 0.12;                                  // …and a floor under the limb-darkening term, because mu goes to 0 at the sphere's edge and would multiply the skirt straight back out of existence. It also leaves the outermost fringe the reddest part of the disc, which is where limb darkening was already heading
    // ── LIMB DARKENING, Hestroffer & Magnan 1998 ── the reason a real sun does not read as a flat white
    // blob: its centre is bright and neutral and its edge is markedly darker AND redder, because a sight
    // line near the limb passes obliquely and only reaches cooler, higher photosphere. I(mu)/I(1) = mu^alpha
    // with alpha(lambda) = -0.023 + 0.292/lambda (lambda in micron), which at the 695/555/433 nm primaries
    // this kind of renderer uses gives the constant below — the same one Hillaire/Frostbite-derived sky
    // implementations and Unreal's SkyAtmosphere carry. At 0.99 of the radius the limb is at 0.46 of centre
    // in red and 0.28 in blue: that gradient, and the definite edge under it, is what makes it a SPHERE
    // rather than a brush stroke.
    const SUN_LIMB : vec3<f32> = vec3<f32>(0.397, 0.503, 0.652);
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
    // ── NIGHT MASK (u.lgt.w) ── the NIGHT panel's own switches (L opens it; ui/hud.js owns the mask and
    // main/tick-camera.js publishes it). lgt.w is where these live rather than a new uniform field because it
    // was the buffer's DECLARED SPARE — tick-camera wrote a literal 0 into it every frame and nothing read it —
    // so the whole panel costs no struct churn and cannot shift a single downstream offset, which is the
    // failure this codebase pays for most dearly. Bits, in panel order:
    //   0 moonlight (a harder moon key + a dimmer isotropic night floor, so moon shadows READ)
    //   1 moon phase (the baked crescent turned to face the sun instead of double-shaded)
    //   2 milky way   3 star twinkle   4 firefly light   5 shooting stars
    fn NG(b : u32) -> bool { return (u32(u.lgt.w + 0.5) & (1u << b)) != 0u; }
    // ── BACK-LIT FOLIAGE ── BAKED IN at 30% of the swept strength (user 2026-08-08), so these are constants
    // and not a uniform: no slider, no keybind, no per-frame float. FOL_LOBE is the forward-lobe exponent and
    // it matters more than the strength does — it sets how wide an arc around the sun the glow fires over, and
    // dropping it 4 → 2 did more than doubling strength did. ?nofol compiles the whole thing out for an A/B.
    // ── AND IT IS A SWITCH NOW, NOT A COMPILE FLAG (user 2026-08-20: put it on the L toggle) ── it was a
    // const driven by ?nofol, which meant judging it needed a reload. Bit 6 of the panel mask, read through
    // NG, so it flips live. The TRACE gate that shoots a sun ray for leaves reads the same bit, so turning it
    // off stops paying for those rays as well as stopping the draw — an A/B that measures cost, not just look.
    const FOLBACK_URL : bool = ${location.search.includes('nofol') ? 'false' : 'true'};   // ?nofol still forces it off for a reload-free-of-panel-state A/B
    const FOL_STR : f32 = 1.2;                                      // 30% of the tuned 4.0
    const FOL_LOBE : f32 = 2.0;
    // ── HOW FAR THE PER-VOXEL GRAIN REACHES (user 2026-08-20) ── see the note at the albedo branch in TRACE.
    // 80 is roughly where a voxel drops under a pixel at this field of view and resolution, and 260 is where
    // even a clump of them does; between the two the jitter eases off rather than ending on a line.
    const GRAIN_NEAR : f32 = 80.0;
    const GRAIN_FAR  : f32 = 260.0;
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
    // ── HOW FAR INTO THE NIGHT ── 1 deep at night, 0 by day, read off the TRUE sun elevation and eased over
    // EXACTLY the window dayScale() below uses. Every night term multiplies through this instead of branching
    // on isMoon(), which is the standing rule here: isMoon() flips the instant the moon clears the horizon, so
    // a light term keyed on it JUMPS at the dusk and dawn swaps. This cannot — at the swap sy is ≈ -0.05, where
    // the smoothstep has already reached 1 and this is already 0, so a term scaled by it is bit-identical to
    // the unscaled one on both sides of the flip. Declared BELOW isMoon(): WGSL has no forward declarations.
    fn nightK() -> f32 { return 1.0 - smoothstep(-0.25, -0.05, select(u.sunDir.y, -u.sunDir.y, isMoon())); }
    // ── HOW MUCH MOON THERE IS ── the illuminated fraction, (1 + cos alpha) / 2, off the same phase the
    // disc is drawn with. ONE definition because it is read in three places that must never disagree: the
    // moon's own face, the KEY light it casts on the world, and the bloom the cloud deck throws for it. It
    // was written out longhand in the first two and simply MISSING from the third, so a new moon still lit
    // the clouds (user 2026-08-28: "when the moon is a new moon it should not emit light at all through the
    // clouds"). 1 at full, 0 at new.
    fn moonPhaseF() -> f32 { return 0.5 + 0.5 * cos(u.rdist.y * 6.2831853); }
    // ── THE NIGHT'S AMBIENT FLOOR ── the faint isotropic top-up every surface gets no matter what reaches
    // it. By day it is a rounding error next to the sun; at night it is the LARGEST term on a shadowed voxel,
    // which is exactly why moon shadows used to sit at ~5:1 against a moonlit face and read as tint rather
    // than as shadow. Night bit 0 takes it down, and the moon key up, so the ratio opens to ~15:1.
    const AMB_FLOOR : vec3<f32> = vec3<f32>(0.012, 0.013, 0.016);
    const MOON_FLR : f32 = 0.42;                                     // …what is left of that floor deep at night with bit 0 on
    const MOON_AMB : f32 = 0.62;                                     // …and of dayScale()'s own night floor, i.e. of the sky glow
    const MOON_LMIN : f32 = 0.06;                                    // what is left of the moon's KEY light at new moon — near nothing, but not zero
    const MOON_KEY : f32 = 1.34;                                     // …against a moon key raised this much, so a LIT face barely moves and a shadowed one falls away
    fn ambFloor() -> vec3<f32> { return AMB_FLOOR * mix(1.0, MOON_FLR, nightK()); }
    fn dayScale() -> f32 {                                           // global light level — SMOOTH through the sun↔moon swap (user: no abrupt jump at dusk 18:13 / dawn 5:46)
      let sy = select(u.sunDir.y, -u.sunDir.y, isMoon());            // TRUE sun elevation: in moon mode u.sunDir is the moon (opposite point), so −u.sunDir.y is the real sun — continuous across the swap
      let dayVal = 0.025 + 0.975 * smoothstep(-0.10, 0.28, sy);      // daytime curve
      let mf = 0.0135 * mix(1.0, MOON_AMB, nightK());   // the MOONLIGHT contrast pass takes the isotropic sky glow down (BAKED IN, user 2026-08-19: "bake in the moonlight effect. remove it from the list" — it was night bit 0); nightK() is 0 by the swap so the day side of this is untouched
      return mix(mf, dayVal, smoothstep(-0.25, -0.05, sy));           // ease from the moon floor (0.0135) up to the day curve across twilight — no step where moonMode flips
    }
    fn sunTint() -> vec3<f32> {                                      // sun goes amber at the horizon; the moon is cool silver-blue
      // ── AND IT SCALES WITH THE PHASE (user 2026-08-28: "the moon is still casting light when it's a new
      // moon; the light should correspond to the phases") ── this is the moon's KEY light, the directional
      // term the whole world is lit and shadowed by at night, and it used to be a constant: a new moon lit
      // the ground exactly as hard as a full one. It is the same phase the disc is drawn with, u.rdist.y, so
      // the sky and the ground can never disagree about how much moon there is.
      // MOON_LMIN and not 0: a real new moon leaves starlight and airglow, and this engine has a separate
      // isotropic night floor for that (ambFloor and dayScale's own mf) which is NOT touched here — so the
      // world stays legible while the moon's own directional light, and its shadows with it, go away.
      if (isMoon()) { let phF = moonPhaseF();
        return vec3<f32>(0.94, 0.95, 1.00) * 0.198 * smoothstep(0.0, 0.15, u.sunDir.y) * mix(1.0, MOON_KEY, nightK()) * mix(MOON_LMIN, 1.0, phF); }   // WHITE moonlight (user 2026-08-19: "make it cast white light from it") — was a deep blue 0.40/0.50/0.78, which is the stylised night-blue every game reaches for; 0.94/0.95/1.00 is a faint cool white instead, so surfaces keep their OWN colour under it and only the exposure says night. The 0.198 magnitude is untouched, so nothing gets brighter — the light only stops being blue. Moonlight follows the darker nights: 0.44 -> 0.352 -> 0.264 -> 0.198   // …and the moonlight pass lifts the KEY while ambFloor()/dayScale() drop everything around it, which is a contrast change and very nearly not a brightness one — the darker nights the four steps above bought are the thing this must not undo
      let warm = mix(vec3<f32>(1.0, 0.42, 0.18), vec3<f32>(1.0, 1.0, 1.0), smoothstep(0.02, 0.38, u.sunDir.y));
      return SUN_COL * warm * smoothstep(-0.05, 0.10, u.sunDir.y);
    }
    // ── VIEW-MODEL LIGHT ── ONE shading model for everything carried in front of the eye: the stone tools in
    // both hands, and the health row. It is the WORLD's own light — sun, sky and the warm ground bounce — with
    // the two visibility scalars JS marches from the eye standing in for the traced irradiance this path has
    // none of (it is composited past the g-buffer). u.heldCfg.x gates the DIRECT term, so a tool in shade goes
    // dark like the ground it stands over; u.heldCfg.y gates the AMBIENT + bounce, doing the job irr.g does for
    // a world voxel — without it a tool kept the full open-sky value under a canopy or underground.
    // NOTE what is deliberately absent: per-voxel grain. It is ±12% per voxel out in the world and it scrambles
    // hand-authored .vox gradients (the axe handle steps by ~5%).
    // SHARED, and that is the point. The hearts used to carry a key light of their own invention, fixed in
    // CAMERA space, identical at noon, at midnight and underground — which is exactly what made them read as a
    // sticker over the game rather than an object in it. One definition means the row and the axe can never
    // drift apart again.
    fn heldLight(nw : vec3<f32>) -> vec3<f32> {
      let direct = sunTint() * max(dot(nw, u.sunDir), 0.0) * u.heldCfg.x * select(0.0, 1.0, LG(16u));   // bit 16: the held item's DIRECT sun term
      let skyIrr = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale();
      let bounce = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * nw.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());   // warm ground bounce on side/under faces — without it side faces are sky-only (cool + dark) and the axe handle read grey-brown
      let skyOcc = select(1.0, u.heldCfg.y, LG(1u));                 // gated on the AO debug bit so it switches WITH the world's AO rather than against it
      return direct + (skyIrr + bounce) * skyOcc + ambFloor();   // …ambFloor(), not the constant: night bit 0 takes this down with the world's, or a carried tool would float free of the ground it stands over the moment the panel deepens the night
    }
    fn vn2(p : vec2<f32>) -> f32 {
      let f = floor(p); let i = vec2<i32>(f);
      var w = p - f; w = w * w * (3.0 - 2.0 * w);
      let a = mix(ih3(i.x, i.y, 7), ih3(i.x + 1, i.y, 7), w.x);
      let b = mix(ih3(i.x, i.y + 1, 7), ih3(i.x + 1, i.y + 1, 7), w.x);
      return mix(a, b, w.y);
    }
    // ↑ MOVED UP from beside caust(): the MILKY WAY in skyColor() below clumps on it, and WGSL has no forward
    // declarations — a call above the definition is a compile error, which in this codebase is a black screen.
    // Byte-for-byte the same function; caust() still calls it from where it always did.
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
    // ── SHOOTING-STAR TUNING ── the whole of "rare enough to feel like an event". Two slots at one chance per
    // MET_PER, 55% of those chances taken, is a meteor about every 42 seconds of night: roughly ten across one
    // night of the 20-minute cycle. MET_Q is the BLOCK — 0.004 rad is about 0.23 deg, i.e. ~6 px across at 1080p
    // and a 70 deg field, which is a chunky pixel rather than a dot. Raise MET_Q and the streak gets blockier.
    const MET_PER : f32 = 32.0;  // HALVED A THIRD TIME (user 2026-08-26: "make the shooting stars 50% less common") - 16.0 -> 32.0. Same lever, same reason as both halvings below: each slot's offset is mi * MET_PER / 8, so doubling the window stretches the spacing with it and the meteors stay evenly spread instead of bunching - they simply arrive half as often. THE RATE IS READABLE OFF THE LOOP: 8 slots, one window each per MET_PER, and 80% of windows fire (the ih3 < 0.20 skip), so arrivals = 8 * 0.8 / MET_PER - one every 2.5 s somewhere in the sky at 16, one every 5.0 s at 32, and the expected number ALIVE at any instant (x MET_LIFE 1.4 s) goes 0.56 -> 0.28. Only a fraction of the sphere is ever in the 70 deg view, which is why that still reads as an event. NOTE the absolute figures in the older notes below ('every 42 seconds', 'every 2.8 minutes') do not survive that arithmetic and are wrong; the halvings they describe are right.   // HALVED AGAIN (user 2026-08-20: "cut the shooting star rate in half in the night sky") — 8.0 -> 16.0, the same lever as the 2026-08-19 halving and for the same reason: doubling each slot's window with the slot COUNT unchanged at 8 keeps them evenly spaced and simply arrives half as often. About one meteor every 2.8 minutes of night somewhere in the sky   // HALVED RATE (user 2026-08-19: "decrease the rate of shooting stars by 50%") — 4.0 -> 8.0 doubles each slot's window, so with the slot COUNT unchanged at 8 the meteors stay evenly spaced and simply arrive half as often     // window per slot. With 8 slots, a 1.4 s life and 80% of windows taken, roughly 2.2 meteors are alive somewhere in the sky at any moment — about one in five frames has one IN VIEW, because a 72-degree view is a small part of a sphere. That last factor is the whole reason the first attempt (2 slots, 46 s) was invisible: it put one meteor every 42 s ANYWHERE, i.e. perhaps one in view a minute.
    const FLARE_SUN : f32 = 0.24;                                    // the sun's own glare, per disc, four of them. 0.16 -> 0.30 to make the on-axis level constant, then -> 0.24 when that read too large. Tightening the radii does most of the shrinking; this stops the smaller discs simply concentrating the same light
    const FLARE_GHOST : f32 = 0.16;                                  // …and the axis ghosts, which SHOULD depend on where you look — that is what a lens flare is
    const MOON_FLARE : f32 = 0.12;                                   // how much of the sun's lens flare the MOON gets. It is a reflector, not a source, and at parity it read as a second sun sitting on a crescent
    const MOON_SOFT : f32 = 0.88;                                    // the moon's edge fade, in rr. Far crisper than the sun's SUN_SOFT: this is a solid body with a real edge, not a light source, and softening it would read as an out-of-focus sticker
    const MOON_CROP : f32 = 1.09;                                    // ── AND THIS IS WHY THERE WAS A BLACK RING (user 2026-08-28: "there's a black outline on the moon") ── the disc samples the photo at texture radius 1/MOON_CROP, so at 0.955 the geometric limb was reading at 1.047 of the file's half-width. MEASURED on game/assets/moon.png (384x384): the moon in it fills 0.938 of the half-width, min 0.927 — so everything past 0.938 was the photograph's own black surround, sampled as albedo and clamped to the file's black edge pixels. A black rim on the limb, all the way round. At 1.09 the limb reads at 0.917, safely inside the disc. It cost the outermost 8% of the photo and nothing else. It was always wrong; cutting the moon's bloom is what stopped hiding it
    const MOON_LL : f32 = 0.85;                                      // Lunar-Lambert blend: how much Lommel-Seeliger against Lambert. High, because the flat full moon is the thing to get right
    const MOON_GAIN : f32 = 1.74;                                    // …and the normaliser that puts the FULL moon's disc centre back at 1.0 (at alpha 0 the blend gives 0.575)
    const MOON_PHMIN : f32 = 0.42;                                   // the crescent's surface brightness against the full moon's. Not 0: the opposition surge is a brightening AT full, not a darkening everywhere else
    const MOON_EARTH : f32 = 0.055;                                  // earthshine strength on the unlit side
    const MOON_ESHINE : vec3<f32> = vec3<f32>(0.62, 0.72, 1.00);     // …and its colour: sunlight that has bounced off an ocean planet comes back blue
    const MOON_PIV : f32 = 0.52;   // the photo's own mid-tone: contrast is expanded ABOUT this, not about black
    const MOON_CON : f32 = 1.75;   // …and by this much
    const MOON_MID : f32 = 0.50;   // …landing on THIS output level. Separating the input pivot from the output mid-point is what keeps the gain from simply pushing the face into the ceiling: at PIV 0.46 / CON 2.30 with no offset, 34% of the disc clipped at 255 and the highlands went featureless — brighter, but no more legible than before. See the note at moonRGB
    const MET_LIFE : f32 = 1.40;
    const MET_Q : f32 = 0.0040;
    const MET_LEN : i32 = 30;     // LONGER (user 2026-08-19: "thin lines") — a streak reads as a line by being long against its width, not by being wide
    const MET_W0 : f32 = 0.75;     // half-width in cells at the HEAD…
    const MET_W1 : f32 = 0.05;     // …and at the tip of the tail. Linear between: see the taper in the draw below. 2.35/0.30 -> 0.75/0.05 (user: "thin lines"): the head was nearly 5 cells across and read as a block with a thread behind it
    fn skyColor(rd : vec3<f32>) -> vec3<f32> {
      var c = skyBase(rd);
      let realSun = select(u.sunDir, -u.sunDir, isMoon());            // the ACTUAL sun (u.sunDir carries the up-body; −it is the down-body). Both are continuous across the dusk/dawn swap.
      let realMoon = -realSun;                                         // the moon is the opposite point — so as the sun sets in the west, the moon RISES in the east, both drawn from their OWN elevation
      // ── IT SETS BEHIND THE HORIZON, IT DOES NOT FADE (user 2026-08-28) ── the gate used to be
      // smoothstep(-0.03, 0.06, realSun.y): a fade keyed on the SUN'S OWN ALTITUDE, so the whole disc dimmed
      // uniformly as it came down and had gone before it ever reached the skyline. A setting sun does not do
      // that; it is OCCLUDED. Keying on the RAY's elevation instead cuts the disc along the horizon line, so
      // the sun sinks into it and is progressively eaten from below — half a sun at the skyline, then a
      // sliver, then nothing, at full brightness throughout. It needs no separate cutoff: once the body is
      // more than its own radius down, no ray inside the disc is above the horizon and it draws nothing.
      // Terrain in front of it was always handled — the sky only draws where no geometry did.
      { let s = dot(rd, realSun); let up = smoothstep(-0.006, 0.006, rd.y);   // ── SUN disc: only while the sun is above the horizon; it sinks below the map at dusk and rises at dawn (no pop) ──
        // ── THE SUN IS THE MOON'S SIZE (user 2026-08-28: "make the sun as big as the moon") ── and it is
        // literally the moon's own two thresholds, copied, not a size worked out to land near it: the moon
        // disc below is smoothstep(0.999742, 0.999787, s) and so is this. Writing the same numbers is the
        // only version of "as big as" that cannot drift when one of them is next retuned.
        //   2.6031 degrees across, from 1.4239 — the earlier size, itself 50% up from the 2026-08-21 shrink.
        // For scale the real sun and moon are both ~0.53 degrees across and very nearly equal, which is why
        // matching them is the physically sensible answer as well as the asked-for one.
        // ── AND THE GLARE IS A LENS FLARE (user 2026-08-28: "its like a lens flare glare", with a reference
        // frame of the sun blooming through thin cloud) ── that reference is the CG_BLOOM path in COMPOSITE,
        // which only fires where the deck has alpha, so in clear sky the sun had nothing around it but a
        // 1.49-degree corona: a hot edge, not a glare. These two lobes put the same falloff in open sky.
        // TWO LOBES, and the split is the whole trick: for pow(cos, n) the half-power angle is
        // sqrt(2*ln2/n), so ONE exponent cannot be both a blown-out core and a wide haze. Measured profile,
        // against a sky of ~0.55 (saturation is anything reaching 1.0):
        //   n = 700  (2.55 deg)      0 deg 2.25 · 2 deg 1.60 · 5 deg 0.46   <- carries the blown-out core
        //   n =  80  (7.54 deg)      7 deg 0.26 · 12 deg 0.08 · 20 deg 0.00 <- carries the haze around it
        // Blown out to ~5 degrees, visible haze to ~12, gone by 20. The old separate corona is GONE rather
        // than kept alongside: at n = 2054 its half-power was 1.49 degrees, inside the moon-sized disc now
        // drawn over it, so it could only have added cost. cloudgen.js CG_BLOOM_TIGHT tracks the TIGHTEST
        // lobe here by design, so it moved 2054 -> 700 with it and the deck's bloom still grows out of this
        // glare rather than appearing as a ring beside it.
        // WARM AT THE HORIZON on the TRUE sun's elevation, so it reddens with the sunset band in skyBase
        // instead of staying white against an orange sky, and it rides the same up-gate as the disc.
        // BRIGHTNESS OF THE DISC IS UNTOUCHED (6.0), and so is every LIGHTING term: the sun that lights the
        // world is u.sunDir and its irradiance, computed nowhere near here. This is the sun you can SEE.
        // ── THE DISC ── a limb-darkened sphere, not a filled circle. rr is (theta/thetaR)^2 taken through
        // the small-angle identity theta^2 ~ 2(1-cos theta), so the 1-s and 1-SUN_COSR halves cancel it
        // exactly and no acos is needed; mu is then the cosine of the angle from the surface normal, which
        // is what the Hestroffer-Magnan law is written in. It reaches 0 AT the limb, so the disc ends on its
        // own and needs no smoothstep to close it.
        let rr = (1.0 - s) / (1.0 - SUN_COSR);
        // min(rr, 1.0) keeps the limb-darkening term inside the sphere it describes; past the geometric
        // limb mu holds at the floor and the smoothstep alone carries the fade out to SUN_EXT.
        if (rr < 4.0) {                                              // 4.0 is where the exponential has fallen to ~0.005 of a display level — past it there is nothing to add
          let mu = max(sqrt(max(0.0, 1.0 - min(rr, 1.0))), SUN_MUFLOOR);
          // ── SUN_DISC, AND WHY IT IS NOT 6.0 (user 2026-08-28: "when looking away from the sun it appears
          // through the clouds as a clear circle outline … let it be faded like it is when you look at it") ──
          // COMPOSITE draws the sky, this disc included, and then the deck mixes over it:
          // col = mix(col, cloud, aSky). So through cloud the disc shows (1 - aSky) x its peak, and it stays
          // CLIPPED AT PURE WHITE until that falls under 1.0. At 6.0 the peak is 21.6 linear, which needs
          // aSky > 0.954 — near-solid cloud — so through anything thinner the sun punched out as a
          // hard-edged circle no matter how soft its own edge was. That is the outline, and no amount of
          // work on SUN_FALL could have reached it: the edge was being redrawn by the compositor.
          // 6.0 -> 2.0 was not enough: at 7.2 it still needed aSky > 0.861, and the clip shows the sun crisp
          // through THIN cloud and soft only through thick. At 0.6 the peak is 2.16 and it fades from aSky 0.537,
          // which covers the thin cloud in that footage.
          // It costs little in clear sky — the white core goes from 1.25 to 0.88 of the radius and
          // the soft band actually WIDENS slightly, 7.5 px to 8.6 px — because the flare, not the disc, is
          // what carries the sun's brightness there.
          c += SUN_COL * SUN_DISC * pow(vec3<f32>(mu), SUN_LIMB) * exp(-SUN_FALL * max(0.0, rr - SUN_CORE)) * up;
        }
        // ── NO AUREOLE HERE, DELIBERATELY (user 2026-08-28: "get rid of that white brush stroke you have
        // around the sun, but the glare looks good") ── the sky-side glow this block used to add is gone.
        // Two pow lobes washed white across the sky around the disc, and however they were weighted they
        // read as a painted smear rather than as light: too strong and they saturated over the disc's own
        // edge and erased it, too weak and they were a grey haze with no shape. Either way the sun stopped
        // looking like an object.
        // THE GLARE IS NOT MISSING — IT MOVED, or rather it was always somewhere else. The layered, tinted
        // bloom that reads correctly is the LENS FLARE in blit.js: four tinted discs which, drawn centred on
        // the sun, are what the eye reads as glare. That is a LENS effect and belongs at the lens, applied to
        // the finished frame, not summed into the sky radiance here where it also becomes the colour the
        // distance haze fades toward. Adding a second glow in this file only competed with it.
        // So the sky's job is now just the disc: a limb-darkened sphere with a definite edge, and the flare
        // over the top of it. Anything tempted to add a glow back here should go to blit.js instead.
        // (cloudgen.js CG_BLOOM_TIGHT no longer has a corona in this file to match — see the note there.)
      }
      // ── THE MOON IS AN OBJECT, NOT A GLOW (user 2026-08-19: "the moon seems to be transparent, make the moon
      // completely solid") ── it used to be an ADDITIVE c += mt * ..., so whatever the sky already held
      // showed straight through it. Worse, the whole star / milky way / nebula / meteor block below runs AFTER
      // this one and adds on top, so stars came out IN FRONT of the moon. Both are fixed by the same move:
      // resolve the disc's colour and coverage here, then COMPOSITE it over everything at the end of the sky,
      // with mix() rather than +. Inside the disc the sky is replaced, which is what opaque means.
      var moonRGB = vec3<f32>(0.0);
      var moonCov = 0.0;
      // ── THE MOON, REWORKED (user 2026-08-28: "rework the moon … give it the moon phases … make the moon
      // the same size as the sun") ──
      // SIZE: SUN_COSR, the sun's own constant, so the two are identical and cannot drift. That is also the
      // physical answer — the real sun and moon are both ~0.53 degrees, which is why eclipses work at all.
      // WHY THE PHOTO IS STILL SAMPLED FLAT: moon.webp is a photograph, i.e. already the orthographic
      // projection of a sphere seen from far away, and the Moon is tidally locked so it is always the same
      // hemisphere. A flat billboard of an orthographic photo IS the correct mapping; re-projecting it onto a
      // sphere would apply the foreshortening twice and smear the limb.
      // PHASES: the game hangs the moon at the anti-solar point, so geometrically it would be full every
      // night — which is exactly why this used to be drawn full and why a phase cannot come from the moon's
      // POSITION here. It comes from the lighting instead: a virtual sun direction in the moon's own frame,
      // swung by the phase angle. alpha 0 puts it behind the observer (full), pi/2 to the side (quarter), pi
      // behind the moon (new), and the terminator sweeps across on its own. The frame's x axis is the sun's
      // bearing, so the lit limb points at where the sun actually is, which is the thing people notice.
      // REFLECTANCE — LUNAR-LAMBERT, not Lambert: lunar regolith is a fine dust that scatters from below the
      // surface, and its single-scattering term is Lommel-Seeliger, mu0/(mu0+mu). At full phase mu0 = mu
      // everywhere on the disc, so that term is 1/2 EVERYWHERE and the moon reads as a flat disc rather than
      // a shaded ball — which is what a real full moon looks like, and what plain Lambert gets wrong. The
      // accepted model is a weighted blend of the two, so that is what this is.
      { let s = dot(rd, realMoon);
        let rrM = (1.0 - s) / (1.0 - SUN_COSR);
        let up = smoothstep(-0.006, 0.006, rd.y);                    // the moon sets behind the horizon too, for the same reason the sun does
        let md = smoothstep(1.0, MOON_SOFT, rrM) * up;               // a body, so its edge is nearly hard — only enough softness to stop it aliasing
        if (md > 0.001) {
          // ── THE DISC FRAME IS ANCHORED TO WORLD UP (user 2026-08-28: "can you turn the moon 90 degrees?
          // it appears to be on its side") ── and that is the fix. Both axes used to be derived from the
          // SUN'S BEARING: TS was the component of -sunH perpendicular to the moon and B was their cross
          // product, so neither had anything to do with which way is up. MEASURED over the moon's whole arc,
          // 22 to 60 degrees of elevation: the old +y axis sat EXACTLY 90 degrees from sky-up at every one of
          // them. Not a drift — a constant quarter turn, which is precisely why it read as "on its side"
          // rather than as merely crooked. A face has an up, and it has to come from the world, not from
          // where the light happens to be.
          // R is horizontal (perpendicular to world up), U completes the frame and points up-sky at the moon.
          // GUARDED: at the zenith cross(up, moon) collapses, and a zero-length axis is a NaN disc.
          var R = cross(vec3<f32>(0.0, 1.0, 0.0), realMoon);
          let rl = length(R);
          R = select(vec3<f32>(1.0, 0.0, 0.0), R / max(rl, 1e-6), rl > 1e-4);
          let U = cross(realMoon, R);
          let du = vec2<f32>(dot(rd, R), dot(rd, U)) / SUN_ANGR;     // disc coordinates, -1..1, with +y up the SKY
          // ── AND v IS NEGATED, WHICH IS THE OTHER HALF OF "UPRIGHT" ── du.y increases with SKY-UP by
          // construction (U = cross(moonDir, R)), but a texture's v increases DOWN the image: row 0 is the
          // top. Feeding one straight into the other lands sky-up on image-bottom and draws the face
          // vertically FLIPPED. Anchoring the frame to world up fixed the 90-degree roll; this fixes the
          // flip that was underneath it. MEASURED by correlating the rendered disc against moon.png under
          // all eight orientations: mirror+180 (i.e. a pure vertical flip) scored r = 0.545 against 0.387
          // for the runner-up — the only reading with a real gap under it.
          let uv = clamp(vec2<f32>(du.x, -du.y) * (0.5 / MOON_CROP) + vec2<f32>(0.5), vec2<f32>(0.0), vec2<f32>(1.0));
          let mt = textureSampleLevel(moonTex, moonSamp, uv, 0.0).rgb;
          let r2 = clamp(dot(du, du), 0.0, 1.0);
          // ── THE SURFACE NORMAL ── +z points at the observer, so this is the visible hemisphere of a unit
          // sphere. Everything below is ordinary surface shading in that frame.
          let N = vec3<f32>(du.x, du.y, sqrt(max(0.0, 1.0 - r2)));
          let alpha = u.rdist.y * 6.2831853;                         // the 8-day phase, already tracked in JS as (moonDay + tday) / 8
          // ── THE TERMINATOR SWINGS ALONG R, WHICH IS HORIZONTAL, SO THE TERMINATOR IS VERTICAL ── and
          // this is the whole of "the moon is on its side". Projecting the sun's bearing onto the disc looks
          // like the physical answer and is a trap here: the game hangs the moon at the ANTI-SOLAR point, so
          // the sun's bearing lies exactly in the moon's own vertical plane. R is perpendicular to that plane
          // by construction, so dot(sunH, R) is identically ZERO and the projection collapses onto U — the
          // light swung up-and-down and the terminator came out HORIZONTAL, a moon lit from below.
          // There is no honest "toward the sun" direction on an anti-solar disc; the phase here is synthetic
          // and its orientation has to be chosen. R is the choice: horizontal, so the lit limb is a left or
          // right crescent and the terminator is vertical, which is what a moon actually looks like.
          let Ld = vec3<f32>(sin(alpha), 0.0, cos(alpha));
          let mu0 = dot(N, Ld);                                      // cos incidence
          let mu = N.z;                                              // cos emission — the observer is straight down +z
          let lsT = max(mu0, 0.0) / max(mu0 + mu, 1e-3);             // Lommel-Seeliger
          let lit = clamp((MOON_LL * lsT + (1.0 - MOON_LL) * max(mu0, 0.0)) * MOON_GAIN, 0.0, 1.0);
          // ── EARTHSHINE ── the dark side is not black: it is lit by a full Earth, about 1/10000 of direct
          // sunlight but easily visible next to a thin crescent, and it is BLUE because it is sunlight that
          // has already bounced off an ocean-and-cloud planet. It is what makes "the old moon in the new
          // moon's arms" the familiar sight it is, and without it a crescent reads as a broken disc.
          // ── PHASE BRIGHTNESS (user 2026-08-28: "make the brightness of the moon match what phase the
          // moon is in") ── the lit AREA already shrinks with the phase; this is the surface brightness on
          // top of it. A real moon is disproportionately bright at full — the opposition surge, regolith
          // backscattering straight at the observer — so a half moon is nowhere near half as bright as a
          // full one. phaseF is the illuminated fraction, (1 + cos alpha) / 2.
          let phaseF = moonPhaseF();                                 // …the same fraction the key light and the deck's bloom use
          let phaseB = mix(MOON_PHMIN, 1.0, phaseF);
          // ── EARTHSHINE RUNS THE OTHER WAY, and that is not a mistake ── it is sunlight bounced off the
          // EARTH, and from the moon the Earth shows the opposite phase: it is full when the moon is new.
          // So the ashen glow is at its strongest on the thinnest crescent, which is exactly when anyone
          // ever notices it.
          let earth = MOON_EARTH * (1.0 - lit) * mix(0.35, 1.0, 1.0 - phaseF);
          // ── CONTRAST, OR THE CRATERS ARE NOT THERE (user 2026-08-19) ── the photograph is a faithful full
          // moon and that is the problem: lit straight down the line of sight there are no shadows in it and
          // the maria differ from the highlands by very little. Expand about the disc's own mid-tone; the
          // pivot matters more than the gain, since expanding about 0 would only brighten and clip.
          let mlum = max(max(mt.r, mt.g), mt.b);
          let mcon = clamp((mlum - MOON_PIV) * MOON_CON + MOON_MID, 0.02, 1.0);
          let mtint = mt / max(mlum, 0.001);                          // keep the photo's own near-neutral hue, drive only the level
          let limb = 0.86 + 0.14 * sqrt(1.0 - r2);                    // a gentle roll so the edge does not read as a cut-out sticker
          // ── THE DARK SIDE CARRIES THE TEXTURE TOO (user 2026-08-28: "the part of the moon that's dark is
          // completely blank, put the moon texture on the blank side as well") ── the earthshine term used to
          // be a flat colour, so the unlit part was a featureless blue disc. It is the same surface: the same
          // maria and highlands are there, just lit by a different and far fainter source, so it takes the
          // same albedo. Multiplying by the photo is both what looks right and what is physically true.
          let face = mtint * mcon;                                     // the moon's own albedo, contrast-expanded once and used by BOTH lighting terms
          moonRGB = clamp(face * limb * lit * phaseB + MOON_ESHINE * face * earth, vec3<f32>(0.0), vec3<f32>(1.0));
          moonCov = md;                                               // OPAQUE across the whole disc, lit or not — the dark limb still hides the stars behind it
        }
      }
      let night = 1.0 - smoothstep(-0.16, 0.02, realSun.y);          // stars fade in on the real sun elevation — continuous across the swap (no pop)
      // ── THE HARD LINE ACROSS THE SKY (user 2026-08-19: "there also seems to be a cutoff in the nightsky") ──
      // this gate used to be rd.y > 0.02, so every pixel below about one degree of elevation got NO stars, no
      // band and no meteors while the pixel just above it got all three. That is a horizontal seam straight
      // across the lower sky, and it is the cutoff. The gate now reaches below the horizon and hz fades the
      // whole set out smoothly instead — which is also what the real sky does, since atmospheric extinction
      // thickens towards the horizon and stars die out before they reach it.
      let hz = smoothstep(-0.02, 0.17, rd.y);
      if (night > 0.02 && rd.y > -0.02) {                            // ── EVERYTHING THAT ONLY EXISTS AT NIGHT ── stars, the milky way, the meteors, all behind ONE compare on a value that is exactly 0 for the whole of the day. A day sky pixel pays that compare and nothing else, which is what makes the night panel free at noon.
        // ── THE MILKY WAY, BAKED IN (user 2026-08-19: "bake in the fog in the night sky", then "I meant remove
        // the milky way from the PANEL") ── the band itself stays exactly as it was; only its switch is gone.
        // MW_N is the band's POLE, so dot(rd, MW_N) is 0 along its centre line. Tilted well off vertical so the
        // band climbs out of the horizon at an angle rather than ringing the sky like a hoop, and written out
        // ALREADY NORMALISED because a const initialiser must be a const-expression and not every driver
        // const-folds normalize(). Bit 2 is retired; nothing reads the band value outside this block any more, because
        // the star threshold stopped leaning on it when the band stopped seeding stars.
        const MW_N : vec3<f32> = vec3<f32>(0.3603, 0.7807, -0.5105);
        const MW_T : vec3<f32> = vec3<f32>(0.8171, 0.0, 0.5767);      // = normalize(cross(MW_N, up))
        const MW_B : vec3<f32> = vec3<f32>(0.4502, -0.6250, -0.6379); // = cross(MW_N, MW_T)
        {
          let mwd = dot(rd, MW_N);
          let mwp = vec2<f32>(dot(rd, MW_T), dot(rd, MW_B));          // an in-band coordinate to clump on
          let band = exp(-mwd * mwd * 18.0);                          // about 14 degrees to half strength — widened from 24 (user 2026-08-20) so the band blends into the dome haze below rather than sitting in a ring of nothing
          let clump = 0.45 + 0.40 * vn2(mwp * 4.7) + 0.30 * vn2(mwp * 12.3);
          let rift = 1.0 - 0.60 * smoothstep(0.30, 0.78, vn2(mwp * 2.9 + vec2<f32>(11.3, 4.7)));   // the dark dust lane that splits the real band down its length
          c += vec3<f32>(0.0205, 0.0216, 0.0300) * band * clump * rift * night * hz;   // the "fog": ~3.4x its original strength (user 2026-08-19: "add more fog")
          // ── AND A BROAD HAZE OVER THE WHOLE DOME (user 2026-08-20: "add more fog to the night sky, there
          // only seem to be a couple of areas with the fog") ── the fog up to now WAS the band above, and a
          // band is by definition a couple of areas: it is exp(-mwd^2 * 18) — about 14 degrees to half
          // strength along ONE great circle — so most of the sky carried none at all. This is the same haze
          // colour spread across everything at about a third of the band's peak, so the band stays the
          // brightest thing up there and the rest of the sky stops being flat black between the stars.
          // SAMPLED IN THE BAND'S OWN TANGENT FRAME. MW_T/MW_B project a unit direction into the unit disc,
          // which is seam-free everywhere; an azimuth/elevation parameterisation would draw a hard line down
          // the sky at +/-pi, which is exactly the kind of cutoff the 2026-08-19 horizon fix removed. The disc
          // maps the two hemispheres onto each other, so mwd — the signed distance off the band plane — is
          // folded into both octaves' coordinates to stop the pattern mirroring itself across the band.
          let hzc = vec2<f32>(dot(rd, MW_T), dot(rd, MW_B));
          let haze = 0.35 + 0.45 * vn2(hzc * 2.1 + vec2<f32>(mwd * 3.7, -mwd * 2.9))
                          + 0.30 * vn2(hzc * 5.3 + vec2<f32>(-mwd * 2.1, mwd * 4.3));
          c += vec3<f32>(0.0205, 0.0216, 0.0300) * 0.33 * haze * night * hz;
        }
        // ── AND IT NO LONGER SEEDS STARS (user 2026-08-19: "the milky way seems to add more stars to the sky,
        // prevent that from happening") ── the threshold used to fall inside the band (0.9985 - 0.0011 * band)
        // so the band carried a denser star field as well as its glow. That was deliberate once, on the theory
        // that a real band IS unresolved stars, but it is not what was wanted: the band now contributes ONLY
        // its haze, and the star field is uniform across the whole sky whether the band is on or off.
        let thr = 0.9985;
        let cc = vec3<i32>(rd * 480.0);
        let hh = ih3(cc.x, cc.y, cc.z);
        if (hh > thr) {
          // ── TWINKLE (night bit 3) ── on a SECOND hash of the same cell. Not on hh: hh only spans the 0.0015
          // above the threshold, so every star would share one phase and the whole sky would pulse together.
          // ── AND IT HAS TO CLEAR A NOISE FLOOR IT DOES NOT SET (user 2026-08-19: "star twinkle doesnt seem to
          // do anything") ── it was +/-28% at 0.14-0.5 Hz, held down out of a fear of blinking pixels. MEASURED
          // why that reads as nothing: TAA jitters the camera sub-pixel every frame, so the cell index flips between
          // neighbouring cells and a star pixel ALREADY varies with a temporal standard deviation of 7.8/255
          // with twinkle switched off entirely. Switched on at 28% the figure was 7.9 — the signal sat under
          // the jitter. Proved it was wiring-clean rather than dead by amplifying it to +/-50% at 8 rad/s,
          // which took the same measurement to 35.8. So the amplitude is the whole bug: +/-55% against a
          // mean-66 star is about +/-36, roughly 3x the floor, and 0.35-1.0 Hz stays far slower than TAA's
          // 8-frame (~0.13 s) window, so it resolves as a shimmer rather than half-averaging into a flicker.
          let tp = ih3(cc.x + 17, cc.y + 5, cc.z - 23);
          // ── max(0), AND THAT IS THE BLACK FLICKER (user 2026-08-28: "the stars that are flickering are
          // flickering black; make them disappear, not flicker black") ── 0.45 + 0.55 * sin spans -0.10 to
          // 1.00, and the line below ADDS tw to the sky. For the tenth of its swing that sat below zero the
          // star was subtracting, punching a hole DARKER than the sky behind it — a black star, which is why
          // it read as a flicker to black rather than a fade out. Clamping the trough at zero leaves the
          // curve alone and lets the star simply go out.
          let tw = max(0.0, 0.45 + 0.55 * sin(u.time * (0.75 + tp * 1.35) + tp * 62.83));   // BAKED IN (user 2026-08-19: "bake in the star twinkle and remove it from the panel") — it was night bit 3, and the select() with it   // SLOWER (user 2026-08-19: "make the star twinkle slower") — 2.2-6.2 rad/s was a 1.0-2.9 s period and read as busy; 0.75-2.10 is 3.0-8.4 s. The AMPLITUDE is what clears TAA's jitter floor, not the rate, so slowing it costs no visibility
          c += vec3<f32>(0.9, 0.93, 1.0) * (hh - thr) * 1000.0 * night * tw * hz;   // 2x (user 2026-08-19: 'double the brightness'). It is the TERM that doubles, not the pixel: aces() and the 1/2.2 encode downstream absorb it, so the brightest star moves up without ever reaching 255 - a star that saturates loses its colour and its size cue, which is the failure mode the moon disc hit
        }
        // ── SHOOTING STARS (night bit 5, user 2026-08-19: "throw in some pixelated shooting stars") ──
        // STATELESS. Every property of an event is hashed from the index of the window it falls in, so every
        // pixel and every frame agree on it without a byte of storage, a buffer or a JS tick — and one survives
        // a reload of the page mid-flight. Two slots, half a period out of step so they cannot land on top of
        // each other; about 55% of windows fire, so the sky is empty far more often than not.
        // ── MORE OF THEM (user 2026-08-19: "I dont see any shooting stars in the night sky. maybe increase the
        // count") ── 2 slots at MET_PER 46 with 55% taken was one meteor every ~42 s of night, i.e. perhaps ten
        // in a whole night and easily none in the minute anybody happens to look up. 5 slots evenly spaced
        // across a 17 s window is about one every 3 s: frequent enough to be a feature you notice rather than
        // one you have to be told about, and still nothing like a continuous stream.
        {                                                        // BAKED IN (user 2026-08-19: "disable the panel completely on the l toggle") — it was night bit 5, the last switchable one; with no panel there is nothing left to read a mask, so the feature is simply on
          for (var mi = 0; mi < 8; mi = mi + 1) {
            let off = f32(mi) * MET_PER / 8.0;
            let ei = floor((u.time + off) / MET_PER);
            let k0 = i32(ei) * 7919 + mi * 104729;
            if (ih3(k0, 3, 11) < 0.20) { continue; }
            let tau = u.time + off - ei * MET_PER - ih3(k0, 7, 29) * (MET_PER - MET_LIFE);   // somewhere inside the window, never on the beat
            if (tau < 0.0 || tau > MET_LIFE) { continue; }
            let el = 0.22 + ih3(k0, 13, 5) * 0.62;                    // where it comes in: clear of the horizon haze, clear of the zenith
            let az = ih3(k0, 23, 41) * 6.2831853;
            let ce = cos(el);
            let A = vec3<f32>(ce * sin(az), sin(el), ce * cos(az));
            let R = normalize(cross(A, vec3<f32>(0.0, 1.0, 0.0)));
            // ── AND THEY DO NOT ALL FALL STRAIGHT DOWN (user 2026-08-20: "the shooting stars seem to go
            // downwards most of the time. make it go horizontal more often as well") ── this was
            // down + R * (+/-0.8), i.e. a unit vertical with a bounded sideways nudge, so the steepest possible
            // path was still atan(0.8) = 39 degrees off vertical and EVERY meteor read as falling. Choosing the
            // ANGLE instead of a lateral offset spreads them evenly from near-vertical to near-horizontal:
            // +/-1.25 rad is +/-72 degrees, so a good half of them now cross the sky rather than drop down it.
            // cos stays positive over that range, so none of them travels upward — they are still meteors.
            let lean = (ih3(k0, 37, 53) * 2.0 - 1.0) * 1.25;
            var D = vec3<f32>(0.0, -cos(lean), 0.0) + R * sin(lean);
            D = normalize(D - A * dot(D, A));                         // re-seated into the plane through A, so the path is a great circle and the rate below is an honest angular speed
            let Bm = cross(A, D);
            let thH = (0.42 + ih3(k0, 59, 67) * 0.34) * tau;          // where the HEAD is now, as an angle from A
            // ── THE BLOCKS ── the streak is deliberately NOT a line. rd is resolved into the path's own frame
            // and then SNAPPED to a grid of MET_Q-radian cells, so what reaches the screen is a chain of
            // hard-edged squares stepping across the sky. 10 cm voxels on the ground and the same discipline
            // overhead (user: "pixelated"); a smooth antialiased streak is the thing this must not be.
            let qb = floor(dot(rd, Bm) / MET_Q + 0.5);                // across the path, in whole cells
            let qa = floor(atan2(dot(rd, D), dot(rd, A)) / MET_Q + 0.5);   // and along it
            let kb = i32(round((thH - qa * MET_Q) / MET_Q));          // how many cells BEHIND the head this one is
            if (kb < 0 || kb >= MET_LEN) { continue; }
            // ── THE WIDTH TAPERS, IT DOES NOT STEP (user 2026-08-19: "have the shooting star have a smooth
            // linear transition from the head of the star to the tail. it looks like the head is just a block,
            // with the tail being much thinner") ── it was a two-state test: three cells across while kb < 2,
            // one cell after, i.e. a blunt block that dropped to a hairline at a hard edge two cells in. The
            // half-width now falls LINEARLY from MET_W0 at the head to MET_W1 at the tip, and the comparison is
            // against that continuous value, so the silhouette narrows a cell at a time down its length.
            // Still snapped to the MET_Q grid, so it stays a chain of blocks rather than becoming a smooth
            // antialiased wedge — "pixelated" was the original ask and the taper must not undo it.
            let tfrac = f32(kb) / f32(MET_LEN);
            let hw = MET_W0 + (MET_W1 - MET_W0) * tfrac;
            if (abs(qb) > hw) { continue; }
            if (ih3(i32(qa), 909, k0) < 0.10 + 0.34 * f32(kb) / f32(MET_LEN)) { continue; }   // and cells drop out down the tail, so it frays instead of stopping on a ruler line
            let fade = pow(1.0 - tfrac, 2.4) * (0.35 + 0.65 * clamp((hw - abs(qb)) * 1.6, 0.0, 1.0));   // ── BRIGHT AT THE HEAD, DIM AT THE TAIL (user 2026-08-19, with a reference image) ── the exponent is the gradient: 1.5 held most of the streak near full and then fell off a cliff, 2.4 falls away from the head immediately so the head reads as the source and the tail as what is left of it. The second factor dims ACROSS the line too, so a one-cell-wide tail still has a soft edge rather than a hard one
            let lf = smoothstep(0.0, 0.08, tau) * (1.0 - smoothstep(MET_LIFE * 0.70, MET_LIFE, tau));   // strikes in, burns out
            c += vec3<f32>(1.00, 0.26, 0.030) * (3.1 * fade * lf * night * hz);   // BRIGHT ORANGE (user 2026-08-19). 9.0 was too hot to BE orange: aces() desaturates highlights, so the head clipped to near-white (measured ~255/250/195) and only the dim tail kept any hue — the opposite of the ask. 3.1 sits under that knee, so the whole streak stays orange while still reading as emissive against a sky whose stars peak near 96.   // BRIGHT ORANGE AND EMISSIVE (user 2026-08-19). The near-white 1.0/0.95/0.86 was the colour of a real meteor and read as another star; this is unmistakably orange. 2.3 -> 9.0 so the head clips to white through aces() while the tail stays orange, which is what "emissive" looks like on this pipeline — a hot core in a coloured envelope, exactly how the lava and the sparks are drawn
          }
        }
      }
      c = mix(c, moonRGB, moonCov);                                  // ── THE DISC GOES ON LAST ── over the stars, the milky way, the nebulas and the meteors, so none of them can be seen through it. mix() and not +=: inside the disc the sky is REPLACED, which is what "solid" means.
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
    // ══ RIPPLE RINGS ══ what a splash and a wake both look like to the surface: a
    // crest that leaves a point and spreads. Returns BOTH answers in one pass because the caller needs both
    // and this runs inside the wave march — x is the height added to the Gerstner sum (so the ring is real
    // stepped geometry, not a decal), y is how much foam the crest carries.
    // The loop BREAKS at the first empty slot rather than skipping it: world/window.js keeps the list
    // compacted, so this costs the number of rings actually alive, which is usually zero. That matters here
    // and nowhere else — every other water term is evaluated once per pixel, and this one is evaluated once
    // per column the surface march steps through.
    const RIP_SPD : f32 = ${RIP_SPD};
    const RIP_LIFE : f32 = ${RIP_LIFE};
    const RIP_AMP : f32 = ${RIP_AMP};
    const RIP_W : f32 = ${RIP_W};
    fn ripHF(wx : f32, wz : f32) -> vec2<f32> {
      var h = 0.0; var fo = 0.0;
      for (var i = 0; i < ${RIP_N}; i++) {
        let r = u.ripple[i];
        if (r.w <= 0.0) { break; }                                   // compacted: nothing live past here
        let age = u.time - r.z;
        if (age < 0.0 || age > RIP_LIFE) { continue; }               // a straggler the CPU has not retired yet
        let e = (length(vec2<f32>(wx, wz) - r.xy) - age * RIP_SPD) / RIP_W;
        let g = exp(-e * e) * (1.0 - age / RIP_LIFE);                // …fading as it spreads, so a wake thins out behind you instead of ending on a line
        h += r.w * RIP_AMP * g;
        fo = max(fo, r.w * g);                                       // foam takes the STRONGEST ring, not the sum: two rings crossing make one white crest, not a doubly white one
      }
      return vec2<f32>(h, fo);
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
    // == SAND, ON THE G-BUFFER == the composite needs to know a sand TOP face to glisten it (user 2026-08-15:
    // "make the sand glisten from the sun like the water"), and the only channel it has is gAlbedo.a. That byte
    // is (faceId | lavaG << 4): faceId runs 0-5 face, 6 water, 7 sky, 8 lava, and lavaG is a FOUR-bit 0-14 glow
    // field, so bits 4-7 are NOT spare -- the room is in the low nibble, at faceId 9-15. SANDF is a top face
    // (composite maps it straight back to 2u for faceN, and gbFace does the same for the denoiser's edge test),
    // so nothing downstream can tell the difference except the code that asks.
    const SANDF : u32 = 9u;
    fn isSandV(v : u32) -> bool { return ${[...SAND, ...DSAND].map((i) => 'v == ' + i + 'u').join(' || ')}; }   // beach/lakebed SAND + desert DSAND -- ids listed one by one rather than as a range, so a future palette reorder cannot silently widen it
    // ── WHICH IDS ARE A CACTUS ── built the way isSandV is, from the ids the loaded models actually
    // reference rather than a hand-written list, so a re-bake or a palette shift cannot leave it stale. The
    // trailing 'or false' keeps the expression valid WGSL if the cacti failed to load and the set is empty.
    fn isCactusV(v : u32) -> bool { return ${[...new Set(CACTI.flatMap((c) => c.vox.map((q) => q >>> 24)))].map((i) => 'v == ' + i + 'u').join(' || ') || 'false'}; }
    // ── WHICH IDS ARE STONE ── the sun sheen in COMPOSITE asks this once per pixel, through bit 12 of the slot
    // word (see slotOut in TRACE). Generated from rockShTab, which material-tabs.js fills from the palette
    // constants AND from the ids the loaded boulder models actually reference — the same discipline isSandV
    // and isCactusV follow, so a re-bake or a palette shift cannot leave this list stale. Written out one id
    // at a time rather than as a range for the reason isSandV gives: a future palette reorder must not be able
    // to silently widen it onto dirt. The trailing 'or false' keeps the expression valid WGSL if the table is
    // somehow empty (a failed rocks26 fetch leaves only the terrain strata, which is already 12 ids, but the
    // guard costs nothing and an empty return would be a compile error, i.e. a black screen).
    fn isRockV(v : u32) -> bool { return ${[...rockShTab].map((f, i) => (f ? i : -1)).filter((i) => i >= 0).map((i) => 'v == ' + i + 'u').join(' || ') || 'false'}; }
    fn gbFace(a : f32) -> u32 { let r = u32(a * 255.0 + 0.5) & 15u; return select(r, 2u, r == SANDF); }   // ...the plain face, sand folded back into TOP: the denoiser rejects a neighbour whose face differs, and without this a sand/grass boundary would stop sharing irradiance samples
  `;

