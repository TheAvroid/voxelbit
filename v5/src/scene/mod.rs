// ---------------------------------------------------------------------------
// scene -- the world: what it is made of, where it is, and how it reaches the
// renderer.
// ---------------------------------------------------------------------------

pub mod chunks;
pub mod collide;
pub mod daynight;
pub mod models;
pub mod palette;
pub mod scatter;
pub mod sky;
pub mod terrain;
pub mod vox;

use bevy::asset::RenderAssetUsages;
use bevy::mesh::{Indices, Mesh, PrimitiveTopology};

use terrain::VoxMesh;

/// Hand a meshed voxel surface to Bevy.
///
/// The four attributes are exactly what Solari's BLAS builder wants, and it
/// wants all four: POSITION, NORMAL, UV_0 and TANGENT, with U32 indices. A mesh
/// missing any of them is silently dropped from the raytracing scene, which
/// looks like a hole in the world rather than an error.
///
/// The TANGENT is filled with a constant rather than generated. Bevy's
/// `generate_tangents` derives one from the UV parameterisation -- and this
/// mesh has no UV parameterisation at all: every vertex of a quad carries the
/// same palette coordinate (see palette.rs), so the UV gradient is zero and
/// mikktspace has nothing to work from. It would fail, or produce garbage. The
/// tangent is only ever used to build a TBN for a normal map, and nothing in
/// this world has one, so a constant is not an approximation -- it is unread.
/// The water surface over one chunk: one quad at the water line, with real UVs
/// so the ripple normal map tiles against the WORLD rather than against the
/// quad. That distinction matters because these quads are per chunk -- UVs that
/// started at 0 in each quad's own corner would restart the wave pattern at
/// every chunk boundary and draw a grid across the lake.
///
/// The tangent is `(1, 0, 0, -1)`: with the normal at +Y and MikkTSpace's
/// `B = w * cross(N, T)`, w = -1 puts the bitangent along +Z, which is the
/// direction v increases in. Get the sign wrong and the ripple is mirrored --
/// invisible on a symmetric pattern, and wrong on this one.
pub fn water_quad_mesh(cx: i32, cz: i32, y: f32, tile_m: f32) -> Mesh {
    let s = terrain::CHUNK_M;
    let x0 = cx as f32 * s;
    let z0 = cz as f32 * s;
    let x1 = x0 + s;
    let z1 = z0 + s;

    let uv = |x: f32, z: f32| [x / tile_m, z / tile_m];
    Mesh::new(
        PrimitiveTopology::TriangleList,
        RenderAssetUsages::default(),
    )
    .with_inserted_attribute(
        Mesh::ATTRIBUTE_POSITION,
        vec![
            [x0, y, z0],
            [x0, y, z1],
            [x1, y, z1],
            [x1, y, z0],
        ],
    )
    .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, vec![[0.0f32, 1.0, 0.0]; 4])
    .with_inserted_attribute(
        Mesh::ATTRIBUTE_UV_0,
        vec![uv(x0, z0), uv(x0, z1), uv(x1, z1), uv(x1, z0)],
    )
    .with_inserted_attribute(Mesh::ATTRIBUTE_TANGENT, vec![[1.0f32, 0.0, 0.0, -1.0]; 4])
    .with_inserted_indices(Indices::U32(vec![0, 1, 2, 0, 2, 3]))
}

pub fn to_bevy_mesh(vm: VoxMesh) -> Mesh {
    let n = vm.position.len();
    Mesh::new(
        PrimitiveTopology::TriangleList,
        RenderAssetUsages::default(),
    )
    .with_inserted_attribute(Mesh::ATTRIBUTE_POSITION, vm.position)
    .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, vm.normal)
    .with_inserted_attribute(Mesh::ATTRIBUTE_UV_0, vm.uv)
    .with_inserted_attribute(Mesh::ATTRIBUTE_TANGENT, vec![[1.0f32, 0.0, 0.0, 1.0]; n])
    .with_inserted_indices(Indices::U32(vm.index))
}
