// ---------------------------------------------------------------------------
// vox.rs -- a MagicaVoxel .vox reader, ported from v2's scene/vox.h.
//
// TWO COORDINATE SYSTEMS, AND THE ONE PLACE THEY MEET.
// MagicaVoxel is Z-UP: a model's sz is its height and sx/sy are the ground
// plane. Bevy, like v2, is Y-UP. The swap happens exactly once, in `to_world`,
// which emits the layout the mesher wants -- so nothing downstream has to
// remember which convention it is holding. Getting this wrong does not crash;
// it lays the tree on its side.
// ---------------------------------------------------------------------------

use std::path::Path;

/// A parsed model in the file's own Z-up layout, indexed x + y*sx + z*sx*sy.
pub struct VoxModel {
    pub sx: i32,
    pub sy: i32,
    pub sz: i32,
    pub m: Vec<u8>,
    /// Entry i (1-based) is `pal[i - 1]`.
    pub pal: [[u8; 4]; 255],
}

impl VoxModel {
    #[inline]
    pub fn at(&self, x: i32, y: i32, z: i32) -> u8 {
        if x < 0 || y < 0 || z < 0 || x >= self.sx || y >= self.sy || z >= self.sz {
            return 0;
        }
        self.m[(x + y * self.sx + z * self.sx * self.sy) as usize]
    }
}

/// A model in WORLD layout: x and z horizontal, y vertical, indexed
/// x + z*sx + y*sx*sz -- the same indexing the voxel grid uses.
#[derive(Default, Clone)]
pub struct VoxAsset {
    pub sx: i32,
    pub sy: i32, // the height
    pub sz: i32,
    pub a: Vec<u8>,
}

impl VoxAsset {
    #[inline]
    pub fn at(&self, x: i32, y: i32, z: i32) -> u8 {
        if x < 0 || y < 0 || z < 0 || x >= self.sx || y >= self.sy || z >= self.sz {
            return 0;
        }
        self.a[(x + z * self.sx + y * self.sx * self.sz) as usize]
    }
}

/// MagicaVoxel's built-in palette, for files carrying no RGBA chunk. Generated
/// from the documented 6x6x6 ramp rather than pasted as 255 literals.
fn default_palette() -> [[u8; 4]; 255] {
    let mut p = [[0u8; 4]; 255];
    let lv = [255u8, 204, 153, 102, 51, 0];
    let mut i = 0usize;
    for r in 0..6 {
        for g in 0..6 {
            for b in 0..6 {
                if i < 255 {
                    p[i] = [lv[r], lv[g], lv[b], 255];
                    i += 1;
                }
            }
        }
    }
    p
}

#[inline]
fn i32le(b: &[u8], o: usize) -> i32 {
    i32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

// The largest model this reader will accept, in voxels. A corrupt SIZE chunk
// would otherwise ask for an allocation the size of the header claims.
const MAX_VOXELS: i64 = 64 * (1 << 20);

// ---------------------------------------------------------------------------
// Every model in the file.
//
// Chunks are walked rather than assumed to be in a fixed order, and unknown
// ones (nTRN, nSHP, MATL, LAYR and the rest of the scene-graph extensions) are
// skipped by their declared size -- a reader that assumed SIZE and XYZI came
// first would work on these files and break on the next MagicaVoxel export.
//
// A .vox may carry many SIZE/XYZI pairs -- flowers.vox has six, one per flower.
// The RGBA chunk is shared: it appears once, usually AFTER the models, so the
// palette is applied to all of them at the end rather than as it is found.
// ---------------------------------------------------------------------------
pub fn parse_all(raw: &[u8]) -> Result<Vec<VoxModel>, String> {
    if raw.len() <= 8 || &raw[0..4] != b"VOX " {
        return Err("not a .vox file".into());
    }

    let mut pal = default_palette();
    let mut out: Vec<VoxModel> = Vec::new();
    let (mut sx, mut sy, mut sz) = (0i32, 0i32, 0i32);
    let mut have_size = false;

    let mut o = 8usize;
    while o + 12 <= raw.len() {
        let id = &raw[o..o + 4];
        let content = i32le(raw, o + 4).max(0) as usize;
        let children = i32le(raw, o + 8).max(0) as usize;
        let body = o + 12;
        if body + content > raw.len() {
            break;
        }

        if id == b"MAIN" {
            // Descend into MAIN rather than skipping it: its children are the payload.
            o = body + content;
            continue;
        }
        if id == b"SIZE" && content >= 12 {
            sx = i32le(raw, body);
            sy = i32le(raw, body + 4);
            sz = i32le(raw, body + 8);
            have_size = sx > 0
                && sy > 0
                && sz > 0
                && (sx as i64) * (sy as i64) * (sz as i64) <= MAX_VOXELS;
        } else if id == b"XYZI" && content >= 4 && have_size {
            let n = i32le(raw, body).max(0) as usize;
            if content >= 4 + n * 4 {
                let mut m = vec![0u8; (sx * sy * sz) as usize];
                let v = &raw[body + 4..body + 4 + n * 4];
                for q in (0..n * 4).step_by(4) {
                    let (x, y, z) = (v[q] as i32, v[q + 1] as i32, v[q + 2] as i32);
                    // Out-of-range voxels are dropped rather than fatal: a
                    // hand-edited file occasionally carries one past its own
                    // SIZE, and losing it beats refusing the tree.
                    if x < sx && y < sy && z < sz {
                        m[(x + y * sx + z * sx * sy) as usize] = v[q + 3];
                    }
                }
                out.push(VoxModel { sx, sy, sz, m, pal });
            }
            have_size = false; // one XYZI per SIZE
        } else if id == b"RGBA" && content >= 1024 {
            for i in 0..255 {
                let p = body + i * 4;
                pal[i] = [raw[p], raw[p + 1], raw[p + 2], raw[p + 3]];
            }
        }
        o = body + content + children;
    }

    if out.is_empty() {
        return Err("no models in file".into());
    }
    for m in &mut out {
        m.pal = pal;
    }
    Ok(out)
}

pub fn load_all(path: &Path) -> Result<Vec<VoxModel>, String> {
    let raw = std::fs::read(path).map_err(|e| format!("cannot open {}: {e}", path.display()))?;
    parse_all(&raw)
}

/// The first model in a file -- what a single-model asset wants.
pub fn load(path: &Path) -> Result<VoxModel, String> {
    let mut all = load_all(path)?;
    Ok(all.swap_remove(0))
}

/// One model's sub-box, converted to world layout and trimmed to its own
/// bounds. The model's z becomes the world's y, and its y becomes the world's z.
pub fn to_world(mo: &VoxModel, x0: i32, x1: i32) -> VoxAsset {
    let (mut min_x, mut max_x) = (x1, x0 - 1);
    let (mut min_y, mut max_y) = (mo.sy, -1);
    let (mut min_z, mut max_z) = (mo.sz, -1);
    for z in 0..mo.sz {
        for y in 0..mo.sy {
            for x in x0..x1 {
                if mo.at(x, y, z) != 0 {
                    min_x = min_x.min(x);
                    max_x = max_x.max(x);
                    min_y = min_y.min(y);
                    max_y = max_y.max(y);
                    min_z = min_z.min(z);
                    max_z = max_z.max(z);
                }
            }
        }
    }

    let mut out = VoxAsset::default();
    if max_x < min_x {
        return out; // empty slab
    }

    out.sx = max_x - min_x + 1;
    out.sz = max_y - min_y + 1; // model y is world z
    out.sy = max_z - min_z + 1; // model z is world y (the height)
    out.a = vec![0u8; (out.sx * out.sy * out.sz) as usize];

    for z in min_z..=max_z {
        for y in min_y..=max_y {
            for x in min_x..=max_x {
                let v = mo.at(x, y, z);
                if v == 0 {
                    continue;
                }
                let (wx, wz, wy) = (x - min_x, y - min_y, z - min_z);
                out.a[(wx + wz * out.sx + wy * out.sx * out.sz) as usize] = v;
            }
        }
    }
    out
}
