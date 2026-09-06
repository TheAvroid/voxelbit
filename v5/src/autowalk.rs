// ---------------------------------------------------------------------------
// autowalk.rs -- drive the player forward on a fixed path.
//
// Ported in spirit from v2's `--walk`, which existed to make the one question a
// denoiser has to answer MEASURABLE. The noise that matters is the noise WHILE
// MOVING, and comparing two hand-flown screenshots compares two different views
// as much as two denoiser settings. With a scripted walk the camera path is
// identical between runs, so the difference is the denoiser and nothing else.
//
// It earns its place twice over here, because it is also the only way to
// exercise the half of the chunk streamer that startup never touches: crossing
// a chunk boundary, which evicts a ring of chunks and queues a new one. A world
// that builds correctly at spawn and comes apart forty metres later would
// otherwise look fine in every screenshot ever taken of it.
// ---------------------------------------------------------------------------

use bevy::prelude::*;

use crate::player::Player;

#[derive(Resource)]
pub struct AutoWalk {
    /// Metres per second along the player's current heading. The heading is not
    /// touched, so `--yaw` still chooses the direction.
    pub speed: f32,
    /// How far it has gone, for the overlay.
    pub travelled: f32,
}

/// Runs BEFORE `input::walk`, and feeds that system rather than moving the body
/// itself. Everything the player has -- gravity, step-up, trunks being solid,
/// the head bob -- therefore applies to a scripted walk exactly as it does to a
/// held W key, which is the point: this is meant to reproduce walking, not to
/// slide a camera through the world.
///
/// `Option<ResMut<AutoWalk>>`, NOT `ResMut<AutoWalk>`. The resource is only
/// inserted when --walk is passed, but the system is in the ordered chain
/// unconditionally so that it always runs immediately before `input::walk`.
/// A bare `ResMut` of a resource that does not exist is not a no-op in Bevy --
/// it fails system-parameter validation and takes the whole app down with
/// "Resource does not exist", on every launch that did NOT pass --walk. Which
/// is to say: the flag worked, and everything else stopped working.
pub fn drive(
    time: Res<Time>,
    auto: Option<ResMut<AutoWalk>>,
    mut player: ResMut<Player>,
) {
    let Some(mut auto) = auto else {
        return;
    };
    let dt = time.delta_secs().min(0.1);
    auto.travelled += player.speed() * dt;
    // `held_forward` is consumed and cleared by input::walk each tick, so
    // nothing keeps walking if this system stops setting it.
    player.held_forward = auto.speed > 0.0;
}
