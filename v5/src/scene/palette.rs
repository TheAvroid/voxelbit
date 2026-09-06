// ---------------------------------------------------------------------------
// palette.rs -- the material table, and the one trick that makes it fit Solari.
//
// v2 stored a material id per TRIANGLE and looked it up in a device-side table
// from the closest-hit program. Bevy Solari has no such hook: a hit resolves
// exactly one `StandardMaterial`, chosen per INSTANCE. Taken literally that
// would mean splitting every chunk mesh into one sub-mesh per material -- up to
// fourteen entities and fourteen BLASes per chunk, thousands across the ring.
//
// So the material id is carried in the UV instead. The whole world shares ONE
// material whose base-colour and metallic-roughness maps are 255x1 palettes,
// and a vertex's UV is (id + 0.5) / 255 across, 0.5 down. Solari samples both
// with `textureSampleLevel(..., 0.0)` -- level zero, nearest filtering, no mip
// chain -- so the fetch lands exactly on its texel and the id round-trips
// bit-exact. One mesh per chunk, one BLAS per chunk, and the per-face material
// v2 had, at the cost of two kilobytes of texture.
//
// WHAT DOES NOT SURVIVE THE PORT. Solari's material is base colour, roughness,
// metallic, emissive and a scalar reflectance; there is no translucency term.
// v2 gave needles 0.45 translucency so a backlit canopy did not read as a black
// cut-out. That is gone, and the lifted foliage albedo below is doing more work
// because of it -- see `for_model_color`.
// ---------------------------------------------------------------------------

use bevy::math::Vec3;
use std::collections::HashMap;

use crate::core::luminance;

// ---------------------------------------------------------------------------
// Material ids
// ---------------------------------------------------------------------------
pub const AIR: u8 = 0;
pub const ROCK: u8 = 1;
pub const DIRT: u8 = 2;
pub const MOSS: u8 = 3;
pub const SAND: u8 = 4;
pub const SILT: u8 = 5;
/// The dropped-needle floor a conifer stand builds.
pub const NEEDLE_LITTER: u8 = 6;

// GROUND COLOURS BORROWED FROM THE TREES.
//
// A single flat green for grass and a single flat brown for soil is what made
// the floor read as a painted plane under a detailed canopy: the eye finds the
// repeat instantly when a whole hillside is one value. These slots are filled
// AFTER the pines load, from the greens and browns the models actually use, so
// the ground is made of the same palette as the things standing in it.
pub const GRASS_0: u8 = 7;
pub const GRASS_COUNT: u8 = 4; // 7..10
pub const SOIL_0: u8 = 11;
pub const SOIL_COUNT: u8 = 3; // 11..13
/// Model palette entries are allocated from here up.
pub const TREE_BASE: u8 = 14;
pub const COUNT: usize = 255;

#[inline]
pub fn is_grass(m: u8) -> bool {
    m >= GRASS_0 && m < GRASS_0 + GRASS_COUNT
}

/// Everything the BSDF asks a surface for, per material id.
#[derive(Clone, Copy)]
pub struct MaterialLook {
    pub albedo: Vec3,
    pub roughness: f32,
    pub specular: f32,
    /// Kept from the port because it is what classifies foliage in
    /// `derive_ground_from_trees`, even though Solari cannot shade with it.
    pub translucency: f32,
}

impl Default for MaterialLook {
    fn default() -> Self {
        Self {
            albedo: Vec3::splat(0.4),
            roughness: 0.95,
            specular: 0.03,
            translucency: 0.0,
        }
    }
}

#[inline]
fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.003_130_8 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

#[inline]
fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.040_45 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

/// The UV a vertex of material `id` carries. Dead centre of the id's texel, so
/// nothing about sampler rounding can move it to a neighbour.
#[inline]
pub fn uv_for(id: u8) -> [f32; 2] {
    [(id as f32 + 0.5) / COUNT as f32, 0.5]
}

// ---------------------------------------------------------------------------
// The material table
//
// Palette entries arrive per model and are deduplicated by colour, so nine pine
// variants that share their greens share their material slots too.
//
// FOLIAGE IS CLASSIFIED FROM THE COLOUR, not from a palette index range. The
// nine assets do not agree on where in the palette their needles live, and
// hardcoding a split would put bark roughness on needles for most of them.
// ---------------------------------------------------------------------------
pub struct Palette {
    look: Vec<MaterialLook>,
    index: HashMap<u32, u8>,
    next: u8,
    overflow: u32,
}

impl Default for Palette {
    fn default() -> Self {
        let mut p = Self {
            look: vec![MaterialLook::default(); COUNT],
            index: HashMap::new(),
            next: TREE_BASE,
            overflow: 0,
        };
        p.build_ground();
        p
    }
}

impl Palette {
    pub fn for_model_color(&mut self, c: [u8; 4]) -> u8 {
        let key = ((c[0] as u32) << 16) | ((c[1] as u32) << 8) | c[2] as u32;
        if let Some(&id) = self.index.get(&key) {
            return id;
        }
        if self.next as usize >= COUNT {
            self.overflow += 1;
            return AIR;
        }

        let id = self.next;
        self.next += 1;
        let m = &mut self.look[id as usize];
        m.albedo = Vec3::new(
            srgb_to_linear(c[0] as f32 / 255.0),
            srgb_to_linear(c[1] as f32 / 255.0),
            srgb_to_linear(c[2] as f32 / 255.0),
        );

        // Green-dominant is foliage; anything else on a conifer is wood.
        if c[1] > c[0] && c[1] > c[2] {
            m.roughness = 0.50;
            m.specular = 0.045;
            // Kept as the FOLIAGE FLAG even though Solari has no transmission
            // term: derive_ground_from_trees splits the table on it.
            m.translucency = 0.45;
            // The authored olive is very dark once linearised, and a canopy of
            // it reads as a black mass under its own shadow. A gentle lift
            // toward the asset's own hue keeps the colour and finds the form.
            //
            // 1.7x is v2's number and it is kept, but it is doing more here:
            // there, needles also passed 45% of the light through, which is
            // what stopped a backlit crown reading as a cut-out. Solari cannot
            // do that, so the lift is the whole of the compensation now.
            m.albedo = m.albedo * 1.7 + Vec3::new(0.012, 0.020, 0.006);
        } else {
            m.roughness = 0.88; // bark is coarse
            m.specular = 0.020;
            m.translucency = 0.0;
        }
        self.index.insert(key, id);
        id
    }

    pub fn used(&self) -> usize {
        self.next as usize
    }

    pub fn overflowed(&self) -> u32 {
        self.overflow
    }

    fn set(&mut self, id: u8, r: f32, g: f32, b: f32, rough: f32) {
        let m = &mut self.look[id as usize];
        m.albedo = Vec3::new(r, g, b);
        m.roughness = rough;
        m.specular = 0.025;
        m.translucency = 0.0;
    }

    fn build_ground(&mut self) {
        // Deliberately desaturated: light bounces between these surfaces many
        // times, and a saturated ground compounds into a colour cast over
        // everything above it. These are LINEAR albedos already.
        self.set(ROCK, 0.42, 0.41, 0.39, 0.88);
        self.set(DIRT, 0.29, 0.22, 0.15, 0.95);
        self.set(MOSS, 0.24, 0.34, 0.16, 0.92);
        self.set(SAND, 0.68, 0.61, 0.45, 0.85);
        self.set(SILT, 0.22, 0.20, 0.16, 0.95);
        self.set(NEEDLE_LITTER, 0.20, 0.14, 0.09, 0.97);
    }

    // -----------------------------------------------------------------------
    // Fill the ground slots from the colours the pines turned out to use.
    //
    // Called once, after every model has been through for_model_color and
    // before the terrain is meshed -- which is why the models load BEFORE the
    // world streams. The greens are sampled spread across the foliage entries
    // rather than taken consecutively: adjacent palette entries in a
    // MagicaVoxel model are usually a shading ramp of one hue, so the first
    // four would have been four barely-different greens.
    // -----------------------------------------------------------------------
    pub fn derive_ground_from_trees(&mut self) {
        let mut foliage: Vec<u8> = Vec::new();
        let mut bark: Vec<u8> = Vec::new();
        for i in TREE_BASE..self.next {
            let l = self.look[i as usize];
            if l.translucency > 0.0 {
                foliage.push(i);
                continue;
            }

            // NOT EVERY NON-GREEN ENTRY IS BARK. for_model_color classifies by
            // green dominance, so "bark" is really "everything else" -- and a
            // pine's palette carries reds and near-whites for cut ends and
            // highlights. Sampling those as soil painted scarlet and chalk-
            // white patches across whole hillsides.
            //
            // Soil has to be warm, mid-dark and unsaturated. Anything outside
            // that is a highlight, not a ground colour.
            let c = l.albedo;
            let lum = luminance(c);
            let warm = c.x >= c.y && c.y >= c.z;
            let max_c = c.x.max(c.y).max(c.z);
            let sat = if max_c > 0.0 {
                (max_c - c.x.min(c.y).min(c.z)) / max_c
            } else {
                0.0
            };
            if warm && lum > 0.015 && lum < 0.30 && sat < 0.72 {
                bark.push(i);
            }
        }

        for k in 0..GRASS_COUNT {
            let albedo = if foliage.is_empty() {
                // No models loaded: a plain green, so the world still renders.
                Vec3::new(
                    0.14 + 0.04 * k as f32,
                    0.26 + 0.05 * k as f32,
                    0.10 + 0.02 * k as f32,
                )
            } else {
                let pick = (foliage.len() * (2 * k as usize + 1)) / (2 * GRASS_COUNT as usize);
                let c = self.look[foliage[pick] as usize].albedo;
                // 0.45, not 1.0, and the reason is LIGHTING not colour. The
                // foliage value is tuned for needles sitting inside their own
                // crown, in shadow most of the day. Grass stands in the open
                // taking full sun, so the same albedo renders far brighter --
                // reading it across literally is what made the floor glow
                // yellow-green under a dark canopy.
                Vec3::new(c.x * 0.45 + 0.008, c.y * 0.44 + 0.010, c.z * 0.32)
            };
            let m = &mut self.look[(GRASS_0 + k) as usize];
            m.albedo = albedo;
            m.roughness = 0.88;
            m.specular = 0.022;
            m.translucency = 0.22;
        }

        for k in 0..SOIL_COUNT {
            let albedo = if bark.is_empty() {
                Vec3::new(
                    0.26 + 0.05 * k as f32,
                    0.19 + 0.03 * k as f32,
                    0.13 + 0.02 * k as f32,
                )
            } else {
                let pick = (bark.len() * (2 * k as usize + 1)) / (2 * SOIL_COUNT as usize);
                let c = self.look[bark[pick] as usize].albedo;
                // Soil is darker and less saturated than the trunk it came
                // from -- bark read literally makes the ground look like decking.
                Vec3::new(c.x * 0.85 + 0.020, c.y * 0.80 + 0.014, c.z * 0.78 + 0.010)
            };
            let m = &mut self.look[(SOIL_0 + k) as usize];
            m.albedo = albedo;
            m.roughness = 0.95;
            m.specular = 0.020;
            m.translucency = 0.0;
        }
    }

    // -----------------------------------------------------------------------
    // The two palette strips, as raw texture bytes.
    //
    // Base colour is sRGB-encoded because it is uploaded as Rgba8UnormSrgb and
    // the sampler decodes it back; the albedos above are linear, so the encode
    // here and the hardware decode there cancel exactly.
    //
    // Metallic-roughness follows the glTF convention Solari reads: roughness in
    // G, metallic in B. Everything in this world is a dielectric, so B is left
    // at zero and the shared material's `metallic` is zero as well -- either
    // one alone would be enough, since the two are multiplied.
    // -----------------------------------------------------------------------
    pub fn base_color_bytes(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(COUNT * 4);
        for m in &self.look {
            out.push((linear_to_srgb(m.albedo.x.clamp(0.0, 1.0)) * 255.0 + 0.5) as u8);
            out.push((linear_to_srgb(m.albedo.y.clamp(0.0, 1.0)) * 255.0 + 0.5) as u8);
            out.push((linear_to_srgb(m.albedo.z.clamp(0.0, 1.0)) * 255.0 + 0.5) as u8);
            out.push(255);
        }
        out
    }

    pub fn metallic_roughness_bytes(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(COUNT * 4);
        for m in &self.look {
            out.push(0);
            out.push((m.roughness.clamp(0.0, 1.0) * 255.0 + 0.5) as u8); // G: roughness
            out.push(0); // B: metallic
            out.push(255);
        }
        out
    }
}
