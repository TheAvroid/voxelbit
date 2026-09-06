// ---------------------------------------------------------------------------
// shot.rs -- render a frame to a png and exit.
//
// v2 had `--out`, which rendered one frame offline at however many samples you
// asked for and wrote it. That flag does not translate: Solari is a real-time
// integrator that carries its estimate in ReSTIR reservoirs and a world cache,
// so there is no "render N samples into this pixel and stop". The equivalent
// here is to let the pipeline SETTLE and then grab the window.
//
// WHAT SETTLING MEANS, and why the wait is measured in frames rather than
// seconds. Three things converge over the first frames and none of them is
// instant:
//
//   the chunk ring       spawned a couple per frame, so the wood arrives over
//                        the first second or two;
//   Solari's reservoirs  temporal reuse needs history before it has anything to
//                        reuse, and the world cache fills over a similar span;
//   DLSS-RR              is a temporal model. Its first frame has no history at
//                        all and looks it.
//
// A shot taken too early is not a slightly noisier version of the right image;
// it is a picture of a half-built wood. So --shot waits, and it also holds the
// clock still while it does, so the sun in the file is the sun that was asked
// for rather than wherever it drifted to during the wait.
// ---------------------------------------------------------------------------

use bevy::diagnostic::{DiagnosticsStore, FrameTimeDiagnosticsPlugin};
use bevy::prelude::*;
use bevy::render::view::screenshot::{save_to_disk, Screenshot};

use crate::scene::chunks::ChunkStreamer;
use crate::scene::daynight::DayNight;

#[derive(Resource)]
pub struct ShotRequest {
    pub path: String,
    /// Frames to let the ring, the reservoirs and the denoiser settle before
    /// the grab.
    pub settle_frames: u32,
    pub frame: u32,
    pub taken: bool,
    /// Frames to keep running after the grab, so the write has a chance to
    /// finish before the app exits.
    pub after: u32,
}

impl ShotRequest {
    pub fn new(path: String, settle_frames: u32) -> Self {
        Self {
            path,
            settle_frames,
            frame: 0,
            taken: false,
            after: 0,
        }
    }
}

pub fn take_shot(
    mut req: ResMut<ShotRequest>,
    streamer: Res<ChunkStreamer>,
    diagnostics: Res<DiagnosticsStore>,
    mut clock: ResMut<DayNight>,
    mut commands: Commands,
    mut exit: MessageWriter<AppExit>,
) {
    // Held rather than merely slowed: a settle of a few hundred frames is a few
    // seconds of wall clock, which at 1x is several minutes of sky.
    clock.paused = true;

    if req.taken {
        req.after += 1;
        if req.after > 30 {
            exit.write(AppExit::Success);
        }
        return;
    }

    req.frame += 1;
    // Both conditions, not either: the frame count alone can elapse while a
    // slow first BLAS build is still queued, and an empty in-flight list is
    // true on frame one before anything has been requested at all.
    let ring_ready = streamer.in_flight() == 0 && streamer.chunk_count() > 0;
    if req.frame < req.settle_frames || !ring_ready {
        return;
    }

    let fps = diagnostics
        .get(&FrameTimeDiagnosticsPlugin::FPS)
        .and_then(|d| d.smoothed())
        .unwrap_or(0.0);
    info!(
        "v5: writing {} after {} frames -- {:.1} fps, {} chunks, {} instances, {:.1} M tris",
        req.path,
        req.frame,
        fps,
        streamer.chunk_count(),
        streamer.resident_instances,
        streamer.resident_tris as f64 / 1e6
    );
    // Every GPU pass Solari and the post chain register, so a slow frame can be
    // attributed rather than guessed at.
    let mut rows: Vec<(String, f64)> = diagnostics
        .iter()
        .filter_map(|d| {
            let v = d.smoothed()?;
            (v > 0.01).then(|| (d.path().as_str().to_string(), v))
        })
        .collect();
    rows.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
    for (name, v) in rows.iter().take(18) {
        info!("  {v:8.3}  {name}");
    }

    let path = req.path.clone();
    commands
        .spawn(Screenshot::primary_window())
        .observe(save_to_disk(path));
    req.taken = true;
}
