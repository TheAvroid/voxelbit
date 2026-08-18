  // ── GOD RAYS AT HALF RESOLUTION ── The 24-tap radial march used to run in BLIT, per CANVAS pixel: 7.3M
  // pixels x 24 fetches = 175M samples a frame, which measured as 85% of the whole blit pass and 20 fps at
  // 3834x1904. The effect itself is a broad radial smear of the sun's halo — there is no detail in it finer
  // than a few pixels — so computing it at half resolution (a quarter of the work) and letting the bilinear
  // sampler put it back is where the cost goes without the look changing.
  //
  // Halving the TAP COUNT instead was tried and measured: 12 taps with the stride, decay and scale re-derived
  // to the same integral came back 1.735 ms against a 1.627 ms baseline — no gain, because doubling the stride
  // costs as much cache locality as the halved count saves. The pixels are the cost, not the iterations.
  //
  // Only the march lives here. The tint, the daylight factor and the lens flare stay in BLIT: the flare is
  // already gated to the ~1.3% of pixels its ghosts actually cover, and it needs full-resolution positions.
  const GOD_SRC = () => /* wgsl */`
    @group(0) @binding(1) var src : texture_2d<f32>;
    @group(0) @binding(2) var samp : sampler;
    @group(0) @binding(3) var godOut : texture_storage_2d<rgba16float, write>;
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      let gw = u32(ceil(u.canvasRes.x * 0.5)); let gh = u32(ceil(u.canvasRes.y * 0.5));
      if (gid.x >= gw || gid.y >= gh) { return; }
      let fc = (vec2<f32>(f32(gid.x), f32(gid.y)) + vec2<f32>(0.5)) * 2.0;   // centre of the 2x2 canvas block this texel stands for
      let sf = dot(u.sunDir, u.fwd);
      let dayK = clamp(u.sunDir.y * 3.0, 0.0, 1.0);
      var glow = vec3<f32>(0.0);
      if (sf > 0.05 && dayK > 0.0 && (u32(u.fx) & 1u) != 0u) {
        let sunNDC = vec2<f32>(dot(u.sunDir, u.right) / (sf * u.tanH * u.aspect), dot(u.sunDir, u.up) / (sf * u.tanH));
        let sunPix = vec2<f32>((sunNDC.x * 0.5 + 0.5) * u.canvasRes.x, (0.5 - sunNDC.y * 0.5) * u.canvasRes.y);
        var sp = fc;
        let delta = (sunPix - sp) * (0.7 / 24.0);                  // rays reach ~70% of the way to the sun
        sp += delta * fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y));   // the same IGN start jitter — at half res it still breaks the 24 discrete steps into a smooth shaft
        var decay = 1.0;
        for (var i = 0; i < 24; i++) {
          sp += delta;
          let suv = sp / u.canvasRes;
          if (suv.x > 0.0 && suv.y > 0.0 && suv.x < 1.0 && suv.y < 1.0) {
            let s4 = textureSampleLevel(src, samp, suv, 0.0);
            let b = max(s4.r, max(s4.g, s4.b));
            let white = min(s4.r, min(s4.g, s4.b)) / max(b, 0.001);   // 1 = white (the sun disc/halo), low = saturated sky
            glow += s4.rgb * (smoothstep(0.86, 1.0, b) * smoothstep(0.45, 0.8, white) * s4.a * decay);
          }
          else { break; }                                          // exact early-out, unchanged: a straight ray leaving the screen never comes back
          decay *= 0.96;
        }
      }
      textureStore(godOut, vec2<i32>(i32(gid.x), i32(gid.y)), vec4<f32>(glow, 1.0));
    }
  `;

  const BLIT_SRC = () => /* wgsl */`
    const HURTV_GRID : vec2<f32> = vec2<f32>(64.0, 36.0);   // the hurt flash's block grid — the resolution the old DOM canvas was painted at, kept so the blocks are the same size they always were
    @group(0) @binding(1) var src : texture_2d<f32>;
    @group(0) @binding(2) var samp : sampler;
    @group(0) @binding(3) var dofT : texture_2d<f32>;
    @group(0) @binding(4) var godT : texture_2d<f32>;                // half-res god-ray glow (see GOD_SRC above) — bilinear on the way back up                // ── DEPTH OF FIELD ── {resolved colour, signed CoC} from the TAA pass
    @vertex fn vs(@builtin(vertex_index) vi : u32) -> @builtin(position) vec4<f32> {
      var P = array<vec2<f32>, 3>(vec2<f32>(-1.0, -3.0), vec2<f32>(3.0, 1.0), vec2<f32>(-1.0, 1.0));
      return vec4<f32>(P[vi], 0.0, 1.0);
    }
    @fragment fn fs(@builtin(position) fc : vec4<f32>) -> @location(0) vec4<f32> {
      let uv = fc.xy / u.canvasRes;
      var col = textureSampleLevel(src, samp, uv, 0.0).rgb;
      // ── DEPTH OF FIELD ── scatter-as-gather over a disc whose radius IS this pixel's circle of confusion.
      // dofT carries {colour, signed CoC} together, so a tap costs one fetch. A tap contributes only while its
      // OWN circle still reaches this pixel — that one test is what stops the blurred background from eating the
      // sharp silhouette standing in front of it, because a tap on a focused surface has a circle of nothing and
      // cannot spread anywhere. Pixels under ~0.8 px of blur skip the loop outright, which on an ordinary frame
      // is almost all of them: the effect costs a single compare wherever the picture is in focus.
      // It runs FIRST so the god rays, the flare ghosts and the vignette below all land on the finished image —
      // a flare is made at the lens, not at the subject, and blurring one along with the scene reads as a smudge.
      if (u.dof.x > 0.0) {
        let R = abs(textureSampleLevel(dofT, samp, uv, 0.0).a * 2.0 - 1.0) * u.dof.y;
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
          let rot = fract(52.9829189 * fract(0.06711056 * fc.x + 0.00583715 * fc.y)) * 6.2831853;   // per-pixel spiral rotation (IGN — the same hash the god rays jitter their march with): without it the fixed taps read as concentric RINGS across a bokeh highlight
          var acc = col; var wsum = 1.0;                            // the sharp centre seeds the sum, so wsum can never reach zero however isolated the pixel is
          for (var i = 0; i < N; i++) {
            let fi = f32(i) + 0.5;
            let ang = fi * 2.39996323 + rot;                        // golden angle → an even disc, no spokes and no clumping
            let rr = R * sqrt(fi / fN);                             // sqrt spacing keeps the sample DENSITY uniform over the disc instead of piling taps at its centre
            let s = textureSampleLevel(dofT, samp, uv + vec2<f32>(cos(ang), sin(ang)) * rr / u.canvasRes, 0.0);
            let rs = abs(s.a * 2.0 - 1.0) * u.dof.y;                // …the TAP's own circle of confusion
            let w = smoothstep(rs + 1.0, rs - 1.0, rr);             // decreasing form: 1 while that circle covers us, 0 once it falls short
            acc += s.rgb * w; wsum += w;
          }
          col = acc / wsum;
        }
      }
      // ── GOD RAYS: screen-space radial scatter toward the sun (ported from the old engine's blit).
      // Bright near-white SKY pixels scatter (alpha = sky mask from the TAA pass); voxels block → shafts.
      let sf = dot(u.sunDir, u.fwd);
      let dayK = clamp(u.sunDir.y * 3.0, 0.0, 1.0);
      if (sf > 0.05 && dayK > 0.0 && (u32(u.fx) & 1u) != 0u) {
        let sunNDC = vec2<f32>(dot(u.sunDir, u.right) / (sf * u.tanH * u.aspect), dot(u.sunDir, u.up) / (sf * u.tanH));
        let sunPix = vec2<f32>((sunNDC.x * 0.5 + 0.5) * u.canvasRes.x, (0.5 - sunNDC.y * 0.5) * u.canvasRes.y);
        let glow = textureSampleLevel(godT, samp, uv, 0.0).rgb;   // ── the march now happens once per 2x2 block in GOD_SRC ── bilinear back up; the effect has no detail this loses
        // ── THE SHAFTS DIM WITH THE SUN THAT CASTS THEM (2026-08-17) ── the storm sky drops the direct beam
        // to 65% and thickens the deck to ~75% cover, and god rays kept firing at full strength through it:
        // bright shafts under a heavy overcast, which reads as a bug rather than as weather. u.hurtV.w is
        // the rain scalar (0 outside the oak forest and outside a storm), so fair weather is unchanged.
        col += glow * (0.055 * dayK) * (1.0 - 0.35 * u.hurtV.w) * select(vec3<f32>(1.0, 0.93, 0.78), vec3<f32>(0.55, 0.65, 1.0) * 1.1, isMoon());   // strong warm sun shafts / cool MOON RAYS — the rays, not the fog
        // ── LENS FLARE: ghosts along the sun→centre axis + streak + bloom, gated by on-screen sun visibility
        // ── GHOST FOOTPRINT FIRST ── sv is a FRAME CONSTANT (a fixed 3×3 tap around the sun pixel), yet it was
        // computed on EVERY pixel of the canvas: nine texture fetches per pixel to decide a value that is the same
        // for all of them. The four ghosts only cover discs of radius ~10–47 px — about 1.3% of a 1280×720 frame —
        // and outside every disc the contribution is TINT * 0 * 0.10 * sv * dayK, i.e. col is unchanged whatever sv
        // holds. So weigh the discs first and fetch only when one of them actually covers this pixel. Bit-identical
        // (x * 0 = 0 for every finite x, and sv is a bounded sum of clamped smoothsteps — never NaN), and it takes
        // the nine fetches off 98%+ of the frame.
        let ctr = u.canvasRes * 0.5;
        let axis = ctr - sunPix;
        var K = array<f32, 4>(0.35, 0.65, 1.15, 1.7);
        var TINT = array<vec3<f32>, 4>(vec3<f32>(1.0, 0.85, 0.6), vec3<f32>(0.6, 1.0, 0.75), vec3<f32>(0.65, 0.75, 1.0), vec3<f32>(1.0, 0.7, 0.85));
        var GHW = array<f32, 4>(0.0, 0.0, 0.0, 0.0);
        var gAny = 0.0;
        for (var gi = 0; gi < 4; gi++) {
          let gp = sunPix + axis * (1.0 + K[gi]);
          let rr = u.canvasRes.y * (0.014 + 0.030 * K[gi]);
          GHW[gi] = smoothstep(rr, rr * 0.22, length(fc.xy - gp));   // 0 outside rr — smoothstep with e0 > e1 is the decreasing form
          gAny += GHW[gi];
        }
        if (gAny > 0.0) {
          var sv = 0.0;
          for (var fy = -1; fy <= 1; fy++) { for (var fxx = -1; fxx <= 1; fxx++) {
            let s5 = textureSampleLevel(src, samp, (sunPix + vec2<f32>(f32(fxx), f32(fy)) * 6.0) / u.canvasRes, 0.0);
            sv += s5.a * smoothstep(0.72, 0.94, max(s5.r, max(s5.g, s5.b)));
          } }
          sv /= 9.0;
          if (sv > 0.02) {
            for (var gi = 0; gi < 4; gi++) { col += TINT[gi] * GHW[gi] * 0.10 * sv * dayK; }   // doubled — bold flare ghosts
          }
        }
      }
      // ── CINEMATIC VIGNETTE ── last thing in the frame, so it darkens the finished image (god rays and flare
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
      if (u.heldCfg.z > 1.5 && u.pickA.z > 0.05) {
        let GLYPH5 = array<u32, 11>(15255086u, 32641252u, 32553487u, 16267791u, 9413964u, 16268335u, 15252526u, 2236959u, 15252014u, 15235630u, 18299345u);
        // ── WHERE IT GOES IS COMPUTED, NOT TUNED (user 2026-08-17: "auto adjust the number count in the top
        // right of the held item in hand. always in the top right") ── this used to be a fixed offset from the
        // viewmodel ANCHOR in item-voxel units, which is the middle of the model's box: right for one item and
        // wrong for the next, because a 5x6x1 steak and a 7x3x6 apple put their top-right corner in completely
        // different places relative to that centre. So main/tick-camera.js projects the held model's own eight
        // box corners and hands over the screen pixel the badge starts at — the item's actual top-right corner,
        // whatever it is holding, whatever pose it is in, however it bobs. u.badge.x/y is that pixel.
        // It is done in JS rather than here for one reason: the corners need the model's GRID DIMENSIONS, and
        // this shader has no way to look up the held item's entry in ITEMD.
        let pxB = max(2.0, floor(u.canvasRes.y / 320.0 * max(0.2, u.badge.z)));   // one glyph pixel, floored to WHOLE screen pixels so the badge cannot shimmer under TAA. The size multiplier is inside the floor for that reason — scaling AFTER it would put the glyph back on fractional pixels. Clamped away from zero so a cold uniform (or a cleared save) can never collapse the badge to nothing.
        let nB = i32(u.heldCfg.z + 0.5);
        let digB = select(1, 2, nB >= 10);
        let orgB = floor(vec2<f32>(u.badge.x, u.badge.y));
        let rel0 = (fc.xy - orgB) / pxB;
        let BADGE_TILT = u.badge.w;   // ~15 deg by default. Tilt + shear so the badge sits IN the scene rather than flat on the screen (user).
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
              col = mix(col, vec3<f32>(0.92, 0.95, 1.0), 0.69);   // 0.92 ink * 0.75 = 25% transparent (user)
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
      return vec4<f32>(col, 1.0);
    }
  `;

