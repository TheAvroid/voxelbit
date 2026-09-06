// ---------------------------------------------------------------------------
// scatter.rs -- what stands on a chunk, and where.
//
// Every decision is hashed on the WORLD column, never on a chunk-local one, so
// a tree is in the same place regardless of which chunk is meshing it and
// regardless of what the camera has visited. That is the property that makes
// eviction safe: a chunk rebuilt an hour later is identical, so walking away
// from a stand and back does not reshuffle it.
//
// Spacing is only enforced WITHIN a chunk. A proper Poisson process would need
// to see across the boundary, which would mean either a shared grid or meshing
// the neighbours first -- and the visible cost of not doing it is that two
// trees occasionally stand closer than they should across a seam. That is a far
// better trade than serialising the chunk builds.
// ---------------------------------------------------------------------------

use bevy::math::{Quat, Vec3};
use bevy::transform::components::Transform;
use std::f32::consts::FRAC_PI_2;

use crate::core::{fbm, hash_u32, hash_unit, saturate, sstep};
use crate::scene::collide::{ModelCollider, Solid};
use crate::scene::palette as mat;
use crate::scene::terrain::{VoxelTerrain, CHUNK_M, CHUNK_VOX, VOXEL_M};

/// Which family of model a placement is for. They differ only in what scatters
/// them and how deep it sinks them.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Pine = 0,
    Rock = 1,
    Flower = 2,
}

/// What a worker decides to put on the ground, in world column coordinates.
#[derive(Clone, Copy)]
pub struct Placement {
    pub kind: Kind,
    /// Which model of that kind.
    pub index: usize,
    /// World column.
    pub ci: i32,
    pub cj: i32,
    /// Terrain height there, in voxels.
    pub h: i32,
    /// Quarter turns.
    pub yaw: i32,
    /// Hash stream for tint and sink.
    pub cell: u32,
}

/// The footprint of a decor model, so a worker can space them without reaching
/// into the meshes.
#[derive(Clone, Copy)]
pub struct Footprint {
    pub sx: i32,
    pub sz: i32,
    pub sy: i32,
    pub col: ModelCollider,
}

/// Everything the scatter needs that is not the terrain itself.
#[derive(Clone)]
pub struct ScatterParams {
    pub seed: u32,
    /// Multiplies the stand-density gate. The old engine capped the wood at a
    /// fixed number of trees over a fixed patch; an endless world has no total
    /// to cap, so the control has to be a DENSITY -- 1.0 is every site the
    /// terrain will accept, which is a genuinely dark forest to stand in.
    pub tree_density: f32,
    pub tree_stride: f32,
    /// Density INSIDE a colony, not over the whole wood: the patches cover a
    /// fifth of the ground, so a low number here is still a thick bed.
    pub rock_density: f32,
    pub flower_density: f32,
    pub pine_foot: Vec<Footprint>,
    pub rock_foot: Vec<Footprint>,
    pub flower_foot: Vec<Footprint>,
}

impl Default for ScatterParams {
    fn default() -> Self {
        Self {
            seed: 20_260_904,
            tree_density: 0.31,
            tree_stride: 2.4,
            rock_density: 0.010,
            flower_density: 0.45,
            pine_foot: Vec::new(),
            rock_foot: Vec::new(),
            flower_foot: Vec::new(),
        }
    }
}

impl ScatterParams {
    fn feet(&self, kind: Kind) -> &[Footprint] {
        match kind {
            Kind::Pine => &self.pine_foot,
            Kind::Rock => &self.rock_foot,
            Kind::Flower => &self.flower_foot,
        }
    }
}

// ---------------------------------------------------------------------------
// The scatter
// ---------------------------------------------------------------------------
pub fn scatter(terrain: &VoxelTerrain, p: &ScatterParams, cx: i32, cz: i32) -> Vec<Placement> {
    let mut out = Vec::new();
    let i0 = cx * CHUNK_VOX;
    let j0 = cz * CHUNK_VOX;
    let wl = (terrain.water_level / VOXEL_M) as i32;

    // ---- trees -------------------------------------------------------------
    if !p.pine_foot.is_empty() {
        struct Placed {
            x: f32,
            z: f32,
            r: f32,
        }
        let mut placed: Vec<Placed> = Vec::new();
        let steps = (CHUNK_M / p.tree_stride) as i32;
        for j in 0..=steps {
            for i in 0..=steps {
                let bx = i0 as f32 * VOXEL_M + i as f32 * p.tree_stride;
                let bz = j0 as f32 * VOXEL_M + j as f32 * p.tree_stride;
                let cell = hash_u32((bx * 16.0) as i32 as u32, ((bz * 16.0) as i32 as u32) ^ 0x9E37);

                let x = bx + (hash_unit(p.seed + 11, cell) - 0.5) * p.tree_stride * 1.8;
                let z = bz + (hash_unit(p.seed + 12, cell) - 0.5) * p.tree_stride * 1.8;

                let ci = (x / VOXEL_M).floor() as i32;
                let cj = (z / VOXEL_M).floor() as i32;
                if ci < i0 || ci >= i0 + CHUNK_VOX || cj < j0 || cj >= j0 + CHUNK_VOX {
                    continue; // it belongs to a neighbour
                }

                let h = terrain.height_vox(ci, cj);
                if h <= wl + 8 {
                    continue;
                }

                let slope = (terrain.height_vox(ci + 1, cj) - terrain.height_vox(ci - 1, cj))
                    .abs()
                    .max((terrain.height_vox(ci, cj + 1) - terrain.height_vox(ci, cj - 1)).abs());
                if slope >= VoxelTerrain::TREE_SLOPE {
                    continue;
                }

                let dens = terrain.stand_density(x, z);
                if hash_unit(p.seed + 13, cell)
                    > (saturate((dens - 0.30) / 0.32) * 0.92 + 0.05) * p.tree_density
                {
                    continue;
                }

                let k = (hash_unit(p.seed + 15, cell) * p.pine_foot.len() as f32) as usize
                    % p.pine_foot.len();
                let yaw = ((hash_unit(p.seed + 16, cell) * 4.0) as i32) & 3;
                let f = &p.pine_foot[k];
                let foot_x = if yaw & 1 == 1 { f.sz } else { f.sx } as f32 * VOXEL_M;
                let foot_z = if yaw & 1 == 1 { f.sx } else { f.sz } as f32 * VOXEL_M;
                let keep = 1.5f32.max(0.30 * foot_x.max(foot_z));

                let clash = placed.iter().any(|q| {
                    let dx = q.x - x;
                    let dz = q.z - z;
                    let r = keep.max(q.r);
                    dx * dx + dz * dz < r * r
                });
                if clash {
                    continue;
                }

                placed.push(Placed { x, z, r: keep });
                out.push(Placement {
                    kind: Kind::Pine,
                    index: k,
                    ci,
                    cj,
                    h,
                    yaw,
                    cell,
                });
            }
        }
    }

    // ---- rocks and flowers -------------------------------------------------
    // Both are scattered on a finer grid than the trees and take whatever
    // ground is left; a rock may sit on rock or soil, a flower only on grass.
    // Neither respects the tree spacing on purpose -- a boulder half under a
    // canopy is what a real wood looks like.
    scatter_small(terrain, p, cx, cz, Kind::Rock, p.rock_density, 1.6, false, &mut out);
    scatter_small(
        terrain,
        p,
        cx,
        cz,
        Kind::Flower,
        p.flower_density,
        0.9,
        true,
        &mut out,
    );

    out
}

// ---------------------------------------------------------------------------
// Flowers grow in COLONIES, ONE SPECIES TO A COLONY. Roses stand with roses,
// lavender with lavender.
//
// A uniform random scatter cannot express either half of that. Raise its
// density and you get an even wash over the whole wood, which reads as
// wallpaper; pick the model per site and you get every species shuffled through
// every patch, which reads as a seed packet rather than a plant that spread
// from one root.
//
// So a colony is an OBJECT here, not a threshold on a noise field. Sites sit on
// a jittered eighteen-metre grid, two thirds of them are used, and each one
// carries its own radius AND its own species. A point takes both from whichever
// site covers it most strongly, so the species cannot change inside a patch --
// that is the property an fbm-threshold field could not offer at any frequency,
// because the field said where the flowers were and nothing at all about what
// they were.
//
// Two colonies of different species can still meet, and that is fine: the
// handover happens where both are at the outer edge of their falloff and there
// is almost nothing growing on either side of the line.
// ---------------------------------------------------------------------------
#[derive(Default, Clone, Copy)]
struct Colony {
    /// 0..1, how strongly this ground is inside a patch.
    w: f32,
    /// The one flower model the whole patch is made of.
    species: usize,
}

/// Eighteen metres between sites, and the site sits in the middle half of its
/// cell. Both numbers are about SEPARATION: two neighbouring colonies that
/// overlap heavily are two species mixed again, just in bigger lumps.
const COLONY_CELL: f32 = 18.0;

fn colony_at(seed: u32, x: f32, z: f32, species_count: usize) -> Colony {
    let mut best = Colony::default();
    if species_count == 0 {
        return best;
    }
    let gi = (x / COLONY_CELL).floor() as i32;
    let gj = (z / COLONY_CELL).floor() as i32;

    // The radius is modulated by a ~five-metre noise, sampled at the QUERY
    // point rather than the site, so the outline is lobed and irregular instead
    // of a disc -- a circle of flowers is as obviously authored as no flowers.
    let wob = 0.70 + 0.60 * fbm(x * 0.19 + 133.7, z * 0.19 + 91.4, 2);

    for dj in -1..=1 {
        for di in -1..=1 {
            let c = hash_u32((gi + di) as u32 ^ 0x9E37_79B9, (gj + dj) as u32);
            // Most of the ground has no colony on it at all. Without this the
            // patches tile and the wood is uniformly flowered again, in lumps.
            if hash_unit(seed + 61, c) > 0.68 {
                continue;
            }

            let sx = ((gi + di) as f32 + 0.25 + 0.50 * hash_unit(seed + 62, c)) * COLONY_CELL;
            let sz = ((gj + dj) as f32 + 0.25 + 0.50 * hash_unit(seed + 63, c)) * COLONY_CELL;
            let rad = (3.0 + 4.0 * hash_unit(seed + 64, c)) * wob;
            let dx = x - sx;
            let dz = z - sz;
            let d = (dx * dx + dz * dz).sqrt();
            if d >= rad {
                continue;
            }

            // Full density through the middle, fading over the outer third so a
            // patch thins out at its edge the way a spreading plant does.
            let w = sstep(saturate((rad - d) / (0.34 * rad)));
            if w <= best.w {
                continue;
            }
            best.w = w;
            best.species = (hash_unit(seed + 65, c) * species_count as f32) as usize % species_count;
        }
    }
    best
}

#[allow(clippy::too_many_arguments)]
fn scatter_small(
    terrain: &VoxelTerrain,
    p: &ScatterParams,
    cx: i32,
    cz: i32,
    kind: Kind,
    density: f32,
    stride: f32,
    grass_only: bool,
    out: &mut Vec<Placement>,
) {
    let feet = p.feet(kind);
    if feet.is_empty() || density <= 0.0 {
        return;
    }
    let i0 = cx * CHUNK_VOX;
    let j0 = cz * CHUNK_VOX;
    let wl = (terrain.water_level / VOXEL_M) as i32;
    let steps = (CHUNK_M / stride) as i32;
    let salt: u32 = if kind == Kind::Rock {
        0x51ED_2701
    } else {
        0x27D4_EB2F
    };

    for j in 0..=steps {
        for i in 0..=steps {
            let bx = i0 as f32 * VOXEL_M + i as f32 * stride;
            let bz = j0 as f32 * VOXEL_M + j as f32 * stride;
            let cell = hash_u32(((bx * 16.0) as i32 as u32) ^ salt, (bz * 16.0) as i32 as u32);

            // Only the flowers colonise; a rock is where a rock is. Asked at
            // the CELL BASE, not at the jittered position below, so every
            // flower in a patch answers from the same colony.
            let col = if kind == Kind::Flower {
                let c = colony_at(p.seed, bx, bz, feet.len());
                if c.w <= 0.0 {
                    continue;
                }
                c
            } else {
                Colony { w: 1.0, species: 0 }
            };
            if hash_unit(p.seed + 41, cell) >= density * col.w {
                continue;
            }

            let x = bx + (hash_unit(p.seed + 42, cell) - 0.5) * stride;
            let z = bz + (hash_unit(p.seed + 43, cell) - 0.5) * stride;
            let ci = (x / VOXEL_M).floor() as i32;
            let cj = (z / VOXEL_M).floor() as i32;
            if ci < i0 || ci >= i0 + CHUNK_VOX || cj < j0 || cj >= j0 + CHUNK_VOX {
                continue;
            }

            let h = terrain.height_vox(ci, cj);
            if h <= wl + 2 {
                continue;
            }
            let top = terrain.top_material_at(ci, cj, h);
            if grass_only && !mat::is_grass(top) {
                continue;
            }

            // The species is the COLONY's, not this cell's: that is the whole
            // point of the patch.
            let k = if kind == Kind::Flower {
                col.species
            } else {
                (hash_unit(p.seed + 44, cell) * feet.len() as f32) as usize % feet.len()
            };
            let yaw = ((hash_unit(p.seed + 45, cell) * 4.0) as i32) & 3;
            out.push(Placement {
                kind,
                index: k,
                ci,
                cj,
                h,
                yaw,
                cell,
            });
        }
    }
}

// ---------------------------------------------------------------------------
// The transform the renderer draws with and, through `solid_out`, the collider
// the player walks into.
//
// ONE function on purpose: the collider is read back out of the transform this
// just wrote, so there is no second copy of the placement arithmetic that could
// drift away from the first.
//
// ROTATION IS IN QUARTER TURNS ONLY. An arbitrary yaw would put a voxel model
// off the lattice its own faces are aligned to, and the crisp axis-aligned
// silhouette that makes a voxel tree look like a voxel tree would turn into
// stair-stepped mush. It is also what lets the collider's half extents be
// SWAPPED rather than rotated, which keeps the ellipse an axis-aligned one.
// ---------------------------------------------------------------------------
pub fn make_instance(
    p: &Placement,
    f: &Footprint,
    seed: u32,
    solid_out: Option<&mut Solid>,
) -> Transform {
    let yaw = p.yaw & 3;
    let rot = Quat::from_rotation_y(yaw as f32 * FRAC_PI_2);

    // A tree standing exactly on the surface looks like it is on tiptoe, so it
    // is sunk a voxel or two. A rock is sunk in proportion to its own height,
    // which is what makes a boulder read as embedded in the ground rather than
    // set down on it.
    let sink = match p.kind {
        Kind::Pine => 1 + (hash_unit(seed + 17, p.cell) * 2.0) as i32,
        Kind::Rock => 1 + (f.sy as f32 * 0.18) as i32,
        Kind::Flower => 1,
    };

    // The model's own centre, in its local frame. The transform is built so
    // that this point lands exactly on the world column the placement chose --
    // rotate about the model origin, then translate the rotated centre onto it.
    let c = Vec3::new(f.sx as f32 * VOXEL_M * 0.5, 0.0, f.sz as f32 * VOXEL_M * 0.5);
    let rc = rot * c;
    let translation = Vec3::new(
        p.ci as f32 * VOXEL_M - rc.x,
        (p.h + 1 - sink) as f32 * VOXEL_M,
        p.cj as f32 * VOXEL_M - rc.z,
    );

    if let Some(s) = solid_out {
        if f.col.solid() {
            // The collider's centre in the model's own frame, put through the
            // same transform.
            let q = Vec3::new(c.x + f.col.cx, 0.0, c.z + f.col.cz);
            let w = rot * q + translation;
            s.cx = w.x;
            s.cz = w.z;
            // A quarter turn swaps the extents rather than rotating them.
            let swap = yaw & 1 == 1;
            s.hx = if swap { f.col.hz } else { f.col.hx };
            s.hz = if swap { f.col.hx } else { f.col.hz };
            s.top = translation.y + f.col.top;
            s.standable = p.kind == Kind::Rock;
        }
    }

    Transform {
        translation,
        rotation: rot,
        scale: Vec3::ONE,
    }
}

/// A few percent of per-tree hue. Nine models over thousands of trees would
/// otherwise show their repeat.
///
/// v2 carried this per INSTANCE, in an OptiX instanceId that indexed a tint
/// table. Solari resolves a material per instance and nothing finer, so the
/// tint has to BE a material: `TINT_VARIANTS` copies of the shared palette
/// material differing only in `base_color`, picked here by hash. The mesh is
/// unchanged, so all of them still share one BLAS -- the variants cost a few
/// material slots and nothing else.
pub const TINT_VARIANTS: usize = 8;

pub fn tint_variant(seed: u32, cell: u32) -> usize {
    (hash_unit(seed + 19, cell) * TINT_VARIANTS as f32) as usize % TINT_VARIANTS
}

/// The colour of tint variant `k`, as a multiplier on the palette.
pub fn tint_color(k: usize) -> Vec3 {
    let t = (k as f32 + 0.5) / TINT_VARIANTS as f32;
    // A hue swing green-to-magenta across the variants, and a value swing
    // roughly in step with it, which is what v2's two hash streams produced
    // between them.
    let v = 0.90 + 0.20 * ((k * 5) % TINT_VARIANTS) as f32 / TINT_VARIANTS as f32;
    Vec3::new(
        crate::core::lerpf(0.94, 1.06, t),
        1.0,
        crate::core::lerpf(1.05, 0.92, t),
    ) * v
}
