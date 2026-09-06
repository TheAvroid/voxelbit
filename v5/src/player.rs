// ---------------------------------------------------------------------------
// player.rs -- a person standing on the ground, rather than a camera in space.
//
// Ported from v2's render/player.h, whose constants came in turn from the JS
// engine's sim/player.js. They convert exactly: that engine measures in VOXELS
// and this one in METRES, and a voxel is 10 cm.
//
//     WALK 46 vox/s   -> 4.6 m/s, since doubled     JUMP 66 vox/s -> 6.6 m/s,
//     SPRINT x1.85                                  since raised -- see JUMP_VEL
//     EYE 18 vox      -> 1.80 m                     GRAVITY 200   -> 20 m/s^2
//
// Gravity is 20 m/s^2, not 9.81. Real gravity makes a jump feel like a moon
// landing -- the arc is right but it takes twice as long, and the hang time
// reads as floating. Doubling it keeps the same jump height at twice the
// cadence, which is what every first-person game does and why they all feel
// crisper than reality.
//
// THE GROUND IS QUERIED, NOT TRACED. The terrain is a height field and a pure
// function of position, so the surface under the player is a direct evaluation
// -- no ray, no acceleration structure, no dependence on which chunks happen to
// be resident. That also means the player can walk into terrain the renderer
// has not built yet without falling through the world. It matters more here
// than it did in v2: Solari has no synchronous "trace one ray" API from a
// gameplay system at all, so a ray-based ground query was never on the table.
//
// THE TREES AND ROCKS ARE SOLID, and without a ray either. Each one is placed
// with a measured collider beside its transform (see collide.rs), and the
// player is handed the handful of them that are nearby. They are NOT solid in
// the same way as each other, because they are not the same kind of obstacle:
//
//   A TRUNK is a wall at every height and never a floor. Its top is a canopy
//   twenty metres up, and a body put on top of that would be standing in the
//   air over a tree.
//
//   A ROCK is a floor with a top, which is all it takes to be a wall as well:
//   the step logic already refuses a surface more than STEP_UP above the feet.
//   That one rule gives a small stone you step onto, a boulder you are stopped
//   by, and a rock you can land on from a jump, without any of the three being
//   written down separately.
// ---------------------------------------------------------------------------

use bevy::prelude::*;
use std::f32::consts::TAU;

use crate::scene::collide::{touches, Solid};
use crate::scene::terrain::{VoxelTerrain, VOXEL_M};

/// The ground, plus whatever decor is close enough to matter.
pub struct WalkWorld<'a> {
    pub terrain: &'a VoxelTerrain,
    pub solids: &'a [Solid],
}

#[derive(Resource)]
pub struct Player {
    /// Feet, in world metres. The camera sits `eye` above this.
    pub pos: Vec3,
    pub vy: f32,
    pub on_ground: bool,
    pub fly: bool,

    pub yaw: f32,
    pub pitch: f32,

    pub walk: f32,
    pub sprint_mul: f32,
    /// The port's was 6.6 (JUMP 66 vox/s), which apexes at 1.09 m. Raised 50%
    /// in HEIGHT -- and height goes as v^2/2g, so that is sqrt(1.5) on the
    /// velocity, not 1.5. 1.09 m -> 1.63 m, while the hang time only lengthens
    /// by 22%, which is the point of taking it this way: 1.5x the velocity
    /// would have been a 2.45 m moon jump.
    pub jump_vel: f32,
    pub gravity: f32,
    pub eye: f32,
    pub half_width: f32,

    /// How far up a step can be climbed without jumping, and how far down the
    /// feet will follow the ground before the player is considered to have
    /// walked off an edge. Both matter on voxel terrain: without a step-up the
    /// player is stopped by every 10 cm lip, and without the step-down a stride
    /// downhill is a series of tiny falls.
    pub step_up: f32,
    pub step_down: f32,
    /// How fast the eye catches up after a step, per second. 18 is about a
    /// 55 ms tail: long enough to remove the jolt, short enough that the view
    /// never feels like it is trailing the body.
    pub step_smooth: f32,

    /// The head bob. `cam_bob_y` is added to the eye, never to `pos` -- physics
    /// and ground contact must not see it.
    pub bob_amp: f32,
    pub bob_phase: f32,
    pub cam_bob_y: f32,

    /// Set by `--walk` to press W without a keyboard. Read and cleared by the
    /// input system every tick, so removing the driver stops the walk rather
    /// than leaving it latched on.
    pub held_forward: bool,

    hvx: f32,
    hvz: f32,
    /// How far the eye is still behind the feet after a step, in metres. Always
    /// decaying toward zero; never read by anything that decides where the body
    /// is.
    step_lag: f32,
}

impl Default for Player {
    fn default() -> Self {
        Self {
            pos: Vec3::ZERO,
            vy: 0.0,
            on_ground: false,
            fly: false,
            yaw: 205.0,
            pitch: 7.0,
            walk: 9.2, // the JS engine's 4.6, doubled
            sprint_mul: 1.85,
            jump_vel: 8.08, // apex 1.63 m
            gravity: 20.0,
            eye: 1.80, // 18 voxels
            half_width: 0.26,
            step_up: 0.62,
            step_down: 0.62,
            step_smooth: 18.0,
            bob_amp: 0.0,
            bob_phase: 0.0,
            cam_bob_y: 0.0,
            held_forward: false,
            hvx: 0.0,
            hvz: 0.0,
            step_lag: 0.0,
        }
    }
}

// THE ONE PLACE THAT DELIBERATELY LEAVES THE PORT BEHIND. The JS engine bobs at
// 0.225 of phase per voxel travelled -- 2.25 per metre -- with a 0.55 voxel
// swing. Those numbers were tuned against ITS walk speed, and this engine
// doubled that speed, which turned the same bob into a fast shallow tremor: the
// right total movement, delivered too quickly to read as anything but vibration.
//
// Half the rate and twice the swing spreads the same motion over twice the
// distance -- a slow deep roll that reads as weight rather than jitter. The two
// go together: halving the rate alone flattens the walk into a drift, and
// doubling the swing alone is seasickness at this cadence.
const BOB_RATE: f32 = 1.125; // radians of phase per metre
const CAM_BOB: f32 = 0.11; // 1.1 voxels, twice the port's
const CAM_BOB_RUN: f32 = 0.65; // extra swing once past a walk

impl Player {
    /// The four things a launch decides. The rest of the struct is either a
    /// tuned constant or internal state, and `hvx`/`hvz`/`step_lag` are private
    /// precisely because nothing outside this file has any business setting
    /// where the body is halfway through a step.
    pub fn new(yaw: f32, pitch: f32, eye: f32, walk: f32) -> Self {
        Self {
            yaw,
            pitch,
            eye,
            walk,
            ..Default::default()
        }
    }

    /// step_lag is carried here and NOT in pos, so the physics still sees the
    /// feet exactly on the ground while the eye is still catching up.
    pub fn eye_position(&self) -> Vec3 {
        Vec3::new(
            self.pos.x,
            self.pos.y + self.eye + self.cam_bob_y + self.step_lag,
            self.pos.z,
        )
    }

    pub fn speed(&self) -> f32 {
        (self.hvx * self.hvx + self.hvz * self.hvz).sqrt()
    }

    /// The direction the head is pointing, from the two angles in degrees.
    pub fn look_dir(&self) -> Vec3 {
        let y = self.yaw.to_radians();
        let p = self.pitch.to_radians();
        Vec3::new(y.cos() * p.cos(), p.sin(), y.sin() * p.cos()).normalize()
    }

    /// Put the body on the ground at (x, z), stepping aside first if that spot
    /// is inside a trunk. The start position is a fixed point on a procedural
    /// map, so it can perfectly well be a tree; being welded into one before
    /// the first frame is drawn is a poor introduction to a wood.
    pub fn place_on_ground(&mut self, w: &WalkWorld, x: f32, z: f32) {
        let (x, z) = self.find_clear(w, x, z);
        self.pos = Vec3::new(x, self.ground_height(w, x, z), z);
        self.vy = 0.0;
        self.on_ground = true;
        self.step_lag = 0.0;
    }

    // -----------------------------------------------------------------------
    // One tick. `move_dir` is the desired horizontal direction, normalised.
    // -----------------------------------------------------------------------
    pub fn update(
        &mut self,
        w: &WalkWorld,
        move_dir: Vec3,
        sprint: bool,
        jump: bool,
        down: bool,
        dt: f32,
    ) {
        if self.fly {
            let spd = self.walk * 3.0 * if sprint { self.sprint_mul } else { 1.0 };
            let k = 1.0 - (-10.0 * dt).exp();
            self.hvx += (move_dir.x * spd - self.hvx) * k;
            self.hvz += (move_dir.z * spd - self.hvz) * k;
            self.pos.x += self.hvx * dt;
            self.pos.z += self.hvz * dt;
            if jump {
                self.pos.y += spd * dt;
            }
            if down {
                self.pos.y -= spd * dt;
            }
            self.vy = 0.0;
            self.on_ground = false;
        } else {
            let spd = self.walk * if sprint { self.sprint_mul } else { 1.0 };
            // Approached exponentially rather than set outright, and far more
            // slowly in the air (3.2 against 14): that difference IS the sense
            // of having weight, and of not being able to change your mind
            // mid-jump.
            let k = 1.0 - (-(if self.on_ground { 14.0 } else { 3.2 }) * dt).exp();
            self.hvx += (move_dir.x * spd - self.hvx) * k;
            self.hvz += (move_dir.z * spd - self.hvz) * k;

            // One axis at a time, so sliding along a wall still works: blocked
            // in x does not have to mean blocked in z.
            self.move_axis(w, 0, self.hvx * dt);
            self.move_axis(w, 2, self.hvz * dt);

            if self.on_ground && jump {
                self.vy = self.jump_vel;
                self.on_ground = false;
            }

            if !self.on_ground {
                self.vy -= self.gravity * dt;
                self.pos.y += self.vy * dt;
                let g = self.ground_height(w, self.pos.x, self.pos.z);
                if self.pos.y <= g && self.vy <= 0.0 {
                    self.pos.y = g;
                    self.vy = 0.0;
                    self.on_ground = true;
                }
            }
        }

        // The eye eases toward the feet rather than being nailed to them.
        // Decayed here, once, so it is frame-rate independent and so a step
        // taken during move_axis has already been folded in.
        self.step_lag *= (-self.step_smooth * dt).exp();
        if self.step_lag.abs() < 1e-4 {
            self.step_lag = 0.0;
        }

        self.update_bob(dt);
    }

    /// The surface the feet rest on: the terrain, and the top of any standable
    /// solid the body is over, whichever is higher.
    ///
    /// The terrain is sampled at the four corners of the body and taken at its
    /// highest -- one sample at the centre lets half the player sink into a
    /// step they are standing against. A rock is not sampled but INTERSECTED,
    /// because a stone can be narrower than the gap between two corners, and a
    /// collider a body can straddle is a collider that flickers.
    pub fn ground_height(&self, w: &WalkWorld, x: f32, z: f32) -> f32 {
        let hw = self.half_width;
        let mut best = -1e9f32;
        for c in 0..4 {
            let cx = x + if c & 1 != 0 { hw } else { -hw };
            let cz = z + if c & 2 != 0 { hw } else { -hw };
            let i = (cx / VOXEL_M).floor() as i32;
            let j = (cz / VOXEL_M).floor() as i32;
            best = best.max((w.terrain.height_vox(i, j) + 1) as f32 * VOXEL_M);
        }
        for s in w.solids {
            if !s.standable || s.top <= best {
                continue;
            }
            if touches(s, x, z, hw) {
                best = s.top;
            }
        }
        best
    }

    /// True if the body at (x, z) is inside something that is a wall at every
    /// height -- a trunk. Rocks are deliberately not here: they are already
    /// walls, by being floors that are too tall to step onto.
    fn blocked(&self, w: &WalkWorld, x: f32, z: f32) -> bool {
        w.solids
            .iter()
            .any(|s| !s.standable && touches(s, x, z, self.half_width))
    }

    /// The nearest spot to (x, z) that is not inside a trunk. In rings outward,
    /// so the answer is the closest one and a spawn moves as little as it must.
    fn find_clear(&self, w: &WalkWorld, x: f32, z: f32) -> (f32, f32) {
        if !self.blocked(w, x, z) {
            return (x, z);
        }
        let mut r = 0.4;
        while r <= 6.0 {
            for a in 0..16 {
                let th = a as f32 * (TAU / 16.0);
                let (cx, cz) = (x + th.cos() * r, z + th.sin() * r);
                if !self.blocked(w, cx, cz) {
                    return (cx, cz);
                }
            }
            r += 0.4;
        }
        (x, z)
    }

    fn move_axis(&mut self, w: &WalkWorld, axis: usize, d: f32) {
        if d == 0.0 {
            return;
        }
        let mut next = self.pos;
        if axis == 0 {
            next.x += d;
        } else {
            next.z += d;
        }

        // A trunk stops a walk and a fall alike, and unlike the ground it is
        // never something to step up onto.
        //
        // The `stuck` half of the test is what keeps a body that is somehow
        // already inside one -- spawned there, or flown into a tree and dropped
        // out of fly mode -- from being welded in place: if standing here is
        // blocked too then moving cannot make it worse, so let it move and walk
        // out.
        let stuck = self.blocked(w, self.pos.x, self.pos.z);
        if !stuck && self.blocked(w, next.x, next.z) {
            return;
        }

        let g = self.ground_height(w, next.x, next.z);

        if self.on_ground {
            let here = self.ground_height(w, self.pos.x, self.pos.z);
            if g > self.pos.y + self.step_up && here <= self.pos.y + self.step_up {
                return; // a wall, not a step
            }
            self.pos = next;
            if g >= self.pos.y - self.step_down {
                // STEP, SMOOTHED IN THE EYE ONLY.
                //
                // The feet snap to the new surface -- they have to, or the body
                // would be standing inside the step and every later query would
                // be answered from the wrong height. What was jagged was that
                // the CAMERA snapped with them: at 10 cm voxels a walk across
                // rolling ground is a stream of instant 10 cm jolts.
                //
                // So the difference is taken out of the eye and paid back over
                // ~55 ms. The feet are exact, the view is continuous, and
                // nothing that reads pos can tell the difference.
                self.step_lag =
                    (self.step_lag + (self.pos.y - g)).clamp(-self.step_down, self.step_up);
                self.pos.y = g;
            } else {
                self.on_ground = false; // walked off a ledge; gravity takes it
            }
        } else {
            // Airborne. A surface above the feet blocks -- but only one you
            // could not have WALKED up, which is why this carries the same
            // step_up tolerance the grounded branch does.
            //
            // WITHOUT IT, EVERY JUMP STICKS TO THE GROUND FOR ITS FIRST FRAMES.
            // The feet leave at 8 m/s, so they are 1 cm up after a tick at
            // 700 fps -- while ground_height is the highest of the four body
            // corners, which crosses into the next 10 cm column the moment you
            // move. So a lip you would have strolled over becomes a wall for
            // however long it takes to rise 10 cm, and the whole horizontal
            // move is thrown away for those ticks. It reads as the jump
            // refusing to carry you forward -- you go up, and come down where
            // you started.
            if g > self.pos.y + self.step_up {
                return; // a wall; a step is not
            }
            self.pos = next;
            if g > self.pos.y {
                // Rode up onto a step in mid-air. The feet snap to it exactly
                // as they do on the ground, with the jolt taken out of the eye.
                self.step_lag =
                    (self.step_lag + (self.pos.y - g)).clamp(-self.step_down, self.step_up);
                self.pos.y = g;
            }
        }
    }

    // -----------------------------------------------------------------------
    // The bob, matching the JS engine including the thing it got wrong first.
    //
    // ONE DIP PER STRIDE, not per footfall. cos(phase * 2) is what a real head
    // does -- it dips on each foot -- but at this cadence it reads as a jitter
    // rather than as walking. cos(phase) is slower and reads as a gait.
    //
    // The amplitude is eased rather than set, so starting and stopping ramp the
    // bob in and out instead of switching it. It is zero in the air and zero
    // flying, which falls out of the same easing: the target simply goes to 0.
    // -----------------------------------------------------------------------
    fn update_bob(&mut self, dt: f32) {
        let spd = self.speed();
        let target = if self.on_ground && !self.fly {
            1.0f32.min(spd / self.walk)
        } else {
            0.0
        };
        self.bob_amp += (target - self.bob_amp) * (1.0 - (-8.0 * dt).exp());

        // Per METRE TRAVELLED, not per second: the bob has to stay locked to
        // the stride, or the gait would speed up and slow down with the walk
        // instead of lengthening.
        self.bob_phase += spd * dt * BOB_RATE;
        if self.bob_phase > TAU * 1024.0 {
            self.bob_phase -= TAU * 1024.0; // keep float precision
        }

        // Sprinting is the same curve pushed further rather than a second rule:
        // bob_amp saturates at a walk, and this term takes over above it.
        let run = (spd / self.walk - 1.0).clamp(0.0, 1.0);
        self.cam_bob_y = -self.bob_phase.cos() * CAM_BOB * self.bob_amp * (1.0 + CAM_BOB_RUN * run);
    }
}
