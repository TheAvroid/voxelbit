//! The voxelbit wordmark, exactly as the website draws it (user 2026-09-23:
//! "make the voxelbit text in the top left match the voxelbit text on the
//! website"). website/index.html, "the wordmark", itself lifted from v1's
//! title screen, stacks three layers:
//!
//!   1. a GOLD BEVEL: eight hard copies of the letters, offset 1/15 em in the
//!      eight directions, lit from the top-left (text-shadow on ::before);
//!   2. the FACE: "voxel" crimson and "bit" lime, each a vertical ramp with a
//!      white sheen raked across it at 104 degrees, clipped to the letters
//!      (background-clip: text on ::after);
//!   3. a DROP SHADOW of the whole stack, 0.083/0.1 em down-right with a
//!      0.067 em blur at 60% black (filter: drop-shadow on the h1).
//!
//! egui paints text in one flat colour and cannot blur, so the mark is
//! composited here on the CPU, layer by layer in the browser's order, into an
//! image the app draws 1:1. Same font, same letter-spacing (1/15 em), same
//! colour stops, same angles. The website's version badge is left off: it is
//! the game's version, not this program's.

use ab_glyph::{Font, FontRef, PxScale, ScaleFont, point};
use eframe::egui::{Color32, ColorImage};

use crate::PX3;

type Rgba = [f32; 4]; // straight (not premultiplied) sRGB, 0..1

fn hex(v: u32) -> [f32; 3] {
    [((v >> 16) & 255) as f32 / 255.0, ((v >> 8) & 255) as f32 / 255.0, (v & 255) as f32 / 255.0]
}

/// The bevel, in the CSS's own order: the FIRST shadow is painted on top.
/// Offsets in units of the bevel step (1/15 em).
const BEVEL: [(i32, i32, u32); 8] = [
    (0, -1, 0xffe6a8),
    (-1, -1, 0xffeec2),
    (1, -1, 0xf7d288),
    (-1, 0, 0xeec684),
    (1, 0, 0xd2a453),
    (-1, 1, 0xab7d31),
    (0, 1, 0x916522),
    (1, 1, 0x754e17),
];

const VOXEL: [(f32, u32); 4] = [(0.0, 0xffc8ce), (0.16, 0xff7a8a), (0.58, 0xc8263a), (1.0, 0x7d0c1c)];
const BIT: [(f32, u32); 4] = [(0.0, 0xe6ffbe), (0.16, 0xb8ff6e), (0.58, 0x5cbc26), (1.0, 0x2a7d10)];
/// The sheen: transparent 34%, white at 30% alpha 46%, transparent 56%.
const SHEEN: [(f32, f32); 3] = [(0.34, 0.0), (0.46, 0.30), (0.56, 0.0)];
const SHEEN_DEG: f32 = 104.0;

fn ramp(stops: &[(f32, u32)], t: f32) -> [f32; 3] {
    let t = t.clamp(0.0, 1.0);
    for w in stops.windows(2) {
        let ((t0, c0), (t1, c1)) = (w[0], w[1]);
        if t <= t1 {
            let k = if t1 > t0 { (t - t0) / (t1 - t0) } else { 0.0 };
            let (a, b) = (hex(c0), hex(c1));
            return [a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k];
        }
    }
    hex(stops[stops.len() - 1].1)
}

fn sheen(t: f32) -> f32 {
    if t <= SHEEN[0].0 || t >= SHEEN[2].0 {
        return 0.0;
    }
    let (a, b) = if t <= SHEEN[1].0 { (SHEEN[0], SHEEN[1]) } else { (SHEEN[1], SHEEN[2]) };
    a.1 + (b.1 - a.1) * (t - a.0) / (b.0 - a.0)
}

/// Source-over, straight alpha.
fn over(dst: &mut Rgba, src: [f32; 3], a: f32) {
    let out_a = a + dst[3] * (1.0 - a);
    if out_a <= 0.0 {
        return;
    }
    for i in 0..3 {
        dst[i] = (src[i] * a + dst[i] * dst[3] * (1.0 - a)) / out_a;
    }
    dst[3] = out_a;
}

pub struct Mark {
    pub image: ColorImage,
    /// Where the text's line box starts inside the image, in pixels. The
    /// padding around it holds the bevel and the shadow.
    pub origin: [f32; 2],
    /// Distance from the line box's top to the baseline, pixels.
    pub baseline: f32,
    /// The line box's width, letter-spacing included, pixels.
    pub width: f32,
}

/// Composite the mark at an em of `em` pixels. Whole pixels throughout only
/// when `em` is a multiple of 15 (the bevel step and the letter-spacing are
/// 1/15 em) -- the caller picks such a size.
pub fn render(em: f32) -> Mark {
    let font = FontRef::try_from_slice(PX3).expect("3x3-pixel.otf parses");
    let upem = font.units_per_em().unwrap_or(640.0);
    // ab_glyph's PxScale is the pixel height of ascent-descent, not the em.
    let scaled = font.as_scaled(PxScale::from(em * font.height_unscaled() / upem));
    let ascent = scaled.ascent();
    let line_h = scaled.height(); // the browser's line box: win/typo metrics agree, 2 em
    let step = em / 15.0;
    // filter: drop-shadow()'s third length IS the standard deviation (its
    // SVG equivalent is feGaussianBlur stdDeviation=<it>) -- unlike
    // box-shadow, where the blur radius is 2 sigma. Halving it here, as for
    // box-shadow, drew a shadow visibly harder than Chrome's.
    let (sdx, sdy, sigma) = (0.083 * em, 0.1 * em, 0.067 * em);
    let pad = (0.45 * em).ceil(); // bevel + shadow offset + 3 sigma of blur

    // Lay the two words out with CSS letter-spacing after every character.
    let words: [(&str, &[(f32, u32); 4]); 2] = [("voxel", &VOXEL), ("bit", &BIT)];
    let mut boxes = Vec::new();
    let mut pen = pad;
    let mut glyphs = Vec::new();
    for (wi, (word, _)) in words.iter().enumerate() {
        let start = pen;
        for c in word.chars() {
            let id = font.glyph_id(c);
            glyphs.push((wi, id.with_scale_and_position(scaled.scale(), point(pen, pad + ascent))));
            pen += scaled.h_advance(id) + step;
        }
        boxes.push((start, pen));
    }
    let (w, h) = ((pen + pad).ceil() as usize, (pad * 2.0 + line_h).ceil() as usize);

    // Coverage, and which word each pixel belongs to (the faces differ).
    let mut mask = vec![0.0f32; w * h];
    let mut owner = vec![0usize; w * h];
    for (wi, g) in glyphs {
        if let Some(outlined) = font.outline_glyph(g) {
            let b = outlined.px_bounds();
            outlined.draw(|x, y, c| {
                let (px, py) = (b.min.x as i32 + x as i32, b.min.y as i32 + y as i32);
                if px >= 0 && py >= 0 && (px as usize) < w && (py as usize) < h {
                    let i = py as usize * w + px as usize;
                    mask[i] = (mask[i] + c).min(1.0);
                    owner[i] = wi;
                }
            });
        }
    }
    let at = |x: i32, y: i32| -> f32 {
        if x < 0 || y < 0 || x as usize >= w || y as usize >= h { 0.0 } else { mask[y as usize * w + x as usize] }
    };

    // 1. The bevel, last shadow first so the first lands on top.
    let mut out = vec![[0.0f32; 4]; w * h];
    let s = step.round() as i32;
    for &(ox, oy, color) in BEVEL.iter().rev() {
        for y in 0..h as i32 {
            for x in 0..w as i32 {
                let a = at(x - ox * s, y - oy * s);
                if a > 0.0 {
                    over(&mut out[y as usize * w + x as usize], hex(color), a);
                }
            }
        }
    }

    // 2. The face, over every bevel (z-index 2 over 1 across both words).
    let (sin, cos) = (SHEEN_DEG.to_radians().sin(), SHEEN_DEG.to_radians().cos());
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            if mask[i] <= 0.0 {
                continue;
            }
            let (x0, x1) = boxes[owner[i]];
            let (bw, bh) = (x1 - x0, line_h);
            let (px, py) = (x as f32 + 0.5, y as f32 + 0.5);
            let mut c = ramp(words[owner[i]].1, (py - pad) / bh);
            // CSS gradient geometry: the line through the box's centre along
            // the angle, as long as the box's projection on it.
            let len = (bw * sin).abs() + (bh * cos).abs();
            let t = ((px - (x0 + bw / 2.0)) * sin - (py - (pad + bh / 2.0)) * cos) / len + 0.5;
            let a = sheen(t);
            for k in 0..3 {
                c[k] = c[k] * (1.0 - a) + a;
            }
            over(&mut out[i], c, mask[i]);
        }
    }

    // 3. The drop shadow: the composite's alpha, Gaussian-blurred, shifted,
    // at 60% black, UNDER everything.
    let alpha: Vec<f32> = out.iter().map(|p| p[3]).collect();
    let blurred = blur(&alpha, w, h, sigma);
    let sample = |x: f32, y: f32| -> f32 {
        let (x0, y0) = (x.floor(), y.floor());
        let (fx, fy) = (x - x0, y - y0);
        let g = |xi: f32, yi: f32| {
            if xi < 0.0 || yi < 0.0 || xi as usize >= w || yi as usize >= h { 0.0 } else { blurred[yi as usize * w + xi as usize] }
        };
        let top = g(x0, y0) * (1.0 - fx) + g(x0 + 1.0, y0) * fx;
        let bot = g(x0, y0 + 1.0) * (1.0 - fx) + g(x0 + 1.0, y0 + 1.0) * fx;
        top * (1.0 - fy) + bot * fy
    };
    let mut pixels = Vec::with_capacity(w * h);
    for y in 0..h {
        for x in 0..w {
            let mut p: Rgba = [0.0, 0.0, 0.0, 0.0];
            over(&mut p, [0.0, 0.0, 0.0], 0.6 * sample(x as f32 - sdx, y as f32 - sdy));
            let top = out[y * w + x];
            over(&mut p, [top[0], top[1], top[2]], top[3]);
            let q = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
            pixels.push(Color32::from_rgba_unmultiplied(q(p[0]), q(p[1]), q(p[2]), q(p[3])));
        }
    }

    let image = ColorImage::new([w, h], pixels);
    Mark { image, origin: [pad, pad], baseline: ascent, width: pen - pad }
}

fn blur(src: &[f32], w: usize, h: usize, sigma: f32) -> Vec<f32> {
    let r = (sigma * 3.0).ceil() as i32;
    let kernel: Vec<f32> = (-r..=r).map(|i| (-(i * i) as f32 / (2.0 * sigma * sigma)).exp()).collect();
    let sum: f32 = kernel.iter().sum();
    let kernel: Vec<f32> = kernel.iter().map(|k| k / sum).collect();
    let pass = |src: &[f32], horizontal: bool| {
        let mut dst = vec![0.0f32; w * h];
        for y in 0..h as i32 {
            for x in 0..w as i32 {
                let mut acc = 0.0;
                for (k, weight) in kernel.iter().enumerate() {
                    let d = k as i32 - r;
                    let (sx, sy) = if horizontal { (x + d, y) } else { (x, y + d) };
                    if sx >= 0 && sy >= 0 && (sx as usize) < w && (sy as usize) < h {
                        acc += src[sy as usize * w + sx as usize] * weight;
                    }
                }
                dst[y as usize * w + x as usize] = acc;
            }
        }
        dst
    };
    pass(&pass(src, true), false)
}

/// Where the font's own hyphen sits, as (top below the cap line, height),
/// in em. The face has no em dash, so the page draws one -- a bar at exactly
/// the hyphen's height and weight, as the website does for its rules.
pub fn hyphen_em() -> (f32, f32) {
    let font = FontRef::try_from_slice(PX3).expect("3x3-pixel.otf parses");
    let upem = font.units_per_em().unwrap_or(640.0);
    let b = font.outline(font.glyph_id('-')).map(|o| o.bounds);
    match b {
        // Outline bounds are font units, y up from the baseline; the cap
        // line is one em above it (five 128-unit cells in a 640 em).
        // ab_glyph hands these back with min/max swapped on y, so order them.
        Some(b) => {
            let (lo, hi) = (b.min.y.min(b.max.y), b.min.y.max(b.max.y));
            ((upem - hi) / upem, (hi - lo) / upem)
        }
        None => (0.6, 0.2),
    }
}
