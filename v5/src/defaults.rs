// ---------------------------------------------------------------------------
// defaults.rs -- the settings v5 starts with.
//
// Carried over from v2's core/defaults.h, which was a GENERATED file rewritten
// by the "Bake as default" row of its in-viewer settings menu. There is no bake
// here yet, so these are hand-maintained -- but the format is deliberately the
// same, constants and nothing else, so a bake could write this file again.
//
// The point of it in v2 was that the settings menu and the command line stopped
// being separate universes: fly around, tune the picture until it looks right,
// bake, rebuild, and the thing you tuned is what the engine opens with. A
// header means the defaults are visible in the diff, travel with the branch,
// and cost nothing at runtime -- unlike a config file read at startup, which is
// one more thing that can be stale, missing, or disagree with the flags.
//
// WHAT MOVED. v2's kScale and kMovingDepth were about brute-force accumulation
// -- how many pixels to trace, and how many bounces to drop while the camera
// moved. Neither has a meaning here: Solari traces at one sample per pixel
// whatever happens, and DLSS is what supplies the resolution scaling, through a
// quality mode rather than a fraction.
// ---------------------------------------------------------------------------

/// Window size. DLSS renders below this and upscales back to it, which is the
/// whole point of the pipeline -- and since the frame is geometry-bound rather
/// than pixel-bound here (see VIEW_CHUNKS), asking for more of them is close to
/// free.
///
/// SO WHY NOT ASK FOR MORE? Because a window LARGER THAN THE DESKTOP is a
/// crash, not a big window. Windows clamps an oversized window to the work
/// area, the clamp arrives as a resize, and the resize reconfigures the wgpu
/// surface while the first frames are still building eighty chunks of
/// acceleration structure -- which times out:
///
///     In Surface::configure
///       Failed to wait for GPU to come idle before reconfiguring the Surface
///     ...
///     Surface is not configured for presentation
///
/// It is intermittent, because it is a race between the clamp and the BLAS
/// builds, and it reads as a driver fault rather than as "the window did not
/// fit". 1920x1080 fits any modern desktop; pass --width/--height for more.
pub const WIDTH: u32 = 1920;
pub const HEIGHT: u32 = 1080;

/// Tone-map exposure, as a plain multiplier on the radiance. Turned into an
/// EV100 for Bevy in main.rs.
pub const EXPOSURE: f32 = 1.2500;

pub const FOV: f32 = 50.0;
pub const EYE: f32 = 1.80;
pub const SPEED: f32 = 9.20;

/// 14:27, and a day is twenty minutes at 1x.
pub const TIME_OF_DAY: f32 = 0.6023;
pub const CYCLE_SPEED: f32 = 1.00;

/// Ring radius in chunks of 25.6 m.
///
/// v2 defaulted to 12 -- 625 chunks, ~307 m -- and could afford it because BVH
/// traversal is logarithmic, so render distance was very nearly free there.
///
/// HERE IT IS THE WHOLE COST, and the reason is that Solari is a HYBRID: the
/// lighting is raytraced, but primary visibility comes from a RASTERISED
/// deferred G-buffer, and a rasteriser is linear in triangles submitted. What
/// was logarithmic in v2 is linear in v5. Measured on the 4070 at 1920x1080:
///
///     view 3    49 chunks    92 M tris   45 fps
///     view 4    81 chunks   150 M tris   39 fps
///     view 6   169 chunks   342 M tris   29 fps
///
/// AND THE TREES ARE 93% OF THAT. A chunk's ground surface is about 135k
/// triangles; a single pine is up to 200k, and a chunk carries dozens.
///
/// LOD ON THE PINES WAS BUILT FOR EXACTLY THIS AND THEN REMOVED. Re-voxelising
/// the models at 2x and 4x cut them to 15% and 3% of their triangles and made
/// view 8 affordable -- but a 22.5 m pine fills a third of the frame at 30 m
/// and most of the ring is closer than the distance a 20 cm voxel would need to
/// go unnoticed (about 115 m), so the coarse trees were visible wherever they
/// were worth having. It was tried, looked at, and judged not worth the frames.
/// Distance is bought with `--view` instead, and paid for honestly.
///
/// The mirror image is that RESOLUTION is nearly free -- 2560x1440 and
/// 1920x1080 measure within noise of each other at the same view distance,
/// because DLSS is tracing a fraction of those pixels and the frame is spent on
/// geometry rather than on rays.
pub const VIEW_CHUNKS: i32 = 4;

pub const SEED: u32 = 20_260_904;
pub const TREE_DENSITY: f32 = 0.31;
pub const GRASS_DENSITY: f32 = 0.105;
pub const FLOWER_DENSITY: f32 = 0.45;
pub const ROCK_DENSITY: f32 = 0.010;
pub const TURBIDITY: f32 = 2.8;

/// Where the water sits, in metres. v2's number, kept.
///
/// LAKES ARE RARE AT THIS VALUE, and that is a property of the height field
/// rather than of the water. A basin only cuts where the low gate in
/// `height_m` is wide open, which needs the landform already down around 10 m
/// -- and that needs the rolling, swell and ridge terms all near zero at the
/// same place, which value-noise fbm does not often do. Raise this to see the
/// water: --water 30 floods the valleys of an ordinary hillside.
pub const WATER_LEVEL: f32 = 2.60;

pub const CAM_X: f32 = -6.0;
pub const CAM_Z: f32 = 34.0;
pub const YAW: f32 = 205.0;
pub const PITCH: f32 = 7.0;

pub const PINE_DIR: &str = "C:/voxelbit/game/assets/foilage/pine9";
pub const DECOR_DIR: &str = "C:/voxelbit/game/assets/decoration";

// ---------------------------------------------------------------------------
// The renderer knobs, which v2 had no equivalent of.
// ---------------------------------------------------------------------------

/// Path length for indirect light.
pub const BOUNCES: u32 = 3;

/// Solari's world-cache cell at the lowest LOD, in metres. Its own default is
/// 0.15, which is right for the diorama it was tuned against and saturates the
/// cache in a 200 m wood -- see main.rs.
pub const GI_CELL: f32 = 0.60;
pub const GI_LOD: f32 = 10.0;

/// Solari's spatiotemporal reuse. Off in its own defaults; on here, because
/// reuse is exactly the signal DLSS Ray Reconstruction was trained on.
pub const RESTIR: bool = true;

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

/// Everything the settings menu's "Bake as default" row can write back.
pub struct Baked {
    pub exposure: f32,
    pub fov: f32,
    pub eye: f32,
    pub speed: f32,
    pub time_of_day: f32,
    pub cycle_speed: f32,
    pub view_chunks: i32,
    pub water_level: f32,
    pub bounces: u32,
    pub gi_cell: f32,
    pub restir: bool,
}

/// Rewrite this file with new values, KEEPING EVERY COMMENT IN IT.
///
/// v2's bake regenerated its defaults header wholesale, which meant the whole
/// of that file's prose lived inside the printf that wrote it -- two copies of
/// every explanation, one of which was the real one. Here the existing source
/// is edited instead: each `pub const NAME` line has the text between its `=`
/// and its `;` replaced, and nothing else in the file is touched. The reasoning
/// stays where it was written, and a constant this does not know about is left
/// exactly as it found it rather than silently dropped.
pub fn rewrite_source(existing: &str, b: &Baked) -> String {
    let subs: [(&str, String); 11] = [
        ("EXPOSURE", format!("{:.4}", b.exposure)),
        ("FOV", format!("{:.1}", b.fov)),
        ("EYE", format!("{:.2}", b.eye)),
        ("SPEED", format!("{:.2}", b.speed)),
        ("TIME_OF_DAY", format!("{:.4}", b.time_of_day)),
        ("CYCLE_SPEED", format!("{:.2}", b.cycle_speed)),
        ("VIEW_CHUNKS", format!("{}", b.view_chunks)),
        ("WATER_LEVEL", format!("{:.2}", b.water_level)),
        ("BOUNCES", format!("{}", b.bounces)),
        ("GI_CELL", format!("{:.2}", b.gi_cell)),
        ("RESTIR", format!("{}", b.restir)),
    ];

    existing
        .lines()
        .map(|line| {
            let t = line.trim_start();
            if !t.starts_with("pub const ") {
                return line.to_string();
            }
            // "pub const NAME: TYPE = VALUE;" -- the name is what comes before
            // the colon, so a prefix like GI_CELL and GI_CELL_SOMETHING could
            // not be confused for each other the way a `contains` test would.
            let after = &t["pub const ".len()..];
            let Some(colon) = after.find(':') else {
                return line.to_string();
            };
            let name = after[..colon].trim();
            let Some((_, value)) = subs.iter().find(|(n, _)| *n == name) else {
                return line.to_string();
            };
            let Some(eq) = line.find('=') else {
                return line.to_string();
            };
            format!("{}= {};", &line[..eq], value)
        })
        .collect::<Vec<_>>()
        .join("\n")
        + "\n"
}
