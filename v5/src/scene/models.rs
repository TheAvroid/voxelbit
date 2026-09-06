// ---------------------------------------------------------------------------
// models.rs -- the pines, rocks and flowers, loaded once and instanced forever.
//
// THE WHOLE DESIGN QUESTION IS STILL INSTANCING. Nine pine models meshed to
// their exposed faces come to millions of triangles between them; twenty-six
// rocks and six flowers more. A stand flattened would be unbuildable, and every
// tree would still be one of the nine shapes actually authored. One `Mesh`
// asset per model, referred to by many entities, keeps the memory at one copy
// of each while the wood draws thousands -- Bevy builds one BLAS per mesh and
// puts an instance of it in the TLAS per entity, which is exactly the OptiX
// GAS/IAS split v2 built by hand.
//
// The models load BEFORE the world streams, and that ordering is load-bearing:
// the terrain's grass and soil colours are sampled from the palette the models
// bring with them (see palette.rs), so a chunk meshed before they arrive would
// carry the fallback greens forever.
// ---------------------------------------------------------------------------

use bevy::asset::{Assets, Handle};
use bevy::mesh::Mesh;
use std::path::Path;

use crate::scene::collide::{measure_collider, BODY_HEIGHT_M};
use crate::scene::palette::{self as mat, Palette};
use crate::scene::scatter::Footprint;
use crate::scene::terrain::{mesh_asset, VOXEL_M};
use crate::scene::vox;

/// The 26 rocks, each its own file. The names are listed rather than globbed:
/// a fixed list means a missing file is a warning about that file rather than a
/// scene that silently has fewer rocks in it than it should.
const ROCK_NAMES: [&str; 26] = [
    "BIG_1_BiG_0",
    "Big_2_BiG_0",
    "Big_3_BiG_0",
    "Big_4_BiG_0",
    "Big_5_BiG_0",
    "Mid_1_MID_0",
    "Mid_2_MID_0",
    "Mid_3_MID_0",
    "Mid_4_MID_0",
    "Mid_4_MID_0_001",
    "Mid_5_MID_0",
    "Runic_1_Runic_0",
    "Runic_2_Runic_0",
    "Runic_3_Runic_0",
    "Runic_4_Runic_0",
    "Runic_5_Runic_0",
    "Runic_6_Runic_0",
    "Runic_7_Runic_0",
    "Small_1_SMall_0",
    "Small_2_SMall_0",
    "Small_3_SMall_0",
    "Small_4_SMall_0",
    "Small_5_SMall_0",
    "Small_6_SMall_0",
    "Small_7_SMall_0",
    "Small_8_SMall_0",
];

/// One loaded model: the mesh the renderer instances, and the footprint the
/// scatter and the collider are measured from.
pub struct ModelTemplate {
    pub mesh: Handle<Mesh>,
    pub foot: Footprint,
    pub tris: usize,
}

#[derive(Default, bevy::ecs::resource::Resource)]
pub struct ModelSet {
    pub pines: Vec<ModelTemplate>,
    pub rocks: Vec<ModelTemplate>,
    pub flowers: Vec<ModelTemplate>,
    pub unique_tris: usize,
}

impl ModelSet {
    /// The heaviest single model, in triangles. Worth reporting at startup:
    /// every instance of it is one TLAS entry pointing at that one BLAS, so
    /// this number is what a stand of a thousand pines actually costs to build,
    /// and it is the first thing to look at if the ring is slow to fill.
    pub fn heaviest(&self) -> usize {
        self.pines
            .iter()
            .chain(&self.rocks)
            .chain(&self.flowers)
            .map(|t| t.tris)
            .max()
            .unwrap_or(0)
    }

    pub fn feet(&self) -> (Vec<Footprint>, Vec<Footprint>, Vec<Footprint>) {
        (
            self.pines.iter().map(|t| t.foot).collect(),
            self.rocks.iter().map(|t| t.foot).collect(),
            self.flowers.iter().map(|t| t.foot).collect(),
        )
    }
}

/// Load every model, registering its colours in the shared palette as it goes.
pub fn load_all(
    pine_dir: &Path,
    decor_dir: &Path,
    palette: &mut Palette,
    meshes: &mut Assets<Mesh>,
) -> ModelSet {
    let mut set = ModelSet::default();

    let pine_paths: Vec<_> = (1..=9)
        .map(|i| pine_dir.join(format!("pine_{i}.vox")))
        .collect();
    load_set(
        &pine_paths,
        false,
        palette,
        meshes,
        &mut set.pines,
        &mut set.unique_tris,
    );

    let rock_paths: Vec<_> = ROCK_NAMES
        .iter()
        .map(|n| decor_dir.join("rocks").join(format!("{n}.vox")))
        .collect();
    load_set(
        &rock_paths,
        false,
        palette,
        meshes,
        &mut set.rocks,
        &mut set.unique_tris,
    );

    load_set(
        &[decor_dir.join("flowers.vox")],
        true,
        palette,
        meshes,
        &mut set.flowers,
        &mut set.unique_tris,
    );

    set
}

fn load_set(
    paths: &[std::path::PathBuf],
    multi_model: bool,
    palette: &mut Palette,
    meshes: &mut Assets<Mesh>,
    out: &mut Vec<ModelTemplate>,
    unique_tris: &mut usize,
) {
    for path in paths {
        let models = if multi_model {
            match vox::load_all(path) {
                Ok(m) => m,
                Err(e) => {
                    bevy::log::warn!("v5: {e}");
                    continue;
                }
            }
        } else {
            match vox::load(path) {
                Ok(m) => vec![m],
                Err(e) => {
                    bevy::log::warn!("v5: {e}");
                    continue;
                }
            }
        };

        for mo in &models {
            let a = vox::to_world(mo, 0, mo.sx);
            if a.sx <= 0 {
                continue;
            }

            // ONLY THE ENTRIES THE MODEL USES. Registering all 255 of a file's
            // palette floods the shared table, and past 255 `for_model_color`
            // returns AIR and real voxels stop being drawn -- which is exactly
            // what six flower files did in one pass.
            let mut id_of_entry = vec![mat::AIR; 256];
            let mut used = [false; 256];
            for &v in &a.a {
                used[v as usize] = true;
            }
            for e in 1..=255usize {
                if used[e] {
                    id_of_entry[e] = palette.for_model_color(mo.pal[e - 1]);
                }
            }

            let vm = mesh_asset(&a, &id_of_entry, VOXEL_M);
            if vm.is_empty() {
                continue;
            }

            // Measured here and not later: `a` is the only place the model's
            // voxels exist, and it goes out of scope with this loop.
            let col = measure_collider(&a, VOXEL_M, BODY_HEIGHT_M);
            let tris = vm.tri_count();
            *unique_tris += tris;

            out.push(ModelTemplate {
                mesh: meshes.add(crate::scene::to_bevy_mesh(vm)),
                foot: Footprint {
                    sx: a.sx,
                    sz: a.sz,
                    sy: a.sy,
                    col,
                },
                tris,
            });
        }
    }
}
