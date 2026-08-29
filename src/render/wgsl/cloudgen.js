  // ── cloud density cache ─────────────────────────────────────────────────────
  // A port of jeantimex/procedural-clouds (MIT, Copyright (c) 2026 Su) —
  // https://github.com/jeantimex/procedural-clouds — whose density function is a WGSL transcription of a
  // Blender cloud node graph: an altitude mask, a macro Voronoi F1 stage, a detail Voronoi stage, an upper
  // cutoff and a falloff. Its five stages and their Map Range remaps are reproduced below with their names.
  //
  // WHY IT IS A CACHE AND NOT A DIRECT EVALUATION, which is the whole architectural point of that project:
  // fractal Voronoi is far too expensive to run per march step. A single F1 octave visits 27 neighbour cells
  // and hashes each; the deck's old value-noise density was 3 x 8 hashes and a sky pixel already ran ~100 of
  // those. Evaluated once per TEXEL into a 3D texture instead, the whole graph costs a fixed ~500k evaluations
  // spread over seconds, and the march — which is the part that runs per pixel per step — becomes ONE texture
  // fetch. That is what buys clouds this shape at a price the march can pay.
  //
  // ── THREE DELIBERATE DEPARTURES FROM THE SOURCE, all forced by this being an endless world ──
  // 1. PERIODIC NOISE. The original caches a fixed 9x1.5x9 box and ray-marches it with a box intersection —
  //    it is a scene with one cloud volume in it. voxelbit's deck has no bounds and the player walks forever,
  //    so a camera-following box would have to REFILL whenever it moved, which is a hitch and a seam. Instead
  //    every noise lookup wraps its cell coordinates at CG_PER, making the texture a TILE of the infinite
  //    deck: the sample coordinate is just fract(world / tile) and the cache never needs refilling for
  //    movement at all. Wind is a pure translation, so that is a lookup offset too, not a refill.
  // 2. LACUNARITY 2.0, where the source uses 3.0 and 2.5. Periodicity is only exact if every octave's cell
  //    grid divides the tile, which needs an integer ratio. This is the one visible concession: octave
  //    spacing is slightly tighter than Blender's.
  // 3. ONE CACHE, NOT A PING-PONG PAIR. The source keeps two rgba16float volumes and blends them so a cache
  //    rebuild cross-fades. That doubles both the memory and — because sampleDensity reads BOTH and mixes —
  //    the texture traffic of every march step and every light step, which is the hottest path there is here.
  //    A single volume, refreshed a few slices a frame with a very slow evolution clock, has no seam to hide.
  // ── THE TUNING, SHARED ── these are read by the FILL (cloudgen) and by the MARCH (composite), so they live in
  // one string that is emitted into both. A tile size that the two passes disagreed about would be a deck that
  // samples itself at the wrong scale, and nothing in the compiler would say so.
  // Values carry the source project's defaults where they survive the port; where they do not, the reason is
  // on the line. The source's own defaults were density 1.0, coverage 0.8, scale 3.75, altitude 0.5, detail 1.0.
  const CG_CONSTS = /* wgsl */`
    // ── NO EVOLUTION CLOCK ── org.w is pinned. It used to carry tday, which meant every slice's SHAPE changed
    // the moment that slice was refilled, and since the refill walks the volume one slice at a time the deck
    // advanced in 24 discrete jumps a sweep — clouds visibly moving in steps rather than drifting. The motion
    // that matters is WIND, and the march applies wind as a smooth lookup offset that never touches the cache.
    // Losing shape evolution costs nothing anyone can see, and it makes the fill IDEMPOTENT, which is what lets
    // it stop entirely once the volume is complete (see CG_SWEEPS in vis.js) instead of running forever.
    const CG_TILE : f32 = 4096.0;                                    // world units the cache tiles over. The trade is repetition against texel size: the deck fades out by ~8000 units so this repeats about twice, and the far copy is already most of the way into the haze. Larger tiles look less repetitive and resolve worse.
    const CG_PER : f32 = 8.0;                                        // …and the tile expressed in base noise cells. Every per-stage period below is CG_PER times that stage's scale and MUST come out an integer, or the wrap does not land on a cell boundary and the tile seams.
    const CG_SNOISE : f32 = 0.5;                                     // stage 1 noise frequency  (source 2.0 in objPos/3.75 space, rescaled so the macro cells land near the deck's existing cloud size)
    const CG_SV1 : f32 = 0.75;                                       // stage 2 macro Voronoi    (source 5.0, same rescale) — 10 cells across the tile, ~410 world units a cell, which is about what a cloud measures here today
    const CG_SV2 : f32 = 3.0;                                        // stage 3 detail Voronoi   (source 2.0, same rescale) — coarse base, many octaves, which is how the source gets broad-spectrum detail out of one node
    const CG_PER_N : i32 = 4;                                        // CG_PER * CG_SNOISE
    const CG_PER_REG : i32 = 2;                                      // CG_PER * CG_SREG
    const CG_PER_V1 : i32 = 6;                                      // CG_PER * CG_SV1
    const CG_PER_V2 : i32 = 24;                                       // CG_PER * CG_SV2
    const CG_DETAIL_OCT : i32 = 4;                                   // source runs 6 (detail 1.0 x 5). Capped at 4 because octave 5's features are ~14 world units and a cache texel is ~26 — past this the octaves are not detail, they are aliasing, and they cost the fill 2.4x
    const CG_V1_OCT : i32 = 1;                                       // ONE octave. The second octave rides at half amplitude on the same field and dents the sphere around each feature point — it is the difference between a cloud and a lumpy cloud-shaped region. Detail belongs to the v2 stage, which erodes edges without moving the core.                                       // source detail 1.0 -> base octave + 1
    const CG_ALT : f32 = 0.32;                                       // source "altitude" — its default is 0.5, this is inside its own 0.1..1.0 slider range. It is the deck's single biggest shaping knob and it does NOT transfer: the cutoff is mapRange(Z, CG_ALT * CG_SALT, 0, 0, 1), so at 0.5 the cut is 0.47 at the deck base and 0.73 through the middle, which against this deck's proportions left only wisps. At 0.32 the base cut falls to ~0.15 and the falloff holds full strength through the lower 70%.
    const CG_LOWALT : f32 = 0.2;                                     // source lowAltDensity
    const CG_MACRO : f32 = 0.8;                                      // source "coverage" -> factorMacro
    const CG_DETAIL : f32 = 0.52;                                    // source factorDetail is 1.0, giving the DETAIL stage 2.5x the swing of the macro stage. That ratio is why the sky went straight from empty to solid overcast with no scattered state in between: coverage was being decided by high-frequency noise, which decorrelates over a few texels, and a ray crossing ~14 of them essentially never misses. Whole COLUMNS have to be empty for there to be blue sky, and only the macro stage varies on that scale. So the macro term is no longer halved and the detail term is cut to about a third: v1 decides where cloud IS, v2 only erodes its edges.
    const CG_CUT : f32 = 0.45;                                      // COVERAGE, and it is MEASURED, not guessed. The slice probe says the pre-cut field runs 0.275..1.18 with a median of 0.588 — a narrow band, which is why every earlier attempt to set this by arithmetic flipped the sky between empty and solid overcast with nothing in between. This is the median, so about half the deck is cloud. Because the cut renormalises, moving it changes how MUCH sky is cloud without changing how solid that cloud is.
    const CG_CUT_H : f32 = 0.40;                                     // the vertical cutoff's reach (source: altitude * scaleAlt = 1.875). cutoff = 1 - (1 - zn)/H, so it is ZERO up to zn = 1 - H and then climbs to 1 at the deck top. At H = 1 that is cutoff = zn, i.e. the cut starts biting immediately at the base, and clouds ~680 units wide came out 60-160 tall — pancakes. At 0.40 the lower 60% of the deck is uncut and only the top 40% tapers, which is the shape of a cumulus rather than a sheet.
    const CG_SALT : f32 = 3.75;                                      // source scaleAlt (this one stays in the source's units: it multiplies CG_ALT inside a Map Range on normalised height, so the rescale above does not reach it)
    const CG_DENS : f32 = 6.0;                                       // source density 1.0 x its own 5.0 raymarch tune
    const CG_YSPAN : f32 = 0.625;                                    // the deck's height in base noise units: (CLOUD_HI - CLOUD_LO) / CG_TILE * CG_PER. Keeping this honest is what stops the clouds being stretched or squashed against their own horizontal shape.
    // ── THE MARCH'S OWN NUMBERS ── read only by composite, but kept here so every knob the source exposes sits
    // in one place. CG_U2W is the bridge: the source's box is 1.5 units tall and this deck is 320 world units,
    // so an optical depth computed in world units would be 213x too large. Distances travel in world units and
    // convert to SOURCE units wherever they meet a density.
    const CG_BLOOM : f32 = 3.20;                                     // how much of the sun's light the deck throws forward as bloom
    const CG_MBLOOM : f32 = 0.18;                                    // 0.60 -> 0.18 (user 2026-08-28: "adjust how the moon looks behind the clouds, it's too bright"). This is the halo the deck throws forward for the MOON, and it was set at 0.6 against the sun's 3.2 — a fifth of the sun's, for a body that reflects about a millionth of its light. Through thin cloud it lit the deck like a second sunrise                                    // …and the moon's, which is a far dimmer body but wants the same behaviour
    const CG_BLOOM_TIGHT : f32 = 800.0;                              // the halo's exponent with no cloud in the way. It USED to be pinned to the sun's own corona in skyColor so the two were continuous; that corona is gone (see the note in PRE — the sky-side glow was the white smear the user asked to remove, and the glare that reads correctly is the LENS FLARE in blit.js instead). So this is now free-standing: it is the DECK's bloom width and nothing else has to agree with it. 2.38 degrees at half-power, which is what it was when it did have to.
    const CG_BLOOM_WIDE : f32 = 90.0;                                // …and through full cloud, about a 6-degree glow. Width goes as 1/sqrt(n).
    const MOON_GLOW : vec3<f32> = vec3<f32>(0.94, 0.95, 1.00);       // the same faint cool white sunTint() gives the moon
    const CG_SKY_OCC : f32 = 1.85;                                   // how much faster the SKY hides behind the deck than the deck's own density says. Only the sky, never the cloud's own lighting — see the note at the call site.
    const CG_U2W : f32 = 213.3333;                                   // world units per source unit = (CLOUD_HI - CLOUD_LO) / 1.5
    const CG_RAY_STEPS : i32 = 28;                                   // source default 48. A fetch is cheap but it is not free, and 28 across a 320-deep deck is ~11 world units a step — already finer than the cache texel, so the extra 20 would resolve nothing the volume holds.
    const CG_LIGHT_STEPS : i32 = 4;                                  // source default
    const CG_LIGHT_STEP : f32 = 32.0;                                // source 0.15, in world units (0.15 x CG_U2W)
    const CG_LIGHT_STEP_U : f32 = 0.15;                              // …and in source units, which is what the shadow's optical depth is measured in
    const CG_SHADOW : f32 = 1.2;                                     // source shadowDarkness is 5.0, and that number does not survive the port either. The light march walks 4 x 0.15 source units up-sun, and against a measured median density of 1.6 that is an optical depth of ~0.96 — times 5 it is exp(-4.8), so EVERY sample came back with 0.8% of the sun on it and the whole deck rendered as ambient-only haze. Measured, not guessed: the probe that found this read the field's percentiles off a screenshot.
    const CG_G : f32 = 0.45;                                         // source HG eccentricity
    const CG_PHASE_MIX : f32 = 0.6;                                  // source blends its phase 60% against isotropic
    const CG_SUN : f32 = 2.2;                                        // NOT the source's 17. That figure is against a white SUN_COLOR of 1.0 and a Reinhard tonemap; here the illuminant is sunTint() at ~3.6 through ACES, so carrying 17 over would clip the entire deck to white. This is the same brightness expressed in this renderer's units.
    // ── THE max_distance CALIBRATION, MEASURED ── Blender's Voronoi node divides its F1 output by an
    // internally computed max_distance before handing it to the Map Range nodes, so the source's windows
    // (0..0.75 for the macro stage, 0..1 for the detail stage) are correct THERE and wrong here. A slice probe
    // says this port's F1 fbm actually spans about 0.11..0.43 with a median near 0.21 — a fifth of the assumed
    // range. Feeding that through the source's windows compressed both stages into a sliver near the bottom of
    // their output, which is what left the field with no dynamic range: s3 sat in a 0.79..1.0 band and no
    // coverage threshold could separate cloud from sky, only make the whole deck thinner. These windows are the
    // measured distribution, so the remaps downstream get the contrast they were written to expect.
    // ── THE VORONOI WINDOWS ── the source's are 0..0.75 (macro) and 0..1 (detail), sized for Blender's
    // internally normalised F1. A single F1 octave over unit cells with randomness 1.0 has a median near 0.5
    // and almost all its mass between 0.2 and 0.8, and the fbm's amplitude normalisation keeps it there, so
    // these centre the window on that. They were briefly set from screenshot measurements; those numbers were
    // wrong (see the ramp note in composite.js) and cost several rounds of tuning.
    const CG_V1_LO : f32 = 0.25;
    const CG_V1_HI : f32 = 0.75;
    const CG_V2_LO : f32 = 0.25;
    const CG_V2_HI : f32 = 0.75;
    const CG_WARP : f32 = 0.58;                                      // domain-warp amplitude, in macro-noise units. RAISED from 0.38 (user 2026-08-28: "make the clouds more irregular, they all are disc shaped") — the warp is the only term that deforms a cloud's BODY rather than nibbling its edge, so it is the one that decides whether the silhouette is round
    const CG_WARP2 : f32 = 0.44;                                     // …and a SECOND, LARGER octave. One warp frequency can only bend a cloud one way: every lobe ends up about the same size and the result is still a disc, just a wobbly one. A second octave at a third the frequency displaces whole GROUPS of cells together, so a cloud can come out long, or bent, or lopsided, at a scale bigger than itself — which is what "not disc shaped" actually means
    const CG_WARP_F2 : f32 = 0.5;                                    // …its frequency. CG_PER * this = 4 = CG_PER_N exactly, so it stays periodic and the tile still tiles
    const CG_PER_WARP : i32 = 14;                                    // CG_PER * CG_WARP_F, and it MUST be an integer — see below
    const CG_WARP_F : f32 = 1.75;                                     // …and its frequency. Low enough to bend a whole cloud rather than fuzz its surface.
    // ── 1.7 -> 1.75, AND THAT IS THE VERTICAL SEAM (user 2026-08-28: "the line artifact is still happening
    // with the clouds") ── every noise lookup here wraps its cell indices at a period so the volume is a TILE
    // of an endless deck, and the period has to be CG_PER * that lookup's frequency or the field does not
    // join up with itself. At 1.7 the warp's true period was 8 * 1.7 = 13.6, not an integer, and it was
    // passing CG_PER_N = 4 — the period belonging to CG_SNOISE. So the warp wrapped in the wrong place and
    // left a hard discontinuity along the tile boundary: a dead-straight line down the sky, at a fixed world
    // x/z, exactly as reported. 1.75 gives 14, which is an integer, and CG_PER_WARP carries it.
    // IT WAS ALWAYS WRONG AND ONLY BECAME VISIBLE NOW: raising CG_WARP 0.38 -> 0.58 for the irregularity
    // work multiplied the size of the discontinuity along with the warp itself. Every other lookup in this
    // file checks out — SNOISE 0.5*8=4, SV1 0.75*8=6, SV2 3.0*8=24, WARP_F2 0.5*8=4, SREG 0.25*8=2,
    // SBASE 1.5*8=12 — this was the only one.
    const CG_SBASE : f32 = 1.5;                                      // ── THE BASE WOBBLE ── frequency. CG_PER * this = 12, an integer, so it stays periodic
    const CG_PER_BASE : i32 = 12;
    const CG_BWOB : f32 = 0.13;                                      // …and its amplitude, as a fraction of deck height. THIS is the real fix for flat bottoms, and the reason a fade alone could not be: the macro cells are ~683 world units across while the deck is only 320 tall, so a cloud spans less than HALF a cell vertically and the Voronoi barely changes through it. The field is a 2D coverage extruded upward, which makes every base a horizontal PLANE by construction — and a skyful of planes at one altitude, seen at a grazing angle, is also what lines them up into an apparent horizontal seam across the sky. Displacing each column's base by a noise breaks both: the bases stop being coplanar, so there is no plane left to catch the light or to line up
    const CG_BSOFT : f32 = 0.30;                                     // …and how much of a cloud's band it then takes to come up from nothing (0.22 -> 0.30)                                     // ── THE BASE FADE (user 2026-08-28: "the clouds appear to be flat on the bottom") ── how much of a cloud's own band it takes to come up from nothing. There was NO base fade at all: stage 4 returns 0 below znC = 0 and full density immediately above it, so every cloud was sliced dead flat underneath by a horizontal plane. Real cumulus do have flattish bases, which is why this is a fade and not a dome — but a hard step is a cut, not a base. Varied per cloud by sd2 below so they do not all soften by the same amount
    const CG_HVAR : f32 = 0.46;                                      // altitude spread, as a fraction of deck height. RAISED from 0.38 (user 2026-08-28: "make the clouds have more height variation"). It is bounded by the pancake floor, not the ceiling: znC divides by (1 - lift), so a cloud lifted this far has 54% of the deck to occupy and gets shorter as it rises                                      // altitude spread, as a fraction of deck height. It is no longer bounded by the CEILING: the profile is normalised into each cloud's own band (see stage 4) so nothing clips at any value. It is bounded by the FLOOR instead, because lift and height now trade one for one — at 0.38 the highest cloud is 62% of a 320-unit deck, ~199 world units against a ~683-unit width, just clear of the 60-160 pancake band CG_CUT_H is tuned against. 0.45 puts it at 176 and inside it.
    const CG_SREG : f32 = 0.25;                                      // ── REGIONAL COVERAGE (user 2026-08-28: "some areas are dense while other areas are more sparse") ── the frequency of it. CG_PER * this = 2, so the whole tile carries about two of these regions and they are exactly periodic. Deliberately far below CG_SV1 (0.75): this has to vary over MANY cells, or it is just another per-cloud term and the sky stays uniformly average
    const CG_REGVAR : f32 = 0.26;                                    // …and how far it may push the cut. +-0.13 against a CG_CUT of 0.45 is the difference between a thick bank and a few stragglers. It ADDS to the per-cloud CG_VARY rather than replacing it, so a sparse region still has big and small clouds in it — the two spreads are about different things
    const CG_VARY : f32 = 0.34;                                      // how far a cloud's own threshold may stray from CG_CUT. This is the size spread, and it is wide on purpose: one Voronoi cell yields one cloud, so without it every cloud is the same sphere cut at the same radius and the sky reads as a tiled pattern.
    const CG_DET_MAX : f32 = 0.39;                                   // = 0.5 * CG_DETAIL * 1.5, the max of (v2-0.5) * CG_DETAIL * (0.55 + 0.95*sd2). IT MUST MOVE WITH CG_DETAIL: it is the bound the conservative early-out below trusts, so leaving it low would cull columns the erosion could still have opened and bite chunks out of clouds at random                                   // the largest boundary shift the erosion can produce = 0.5 * CG_DETAIL * 1.75. It exists so the cheap early-out above stays CONSERVATIVE: too small and clouds get their edges clipped off, too large and the early-out stops paying for itself.
    const CG_STORE : f32 = 8.0;                                      // the density that maps to 1.0 in the unorm volume. CG_DENS is 5.0 and the graph clamps before it, so this has headroom and never clips
    const CG_BASE : f32 = 0.35;                                      // ── THE PEDESTAL, AND THE ONE CALIBRATION THE PORT COULD NOT INHERIT ── stage 1 hands stages 2 and 3 a base of ~0.5 (a normalised noise's mean), they add ~0.2 and ~0.375 on top, and the clamp01 between them pinned the field at 1.0 across the entire deck: a perfectly uniform slab with no structure in it at all, which renders as flat haze and reads as "no clouds". Blender's Voronoi node normalises its F1 output against an internally computed max_distance that this port does not reproduce, so the three stages arrive with the wrong relative weight. Scaling the pedestal down is where that calibration goes: it costs nothing, it leaves the graph's shape and every one of its Map Range remaps untouched, and it is the difference between a field that varies and a field that is clipped.
    const CG_MIN_D : f32 = 0.03;                                     // density below which a sample is skipped outright (source 0.01). At 0.01 a haze thin enough to be invisible still paid a full light march
    const CG_MIN_W : f32 = 0.004;                                    // …and the contribution below which a sample keeps its ambient but skips the LIGHT MARCH, which is 4 of the 5 volume fetches a lit sample costs
    const CG_T_CUT : f32 = 0.04;                                     // transmittance early-out (source 0.01). The deck behind 96% absorption is not visible and the remaining steps are pure cost
    const CG_RES : vec3<f32> = vec3<f32>(160.0, 24.0, 160.0);        // cache resolution. 2.46 MB at r32float; a texel is ~26 world units across and ~13 tall, both finer than the march step, so the cache is not the limiting detail anywhere
  `;
  const CLOUDGEN_SRC = () => /* wgsl */`
    ${CG_CONSTS}
    struct CG { org : vec4<f32>, cfg : vec4<f32> }                   // org.xyz = tile origin in noise space (wind), org.w = evolution time; cfg.x = first y slice this dispatch owns, cfg.y = slice count, cfg.z/w spare
    @group(0) @binding(0) var<uniform> cg : CG;
    @group(0) @binding(1) var cgOut : texture_storage_3d<rgba8unorm, write>;   // NOT r32float. WebGPU classes r32float as UNFILTERABLE, so a linear sampler cannot read it and the bind group is rejected outright — a black canvas at full frame rate. rgba8unorm is filterable AND storage-capable, costs the same 4 bytes, and 256 levels across CG_STORE is far finer than a density feeding exp() needs.

    // ── HASH ── PCG-style integer mix, 3D. The source uses Blender's Jenkins lookup3 for Perlin and a PCG4D
    // for Voronoi; one good 3D mix serves both here and keeps the register pressure down in a shader that is
    // already visiting 27 cells an octave.
    fn cgHash3(a : vec3<i32>) -> vec3<f32> {
      var v = vec3<u32>(a) * 1664525u + 1013904223u;                 // MULTIPLY THEN ADD — the LCG step, both halves of it. Multiplying by three different constants instead (which is what this was) leaves any component that starts at zero at zero through both mixing rounds, so every cell on the y=z=0 line hashed to 0 and the Voronoi feature points collapsed onto their cell corners. The deck came out empty and nothing errored.
      v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
      v ^= v >> vec3<u32>(16u);
      v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
      return vec3<f32>(v & vec3<u32>(0xffffffu)) / 16777216.0;
    }
    fn cgWrap(c : vec3<i32>, per : i32) -> vec3<i32> {               // THE TILE. Every cell index folds into [0, per), so the field is exactly periodic and the cache is a tile of an endless deck rather than a box in the middle of one
      return ((c % per) + vec3<i32>(per)) % per;
    }
    // ── VORONOI F1 (Blender "Voronoi Texture", feature F1, Euclidean) ── minimum distance to a jittered
    // feature point over the 27 neighbouring cells. This is the function the source's look actually comes
    // from: the rounded, welded, cauliflower lobes are F1 iso-surfaces, which is what value-noise FBM — the
    // deck's old density — cannot produce at any octave count.
    // Returns BOTH the F1 distance and the winning cell's own random number — Blender's Voronoi node outputs a
    // "seed colour" alongside the distance for exactly this reason. That second value is what makes one cloud
    // different from the next: it is constant across a whole cell, so anything keyed to it varies per CLOUD
    // rather than per sample, and it is derived from wrapped cell indices so it stays periodic with the tile.
    fn cgVoroF1(p : vec3<f32>, per : i32, rnd : f32) -> vec2<f32> {
      let ip = vec3<i32>(floor(p));
      let fp = p - floor(p);
      var best = 8.0;
      var seed = 0.0;
      for (var k = -1; k <= 1; k++) {
        for (var j = -1; j <= 1; j++) {
          for (var i = -1; i <= 1; i++) {
            let off = vec3<i32>(i, j, k);
            let h = cgHash3(cgWrap(ip + off, per));
            let feat = vec3<f32>(off) + h * rnd - fp;
            let d2 = dot(feat, feat);                                // squared while searching; one sqrt at the end instead of 27
            if (d2 < best) { best = d2; seed = h.y; }
          }
        }
      }
      return vec2<f32>(sqrt(best), seed);
    }
    // ── FRACTAL VORONOI (Blender's detail/roughness/lacunarity) ── octaves of F1 with the amplitude falling
    // by rough and the frequency rising by 2.0 (see departure 2 above). The period doubles with the
    // frequency so every octave stays inside the same tile.
    fn cgVoroFbm(p : vec3<f32>, per : i32, oct : i32, rough : f32, rnd : f32) -> f32 {
      var sum = 0.0; var amp = 1.0; var maxAmp = 0.0; var sc = 1.0; var pr = per;
      for (var i = 0; i < oct; i++) {
        sum += cgVoroF1(p * sc, pr, rnd).x * amp;
        maxAmp += amp; amp = amp * rough; sc = sc * 2.0; pr = pr * 2;
      }
      return sum / max(maxAmp, 1e-5);                                // Blender normalises by the summed amplitude, which is what keeps detail from also changing brightness
    }
    // ── VALUE-NOISE FBM ── stands in for the source's Blender Perlin at STAGE 1 only, where the noise is a
    // multiplier on an altitude ramp and its character never reaches the silhouette. Periodic, same tile.
    fn cgVal(p : vec3<f32>, per : i32) -> f32 {
      let ip = vec3<i32>(floor(p)); let f = p - floor(p);
      let w = f * f * (3.0 - 2.0 * f);
      var acc = 0.0;
      for (var k = 0; k < 2; k++) { for (var j = 0; j < 2; j++) { for (var i = 0; i < 2; i++) {
        let h = cgHash3(cgWrap(ip + vec3<i32>(i, j, k), per)).x;
        let wx = select(1.0 - w.x, w.x, i == 1);
        let wy = select(1.0 - w.y, w.y, j == 1);
        let wz = select(1.0 - w.z, w.z, k == 1);
        acc += h * wx * wy * wz;
      } } }
      return acc;
    }
    fn cgMapRange(v : f32, f0 : f32, f1 : f32, t0 : f32, t1 : f32) -> f32 {   // Blender's Map Range node, clamped — the source leans on this in four of its five stages and the shaping does not survive replacing it with a smoothstep
      if (abs(f1 - f0) < 1e-5) { return t0; }
      let t = (v - f0) / (f1 - f0);
      return clamp(mix(t0, t1, t), min(t0, t1), max(t0, t1));
    }
    fn cg01(v : f32) -> f32 { return clamp(v, 0.0, 1.0); }

    // ── THE NODE GRAPH ── p is in TILE space: xz already wrapped to 0..CG_PER, y normalised 0..1 across the
    // deck. Z is height measured DOWNWARD (1 at the base, 0 at the top), which is the source's convention and
    // the reason its cutoff and falloff read the way they do.
    fn cgDensity(p : vec3<f32>, zn : f32) -> f32 {
      let Z = 1.0 - clamp(zn, 0.0, 1.0);
      // STAGE 1 — altitude mask: a ramp from (1 - lowAltDensity) up to 1 over the lowest fifth of CG_ALT, times a broad noise
      let altMask = cg01(CG_BASE * cgMapRange(Z, 0.0, CG_ALT / 5.0, 1.0 - CG_LOWALT, 1.0) * cgVal(p * CG_SNOISE + vec3<f32>(cg.org.w * 0.11), CG_PER_N));
      // ══ STAGE 2: ONE CLOUD PER CELL, AND EVERY ONE OF THEM DIFFERENT ══════════════════════════════════
      // Coverage is decided by the macro Voronoi ALONE, because coverage is a question about a COLUMN — a ray
      // crosses ~14 texels, so if high-frequency detail is in that decision every column finds something and
      // the sky is overcast at any threshold.
      // INVERTED (1 - F1): F1's iso-surface is a SPHERE near a feature point and the flat perpendicular
      // bisector out where two cells meet. Taking cloud where F1 is large puts every cloud in the gaps, bounded
      // by those planes — that is a straight wall, and it is polyhedral by construction.
      // ── DOMAIN WARP ── displace the lookup before the Voronoi sees it. Eroding a sphere by subtracting noise
      // can only nibble its edge; bending the SPACE the sphere is measured in deforms the whole body, which is
      // what turns a smooth blob into something lumpy and cauliflowered. Three cheap value-noise taps, paid once
      // per texel in the fill and never in the march. Periodic like everything else, so the tile still tiles.
      let wq = p * CG_WARP_F;
      let warp = vec3<f32>(cgVal(wq, CG_PER_WARP), cgVal(wq + vec3<f32>(31.7), CG_PER_WARP), cgVal(wq + vec3<f32>(67.3), CG_PER_WARP)) - 0.5;   // CG_PER_WARP, not CG_PER_N — see the note at CG_WARP_F
      let wq2 = p * CG_WARP_F2;                                      // …and the larger octave, which bends whole groups of cells rather than one cloud
      let warp2 = vec3<f32>(cgVal(wq2 + vec3<f32>(13.1), CG_PER_N), cgVal(wq2 + vec3<f32>(47.9), CG_PER_N), cgVal(wq2 + vec3<f32>(83.5), CG_PER_N)) - 0.5;
      let vc = cgVoroF1((p + warp * CG_WARP + warp2 * CG_WARP2) * CG_SV1, CG_PER_V1, 1.0);
      let f1 = vc.x;
      let sd = vc.y;                                                 // THIS CLOUD'S IDENTITY — constant across the cell
      let sd2 = fract(sd * 7.31 + 0.17);                             // …and two more decorrelated draws from it, so size,
      let sd3 = fract(sd * 19.7 + 0.63);                             // roughness, depth and density do not all move together
      // ── THE REGIONAL TERM ── a very low-frequency noise on the CUT, so coverage varies across the sky and
      // not merely from one cloud to the next. Raising the cut in a region thins it to stragglers; lowering it
      // lets the cells merge into a bank. It goes in HERE, into the same threshold the per-cloud spread uses,
      // which is why it costs one noise tap and nothing downstream has to know about it.
      let reg = cgVal(p * CG_SREG + vec3<f32>(5.3, 0.0, 11.9), CG_PER_REG) - 0.5;
      // ── THE FLOOR IS 0.30 AND THAT IS THE FIX FOR THE FLAT WALLS (user 2026-08-28: "some of the clouds
      // seem to have a hard flat edge on the side") ── which was mine. cov is 1 - mapRange(f1), so LOWERING the
      // cut pushes a cloud's boundary outward to larger f1 — and large f1 is exactly where cells meet, where
      // the F1 iso-surface stops being a sphere and becomes the flat perpendicular bisector between two
      // feature points. The regional term I added could take the cut to 0.15, far out into that flat
      // far-field, and the clouds there came out polyhedral with straight walls. The old 0.06 floor allowed it
      // in principle; nothing had ever driven the cut low enough to reach it before.
      // So: the floor rises to 0.30, and the regional term is allowed to RAISE the cut freely (thinning a
      // region is safe — it shrinks clouds back toward their feature points) but only to dip a little.
      // Sparse-vs-dense survives; the walls do not.
      let cutC = clamp(CG_CUT + (sd - 0.5) * CG_VARY + max(reg, -0.20) * CG_REGVAR, 0.30, 0.92);   // SIZE: each cloud gets its own threshold, so the cell's sphere is cut at a different radius — big ones and small ones out of one field
      let cov = 1.0 - cgMapRange(f1, CG_V1_LO, CG_V1_HI, 0.0, 1.0);
      if (cov < cutC - CG_DET_MAX) { return 0.0; }                   // CHEAP CONSERVATIVE EARLY-OUT, kept deliberately: erosion below can only move the boundary by CG_DET_MAX, so anything under this cannot become cloud however the detail falls. Most of the sky still never runs the detail octaves.
      // ── STAGE 3: the detail now CARVES THE SILHOUETTE instead of shading it ──
      // Sampled at a per-cloud offset, so no two clouds are eroded by the same piece of noise, and subtracted
      // from cov BEFORE the cut — that is the difference between a cloud with a bitten edge and a smooth disc
      // with darker patches on it. Its bite varies per cloud too: some come out ragged, some nearly smooth.
      let dp = (p + vec3<f32>(sd * 53.0, sd2 * 29.0, sd3 * 41.0)) * CG_SV2;
      let v2 = cgVoroFbm(dp, CG_PER_V2, CG_DETAIL_OCT, 0.62, 1.0);
      let det = (v2 - 0.5) * (CG_DETAIL * (0.55 + 0.95 * sd2));
      let covM = clamp((cov - det - cutC) / max(1.0 - cutC, 0.05), 0.0, 1.0);
      if (covM <= 0.002) { return 0.0; }
      // ── STAGE 4: the vertical profile, per cloud — its ALTITUDE, its depth, and its taper ──
      // Every cloud used to sit on the slab floor, because the profile was keyed to zn (height within the
      // DECK) and so every base landed on CLOUD_LO exactly. The lift gives each cloud its own band inside the
      // deck and the profile is measured from THAT, so the deck stops being one layer of clouds at one
      // altitude and becomes a depth of sky with clouds at different heights in it.
      // Lift is one-sided on purpose: a negative one would push a cloud's base below CLOUD_LO, where the slab
      // would cut it off again — the very thing this is fixing.
      // ── THE LIFT IS NORMALISED, AND IT HAS TO BE ── znC used to be a plain (zn - lift), which slides the
      // profile up without moving the DECK it is measured against. Both shaping terms below key off
      // Zc = 1 - znC, so both of them slid with it: at the ceiling Zc came out at the lift rather than 0, which
      // is to say the taper's zero point sat at zn = 1 + lift, ABOVE the slab. So a lifted cloud never
      // tapered at all — it ran into CLOUD_HI at full strength and was cut flat there. Measured on the field
      // this replaces: at CG_HVAR 0.30 a lifted cloud still held 65-94% of its own peak density in the top
      // texel, and 51% of the deck was guillotined. (The comment that stood here claimed CG_HVAR "stays
      // inside the headroom the top taper leaves". There was no headroom: the taper rises WITH the cloud, so
      // there never could be — the lift was buying its altitude by having its top sawn off.)
      // Dividing by (1 - lift) maps the room the cloud actually has — zn in [lift, 1] — onto its own 0..1, so
      // the base still lands flat at its own altitude and the taper now reaches zero exactly AT the ceiling
      // instead of through it. One divide, at FILL time, once per texel: the march never sees it.
      // The one thing it trades: a raised cloud is a SHORTER cloud. That is what bounds CG_HVAR now — the
      // pancake floor underneath it, not the ceiling above it.
      // ── THE BASE UNDULATES ── the per-cloud lift decides WHICH altitude band this cloud sits in; this
      // second term varies the base WITHIN the cloud, column by column, so its underside is a surface rather
      // than a plane. Clamped at 0 so it can still never push a base below CLOUD_LO, where the slab would cut
      // it flat again — which is the very thing stage 4 exists to avoid.
      let bwob = (cgVal(p * CG_SBASE + vec3<f32>(23.7, 9.1, 61.3), CG_PER_BASE) - 0.5) * CG_BWOB;
      let lift = max(fract(sd * 3.77 + 0.41) * CG_HVAR + bwob, 0.0);                  // a fourth decorrelated draw: altitude must not track the depth that sd3 already drives
      let znC = (zn - lift) / (1.0 - lift);                          // deck height -> THIS CLOUD's own 0..1, so the profile scales into its band instead of sliding out of the slab
      if (znC < 0.0) { return 0.0; }                                 // below this cloud's own base
      let Zc = 1.0 - znC;
      let cutoff = cgMapRange(Zc, CG_CUT_H * (0.40 + 1.35 * sd3), 0.0, 0.0, 1.0);   // WIDENED from (0.55 + 0.90): this is a cloud's own DEPTH, and with the taper reach spanning 0.40..1.75 of CG_CUT_H instead of 0.55..1.45 the deck carries genuinely squat and genuinely towering clouds rather than a range of medium ones
      // ── AND THE BASE COMES UP RATHER THAN STARTING ── znC is 0 at this cloud's own floor, so this fades
      // it in over the lowest CG_BSOFT of its band. Per-cloud via sd2 so the softness varies with the cloud.
      let bSoft = smoothstep(0.0, CG_BSOFT * (0.55 + 0.90 * sd2), znC);
      let shaped = cg01(covM + altMask - cutoff) * bSoft;
      // STAGE 5 — falloff to nothing at the deck top, then the density scale
      return shaped * cgMapRange(Zc, 0.0, CG_ALT, 0.0, 1.0) * CG_DENS * (0.55 + 0.95 * sd2);   // Zc, not Z: the top fade belongs to the cloud, not to the slab   // …and some are simply thicker than others
    }

    // ── THE FILL ── one workgroup covers an 8x8x4 block of the volume. cfg.x/y hand this dispatch a BAND of
    // y slices so the whole cache is refreshed a slice at a time across many frames instead of in one hitch;
    // the evolution clock in org.w advances slowly enough that a half-old cache has no seam in it.
    @compute @workgroup_size(8, 4, 8)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      let dims = textureDimensions(cgOut);
      let y = gid.y + u32(cg.cfg.x);
      if (gid.x >= dims.x || y >= dims.y || gid.z >= dims.z || gid.y >= u32(cg.cfg.y)) { return; }
      let zn = (f32(y) + 0.5) / f32(dims.y);
      let tile = (vec2<f32>(f32(gid.x), f32(gid.z)) + 0.5) / vec2<f32>(f32(dims.x), f32(dims.z)) * CG_PER;
      let d = cgDensity(vec3<f32>(tile.x, zn * CG_YSPAN, tile.y), zn);
      textureStore(cgOut, vec3<i32>(vec3<u32>(gid.x, y, gid.z)), vec4<f32>(d / CG_STORE, 0.0, 0.0, 1.0));   // normalised into the unorm range; the march multiplies it straight back
    }
  `;
