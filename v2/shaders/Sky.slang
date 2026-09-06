// ---------------------------------------------------------------------------
// Sky.slang -- the device half of Preetham's analytic daylight model.
//
// WHY AN ANALYTIC SKY AND NOT A CONSTANT: almost all of the light reaching the
// floor of a wood is sky light, not sun -- the canopy blocks the sun over most
// of the ground. Sky light is strongly directional (bright near the horizon in
// haze, deep blue at zenith) and strongly coloured, and a constant-colour dome
// loses the blue fill on the shaded side of every trunk, which is a large part
// of why an outdoor render looks outdoor.
//
// The host fits the coefficients (see scene/sky.h) and this file evaluates
// them. That split is why the miss path costs twenty lines of arithmetic with
// no branches worth the name, instead of the tan() and pow() the fit needs.
// ---------------------------------------------------------------------------
#pragma once

#include "Atmosphere.slang"
#include "Shared.slang"
#include "Vecmath.slang"

float skySolidAngle()
{
    return TWO_PI * (1.0f - kSunCosThetaMax);
}

// cosGamma is passed IN rather than taken as cos(gamma), because gamma was
// itself made by an acos of that very number a moment ago -- the round trip was
// three cosines per sky evaluation to recover a value the caller already had,
// and it recovers it slightly less accurately than not doing it.
float perezF(float cosTheta, float gamma, float cosGamma, float a, float b, float c, float d,
             float e)
{
    const float ct = max(cosTheta, 0.01f);
    return (1.0f + a * exp(b / ct)) * (1.0f + c * exp(d * gamma) + e * cosGamma * cosGamma);
}

// Radiance of the dome in the renderer's working units.
float3 skyPerez(V6Sky sky, float3 dir)
{
    const float3 dn = normalize(dir);
    const float cosTheta = max(dn.y, 0.01f);
    const float cosGamma = clamp(dot(dn, sky.sunDir), -1.0f, 1.0f);
    const float gamma = acos(cosGamma);

    // xyY, one channel at a time. Written out rather than looped because the
    // coefficients live in the .xyz of five float4s -- see the packing note in
    // Shared.slang for why they are not an array.
    const float3 a = float3(sky.A.x, sky.A.y, sky.A.z);
    const float3 b = float3(sky.B.x, sky.B.y, sky.B.z);
    const float3 c = float3(sky.C.x, sky.C.y, sky.C.z);
    const float3 d = float3(sky.D.x, sky.D.y, sky.D.z);
    const float3 e = float3(sky.E.x, sky.E.y, sky.E.z);
    const float3 zen = float3(sky.zenith.x, sky.zenith.y, sky.zenith.z);
    const float3 nf = float3(sky.normF.x, sky.normF.y, sky.normF.z);

    float3 v;
    v.x = zen.x * perezF(cosTheta, gamma, cosGamma, a.x, b.x, c.x, d.x, e.x) / max(1e-6f, nf.x);
    v.y = zen.y * perezF(cosTheta, gamma, cosGamma, a.y, b.y, c.y, d.y, e.y) / max(1e-6f, nf.y);
    v.z = zen.z * perezF(cosTheta, gamma, cosGamma, a.z, b.z, c.z, d.z, e.z) / max(1e-6f, nf.z);

    // xyY -> XYZ -> linear sRGB.
    const float Y = max(0.0f, v.x), x = v.y, y = max(1e-4f, v.z);
    const float X = (x / y) * Y;
    const float Z = ((1.0f - x - y) / y) * Y;
    float3 rgb = float3(
        3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
        0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z
    );
    rgb = max(rgb, float3(0.0f));
    return rgb * sky.skyScale;
}

// The dome WITHOUT the sun disk. Kept separate from the disk so multiple
// importance sampling can weight the two contributions differently: the disk is
// found by the cone sampler, the dome by whatever the BSDF did.
// ---------------------------------------------------------------------------
// THE SAME DOME, OUT OF THE SKY-VIEW TABLE.
//
// atmoViewAngles undoes on the read the rotation AtmoSkyView.cs.slang applied
// on the write: the table is stored in the sun's own frame, so the azimuth
// asked for here is the angle to the SUN rather than to any world axis. That is
// what lets 192x108 cover a dome that changes all day.
// ---------------------------------------------------------------------------
float3 skyAtmoLookup(V6Sky sky, float3 d, Texture2D<float4> lut, SamplerState samp)
{
    const float3 dn = normalize(d);
    float viewZenithCos, lightViewCos;
    atmoViewAngles(dn, sky.sunDir, viewZenithCos, lightViewCos);

    const float vh = max(sky.atmoViewHeight, kAtmoGroundR + 1e-4f);

    // Whether this ray meets the planet, by the same test the bake used. The
    // table stores both halves and the mapping is discontinuous between them,
    // so reading it with the wrong half is a hard seam exactly along the
    // horizon -- the one place the eye is guaranteed to be looking.
    const bool ground = (viewZenithCos < 0.0f) &&
                        (viewZenithCos * viewZenithCos >=
                         1.0f - (kAtmoGroundR * kAtmoGroundR) / (vh * vh));

    const float2 uv = atmoSkyViewParamsToUv(ground, viewZenithCos, lightViewCos, vh);
    return max(lut.SampleLevel(samp, uv, 0.0f).rgb, float3(0.0f, 0.0f, 0.0f));
}

// THE TABLE ARRIVES AS A PARAMETER, for the same reason the moon's face does:
// Sky.slang is included by ProbeTrace as well, and a declared-but-unbound
// global resource fails that whole dispatch rather than the branch that would
// have read it.
float3 skyDome(V6Sky sky, Texture2D<float4> skyLut, SamplerState lutSamp, float3 d)
{
    const bool atmo = sky.atmoOn > 0.5f;
    // Below the horizon there is no sky, but a hard cut at y = 0 draws a
    // visible seam right where the eye is looking. Fade into a dim ground
    // bounce instead -- this stands in for the world beyond what is resident,
    // and it keeps the far treeline sitting in something.
    if (d.y <= 0.0f)
    {
        // KEPT EVEN WITH THE TABLE ON. The sky-view table does have rows below
        // the horizon, but they describe the PLANET seen through air -- lit
        // ground at a hundred kilometres -- and this engine draws its own
        // ground much closer than that. The fade stands in for the world beyond
        // what is resident either way.
        const float t = saturate(-d.y / 0.10f);
        const float3 flat = float3(d.x, 1e-4f, d.z);
        const float3 horizon =
            atmo ? skyAtmoLookup(sky, flat, skyLut, lutSamp) : skyPerez(sky, flat);
        const float avg = (horizon.x + horizon.y + horizon.z) * (1.0f / 3.0f);
        return lerp(horizon, sky.groundAlbedo * avg, sstep(t));
    }
    return atmo ? skyAtmoLookup(sky, d, skyLut, lutSamp) : skyPerez(sky, d);
}

// The solar disk itself. Zero outside the cone, so this can simply be added.
float3 skyDisk(V6Sky sky, float3 d)
{
    return dot(d, sky.sunDir) >= kSunCosThetaMax ? sky.sunRadiance : float3(0.0f);
}

// ---------------------------------------------------------------------------
// THE MOON, with phases.
//
// SAME ANGULAR SIZE AS THE SUN, which is also the physical answer -- both are
// about 0.53 degrees, which is why eclipses work at all.
//
// ---------------------------------------------------------------------------
// THE PHASE IS SYNTHETIC, AND IT HAS TO BE.
//
// The moon hangs at the anti-solar point, so the observer is always between it
// and the sun and the geometry says FULL, every night. A phase cannot come from
// the moon's POSITION here. It comes from the lighting instead: a virtual sun
// direction in the moon's own frame, swung by the phase angle. alpha 0 puts it
// behind the observer (full), pi/2 to the side (quarter), pi behind the moon
// (new), and the terminator sweeps across on its own.
//
// THE TERMINATOR SWINGS ALONG R, WHICH IS HORIZONTAL. Projecting the sun's real
// bearing onto the disc looks like the physical answer and is a trap: at the
// anti-solar point the sun's bearing lies exactly in the moon's own vertical
// plane, so its component along a horizontal R is identically zero, the light
// swings up-and-down instead, and the terminator comes out HORIZONTAL -- a moon
// lit from below, which is not a thing. There is no honest "toward the sun"
// direction on an anti-solar disc, so the orientation is chosen: horizontal, so
// the lit limb is a left or right crescent and the terminator is vertical.
//
// THE FRAME IS ANCHORED TO WORLD UP, not to the sun. A face has an up, and it
// has to come from the world. Deriving both axes from the light is what makes a
// moon appear on its side.
//
// LUNAR-LAMBERT, NOT LAMBERT. Lunar regolith is a fine dust that scatters from
// below the surface, and its single-scattering term is Lommel-Seeliger,
// mu0/(mu0+mu). At full phase mu0 = mu everywhere on the disc, so that term is
// 1/2 EVERYWHERE and the moon reads as a flat disc rather than a shaded ball --
// which is exactly what a real full moon looks like, and what plain Lambert
// gets wrong. The accepted model is a weighted blend of the two.
// ---------------------------------------------------------------------------
static const float kMoonLL = 0.85f;      // how much of the blend is Lommel-Seeliger
static const float kMoonGain = 1.74f;
static const float kMoonSoft = 0.88f;    // a body, so the edge is nearly hard
static const float kMoonEarth = 0.055f;  // earthshine, as a fraction of full
static const float kMoonCrop = 1.09f;    // how much of the photo the disc uses
static const float kMoonCon = 1.75f;     // contrast on the maria, about the pivot
static const float kMoonPiv = 0.52f;
static const float kMoonPhMin = 0.42f;   // a crescent's surface brightness vs full


// ---------------------------------------------------------------------------
// THE KEY LIGHT: whichever body is up.
//
// The moon hangs at the anti-solar point, so exactly one of the two is above
// the horizon at any moment and "the key light" is a single well-defined thing.
// Everything that samples the sun goes through here -- the next-event
// estimation, the fog march, the cloud deck -- which means MOONLIGHT IS RAY
// TRACED BY THE SAME SHADOW RAYS the sun uses, and needs no second code path
// of its own. Moon shadows are real shadows because they are the same shadows.
//
// THE DIRECTION FLIPS HARD AND THE INTENSITY DOES NOT. Blending the two
// directions would be meaningless -- they are opposite vectors, and halfway
// between them is the horizon, which is not where either body is. So the switch
// is a hard one at the horizon, and it is invisible because the host fades the
// moon's radiance in across the same crossing while the sun's own radiance is
// going to nothing. Nothing pops because nothing is bright at the moment it
// changes.
// ---------------------------------------------------------------------------
// THE MOON IS DRAWN BIGGER THAN LIFE, as it is in the JS engine -- 1.27
// degrees against the real 0.265. A real moon is three pixels and a phase
// cannot be read on three pixels, which is the whole point of drawing one.
//
// THE SAMPLED CONE SWAPS WITH IT, which is what keeps the MIS honest: the
// next-event estimator samples keyCosR and the miss path weights the disc
// against the pdf of that same cone, so whichever body is up is sampled and
// weighted at its own size. Drawing a disc larger than the cone that samples
// it is how a light ends up half double-counted and half missing.
static const float kMoonCosR = 0.999756245f;
float keyCosR(V6Sky sky) { return sky.sunDir.y > 0.0f ? kSunCosThetaMax : kMoonCosR; }
float keySolidAngle(V6Sky sky) { return TWO_PI * (1.0f - keyCosR(sky)); }

float3 keyDir(V6Sky sky) { return sky.sunDir.y > 0.0f ? sky.sunDir : sky.moonDir; }
// THE KEY IS BOOSTED OFF THE DISC, and the two cannot be the same number.
// moonRadiance is sized so the disc shows its phase under a daylight tone
// curve; the light needed to actually reach the forest floor through that same
// curve is far larger. Physically this is one body with one radiance, but a
// fixed ACES curve cannot hold both readings at once, so the split is where
// the compromise is made honestly rather than hidden in a magic constant.
float3 keyRadiance(V6Sky sky)
{
    return sky.sunDir.y > 0.0f ? sky.sunRadiance : sky.moonRadiance * sky.moonKey;
}

// THE DISC'S OWN RADIANCE, WITH NO KEY BOOST.
//
// The boost above buys a navigable forest floor and is the right number for
// anything lit THROUGH next event estimation. Anything that multiplies the key
// directly -- the cloud deck is the only such consumer today -- has to take
// this one instead, or it inherits a 300x that was calibrated against surfaces
// it is not.
float3 keyDiscRadiance(V6Sky sky)
{
    return sky.sunDir.y > 0.0f ? sky.sunRadiance : sky.moonRadiance;
}

// The face texture is a PARAMETER, not a global. Sky.slang is included by
// ProbeTrace as well, and a declared-but-unbound resource fails that dispatch
// rather than the branch that would have read it.
float3 moonDisk(V6Sky sky, float3 d, Texture2D<float4> moonTex, SamplerState moonSamp)
{
    const float s = dot(d, sky.moonDir);
    const float rr = (1.0f - s) / (1.0f - kMoonCosR);
    if (rr > 1.0f) return float3(0.0f);

    // It sets behind the horizon, for the same reason the sun does.
    const float up = smoothstep(-0.006f, 0.006f, d.y);
    const float md = smoothstep(1.0f, kMoonSoft, rr) * up;
    if (md <= 0.001f) return float3(0.0f);

    // R horizontal, U completing the frame and pointing up-sky at the moon.
    // GUARDED: at the zenith the cross product collapses and a zero-length axis
    // is a NaN disc.
    float3 R = cross(float3(0.0f, 1.0f, 0.0f), sky.moonDir);
    const float rl = length(R);
    R = rl > 1e-4f ? R / max(rl, 1e-6f) : float3(1.0f, 0.0f, 0.0f);
    const float3 U = cross(sky.moonDir, R);

    // Disc coordinates, -1..1, +y up the sky.
    const float angR = sqrt(max(0.0f, 2.0f * (1.0f - kMoonCosR)));
    const float2 du = float2(dot(d, R), dot(d, U)) / angR;
    const float r2 = saturate(dot(du, du));

    // ── THE SURFACE, FROM THE PHOTOGRAPH ────────────────────────────────────
    //
    // SAMPLED FLAT, and that is correct rather than lazy: moon.png is already
    // the orthographic projection of a sphere seen from very far away, and the
    // Moon is tidally locked so it is always the same hemisphere. A flat
    // billboard of an orthographic photo IS the right mapping. Re-projecting it
    // onto a sphere would apply the foreshortening twice and smear the limb.
    //
    // v IS NEGATED. du.y increases with SKY-UP by construction, but a texture.s
    // v increases DOWN the image -- row 0 is the top. Feeding one straight into
    // the other lands sky-up on image-bottom and draws the face upside down.
    const float2 uv = saturate(float2(du.x, -du.y) * (0.5f / kMoonCrop) + 0.5f);
    float3 face = moonTex.SampleLevel(moonSamp, uv, 0.0f).rgb;
    // The maria are subtle in a photo that has been through a tone curve twice.
    // Pushing contrast about a pivot keeps the mean where it was.
    face = max(float3(0.0f), (face - kMoonPiv) * kMoonCon + kMoonPiv);

    // +z points at the observer, so this is the visible hemisphere of a sphere.
    const float3 N = float3(du.x, du.y, sqrt(max(0.0f, 1.0f - r2)));
    const float alpha = sky.moonPhase * 6.2831853f;
    const float3 Ld = float3(sin(alpha), 0.0f, cos(alpha));

    const float mu0 = dot(N, Ld);
    const float mu = N.z;
    const float ls = max(mu0, 0.0f) / max(mu0 + mu, 1e-3f);
    const float lit = saturate((kMoonLL * ls + (1.0f - kMoonLL) * max(mu0, 0.0f)) * kMoonGain);

    // EARTHSHINE. The dark side is not black: it is lit by a full Earth, about
    // a ten-thousandth of direct sunlight but easily visible beside a thin
    // crescent, and BLUE because it is sunlight that has already bounced off an
    // ocean. It is what makes "the old moon in the new moon's arms" the
    // familiar sight it is; without it a crescent reads as a broken disc.
    const float3 earth = float3(0.55f, 0.68f, 1.00f) * kMoonEarth;

    // A crescent's surface is dimmer than a full moon's, but not proportionally
    // -- the opposition surge is a brightening AT full, not a darkening
    // everywhere else.
    const float phaseLit = 0.5f + 0.5f * cos(alpha);
    const float surf = kMoonPhMin + (1.0f - kMoonPhMin) * phaseLit;

    return (sky.moonRadiance * face * (lit * surf) + sky.moonRadiance * earth) * md;
}
