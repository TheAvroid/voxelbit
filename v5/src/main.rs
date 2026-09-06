// ---------------------------------------------------------------------------
// v5 -- an endless voxel pine forest, ray traced by Bevy Solari and denoised by
//       NVIDIA DLSS Ray Reconstruction.
//
//   The world is the same one v2 renders: 10 cm voxels on the grid the
//   pine_1..9 assets are authored on, a height field evaluated as a pure
//   function of position and meshed to its exposed faces only, with nine pines,
//   twenty-six rocks and six flowers instanced across it in quarter turns.
//   Terrain, trees, player, day/night cycle and controls are ports.
//
//   WHAT IS ACTUALLY NEW IS THE RENDERER, and it is a different KIND of
//   renderer rather than a faster one. v2 path traces on OptiX and converges by
//   brute force: a still camera accumulates and looks superb, and a moving one
//   is permanently back to one sample per pixel, so walking through the wood
//   crawls with noise. Every mode of the OptiX denoiser was tried on it and
//   removed. OptiX is a rendering API; that trade is the right one for a frame
//   you are going to wait for, and the wrong one for a frame you are going to
//   walk through.
//
//   Solari makes the opposite trade. ReSTIR direct and indirect lighting at one
//   or two rays per pixel, designed from the start to be reused across frames
//   and across neighbouring pixels rather than averaged over time -- and it
//   writes, as it goes, the depth, normal, roughness and motion-vector guide
//   buffers that DLSS Ray Reconstruction was trained on. DLSS-RR then does the
//   job the OptiX denoiser could not: it is a temporal model that knows it is
//   looking at a ReSTIR signal, and it upscales at the same time, so the tracer
//   runs at a fraction of the output pixels.
//
//   THE ONE PLACE THE PORT COULD NOT BE LITERAL is the sky -- Solari has no
//   environment lighting at all, and sky light is most of the light on a forest
//   floor. See world.rs, which explains why the sun is emissive geometry here
//   rather than a DirectionalLight.
// ---------------------------------------------------------------------------

mod autowalk;
mod core;
mod defaults;
mod hud;
mod input;
mod menu;
mod player;
mod scene;
mod shot;
mod world;

use bevy::anti_alias::contrast_adaptive_sharpening::ContrastAdaptiveSharpening;
use bevy::camera::{CameraMainTextureUsages, Exposure, Projection};
use bevy::diagnostic::FrameTimeDiagnosticsPlugin;
use bevy::post_process::bloom::Bloom;
use bevy::prelude::*;
use bevy::render::render_resource::TextureUsages;
use bevy::solari::prelude::{SolariLighting, SolariPlugins};
use bevy::window::{PresentMode, WindowResolution};

#[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))]
use bevy::anti_alias::dlss::{
    Dlss, DlssProjectId, DlssRayReconstructionFeature, DlssRayReconstructionSupported,
};

use hud::HudState;
use input::{MouseCaptured, NearbySolids};
use menu::SettingsMenu;
use player::{Player, WalkWorld};
use scene::chunks::ChunkStreamer;
use scene::daynight::DayNight;
use scene::palette::Palette;
use scene::scatter::ScatterParams;
use scene::terrain::VoxelTerrain;
use world::SkyState;

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------
#[derive(Resource, Clone)]
struct Options {
    width: u32,
    height: u32,
    exposure: f32,
    fov: f32,
    eye: f32,
    speed: f32,
    view: i32,
    seed: u32,
    tree_density: f32,
    grass: f32,
    flowers: f32,
    rocks: f32,
    turbidity: f32,
    water: f32,
    time_of_day: f32,
    cycle: f32,
    cam_x: f32,
    cam_z: f32,
    yaw: f32,
    pitch: f32,
    pines: String,
    decor: String,
    dlss: bool,
    vsync: bool,
    background: bool,
    shot: Option<String>,
    settle: u32,
    walk_speed: f32,
    restir: bool,
    bounces: u32,
    gi_cell: f32,
    gi_lod: f32,
    open_menu: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            width: defaults::WIDTH,
            height: defaults::HEIGHT,
            exposure: defaults::EXPOSURE,
            fov: defaults::FOV,
            eye: defaults::EYE,
            speed: defaults::SPEED,
            view: defaults::VIEW_CHUNKS,
            seed: defaults::SEED,
            tree_density: defaults::TREE_DENSITY,
            grass: defaults::GRASS_DENSITY,
            flowers: defaults::FLOWER_DENSITY,
            rocks: defaults::ROCK_DENSITY,
            turbidity: defaults::TURBIDITY,
            water: defaults::WATER_LEVEL,
            time_of_day: defaults::TIME_OF_DAY,
            cycle: defaults::CYCLE_SPEED,
            cam_x: defaults::CAM_X,
            cam_z: defaults::CAM_Z,
            yaw: defaults::YAW,
            pitch: defaults::PITCH,
            pines: defaults::PINE_DIR.into(),
            decor: defaults::DECOR_DIR.into(),
            dlss: true,
            vsync: false,
            background: false,
            shot: None,
            // Long enough for the ring to arrive and for DLSS-RR and the
            // ReSTIR reservoirs to have real history behind them. See shot.rs.
            settle: 240,
            walk_speed: 0.0,
            // ReSTIR spatiotemporal reuse. Off in Solari's own defaults; on
            // here, because reuse across frames and across neighbouring pixels
            // is exactly the signal DLSS Ray Reconstruction was trained on.
            restir: defaults::RESTIR,
            bounces: defaults::BOUNCES,
            // SOLARI'S DEFAULTS ARE 0.15 m AND 15.0, AND THEY ARE WRONG FOR
            // THIS WORLD -- not badly chosen, chosen for a different one. They
            // are tuned against the pica-pica diorama, a room a few metres
            // across. Here the resident ring is over two hundred metres wide,
            // and at a 15 cm base cell the world cache saturates: measured at
            // 979k active cells against a hard cap of 2^20, i.e. 93% full, with
            // the per-frame cache passes then dominating the whole frame.
            //
            // Cells go as the CUBE of their size, so 0.6 m is roughly a
            // sixty-fourth of the cell count -- for indirect light that is
            // low-frequency anyway. A forest floor's bounce light has nothing
            // in it at 15 cm that does not survive being averaged over 60.
            gi_cell: defaults::GI_CELL,
            gi_lod: defaults::GI_LOD,
            open_menu: false,
        }
    }
}

fn usage() {
    println!(
        "v5 -- endless voxel pine forest, ray traced on Bevy Solari, denoised by DLSS-RR\n\
         \n\
         \x20 --width N --height N      window size -- must fit the desktop (1920x1080)\n\
         \x20 --view N                  chunks of 25.6 m kept resident, radius   (4)\n\
         \x20 --density F               how thick the wood is, 0..1           (0.31)\n\
         \x20 --grass F                 fraction of grass columns with a strand (0.105)\n\
         \x20 --flowers F               how thick a flower bed is, 0..1       (0.45)\n\
         \x20 --rocks F                 rock density                         (0.010)\n\
         \x20 --seed N                  world seed                       (20260904)\n\
         \x20 --time H                  start hour, 0-24                     (14.45)\n\
         \x20 --cycle F                 day/night speed, negative rewinds        (1)\n\
         \x20 --turbidity F             haze, 2 clear .. 8                     (2.8)\n\
         \x20 --water F                 water line, m -- lakes are rare this low (2.6)\n\
         \x20 --exposure F              tone-map exposure                     (1.25)\n\
         \x20 --fov DEG                 vertical field of view                  (50)\n\
         \x20 --speed F                 walk speed, m/s                        (9.2)\n\
         \x20 --eye F                   eye height, metres -- 18 voxels       (1.80)\n\
         \x20 --cam-x F --cam-z F       where to start                     (-6, 34)\n\
         \x20 --yaw DEG --pitch DEG     which way to face                  (205, 7)\n\
         \x20 --pines DIR --decor DIR   where the .vox assets are\n\
         \x20 --no-dlss                 leave the ReSTIR output raw (to see the difference)\n\
         \x20 --vsync                   cap to the display's refresh\n\
         \x20 --background              open unfocused and minimised\n\
         \x20 --shot PATH               settle, write a png and exit\n\
         \x20 --settle N                frames to settle before --shot         (240)\n\
         \x20 --walk F                  walk forward by itself, m/s -- see the denoiser\n\
         \x20                           under motion, and stream chunks without a keyboard\n\
         \x20 --no-restir               drop Solari's spatiotemporal reuse\n\
         \x20 --bounces N               path length for indirect light            (3)\n\
         \x20 --gi-cell F               world-cache cell size, metres           (0.6)\n\
         \x20 --gi-lod F                how far the cache keeps cells small      (10)\n\
         \x20 --open-menu               start with the settings menu (Y) open\n\
         \x20 --help                    this\n"
    );
}

fn parse(o: &mut Options) -> bool {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    let f = |a: &Vec<String>, i: usize| -> f32 {
        a.get(i + 1).and_then(|s| s.parse().ok()).unwrap_or(0.0)
    };
    while i < args.len() {
        match args[i].as_str() {
            "--help" | "-h" => {
                usage();
                return false;
            }
            "--width" => o.width = f(&args, i).max(64.0) as u32,
            "--height" => o.height = f(&args, i).max(64.0) as u32,
            "--view" => o.view = f(&args, i) as i32,
            "--density" => o.tree_density = f(&args, i),
            "--grass" => o.grass = f(&args, i),
            "--flowers" => o.flowers = f(&args, i),
            "--rocks" => o.rocks = f(&args, i),
            "--seed" => o.seed = f(&args, i) as u32,
            "--time" => {
                let h = f(&args, i) / 24.0;
                o.time_of_day = h - h.floor();
            }
            "--cycle" => o.cycle = f(&args, i),
            "--turbidity" => o.turbidity = f(&args, i),
            "--water" => o.water = f(&args, i),
            "--exposure" => o.exposure = f(&args, i),
            "--fov" => o.fov = f(&args, i),
            "--speed" => o.speed = f(&args, i),
            "--eye" => o.eye = f(&args, i),
            "--cam-x" => o.cam_x = f(&args, i),
            "--cam-z" => o.cam_z = f(&args, i),
            "--yaw" => o.yaw = f(&args, i),
            "--pitch" => o.pitch = f(&args, i),
            "--pines" => o.pines = args.get(i + 1).cloned().unwrap_or_default(),
            "--decor" => o.decor = args.get(i + 1).cloned().unwrap_or_default(),
            "--shot" => o.shot = args.get(i + 1).cloned(),
            "--settle" => o.settle = f(&args, i).max(1.0) as u32,
            "--walk" => o.walk_speed = f(&args, i).max(0.0),
            "--bounces" => o.bounces = f(&args, i).max(1.0) as u32,
            "--gi-cell" => o.gi_cell = f(&args, i).max(0.01),
            "--gi-lod" => o.gi_lod = f(&args, i).max(0.1),
            "--open-menu" => {
                o.open_menu = true;
                i += 1;
                continue;
            }
            "--no-restir" => {
                o.restir = false;
                i += 1;
                continue;
            }
            "--no-dlss" => {
                o.dlss = false;
                i += 1;
                continue;
            }
            "--vsync" => {
                o.vsync = true;
                i += 1;
                continue;
            }
            "--background" => {
                o.background = true;
                i += 1;
                continue;
            }
            other => {
                eprintln!("v5: unknown option {other}");
                usage();
                return false;
            }
        }
        i += 2;
    }
    true
}

// ---------------------------------------------------------------------------
fn main() {
    let mut o = Options::default();
    if !parse(&mut o) {
        return;
    }

    let mut app = App::new();

    // DlssProjectId must be in the world BEFORE DefaultPlugins, because
    // DlssInitPlugin reads it while it is configuring raw Vulkan instance
    // creation -- DLSS has to register instance extensions before the device
    // exists, so there is no later point at which this could be supplied.
    #[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))]
    app.insert_resource(DlssProjectId(bevy::asset::uuid::uuid!(
        "b7a5c81e-3d64-4f2a-9c17-8e4d6f0a2b93"
    )));

    app.add_plugins(
        DefaultPlugins
            .set(WindowPlugin {
                primary_window: Some(Window {
                    title: "v5 -- voxel pine forest, Bevy Solari + DLSS Ray Reconstruction".into(),
                    resolution: WindowResolution::new(o.width, o.height),
                    present_mode: if o.vsync {
                        PresentMode::AutoVsync
                    } else {
                        PresentMode::AutoNoVsync
                    },
                    // Never steal the desktop: an automated run opens
                    // unfocused and invisible, which is what makes it safe to
                    // launch one while somebody is working.
                    focused: !o.background,
                    visible: !o.background,
                    ..default()
                }),
                ..default()
            })
            // The palette strips are looked up by exact texel (palette.rs), so
            // the default sampler has to be NEAREST or a material id would
            // blend into its neighbour.
            .set(ImagePlugin::default_nearest()),
    )
    .add_plugins((
        SolariPlugins,
        FrameTimeDiagnosticsPlugin::default(),
        bevy::render::diagnostic::RenderDiagnosticsPlugin,
    ));

    app.insert_resource(DayNight {
        tday: o.time_of_day,
        cycle_speed: o.cycle,
        ..default()
    })
    .insert_resource(SkyState::default())
    .insert_resource(HudState::default())
    .insert_resource(MouseCaptured(false))
    .insert_resource(NearbySolids::default())
    .insert_resource(SettingsMenu {
        // --open-menu exists so the menu can be screenshotted with --shot;
        // there is no other way to see it without a window.
        open: o.open_menu,
        ..default()
    })
    .add_message::<menu::MenuAdjust>()
    .add_message::<menu::MenuPreset>()
    .insert_resource(o.clone())
    .add_systems(Startup, (setup, hud::spawn_hud, menu::spawn_menu))
    .add_systems(
        Update,
        (
            // The menu takes the keyboard and the wheel before anything else
            // gets a look at them.
            input::menu_input,
            menu::menu_hover,
            menu::apply_menu_adjust,
            input::grab_mouse,
            input::look,
            input::clock_controls,
            autowalk::drive,
            input::walk,
            input::drive_camera,
            world::update_sky,
            world::follow_camera,
            scene::chunks::stream_chunks,
            scene::chunks::spawn_finished_chunks,
            hud::update_hud,
            menu::update_menu,
        )
            // Chained on purpose. The camera has to be where the player put it
            // BEFORE the sky is centred on it and before the ring is measured
            // against it, or both are a frame behind the body -- which on a
            // sprint is a quarter of a metre of sky slide per frame.
            .chain(),
    );

    // --shot is added last, so it runs after the ring and the sky have had
    // their turn in the same frame it decides to grab.
    if o.walk_speed > 0.0 {
        app.insert_resource(autowalk::AutoWalk {
            speed: o.walk_speed,
            travelled: 0.0,
        });
    }

    if let Some(path) = o.shot.clone() {
        app.insert_resource(shot::ShotRequest::new(path, o.settle))
            .add_systems(Update, shot::take_shot);
    }

    app.run();
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
#[allow(clippy::too_many_arguments)]
fn setup(
    mut commands: Commands,
    o: Res<Options>,
    mut meshes: ResMut<Assets<Mesh>>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    mut images: ResMut<Assets<Image>>,
    mut hud: ResMut<HudState>,
    mut sky: ResMut<SkyState>,
    #[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))] dlss_rr_supported: Option<
        Res<DlssRayReconstructionSupported>,
    >,
) {
    let t0 = std::time::Instant::now();

    // The models load FIRST and the ordering is load-bearing: the terrain's
    // grass and soil colours are sampled from the palette the pines bring with
    // them, so a chunk meshed before they arrive would carry the fallback
    // greens for the life of the run.
    let mut palette = Palette::default();
    let models = scene::models::load_all(
        std::path::Path::new(&o.pines),
        std::path::Path::new(&o.decor),
        &mut palette,
        &mut meshes,
    );
    palette.derive_ground_from_trees();

    if models.pines.is_empty() {
        error!(
            "v5: no pine models loaded from {} -- pass --pines. The wood will be bare.",
            o.pines
        );
    }
    info!(
        "v5: {} pines, {} rocks, {} flowers, {} materials, {:.2} M unique tris \
         (largest model {} k) in {:.2} s",
        models.pines.len(),
        models.rocks.len(),
        models.flowers.len(),
        palette.used(),
        models.unique_tris as f64 / 1e6,
        models.heaviest() / 1000,
        t0.elapsed().as_secs_f64()
    );
    if palette.overflowed() > 0 {
        warn!(
            "v5: {} model colours did not fit the 255-entry palette",
            palette.overflowed()
        );
    }

    let mats = world::build_materials(&palette, &mut materials, &mut images);
    world::spawn_sky(&mut commands, &mut meshes, &mats);

    sky.sky.turbidity = o.turbidity;

    let terrain = VoxelTerrain {
        water_level: o.water,
        grass_density: o.grass.clamp(0.0, 1.0),
        // A stream of its own, so re-seeding the wood does not also reshuffle
        // every blade of grass in it -- the two are independent things to want
        // varied.
        strand_seed: o.seed.wrapping_add(991),
        ..default()
    };

    let (pine_foot, rock_foot, flower_foot) = models.feet();
    let params = ScatterParams {
        seed: o.seed,
        tree_density: o.tree_density.clamp(0.0, 1.0),
        rock_density: o.rocks.clamp(0.0, 1.0),
        flower_density: o.flowers.clamp(0.0, 1.0),
        pine_foot,
        rock_foot,
        flower_foot,
        ..default()
    };

    // The player is placed before any chunk exists, which is fine and
    // deliberate: the ground is a pure function, so its height is known
    // everywhere and the feet can be put on it immediately. Only the TREES are
    // not known yet -- they live in chunks -- so the spawn point can be inside
    // one until the chunk holding it arrives, and `move_axis`'s stuck rule is
    // what walks the body back out if it is.
    let mut player = Player::new(o.yaw, o.pitch, o.eye, o.speed);
    let empty: [scene::collide::Solid; 0] = [];
    player.place_on_ground(
        &WalkWorld {
            terrain: &terrain,
            solids: &empty,
        },
        o.cam_x,
        o.cam_z,
    );

    let streamer = ChunkStreamer::new(terrain, params, o.view);

    let eye = player.eye_position();
    let mut camera = commands.spawn((
        Camera3d::default(),
        Camera {
            clear_color: ClearColorConfig::Custom(Color::BLACK),
            ..default()
        },
        Projection::Perspective(PerspectiveProjection {
            fov: o.fov.to_radians(),
            ..default()
        }),
        Transform::from_translation(eye).looking_at(eye + player.look_dir(), Vec3::Y),
        // Bevy's exposure multiplier is 1 / (1.2 * 2^ev100); v2's --exposure
        // was a plain multiplier on radiance, so this inverts one into the
        // other and the same number means the same thing in both engines.
        Exposure {
            ev100: -(1.2 * o.exposure.max(1e-3)).log2(),
        },
        // Both are required by Solari: it writes the lit image into the main
        // texture with a compute shader, and MSAA has no meaning for an image
        // that was never rasterised.
        CameraMainTextureUsages::default().with(TextureUsages::STORAGE_BINDING),
        Msaa::Off,
        SolariLighting {
            restir: o.restir,
            max_bounces: o.bounces,
            world_cache_position_base_cell_size: o.gi_cell,
            world_cache_position_lod_scale: o.gi_lod,
            // 50 m is Solari's default for the GI ray distance and it happens
            // to suit a wood: past that, indirect light is arriving through so
            // much canopy that it is really the sky term, which the dome
            // already supplies directly.
            ..default()
        },
        // The sun disk is a real light with a real radiance, so looking at it
        // pins the tone mapper. Bloom is what turns that into glare rather than
        // a flat white circle, and it is also what sells a low sun raking
        // through a canopy.
        Bloom {
            intensity: 0.08,
            ..Bloom::NATURAL
        },
    ));

    // Using DLSS Ray Reconstruction for denoising -- and for cheaper rendering
    // via upscaling -- is the entire reason this engine exists. Without it the
    // ReSTIR output is a usable but visibly noisy image; with it the noise is
    // resolved by a model trained on exactly this signal.
    #[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))]
    if o.dlss {
        if dlss_rr_supported.is_some() {
            camera.insert((
                Dlss::<DlssRayReconstructionFeature>::default(),
                // DLSS leaves a slightly soft image by design; a light sharpen
                // after it is the standard pairing, and the one Bevy's own DLSS
                // documentation recommends.
                ContrastAdaptiveSharpening {
                    sharpening_strength: 0.4,
                    ..default()
                },
            ));
            hud.dlss = "DLSS-RR".into();
            info!("v5: DLSS Ray Reconstruction is on");
        } else {
            hud.dlss = "unsupported".into();
            warn!(
                "v5: DLSS Ray Reconstruction is not available here (needs an RTX GPU on the \
                 Vulkan backend) -- showing the raw ReSTIR output instead"
            );
        }
    } else {
        hud.dlss = "off (--no-dlss)".into();
    }

    #[cfg(not(all(feature = "dlss", not(feature = "force_disable_dlss"))))]
    {
        let _ = &mut camera;
        hud.dlss = "not compiled in".into();
    }

    commands.insert_resource(player);
    commands.insert_resource(streamer);
    commands.insert_resource(models);
    commands.insert_resource(mats);

    hud.print_help();
}
