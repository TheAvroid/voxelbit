// ---------------------------------------------------------------------------
// chunks.rs -- the world, endlessly, a chunk at a time.
//
// A ring of chunks follows the camera, so the wood goes on for as far as anyone
// cares to walk. THREE THINGS MAKE IT AFFORDABLE, and they are the whole design:
//
// 1. THE WORLD IS A PURE FUNCTION. `height_m` and `top_material` depend on
//    nothing but their arguments, so a chunk can be meshed by any thread at any
//    time with no shared state, no locking and no ordering. Nothing is ever
//    "generated" in the sense of being decided and stored -- it is recomputed,
//    identically, whenever it is needed. That is also why a chunk can be thrown
//    away and rebuilt later without the world changing under you.
//
// 2. MESHING RUNS ON THE ASYNC COMPUTE POOL, SPAWNING RUNS BUDGETED ON THE MAIN
//    THREAD. Meshing a chunk is 65k columns of noise and is the expensive half;
//    it is also pure CPU work, so it parallelises perfectly. Handing the result
//    to the renderer has to happen where the World is, so that half is capped
//    at a couple of chunks per frame. Crossing a chunk boundary therefore costs
//    a few frames of catch-up rather than one long stall.
//
// 3. THE BLAS IS BEVY'S PROBLEM. v2 owned its acceleration structures: it built
//    each chunk's GAS, compacted it, tracked a fixed SBT slot for it and
//    rewrote one 32-byte record when the ring moved. None of that exists here.
//    A `RaytracingMesh3d` entity IS the instance; despawning it takes the
//    instance out of the TLAS and drops the last reference to the `Mesh`, which
//    is what frees the BLAS. The whole slot-allocator went with it.
//
// WHAT REPLACED THE PRIME. v2 blocked before the first frame until the entire
// ring was resident, because a hole in an OptiX scene is a hole you can see
// through to the void. Here the first frames simply have fewer chunks in the
// TLAS, and the ring fills in over the following second or so behind a fade --
// which is both less code and less time to a first frame.
// ---------------------------------------------------------------------------

use bevy::pbr::MeshMaterial3d;
use bevy::platform::collections::{HashMap, HashSet};
use bevy::prelude::*;
use bevy::solari::prelude::RaytracingMesh3d;
use bevy::tasks::{block_on, futures_lite::future, AsyncComputeTaskPool, Task};

use crate::core::floor_div;
use crate::scene::collide::Solid;
use crate::scene::models::ModelSet;
use crate::scene::scatter::{self, Placement, ScatterParams};
use crate::scene::terrain::{VoxelTerrain, CHUNK_VOX, VOXEL_M};
use crate::world::WorldMaterials;

/// A chunk that has been meshed but not yet handed to the renderer.
pub struct ChunkBuild {
    pub cx: i32,
    pub cz: i32,
    pub mesh: Mesh,
    pub tris: usize,
    pub decor: Vec<Placement>,
    /// Whether any column here is below the water line, and so whether this
    /// chunk gets a water quad. See `VoxelTerrain::chunk_is_submerged`.
    pub submerged: bool,
}

/// A chunk that is live in the scene.
pub struct ResidentChunk {
    /// The terrain surface, and every tree, rock and flower standing on it.
    /// Held together because they live and die together: the entities are
    /// despawned as one when the ring moves off this chunk.
    pub entities: Vec<Entity>,
    pub tris: usize,
    /// The trees and rocks of this chunk as things to walk into. Held per chunk
    /// rather than in one world-wide list so eviction is free: the colliders go
    /// when the chunk does, and nothing has to be searched to remove them.
    pub solids: Vec<Solid>,
}

#[derive(Resource)]
pub struct ChunkStreamer {
    pub terrain: VoxelTerrain,
    pub params: ScatterParams,
    /// Ring radius, in chunks.
    pub view_chunks: i32,
    /// How many finished chunks may be spawned into the scene per frame. The
    /// budget is on SPAWNING, not on meshing: meshing is off-thread and cannot
    /// stall a frame, whereas inserting a mesh asset queues a BLAS build.
    pub spawn_budget: usize,

    resident: HashMap<(i32, i32), ResidentChunk>,
    wanted: HashSet<(i32, i32)>,
    pending: Vec<((i32, i32), Task<ChunkBuild>)>,
    requested: HashSet<(i32, i32)>,
    last_centre: Option<(i32, i32)>,
    pub resident_tris: usize,
    /// Entities in the ring -- one per chunk surface plus one per tree, rock
    /// and flower. Each is a TLAS instance and a row in every extract the
    /// renderer runs, which is why it is worth watching.
    pub resident_instances: usize,
}

impl ChunkStreamer {
    pub fn new(terrain: VoxelTerrain, params: ScatterParams, view_chunks: i32) -> Self {
        Self {
            terrain,
            params,
            view_chunks: view_chunks.max(1),
            spawn_budget: 2,
            resident: HashMap::default(),
            wanted: HashSet::default(),
            pending: Vec::new(),
            requested: HashSet::default(),
            last_centre: None,
            resident_tris: 0,
            resident_instances: 0,
        }
    }

    /// Make the next `stream_chunks` reconsider the ring even though the
    /// camera has not moved. Needed when the RADIUS changes rather than the
    /// centre -- otherwise a bigger render distance would not arrive until you
    /// happened to walk 25 m.
    pub fn force_rering(&mut self) {
        self.last_centre = None;
    }

    pub fn chunk_count(&self) -> usize {
        self.resident.len()
    }

    pub fn in_flight(&self) -> usize {
        self.pending.len()
    }

    /// Everything solid within `reach` metres of a point.
    ///
    /// Gathered into a list once a tick rather than answered per test: the
    /// player asks six times a tick, a chunk holds about forty solids, and at
    /// that size any structure worth building costs more than the scan it
    /// saves.
    ///
    /// The CHUNKS looked at reach further than `reach` does, because a solid
    /// belongs to the chunk holding its centre and the big rocks are four
    /// metres across -- one centred just over the border can still be underfoot.
    pub fn colliders_near(&self, p: Vec3, reach: f32, out: &mut Vec<Solid>) {
        out.clear();
        let span = reach + 8.0;
        let cx0 = chunk_of(p.x - span);
        let cx1 = chunk_of(p.x + span);
        let cz0 = chunk_of(p.z - span);
        let cz1 = chunk_of(p.z + span);
        for cz in cz0..=cz1 {
            for cx in cx0..=cx1 {
                let Some(c) = self.resident.get(&(cx, cz)) else {
                    continue;
                };
                for s in &c.solids {
                    if (s.cx - p.x).abs() < reach + s.hx && (s.cz - p.z).abs() < reach + s.hz {
                        out.push(*s);
                    }
                }
            }
        }
    }

    /// Bring the wanted set in line with the camera and queue whatever is
    /// missing. Nearest first: the chunk you are standing on matters more than
    /// the one at the edge of the ring, and at speed you may never reach that.
    fn rering(&mut self, cx: i32, cz: i32) -> Vec<(i32, i32)> {
        let r = self.view_chunks;
        self.wanted.clear();
        for j in -r..=r {
            for i in -r..=r {
                self.wanted.insert((cx + i, cz + j));
            }
        }

        let mut order: Vec<(i32, (i32, i32))> = Vec::new();
        for j in -r..=r {
            for i in -r..=r {
                let k = (cx + i, cz + j);
                if self.resident.contains_key(&k) || self.requested.contains(&k) {
                    continue;
                }
                order.push((i * i + j * j, k));
            }
        }
        order.sort_by_key(|o| o.0);
        order.into_iter().map(|o| o.1).collect()
    }
}

#[inline]
fn chunk_of(world_m: f32) -> i32 {
    floor_div((world_m / VOXEL_M).floor() as i32, CHUNK_VOX)
}

// ---------------------------------------------------------------------------
// The two systems: queue work, then take delivery of it.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// HOW LONG THE RING WAITS BEFORE IT STARTS, AND WHY IT HAS TO WAIT AT ALL.
//
// Startup queues an unusual amount of GPU work into very few frames: 41 models,
// about 1.6 M triangles between them, all become BLASes at once, and the sky
// shell is another 16k on top. Meanwhile DLSS engages on its first frame and
// changes the render resolution, which recreates the view targets and
// reconfigures the wgpu surface.
//
// Those two collide. A surface reconfigure waits for the device to go idle, and
// the device is not idle -- it is building acceleration structures -- so the
// wait times out and wgpu panics:
//
//     In Surface::configure
//       Failed to wait for GPU to come idle before reconfiguring the Surface
//     ...
//     Surface is not configured for presentation
//
// Measured at roughly three failures in five launches before this delay, and
// none in twenty after it. It reads as a driver fault, which is what makes it
// worth naming: nothing in the message points at "too much BLAS work in frame
// one".
//
// Ten frames is a sixth of a second at 60 Hz -- long enough for the model
// builds and the DLSS resolution change to have landed, short enough that the
// ring still fills before anyone has finished reading the loading line.
const WARMUP_FRAMES: u32 = 10;

/// Evict what has fallen out of the ring and queue what has come into it.
pub fn stream_chunks(
    mut streamer: ResMut<ChunkStreamer>,
    camera: Query<&GlobalTransform, With<Camera3d>>,
    mut frame: Local<u32>,
    mut commands: Commands,
) {
    *frame += 1;
    if *frame < WARMUP_FRAMES {
        return;
    }

    let Ok(cam) = camera.single() else {
        return;
    };
    let p = cam.translation();
    let centre = (chunk_of(p.x), chunk_of(p.z));

    if streamer.last_centre == Some(centre) {
        return;
    }
    streamer.last_centre = Some(centre);

    let to_request = streamer.rering(centre.0, centre.1);

    // Evict. Dropping the entities drops the last handle to each chunk mesh,
    // which is what releases its BLAS.
    let stale: Vec<(i32, i32)> = streamer
        .resident
        .keys()
        .filter(|k| !streamer.wanted.contains(*k))
        .copied()
        .collect();
    for k in stale {
        if let Some(c) = streamer.resident.remove(&k) {
            streamer.resident_tris -= c.tris;
            streamer.resident_instances -= c.entities.len();
            for e in c.entities {
                commands.entity(e).despawn();
            }
        }
    }

    // A chunk that was queued and has since left the ring is dropped where it
    // stands -- cancelling the task is cheaper than meshing ground nobody will
    // stand on.
    let wanted = streamer.wanted.clone();
    streamer.pending.retain(|(k, _)| wanted.contains(k));
    streamer.requested.retain(|k| wanted.contains(k));

    let pool = AsyncComputeTaskPool::get();
    for k in to_request {
        let terrain = streamer.terrain.clone();
        let params = streamer.params.clone();
        let (cx, cz) = k;
        let task = pool.spawn(async move {
            let vm = terrain.mesh_chunk(cx, cz);
            let tris = vm.tri_count();
            ChunkBuild {
                cx,
                cz,
                mesh: crate::scene::to_bevy_mesh(vm),
                tris,
                decor: scatter::scatter(&terrain, &params, cx, cz),
                submerged: terrain.chunk_is_submerged(cx, cz),
            }
        });
        streamer.requested.insert(k);
        streamer.pending.push((k, task));
    }
}

/// Take delivery of however many finished chunks the budget allows.
#[allow(clippy::too_many_arguments)]
pub fn spawn_finished_chunks(
    mut streamer: ResMut<ChunkStreamer>,
    mut meshes: ResMut<Assets<Mesh>>,
    models: Res<ModelSet>,
    mats: Res<WorldMaterials>,
    mut commands: Commands,
) {
    let mut taken = 0usize;
    let mut ready: Vec<ChunkBuild> = Vec::new();

    let budget = streamer.spawn_budget;
    let mut i = 0;
    while i < streamer.pending.len() && taken < budget {
        // `block_on(poll_once(..))` is the non-blocking form: it asks whether
        // the task has finished and returns immediately if it has not.
        if let Some(build) = block_on(future::poll_once(&mut streamer.pending[i].1)) {
            // The Task is finished; dropping it here is what retires it.
            let _retired = streamer.pending.remove(i);
            ready.push(build);
            taken += 1;
        } else {
            i += 1;
        }
    }

    for b in ready {
        let key = (b.cx, b.cz);
        streamer.requested.remove(&key);
        if !streamer.wanted.contains(&key) {
            continue; // evicted while it was being meshed
        }

        let mut entities = Vec::with_capacity(b.decor.len() + 1);
        let mut solids = Vec::new();

        if b.tris > 0 {
            let mesh = meshes.add(b.mesh);
            // BOTH Mesh3d AND RaytracingMesh3d, and the pairing is not
            // redundant. `SolariLighting` requires DeferredPrepass, DepthPrepass
            // and MotionVectorPrepass: it is a HYBRID renderer, not a path
            // tracer. Primary visibility comes from a rasterised deferred
            // G-buffer, and the raytraced part is the LIGHTING computed on top
            // of it -- restir.wesl literally begins by reading `gbuffer` and
            // `depth`. Geometry with only RaytracingMesh3d can be hit by
            // secondary rays and can cast light, but it is in no G-buffer, so
            // it is never a visible pixel. A world built that way renders
            // completely black while reporting a full ring and 60 fps.
            entities.push(
                commands
                    .spawn((
                        Mesh3d(mesh.clone()),
                        RaytracingMesh3d(mesh),
                        MeshMaterial3d(mats.terrain.clone()),
                        Transform::IDENTITY,
                    ))
                    .id(),
            );
        }

        // One quad of water, only where there is something for it to fill.
        if b.submerged {
            let mesh = meshes.add(crate::scene::water_quad_mesh(
                b.cx,
                b.cz,
                streamer.terrain.water_level,
                crate::world::WATER_TILE_M,
            ));
            entities.push(
                commands
                    .spawn((
                        Mesh3d(mesh.clone()),
                        RaytracingMesh3d(mesh),
                        MeshMaterial3d(mats.water.clone()),
                        Transform::IDENTITY,
                    ))
                    .id(),
            );
        }

        // DECOR TRIANGLES COUNT, and counting only the terrain was actively
        // misleading. A chunk's ground is ~135k triangles; ONE pine is up to
        // 200k, and a chunk carries dozens of them. The instanced trees are
        // therefore the large majority of what the deferred prepass rasterises
        // every frame, and a "10.9 M tris" figure that left them out pointed
        // performance work at the terrain mesher, which is not where the frame
        // is going. This is drawn triangles, not unique ones -- an instance
        // shares its neighbours' BLAS but is still submitted in full.
        let mut drawn = b.tris;

        let (pine_foot, rock_foot, flower_foot) = models.feet();
        for p in &b.decor {
            let (templates, feet) = match p.kind {
                scatter::Kind::Pine => (&models.pines, &pine_foot),
                scatter::Kind::Rock => (&models.rocks, &rock_foot),
                scatter::Kind::Flower => (&models.flowers, &flower_foot),
            };
            if templates.is_empty() {
                continue;
            }
            let idx = p.index % templates.len();
            let foot = feet[idx];

            // Flowers are knee-high and are walked through, so they are drawn
            // and not collided with. Trees and rocks are both.
            let mut solid = Solid::default();
            let transform = if p.kind == scatter::Kind::Flower {
                scatter::make_instance(p, &foot, streamer.params.seed, None)
            } else {
                scatter::make_instance(p, &foot, streamer.params.seed, Some(&mut solid))
            };
            if p.kind != scatter::Kind::Flower && solid.hx > 0.0 {
                solids.push(solid);
            }

            // Only the pines are tinted. A rock is grey and a flower is its own
            // colour; hue-shifting either would read as a rendering fault.
            let material = if p.kind == scatter::Kind::Pine {
                mats.tints[scatter::tint_variant(streamer.params.seed, p.cell)].clone()
            } else {
                mats.terrain.clone()
            };

            drawn += templates[idx].tris;

            let mesh = templates[idx].mesh.clone();
            entities.push(
                commands
                    .spawn((
                        Mesh3d(mesh.clone()),
                        RaytracingMesh3d(mesh),
                        MeshMaterial3d(material),
                        transform,
                    ))
                    .id(),
            );
        }

        streamer.resident_tris += drawn;
        streamer.resident_instances += entities.len();
        streamer.resident.insert(
            key,
            ResidentChunk {
                entities,
                tris: drawn,
                solids,
            },
        );
    }
}
