// ---------------------------------------------------------------------------
// terrain.rs -- the world as 10 cm voxels, meshed into faces.
//
// The terrain is quantised to the same 10 cm grid the pine models are authored
// on, so a trunk sits in ground made of the lattice the trunk is made of. A
// smooth landscape under a voxel tree reads as two different games.
//
// WHY FACES AND NOT BOXES: only the surface is extracted -- quads with nothing
// in front of them. Interior voxels never become geometry at all, which is what
// keeps a chunk of sixty-five thousand columns tractable as a BLAS.
//
// WHAT CHANGED FROM v2. v2 packed a material id and a face direction into one
// uint16 per triangle and read it back in the closest-hit program. Solari has
// no such hook, so the material rides in the UV (see palette.rs) and the face
// direction becomes an actual per-vertex NORMAL. That is not a loss: the faces
// were axis-aligned when they were emitted and are axis-aligned still, so the
// normal is exact by construction rather than reconstructed from a cross
// product -- which is the same argument v2 made for storing the direction, just
// spent on the attribute Bevy already has a slot for.
// ---------------------------------------------------------------------------

use crate::core::{fbm, hash_u32, hash_unit, ridged, saturate, sstep, warped_fbm};
use crate::scene::palette as mat;

/// The edge of a voxel, in metres. This is the number the pine assets are
/// authored against -- they come out 22.5 m tall at this scale -- and every
/// other length in the world follows from it.
pub const VOXEL_M: f32 = 0.1;

// One chunk is this many voxel columns on a side. 256 columns is 25.6 m, which
// is the balance the numbers push you to: big enough that a chunk's build
// amortises the fixed cost of an acceleration structure, small enough that
// meshing one is a few tens of milliseconds and the ring around the camera can
// be extended a chunk at a time without a visible hitch.
pub const CHUNK_VOX: i32 = 256;
pub const CHUNK_M: f32 = CHUNK_VOX as f32 * VOXEL_M;

// ---------------------------------------------------------------------------
// A meshed voxel surface.
//
// Positions, normals and UVs are separate arrays because that is the layout
// `bevy_mesh::Mesh` wants; the UV is a palette lookup (palette.rs) and carries
// no surface parameterisation at all, which is why it is the same for all four
// corners of a quad.
// ---------------------------------------------------------------------------
#[derive(Default)]
pub struct VoxMesh {
    pub position: Vec<[f32; 3]>,
    pub normal: Vec<[f32; 3]>,
    pub uv: Vec<[f32; 2]>,
    pub index: Vec<u32>,
}

/// Face directions, as the normals they expand to. Emitting the normal here
/// rather than an index means nothing downstream has to hold the table.
pub const POS_Y: [f32; 3] = [0.0, 1.0, 0.0];
pub const NEG_Y: [f32; 3] = [0.0, -1.0, 0.0];
pub const POS_X: [f32; 3] = [1.0, 0.0, 0.0];
pub const NEG_X: [f32; 3] = [-1.0, 0.0, 0.0];
pub const POS_Z: [f32; 3] = [0.0, 0.0, 1.0];
pub const NEG_Z: [f32; 3] = [0.0, 0.0, -1.0];

impl VoxMesh {
    pub fn tri_count(&self) -> usize {
        self.index.len() / 3
    }

    pub fn is_empty(&self) -> bool {
        self.index.is_empty()
    }

    #[allow(clippy::too_many_arguments)]
    pub fn add_quad(
        &mut self,
        a: [f32; 3],
        b: [f32; 3],
        c: [f32; 3],
        d: [f32; 3],
        m: u8,
        n: [f32; 3],
    ) {
        let base = self.position.len() as u32;
        self.position.extend_from_slice(&[a, b, c, d]);
        self.normal.extend_from_slice(&[n, n, n, n]);
        let uv = mat::uv_for(m);
        self.uv.extend_from_slice(&[uv, uv, uv, uv]);
        self.index
            .extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
    }
}

// ---------------------------------------------------------------------------
// The terrain
//
// A heightfield, so the world is a pure function of (i, j) and never has to be
// stored: one integer height and one surface material per column. That purity
// is what lets a chunk be meshed on any thread at any time with no shared
// state, and what lets a chunk be thrown away and rebuilt later without the
// world changing under you.
// ---------------------------------------------------------------------------
#[derive(Clone)]
pub struct VoxelTerrain {
    pub water_level: f32, // metres
    pub grass_density: f32,
    pub grass_min_rows: i32,
    pub grass_max_rows: i32,
    pub strand_seed: u32,
}

impl Default for VoxelTerrain {
    fn default() -> Self {
        Self {
            water_level: 2.6,
            // DENSITY IS WHAT MAKES IT READ AS GRASS. A strand is one voxel
            // across because that is the smallest thing the lattice can
            // express, so at a tenth coverage they stand isolated and every one
            // reads as a fence post. At this density the blades no longer close
            // into a mass, which is the point: the ground colour shows between
            // them and they read as scattered tufts on grass rather than as the
            // grass itself.
            grass_density: 0.105,
            grass_min_rows: 3,
            grass_max_rows: 6,
            strand_seed: 20_260_904,
        }
    }
}

impl VoxelTerrain {
    /// WORLD COLUMN INDICES, not patch-relative ones. Column I sits at
    /// I * VOXEL_M, for any I in either direction, and a chunk is a range of
    /// those -- which is most of what makes the height field chunkable at all.
    #[inline]
    pub fn wx(i: i32) -> f32 {
        i as f32 * VOXEL_M
    }

    // -----------------------------------------------------------------------
    // The continuous landform, before quantisation.
    //
    // ROUNDED, AND FOUR TIMES THE RELIEF this field started with. The
    // amplitudes below and the two gates under them are the only numbers that
    // move: the FREQUENCIES are deliberately untouched, so the hills keep the
    // width they had and gain height, which is what makes a rolling wood read
    // as a mountain one. Doubling the frequency instead would have given twice
    // as many hills of the same shape.
    //
    // Every slope therefore doubles as well, and two thresholds downstream are
    // measured in slope: ROCK_SLOPE and TREE_SLOPE. They move with it, or the
    // same hillside that held soil and pines yesterday is bare rock today. That
    // coupling is the whole reason those constants live next to this function.
    //
    // The dominant term is a low-octave warped fbm, which gives broad domes,
    // plus an even lower-frequency swell underneath it for the large forms. The
    // ridged multifractal survives at a fifth of its old weight purely so the
    // landscape is not all one shape -- 1 - |2n-1| is a fold by construction,
    // which is the opposite of rounded.
    // -----------------------------------------------------------------------
    pub fn height_m(&self, x: f32, z: f32) -> f32 {
        let roll = warped_fbm(x * 0.0130, z * 0.0130, 1.5, 5);
        let swell = fbm(x * 0.0070 + 71.3, z * 0.0070 + 29.7, 3);
        let ridge = ridged(x * 0.0300 + 13.1, z * 0.0300 + 7.3, 3);

        let mut h = 4.0 + roll * 60.0 + swell * 24.0 + ridge * 6.0;

        let b = fbm(x * 0.0160 + 311.7, z * 0.0160 + 157.3, 4);
        if b < 0.40 {
            let m = sstep(1.0f32.min((0.40 - b) / 0.10));
            // Scaled with the terrain, every time it grows. This gate is a
            // FRACTION of the landform's range dressed up as metres. Leave it
            // behind and it sits below almost every column, no basin ever cuts,
            // and the water plane ends up buried under the whole world with not
            // a lake anywhere.
            let low_gate = saturate((36.0 - h) / 28.0);
            h -= m * low_gate * (h - (self.water_level - 3.2));
        }

        // AND THE FINE OCTAVE HAS TO STAY. At 10 cm voxels a slope quantises
        // into steps whose WIDTH is the voxel size over the gradient, so a field
        // that is smooth everywhere terraces into wide flat plateaus -- which
        // reads as worse, not rounder. Roundness belongs in the large shapes;
        // the small ones have to keep enough gradient to break the steps up.
        h += (fbm(x * 0.090 + 3.7, z * 0.090 + 9.1, 3) - 0.5) * 1.2;
        h
    }

    /// Column height in VOXELS -- the one place the world is quantised.
    #[inline]
    pub fn height_vox(&self, i: i32, j: i32) -> i32 {
        (self.height_m(Self::wx(i), Self::wx(j)) / VOXEL_M).floor() as i32
    }

    // Slope, in voxels of drop across two columns. These are the numbers that
    // decide what the wood LOOKS like, far more than the amplitudes do: they
    // are the line between a forested hill and a scree slope, and they have to
    // be kept in step with height_m by hand.
    pub const ROCK_SLOPE: i32 = 36;
    pub const TREE_SLOPE: i32 = 30;

    pub fn stand_density(&self, x: f32, z: f32) -> f32 {
        fbm(x * 0.0165 + 71.3, z * 0.0165 + 44.1, 3)
    }

    // -----------------------------------------------------------------------
    // Which material shows on top of a column.
    //
    // THE SURFACE IS A BAND, NOT A SKIN: a single coloured top voxel reads as
    // paint on stone the moment the camera nears a slope, because a steep
    // column shows its SIDE rather than its top. The soil band underneath is
    // what makes a cut bank look like earth.
    //
    // THE SLOPE IS PASSED IN, not measured here, and that is the single largest
    // saving in the whole generator. height_m is roughly twenty-three octaves
    // of value noise; differencing four neighbours to get a slope would cost
    // five height evaluations per column instead of one. The mesher has already
    // computed every one of those heights into a grid, so handing the slope
    // over turns 5x the noise work into 1x.
    // -----------------------------------------------------------------------
    pub fn top_material(&self, i: i32, j: i32, h: i32, slope: i32) -> u8 {
        let wl = (self.water_level / VOXEL_M) as i32;
        if h <= wl {
            return if wl - h <= 8 { mat::SAND } else { mat::SILT };
        }
        if h <= wl + 8 {
            return mat::SAND; // the shore band
        }
        if slope >= Self::ROCK_SLOPE {
            return mat::ROCK; // too steep to hold soil
        }

        let x = Self::wx(i);
        let z = Self::wx(j);
        // Litter follows the canopy, and the canopy follows the same density
        // field the trees are planted from -- so the floor browns where the
        // stand is thick, without storing a mask.
        if self.stand_density(x, z) > 0.44 && fbm(x * 3.1 + 63.0, z * 3.1 + 88.0, 2) > 0.36 {
            return mat::NEEDLE_LITTER;
        }

        // WHICH grass or WHICH soil comes from a field of its own, at a few
        // metres across. Picking per column from a hash would give confetti --
        // the variants have to form patches or they average back to the single
        // flat colour they were brought in to replace.
        let patch = fbm(x * 0.30 + 117.3, z * 0.30 + 241.1, 3);
        if fbm(x * 0.55 + 31.7, z * 0.55 + 17.2, 4) > 0.46 {
            let k = ((patch * mat::GRASS_COUNT as f32) as i32).min(mat::GRASS_COUNT as i32 - 1);
            return mat::GRASS_0 + k as u8;
        }
        let k = ((patch * mat::SOIL_COUNT as f32) as i32).min(mat::SOIL_COUNT as i32 - 1);
        mat::SOIL_0 + k as u8
    }

    /// The sparse form, for the scatter paths that have no height grid to read
    /// a slope from. Below the shore band the slope is never consulted, so it
    /// is not worth four height evaluations to compute one that is discarded.
    pub fn top_material_at(&self, i: i32, j: i32, h: i32) -> u8 {
        let wl = (self.water_level / VOXEL_M) as i32;
        if h <= wl + 8 {
            return self.top_material(i, j, h, 0);
        }
        let slope = (self.height_vox(i + 1, j) - self.height_vox(i - 1, j))
            .abs()
            .max((self.height_vox(i, j + 1) - self.height_vox(i, j - 1)).abs());
        self.top_material(i, j, h, slope)
    }

    // -----------------------------------------------------------------------
    // One chunk, meshed.
    //
    // The grid is padded so a face on the chunk boundary can ask its neighbour
    // how tall it is. Without the pad, every chunk edge would emit the full
    // side of its own columns and the seams would show as walls -- and because
    // both chunks would do it, the geometry would be doubled there too.
    //
    // TWO rings of padding on the heights, one on everything else: the material
    // at a column one outside the chunk needs that column's slope, and a slope
    // reaches one further again.
    // -----------------------------------------------------------------------
    /// Is any column of this chunk below the water line?
    ///
    /// WATER IS PER CHUNK, not one big plane. v2 built a single 8 km quad,
    /// which it could afford because its ring reached 307 m and the quad's edge
    /// was always over the horizon. v5's ring is 102 m, so the same quad would
    /// read as an ocean surrounding a small island of world -- every direction
    /// you looked past the treeline would be open water.
    ///
    /// A chunk that has a submerged column gets one quad across the whole
    /// chunk, and nothing else does. The quad covers dry ground in that chunk
    /// too, but dry ground is by definition ABOVE the water line, so the quad
    /// is buried under it and never seen. Two triangles, no edge anywhere the
    /// terrain does not already hide.
    pub fn chunk_is_submerged(&self, cx: i32, cz: i32) -> bool {
        let wl = (self.water_level / VOXEL_M).floor() as i32;
        let i0 = cx * CHUNK_VOX;
        let j0 = cz * CHUNK_VOX;
        // Every fourth column: the basins this tests for are tens of metres
        // across, so a 40 cm sample grid cannot miss one, and it is a
        // sixteenth of the noise evaluations.
        let mut j = 0;
        while j < CHUNK_VOX {
            let mut i = 0;
            while i < CHUNK_VOX {
                if self.height_vox(i0 + i, j0 + j) <= wl {
                    return true;
                }
                i += 4;
            }
            j += 4;
        }
        false
    }

    pub fn mesh_chunk(&self, cx: i32, cz: i32) -> VoxMesh {
        let mut m = VoxMesh::default();
        let n = CHUNK_VOX;
        // Measured at roughly 1.5 quads per column across this terrain; two is
        // a comfortable margin. Growing these by doubling instead copies tens
        // of megabytes per chunk, which is meshing time spent on memcpy.
        let cap = (n * n) as usize;
        m.position.reserve(cap * 8);
        m.normal.reserve(cap * 8);
        m.uv.reserve(cap * 8);
        m.index.reserve(cap * 12);

        let i0 = cx * CHUNK_VOX;
        let j0 = cz * CHUNK_VOX;
        let s = VOXEL_M;

        let hstride = (n + 4) as usize;
        let mut h = vec![0i32; hstride * hstride];
        for j in -2..=n + 1 {
            for i in -2..=n + 1 {
                h[(j + 2) as usize * hstride + (i + 2) as usize] = self.height_vox(i0 + i, j0 + j);
            }
        }
        let hh = |i: i32, j: i32| -> i32 { h[(j + 2) as usize * hstride + (i + 2) as usize] };

        let tstride = (n + 2) as usize;
        let mut top = vec![0u8; tstride * tstride];
        for j in -1..=n {
            for i in -1..=n {
                let slope = (hh(i + 1, j) - hh(i - 1, j))
                    .abs()
                    .max((hh(i, j + 1) - hh(i, j - 1)).abs());
                top[(j + 1) as usize * tstride + (i + 1) as usize] =
                    self.top_material(i0 + i, j0 + j, hh(i, j), slope);
            }
        }
        let tt = |i: i32, j: i32| -> u8 { top[(j + 1) as usize * tstride + (i + 1) as usize] };

        // How tall a strand stands on each column, 0 for none. Computed for the
        // padded grid so a column on the edge can still ask its neighbours.
        let mut sr = vec![0u8; tstride * tstride];
        for j in -1..=n {
            for i in -1..=n {
                if !mat::is_grass(tt(i, j)) {
                    continue;
                }
                // Hashed on the WORLD column, so a strand is in the same place
                // no matter which chunk happens to be meshing it -- otherwise
                // the grass would reshuffle every time a chunk was rebuilt.
                let cell = hash_u32((i0 + i) as u32, (j0 + j) as u32);
                if hash_unit(self.strand_seed, cell) >= self.grass_density {
                    continue;
                }
                let span = (self.grass_max_rows - self.grass_min_rows + 1).max(1);
                sr[(j + 1) as usize * tstride + (i + 1) as usize] = (self.grass_min_rows
                    + ((hash_unit(self.strand_seed + 1, cell) * span as f32) as i32).min(span - 1))
                    as u8;
            }
        }
        let srr = |i: i32, j: i32| -> i32 { sr[(j + 1) as usize * tstride + (i + 1) as usize] as i32 };

        // -------------------------------------------------------------------
        // TOP FACES, MERGED ALONG X.
        //
        // One quad per column is the obvious way to do this: 65 536 quads per
        // chunk whatever the ground looked like. But a top face only needs to
        // be its own quad where something CHANGES -- a step in height or a
        // change of material. Everywhere else a run of columns is one flat
        // rectangle, and the rounder the terrain got the longer those runs
        // became.
        //
        // Merged only along X, not into rectangles. Full 2D greedy meshing
        // would do better again, but it needs a visited mask and a second pass,
        // and one dimension already takes most of what there is to take.
        // -------------------------------------------------------------------
        for j in 0..n {
            let mut i = 0;
            while i < n {
                let hc = hh(i, j);
                let tm = tt(i, j);
                let mut k = i + 1;
                while k < n && hh(k, j) == hc && tt(k, j) == tm {
                    k += 1;
                }

                let x0 = (i0 + i) as f32 * s;
                let x1 = (i0 + k) as f32 * s;
                let z0 = (j0 + j) as f32 * s;
                let z1 = z0 + s;
                let y = (hc + 1) as f32 * s;
                m.add_quad(
                    [x0, y, z0],
                    [x0, y, z1],
                    [x1, y, z1],
                    [x1, y, z0],
                    tm,
                    POS_Y,
                );
                i = k;
            }
        }

        // -------------------------------------------------------------------
        // Grass strands: a 1x1 column of voxels standing on the surface.
        //
        // NEIGHBOUR-AWARE, and that is what turns it from a field of fence
        // posts into grass. A strand that emits all four of its sides
        // regardless is drawing the faces buried inside its neighbours. That is
        // not merely wasted geometry (it was about half of it): those interior
        // faces are what make a dense patch read as a bundle of separate posts
        // instead of one continuous mass, because every blade keeps its own
        // hard silhouette.
        //
        // So each side is emitted only over the rows the neighbour does NOT
        // cover. The uncovered part is at most two intervals -- above the
        // neighbour and below it -- which is why this takes a span rather than
        // a flag. No bottom face either: it is standing on the ground.
        // -------------------------------------------------------------------
        for j in 0..n {
            for i in 0..n {
                let rows = srr(i, j);
                if rows == 0 {
                    continue;
                }
                let hc = hh(i, j);
                let tm = tt(i, j);
                let x0 = (i0 + i) as f32 * s;
                let x1 = x0 + s;
                let z0 = (j0 + j) as f32 * s;
                let z1 = z0 + s;

                // Three to six voxels is 30-60 cm -- knee height beside a 22 m
                // pine, which is what keeps it reading as grass rather than a
                // hedge.
                let lo = hc + 1;
                let hi = lo + rows;
                const DI: [i32; 4] = [1, -1, 0, 0];
                const DJ: [i32; 4] = [0, 0, 1, -1];
                for d in 0..4 {
                    let (ni, nj) = (i + DI[d], j + DJ[d]);
                    let nr = srr(ni, nj);
                    // The neighbour's solid span is its strand if it has one,
                    // and in either case the ground it stands on -- which also
                    // hides anything at or below its own top.
                    let ground = hh(ni, nj) + 1;
                    let (nlo, nhi) = if nr > 0 {
                        (ground, ground + nr)
                    } else {
                        (hi, hi)
                    };
                    // Below the neighbour's surface is buried in terrain.
                    emit_uncovered(&mut m, i0 + i, j0 + j, d, lo.max(ground), hi, nlo, nhi, tm);
                }

                let yt = hi as f32 * s;
                m.add_quad(
                    [x0, yt, z0],
                    [x0, yt, z1],
                    [x1, yt, z1],
                    [x1, yt, z0],
                    tm,
                    POS_Y,
                );
            }
        }

        // -------------------------------------------------------------------
        // WALLS, MERGED ALONG THEIR OWN AXIS.
        //
        // Each of the four horizontal directions is walked separately, and for
        // each one the run extends along the axis the wall lies in: an
        // east-facing wall runs north-south, so it merges along z. A run
        // continues while the column height, the neighbour's height and the
        // surface material all hold, because those three are exactly what
        // decide where the material bands split -- if any changes, the quads
        // below would differ and the run has to end.
        //
        // The band structure inside a run: the surface voxel, up to three of
        // soil, then rock down to the neighbour's level.
        // -------------------------------------------------------------------
        const KDI: [i32; 4] = [1, -1, 0, 0];
        const KDJ: [i32; 4] = [0, 0, 1, -1];
        for d in 0..4 {
            let (di, dj) = (KDI[d], KDJ[d]);
            let along_z = d == 0 || d == 1;

            for outer in 0..n {
                let mut inner = 0;
                while inner < n {
                    let (i, j) = if along_z {
                        (outer, inner)
                    } else {
                        (inner, outer)
                    };
                    let hc = hh(i, j);
                    let nb = hh(i + di, j + dj);
                    if hc - nb <= 0 {
                        inner += 1;
                        continue;
                    }
                    let tm = tt(i, j);

                    let mut k = inner + 1;
                    while k < n {
                        let (i2, j2) = if along_z { (outer, k) } else { (k, outer) };
                        if hh(i2, j2) != hc || hh(i2 + di, j2 + dj) != nb || tt(i2, j2) != tm {
                            break;
                        }
                        k += 1;
                    }
                    let run = k - inner;

                    let mut cursor = hc + 1;
                    let surf_lo = (nb + 1).max(hc);
                    side_band(&mut m, i0 + i, j0 + j, d, surf_lo, cursor, tm, run);
                    cursor = surf_lo;
                    if cursor > nb + 1 {
                        if tm != mat::ROCK {
                            let soil_lo = (nb + 1).max(hc - 3);
                            side_band(
                                &mut m,
                                i0 + i,
                                j0 + j,
                                d,
                                soil_lo,
                                cursor,
                                mat::SOIL_0 + 1,
                                run,
                            );
                            cursor = soil_lo;
                        }
                        if cursor > nb + 1 {
                            side_band(&mut m, i0 + i, j0 + j, d, nb + 1, cursor, mat::ROCK, run);
                        }
                    }
                    inner = k;
                }
            }
        }

        m
    }
}

/// A side quad from voxel row `lo` up to row `hi` (exclusive), in one band,
/// spanning `run` columns along the wall's own axis.
///
/// The run is what makes this worth doing. A wall was previously one quad per
/// column per band, so a fifty-metre bank of uniform height emitted five
/// hundred separate quads describing one flat rectangle.
#[allow(clippy::too_many_arguments)]
fn side_band(out: &mut VoxMesh, wi: i32, wj: i32, dir: usize, lo: i32, hi: i32, m: u8, run: i32) {
    if hi <= lo {
        return;
    }
    let s = VOXEL_M;
    let along_z = dir == 0 || dir == 1; // +/-X walls extend in z
    let x0 = wi as f32 * s;
    let x1 = x0 + if along_z { s } else { run as f32 * s };
    let z0 = wj as f32 * s;
    let z1 = z0 + if along_z { run as f32 * s } else { s };
    let y0 = lo as f32 * s;
    let y1 = hi as f32 * s;
    emit_side(out, dir, x0, x1, z0, z1, y0, y1, m);
}

/// One strand's side, one column wide.
#[allow(clippy::too_many_arguments)]
fn side_quad(out: &mut VoxMesh, wi: i32, wj: i32, dir: usize, lo: i32, hi: i32, m: u8) {
    if hi <= lo {
        return;
    }
    let s = VOXEL_M;
    let x0 = wi as f32 * s;
    let z0 = wj as f32 * s;
    emit_side(
        out,
        dir,
        x0,
        x0 + s,
        z0,
        z0 + s,
        lo as f32 * s,
        hi as f32 * s,
        m,
    );
}

// Wound counter-clockwise seen from outside, so the winding and the stored
// normal agree about which way is out.
#[allow(clippy::too_many_arguments)]
fn emit_side(
    out: &mut VoxMesh,
    dir: usize,
    x0: f32,
    x1: f32,
    z0: f32,
    z1: f32,
    y0: f32,
    y1: f32,
    m: u8,
) {
    match dir {
        0 => out.add_quad(
            [x1, y0, z0],
            [x1, y1, z0],
            [x1, y1, z1],
            [x1, y0, z1],
            m,
            POS_X,
        ),
        1 => out.add_quad(
            [x0, y0, z0],
            [x0, y0, z1],
            [x0, y1, z1],
            [x0, y1, z0],
            m,
            NEG_X,
        ),
        2 => out.add_quad(
            [x0, y0, z1],
            [x1, y0, z1],
            [x1, y1, z1],
            [x0, y1, z1],
            m,
            POS_Z,
        ),
        _ => out.add_quad(
            [x0, y0, z0],
            [x0, y1, z0],
            [x1, y1, z0],
            [x1, y0, z0],
            m,
            NEG_Z,
        ),
    }
}

/// The part of [lo, hi) that [nlo, nhi) does not cover, as up to two runs.
#[allow(clippy::too_many_arguments)]
fn emit_uncovered(
    out: &mut VoxMesh,
    wi: i32,
    wj: i32,
    dir: usize,
    lo: i32,
    hi: i32,
    nlo: i32,
    nhi: i32,
    m: u8,
) {
    if nhi <= nlo {
        side_quad(out, wi, wj, dir, lo, hi, m);
        return;
    }
    side_quad(out, wi, wj, dir, lo, hi.min(nlo), m);
    side_quad(out, wi, wj, dir, lo.max(nhi), hi, m);
}

// ---------------------------------------------------------------------------
// Face extraction for a dense grid -- used for the pine, rock and flower models.
//
// The six directions are walked separately and a face is emitted only where the
// neighbour is empty. For a conifer that is most of them: the canopy is nearly
// all surface, which is why a tree of 30k voxels still costs a few hundred
// thousand triangles.
// ---------------------------------------------------------------------------
pub fn mesh_asset(a: &crate::scene::vox::VoxAsset, id_of_entry: &[u8], scale: f32) -> VoxMesh {
    let mut m = VoxMesh::default();
    let s = scale;

    let solid = |x: i32, y: i32, z: i32| -> bool {
        let v = a.at(x, y, z);
        v != 0 && id_of_entry[v as usize] != mat::AIR
    };

    for y in 0..a.sy {
        for z in 0..a.sz {
            for x in 0..a.sx {
                let v = a.at(x, y, z);
                if v == 0 {
                    continue;
                }
                let id = id_of_entry[v as usize];
                if id == mat::AIR {
                    continue;
                }

                let (x0, x1) = (x as f32 * s, x as f32 * s + s);
                let (y0, y1) = (y as f32 * s, y as f32 * s + s);
                let (z0, z1) = (z as f32 * s, z as f32 * s + s);

                if !solid(x, y + 1, z) {
                    m.add_quad(
                        [x0, y1, z0],
                        [x0, y1, z1],
                        [x1, y1, z1],
                        [x1, y1, z0],
                        id,
                        POS_Y,
                    );
                }
                if !solid(x, y - 1, z) {
                    m.add_quad(
                        [x0, y0, z0],
                        [x1, y0, z0],
                        [x1, y0, z1],
                        [x0, y0, z1],
                        id,
                        NEG_Y,
                    );
                }
                if !solid(x + 1, y, z) {
                    m.add_quad(
                        [x1, y0, z0],
                        [x1, y1, z0],
                        [x1, y1, z1],
                        [x1, y0, z1],
                        id,
                        POS_X,
                    );
                }
                if !solid(x - 1, y, z) {
                    m.add_quad(
                        [x0, y0, z0],
                        [x0, y0, z1],
                        [x0, y1, z1],
                        [x0, y1, z0],
                        id,
                        NEG_X,
                    );
                }
                if !solid(x, y, z + 1) {
                    m.add_quad(
                        [x0, y0, z1],
                        [x1, y0, z1],
                        [x1, y1, z1],
                        [x0, y1, z1],
                        id,
                        POS_Z,
                    );
                }
                if !solid(x, y, z - 1) {
                    m.add_quad(
                        [x0, y0, z0],
                        [x0, y1, z0],
                        [x1, y1, z0],
                        [x1, y0, z0],
                        id,
                        NEG_Z,
                    );
                }
            }
        }
    }
    m
}
