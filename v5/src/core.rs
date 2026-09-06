// ---------------------------------------------------------------------------
// core.rs -- the procedural basis for terrain, scatter and decoration, and the
// handful of scalar helpers the rest of the engine shapes itself with.
//
// Ported from v2's core/noise.h and core/vecmath.h unchanged in behaviour. The
// lattice hash stays in u32 integer maths, so a coordinate far from the origin
// hashes as well as the one at it -- that property is what lets the world be
// endless without a float-multiply hash losing its low bits a few kilometres
// out. Everything else is f32: v5, like v2, meshes a smooth triangle surface
// with no lattice thresholding anywhere, so f64 parity buys nothing.
// ---------------------------------------------------------------------------

/// Smoothstep on an already-normalised t. Used all over the terrain shaping.
#[inline]
pub fn sstep(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

#[inline]
pub fn saturate(x: f32) -> f32 {
    x.clamp(0.0, 1.0)
}

#[inline]
pub fn lerpf(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

/// Rec.709 luminance.
#[inline]
pub fn luminance(c: bevy::math::Vec3) -> f32 {
    0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z
}

/// Floor division and modulo that stay correct at negative coordinates -- the
/// world runs in both directions from the origin, and a truncating `/` folds
/// the negative side onto the positive one, which puts a seam through 0.
#[inline]
pub fn floor_div(a: i32, b: i32) -> i32 {
    if a >= 0 { a / b } else { -(((-a) + b - 1) / b) }
}

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

/// A 2D integer hash with a full 32-bit avalanche. Integer in, float out, so a
/// cell a million units from the origin hashes as well as the one at it.
#[inline]
pub fn ihash2(x: i32, z: i32) -> f32 {
    let mut h = (x as u32)
        .wrapping_mul(374_761_393)
        .wrapping_add((z as u32).wrapping_mul(668_265_263));
    h = (h ^ (h >> 13)).wrapping_mul(1_274_126_177);
    h ^= h >> 16;
    h as f32 * 2.328_306_4e-10 // / 2^32
}

/// A seeded scalar hash, for per-tree and per-branch decisions.
#[inline]
pub fn hash_u32(a: u32, b: u32) -> u32 {
    let mut h = a
        .wrapping_mul(374_761_393)
        .wrapping_add(b.wrapping_mul(668_265_263));
    h = (h ^ (h >> 13)).wrapping_mul(1_274_126_177);
    h ^ (h >> 16)
}

#[inline]
pub fn hash_unit(a: u32, b: u32) -> f32 {
    hash_u32(a, b) as f32 * 2.328_306_4e-10
}

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

/// 2D value noise: bilinear smoothstep over the hash lattice, one unit per cell.
#[inline]
pub fn vnoise(x: f32, z: f32) -> f32 {
    let fx = x.floor();
    let fz = z.floor();
    let ix = fx as i32;
    let iz = fz as i32;
    let tx = sstep(x - fx);
    let tz = sstep(z - fz);
    let a = ihash2(ix, iz);
    let b = ihash2(ix + 1, iz);
    let c = ihash2(ix, iz + 1);
    let d = ihash2(ix + 1, iz + 1);
    lerpf(lerpf(a, b, tx), lerpf(c, d, tx), tz)
}

/// Fractal brownian motion. The offsets decorrelate the lattices, which would
/// otherwise all pass through the origin together and leave a visible cross
/// there.
pub fn fbm(x: f32, z: f32, octaves: i32) -> f32 {
    let mut sum = 0.0;
    let mut amp = 0.5;
    let mut freq = 1.0;
    let mut norm = 0.0;
    let mut ox = 0.0;
    let mut oz = 0.0;
    for _ in 0..octaves {
        sum += vnoise(x * freq + ox, z * freq + oz) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2.03; // not exactly 2, so octaves never re-align on the lattice
        ox += 17.3;
        oz += 9.7;
    }
    sum / norm
}

/// Ridged multifractal: 1 - |2n - 1|, squared. This is what puts creases along
/// the tops of the ridges instead of the rounded domes plain fbm gives, and a
/// conifer landscape reads as ridge-and-valley more than as dunes.
pub fn ridged(x: f32, z: f32, octaves: i32) -> f32 {
    let mut sum = 0.0;
    let mut amp = 0.5;
    let mut freq = 1.0;
    let mut norm = 0.0;
    let mut ox = 0.0;
    let mut oz = 0.0;
    for _ in 0..octaves {
        let mut n = vnoise(x * freq + ox, z * freq + oz);
        n = 1.0 - (2.0 * n - 1.0).abs();
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2.03;
        ox += 23.1;
        oz += 31.9;
    }
    sum / norm
}

/// Displacing the sample point by another noise field before evaluating.
/// Without it every ridge runs along the lattice axes; with it they meander the
/// way an eroded ridge does.
pub fn warped_fbm(x: f32, z: f32, warp: f32, octaves: i32) -> f32 {
    let wx = fbm(x * 0.5 + 5.2, z * 0.5 + 1.3, 3) - 0.5;
    let wz = fbm(x * 0.5 + 9.1, z * 0.5 + 7.7, 3) - 0.5;
    fbm(x + wx * warp, z + wz * warp, octaves)
}
