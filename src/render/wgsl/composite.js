  const COMPOSITE_SRC = ({ DDAW, pickWGSL }) => /* wgsl */`
    @group(0) @binding(1) var gAlbedo : texture_2d<f32>;
    @group(0) @binding(2) var irrF : texture_2d<f32>;
    @group(0) @binding(3) var colorOut : texture_storage_2d<rgba8unorm, write>;
    @group(0) @binding(16) var<storage, read> pool : array<u32>;    // ── PAGED BRICK POOL ── the same two buffers TRACE reads, under the same names, because DDAW is shared source: creature pixels trace a REAL sun ray
    @group(0) @binding(17) var<storage, read> bdesc : array<u32>;   // per-brick: 0 = all air, else slot+1. A nonzero descriptor IS the occupancy bit, so this replaces the bitmask here exactly as it does in TRACE
    @group(0) @binding(18) var<storage, read> bricks2 : array<u32>;
    @group(0) @binding(19) var<storage, read> pal : array<vec4<f32>>;   // palette — the traced water reflection/refraction shades its hits here
    @group(0) @binding(20) var<storage, read> wbricks : array<u32>;     // water-only brick bits — skipW rays stride these
    @group(0) @binding(23) var<storage, read> bodyVox : array<u32>;  // ── RIGID BODIES ── shared with TRACE; DDAW's bodyTrace() reads it
    @group(0) @binding(21) var slotT : texture_2d<u32>;             // ── DYNAMIC LIFE ── TRACE's per-pixel creature id + hit-axis bits (see slotOut) — true-normal reconstruction + debug views
    ${DDAW}
    ${pickWGSL}
    // ── DEPTH OF FIELD ── the signed circle of confusion for a surface d voxels away. Written into colorOut.a
    // and gathered by the BLIT. Thin-lens shape (1 - focus/d): exactly 0 on the focal plane, saturating at +1
    // however far past it you go — the sky included — and going negative in front of it. The dead zone holds a
    // band either side of the focus perfectly sharp, so the picture has somewhere to BE in focus rather than
    // being softened everywhere by a fraction of a pixel; and the near side is scaled down because a lens blurs
    // the foreground far harder than the background, which at voxel scale reads as a smear over half the screen
    // rather than as depth.
    const DOF_DEAD : f32 = 0.16;                                     // fraction of the 0..1 range that stays sharp either side of the focal plane
    const DOF_NEAR : f32 = 0.55;                                     // foreground blur as a fraction of the background's
    // ── THE SAND GLINT IS A CHROMA EFFECT, NOT A BRIGHTNESS ONE ── (user 2026-08-16: "rework the sun
    // reflections on the sand. it just looks white.") Everything below is measured in the real desert with
    // the camera aimed analytically down the MIRRORED sun (yaw atan2(sun.x, sun.z), pitch -asin(sun.y)) —
    // the glint exists only inside that column, so a framing chosen for "most sand on screen" proves nothing.
    //
    // WHY IT LOOKED WHITE, and it is not the obvious answer: the spark was never clipping. ACES compresses the
    // biggest channel hardest, so a spark that multiplies all three roughly evenly comes OUT of the curve LESS
    // saturated than the sand it sits on — the lift is a wash toward grey. Measured on the version this one
    // replaced: sand saturation 0.369 -> glint 0.184 at noon, with 34.5% of the lit pixels landing inside 12%
    // of neutral, and of the 400 pixels it changed most, exactly ONE came out warmer than the sand under it.
    //
    // …and why GAIN cannot fix it. Sunlit sand at noon already renders sRGB 190-243: a 3.4x spark on a softer
    // tint (1.30/1.00/0.55) was tried and measured WORSE than the version being replaced — 60.7% of its lit
    // pixels near-neutral, because all the extra energy went into the channel that had room, which is blue.
    // The tonemapper has no headroom left for brightness up here. It has plenty for COLOUR.
    //
    // So the spark is tinted hard toward the sun instead of lifted: R x3.8, G x2.6, B x0.94 — blue is the one
    // channel it does NOT raise. Measured after: at a low sun the lit cells go 0.185 -> 0.237 saturation
    // (it RISES above the sand now), R:B 1.223 -> 1.310, blue flat to within a unit (181.0 -> 181.2) while red
    // and green climb 16, and 1.5% near-neutral instead of 13.4%. At noon 0.204 -> 0.191 and 11.6% neutral.
    // It reads as warm sunlight on sand, which is the one thing a snow-white dot can never be mistaken for.
    const SAND_GLINT : bool = false;                                 // the sun glisten on sand: OFF (user 2026-08-16)
    const SAND_GAIN : f32 = 2.6;                                     // how far above the sand under it a lit cell sits, before the tint. Deliberately modest: past ~2.6 the curve returns luminance the eye cannot see and blue the eye reads as white — see the 3.4x control above
    const SAND_TINT : vec3<f32> = vec3<f32>(1.45, 1.00, 0.36);       // the sun's own bias, exaggerated to survive the tonemapper. B BELOW 1 ON PURPOSE: 0.36 x 2.6 = 0.94, so the spark leaves blue where it found it and the cell reads gold against cream instead of white against cream
    // ── AND THE SAME COMPLAINT AT NIGHT ── the moon branch is a FLOOR, not a keyed spark: below a certain
    // light there is no sand colour left to tint, so a fixed value has to take over or the glint dies with
    // the sun. It was 0.52/0.53/0.59, inherited from the liquid glint, and on water that is right because
    // water is dark. On sand it measured as the worst case in the whole effect: moonlit sand renders sRGB
    // 56/52/42 and the floor put the lit cells at 196 — BRIGHTER, in absolute terms, than the noon glint, and
    // near-neutral with it. A field of white dots on dark ground is a snowfield, which is the exact reading
    // the user has objected to twice — measured 62.0% of the lit pixels inside 12% of neutral, and the
    // brightest tenth of them at 157/158/162, saturation 0.03. Quartered, and biased COOL rather than
    // neutral (B/R 1.38, was 1.13). After: 8.9% near-neutral, lit saturation 0.104 -> 0.179, and the lit
    // cells sit at luminance 73 against the daytime spark's 201 — a third of it, where they were half
    // again as bright. What is left is a dim blue-silver glitter path that cannot read as a snowfield.
    const SAND_MOON : vec3<f32> = vec3<f32>(0.13, 0.14, 0.18);
    // ── ONE CELL IN FIVE, NOT ONE IN TWO ── the density is half the complaint. The water's hash keeps HALF the
    // cells, and at the shared 0.30-0.85 duty window that is about a third of the sand lifting at once: on water
    // it reads as points because the base is dark, but on bright sand a third of the surface rising together IS
    // the white. MEASURED at the same colour, 0.50 against 0.80 at noon: the lit cells FUSE — 77 blobs averaging
    // 129 px against 266 blobs averaging 24 px — and near-neutral pixels double, 11.6% -> 23.5%. Thinning is not
    // a brightness cut; the surviving cells are identical, they simply have sand between them again.
    // The TEMPORAL window is deliberately left alone at 0.30-0.85: narrowing it is what made the water read as
    // "it changes its pattern, removes itself, then changes again" (user 2026-08-05). Population is the safe lever.
    const SAND_PICK : f32 = 0.80;
    // ══ SUN REFLECTION ON STONE ══ (user 2026-08-16: "can you give the rock a reflection property from the
    // sun?" … "all of the rocks") A ROCK IS NOT WATER, AND THIS IS DELIBERATELY NOT THE GLINT ABOVE.
    // Everything the water, the ice and the sand wear is one effect: a world-space XZ cell grid, one lit cell
    // per 10 cm voxel, twinkling on a sine phase. That is a model of a surface made of INDEPENDENT MOVING
    // PIECES — wavelets, frost facets, grains — and a boulder is the opposite of that: one solid mass whose
    // faces are metres across. Three concrete reasons it was not reused:
    //   * the cell grid is indexed on (x, z), so on a VERTICAL rock face every cell in a column collapses to
    //     one value and the twinkle degenerates into stripes. Water and sand are top faces only; stone is not.
    //   * the twinkle is a TIME function. A boulder that sparkles while you and it both stand still reads as
    //     an enchanted object, not as a rock, and "white dots on a dark surface" is the exact reading the user
    //     has now objected to twice on sand (see SAND_MOON).
    //   * a specular on stone is view-dependent, not time-dependent. It has to arrive because YOU moved.
    // So this is an ordinary specular lobe: the sun's mirror image off the face, smeared by roughness.
    // WHY IT IS ADDED AND NOT MIXED, WHICH IS THE WHOLE COLOUR ARGUMENT. The sand work established that ACES
    // compresses the largest channel hardest, so a near-uniform brightness MULTIPLY leaves the curve less
    // saturated than it entered — a wash toward white. On sand the answer was to multiply by a tint whose blue
    // channel sits below 1. On stone that answer is not available: rock is NEUTRAL (PEBBLE is literally
    // r = g = b; ROCK is 124/122/116), so there is no chroma in the surface to exaggerate and col * gain * tint
    // just paints the tint's own hue on at the rock's own luminance. But a specular does not multiply albedo in
    // the first place — for a dielectric the reflectance at the surface is achromatic, so the highlight carries
    // the ILLUMINANT'S colour, not the material's. sunTint() already IS that colour, and it is (1.0, 0.42, 0.18)
    // amber at a low sun and cool blue-silver under the moon. Adding it to a neutral grey therefore RAISES
    // saturation by construction, which is the one thing gain can never do, and it is free: no new constant
    // decides the hue, the sun does, so the effect tracks sunrise, noon and moonlight without a second table.
    const ROCK_SHEEN : bool = true;                                  // the sun sheen on stone; the live switch is LG2 bit 0 (__vb.lgt2(0) / __vb.lgt2(1))
    const ROCK_GLOSS : f32 = 18.0;                                   // Phong exponent. NOT the water's 26: that is a glitter point, this is a glare that has to survive being looked at from a few degrees off. Below about 10 it stops being a reflection OF anything and just brightens every sunward face at once
    const ROCK_SPEC : f32 = 0.22;                                    // strength at normal incidence, with the lobe normalisation folded in — at the peak of the lobe it roughly doubles a sunlit rock, which is what a glare off stone does
    const ROCK_GRAZE : f32 = 2.5;                                    // Fresnel: how much stronger the sheen is edge-on than face-on. Real stone goes far above 2.5x, but the geometry here is voxels and every silhouette is a hard 90-degree edge, so the honest number blew the rim out
    // The two tints are a bias ON TOP of sunTint(), not a replacement for it. At noon sunTint is only mildly
    // warm (3.60/3.24/2.74, R:B 1.31) while lit grey rock is warmer than that already (R:B about 1.49), so a
    // raw additive sun would have pulled the highlight very slightly toward neutral — the sand failure mode
    // again, at a fraction of the size. A modest golden bias puts the added light at R:B 2.16 instead, so the
    // lit pixels come out warmer than the rock they sit on at EVERY sun elevation, not only at sunset.
    const ROCK_TINT : vec3<f32> = vec3<f32>(1.18, 1.00, 0.72);
    // …and the moon needs the opposite bias, because moonlight is the one light in this game that is cool.
    // sunTint() already returns a blue for it; carrying the day's golden bias across would have cancelled that
    // almost exactly (0.40/0.50/0.78 x 1.18/1.00/0.72 is a flat grey), which is how a night highlight turns
    // into white static. There is no jump at the dusk/dawn swap despite the select: BOTH branches of sunTint()
    // are gated by a smoothstep on the light's own elevation and reach zero at the horizon, so at the moment
    // isMoon() flips, the term this tints is already nothing.
    const ROCK_MOONT : vec3<f32> = vec3<f32>(0.85, 0.95, 1.18);
    // -- THE FLOATING HEARTS -- (user 2026-08-15: "use this for the hearts. have this voxel float in front of the
    // players screen") every number the health readout is drawn with. They live up here, as constants, rather
    // than in the uniform, because only two things about the hearts actually change while the game runs -- how
    // much health is left and whether you were just hit -- and both of those already have a lane in u.heartC.
    //
    // -- THE LIGHT IS THE WORLD'S, THE SAME ONE THE STONE TOOLS GET -- (user 2026-08-16: "the hearts shouldnt
    // be in html. they need to be actually in the game. like the stone tools for example.") They never WERE
    // html, and that is the point of the note: what the user is describing is the LOOK. The row used to carry a
    // key light of its own invention, fixed in CAMERA space, deliberately untouched by sunDir, dayScale,
    // irradiance or heldCfg -- so it painted the identical picture at noon, at midnight and underground. That is
    // what a UI layer does, and no amount of per-face shading rescues an object whose light does not belong to
    // the scene it is standing in. The row now calls heldLight() in PRE, the ONE view-model shading model, which
    // is literally what the axe in the other hand is shaded by.
    //
    // -- ...AND WHY IT IS STILL READABLE AT MIDNIGHT -- the tension the old constant light existed to dodge is
    // real: the axe DOES go near-black in a gorge, and a health readout may not. The answer is not a second
    // invented light, it is a FLOOR. HEART_FLOOR is the least luminance a heart may be lit by; whatever the
    // world light falls short of it is topped up ISOTROPICALLY, so the top-up carries no direction of its own
    // and cannot pretend to be a lamp. In open daylight the shortfall is zero and the row is lit by nothing but
    // the sun, the sky and the ground bounce. Underground the world term is ~0.013 and the floor does all the
    // work: the heart lands near sRGB 118/17/17 against a near-black cave, which counts.
    const HEART_FLOOR : f32 = 0.14;                                  // ...in LINEAR luminance, before aces() and the 1/2.2. Chosen as the least value that keeps five hearts countable buried in stone at midnight -- raise it and the row starts to glow at dusk, which is the fake it replaced
    const HEART_LUMA : vec3<f32> = vec3<f32>(0.2126, 0.7152, 0.0722);   // Rec.709 weights: the floor is compared against the LIGHT's luminance, never the heart's own colour, so a red albedo cannot pull the lift up
    const HEART_MIN : f32 = 0.58;                                    // a heart drained to nothing is this fraction of full size - it SHRINKS as it empties, which is how a partial heart reads on a model with no sub-voxel geometry to fill
    const HEART_SPENT : f32 = 0.085;                                 // ...and a spent one keeps its place in the row as a dark socket at that size, so the bar's LENGTH never changes and "one heart left" can never be mistaken for "a full bar of one". A MULTIPLIER ON ALBEDO, not on light, so the socket stays ~12x darker than a live heart under any sun
    const HEART_BOB : f32 = 0.010;                                   // how far it hovers, in camera units. Stepped to 24 fps in the block itself, like every other animation in the game
    const HEART_KICK : f32 = 0.20;                                   // ...and how much the whole row swells on a hit (u.heartC.w rides VIT.hurtT down over ~0.55 s)
    const HEART_YAW : vec2<f32> = vec2<f32>(0.85252, 0.52269);       // cos/sin of the row's 31.5 deg turn about camera-up. A PURE yaw: the eye already looks DOWN on a row this far below the crosshair, so the third face comes for free, and a pitch on top of it turned the cube edge-on to its own top face
    fn dofCoc(d : f32) -> f32 {
      if (u.dof.x <= 0.0) { return 0.0; }
      let dd = select(u.dof.x * 64.0, d, d > 0.0);                   // d < 0 is SKY: infinitely far, so it sits at the far stop
      var c = 1.0 - u.dof.x / max(dd, 0.05);
      c = sign(c) * max(abs(c) - DOF_DEAD, 0.0) / (1.0 - DOF_DEAD);
      c = clamp(c, -1.0, 1.0);
      return select(c, c * DOF_NEAR, c < 0.0);
    }
    fn vn3(p : vec3<f32>) -> f32 {
      let f = floor(p); let i = vec3<i32>(f);
      var w = p - f; w = w * w * (3.0 - 2.0 * w);
      let a = mix(ih3(i.x, i.y, i.z), ih3(i.x + 1, i.y, i.z), w.x);
      let b = mix(ih3(i.x, i.y, i.z + 1), ih3(i.x + 1, i.y, i.z + 1), w.x);
      let c = mix(ih3(i.x, i.y + 1, i.z), ih3(i.x + 1, i.y + 1, i.z), w.x);
      let d = mix(ih3(i.x, i.y + 1, i.z + 1), ih3(i.x + 1, i.y + 1, i.z + 1), w.x);
      return mix(mix(a, b, w.z), mix(c, d, w.z), w.y);
    }
    // ── CLOUD DECK ALTITUDE (user 2026-08-15: higher) ── was 480-800. Named because the figure appeared in
    // THREE places - the density ramp and both slab entry/exit distances - and moving one without the others
    // either flattens the deck or makes the raymarch miss it entirely.
    const CLOUD_LO : f32 = 760.0;
    const CLOUD_HI : f32 = 1080.0;
    // ── RAIN SKY ── (user 2026-08-17: cloudier + darker clouds while it rains, a slightly dimmer sun, and a
    // clean return to normal afterwards). ONE uniform scalar, 0..1: how hard it is raining AT THE CAMERA.
    // tick-camera writes it as the storm ramp times oakM(P.x, P.z), so it is 0 in the pine forest and the desert
    // — which get SNOW from the same storm and must keep exactly the sky they have today — and it rises as the
    // player walks west across the 450-voxel oak border. It rides in u.hurtV.w; see UF_RAINK in
    // render/buffers.js for why that lane was the one available and why taking it is safe.
    //
    // THE RULE EVERY TERM BELOW OBEYS: at rainK 0 the expression must reduce to the fair-weather one EXACTLY,
    // not approximately. The deck is scaled by 1.0 - x*0.0, sunTintR multiplies
    // sunTint by 1.0, and skyRain returns its argument untouched behind a compare. That is what makes "then when
    // the rain is gone, the clouds return to normal" a property of the code rather than a tuning claim.
    fn rainK() -> f32 { return u.hurtV.w; }
    const RAIN_CLOUD_DARK : f32 = ${RAIN_CLOUD_DARK.toFixed(4)};
    const RAIN_SKY_DESAT : f32 = ${RAIN_SKY_DESAT.toFixed(4)};
    const RAIN_SKY_DIM : f32 = ${RAIN_SKY_DIM.toFixed(4)};
    const RAIN_SUN_DIM : f32 = ${RAIN_SUN_DIM.toFixed(4)};
    // THE DIRECT SUN, dimmed by the deck overhead. sunTint() is the illuminant's own colour and already carries
    // the entire day/night story inside it — the isMoon() branch, the horizon amber, the smoothstep fade through
    // dawn — and a uniform scalar multiplied onto a continuous function is still continuous, so this cannot
    // introduce a jump at the dusk/dawn moon swap the way a select() on isMoon() would. Every direct-sun site in
    // this pass calls THIS instead of sunTint(); the one deliberate exception is the cloud march, whose deck is
    // lit from ABOVE by the very cloud layer doing the dimming. dayScale() is untouched, on purpose: see
    // RAIN_SUN_DIM in render/buffers.js.
    fn sunTintR() -> vec3<f32> { return sunTint() * (1.0 - RAIN_SUN_DIM * rainK()); }
    // A RAIN SKY HAS NO DEEP BLUE IN IT. One colour transform, applied to every place the sky is read: the sky
    // itself, the haze the far world fades into, and the sky reflected in water and ice. All of them, or the
    // horizon splits — dulled sky above a fair-weather haze is a seam right across the middle of the frame.
    // This is also the only handle on the sun DISC and its glare, which are drawn inside skyColor() in pre.js;
    // at night it is what takes the stars and the moon down behind the overcast. The compare is on a UNIFORM,
    // so the whole dispatch takes one side of it and fair weather pays one scalar test.
    fn skyRain(c : vec3<f32>) -> vec3<f32> {
      let rk = rainK();
      if (rk <= 0.0005) { return c; }
      let lum = dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
      return mix(c, vec3<f32>(lum), RAIN_SKY_DESAT * rk) * (1.0 - RAIN_SKY_DIM * rk);
    }
    fn skyBaseR(rd : vec3<f32>) -> vec3<f32> { return skyRain(skyBase(rd)); }
    // ══ THE WORLD'S DISTANCE FOG, ONCE (user 2026-08-20: "unify the render paths") ══ this lived as three
    // byte-identical copies inside COMPOSITE's shading branches, and every object drawn OUTSIDE those branches
    // — a creature, a drop, the held tool — had to remember to haze itself. That is the shape of the bug class
    // this is meant to retire: the fog pass ending before the creatures were drawn is exactly how fish, birds
    // and drops ended up sitting at full saturation in front of a hazed treeline.
    // Two terms, unchanged: an exponential on distance, and a hard ramp into the render-distance wall so
    // nothing pops at the edge of the generated world. LG(5u) still switches the whole thing off in one place.
    // IT LIVES HERE, NOT IN PRE, and that is not a preference: it calls skyBaseR, which is declared four
    // lines up in THIS file, and WGSL has no forward declarations — a call above its definition is a compile
    // error, which in this codebase is a game that never boots. PRE is prepended to every pipeline, so the
    // obvious home for a shared helper is the one place this one cannot go.
    // A FUNCTION, not a macro-by-copy: anything that draws a world-space surface can now ask for the same haze
    // at the same distance and cannot get a stale version of the formula.
    fn worldFog(col : vec3<f32>, rd : vec3<f32>, dist : f32) -> vec3<f32> {
      if (!LG(5u)) { return col; }                                 // LIGHT DEBUG bit 5: distance fog
      var fogA = 1.0 - exp(-dist * 0.0006);
      // ── THE VIEW-DISTANCE FADE IS A FRACTION OF THE VIEW, NOT A FIXED 66 VOXELS ── it used to run
      // rdist-72 .. rdist-6, and that is the "large slice in the terrain" (user 2026-08-31, debug clip 52):
      // a straight edge at the far plane with fully-lit terrain on one side and flat sky colour on the other.
      // The distance fog above is exp(-d * 0.0006), which is only 68% opaque at a 1900-voxel view and 48% at
      // 1083 - so at the moment the geometry stops, between a third and a HALF of the terrain's contrast is
      // still showing, and this smoothstep then removed all of it inside 66 voxels. At that range 66 voxels is
      // a couple of pixels, which is why it reads as a cut rather than a haze.
      // It is worst exactly where it was filmed: rdist is not fixed at 2000, it tracks the ring's real fill
      // (see UF[64] in main/tick-camera.js), so right after a teleport or when the streamer is behind it drops
      // to ~1080 - and the SHORTER the view, the LESS the exponential has saturated and the bigger the step
      // this has to hide. A fixed band cannot win that; a fraction of rdist can, because it widens precisely
      // when it has more to hide. At 0.62 the fade spans ~400 voxels at a 1083 view and ~700 at 1900, and the
      // nearest 62% of the view keeps exactly the fog it had.
      fogA = max(fogA, smoothstep(u.rdist.x * 0.62, u.rdist.x - 6.0, dist * length(vec2<f32>(rd.x, rd.z))));
      return mix(col, skyBaseR(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogA);
    }
    // ══ PROCEDURAL-CLOUDS CACHE (jeantimex/procedural-clouds, MIT) ══ the 3D density volume the CLOUDGEN
    // compute pass fills, and a sampler of its own. NOT linSamp: that one is clamp-to-edge, and this volume is
    // a TILE, so clamping would put a one-texel seam along every tile boundary in the sky. Both are read below
    // — a binding that is declared and never read is pruned by layout:'auto' and takes the canvas black.
    @group(0) @binding(24) var cgTex : texture_3d<f32>;
    @group(0) @binding(25) var cgSamp : sampler;
    ${CG_CONSTS}
    // ── ONE FETCH ── the entire cost of a density sample, against ~24 hash lookups for the procedural deck.
    // xz wrap through the sampler (the volume is a TILE, see cloudgen.js), y clamps across the deck. The wind
    // is applied here as a lookup offset instead of being baked into the cache, so the deck drifts forever
    // without the cache ever being refilled for it.
    fn cgSample(p : vec3<f32>) -> f32 {
      let yn = (p.y - CLOUD_LO) / (CLOUD_HI - CLOUD_LO);
      if (yn < 0.0 || yn > 1.0) { return 0.0; }
      let uvw = vec3<f32>(fract((p.x + u.cloudT.x * 9.0) / CG_TILE), clamp(yn, 0.0, 1.0), fract((p.z + u.cloudT.x * 3.5) / CG_TILE));   // u.cloudT, NOT u.time: the wind rides the day/night cycle's own clock, so speeding the cycle speeds the drift with it. Identical to u.time at 1x
      return textureSampleLevel(cgTex, cgSamp, uvw, 0.0).r * CG_STORE;
    }
    // ── INTERLEAVED GRADIENT NOISE (Jorge Jimenez) ── the dither the march starts on, and the source's own
    // choice. It replaced a per-pixel-per-frame WHITE-NOISE hash, which is the worst input TAA can be given:
    // white noise clumps, so neighbouring pixels get wildly different march offsets and the resolve has
    // nothing coherent to average. The symptom was heavy grain through the whole deck, worst around a low
    // sun where the bloom multiplies the march's alpha by a large number and amplifies its noise with it
    // (user 2026-08-28: "when the sun settles in the west there is a lot of noise in the clouds").
    // PROVEN, not assumed: replacing the offset with a constant 0.5 removed the grain entirely and left the
    // concentric step banding the dither exists to hide — so the dither was the source, and the fix is a
    // better-distributed one rather than fewer steps or more of them.
    // IGN is near-optimal over a 2x2..8x8 neighbourhood, so adjacent pixels get well-spread offsets and the
    // resolve averages them in ONE frame instead of waiting for temporal luck. The golden-ratio term rotates
    // the pattern per frame so it also decorrelates over time, which is what stops it printing a fixed
    // texture into the sky when the camera holds still.
    fn cgIGN(p : vec2<f32>, f : u32) -> f32 {
      let q = p + 5.588238 * f32(f % 64u);                          // golden-ratio conjugate walk — 64 frames before it repeats, far longer than the resolve's memory
      return fract(52.9829189 * fract(dot(q, vec2<f32>(0.06711056, 0.00583715))));
    }
    // ── HENYEY-GREENSTEIN ── the source's own phase, mixed 60% against isotropic exactly as it does
    fn cgPhase(cosT : f32, g : f32) -> f32 {
      let g2 = g * g;
      return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
    }
    // ── LIGHT MARCH ── the source's lightMarch: accumulate density along the sun ray and take exp(-shadow*k).
    // This is what a single 60-unit shadow tap could never give — a real gradient through the cloud — and the
    // cache is what makes four fetches here affordable where four noise evaluations were not.
    fn cgLight(p : vec3<f32>) -> f32 {
      var sh = 0.0;
      for (var i = 1; i <= CG_LIGHT_STEPS; i++) {
        sh += cgSample(p + u.sunDir * (f32(i) * CG_LIGHT_STEP)) * CG_LIGHT_STEP_U;   // world units to STEP, source units to ACCUMULATE — see CG_U2W
      }
      return exp(-sh * CG_SHADOW);
    }
    @group(0) @binding(22) var<storage, read> visb : array<u32>;     // per-8×8-tile drop-slot visibility bitmask (4×u32/tile) — computed ONCE per frame by the VIS prepass and shared with TRACE (this pass used to recompute it per workgroup behind a barrier)
    // ── VOLUMETRIC LIGHT ── march the camera ray, gathering in-scatter from the emissive point lights
    // and testing visibility from each step to the light. Teardown's method; see the call site for how
    // this differs from theirs.
    fn volLight(ro : vec3<f32>, rd : vec3<f32>, tMax : f32, dither : f32) -> vec3<f32> {
      var acc = vec3<f32>(0.0);
      var far = min(tMax, 90.0);
      if (far <= 0.2) { return acc; }
      // ── CULL AGAINST THE RAY ── one dot product per light drops every one the ray never passes near,
      // and narrows the march to the span that can actually receive anything.
      var mask = 0u; var lo = far; var hi = 0.0;
      for (var f = 0; f < 8; f++) {
        let F = u.fflies[f];
        if (F.w <= 0.0) { continue; }
        let dv = F.xyz - ro;
        let tc = clamp(dot(dv, rd), 0.0, far);                   // closest approach along the ray
        if (dot(dv, dv) - tc * tc > 900.0) { continue; }         // the ray never comes within reach of this light
        mask |= (1u << u32(f));
        lo = min(lo, max(tc - 30.0, 0.0));                       // …and only this span of the ray matters
        hi = max(hi, min(tc + 30.0, far));
      }
      if (mask == 0u || hi <= lo) { return acc; }                // nothing on this ray — the common case, and it costs 8 dots
      let STEPS = 8;                                             // fewer steps than before; the dither and TAA carry the smoothing
      let dt = (hi - lo) / f32(STEPS);
      for (var i = 0; i < STEPS; i++) {
        let ps = ro + rd * (lo + (f32(i) + dither) * dt);
        // strongest contributor at this point — ONE trace serves it, as the surface glow term does
        var bw = 0.0; var bi = -1; var bd2 = 0.0;
        for (var f = 0; f < 8; f++) {
          if ((mask & (1u << u32(f))) == 0u) { continue; }
          let F = u.fflies[f];
          let dv = F.xyz - ps;
          let d2 = dot(dv, dv);
          if (d2 > 900.0) { continue; }
          let w = F.w / (1.0 + d2 * 0.05) * (1.0 - smoothstep(400.0, 900.0, d2));
          if (w > bw) { bw = w; bi = f; bd2 = d2; }
        }
        if (bi < 0) { continue; }
        let dvB = u.fflies[bi].xyz - ps;
        let dist = sqrt(max(bd2, 1e-4));
        let sh = traceAll(ps, dvB / dist, max(dist - 0.7, 0.0), true);   // the visibility ray the article describes
        if (sh.t >= 0.0) { continue; }                           // solid between the medium and the light — this step is in shadow
        acc += vec3<f32>(1.0, 0.34, 0.16) * bw;                  // warm: everything in this lane is an ember, a firefly or a wound
      }
      return acc * dt * 0.010;                                   // scatter per unit length, HALVED (user) — the march integrates it
    }
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>, @builtin(workgroup_id) wgid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let px = vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y);
      let rd = rayDir(px);
      let alb4 = textureLoad(gAlbedo, vec2<i32>(gid.xy), 0);
      let faceRaw = u32(alb4.a * 255.0 + 0.5) & 15u;
      let isSandTop = (faceRaw == SANDF);                             // ── SAND TOP FACE ── TRACE tags it (see SANDF in PRE); the sun glisten in the land branch is the only thing that asks
      let face = select(faceRaw, 2u, isSandTop);                      // …and to every other reader it is an ordinary TOP face, so faceN, the water/lava branches and the depth logic are untouched
      let lavaG = f32(u32(alb4.a * 255.0 + 0.5) >> 4u) / 14.0;
      var col : vec3<f32>;
      // ── SURFACE TERMS, HOISTED ── the creature/drop path far below composites submerged hits through the
      // water, and to do that the way the BED does it needs the same three numbers the bed uses: the water's
      // own in-scatter glow, the mirror, and the Fresnel split between them. Without them it was faking the
      // whole thing by fading the creature toward the finished SURFACE colour, which already has the sky
      // reflection mixed in -- so a fish saturated toward a bright grey and read as a pale blob, brighter
      // than the water it was supposedly inside.
      var waterScat = vec3<f32>(0.0);
      var waterRefl = vec3<f32>(0.0);
      var waterFres = 0.0;
      var waterIceC = vec3<f32>(0.0);                                // …and the ICE the surface turned into, with the amount of it: a submerged fish has to hide behind the SAME sheet the lakebed hides behind (user 2026-08-08)
      var waterIceK = 0.0;
      if (u.misc.w > 0.5) {                                          // ── EYE INSIDE A VOXEL ── show the MATERIAL you are buried in as chunky pixels, not a black void
        let pk = u32(u.misc.w + 0.5) - 1u;                           // packed sRGB of that voxel: stone, needle green, trunk brown…
        let base = pow(vec3<f32>(f32(pk & 255u), f32((pk >> 8u) & 255u), f32((pk >> 16u) & 255u)) / 255.0, vec3<f32>(2.2));
        let bq = vec2<i32>(vec2<f32>(gid.xy) * 0.04);                // ~24 px blocks — 4x coarser than the first pass (user), so it reads as chunky voxel material
        var hh = u32(bq.x) * 374761393u + u32(bq.y) * 668265263u;    // (ivhash lives in TRACE, not PRE — this is the same mix, inlined)
        hh = (hh ^ (hh >> 13u)) * 1274126177u;
        let h1 = f32((hh ^ (hh >> 16u)) & 1023u) / 1023.0;
        var hc = u32(bq.x >> 2) * 2246822519u + u32(bq.y >> 2) * 374761393u;   // a coarser second octave breaks up the regular grid
        hc = (hc ^ (hc >> 13u)) * 668265263u;
        let h2 = f32((hc ^ (hc >> 16u)) & 1023u) / 1023.0;
        let k = 0.30 + 0.42 * h1 + 0.24 * h2;                        // per-block shade — you are INSIDE it, so it stays dim whatever the material
        textureStore(colorOut, vec2<i32>(gid.xy), vec4<f32>(sqrt(base * k), 0.5));   // alpha 0.5 = a circle of confusion of ZERO: your eye is inside the voxel, there is no depth here to blur by
        return;
      }
      if (face == 7u) {
        col = skyRain(skyColor(rd));                                 // ── RAIN ── dulled BEFORE the march, so the sky showing THROUGH the deck (the aSky composite below) is the rain sky and not the fair-weather one
        if (rd.y > 0.02) {                                           // VOLUMETRIC CLOUDS — the ported march below, over the cached density volume
          let camY = u.camPos.y;
          let t0c = (CLOUD_LO - camY) / rd.y;
          let t1c = (CLOUD_HI - camY) / rd.y;
          let ta = max(min(t0c, t1c), 0.0);
          let tb = min(max(t0c, t1c), 12000.0);
          if (tb > ta) {
            let roW = vec3<f32>(u.camPos.x + u.winO.x, camY, u.camPos.z + u.winO.y);
            // ══ THE PORTED MARCH ══ (jeantimex/procedural-clouds, MIT) — box-limited raymarch of a cached
            // density volume, dithered start, Beer transmittance, a real light march per lit sample and an HG
            // phase mixed against isotropic. The structure is the source's; what is NOT the source's is the
            // illuminant: SUN_COLOR/AMBIENT/BG_COLOR and its Reinhard tonemap are replaced by sunTint(),
            // dayScale() and this renderer's sky, so the deck sits inside voxelbit's day/night and rain rather
            // than carrying its own lighting model into the middle of the frame.
            // ── SLICE PROBE (LG2 bit 6, __vb.lgt2(67)) ── paint one horizontal slice of the density volume
            // straight onto the screen, one texel per pixel-ish, bypassing the march entirely. This exists
            // because estimating the field from the MARCH is how the first calibration went wrong twice: a ray
            // reports the MAX of ~14 samples, which sits far above the per-texel distribution the graph actually
            // produces, so a threshold set from it barely moves the coverage. A slice is the distribution.
            // ── AND IT LIVES ON BIT 6, NOT BIT 3, BECAUSE BIT 3 STOPPED BEING FREE ──────────────────────
            // This probe returns EARLY and paints the whole sky, so whichever bit carries it is a switch that
            // blanks the sky. That was safe while LGT2_ALL was 0x3 and bit 3 could only ever be set by typing
            // __vb.lgt2(15) on purpose. The water panel then took bits 2-5 for its rows and raised LGT2_ALL to
            // 0x3f, so bit 3 became ON BY DEFAULT and this fired every frame: black sky with grey blobs, which
            // is the density field, over correctly-lit terrain (user 2026-08-30: "half the screen is black").
            // Bisected to the bit: with only bit 3 added the sky measured (5,5,5), and (145,167,184),
            // (144,167,183), (144,166,183) for bits 2, 4 and 5 — so it was this branch, not the water work.
            // Moved rather than deleted: it is the instrument that got the cloud coverage right, and its own
            // note explains why estimating the field from the march went wrong twice. Bit 6 is the first bit
            // above the panel's range, so it is outside LGT2_ALL and cannot be reached by the panel, the bake
            // or the stored mask — only by asking for it, which is what a probe should need.
            if (LG2(6u)) {
              let sl = vec2<f32>(gid.xy) / vec2<f32>(u.res.x, u.res.y);
              let dS = select(cgSample(vec3<f32>(sl.x * CG_TILE, CLOUD_LO + 0.35 * (CLOUD_HI - CLOUD_LO), sl.y * CG_TILE)) * 0.1, sl.x, sl.y < 0.06);   // top 6% is a known 0..1 ramp. IT PROVED THE PROBE CANNOT READ ABSOLUTE VALUES: the measured transfer is neither linear nor gamma (0.7 -> 147, 0.9 -> 172), because the god-ray and TAA passes run after this store and both add to it. The slice view is still good for SHAPE; every density number read off it was wrong, which is what sent the calibration in circles.
              textureStore(colorOut, vec2<i32>(gid.xy), vec4<f32>(vec3<f32>(dS), 0.5));
              return;
            }
            let stepW = (tb - ta) / f32(CG_RAY_STEPS);
            let stepU = stepW / CG_U2W;                            // the same step, in the units the density was authored in
            let cosT = dot(rd, u.sunDir);
            let phase = mix(1.0, cgPhase(cosT, CG_G), CG_PHASE_MIX);
            // ── THE DECK HAS A NIGHT OF ITS OWN (user 2026-08-28: "make the clouds lighter during the
            // night, they are pitch black") ── and they were, measurably: the ambient below is
            // mix(HORIZON, ZENITH, 0.4) * 0.55 * dayScale(), and dayScale bottoms out at 0.0084, so the whole
            // term came to (0.3, 0.5, 0.9) out of 255. Below one display level. Black.
            // TWO SEPARATE FAULTS, and only fixing both gets a moonlit deck:
            //  1. the KEY was double-dimmed. sunTint() ALREADY carries the moon's own 0.198 scale at night,
            //     and multiplying it by dayScale() applied the night floor to it a second time — 0.265 became
            //     0.0017. mix(dayScale(), CG_NKEY, nightK()) leaves the day untouched (nightK is 0 there) and
            //     hands the night a real, if modest, moon key.
            //  2. the AMBIENT had no night floor at all. Every other surface in this renderer gets one
            //     (ambFloor for the world, dayScale's own mf for the sky); the deck was the one thing lit
            //     purely multiplicatively, so it alone went to nothing. CG_NAMB is that floor, cool and blue
            //     because a moonlit cloud is lit by a blue-grey sky and reads that way in any night photograph.
            // Both fade in on nightK(), which is the standing rule here — keyed on the TRUE sun elevation, so
            // neither jumps at the dusk/dawn moon swap.
            let sunC = sunTint() * mix(dayScale(), CG_NKEY, nightK());
            let ambC = mix(HORIZON, ZENITH, 0.4) * 0.55 * dayScale() + CG_NAMB * nightK();
            var tg = ta + stepW * cgIGN(vec2<f32>(gid.xy), u32(u.frame));   // …and this is that dither, restored to the source's own choice — see cgIGN
            var Tg = 1.0;
            var accG = vec3<f32>(0.0);
            for (var ci = 0; ci < CG_RAY_STEPS; ci++) {
              let d = cgSample(roW + rd * tg);
              if (d > CG_MIN_D) {
                let stTr = exp(-d * stepU);
                let wt = Tg * (1.0 - stTr);                       // ── THE SAMPLE'S ACTUAL CONTRIBUTION ── computed BEFORE the light march, because it is what decides whether that march is worth running. A wisp seen through cloud that has already absorbed most of the ray cannot change the pixel, and the four extra volume fetches it would cost are the most expensive thing in this shader
                if (wt > CG_MIN_W) {
                  let scat = cgLight(roW + rd * tg) * phase * (1.0 - exp(-d));
                  accG += wt * (sunC * scat * CG_SUN + ambC);
                } else {
                  accG += wt * ambC;                              // …still deposit the ambient, so skipping the light march dims the sample rather than deleting it
                }
                Tg = Tg * stTr;
                if (Tg < CG_T_CUT) { break; }
              }
              tg += stepW;
            }
            // ── COMPOSITE THE DECK AS A SURFACE WITH ALPHA, NOT AS A BLEND AGAINST THE RAW SKY ──
            // The old line was mix(sky, cloud + sky*T, fade), which adds (1 - fade) of the UNATTENUATED sky
            // back on top of the cloud. skyColor() draws the sun DISC, so at fade 0.83 a fifth of a very
            // bright disc came through even where the cloud was fully opaque, and it read as a crisp circle
            // behind the cloud. Distance is the wrong thing to make a cloud transparent with anyway: a far
            // cloud is still opaque, it just takes the colour of the air in front of it. So the fade now
            // moves the cloud's COLOUR toward the horizon haze and leaves its ALPHA alone, and the deck is
            // composited over the sky by that alpha — an opaque cloud hides the disc completely.
            // The brightness still comes through: it arrives as the deck's own scattering, which is what the
            // phase function and the light march are for, so a cloud in front of the sun glows rather than
            // showing a hole with the sun in it.
            let fadeG = exp(-ta * 0.00035);
            let aG = 1.0 - Tg;                                     // the deck's coverage of this pixel
            let hazeG = skyBaseR(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z)));
            let cloudG = accG * (1.0 - RAIN_CLOUD_DARK * rainK()) / max(aG, 1e-3);   // accG is premultiplied by alpha; undo that to get the cloud's own colour
            // THE SKY TERM OCCLUDES FASTER THAN THE DECK IS DENSE, and that is a deliberate look control.
            // The sun's disc is ~21.6 radiance against a glare of 0.5, so under ACES the disc survives thin
            // cloud while its halo does not — which is precisely the "little dot behind the clouds": a hard
            // core with nothing around it. Killing the sky faster kills the DISC first, and the bloom below
            // puts the light back as a halo. It applies to the moon on the same line, without this pass
            // needing to know anything about how either body is drawn.
            let aSky = min(1.0, aG * CG_SKY_OCC);
            col = mix(col, mix(hazeG, cloudG, fadeG), aSky);
            // ── THE BODY BEHIND THE DECK ── the composite above multiplies the whole sky by transmittance,
            // and skyColor draws the sun and moon INTO that sky, so both the disc and its glare were being
            // extinguished together. That is right for the crisp disc — a circle showing through cloud was
            // the original complaint — and wrong for everything else about them: a sun behind cloud does not
            // go dull, it turns into a broad bright bloom, because the light is scattered rather than
            // absorbed. So the disc stays extinguished and the ENERGY comes back as a halo whose width grows
            // with the deck's opacity: at aG 0 this term vanishes entirely and clear sky is untouched, at
            // aG 1 it is a wide diffuse glow with no outline in it.
            // The moon gets the same treatment on the same lines. rsC is the up-body (u.sunDir holds
            // whichever of the two is up), so the two 'up' gates below mean only the body actually in the sky
            // ever contributes, and the swap at dusk needs no special case.
            let rsC = select(u.sunDir, -u.sunDir, isMoon());
            let rmC = -rsC;
            let widthG = mix(CG_BLOOM_TIGHT, CG_BLOOM_WIDE, aG);
            let rk2 = 1.0 - RAIN_CLOUD_DARK * rainK();
            col += SUN_COL * (CG_BLOOM * aG * rk2 * smoothstep(-0.03, 0.06, rsC.y)) * pow(max(dot(rd, rsC), 0.0), widthG);
            // ── AND THE MOON'S BLOOM SCALES WITH ITS PHASE ── it did not, which is why a new moon still lit
            // the deck: the disc went dark and the KEY light went with it, but this term carried on throwing
            // a full moon's halo through the cloud. moonPhaseF() is the same fraction both of those use.
            col += MOON_GLOW * (CG_MBLOOM * aG * rk2 * moonPhaseF() * smoothstep(-0.03, 0.06, rmC.y)) * pow(max(dot(rd, rmC), 0.0), widthG);
          }
        }
      } else if (face == 8u) {                                       // LAVA: emissive — burns through the fog
        let irr = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        let alb = alb4.rgb * alb4.rgb;
        col = alb * (3.1 + 0.6 * sin(u.time * 2.1)) + vec3<f32>(0.65, 0.13, 0.0);   // molten orange, glowing, gentle pulse
        col = worldFog(col, rd, irr.b);                            // ── ONE FOG (see worldFog in PRE) ── this was three byte-identical copies of the same four lines
      } else if (face == 6u) {                                       // ── PHYSICALLY-BASED WATER ── Gerstner surface, RAY-TRACED reflection + refraction, Beer–Lambert absorption + single scattering. The voxel aesthetic survives on purpose: the surface is still stepped 10 cm columns, the mirror image is the voxel world itself, glints stay discrete.
        let irr = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        let alb = alb4.rgb * alb4.rgb;
        let tWat = irr.b;
        let pw2 = u.camPos + rd * tWat;
        let wx2 = pw2.x + u.winO.x; let wz2 = pw2.z + u.winO.y;
        // ── AN IF, NOT A select() ── select evaluates BOTH operands, so this paid for gerstN — four sin and
        // four cos over the whole GW table — on every water pixel in the frame even though WATER_BAKE ships
        // waves: 0 and the result was thrown away every time. Pure waste at the shipping default.
        var nW = vec3<f32>(0.0, 1.0, 0.0);
        if (LG(23u)) { nW = gerstN(wx2, wz2); }   // bit 23: WATER WAVES — off = a flat mirror plane, so wave shape can be told apart from wave lighting                                   // the GERSTNER normal — same field that raises the voxel crests, crest-pinched by the Q term
        let foamW = smoothstep(0.16, 0.50, alb.g) * (1.0 - u.pickZ.w);   // foam carries a bright albedo → shade it as SURFACE, not window (ice path handles frozen)
        let cosI = clamp(-dot(rd, nW), 0.02, 1.0);
        let fres = 0.02 + 0.98 * pow(1.0 - cosI, 5.0);               // true Schlick, F0 = 0.02 (air→water)
        let sunW = smoothstep(-0.02, 0.12, u.sunDir.y) * irr.r;      // sun above horizon AND this surface point actually sees it
        // ── RAY-TRACED REFLECTION ── the mirrored ray walks the real voxel scene; misses (and far hits) fall back to the sky.
        // PERF (user: "fps tanks"): the killer was GRAZING views over big lakes — every distant water pixel launched a ray
        // that SKIMS the surface for hundreds of voxels. Now only NEAR water traces (≤110 — where mirrored trees actually
        // resolve; beyond that the mirror is sub-pixel mush and sky+glitter reads identically), near-vertical views skip
        // (fres ≈ 0.02 → the mirror is invisible anyway), and the cap+fade tightened so the trace never outlives its
        // visible contribution. Same look where it counts, a fraction of the rays.
        var refl = reflect(rd, nW);
        if (refl.y < 0.03) { refl = normalize(vec3<f32>(refl.x, 0.06 - refl.y * 0.5, refl.z)); }   // a grazing mirror ray that would dive back under the surface folds just above it — no self-hit acne
        var reflC = skyBaseR(refl);
        if (tWat < 110.0 && fres > 0.045) {
          let rh = traceAll(pw2 + vec3<f32>(0.0, 0.06, 0.0), refl, 140.0, true);   // skipW: the folded mirror ray can never re-enter the flat water plane (crests live in the analytic field, not the grid) — output-identical, and it STRIDES the water-only bricks it skims instead of fine-stepping them
          if (rh.t >= 0.0) {
            let rpos = pw2 + refl * rh.t;
            let rvc = vec3<i32>(floor(rpos - rh.n * 0.01)) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));
            let ralb = pal[rh.vox].rgb * (0.88 + 0.24 * ivhash(rvc));
            let rlit = sunTintR() * (max(dot(rh.n, u.sunDir), 0.0) * 0.9 * irr.r) + mix(HORIZON, ZENITH, 0.5 + 0.5 * rh.n.y) * 0.95 * dayScale() + ambFloor();
            reflC = mix(ralb * rlit, skyBaseR(refl), 1.0 - exp(-rh.t * 0.014));   // the mirror fades into sky with distance, like the world fades into haze
          }
        }
        // ── REFRACTION + BEER–LAMBERT ── the transmitted ray bends by Snell (η = 1/1.33) and marches to the bed; what
        // comes back is absorbed per-channel over the traveled water path — red dies first, blue carries.
        // PERF: capped at 34 (beyond that Beer–Lambert leaves <3% — invisible), and skipped entirely at grazing angles
        // (fres > 0.8 → the Fresnel split hands nearly everything to the mirror; the in-scatter constant stands in).
        let sigT = WATER_SIG;                                        // extinction per voxel (10 cm) — 20% MORE TRANSPARENT (user 2026-08-06): every channel scaled by 0.8, so the RATIO is untouched and the water keeps its colour (red still dies first, blue carries) while you see 25% deeper before the same amount is absorbed. Was (0.30, 0.115, 0.052).
        let scatC = vec3<f32>(0.018, 0.070, 0.092) * (0.30 + 0.70 * sunW) * dayScale() * (0.45 + 0.55 * irr.g);   // single-scatter source — the water's own glow, lit by sun + sky
        var refrC = scatC;                                           // no bed within reach → the column saturates to pure in-scatter
        if (fres < 0.80 && LG(19u)) {                                // bit 19: WATER REFRACTION — off leaves refrC at the pure in-scatter colour, no bed, no Beer-Lambert
          var refr = refract(rd, nW, 0.752);
          if (dot(refr, refr) < 1e-5) { refr = rd; }                 // grazing/TIR fallback: continue straight
          let bh = traceAll(pw2 + rd * 0.02, refr, 48.0, true);   // 34 -> 48 when the arctic bed went to WL-42 (see ARCT_SEA): the old cap was argued from the RED channel, and blue still returns 17% at 42, so a shorter ray was throwing away floor that is plainly visible
          if (bh.t >= 0.0) {
            let bpos = pw2 + rd * 0.02 + refr * bh.t;
            let bvc = vec3<i32>(floor(bpos - bh.n * 0.01)) + vec3<i32>(i32(u.winO.x), 0, i32(u.winO.y));
            // ── WATER CAUSTICS, lgt.z bit 2 (the panel's caustics row) ── an if, never a select(): select
            // is a FUNCTION in WGSL, so both operands are evaluated and the off state would still pay for two
            // drifting noise fields on every refracted water pixel in the frame. Same trap the waves hit above.
            var ca = 0.0;
            if (LG2(2u)) { ca = caust(floor(vec2<f32>(bpos.x + u.winO.x, bpos.z + u.winO.y)) + vec2<f32>(0.5)); }   // caustic webs dance on the refracted bed
            let balb = pal[bh.vox].rgb * (0.88 + 0.24 * ivhash(bvc)) * (1.0 + 1.6 * ca * sunW);
            let blit = (0.45 + 0.55 * irr.g) * dayScale() * (0.55 + 0.45 * irr.r);
            let trB = exp(-sigT * bh.t);                             // Beer–Lambert over the in-water path
            refrC = balb * blit * trB + scatC * (vec3<f32>(1.0) - trB);
          }
        }
        waterScat = scatC; waterRefl = reflC;                        // …the creature path below composites against these, so a fish sits in the same water the bed sits in
        waterFres = clamp(select(0.0, fres * u.lgt.y, LG(18u)), 0.0, 1.0);
        // energy split by Fresnel — transmission vs mirror. Bit 18: WATER REFLECTION — off hands the whole
        // surface to refraction. u.lgt.y is the panel's REFLECTION STRENGTH slider (1 = physical Schlick).
        col = mix(refrC, reflC, clamp(select(0.0, fres * u.lgt.y, LG(18u)), 0.0, 1.0));
        col = mix(col, alb * (0.55 + 0.45 * irr.g) * (0.60 + 0.40 * irr.r) * dayScale() * 1.30, select(0.0, foamW, LG(20u)));   // bit 20: WATER FOAM   // FOAM voxels stay bright chunky surface
        if (u.pickZ.w > 0.015 && LG(21u)) {                           // bit 21: WATER ICE — off keeps the surface liquid-looking however frozen it is.   // FREEZING/FROZEN — the ice look BLENDS in over ~25 s as the lake freezes, and back out as it thaws
          let nI = vec3<f32>(0.0, 1.0, 0.0);
          let fresI = 0.03 + 0.22 * pow(1.0 - clamp(-dot(rd, nI), 0.0, 1.0), 4.0);
          let frost = 0.9 + 0.2 * fract(sin(floor(wx2) * 12.9898 + floor(wz2) * 78.233) * 43758.5453);
          var iceC = mix(alb, vec3<f32>(0.74, 0.81, 0.90), 0.62) * frost * (0.5 + 0.5 * irr.g) * dayScale() + skyBaseR(reflect(rd, nI)) * fresI;
          // -- ICE GLISTEN (same lgt.x bit 22 as the liquid one) -- the frozen surface wears the SAME discrete 10 cm cube
          // glint the water wears (user 2026-08-09: "make the ice glisten like the water does"). Same cell grid, same phase
          // and pick hashes, same duty window, same reflection column off the same flat surface normal -- so as a lake skins
          // over, the glitter path stays exactly where it was and only its brightness key changes (see sparkCI). Folded into
          // iceC BEFORE waterIceC is captured, so a fish sealed under the sheet is covered by GLINTING ice rather than by a
          // dull fish-shaped patch of it -- the same trap the 2026-08-08 refraction bug fell into.
          // A per-cell frost-facet tilt was tried here first and removed: scattering the mirror ray by +-30 deg drops
          // pow(., 26) to a few percent on every cell but the untilted ones, leaving a sparse speckle, not a glitter path.
          if (LG(22u)) {
            let gkI = select(smoothstep(-0.02, 0.10, u.sunDir.y), 0.6, isMoon());
            if (gkI > 0.01) {
              let cellI = floor(vec2<f32>(wx2, wz2));                 // one glint cell = one 10 cm voxel, exactly as on water
              let columnI = pow(max(dot(reflect(rd, nI), u.sunDir), 0.0), 26.0);   // the liquid glint's reflection column, unchanged -- and nI is the same flat normal the ice above is shaded with
              let phI = fract(sin(cellI.x * 91.7 + cellI.y * 47.3) * 4321.7) * 6.2831853;   // SAME phase + pick hashes as the liquid glint, so the crossfade below hands the surface over without the lit cells jumping to a different set
              let twI = sin(u.time * 1.6 + phI) * 0.5 + 0.5;
              let pickI = step(0.5, fract(sin(cellI.x * 12.9898 + cellI.y * 78.233) * 43758.5453));
              let sparkI = smoothstep(0.30, 0.85, twI) * pickI;       // the same wide 0.30-0.85 duty window: a glint that arrives and leaves, never blinks
              let sparkCI = max(select(vec3<f32>(1.30, 1.34, 1.42), vec3<f32>(1.15, 1.18, 1.30), isMoon()) * (0.35 + 0.65 * dayScale()), iceC * 3.4);   // -- THE WATER GLINT COLOUR, FLOORED UP TO 3.4x THE ICE UNDER IT -- the liquid glint mixes ~1.35 into a DARK blue surface, so it pops. Daylight ice is already ~1.8 linear (206/255 after ACES, essentially clipped white) and mixing 1.35 into that is invisible: measured p99 210 -> 210, the term did nothing. Keying the spark to the surface fixes both ends at once. In daylight it rises to ~6 and the glitter path reads as bright cubes on white; by moonlight the ice is dark again, the floor takes over, and it lands on the water's own night value instead of the blizzard of white static a fixed daylight constant produced there.
              iceC = mix(iceC, sparkCI, sparkI * columnI * gkI * 0.85 * irr.r);
            }
          }
          waterIceC = iceC; waterIceK = min(1.0, u.pickZ.w * 1.12);   // ── AND THE CREATURE PATH BELOW MUST SEE THIS ── the ice used to be mixed into the surface AFTER waterScat/waterRefl/waterFres were captured, so a fish under a frozen lake was still composited through LIQUID water: at 12 voxels' depth Beer-Lambert ate it down to the water's own in-scatter blue and painted that fish-shaped patch of lake over a white ice sheet. It read as a hole punched in the ice (user 2026-08-08: "the visuals are messed up when the fish are frozen"). The bed is fully hidden at freezeK 1, so the fish must be too.
          col = mix(col, iceC, waterIceK);
        }
        // ── PIXEL GLISTEN (lgt.x bit 22) ── discrete 10 cm cubes flashing on and off (the engine.html look).
        // It used to have a companion, a smooth SOFT sheen on lgt.z bit 0, sharing this same light column;
        // that one is gone (user 2026-08-09) and the column now serves the pixel glint alone.
        if (waterIceK < 0.998 && LG(22u)) {                            // ...and the LIQUID glint fades out exactly as fast as the ice glint above fades in (1 - waterIceK), so a freezing lake never loses its sparkle for a moment. It used to hard-cut at freezeK 0.4 and go dead until the thaw.
          let gk = select(smoothstep(-0.02, 0.10, u.sunDir.y), 0.6, isMoon());
          if (gk > 0.01) {
            let column = pow(max(dot(refl, u.sunDir), 0.0), 26.0);   // reflection column toward the light — the glint lives ONLY inside this
            let cell = floor(vec2<f32>(wx2, wz2));                 // one glint cell = one 10 cm voxel
            let ph = fract(sin(cell.x * 91.7 + cell.y * 47.3) * 4321.7) * 6.2831853;   // per-voxel random phase
            let tw = sin(u.time * 1.6 + ph) * 0.5 + 0.5;   // glint twinkle halved to match
            let pick = step(0.5, fract(sin(cell.x * 12.9898 + cell.y * 78.233) * 43758.5453));   // ~half the voxels participate — scattered, not a sheet
            // ── NO GAPS (user 2026-08-05: "it changes its pattern, removes itself, then changes again") ──
            // the window used to be smoothstep(0.82, 1.0): a cell sat above 0.82 for only 28% of its 3.9 s
            // cycle and reached full brightness for a sliver of that, so at any instant barely 5% of the
            // water carried a glint. The eye reads that as a patch lighting up, going out, and a DIFFERENT
            // patch lighting up — the gap is simply the off part of a very low duty cycle. Widened to
            // 0.30-0.85: lit 63% of the cycle instead of 28%, and the long ramp means a cell fades in and
            // out rather than snapping, so the field is continuously populated and the transitions blend.
            let spark = smoothstep(0.30, 0.85, tw) * pick;         // a soft rise and fall → a glint that arrives and leaves, never blinks
            let sparkC = select(vec3<f32>(1.35, 1.25, 1.0), vec3<f32>(1.15, 1.18, 1.3), isMoon()) * (0.35 + 0.65 * dayScale());
            col = mix(col, sparkC, spark * column * gk * 0.85 * irr.r * (1.0 - waterIceK));   // translucent voxel glint (mix, not add — reads as a bright cube on the surface)
          }
        }
        col = worldFog(col, rd, irr.b);                            // ── ONE FOG (see worldFog in PRE) ── this was three byte-identical copies of the same four lines
      } else {
        let irr = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        let alb = alb4.rgb * alb4.rgb;
        var n = faceN(face);
        let slRaw = textureLoad(slotT, vec2<i32>(gid.xy), 0).r;
        if ((slRaw & 255u) != 0u) {                                  // ── DYNAMIC LIFE ── a trace-injected creature: rebuild its TRUE rotated normal from the slot's
          let s4 = (i32(slRaw & 255u) - 1) * 4;                      // model axes + the stored hit-axis bits, so shading doesn't quantize to world axes and pop as it turns
          let ax = (slRaw >> 8u) & 7u;
          var nl = vec3<f32>(0.0);
          let sv = select(-1.0, 1.0, (ax & 1u) != 0u);
          if ((ax >> 1u) == 0u) { nl.x = sv; } else if ((ax >> 1u) == 1u) { nl.y = sv; } else { nl.z = sv; }
          let nc = dropV(s4 + 1).xyz * nl.x + dropV(s4 + 2).xyz * nl.y + dropV(s4 + 3).xyz * nl.z;
          n = normalize(u.right * nc.x + u.up * nc.y + u.fwd * nc.z);
        }
        let direct = sunTintR() * (irr.r * max(dot(n, u.sunDir), 0.0));
        let skyIrr = mix(HORIZON, ZENITH, 0.5 + 0.5 * n.y) * 0.95 * dayScale();
        let bounce = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * n.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());
        col = alb * (direct + (skyIrr + bounce) * irr.g + ambFloor());   // faint cave ambient
        // ── SAND GLISTEN (the same lgt.x bit 22 the water and the ice glint wear) ── user 2026-08-15: "make the
        // sand glisten from the sun like the water". This is the WATER's column, not a new effect: the same one-glint-
        // cell-per-10-cm-voxel grid, the same phase and pick hashes, the same 0.30–0.85 duty window, the same pow(., 26)
        // reflection lobe off the same FLAT surface normal. Water ships with waves: 0 (see WATER_BAKE), so its normal is
        // the flat plane too — reusing it verbatim is exactly what "like the water" asks for, and it puts the glitter in
        // the same place: a path along the mirrored sun rather than a speckle spread over the whole biome. A per-cell
        // hash-jittered micro-normal was considered and NOT used: at pow(., 26) a few degrees of scatter is the difference
        // between a lit cell and a dead one, so jitter trades the coherent path for sparse static — the same finding that
        // removed the ice's frost-facet tilt, and the opposite of what the rejected snow sparkle wanted.
        // TOP faces only, and that is enforced upstream in TRACE (h.face == 2u), so a dune's vertical wall never glints.
        // ── SAND GLISTEN OFF (user 2026-08-16: "remove the sand glisten") ── SAND_GLINT gates the whole block
        // rather than the code being deleted, because the tuning underneath it was expensive to arrive at: the
        // ACES-desaturation finding, the (1.45, 1.00, 0.36) tint that survives the curve, the 1-in-5 cell
        // density that stops the sparks fusing into a wash, and the separate moon value. Flip SAND_GLINT to
        // true to get all of that back. The sand face id, its denoise.js decode and the trace tag stay live —
        // they are harmless and removing them would be the part that is hard to undo.
        if (SAND_GLINT && isSandTop && LG(22u) && (u32(u.fx) & 2u) == 0u) {   // …and not from INSIDE the water: a sun specular off a submerged lakebed is seen through a refracting interface, so the mirror direction the column is built on is simply wrong there (the same reason the liquid glint lives on the surface pixel, not the bed)
          let gkS = select(smoothstep(-0.02, 0.10, u.sunDir.y), 0.6, isMoon());
          if (gkS > 0.01) {
            let pS = u.camPos + rd * irr.b;
            let cellS = floor(vec2<f32>(pS.x + u.winO.x, pS.z + u.winO.y));   // WORLD-space cell, like the water's — the grid must not crawl when the streaming window shifts under it
            let columnS = pow(max(dot(reflect(rd, n), u.sunDir), 0.0), 26.0);
            let phS = fract(sin(cellS.x * 91.7 + cellS.y * 47.3) * 4321.7) * 6.2831853;
            let twS = sin(u.time * 1.6 + phS) * 0.5 + 0.5;
            let pickS = step(SAND_PICK, fract(sin(cellS.x * 12.9898 + cellS.y * 78.233) * 43758.5453));   // ...the water's hash, read at a HIGHER threshold: see SAND_PICK
            let sparkS = smoothstep(0.30, 0.85, twS) * pickS;
            // ── THE SPARK COLOUR IS KEYED TO THE SAND UNDER IT ── the same trap the ice glisten fell into first. The liquid
            // glint mixes a FIXED ~1.35 into a base of ~0.1, so it pops; DAYLIT SAND is already 1.9 linear and mixing
            // 1.35 into that measured as nothing at all — worse, it pulled R and G DOWN and B UP, which is a wash toward
            // grey. Keying to the surface fixes both ends: full sun raises the spark with the sand, and by moonlight the
            // sand is dark again so the floor — the water's own night value — takes over instead of white static.
            // The TINT is the part that actually reads: see SAND_TINT for why the answer at noon is colour, not gain.
            let daySparkS = col * SAND_GAIN * SAND_TINT;
            let sparkCS = select(daySparkS, max(daySparkS, SAND_MOON), isMoon());
            col = mix(col, sparkCS, sparkS * columnS * gkS * 0.85 * irr.r);
          }
        }
        // ── THE SUN, REFLECTED OFF STONE ── see ROCK_SHEEN above for why this is a specular lobe and not the
        // water's glitter, and why it ADDS the sun's own colour instead of multiplying the rock's.
        // WHICH PIXELS: bit 12 of the slot word, set by TRACE from isRockV — the terrain strata (ROCK/ROCKX),
        // the medium boulder (BROCK), the 26 GLB boulders that scatter through BOTH the pine forest and the
        // desert, the desert_rocks set, the pickable field stone (PEBBLE), and a boulder chunk that has been
        // chopped loose and is falling as a rigid body. Six faces, not just the top: unlike a dune, a rock's
        // sunward WALL is the part that glares, so the top-face restriction the sand glisten wears is wrong.
        // WHY THE FLAT VOXEL NORMAL IS ENOUGH, and why there is no per-voxel micro-normal or mica sparkle on
        // top of it: reflect() takes rd, and rd is per-PIXEL, so the lobe already falls off smoothly ACROSS a
        // flat face with perspective — a real glare spot with a soft edge, not the uniform plateau a
        // face-constant term would give. A per-voxel hash was written and thrown away for two reasons: it
        // would have to be indexed in WORLD space here (the composite has no body-local cell, which is exactly
        // the coordinate the terrain grain in TRACE is careful to use), so it would CRAWL across any rock
        // chunk the physics moves; and the albedo already carries the terrain grain's +/-12% per voxel, so the
        // surface is mottled under the highlight whether or not the highlight is mottled too.
        // The two guards are not decoration. shR kills the lobe on faces the sun is behind, which reflect()
        // alone does not do — a mirror ray can still point at the sun through the wall it bounced off. irr.r
        // is the traced sun visibility, so a boulder in tree shade stays matte, and that is most of what sells
        // this as light landing on the rock rather than paint applied to the rock.
        if (ROCK_SHEEN && ((slRaw >> 12u) & 1u) != 0u && LG2(0u)) {
          let shR = dot(n, u.sunDir);
          if (shR > 0.0 && irr.r > 0.0) {
            let lobeR = pow(max(dot(reflect(rd, n), u.sunDir), 0.0), ROCK_GLOSS);
            let fresR = 1.0 + (ROCK_GRAZE - 1.0) * pow(1.0 - clamp(dot(-rd, n), 0.0, 1.0), 5.0);
            col += sunTintR() * select(ROCK_TINT, ROCK_MOONT, isMoon()) * (ROCK_SPEC * lobeR * fresR * shR * irr.r);
          }
        }
        // ── BACK-LIT FOLIAGE ── a needle is thin enough to pass light, and the whole reason a forest reads as
        // a forest when you look toward the sun is that the canopy GLOWS rather than silhouetting flat. Land
        // shading is pure lambert, so until now a leaf facing away from the sun was simply dark.
        // Two factors, and both have to be present: tr is the forward-scatter lobe, so this only fires when
        // you are looking INTO the sun, and wrap is how far the face is turned AWAY from it, which is exactly
        // the side light has to travel through. irr.r carries whether the sun actually reaches the far side —
        // the TRACE gate above was widened to shoot that ray for leaves, or it would be zero on every one of
        // these pixels and nothing would ever glow. So a leaf deep inside a crown stays dark and only the
        // canopy edge lights up, which is the real behaviour.
        if (FOLBACK_URL && ((slRaw >> 11u) & 1u) != 0u) {   // BAKED IN (user 2026-08-20: "bake in all the new graphic settings") — it was panel bit 6 for a few hours; ?nofol still compiles it out for an A/B
          let tr = pow(max(dot(rd, u.sunDir), 0.0), FOL_LOBE);      // rd runs FROM the eye INTO the scene, so looking toward the sun is dot(rd, sunDir) -> 1. Do NOT flip this: dot(-rd, ...) peaks when the sun is BEHIND you, which is exactly when wrap is 0, so the two factors can never both be large and the whole term goes dead.
          let wrap = clamp(-dot(n, u.sunDir), 0.0, 1.0);            // …how far the face is turned AWAY from the sun, which is the side the light has to travel through
          col += alb * vec3<f32>(1.15, 1.35, 0.70) * sunTintR() * (tr * wrap * irr.r * FOL_STR);   // transmitted light is warmer and more saturated than the reflected colour — it has been filtered by the leaf. irr.r is what keeps a leaf deep inside a crown dark: see the sunOrg note in TRACE, without which that term is zero on every pixel this fires on.
        }
        let glowY = u.camPos.y + rd.y * irr.b;                                                   // the shared 4-bit glow field: bedrock hits = LAVA orange, surface hits = FIREFLY warm yellow
        if (glowY < 28.0) { col += alb * lavaG * vec3<f32>(1.0, 0.44, 0.13) * 3.6; }             // lava: linear decode, unchanged
        else { col += alb * lavaG * lavaG * vec3<f32>(1.0, 0.82, 0.30) * 4.6; }                  // firefly: sqrt-encoded in TRACE → SQUARED decode restores the physical falloff curve
        col = worldFog(col, rd, irr.b);                            // ── ONE FOG (see worldFog in PRE) ── this was three byte-identical copies of the same four lines
      }
      // Distance to the nearest FOREGROUND surface drawn over the g-buffer (creature, drop, held item), or -1 if the scene
      // itself is what you see. The UNDERWATER block at the bottom needs this: it attenuates by the in-water path, and using
      // the SCENE's depth for a pixel covered by a fish 1.4 m away absorbed that fish as if it were the far lakebed.
      var fgT = -1.0;
      var dofHeld = false;                                         // …and whether that foreground surface is the TOOL IN YOUR HANDS, which depth of field leaves alone (see the store at the end of main)
      if (ITEMN > 0) {                                             // DROPPED ITEMS — hovering, spinning, world-scale voxels, depth-tested against the scene
        let ib = textureLoad(irrF, vec2<i32>(gid.xy), 0).b;
        var bestT = select(1e9, ib, ib > 0.0);
        var waterT = -1.0;                                           // primary hit is a WATER surface → its distance; a drop beyond it is UNDERWATER and composites THROUGH the surface (user: see the fish)
        var waterCol = col;                                          // the shaded surface color (body + sheen + glitter) — what the underwater hit blends toward with depth
        if (face == 6u && ib > 0.0) { waterT = ib; bestT = ib + 48.0; }   // extend the occlusion bound into the water — same 48-vox see-through range as the translucent bed
        let ndc3 = (px / u.res) * 2.0 - 1.0;
        let dc3 = normalize(vec3<f32>(ndc3.x * u.tanH * u.aspect, -ndc3.y * u.tanH, 1.0));
        let dropN = clamp(i32(u.pick2Y.w + 0.5), 9, DROP_N);             // JS COMPACTS live creatures into consecutive slots from 9 and passes the count — the loop never wastes pixels on empty slots (the fixed 64 loop tanked fps over busy water)
        let tiV = (wgid.y * ((u32(u.res.x) + 7u) / 8u) + wgid.x) * ${VIS_W}u;   // this workgroup IS one 8×8 tile — read its four prepass mask words (under ?uni the stride is 8: words 0-3 primary, 4-7 the grown SECONDARY mask)
        let visM0 = visb[tiV]; let visM1 = visb[tiV + 1u]; let visM2 = visb[tiV + 2u]; let visM3 = visb[tiV + 3u];   // FOUR words now (128 slots) and all four stay in REGISTERS: re-fetching the word from storage per iteration measured 4× the per-slot cost
        let lifeBase = i32(u.lifeCfg.w + 0.5);
        for (var di = 0; di < dropN; di++) {                         // slots 0..3 = dropped items, slot 4 = the flying cardinal, slots 5..8 = clash sparks, 9+ = live creatures (compacted)
          { let mw = select(select(visM0, visM1, di >= 32), select(visM2, visM3, di >= 96), di >= 64); let mrem = mw >> (u32(di) & 31u); if (mrem == 0u) { di = i32(u32(di) | 31u); continue; } if ((mrem & 1u) == 0u) { di += i32(countTrailingZeros(mrem)) - 1; continue; } }   // ── TILE CULL, BIT-SCANNED ── same mask, same slots visited, but the loop JUMPS to the next slot whose sphere touches this 8×8 tile rather than testing them one at a time. The mask words stay in registers on purpose: re-fetching the word from storage each iteration measured 4× the per-slot cost.
          let dXv = dropV(di * 4 + 1);
          let dit = i32(dXv.w + 0.5);
          if (dit < 1) { continue; }                                 // itemId checked FIRST — an empty slot costs one uniform load, not four
          // -- THE SLOT THE TRACE ACTUALLY INJECTS IS 4, NOT 8 (2026-08-24) -- this test exists to say "TRACE
          // already drew this one, do not draw it again analytically", so it has to name the same slots the
          // trace loop does: it starts at di = 4, the FLYING CARDINAL, and resumes at lifeBase. The drop
          // layout is 0-3 items, 4 cardinal, 5-8 clash sparks, then the particle band (render/buffers.js).
          // Naming 8 got both ends wrong: the cardinal at 4 was drawn TWICE, once by the trace with full
          // SVGF and again by this analytic path over the top of it, while the clash spark at 8 was claimed
          // as already-traced and skipped here -- and the trace skips it too, so nothing drew it at all.
          let tInj = u.lifeCfg.y > 0.5 && (di == 4 || di >= lifeBase) && face != 6u && (u32(lifeMotV(di).w + 0.5) & 1u) == 0u;   // ── DYNAMIC LIFE ── trace-injected creatures were ALREADY drawn by TRACE with full SVGF; the analytic path only remains for pixels that look THROUGH a water surface (Beer–Lambert) and for the analytic-flagged slots (fireflies). Creature base → 25 (20 death-burst slots 5-24: 4 sparks + 16 individual smoke voxels, user).
          // …with ONE exception: a model that carries TRANSLUCENT voxels still comes down here, because TRACE
          // walked straight past them (see the alpha test there) and something has to draw them over the pixel
          // it left behind. Only the translucent voxels are drawn on that pass — the opaque body is already in
          // the g-buffer, and re-shading it analytically would double it with a different lighting model. The
          // id ranges are measured at load, so every other creature keeps skipping this loop on two compares.
          // TWO ranges because the translucent models are not one block in the item table: the butterflies and
          // the dragonfly sit together near its head and the desert fly sits far down it, and spanning that gap
          // with a single range would send every duck, fish, songbird and land mammal in between down this DDA.
          if (tInj && (dit < TRA_LO || dit > TRA_HI) && (dit < TRA2_LO || dit > TRA2_HI)) { continue; }
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
          var iMapD = eOff + vcD.x + vcD.y * eW + vcD.z * eW * eD;   // running flat index — one add per DDA step
          for (var i = 0; i < PICKSTEPS; i++) {
            let cell = ITEMMAP[u32(iMapD)];
            if (cell.w > 0.0 && !(tInj && cell.w > 0.99)) {           // any COVERED voxel stops the walk — .w is alpha, so a plain greater-than-zero is the occupancy test. On the translucent second pass the OPAQUE voxels are stepped over instead: TRACE already put them in the g-buffer, and the depth test below (bestT = that same g-buffer distance) is what keeps a wing BEHIND the body from being drawn through it.
              if (tHit * vsD < bestT) {                              // scene + nearer-drop occlusion
                bestT = tHit * vsD;
                fgT = bestT;                                         // this pixel now shows a creature/drop — the underwater pass must absorb over THIS distance
                let behind = col;                                    // …and what the scene had here BEFORE it: the translucent blend at the end of this block mixes back toward exactly this
                var nl = vec3<f32>(0.0);
                if (vaxD == 0) { nl.x = -f32(istD.x); } else if (vaxD == 1) { nl.y = -f32(istD.y); } else { nl.z = -f32(istD.z); }
                let nc = dXv.xyz * nl.x + dYv.xyz * nl.y + dZv.xyz * nl.z;
                let nw = u.right * nc.x + u.up * nc.y + u.fwd * nc.z;
                if (di == 8 || di >= lifeBase) {                           // the CARDINAL + ALL WORLD CREATURES incl. worms + lily pads — the EXACT world surface model (shadowed sun + sky + bounce + fog). Creature base → 25 (20 death-burst slots 5-24).
                  var sunC = 0.0;                                    // REAL sun occlusion: a creature under the canopy sits in tree shade exactly like the ground below it
                  if (${location.search.includes('noshadow') ? 0 : 1} == 1 && dot(nw, u.sunDir) > 0.0 && u.sunDir.y > -0.04) {   // (deterministic un-jittered ray, no denoiser in the path — none of the irr-coupling translucency/shimmer; ?noshadow disables for A/B)
                    let cSp = u.camPos + rd * bestT + nw * 0.6;
                    let cCeil = f32((u32(u.fx) >> 8u) & 31u) * 32.0;   // same world-ceiling cap as the terrain sun ray — a flying bird is above most of it
                    let cCap = select(900.0, min(900.0, (cCeil - cSp.y) / max(u.sunDir.y, 1e-4)), u.sunDir.y > 1e-4);
                    if (cCap <= 0.0) { sunC = 1.0; }
                    else {
                    // skipW was hardcoded false, so for a creature UNDER the surface the first thing this ray
                    // hit was the water itself and every fish came back fully shadowed -- no direct term at all,
                    // lit by sky alone. Water is not an occluder for sunlight; the bed under it is lit through
                    // the surface by its own blit term. The select is false for anything above water, so a duck
                    // still takes tree shade exactly as before.
                    let shC = trace(cSp, u.sunDir, cCap, waterT > 0.0 && bestT > waterT);
                    sunC = select(1.0, 0.0, shC.t >= 0.0);
                    }
                  } else if (${location.search.includes('noshadow') ? 1 : 0} == 1) { sunC = 1.0; }
                  let direct = sunTintR() * (sunC * max(dot(nw, u.sunDir), 0.0));
                  let skyIrr = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale();
                  let bounceC = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * nw.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());
                  var alb2 = cell.rgb;                                 // DUCK EYE BLINK: the black eye voxel flashes the head-GREEN when the slot's blink lane (dYv.w) is lit (user)
                  if (${DUCK_ITEM0 > 0 ? 1 : 0} == 1 && dYv.w > 0.5 && (dit == ${DUCK_ITEM0 || 9999} || dit == ${DUCKB_ITEM0 || 9998}) && alb2.r < 0.02 && alb2.g < 0.02 && alb2.b < 0.02) { alb2 = vec3<f32>(${DUCK_GREEN[0].toFixed(4)}, ${DUCK_GREEN[1].toFixed(4)}, ${DUCK_GREEN[2].toFixed(4)}); }
                  let grain = 0.95 + 0.10 * ih3(vcD.x, vcD.y, vcD.z);   // per-voxel texture — GENTLE (±5%) so authored colour transitions dominate the noise (user); hashed on the MODEL-local voxel so it's stable as the creature drifts/rotates
                  // ── CAUSTIC WEBS ON A SUBMERGED CREATURE (user 2026-08-08: "increase the quality of the fish
                  // through the water") ── the lakebed under this same water gets caustics multiplied into its
                  // albedo (see the refraction block); a fish swimming just above that bed got none, so the one
                  // cue that most says "this is underwater" was missing from the animal and present on the
                  // ground behind it. Same caust() call, same 1.6 gain, keyed on the creature's OWN sun
                  // visibility rather than the surface's, so a fish under a tree's shadow is not painted with
                  // sunlit webs. Gated on being submerged: a duck, a bird and every land animal are untouched.
                  //
                  // ── AND THE LIGHT REACHING IT IS *NOT* ATTENUATED HERE (tried twice, rejected by eye) ──
                  // it looks obviously right on paper: the sun crosses the water column before it reaches the
                  // fish, so absorb it over depth/sinAlt. Both a full-strength version and a 0.35-scaled one
                  // turned the fish into a black silhouette, because the composite further down ALREADY
                  // absorbs the view path back to the eye — the depth was being paid for twice — and the slant
                  // path multiplies it again at a low sun. The bed does not attenuate its own incoming term
                  // either, for exactly this reason. If this is revisited, the missing piece is a scattered
                  // in-water light term to fade toward, not a bigger exponent.
                  var alb3 = alb2;
                  if (waterT > 0.0 && bestT > waterT && LG2(2u)) {                 // …and lgt.z bit 2 switches it off with the rest of the caustics: this whole block IS the webs on a submerged creature, so the gate belongs on the condition rather than inside
                    let wpC = u.camPos + rd * bestT;
                    let caC = caust(floor(vec2<f32>(wpC.x + u.winO.x, wpC.z + u.winO.y)) + vec2<f32>(0.5));
                    alb3 = alb2 * (1.0 + 1.6 * caC * smoothstep(-0.02, 0.12, u.sunDir.y) * sunC * (1.0 - waterIceK));   // …and the webs stop as the lid closes: caustics need a moving surface to focus the sun through
                  }
                  col = alb3 * (direct + (skyIrr + bounceC) * 0.85 + ambFloor()) * grain;   // 0.85 ≈ the terrain's typical open-air AO term — same formula as the static world (irr coupling stays banned: it read as translucent)
                  if (${WORM_ITEM0 > 0 ? 1 : 0} == 1 && dit >= ${WORM_ITEM0 || 9999} && dit < ${(WORM_ITEM0 || 9999) + (WORM_NFRAMES || 1)}) {   // WORM fake-AO — the smooth off-grid worm's stand-in for the grid-stamped cardinal's REAL contact AO (user chose 'fake AO, stay smooth'): darken the underside + lower body so it reads GROUNDED, no grid-stamp shimmer
                    let upN = clamp(0.5 + 0.5 * nw.y, 0.0, 1.0);              // 1 = top-facing, 0 = down-facing (the shaded underside against the ground)
                    let hN = f32(vcD.z) / max(1.0, f32(eH - 1));             // 0 = ground side of the body, 1 = its top
                    col = col * mix(0.5, 1.0, 0.65 * upN + 0.35 * smoothstep(0.0, 0.9, hN));   // contact shadow: darkest at the bottom + underside, full-lit on top
                  }
                  if (${FFLY_ITEM0 > 0 ? 1 : 0} == 1 && dit >= ${FFLY_ITEM0 || 9999} && dit < ${(FFLY_ITEM0 || 9999) + (FFLY_NFRAMES || 1)} && dYv.w > 0.0 && cell.r > 0.6 && cell.g > 0.4 && cell.b < 0.1) { col = cell.rgb * (0.4 + dYv.w); }   // FIREFLY abdomen — yellow voxel EMISSIVE (FFLY-range-gated so a blinking duckling's yellow doesn't glow)
                  var fogB = 1.0 - exp(-bestT * 0.0006);
                  fogB = max(fogB, smoothstep(u.rdist.x * 0.62, u.rdist.x - 6.0, bestT * length(vec2<f32>(rd.x, rd.z))));
                  if (!LG(5u)) { fogB = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
                  col = mix(col, skyBaseR(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogB);
                  if (${FFLY_ITEM0 > 0 ? 1 : 0} == 1 && dit >= ${FFLY_ITEM0 || 9999} && dit < ${(FFLY_ITEM0 || 9999) + (FFLY_NFRAMES || 1)} &&
                      cell.r > 0.88 && cell.g > 0.88 && cell.b > 0.88) { col = mix(behind, col, 0.6); }   // FIREFLY WINGS (the white voxels) — 40% translucent
                } else if (di >= 9) {                                // slots 5-24: clash/death SPARKS + death SMOKE (dYv.w = fade)
                  if (dit == ${SMOKE_IT || 9997}) {                  // DEATH SMOKE — each slot is ONE individual white VOXEL (like a snowflake), 24% opacity, fading (col here still holds the scene BEHIND it → mix = true translucency). Off-grid look comes from its own continuous position + snowflake spin in the emit.
                    col = mix(col, vec3<f32>(1.0), 0.24 * dYv.w);
                  } else if (dit == ${HITRED_IT || 9996}) {          // ── BLOOD ── the SAME red the animal itself flashes (user 2026-08-05). It reuses the hit
                    // flash's own albedo constant rather than a palette colour, so the two can never drift apart, and
                    // it is lit the way the flash lights the animal: the flash floors sun visibility at 0.55 and sky at
                    // 0.75 of its strength, so a blood voxel carries those same floors. That is what makes a voxel in
                    // the air read as the same material as the red on the animal it came off. It does NOT pulse or fade
                    // with dYv.w — the flash is a steady colour for the half second it lasts, and so is this.
                    let bDir = sunTintR() * max(dot(nw, u.sunDir), 0.0) * 0.55;
                    let bSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale() * 0.75;
                    col = HURT_RED * (bDir + bSky + ambFloor());
                  } else if (dit == ${FOAM_IT || 9995}) {            // ── SPLASH ── a droplet of the SAME foam the shoreline draws (FOAM_C), so a burst
                    // off the surface reads as water torn off the water rather than a white speck. Lit as a
                    // diffuse surface, NOT emissive like a spark: foam does not glow. It thins out as it dies
                    // (mix toward the scene behind), which is what sells it as spray rather than a solid cube.
                    let sDir = sunTintR() * max(dot(nw, u.sunDir), 0.0) * 0.55;
                    let sSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale() * 0.85;
                    col = mix(col, FOAM_C * (sDir + sSky + vec3<f32>(0.02, 0.022, 0.025)), 0.35 + 0.65 * dYv.w);
                  } else if (dit == ${PETAL_IT || 9994} || dit == ${PETALW_IT || 9993} || dit == ${PETALG_IT || 9992} || dit == ${PETALGL_IT || 9991} || dit == ${PETALN_IT || 9990}) {   // ── FALLING LEAF ── the two GREENS join the two blossoms here (user 2026-08-19): the branch is about how a scrap of canopy is LIT, which is the same question whatever colour it is, and a green leaf that missed this test would fall out of the else as an emissive spark and glow in the dark. ── a petal is not a spark: it does not glow, it is a scrap of the
                    // canopy catching the light. So it is lit as a DIFFUSE surface off its own item colour,
                    // the way the splash droplet is lit off the foam's, and it does NOT fade with dYv.w —
                    // it lands and is gone at full colour rather than dissolving in the air.
                    let ptDir = sunTintR() * max(dot(nw, u.sunDir), 0.0) * 0.75;
                    let ptSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale() * 0.9;
                    col = cell.rgb * (ptDir + ptSky + vec3<f32>(0.02, 0.02, 0.024));
                  } else {                                           // SPARK — emissive 10 cm ember
                    col = cell.rgb * (0.8 + 2.6 * dYv.w);
                  }
                } else {                                             // ── DROPPED ITEMS ── (user 2026-08-07: "can you ray trace floating objects")
                  // They were always trace-injected — they come down this very DDA — they just opted OUT of
                  // lighting: a flat lambert with no shadow ray, no sky term, no bounce and no grain, which is
                  // why a dropped mushroom read as a sticker pasted over the forest instead of an object in it.
                  // This is the SAME formula the creature branch above uses, so a drop now sits in tree shade,
                  // takes the sky's colour on its upper faces and picks up ground bounce underneath.
                  var sunD = 0.0;
                  if (${location.search.includes('noshadow') ? 0 : 1} == 1 && dot(nw, u.sunDir) > 0.0 && u.sunDir.y > -0.04) {
                    let dSp = u.camPos + rd * bestT + nw * 0.6;
                    let dCeil = f32((u32(u.fx) >> 8u) & 31u) * 32.0;
                    let dCap = select(900.0, min(900.0, (dCeil - dSp.y) / max(u.sunDir.y, 1e-4)), u.sunDir.y > 1e-4);
                    if (dCap <= 0.0) { sunD = 1.0; }
                    else { let dsh = trace(dSp, u.sunDir, dCap, false); sunD = select(1.0, 0.0, dsh.t >= 0.0); }
                  } else if (${location.search.includes('noshadow') ? 1 : 0} == 1) { sunD = 1.0; }
                  let dDirect = sunTintR() * (sunD * max(dot(nw, u.sunDir), 0.0));
                  let dSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale();
                  let dBounce = select(vec3<f32>(0.0), BOUNCE, LG(14u)) * clamp(0.55 - 0.55 * nw.y, 0.0, 1.0) * max(u.sunDir.y, 0.0) * 2.2 * select(1.0, 0.12, isMoon());
                  let dGrain = 0.95 + 0.10 * ih3(vcD.x, vcD.y, vcD.z);
                  col = cell.rgb * (dDirect + (dSky + dBounce) * 0.85 + ambFloor()) * dGrain;
                }
                // ── ONLY WHAT IS BEHIND THE SURFACE ── keyed on waterT, i.e. on this pixel's primary hit being
                // the water surface. That IS a discontinuity: where the pixel behind a fish shows the bed or the
                // far shore instead, the same fish draws untinted, and panning across that edge pops it. A
                // geometric replacement (measure the submerged path from the waterline crossing) was tried on
                // 2026-08-07 and REVERTED — with no surface pixel to borrow a colour from it saturated toward a
                // stand-in tint that is brighter than the water, turning every fish into a pale cyan blob. Any
                // retry needs a real water colour for that case, not a scaled constant.
                if (waterT > 0.0 && bestT > waterT) {                // the hit sits UNDER the water surface, seen THROUGH it
                  // ── COMPOSITED LIKE THE BED, NOT LIKE A DECAL ── the lakebed reads correctly through this
                  // exact water, so a fish in front of it should be built the same way, and it was not:
                  //   · it faded toward waterCol, the FINISHED surface colour with the sky mirror already
                  //     mixed in, so depth pushed it toward a bright grey instead of into the water;
                  //   · it never took the Fresnel split at all, so the mirror arrived as a function of DEPTH
                  //     rather than of viewing angle;
                  //   · and its extinction was a second literal that missed the 2026-08-06 re-tune.
                  // Now: Beer-Lambert on the shared constant, fade into the in-scatter the bed fades into,
                  // then the same Fresnel mix against the same mirror. Straight down the fish is clear; at a
                  // grazing angle the reflection covers it, which is what water actually does.
                  let trF = exp(-WATER_SIG * (bestT - waterT));
                  let through = col * trF + waterScat * (vec3<f32>(1.0) - trF);
                  col = mix(mix(through, waterRefl, waterFres), waterIceC, waterIceK);   // …then the ICE goes over the top, exactly as it goes over the surface, so a freezing lake closes over the fish on the same 5 s ramp instead of leaving them printed on it
                }
                // ── AND THE DISTANCE FOG, WHICH THE SCENE GETS AND THIS DID NOT ── the fog block above runs on
                // the g-buffer BEFORE this DDA and uses the SCENE's depth, so every creature and drop was
                // composited over an already-hazed world carrying no haze of its own. A fish thirty voxels out
                // read as a crisp sticker pasted on a soft background, and it got worse the further away it was
                // (user 2026-08-07). Same curve, same horizon roll-off, measured against the creature's OWN
                // distance. The held tool is centimetres away, so its fog term is zero and it is unaffected.
                var fogC = 1.0 - exp(-bestT * 0.0006);
                fogC = max(fogC, smoothstep(u.rdist.x * 0.62, u.rdist.x - 6.0, bestT * length(vec2<f32>(rd.x, rd.z))));
                if (!LG(5u)) { fogC = 0.0; }                               // LIGHT DEBUG bit 5: distance fog
                col = mix(col, skyBaseR(normalize(vec3<f32>(rd.x, max(rd.y, 0.02), rd.z))), fogC);
                // (No submerged tint here: the UNDERWATER block at the end of main now absorbs this pixel over fgT — the
                //  creature's OWN in-water path — so it dims exactly like the world does, with no double attenuation.)
                // ── PER-VOXEL ALPHA (the fly's wings 50%, the dragonfly's 50%, the butterfly's 72%) ── LAST, after water and fog, so the wing and the scene
                // behind it have had the same terms applied and the mix is a true coverage blend rather than a
                // blend of two differently-hazed images. It is a real average of two colours, not a stochastic
                // or dithered cutout, so there is nothing for the denoiser or TAA to resolve and the wing
                // cannot sparkle as the fly moves. Opaque voxels have .w = 1 and the mix is the identity.
                if (cell.w < 0.99) { col = mix(behind, col, cell.w); }
              }
              break;
            }
            if (vNxD.x <= vNxD.y && vNxD.x <= vNxD.z) { tHit = vNxD.x; vNxD.x += abs(invD.x); vcD.x += istD.x; iMapD += istD.x; vaxD = 0; }
            else if (vNxD.y <= vNxD.z) { tHit = vNxD.y; vNxD.y += abs(invD.y); vcD.y += istD.y; iMapD += istD.y * eW; vaxD = 1; }
            else { tHit = vNxD.z; vNxD.z += abs(invD.z); vcD.z += istD.z; iMapD += istD.z * eW * eD; vaxD = 2; }
            if (any(vcD < vec3<i32>(0)) || any(vcD >= vec3<i32>(eW, eD, eH))) { break; }
          }
        }
      }
      if (ITEMN > 0) {                                             // HELD ITEMS — RIGHT hand + LEFT hand (dual-wield rocks) as TRUE 3D voxels, DDA-walked in camera space; the nearer hand wins overlaps
        var heldT = 1e18;
        let ndc2 = (px / u.res) * 2.0 - 1.0;
        let dc = normalize(vec3<f32>(ndc2.x * u.tanH * u.aspect, -ndc2.y * u.tanH, 1.0));
        for (var hand = 0; hand < 3; hand = hand + 1) {   // 0 = right, 1 = left, 2 = the CRAFT PREVIEW hovering between them
          var pA = u.pickA; var pX = u.pickX; var pY = u.pickY; var pZ = u.pickZ;
          if (hand == 1) { pA = u.pick2A; pX = u.pick2X; pY = u.pick2Y; pZ = u.pick2Z; }
          if (hand == 2) { pA = u.pick3A; pX = u.pick3X; pY = u.pick3Y; pZ = u.pick3Z; }
          if (pX.w < 0.5) { continue; }
          let it = clamp(i32(pX.w + 0.5) - 1, 0, ITEMN - 1);
          let PICKW = ITEMD[it].x; let PICKD = ITEMD[it].y; let PICKH = ITEMD[it].z; let IOFF = ITEMD[it].w;
          let C = pA.xyz;
          let vs = pA.w;
          let hw = f32(PICKW) * 0.5; let hd = f32(PICKD) * 0.5; let hh = f32(PICKH) * 0.5;
          let rad = vs * (sqrt(hw * hw + hd * hd + hh * hh) + 1.0);
          let tc = dot(C, dc);
          if (PICKW > 0 && tc > 0.0 && length(dc * tc - C) < rad) {
            // item-local grid space (voxel units): x along pX (width), y along pY (depth), z along pZ (height)
            let roL = vec3<f32>(-dot(C, pX.xyz), -dot(C, pY.xyz), -dot(C, pZ.xyz)) / vs + vec3<f32>(hw, hd, hh);
            var rdL = vec3<f32>(dot(dc, pX.xyz), dot(dc, pY.xyz), dot(dc, pZ.xyz));
            if (abs(rdL.x) < 1e-6) { rdL.x = 1e-6; }
            if (abs(rdL.y) < 1e-6) { rdL.y = 1e-6; }
            if (abs(rdL.z) < 1e-6) { rdL.z = 1e-6; }
            let invL = 1.0 / rdL;
            let ta2 = -roL * invL;
            let tb2 = (vec3<f32>(f32(PICKW), f32(PICKD), f32(PICKH)) - roL) * invL;
            let tn2 = min(ta2, tb2); let tf2 = max(ta2, tb2);
            let te = max(max(tn2.x, tn2.y), max(tn2.z, 0.0));
            let tl = min(min(tf2.x, tf2.y), tf2.z);
            if (te < tl) {
              var vax = 0;
              if (tn2.y == te) { vax = 1; } if (tn2.z == te) { vax = 2; }
              var vc = clamp(vec3<i32>(floor(roL + rdL * (te + 1e-4))), vec3<i32>(0), vec3<i32>(PICKW - 1, PICKD - 1, PICKH - 1));
              let istep = vec3<i32>(sign(rdL));
              var vNext = (vec3<f32>(vc + max(istep, vec3<i32>(0))) - roL) * invL;
              var tCur = te;
              for (var i = 0; i < PICKSTEPS; i++) {
                let cell = ITEMMAP[IOFF + vc.x + vc.y * PICKW + vc.z * PICKW * PICKD];
                if (cell.w > 0.5) {
                  if (tCur * vs < heldT) {                         // both hands can cover a pixel (the spark clash) — keep the nearer surface
                    heldT = tCur * vs;
                    dofHeld = true;                                 // this pixel IS the held tool — depth of field must not touch it (see the store at the end of main)
                    if (fgT < 0.0 || heldT < fgT) { fgT = heldT; }   // the tool in your hands is centimetres away — it must not be absorbed like the far lakebed either
                    var nl = vec3<f32>(0.0);                       // CUBE face normal from the axis the ray entered through — real edges, not a flat card
                    if (vax == 0) { nl.x = -f32(istep.x); } else if (vax == 1) { nl.y = -f32(istep.y); } else { nl.z = -f32(istep.z); }
                    let nc = pX.xyz * nl.x + pY.xyz * nl.y + pZ.xyz * nl.z;
                    let nw = u.right * nc.x + u.up * nc.y + u.fwd * nc.z;
                    var aoF = 1.0;
                    if (i32(pX.w + 0.5) == 1) {                     // AXE ONLY (user): cheap voxel cavity AO — the exposed face darkens where the 4 in-plane neighbours are solid (head↔handle join + crevices). Geometry-based, so it adds depth WITHOUT scrambling the hand-authored gradient like grain would
                      var nlo = vec3<i32>(0);
                      if (vax == 0) { nlo.x = -istep.x; } else if (vax == 1) { nlo.y = -istep.y; } else { nlo.z = -istep.z; }
                      let oc = vc + nlo;               // the empty cell just outside the hit face
                      var t1 = vec3<i32>(0); var t2 = vec3<i32>(0);
                      if (vax == 0) { t1.y = 1; t2.z = 1; } else if (vax == 1) { t1.x = 1; t2.z = 1; } else { t1.x = 1; t2.y = 1; }
                      let dims = vec3<i32>(PICKW, PICKD, PICKH);
                      var occ = 0;
                      for (var s = 0; s < 4; s = s + 1) {
                        var p = oc + t1;
                        if (s == 1) { p = oc - t1; } else if (s == 2) { p = oc + t2; } else if (s == 3) { p = oc - t2; }
                        if (all(p >= vec3<i32>(0)) && all(p < dims) && ITEMMAP[IOFF + p.x + p.y * PICKW + p.z * PICKW * PICKD].w > 0.5) { occ = occ + 1; }
                      }
                      aoF = 1.0 - 0.14 * f32(occ);    // up to ~0.44 in a full crevice; ~0.86–0.72 for typical creases
                    }
                    // ── THE CRAFT PREVIEW GOES RED WHEN IT CANNOT BE AFFORDED (user 2026-08-20) ── the whole tool,
                    // not an outline or a tint: pick3Y.w is 1 while the hotbar is short of what this recipe costs
                    // (see the lane's note in tick-camera). HURT_RED is the animal hit flash's own constant rather
                    // than a second red of its own, which is what the user asked for — "similar to how the life
                    // voxels look when the player hits it" — and means the two can never drift apart.
                    // AND IT IS LIT THE WAY THE FLASH LIGHTS THE ANIMAL (user 2026-08-20: "is the red … the
                    // exact same … as the life when they have been hit? if not make it so"). The flash's rule is
                    // two FLOORS on the wounded surface's own visibility — sun >= 0.55, sky >= 0.75 (see the
                    // hurtGlow line in TRACE) — and those two maxes are transcribed here onto heldCfg.x/.y,
                    // which are the viewmodel's equivalents of exactly those two scalars. So the tool now
                    // carries the same albedo constant AND the same lighting rule as the animal, and keeps its
                    // own face shading through aoF, which is the whole point of a FLOOR: the trace note is
                    // explicit that pinning the terms to full flattened the animal and blew it out.
                    //   * NOT the BLOOD voxel's version of the same two lines (HITRED_IT above), even though it
                    //     exists for this reason: a blood drop is a lone voxel in the air with no shadow test,
                    //     so it PINS the two at 0.55/0.75 rather than flooring them. Measured against a real
                    //     wounded animal standing in open sun that is 31/255 too DARK, because the animal's own
                    //     sun visibility there is ~1.0 and max() keeps it.
                    //   * the min() ceiling stays. heldLight runs past 1.0 in open sun and HURT_RED's own note
                    //     says what happens then: the red channel saturates and the tonemap flattens it to pale
                    //     salmon, which is what the first cut of this did.
                    // The two can never be bit-identical and should not be chased any further: a creature is a
                    // TRACED surface that goes through SVGF and TAA, and these are analytic composite pixels
                    // that deliberately never enter the denoiser (see the health-row note below).
                    let noPay = (hand == 2 && pY.w > 0.5);
                    var lit5 = heldLight(nw) * aoF;
                    if (noPay) {
                      let hDir = sunTintR() * max(dot(nw, u.sunDir), 0.0) * max(u.heldCfg.x, 0.55) * select(0.0, 1.0, LG(16u));
                      let hSky = mix(HORIZON, ZENITH, 0.5 + 0.5 * nw.y) * 0.95 * dayScale() * max(select(1.0, u.heldCfg.y, LG(1u)), 0.75);
                      lit5 = min((hDir + hSky + ambFloor()) * aoF, vec3<f32>(1.0));
                    }
                    col = select(cell.rgb, HURT_RED, noPay) * lit5;             // world-matched sun + sky + ground bounce, gated by the eye's own marched sun/sky visibility — see heldLight in PRE. The health row calls the SAME function, which is the whole reason it lives there and not here.
                  }
                  break;
                }
                if (vNext.x <= vNext.y && vNext.x <= vNext.z) { tCur = vNext.x; vNext.x += abs(invL.x); vc.x += istep.x; vax = 0; }
                else if (vNext.y <= vNext.z) { tCur = vNext.y; vNext.y += abs(invL.y); vc.y += istep.y; vax = 1; }
                else { tCur = vNext.z; vNext.z += abs(invL.z); vc.z += istep.z; vax = 2; }
                if (any(vc < vec3<i32>(0)) || any(vc >= vec3<i32>(PICKW, PICKD, PICKH))) { break; }
              }
            }
          }
        }
      }
      // -- HEALTH: FIVE VOXELS CARRIED IN FRONT OF THE EYE --
      // single.vox, five times, hanging below the crosshair and riding the same view-model frame the tools do
      // (tick-camera adds heldBob to the anchor). The DDA below is the held item's, one instance per heart, and
      // the shading is heldLight() -- the stone tools' own. Three things about WHERE this block sits are
      // load-bearing:
      //   * AFTER the held items, so a heart is never buried under the axe when a swing drives it to centre;
      //   * with its OWN depth bound (bestH), never the scene's bestT, so the readout cannot be occluded by a
      //     wall you walk into - a health bar a hillside can hide is not a health bar;
      //   * in COMPOSITE at all, rather than trace-injected like the creatures. This is where the STONE TOOLS
      //     are drawn too, so "in the game like the stone tools" means HERE, not in the tracer. It also has to
      //     be: trace-injected geometry goes through SVGF, and a surface pinned to the camera moves in WORLD
      //     space every frame, which is the one thing that makes the denoiser throw its history away - that is
      //     the shimmer the held viewmodel had. Analytic pixels are drawn after the denoiser and never enter
      //     it, so there is no history to lose and the hearts are as steady as the axe.
      // ── THE HEART ROW IS GONE (user 2026-08-16: "remove the voxel hearts. keep the mechanics but
      // remove the hearts.") ── the DRAW is what went; sim/vitals.js is untouched and still ticks health,
      // hunger, saturation, exhaustion, regen and starvation exactly as before. __vb.vit() reads them.
      // The uniform lane and the HEART_* constants above are left in place and tick-camera still fills them
      // every frame — nothing now READS them, which costs one lane and no shader work. Restoring the row means
      // re-adding this block, not rebuilding the feature.
      if (u.lifeCfg.x > 0.5) {                                       // ── DYNAMIC-LIFE DEBUG VIEWS (__vb.lifedbg) ── 1: slot ids (occupancy/object identity) · 2: history confidence (red = rejected/fresh, green = converged) · 3: per-slot motion vectors · 4: raw denoised AO
        let dbgm = i32(u.lifeCfg.x + 0.5);
        let slD = textureLoad(slotT, vec2<i32>(gid.xy), 0).r & 255u;
        let irrD = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        if (dbgm == 1 && slD != 0u) { col = mix(col, vec3<f32>(fract(f32(slD) * 0.61803), fract(f32(slD) * 0.3247 + 0.33), fract(f32(slD) * 0.7548 + 0.66)), 0.72); }
        else if (dbgm == 2) {                                          // history confidence — RED fresh, GREEN converged, and BLUE for a pixel with NO g-buffer depth at all
          if (irrD.b > 0.0) { let hc = clamp(irrD.a / max(u.maxHist, ${AO_HIST}.0), 0.0, 1.0); col = mix(vec3<f32>(0.9, 0.05, 0.05), vec3<f32>(0.05, 0.85, 0.15), hc); }   // …divided by the LONGER of the two ceilings (see AO_HIST in denoise.js): the stored counter now runs to the AO window, so dividing by the sun's alone would paint every settled pixel full green and the view would stop discriminating
          else { col = vec3<f32>(0.05, 0.25, 1.0); }                     // t <= 0: TEMPORAL skips it, so it gets no irradiance (renders black) AND the creature occlusion bound goes infinite (they draw through). If the bad shadow lights up BLUE, that is the bug.
        }
        else if (dbgm == 3 && slD != 0u) { let mv = lifeMotV(i32(slD) - 1).xyz; col = clamp(abs(mv) * 2.5, vec3<f32>(0.06), vec3<f32>(1.0)); }
        else if (dbgm == 4 && irrD.b > 0.0) { col = vec3<f32>(irrD.g); }
        else if (dbgm == 5 && irrD.b > 0.0) { col = vec3<f32>(irrD.r); }   // DENOISED sun visibility alone — no AO, no albedo. If the blobby dark regions are ABSENT here, they are the AO term, not a shadow; if they are present and still blobby, the filter is smearing a hard shadow.
      }
      if ((u32(u.fx) & 2u) != 0u && LG2(3u)) {                        // ── UNDERWATER ── lgt.z bit 3 (the panel's underwater row) takes the whole submerged look off in one gate — absorption AND the marched scatter — because half of it is not a look anyone would want: Beer-Lambert with no in-scatter is a black lake, and in-scatter with no absorption is fog over a perfectly clear one. Off, swimming looks like standing in air. (camera submerged) Beer–Lambert absorption over the in-water path + a RAY-MARCHED single-scatter with caustic-modulated sun shafts (replaces the old flat blue tint)
        let irrU = textureLoad(irrF, vec2<i32>(gid.xy), 0);
        var wD = select(1e4, irrU.b, irrU.b > 0.0);                  // in-water path toward the hit (sky pixels: the whole march range)
        if (fgT > 0.0) { wD = fgT; }                                 // …but if a CREATURE / drop / held item is what this pixel actually shows, the water only reaches THAT far. Using the scene depth here absorbed a fish 14 vox away as if it were the 160-vox background (exp(-0.062*160) ≈ 5e-5) — fish, drops and the held tool all vanished the moment you swam under. This is what "I can't see the fish underwater" was.
        if (rd.y > 0.001) { wD = min(wD, max((WLF + 1.0 - u.camPos.y) / rd.y, 0.0)); }   // the ray exits through the surface — only the submerged stretch attenuates
        wD = clamp(wD, 0.0, 160.0);
        let sigU = vec3<f32>(0.16, 0.062, 0.030);                    // gentler than the surface view — swimming has to stay playable
        let trU = exp(-sigU * wD);
        var seedW = ((gid.x * 7817u) ^ (gid.y * 45589u) ^ (u32(u.frame) * 2657u)) | 1u;
        var accW = vec3<f32>(0.0);
        let sunUp = smoothstep(-0.02, 0.12, u.sunDir.y);
        let dtW = wD / 4.0;                                          // 4 jittered steps (was 6) — the estimator is unbiased, so TAA converges to the SAME image; only the per-frame noise rises a hair (near-lossless)
        var tw2 = dtW * rand(&seedW);                                // jittered march start — TAA melts the step banding
        let sunInv = 1.0 / max(u.sunDir.y, 0.25);
        for (var si = 0; si < 4; si++) {
          let p = u.camPos + rd * tw2;
          let dBelow = max(WLF + 1.0 - p.y, 0.0);
          let lightT = exp(-sigU * dBelow * sunInv);                 // the sun's own path through the water down to this point
          var caW = 1.0;                                              // …and with caustics off the shafts flatten to an even column rather than vanishing: 1.0 is what 0.45 + 1.75*caust averages to, so the water keeps its brightness and only loses the dancing
          if (LG2(2u)) { caW = 0.45 + 1.75 * caust(floor(vec2<f32>(p.x + u.winO.x + dBelow * u.sunDir.x * sunInv, p.z + u.winO.y + dBelow * u.sunDir.z * sunInv)) + vec2<f32>(0.5)); }   // project along the sun to the surface → dancing god-ray shafts
          accW += vec3<f32>(0.020, 0.078, 0.098) * (0.22 + 0.78 * sunUp * caW) * lightT * dayScale() * exp(-sigU * tw2) * (sigU * dtW * 2.6);
          tw2 += dtW;
        }
        col = col * trU + accW;
      }
      col = aces(col * 0.95);
      col = pow(col, vec3<f32>(1.0 / 2.2));
      // ── VOLUMETRICS ── added on top of the shaded image, the way Teardown composites its volumetric
      // buffer alongside diffuse and specular. Gated on the lane holding anything at all, so a scene
      // with no embers, fireflies or wounded animals in it pays one compare.
      if (LG(17u)) {
        var anyL = false;
        for (var f = 0; f < 8; f++) { if (u.fflies[f].w > 0.0) { anyL = true; break; } }
        if (anyL) {
          let vD = textureLoad(irrF, vec2<i32>(gid.xy), 0).b;      // distance to whatever this pixel shows
          let wDepth = select(90.0, vD, vD > 0.0);                 // stop the march at that surface — past it the medium is behind geometry
          // interleaved-gradient noise, inlined: ign() lives in the tracer module, not here
          let ig = fract(52.9829189 * fract(0.06711056 * f32(gid.x) + 0.00583715 * f32(gid.y)));
          let dth = fract(ig + f32(u32(u.frame) & 31u) * 0.7548777);   // …rotated per frame so TAA averages the march offsets away
          col += volLight(u.camPos, rayDir(vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y)), wDepth, dth);
        }
      }
      // ── DEPTH OF FIELD ── alpha carries the signed circle of confusion, encoded to 0..1. It is computed HERE
      // rather than in the blit because this is the only pass that knows what the pixel actually SHOWS: the
      // g-buffer distance behind a fish, a dropped rock or the held axe is the hillside they stand against, and
      // blurring a foreground object by the depth of the background behind it is precisely the halo artefact
      // depth of field is notorious for. The held tool is exempted outright: it sits two voxels from the lens, so
      // any focal plane out in the world puts it at the near stop, and a permanently smeared axe is not depth —
      // it is just a blurry axe. Everything else blurs by its own distance.
      var dofD = textureLoad(irrF, vec2<i32>(gid.xy), 0).b;          // scene depth; < 0 = sky
      if (fgT > 0.0) { dofD = fgT; }                                 // …but a creature, a dropped item or the held tool is nearer, and IT is what you see
      let cocS = select(dofCoc(dofD), 0.0, dofHeld);
      textureStore(colorOut, vec2<i32>(gid.xy), vec4<f32>(col, cocS * 0.5 + 0.5));
    }
  `;

