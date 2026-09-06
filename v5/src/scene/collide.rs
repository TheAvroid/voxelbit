// ---------------------------------------------------------------------------
// collide.rs -- the things in the wood a body can walk into.
//
// The terrain needs nothing here. It is a height field and a pure function of
// position, so the player queries it directly and the answer does not depend on
// what the renderer has resident. Trees and rocks are not like that: they are
// instanced models, and the only place a particular tree's position exists at
// all is the transform the chunk builds for it. So a collider is made ALONGSIDE
// that transform, out of the same numbers -- see `make_instance` in scatter.rs,
// which fills one as it writes the other. Two independent placements would
// agree until the first time a rule changed, and then the trunk would be
// somewhere the tree was not, which is the kind of bug you cannot see in a
// screenshot.
//
// WHAT A COLLIDER IS: an upright elliptic cylinder -- a centre, two half
// extents and a top. Not the model's triangles, and not a ray into the
// acceleration structure either. Both of those are exact and both would tie
// where you can walk to what happens to be built on the GPU this frame, which
// is the one property the ground query was written to avoid.
//
// The ellipse rather than the box it is inscribed in, because a boulder is
// round and so is a trunk. On a five-metre rock the box's corner is a metre of
// invisible wall, and invisible walls in a wood are the thing players notice.
//
// WHY THE COLLIDER IS MEASURED FROM THE VOXELS. A pine model is 22 metres tall
// and its canopy starts NINE metres up; the trunk under it is 40 to 80 cm
// across depending on which of the nine it is, and in several of them the trunk
// is not centred in the model. One guessed radius would be wrong twice over --
// too fat for the thin ones, and off-centre for most. `measure_collider` takes
// the model's extent in the first two metres above its base, which is the only
// part of it a walking body can ever reach.
// ---------------------------------------------------------------------------

use crate::scene::vox::VoxAsset;

/// How tall a body is, in metres -- the player's eye height plus a little. The
/// collider is measured over this much of the model and no more: a pine's
/// branches above it are not something a walking body can be stopped by.
pub const BODY_HEIGHT_M: f32 = 2.0;

/// A collider in the model's own frame, in metres, measured from the centre of
/// the model's footprint -- which is the point a placement sets on the column.
#[derive(Default, Clone, Copy)]
pub struct ModelCollider {
    pub cx: f32,
    pub cz: f32,
    pub hx: f32,
    pub hz: f32,
    /// The highest voxel, above the model's base.
    pub top: f32,
}

impl ModelCollider {
    #[inline]
    pub fn solid(&self) -> bool {
        self.hx > 0.0 && self.hz > 0.0
    }
}

/// One solid thing, placed in the world.
#[derive(Clone, Copy)]
pub struct Solid {
    /// Centre of the footprint, world metres.
    pub cx: f32,
    pub cz: f32,
    /// Half extents; the footprint is the ellipse inscribed in them.
    pub hx: f32,
    pub hz: f32,
    /// World y of the top.
    pub top: f32,
    /// A rock is standable -- you can be blocked by one, step up onto a small
    /// one, or land on it. A trunk is not: its top is a canopy twenty metres up
    /// and nothing should ever be put there.
    pub standable: bool,
}

impl Default for Solid {
    fn default() -> Self {
        // Zero extents by default and never pushed that way -- a Solid that was
        // never filled in must not read as a one-metre block at the origin.
        Self {
            cx: 0.0,
            cz: 0.0,
            hx: 0.0,
            hz: 0.0,
            top: 0.0,
            standable: false,
        }
    }
}

/// Does a body of half-width `w`, centred at (x, z), touch the footprint?
///
/// Exact rather than approximate. Dividing through by the half extents maps the
/// ellipse to a unit circle and the body's square to a rectangle, and the
/// closest-point distance from a rectangle to the origin is the same expression
/// in either space -- so this is a box-circle test, which has an exact form,
/// wearing an ellipse's clothes.
#[inline]
pub fn touches(s: &Solid, x: f32, z: f32, w: f32) -> bool {
    let dx = ((x - s.cx).abs() - w).max(0.0) / s.hx;
    let dz = ((z - s.cz).abs() - w).max(0.0) / s.hz;
    dx * dx + dz * dz < 1.0
}

/// The widest extent of a model in the first `body_m` metres above its base,
/// plus its full height.
///
/// The height needs no search: `to_world` has already trimmed the asset to its
/// own bounding box, so the top voxel is the top layer.
pub fn measure_collider(a: &VoxAsset, voxel_m: f32, body_m: f32) -> ModelCollider {
    let mut c = ModelCollider::default();
    if a.sx <= 0 || a.sy <= 0 || a.sz <= 0 {
        return c;
    }
    c.top = a.sy as f32 * voxel_m;

    let slab = a.sy.min((body_m / voxel_m + 0.5) as i32).max(1);
    let (mut x0, mut x1) = (a.sx, -1);
    let (mut z0, mut z1) = (a.sz, -1);
    for y in 0..slab {
        for z in 0..a.sz {
            for x in 0..a.sx {
                if a.at(x, y, z) != 0 {
                    x0 = x0.min(x);
                    x1 = x1.max(x);
                    z0 = z0.min(z);
                    z1 = z1.max(z);
                }
            }
        }
    }
    if x1 < 0 {
        return c; // nothing at all at body height: not solid
    }

    c.hx = (x1 - x0 + 1) as f32 * voxel_m * 0.5;
    c.hz = (z1 - z0 + 1) as f32 * voxel_m * 0.5;
    c.cx = ((x0 + x1 + 1) as f32 * 0.5 - a.sx as f32 * 0.5) * voxel_m;
    c.cz = ((z0 + z1 + 1) as f32 * 0.5 - a.sz as f32 * 0.5) * voxel_m;
    c
}
