  const BLIT_SRC = () => /* wgsl */`
    const HURTV_GRID : vec2<f32> = vec2<f32>(64.0, 36.0);   // the hurt flash's block grid — the resolution the old DOM canvas was painted at, kept so the blocks are the same size they always were
    @group(0) @binding(1) var src : texture_2d<f32>;
    @group(0) @binding(2) var samp : sampler;
    @group(0) @binding(3) var dofT : texture_2d<f32>;
    @vertex fn vs(@builtin(vertex_index) vi : u32) -> @builtin(position) vec4<f32> {
      var P = array<vec2<f32>, 3>(vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0), vec2<f32>(-1.0, 1.0));
      return vec4<f32>(P[vi], 0.0, 1.0);
    }
    @fragment fn fs(@builtin(position) fc : vec4<f32>) -> @location(0) vec4<f32> {
      let uv = fc.xy / u.canvasRes;
      let src4 = textureSampleLevel(src, samp, uv, 0.0);            // .a = the SKY mask the TAA pass wrote (1 = sky) - free, this fetch was already here for the colour
      var col = src4.rgb;
      // ── DEPTH OF FIELD ── scatter-as-gather over a disc whose radius IS this pixel's circle of confusion.
      // dofT carries {colour, signed CoC} together, so a tap costs one fetch. A tap contributes only while its
      // OWN circle still reaches this pixel — that one test is what stops the blurred background from eating the
      // sharp silhouette standing in front of it, because a tap on a focused surface has a circle of nothing and
      // cannot spread anywhere. Pixels under ~0.8 px of blur skip the loop outright, which on an ordinary frame
      // is almost all of them: the effect costs a single compare wherever the picture is in focus.
      // It runs FIRST so the flare ghosts and the vignette below land on the finished image —
      // a flare is made at the lens, not at the subject, and blurring one along with the scene reads as a smudge.
      if (u.dof.x > 0.0) {
        let cocA = textureSampleLevel(dofT, samp, uv, 0.0).a;       // this pixel's own SIGNED circle of confusion, encoded to 0..1
        var R = abs(cocA * 2.0 - 1.0) * u.dof.y;
        // ── AND IT EASES OFF AT THE FRAME EDGE (user 2026-08-20, bit 7) ── near-field geometry clipped by the
        // border reads as a SMEAR rather than as a lens: there is no in-focus subject beside it for the eye to
        // refer the blur to, so it looks like the renderer failed rather than like depth. Blur is scaled down
        // over the outer quarter of the frame, to a third rather than to zero — killing it outright would put a
        // hard sharp/soft seam exactly where the eye is most sensitive to one. The CENTRE is untouched, so the
        // subject you are actually looking at keeps the full effect.
        { let e = max(abs(uv.x * 2.0 - 1.0), abs(uv.y * 2.0 - 1.0));   // BAKED IN (user 2026-08-20) — it was panel bit 7 for a few hours
          R *= mix(1.0, 0.33, smoothstep(0.74, 1.0, e)); }
        let isSky = src4.a > 0.5;                                   // ...and whether it is sky at all - read by the tap below
        if (R > 0.8) {
          // ── THE TAP COUNT FOLLOWS THE DISC (user 2026-08-07: "it takes off some fps") ── a flat 32 was
          // sized for the WIDEST circle the strength slider can ask for (~21 px at 1080p), then paid on every
          // blurred pixel — including the ~4 px ones the default 40% actually produces, which is sixteen times
          // the samples that area can use. What has to stay constant is sample DENSITY, not count, so the count
          // is driven by the radius: u.dof.z taps per pixel of radius, 1.6 by default, which lands on exactly 32
          // at the 21 px maximum — the spacing that was eyeballed and kept. The floor of 8 is where a small disc
          // still reads as a disc rather than a ring of points. __vb.dof({taps: 40}) pins the old flat 32 back
          // for an A/B. R is spatially smooth, so neighbouring pixels agree on N and the loop barely diverges.
          let N = i32(clamp(ceil(R * u.dof.z), 8.0, 32.0));
          let fN = f32(N);
          let rot = fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y)) * 6.2831853;   // per-pixel spiral rotation (IGN): without it the fixed taps read as concentric RINGS across a bokeh highlight
          var acc = col; var wsum = 1.0;                            // the sharp centre seeds the sum, so wsum can never reach zero however isolated the pixel is
          for (var i = 0; i < N; i++) {
            let fi = f32(i) + 0.5;
            let ang = fi * 2.39996323 + rot;                        // golden angle → an even disc, no spokes and no clumping
            let rr = R * sqrt(fi / fN);                             // sqrt spacing keeps the sample DENSITY uniform over the disc instead of piling taps at its centre
            let s = textureSampleLevel(dofT, samp, uv + vec2<f32>(cos(ang), sin(ang)) * rr / u.canvasRes, 0.0);
            let rs = abs(s.a * 2.0 - 1.0) * u.dof.y;                // …the TAP's own circle of confusion
            let w = smoothstep(rs + 1.0, rs - 1.0, rr);             // decreasing form: 1 while that circle covers us, 0 once it falls short
            acc += select(s.rgb, src4.rgb, isSky && (s.a > cocA - 0.01)) * w; wsum += w;   // -- THE SKY IS A BACKDROP, NOT A SUBJECT -- it sits at the FAR STOP, so every star used to be gathered across the whole disc and came out a 5.5 px smudge at half its peak (measured 2026-08-19, and this was the whole of the blur). A sky tap now carries the CENTRE's own sky colour instead of its neighbour's, so the stars, the moon's rim and the cloud edges stay as sharp as the composite drew them - while the weight still lands in wsum, which is what keeps a near branch or a far ridge spilling onto the sky at exactly the ratio it always did. The test is self-calibrating rather than a magic number: sky is pinned at the LARGEST circle of confusion the encode can hold (real geometry would have to stand 20000+ voxels out to reach it), so 'the same CoC as this sky pixel' means 'also sky'
          }
          col = acc / wsum;
        }
      }
      // ── LENS FLARE ── all that is left of the sun block: the god-ray scatter that used to open it was
      // REMOVED on the user's word (2026-08-28), along with its half-resolution march, its uniform lanes and
      // the [U] key that A/B'd it. The flare is a separate effect that merely shared this conditional, so it
      // keeps the same gate it always had — sun in front of the camera, above the horizon, effects enabled.
      let sf = dot(u.sunDir, u.fwd);
      // ── AND IT HAS TO SURVIVE A LOW SUN (user 2026-08-28: "polish how the sun looks through the clouds
      // when it rises and sets; it looks fine when it's mid day") ── that is this line, and the old form was
      // the whole of the problem. clamp(sunDir.y * 3) does not reach full strength until the sun is 19.5
      // degrees up, so it was scaling the flare DOWN through the entire sunrise and sunset: 26% at 5 degrees,
      // 10% at 2, near zero at the horizon. The glare was being faded out exactly when a real sun has the
      // most of it, which is why only midday ever looked right. This holds full strength while the sun is up
      // and only lets go as it crosses the horizon, where the disc itself is going anyway.
      // ── AND IT IS THE SUN'S FLARE, NOT THE MOON'S ── u.sunDir carries whichever body is UP, so at night
      // this whole block was aiming at the moon and giving it the sun's glare at full strength. A moon does
      // have a small halo, but it is a body reflecting ~0.12 of the light, not a source; at parity it read as
      // a second sun and it sat on top of a crescent that is supposed to be mostly dark. Scaled by isMoon(),
      // which is the flag that says which body u.sunDir is describing.
      let dayK = smoothstep(-0.03, 0.05, u.sunDir.y) * select(1.0, MOON_FLARE, isMoon());
      if (sf > 0.05 && dayK > 0.0 && (u32(u.fx) & 1u) != 0u) {
        let sunNDC = vec2<f32>(dot(u.sunDir, u.right) / (sf * u.tanH * u.aspect), dot(u.sunDir, u.up) / (sf * u.tanH));
        let sunPix = vec2<f32>((sunNDC.x * 0.5 + 0.5) * u.canvasRes.x, (0.5 - sunNDC.y * 0.5) * u.canvasRes.y);
        // ── LENS FLARE: ghosts along the sun→centre axis + streak + bloom, gated by on-screen sun visibility
        // ── GHOST FOOTPRINT FIRST ── sv is a FRAME CONSTANT (a fixed 3×3 tap around the sun pixel), yet it was
        // computed on EVERY pixel of the canvas: nine texture fetches per pixel to decide a value that is the same
        // for all of them. The four ghosts only cover discs of radius ~10–47 px — about 1.3% of a 1280×720 frame —
        // and outside every disc the contribution is TINT * 0 * 0.10 * sv * dayK, i.e. col is unchanged whatever sv
        // holds. So weigh the discs first and fetch only when one of them actually covers this pixel. Bit-identical
        // (x * 0 = 0 for every finite x, and sv is a bounded sum of clamped smoothsteps — never NaN), and it takes
        // the nine fetches off 98%+ of the frame.
        // ── THE FLARE IS SIZED OFF THE SUN NOW (user 2026-08-28: "tie the flare's radii to the disc
        // instead of to the canvas") ── and the old form is why changing the sun's size did not read. The
        // ghost radii were canvasRes.y * (0.014 + 0.030 * K), which has no reference to the sun at all, so the
        // brightest thing around the disc stayed exactly the same size however the disc was scaled and pinned
        // the apparent size of the whole blob. Measured: a 20% larger disc moved the saturated core 17.9 -> 17.6
        // px, i.e. not at all, because the flare set where the image clipped.
        // sunPx is the disc's on-screen radius. The projection is a TANGENT mapping — ndc = tan(theta)/tanH —
        // so it is tan of the angular radius over tanH, half the vertical resolution. The coefficients are
        // fitted to reproduce today's radii at today's sun (1.741/2.381/3.447/4.619 x the disc), so this is a
        // no-op at the current size and scales from here on.
        // ── AND IT GROWS OFF-AXIS, BECAUSE THE DISC DOES (user 2026-08-28: "if the player looks away from
        // the sun while it's behind a cloud it returns to a harder outline") ── this was the gaze dependence,
        // and it is pure projection. A tangent projection magnifies away from the centre: a small circle at
        // angle theta images RADIALLY by 1/sf^2 and tangentially by 1/sf, where sf = dot(sunDir, fwd). The
        // sky shader draws the disc by ANGLE so it grows with that automatically; this flare is drawn in
        // SCREEN space and was pinned to the centre-of-screen size, so the two came apart the moment the sun
        // left the middle of the frame. MEASURED at the current disc: 16.4 px at centre against a 17.3 px
        // innermost halo — a halo, just — but 24.5 px radial at 35 degrees off-axis and 50.0 px at 55, with
        // the halo still 17.3. Past about 15 degrees the disc OUTGROWS its own glare and what is left is the
        // bare circle, which is exactly the hard outline reported, and why it looked fine dead ahead.
        // 1.5 is the exponent between the two magnifications, and it tracks the disc's mean to within a few
        // percent from 0 to 55 degrees. The max() is a guard: sf reaches 0.05 at the edge of this block's own
        // gate and an unclamped 1/sf^1.5 would blow the flare up to the size of the screen.
        let sunPx = tan(acos(SUN_COSR)) / u.tanH * 0.5 * u.canvasRes.y / pow(max(sf, 0.2), 1.5);
        let ctr = u.canvasRes * 0.5;
        let axis = ctr - sunPix;
        var K = array<f32, 4>(0.35, 0.65, 1.15, 1.7);
        var TINT = array<vec3<f32>, 4>(vec3<f32>(1.0, 0.85, 0.6), vec3<f32>(0.6, 1.0, 0.75), vec3<f32>(0.65, 0.75, 1.0), vec3<f32>(1.0, 0.7, 0.85));
        var GHW = array<f32, 4>(0.0, 0.0, 0.0, 0.0);
        var gAny = 0.0;
        // ── THE SUN'S OWN GLARE (user 2026-08-28: "this effect is achieved when the cursor hovers over it,
        // just make it like that always") ── and that observation is exactly right about the mechanism. The
        // ghosts sit at sunPix + axis * (1 + K) where axis = screen centre - sunPix, so when the sun IS at
        // the centre — crosshair on it — axis goes to zero and all four discs collapse onto the sun and pile
        // up there. That pile IS the glare; it was never a separate effect, just the ghost train degenerating
        // at one camera angle. So the same four discs, the same radii, the same tints and the same weight are
        // now ALSO drawn centred on the sun at every angle, which makes "always" literally the hover look
        // rather than an imitation of it.
        // Deliberately IN ADDITION to the ghosts, not instead: the axis train is a real lens artefact and is
        // worth keeping when the sun is off to the side. Looking straight at the sun therefore lands both at
        // once and is brighter still, which is the right way round.
        var SGW = array<f32, 4>(0.0, 0.0, 0.0, 0.0);
        let dSun = length(fc.xy - sunPix);
        for (var gi = 0; gi < 4; gi++) {
          let gp = sunPix + axis * (1.0 + K[gi]);
          // ── TIGHTENED (user 2026-08-28: "now you made the sun much bigger") ── and it was the glare, not
          // the disc: SUN_COSR never moved. Making the off-axis glare match the on-axis one took the
          // sun-centred stack from 4 x 0.16 to 4 x 0.30, ~1.9x brighter in every direction but dead centre,
          // and with the outermost disc sitting at 4.62x the disc radius that extra brightness pushed
          // visible light a long way out — a bigger BLOB from an unchanged sun. The span comes in from
          // 1.74..4.62x to 1.08..2.61x, which is also much closer to the reference proportions: a white core
          // with a halo about 2.5x its radius, rather than one reaching nearly five.
          let rr = sunPx * (0.65 + 1.15 * K[gi]);                     // …still sized off the DISC, so it keeps scaling with it
          GHW[gi] = smoothstep(rr, rr * 0.22, length(fc.xy - gp));   // 0 outside rr — smoothstep with e0 > e1 is the decreasing form
          // ── A GHOST FADES OUT AS IT LANDS ON THE SUN (user 2026-08-28: "the sun glare goes away when the
          // player looks away from the sun; make it consistent no matter where the player is looking") ── and
          // the glare was not going away off-axis, it was DOUBLING on-axis. The ghosts sit at
          // sunPix + axis * (1 + K) with axis = centre - sunPix, so when the sun is centred axis collapses to
          // zero and all four of them pile onto the sun ON TOP of the four sun-centred discs: eight discs
          // looking straight at it, four looking anywhere else. Exactly 2x, which reads as the glare dying as
          // you turn away. Fading each ghost by its separation from the sun leaves the sun's own glare
          // constant in every direction and keeps the ghost train, which is the part that should depend on
          // where you look.
          let sep = length(gp - sunPix) / max(rr, 1.0);              // separation in this ghost's own radii
          GHW[gi] *= smoothstep(0.6, 1.6, sep);
          gAny += GHW[gi];
          SGW[gi] = smoothstep(rr, rr * 0.22, dSun);                 // …and the same disc about the sun itself
          gAny += SGW[gi];
        }
        if (gAny > 0.0) {
          var sv = 0.0;
          for (var fy = -1; fy <= 1; fy++) { for (var fxx = -1; fxx <= 1; fxx++) {
            let s5 = textureSampleLevel(src, samp, (sunPix + vec2<f32>(f32(fxx), f32(fy)) * 6.0) / u.canvasRes, 0.0);
            // ── SKY-OR-GEOMETRY ONLY, NOT BRIGHTNESS (user 2026-08-28: "it should have a consistent look
            // behind the clouds period") ── s5.a is the sky mask, so this asks the one question an occlusion
            // test should: is something SOLID in front of the sun? It used to be multiplied by
            // smoothstep(0.72, 0.94, brightness), which asks a second and quite different question — is the
            // sun still BRIGHT there — and that is what let the deck withhold the glare. Cloud is sky, so the
            // sun now keeps behind a cloud the same halo it has in the open, which is the requirement. A tree
            // or a hill still hides it, because those clear the sky mask.
            // NOTE this is justified by the REQUIREMENT, not by a measurement: the sv instrumentation I tried
            // was invalid (it painted sv from inside the gAny gate, which never runs on the patch pixels), so
            // treat any numbers about sv in this file's history as unfounded.
            sv += s5.a;
          } }
          sv /= 9.0;
          if (sv > 0.02) {
            // TWO WEIGHTS, not one: with the doubling gone the sun-centred stack has to carry on its own what eight discs used to, so it takes the larger share. The ghost train keeps the weight it had.
            for (var gi = 0; gi < 4; gi++) { col += TINT[gi] * (GHW[gi] * FLARE_GHOST + SGW[gi] * FLARE_SUN) * sv * dayK; }   // 0.10 -> 0.16 (user 2026-08-28: "make the sun have more glare") — eight discs land on the sun at once, so the peak there goes ~0.80 -> ~1.28 x sv, well past white, and the halo reaches further out before it falls under the sky
          }
        }
      }
      // ── CINEMATIC VIGNETTE ── last thing in the frame, so it darkens the finished image (the flare
      // ghosts included) rather than being scattered back out by them. Aspect-corrected: measuring the falloff on
      // raw UV would make it an ellipse stretched to the window, which reads as a letterbox rather than a vignette.
      if (u.misc.x > 0.001) {
        let q = (uv - 0.5) * vec2<f32>(max(1.0, u.aspect), max(1.0, 1.0 / u.aspect));
        let vig = 1.0 - u.misc.x * smoothstep(0.18, 0.78, length(q));
        col *= max(vig, 0.0);
      }
      // ── STACKBADGE ── x{n} beside the HELD item, drawn INTO the image (user: not HTML in the corner).
      // px3's own digits, baked to their native 5x5 grid and packed 25 bits each, LSB = leftmost pixel.
      // The anchor IS the viewmodel anchor, so the badge bobs with the hand instead of floating near it.
      // ── BOTH HANDS CARRY A STACK BADGE (user 2026-08-19: "the rock in the left hand doesnt have the stack
      // number") ── this was written for the right hand and read u.heldCfg.z / u.badge directly. It is now a
      // loop over the two hands, because a badge is a property of a HELD STACK and the off-hand holds one too
      // (a second rock when dual-wielding, or the other half of a craft pair). Same glyphs, same tilt-and-shear,
      // same gold-at-a-full-stack, from the same code — duplicating it would have let the two drift.
      //   hand 0: count u.heldCfg.z, placement u.badge,  visibility u.pickA.z
      //   hand 1: count u.vitG.y,    placement u.badge2, visibility u.pick2A.z
      // The visibility term is each hand's OWN anchor depth: a badge must not be drawn for a hand that is
      // empty or behind the eye, and the two hands are empty independently.
      {
        let GLYPH5 = array<u32, 11>(15255086u, 32641252u, 32553487u, 16267791u, 9413964u, 16268335u, 15252526u, 2236959u, 15252014u, 15235630u, 18299345u);
        for (var bh = 0; bh < 2; bh = bh + 1) {
          let cntB = select(u.heldCfg.z, u.vitG.y, bh == 1);
          let bq   = select(u.badge,     u.badge2,  bh == 1);
          let depB = select(u.pickA.z,   u.pick2A.z, bh == 1);
          if (cntB <= 1.5 || depB <= 0.05) { continue; }
          let pxB = max(2.0, floor(u.canvasRes.y / 320.0 * max(0.2, bq.z)));   // one glyph pixel, floored to WHOLE screen pixels so the badge cannot shimmer under TAA
          let fullB = cntB > 99.5;
          let nB = i32(cntB - select(0.0, 100.0, fullB) + 0.5);
          let digB = select(1, 2, nB >= 10);
          let orgB = floor(vec2<f32>(bq.x, bq.y));
          let rel0 = (fc.xy - orgB) / pxB;
          let BADGE_TILT = bq.w;   // ~15 deg by default. Tilt + shear so the badge sits IN the scene rather than flat on the screen (user).
          let cB = cos(BADGE_TILT); let sB = sin(BADGE_TILT);
          let rotB = vec2<f32>(rel0.x * cB - rel0.y * sB, rel0.x * sB + rel0.y * cB);
          let relB = vec2<f32>(rotB.x + rotB.y * 0.30, rotB.y);   // shear stands in for the foreshortening of a real angled quad
          let cellB = vec2<i32>(floor(relB));
          if (cellB.y >= 0 && cellB.y < 5 && cellB.x >= 0 && cellB.x < (digB + 1) * 6) {
            let giB = cellB.x / 6;                            // glyph 0 is the x, then the digits
            let cxB = cellB.x - giB * 6;                      // 6th column is the inter-glyph gap
            if (cxB < 5) {
              var gB = 10;
              if (giB == 1) { gB = select(nB, nB / 10, digB == 2); }
              if (giB == 2) { gB = nB % 10; }
              if (((GLYPH5[gB] >> u32(cellB.y * 5 + cxB)) & 1u) == 1u) {
                col = mix(col, select(vec3<f32>(0.92, 0.95, 1.0), vec3<f32>(1.00, 0.80, 0.26), fullB), 0.69);   // 0.92 ink * 0.75 = 25% transparent (user) — GOLD at a full stack
              }
            }
          }
        }
      }
      // ── THE HURT FLASH ── the red vignette that fires on every hit. Drawn HERE, last thing before the
      // canvas, for one reason: it is the only place a screen effect is both SEEN and RECORDED. It used to be a
      // DOM <canvas id="hurtFx"> stacked over the game and faded by a CSS keyframe, and veStartRec captures the
      // WebGPU canvas with canvas.captureStream(60) — a DOM element on top of that canvas is not part of the
      // captured surface, so every recording came back with no flash in it at all (user 2026-08-16). Compositing
      // the two canvases into a third was not open either: drawImage of this canvas reads back all zero.
      //
      // It is a PORT, not a redesign — every number below is the one the old canvas painted with, so the look is
      // unchanged. BLIT, not COMPOSITE: this is downstream of TAA, and a screen-FIXED pattern run through a
      // reprojecting resolve would be dragged across the frame by camera motion and smear.
      // A STATE, NOT AN EVENT (user 2026-08-16). This used to paint only while a hit flash was decaying; it now
      // stands for as long as the hearts are down, and the flash is a spike on top. A hit at full health still
      // paints at shade 1 so the blow always registers, then clears with the flash.
      let hLev = max(u.hurtV.z, select(0.0, 1.0, u.hurtV.x > 0.0));
      if (hLev > 0.0) {
        // THE GRID IS WHAT MAKES IT VOXELISED. The old canvas was 64x36 pixels stretched over the window by CSS
        // with image-rendering:pixelated; this is the same 64x36 cells over the same window, so a block is a block
        // at any resolution rather than a shape that gets finer as the canvas grows.
        let hCell = floor(uv * HURTV_GRID);
        let hN = (hCell / (HURTV_GRID - vec2<f32>(1.0))) * 2.0 - 1.0;
        let hD = sqrt(hN.x * hN.x * 0.85 + hN.y * hN.y);
        // STARTS AT 0.78, NOT 0.42 — the first cut of the old canvas painted from 42% of the way out, which on a
        // wide window is most of the screen. A hurt cue frames the view and never obscures it, so only the outer
        // fifth paints and it stays under half opacity.
        // 0.60, not 0.78 (user 2026-08-16: "turn the SCREEN 5 shades of red") — the earlier number framed the
        // view from the outer fifth, which is a cue rather than the screen going red. This reaches well inward
        // while the dither below still thins toward the middle, so the centre never fills and the hit is
        // unmistakable without blinding the player mid-fight.
        // The red CREEPS INWARD as the hearts go: one heart down frames the very edge, one heart left reaches
        // most of the way in. Coverage carries the severity that the four shades alone could not.
        // The band creeps inward one step per heart. These numbers were briefly widened on a bad reading —
        // a vitSet-driven test let REGENERATION climb hp back between the set and the screenshot, so every
        // level came out one lower than intended and the first one looked dead. Drive the level through
        // vitHurt (which zeroes the calm timer and so blocks regen) or the measurement lies to you.
        let hA = (hD - (0.86 - hLev * 0.10)) / 0.62;
        if (hA > 0.0) {
          // The dither: distance from centre is a PROBABILITY, so the red gathers at the edge and breaks into
          // scattered blocks reaching inward. Seeded per HIT (u.hurtV.y), never per frame — a per-frame hash
          // would re-roll every block 60 times a second and sizzle instead of fade, where the old canvas was
          // painted once at the hit and only its opacity moved.
          let hS = i32(u.hurtV.y);
          // ── MORE DAMAGE, MORE RED PIXELS (user 2026-08-16) ── distance from centre is still the probability a
          // block paints, but the whole curve is now scaled by the heart level: one heart down scatters a thin
          // few at the very rim, one heart left fills most of the frame. Same blocks, same red, same per-hit
          // seed — only the COUNT moves, which is the one thing the player is meant to read off it.
          if (ih3(i32(hCell.x), i32(hCell.y), hS) <= min(1.0, hA * hA * 1.1 * (0.34 + hLev * 0.42))) {
            let hV = ih3(i32(hCell.x), i32(hCell.y), hS + 7717);
            // ── EXACTLY FIVE SHADES (user 2026-08-16) ── this used to be 150 + floor(hV*60), which is sixty reds
            // and reads as one noisy red. floor(...*5.0) can only produce 0,1,2,3,4, so hT can only be
            // 0, .25, .5, .75, 1 and the mix can only land on five colours — the count is a property of the
            // arithmetic, not something that has to be eyeballed. Distance carries most of the weight so the
            // shades stack outward as bands (deepest at the rim, lightest reaching in), with enough of the
            // per-block hash mixed in that the bands break up instead of reading as clean concentric rings.
            // NO SHADE (user 2026-08-16) — one red, the same one the effect has always used. Damage is carried
            // entirely by HOW MANY pixels are red, never by what colour they are.
            let hRed = vec3<f32>((150.0 + floor(hV * 60.0)) / 255.0, 10.0 / 255.0, 14.0 / 255.0);   // rgb(150..209, 10, 14), the old canvas's own reds
            col = mix(col, hRed, min(0.55, 0.16 + hA * 0.34 + 0.22 * u.hurtV.x));   // per-block opacity stays PUT as damage rises — if it climbed too, the effect would read as one red sheet thickening rather than as more pixels   // the standing tint is deliberately under a third opacity — it has to be readable for minutes at a time, where the flash only has to survive a fraction of a second   // col is already display-encoded here (COMPOSITE did the 1/2.2), which is the space the DOM canvas composited in — so this is the identical blend
          }
        }
      }
      // ── AND THE SAME EFFECT IN GOLD, FOR HUNGER (user 2026-08-19: "copy the pixels on the screen when the
      // player loses health, except make them gold pixels to represent hunger") ── deliberately a COPY of the
      // block above rather than a shared helper: every number in it was tuned by eye against the red over two
      // days of the user's own notes, and folding the two into one function would mean any later tweak to one
      // bar silently retuning the other. They are two readouts that happen to look alike, not one readout.
      // THREE THINGS ARE DIFFERENT, and each for a reason:
      //   * the SEED is offset. Both bars dither over the same 64x36 grid, so sharing a seed would have gold and
      //     red competing for exactly the same cells — whichever drew second would win and the first bar would
      //     simply vanish wherever they overlapped. Offset, they interleave and both stay readable at once.
      //   * there is no per-hit SPIKE. hurtV.x is an event (you were just struck); going hungry is not an event,
      //     it is a slow state, so there is nothing to flash and the level alone drives it.
      //   * it paints UNDER the red: hunger is the slower, less urgent bar, and a hit landing while starving
      //     should still read as a hit.
      let gLev = u.vitG.x;
      if (gLev > 0.0) {
        let gCell = floor(uv * HURTV_GRID);
        let gN = (gCell / (HURTV_GRID - vec2<f32>(1.0))) * 2.0 - 1.0;
        let gD = sqrt(gN.x * gN.x * 0.85 + gN.y * gN.y);
        let gA = (gD - (0.86 - gLev * 0.10)) / 0.62;
        if (gA > 0.0) {
          let gS = 4409;                                  // a FIXED seed, not a per-hit one: hunger has no hit to seed from, and a per-frame hash would sizzle
          if (ih3(i32(gCell.x), i32(gCell.y), gS) <= min(1.0, gA * gA * 1.1 * (0.34 + gLev * 0.42))) {
            let gV = ih3(i32(gCell.x), i32(gCell.y), gS + 7717);
            let gGold = vec3<f32>((205.0 + floor(gV * 45.0)) / 255.0, (150.0 + floor(gV * 40.0)) / 255.0, 20.0 / 255.0);   // rgb(205..249, 150..189, 20) — the hotbar badge's gold, kept warm enough that it never reads as the red's lighter end
            col = mix(col, gGold, min(0.55, 0.16 + gA * 0.34));
          }
        }
      }
      return vec4<f32>(col, 1.0);
    }
  `;

