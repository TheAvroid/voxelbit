  const TAA_SRC = () => /* wgsl */`
    @group(0) @binding(1) var colorCur : texture_2d<f32>;
    @group(0) @binding(2) var gIrr : texture_2d<f32>;
    @group(0) @binding(3) var colorPrev : texture_2d<f32>;
    @group(0) @binding(4) var samp : sampler;
    @group(0) @binding(5) var colorHistOut : texture_storage_2d<rgba8unorm, write>;
    @group(0) @binding(7) var dofOut : texture_storage_2d<rgba8unorm, write>;   // ── DEPTH OF FIELD ── {resolved colour, circle of confusion}: a SECOND target so colHist.a keeps carrying the sky mask (the blit's depth-of-field and lens flare read it; the god rays that first needed it were removed 2026-08-28), and so the blit's gather costs one fetch per tap instead of two
    @group(0) @binding(6) var slotT : texture_2d<u32>;               // dynamic-life ids — creature pixels reproject through their slot's rigid motion (kills color-history ghost trails)
    // ── YCoCg ── a rotation, not a tone curve: exactly invertible, one dot product each way. Variance in this
    // space separates "this pixel got brighter" from "this pixel changed hue", which is the distinction an RGB
    // box cannot make and the reason chroma ghosts survive one.
    fn rgb2ycocg(c : vec3<f32>) -> vec3<f32> {
      return vec3<f32>(dot(c, vec3<f32>(0.25, 0.5, 0.25)), dot(c, vec3<f32>(0.5, 0.0, -0.5)), dot(c, vec3<f32>(-0.25, 0.5, -0.25)));
    }
    fn ycocg2rgb(c : vec3<f32>) -> vec3<f32> {
      let t = c.x - c.z;
      return vec3<f32>(t + c.y, c.x + c.z, t - c.y);
    }
    // Walk the history sample along the line toward the box centre until it is inside, and convert back. Note
    // it is ONE scale for all three channels — clamping them independently is what lets a rejected sample land
    // on a colour that appears nowhere in the neighbourhood, which reads as a coloured fringe on moving edges.
    fn clipToAABB(prevY : vec3<f32>, lo : vec3<f32>, hi : vec3<f32>) -> vec3<f32> {
      let ctr = 0.5 * (hi + lo);
      let ext = max(0.5 * (hi - lo), vec3<f32>(1e-5));
      let dv = prevY - ctr;
      let r = abs(dv / ext);
      let m = max(r.x, max(r.y, r.z));
      return ycocg2rgb(select(prevY, ctr + dv / m, m > 1.0));
    }
    // Catmull-Rom over a 4x4 footprint, evaluated in 5 bilinear fetches: each pair of adjacent taps is folded
    // into one sample placed off-centre so the hardware's own interpolation weights them. It reads colorPrev
    // and samp from module scope on purpose — the sampler must be LINEAR for the fold to be exact.
    fn catmullRomPrev(uv : vec2<f32>, res : vec2<f32>) -> vec3<f32> {
      let sp = uv * res;
      let tp1 = floor(sp - 0.5) + 0.5;
      let f = sp - tp1;
      let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
      let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
      let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
      let w3 = f * f * (-0.5 + 0.5 * f);
      let w12 = w1 + w2;
      let tp0 = (tp1 - 1.0) / res;
      let tp3 = (tp1 + 2.0) / res;
      let tp12 = (tp1 + w2 / max(w12, vec2<f32>(1e-5))) / res;
      var acc = textureSampleLevel(colorPrev, samp, vec2<f32>(tp12.x, tp0.y), 0.0).rgb * (w12.x * w0.y);
      acc += textureSampleLevel(colorPrev, samp, vec2<f32>(tp0.x, tp12.y), 0.0).rgb * (w0.x * w12.y);
      acc += textureSampleLevel(colorPrev, samp, vec2<f32>(tp12.x, tp12.y), 0.0).rgb * (w12.x * w12.y);
      acc += textureSampleLevel(colorPrev, samp, vec2<f32>(tp3.x, tp12.y), 0.0).rgb * (w3.x * w12.y);
      acc += textureSampleLevel(colorPrev, samp, vec2<f32>(tp12.x, tp3.y), 0.0).rgb * (w12.x * w3.y);
      return acc;
    }
    @compute @workgroup_size(8, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) { return; }
      let p = vec2<i32>(gid.xy);
      let cur4 = textureLoad(colorCur, p, 0);
      let cur = cur4.rgb;                                            // …alpha is the composite's circle of confusion, passed straight through to dofOut below
      // ── NEIGHBOURHOOD ── first and second moments of the 3x3 in YCoCg. These 9 taps used to build a min/max
      // box; the moments cost one multiply-add per tap on top of the fetch and buy a far tighter rejection
      // test (see below), so the box is gone rather than computed alongside.
      var m1 = vec3<f32>(0.0); var m2 = vec3<f32>(0.0);
      for (var dy = -1; dy <= 1; dy++) { for (var dx = -1; dx <= 1; dx++) {
        let q = clamp(p + vec2<i32>(dx, dy), vec2<i32>(0), vec2<i32>(i32(u.res.x) - 1, i32(u.res.y) - 1));
        let yc = rgb2ycocg(textureLoad(colorCur, q, 0).rgb);
        m1 += yc; m2 += yc * yc;
      } }
      let px = vec2<f32>(f32(gid.x) + 0.5 + u.jit.x, f32(gid.y) + 0.5 + u.jit.y);
      let rd = rayDir(px);
      let t = textureLoad(gIrr, p, 0).b;
      var d = rd;
      if (t > 0.0) {
        var wpT = u.camPos + rd * t;
        let slT = textureLoad(slotT, p, 0).r & 255u;
        if (slT != 0u) { wpT -= lifeMotV(i32(slT) - 1).xyz; }           // rigid per-creature motion — same reprojection the irradiance temporal uses
        d = wpT - u.pPos;
      }
      let uv = prevUVd(d);
      var outc = cur;
      if (u.reset < 0.5 && uv.x > 0.001 && uv.y > 0.001 && uv.x < 0.999 && uv.y < 0.999) {
        // ── HISTORY REJECTION ── two independent sources of TAA blur, both addressed here, neither costing
        // a new tap:
        //   · the FETCH. A bilinear sample of the history is a box the width of a pixel, and reprojection
        //     resamples it EVERY frame, so a converged pixel has been through that filter dozens of times and
        //     a voxel edge is soft for no reason but repeated resampling. Catmull-Rom is the standard answer:
        //     a 4x4 bicubic collapsed to 5 bilinear fetches by riding the hardware's own interpolation, with
        //     a mildly negative lobe that puts back what the box took out. Clamped to non-negative afterwards
        //     because that same lobe can undershoot into black on a hard edge and leave a dark rim.
        //   · the CLAMP. Clamping to the 3x3 min/max is the loosest possible box: one bright speck anywhere in
        //     the neighbourhood widens it, so stale history walks through and shows up as a ghost trail, and
        //     the usual reflex is to raise the blend rate — which is more blur. Variance CLIPPING instead
        //     builds the box from the mean and standard deviation (mu +/- 1.25 sigma) and CLIPS toward the
        //     current colour along the line joining them rather than squashing each channel on its own,
        //     which is what stops the clamp from inventing a colour that was never in the neighbourhood.
        //     In YCoCg because luma and chroma get their own variance there — the ghost that survives an RGB
        //     box is usually a chroma one.
        //   · and the FLICKER, which is what the clamp above costs when the feature is SMALL. A distant ice
        //     floe is a handful of bright pixels on dark water: the 3x3 is then mostly water, sigma is small,
        //     and the bright history gets clipped away — so the speck only survives on the frames the jitter
        //     happens to land on it, and it strobes instead of converging. Reported as terrain "flashing" and
        //     found by diffing two adjacent frames of a capture: the change was thin bright OUTLINES tracing
        //     the floe silhouettes, interiors perfectly stable. It is worst in the arctic because that is the
        //     only biome that puts small bright objects on a large dark uniform background.
        //     Two changes, both standard, both aimed at that:
        //       1.75 sigma instead of 1.25 — a wider box keeps more history on a high-contrast edge. Trades a
        //       little ghosting on motion for a lot less strobe; the clip is unambiguous about which is worse.
        //       LUMINANCE-WEIGHTED blending (Karis) — weight each sample by 1/(1+Y) so one bright frame cannot
        //       dominate the average. It is a NO-OP on a converged pixel (prev and cur share a luma, so the
        //       weights collapse back to the plain mix) and only bites when the two disagree in brightness,
        //       which is exactly the flicker case. Y is the same dot() rgb2ycocg uses, so it agrees with the
        //       clamp above rather than introducing a second definition of luma.
        let mu = m1 / 9.0;
        let sg = sqrt(max(m2 / 9.0 - mu * mu, vec3<f32>(0.0)));
        // lgt2 BIT 7 SELECTS THE OLD RESOLVE, so the two can be compared inside ONE session. That matters more
        // than usual here: a first cross-reload attempt read 0.88% before and 2.49% after, but repeated runs
        // at a FIXED vantage scatter between 1.4% and 1.8% on an unchanged build, so a single before/after
        // pair cannot resolve a difference this size. __vb.lgt2(63) = new, __vb.lgt2(63|128) = old.
        let sgK = select(1.75, 1.25, LG2(7u));
        let prev = clipToAABB(rgb2ycocg(max(catmullRomPrev(uv, u.res), vec3<f32>(0.0))), mu - sg * sgK, mu + sg * sgK);
        // ── TAA REACTS TO MOVING BODIES NOW (user 2026-09-01: "when a tree falls you see a damn
        // streak") ── this pass was binding gIrr and reading ONLY .b, the depth. The reactive mask has
        // been sitting in .a the whole time, written by the trace for exactly this purpose and consumed
        // by the irradiance denoiser but never here — so the COLOUR history, which is the one you can
        // actually see, blended a falling trunk against eight frames of the empty sky it used to be in
        // front of. That is the streak: not the shadow lagging, the colour itself.
        // ta is the weight of THIS frame. 0.12 is an eight-frame time constant, which is right for a
        // static world and hopeless for a body crossing the screen. On a fully reactive pixel it goes to
        // 0.65 - about a frame and a half - so the trail collapses to nothing while a still world keeps
        // the long history that makes it quiet. Not 1.0: that is the ?nohist path and it is visibly noisy.
        let rk8 = clamp(textureLoad(gIrr, p, 0).a, 0.0, 1.0);
        let ta = select(1.0, mix(0.12, 0.65, rk8), LG(8u));         // bit 8 off = no colour history (raw, jittery, but instant): ta = 1 collapses either branch to cur
        if (LG2(7u)) {
          outc = mix(prev, cur, ta);
        } else {
          let wp = (1.0 - ta) / (1.0 + max(dot(prev, vec3<f32>(0.25, 0.5, 0.25)), 0.0));
          let wc = ta / (1.0 + max(dot(cur, vec3<f32>(0.25, 0.5, 0.25)), 0.0));
          outc = (prev * wp + cur * wc) / max(wp + wc, 1e-5);
        }
      }
      textureStore(dofOut, vec2<i32>(gid.xy), vec4<f32>(outc, cur4.a));   // the CoC is deliberately NOT temporally blended: it is a depth-derived field, and mixing this frame's against a reprojected one would drag a stale blur radius across every silhouette
      textureStore(colorHistOut, vec2<i32>(gid.xy), vec4<f32>(outc, select(0.0, 1.0, t < 0.0)));   // alpha = SKY mask → the blit tells sky from geometry (depth of field, lens flare)
    }
  `;

