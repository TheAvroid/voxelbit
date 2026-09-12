// ---------------------------------------------------------------------------
// Clouds.slang -- a volumetric cloud deck, ported from the WebGPU game in
//                 src/render/wgsl/cloudgen.js and composite.js.
//
// That version is itself a port of jeantimex/procedural-clouds (MIT), whose
// density function is a WGSL transcription of a Blender cloud node graph: an
// altitude mask, a macro Voronoi stage, a detail Voronoi stage, an upper cutoff
// and a falloff. The five stages and their Map Range remaps are reproduced here
// with the same names, and the constants come across with the tuning the JS
// version earned -- several of them are measured rather than chosen, and their
// notes are kept because a number nobody can justify is a number nobody can
// change.
//
// ---------------------------------------------------------------------------
// WHY IT IS A CACHE AND NOT A DIRECT EVALUATION.
//
// This is the architectural point of the original and it survives the port
// intact. Fractal Voronoi is far too expensive to run per march step: one F1
// octave visits 27 neighbour cells and hashes each. Evaluated once per TEXEL
// into a 3D texture instead, the whole graph is a fixed cost paid over a few
// frames, and the march -- the part that runs per pixel per step -- becomes ONE
// texture fetch.
//
// ---------------------------------------------------------------------------
// THE TILE, which is what makes an endless deck affordable.
//
// Every noise lookup wraps its cell coordinates at a period, so the volume is a
// TILE of an infinite deck rather than a box in the middle of one. The sample
// coordinate is frac(world / CG_TILE) and the cache NEVER needs refilling as
// the player moves -- which is the same problem v2's fog volume solves by
// snapping and shifting, solved a different way because a cloud deck is
// periodic in a way a wood is not.
//
// Every per-stage period must be CG_PER times that stage's frequency AND come
// out an integer, or the wrap does not land on a cell boundary and the tile
// seams -- which shows up as a dead-straight line across the sky at a fixed
// world x/z. The JS version shipped that bug at CG_WARP_F 1.7 (8 * 1.7 = 13.6,
// not an integer); it is 1.75 here for the same reason.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"

// ---- the deck, in world metres -------------------------------------------
static const float CLOUD_LO = 760.0f;
static const float CLOUD_HI = 1080.0f;

// World units the cache tiles over. The trade is repetition against texel size.
static const float CG_TILE = 4096.0f;
// ...and the tile expressed in base noise cells.
static const int CG_PER = 8;

// Stage frequencies, and the integer period each one needs.
static const float CG_SNOISE = 0.5f;    static const int CG_PER_N = 4;
static const float CG_SV1 = 0.75f;      static const int CG_PER_V1 = 6;
static const float CG_SV2 = 3.0f;       static const int CG_PER_V2 = 24;
static const float CG_SREG = 0.25f;     static const int CG_PER_REG = 2;
static const float CG_SBASE = 1.5f;     static const int CG_PER_BASE = 12;
static const float CG_WARP_F = 1.75f;   static const int CG_PER_WARP = 14;
static const float CG_WARP_F2 = 0.5f;

static const int CG_DETAIL_OCT = 4;
static const float CG_BASE = 0.35f;
static const float CG_ALT = 0.32f;
static const float CG_LOWALT = 0.2f;
static const float CG_DETAIL = 0.52f;
static const float CG_CUT = 0.45f;
static const float CG_CUT_H = 0.40f;
static const float CG_DENS = 6.0f;
static const float CG_V1_LO = 0.25f;
static const float CG_V1_HI = 0.75f;
static const float CG_WARP = 0.58f;
static const float CG_WARP2 = 0.44f;
static const float CG_BWOB = 0.13f;
static const float CG_BSOFT = 0.30f;
static const float CG_HVAR = 0.46f;
static const float CG_REGVAR = 0.26f;
static const float CG_VARY = 0.34f;
static const float CG_DET_MAX = 0.39f;

// ---- the march's own numbers ---------------------------------------------
// CG_U2W is the bridge: the source's box is 1.5 units tall and this deck is 320
// world metres, so an optical depth computed in world units would be 213x too
// large. Distances travel in world units and convert to SOURCE units wherever
// they meet a density.
static const float CG_U2W = (CLOUD_HI - CLOUD_LO) / 1.5f;
static const int CG_RAY_STEPS = 28;
static const int CG_LIGHT_STEPS = 4;
static const float CG_LIGHT_STEP = 32.0f;
static const float CG_LIGHT_STEP_U = 0.15f;
static const float CG_SHADOW = 1.2f;
static const float CG_G = 0.45f;
static const float CG_PHASE_MIX = 0.6f;
static const float CG_MIN_D = 0.03f;
static const float CG_MIN_W = 0.004f;
static const float CG_T_CUT = 0.04f;

// ---------------------------------------------------------------------------
// The noise primitives. All of them wrap, all of them are periodic.
// ---------------------------------------------------------------------------

// PCG-style integer mix, 3D.
//
// MULTIPLY THEN ADD -- the LCG step, both halves of it. The JS version records
// that multiplying by three different constants instead leaves any component
// starting at zero AT zero through both mixing rounds, so every cell on the
// y=z=0 line hashed to 0, the Voronoi feature points collapsed onto their cell
// corners, and the deck came out empty with nothing reporting an error.
float3 cgHash3(int3 a)
{
    uint3 v = uint3(a) * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return float3(v & 0xffffffu) / 16777216.0f;
}

// THE TILE. Every cell index folds into [0, per).
int3 cgWrap(int3 c, int per)
{
    return ((c % per) + per) % per;
}

// Voronoi F1 (Blender "Voronoi Texture", feature F1, Euclidean): minimum
// distance to a jittered feature point over the 27 neighbouring cells. This is
// where the look actually comes from -- the rounded, welded, cauliflower lobes
// are F1 iso-surfaces, which value-noise FBM cannot produce at any octave count.
//
// Returns the distance AND the winning cell's own random number. That second
// value is what makes one cloud different from the next: it is constant across
// a whole cell, so anything keyed to it varies per CLOUD rather than per sample.
float2 cgVoroF1(float3 p, int per, float rnd)
{
    const int3 ip = int3(floor(p));
    const float3 fp = p - floor(p);
    float best = 8.0f;
    float seed = 0.0f;
    for (int k = -1; k <= 1; ++k)
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
            {
                const int3 off = int3(i, j, k);
                const float3 h = cgHash3(cgWrap(ip + off, per));
                const float3 feat = float3(off) + h * rnd - fp;
                const float d2 = dot(feat, feat);   // squared while searching
                if (d2 < best) { best = d2; seed = h.y; }
            }
    return float2(sqrt(best), seed);
}

// Octaves of F1, amplitude falling by `rough`, frequency rising by 2. The
// period doubles with the frequency so every octave stays inside the tile.
float cgVoroFbm(float3 p, int per, int oct, float rough, float rnd)
{
    float sum = 0.0f, amp = 1.0f, maxAmp = 0.0f, sc = 1.0f;
    int pr = per;
    for (int i = 0; i < oct; ++i)
    {
        sum += cgVoroF1(p * sc, pr, rnd).x * amp;
        maxAmp += amp; amp *= rough; sc *= 2.0f; pr *= 2;
    }
    return sum / max(maxAmp, 1e-5f);
}

// Value noise, periodic, trilinear with a smoothstep weight.
float cgVal(float3 p, int per)
{
    const int3 ip = int3(floor(p));
    const float3 f = p - floor(p);
    const float3 w = f * f * (3.0f - 2.0f * f);
    float acc = 0.0f;
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i)
            {
                const float h = cgHash3(cgWrap(ip + int3(i, j, k), per)).x;
                const float wx = (i == 1) ? w.x : 1.0f - w.x;
                const float wy = (j == 1) ? w.y : 1.0f - w.y;
                const float wz = (k == 1) ? w.z : 1.0f - w.z;
                acc += h * wx * wy * wz;
            }
    return acc;
}

// Blender's Map Range node, clamped. The shaping does not survive replacing it
// with a smoothstep -- four of the five stages lean on it.
float cgMapRange(float v, float f0, float f1, float t0, float t1)
{
    if (abs(f1 - f0) < 1e-5f) return t0;
    const float t = (v - f0) / (f1 - f0);
    return clamp(lerp(t0, t1, t), min(t0, t1), max(t0, t1));
}

float cg01(float v) { return saturate(v); }

// ---------------------------------------------------------------------------
// THE NODE GRAPH. p is in TILE space: xz wrapped to 0..CG_PER, y normalised
// 0..1 across the deck. Z is height measured DOWNWARD, which is the source's
// convention and the reason the cutoff and falloff read the way they do.
// ---------------------------------------------------------------------------
// `cut` and `regVar` are passed in rather than taken from the constants above
// because they are the two knobs worth turning from outside a rebuild: the cut
// decides how MUCH of the sky is cloud, and regVar decides how much that varies
// from one region to the next -- which is the difference between a uniformly
// covered sky and one with weather in it.
float cgDensity(float3 p, float zn, float cut, float regVar)
{
    const float Z = 1.0f - saturate(zn);

    // STAGE 1 -- altitude mask: a ramp from (1 - lowAltDensity) up to 1 over the
    // lowest fifth of CG_ALT, times a broad noise.
    const float altMask = cg01(CG_BASE *
        cgMapRange(Z, 0.0f, CG_ALT / 5.0f, 1.0f - CG_LOWALT, 1.0f) *
        cgVal(p * CG_SNOISE, CG_PER_N));

    // STAGE 2 -- one cloud per cell, and every one of them different.
    //
    // Coverage is decided by the macro Voronoi ALONE, because coverage is a
    // question about a COLUMN: a ray crosses ~14 texels, so if high-frequency
    // detail is in that decision every column finds something and the sky is
    // overcast at any threshold.
    //
    // DOMAIN WARP: displace the lookup before the Voronoi sees it. Eroding a
    // sphere by subtracting noise can only nibble its edge; bending the SPACE
    // the sphere is measured in deforms the whole body. Two octaves -- one
    // frequency can only bend a cloud one way and every lobe comes out the same
    // size, which is a wobbly disc rather than an irregular cloud.
    const float3 wq = p * CG_WARP_F;
    const float3 warp = float3(cgVal(wq, CG_PER_WARP),
                               cgVal(wq + 31.7f, CG_PER_WARP),
                               cgVal(wq + 67.3f, CG_PER_WARP)) - 0.5f;
    const float3 wq2 = p * CG_WARP_F2;
    const float3 warp2 = float3(cgVal(wq2 + 13.1f, CG_PER_N),
                                cgVal(wq2 + 47.9f, CG_PER_N),
                                cgVal(wq2 + 83.5f, CG_PER_N)) - 0.5f;

    const float2 vc = cgVoroF1((p + warp * CG_WARP + warp2 * CG_WARP2) * CG_SV1,
                               CG_PER_V1, 1.0f);
    const float f1 = vc.x;
    const float sd = vc.y;                          // this cloud's identity
    const float sd2 = frac(sd * 7.31f + 0.17f);     // decorrelated draws, so size,
    const float sd3 = frac(sd * 19.7f + 0.63f);     // roughness and depth differ

    // A very low-frequency noise on the CUT, so coverage varies across the sky
    // and not merely from one cloud to the next.
    const float reg = cgVal(p * CG_SREG + float3(5.3f, 0.0f, 11.9f), CG_PER_REG) - 0.5f;

    // THE FLOOR IS 0.30 AND IT IS WHAT STOPS FLAT WALLS. cov is 1 - mapRange(f1),
    // so LOWERING the cut pushes a cloud's boundary out to larger f1 -- and large
    // f1 is where cells meet, where the F1 iso-surface stops being a sphere and
    // becomes the flat perpendicular bisector between two feature points. Clouds
    // out there come out polyhedral. So the regional term may RAISE the cut
    // freely (thinning is safe) but only dip a little.
    const float cutC = clamp(cut + (sd - 0.5f) * CG_VARY + max(reg, -0.20f) * regVar,
                             0.30f, 0.92f);
    const float cov = 1.0f - cgMapRange(f1, CG_V1_LO, CG_V1_HI, 0.0f, 1.0f);

    // CHEAP CONSERVATIVE EARLY-OUT. Erosion below can only move the boundary by
    // CG_DET_MAX, so anything under this cannot become cloud however the detail
    // falls -- and most of the sky never runs the detail octaves.
    if (cov < cutC - CG_DET_MAX) return 0.0f;

    // STAGE 3 -- the detail CARVES THE SILHOUETTE rather than shading it.
    // Sampled at a per-cloud offset and subtracted from cov BEFORE the cut,
    // which is the difference between a bitten edge and a smooth disc with
    // darker patches on it.
    const float3 dp = (p + float3(sd * 53.0f, sd2 * 29.0f, sd3 * 41.0f)) * CG_SV2;
    const float v2 = cgVoroFbm(dp, CG_PER_V2, CG_DETAIL_OCT, 0.62f, 1.0f);
    const float det = (v2 - 0.5f) * (CG_DETAIL * (0.55f + 0.95f * sd2));
    const float covM = saturate((cov - det - cutC) / max(1.0f - cutC, 0.05f));
    if (covM <= 0.002f) return 0.0f;

    // STAGE 4 -- the vertical profile, per cloud: its altitude, depth and taper.
    //
    // THE LIFT IS NORMALISED, AND IT HAS TO BE. A plain (zn - lift) slides the
    // profile up without moving the deck it is measured against, so the taper's
    // zero point ends up ABOVE the slab and a lifted cloud never tapers -- it
    // runs into CLOUD_HI at full strength and is cut flat there. Dividing by
    // (1 - lift) maps the room the cloud actually has onto its own 0..1. The
    // trade: a raised cloud is a SHORTER cloud.
    const float bwob = (cgVal(p * CG_SBASE + float3(23.7f, 9.1f, 61.3f), CG_PER_BASE) - 0.5f) * CG_BWOB;
    const float lift = max(frac(sd * 3.77f + 0.41f) * CG_HVAR + bwob, 0.0f);
    const float znC = (zn - lift) / (1.0f - lift);
    if (znC < 0.0f) return 0.0f;                    // below this cloud's own base
    const float Zc = 1.0f - znC;

    const float cutoff = cgMapRange(Zc, CG_CUT_H * (0.40f + 1.35f * sd3), 0.0f, 0.0f, 1.0f);
    // THE BASE COMES UP RATHER THAN STARTING. Without this every cloud is sliced
    // dead flat underneath by a horizontal plane.
    const float bSoft = smoothstep(0.0f, CG_BSOFT * (0.55f + 0.90f * sd2), znC);
    const float shaped = cg01(covM + altMask - cutoff) * bSoft;

    // STAGE 5 -- falloff to nothing at the deck top, then the density scale.
    // Zc, not Z: the top fade belongs to the cloud, not to the slab.
    return shaped * cgMapRange(Zc, 0.0f, CG_ALT, 0.0f, 1.0f) * CG_DENS * (0.55f + 0.95f * sd2);
}

// ---------------------------------------------------------------------------
// THE MARCH. Everything below runs per pixel and reads the cache; nothing below
// evaluates the graph above.
// ---------------------------------------------------------------------------

// One fetch. The wind is a pure lookup OFFSET rather than a refill, which is
// the whole reason the tile is periodic: the deck drifts forever and the cache
// is never rebuilt for it.
float cgSample(Texture3D<float> vol, SamplerState samp, float3 p, float windT)
{
    const float yn = (p.y - CLOUD_LO) / (CLOUD_HI - CLOUD_LO);
    if (yn < 0.0f || yn > 1.0f) return 0.0f;
    const float3 uvw = float3(frac((p.x + windT * 9.0f) / CG_TILE),
                              saturate(yn),
                              frac((p.z + windT * 3.5f) / CG_TILE));
    return vol.SampleLevel(samp, uvw, 0.0f);
}

// Henyey-Greenstein, mixed against isotropic exactly as the source does.
float cgPhase(float cosT, float g)
{
    const float g2 = g * g;
    return (1.0f - g2) / (12.5663706f * pow(max(1.0f + g2 - 2.0f * g * cosT, 1e-4f), 1.5f));
}

// Interleaved gradient noise (Jorge Jimenez) -- the offset the march starts on.
//
// NOT a white-noise hash, and the JS version proved why: white noise clumps, so
// neighbouring pixels get wildly different march offsets and a temporal resolve
// has nothing coherent to average. The symptom was heavy grain through the
// whole deck, worst around a low sun where the bloom multiplies the alpha by a
// large number and amplifies the noise with it. IGN is near-optimal over a
// small neighbourhood, so adjacent pixels get well-spread offsets and the
// resolve averages them in ONE frame instead of waiting for temporal luck.
//
// The golden-ratio walk rotates the pattern per frame so it also decorrelates
// over time, which stops it printing a fixed texture into the sky when the
// camera holds still. That is safe here in a way the FOG march is not: the fog
// writes into Lemit, which Ray Reconstruction reprojects along surface motion
// vectors it cannot apply to a volume. The deck is composited on rays that hit
// NO surface, so there is no surface history for it to be reprojected against
// wrongly -- the sky is what DLSS treats as pure background.
float cgIGN(float2 p, uint f)
{
    const float2 q = p + 5.588238f * float(f % 64u);
    return frac(52.9829189f * frac(dot(q, float2(0.06711056f, 0.00583715f))));
}

// The light march: accumulate density up-sun and take exp(-shadow * k). This is
// what a single shadow tap could never give -- a real gradient through the
// cloud -- and the cache is what makes four fetches here affordable where four
// evaluations of the graph would not be.
float cgLight(Texture3D<float> vol, SamplerState samp, float3 p, float3 sunDir, float windT)
{
    float sh = 0.0f;
    for (int i = 1; i <= CG_LIGHT_STEPS; ++i)
        sh += cgSample(vol, samp, p + sunDir * (float(i) * CG_LIGHT_STEP), windT) * CG_LIGHT_STEP_U;
    return exp(-sh * CG_SHADOW);
}

// Where the ray crosses the slab. Returns false when it never does, which for a
// camera under the deck looking down is most of the screen.
bool cgSlab(float3 ro, float3 rd, out float ta, out float tb)
{
    ta = 0.0f;
    tb = 0.0f;
    if (abs(rd.y) < 1e-4f)
    {
        if (ro.y < CLOUD_LO || ro.y > CLOUD_HI) return false;
        ta = 0.0f;
        tb = 40000.0f;
        return true;
    }
    const float t0 = (CLOUD_LO - ro.y) / rd.y;
    const float t1 = (CLOUD_HI - ro.y) / rd.y;
    ta = max(min(t0, t1), 0.0f);
    tb = max(t0, t1);
    return tb > ta;
}

// Front-to-back through the deck. Returns the deck colour PREMULTIPLIED by its
// own coverage in rgb, and that coverage in a.
//
// COMPOSITED AS A SURFACE WITH ALPHA, not blended against the raw sky. The
// caller does `sky * (1 - a) + rgb`, so an opaque cloud hides what is behind it
// completely -- including the sun disk. The brightness still comes through,
// because it arrives as the deck scattering the sun rather than as the disk
// leaking past it, which is what the phase function and the light march are for.
float4 cloudMarch(Texture3D<float> vol, SamplerState samp, float3 ro, float3 rd,
                  float3 sunDir, float3 sunC, float3 ambC, float3 hazeC, float windT, float dither)
{
    float ta, tb;
    if (!cgSlab(ro, rd, ta, tb)) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    // A ray that grazes the slab can travel for kilometres inside it. Cap the
    // span so the step size stays meaningful instead of stepping over whole
    // clouds at the horizon.
    // ── THE SPAN IS CAPPED WHERE THE FADE HAS ALREADY WON ────────────────────
    //
    // A grazing ray enters the slab and stays in it for kilometres. Marching
    // all of that with a fixed step count is what put the fan-shaped streaks
    // along the horizon: at a 12 km span, 28 steps is 428 m each -- coarser
    // than a whole cloud, so the deck was being point-sampled and aliasing.
    //
    // exp(-8560 * 0.00035) is 0.05, so past about that distance the fade has
    // taken 95% of the deck into haze and the samples out there cannot change
    // the pixel. Capping at 9 km spends the step budget where it is visible
    // instead of on air that has already faded out.
    tb = min(tb, ta + 9000.0f);

    // ── AND THE STEPS GROW ───────────────────────────────────────────────────
    //
    // Same reasoning as the fog march, for the same reason: the deck is uniform
    // in space but the ERROR is not. Near samples are seen at full strength and
    // far ones through the fade, so a geometric progression puts the samples
    // where they are still worth taking. Each step is 6% longer than the last,
    // which over 28 steps spends the first third of the budget on the first
    // eighth of the distance.
    const float kGrow = 1.06f;
    const float geoSum = (pow(kGrow, float(CG_RAY_STEPS)) - 1.0f) / (kGrow - 1.0f);
    const float step0 = (tb - ta) / geoSum;
    const float phase = lerp(1.0f, cgPhase(dot(rd, sunDir), CG_G), CG_PHASE_MIX);

    float t = ta;
    float ds = step0;
    float T = 1.0f;
    float3 acc = float3(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < CG_RAY_STEPS; ++i)
    {
        // The dither offsets WITHIN this step, so it stays proportional as the
        // steps grow instead of being a fixed jitter that vanishes at the far
        // end and over-scatters at the near one.
        const float3 sp = ro + rd * (t + ds * dither);
        const float d = cgSample(vol, samp, sp, windT);
        if (d > CG_MIN_D)
        {
            const float stepU = ds / CG_U2W;   // this step, in the units the density was authored in
            const float stTr = exp(-d * stepU);
            // THE SAMPLE CONTRIBUTION, computed BEFORE the light march, because
            // it is what decides whether that march is worth running. A wisp
            // seen through cloud that has already absorbed most of the ray
            // cannot change the pixel, and the four extra fetches it would cost
            // are the most expensive thing in here.
            const float wt = T * (1.0f - stTr);
            if (wt > CG_MIN_W)
            {
                const float scat = cgLight(vol, samp, sp, sunDir, windT) *
                                   phase * (1.0f - exp(-d));
                acc += wt * (sunC * scat + ambC);
            }
            else
            {
                acc += wt * ambC;   // still deposit the ambient, so skipping the
                                    // light march dims the sample rather than
                                    // deleting it
            }
            T *= stTr;
            if (T < CG_T_CUT) break;
        }
        t += ds;
        ds *= kGrow;
    }

    // ── THE DISTANCE FADE, AND IT IS NOT A LOOK CONTROL ──────────────────────
    //
    // Without it the deck is a SOLID WHITE WALL along the horizon. A grazing
    // ray enters a 320 m slab and stays in it for kilometres, so it saturates
    // to full opacity long before it runs out of steps -- and every one of
    // those rays crosses several copies of a 4096 m tile, so the wall is also
    // where the repetition is most visible. Both are the same artefact.
    //
    // THE FADE MOVES COLOUR, NOT ALPHA. Distance is the wrong thing to make a
    // cloud transparent with: a far cloud is still opaque, it just takes the
    // colour of the air in front of it. Fading alpha instead would let the sun
    // disk show through a cloud that ought to hide it. So the colour walks
    // toward the horizon haze and the coverage is left alone -- which dissolves
    // the wall into the sky, and takes the repetition with it, because a tile
    // you cannot distinguish from haze is a tile nobody can see repeat.
    const float a = 1.0f - T;
    if (a <= 1e-4f) return float4(0.0f, 0.0f, 0.0f, 0.0f);

    // Un-premultiply to get the deck's own colour, fade it, and premultiply
    // back so the caller can composite by alpha exactly as before.
    const float3 own = acc / max(a, 1e-3f);
    const float fade = exp(-ta * 0.00035f);
    return float4(lerp(hazeC, own, fade) * a, a);
}
