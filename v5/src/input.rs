// ---------------------------------------------------------------------------
// input.rs -- the controls, kept the same as v2's so muscle memory carries over.
//
//   W A S D walk        shift sprint        space jump / fly up
//   Q or ctrl down      F toggle fly        right-drag or captured mouse looks
//   arrows scrub the clock (up/down are the fast ones)
//   X + wheel sets the cycle speed -- scroll DOWN past the slowest notch to run
//   time BACKWARDS (see daynight.rs; there is no reverse mode to enter)
//   wheel alone zooms the field of view
//   P pause the clock       Y toggles the HUD       F1 prints the help
//   ESC releases the mouse; ESC again quits
// ---------------------------------------------------------------------------

use bevy::input::mouse::{AccumulatedMouseMotion, MouseScrollUnit, MouseWheel};
use bevy::prelude::*;
use bevy::window::{CursorGrabMode, CursorOptions, PrimaryWindow};

use crate::hud::HudState;
use crate::menu::{MenuAdjust, MenuPreset, SettingsMenu, ROW_COUNT};
use crate::player::{Player, WalkWorld};
use crate::scene::chunks::ChunkStreamer;
use crate::scene::collide::Solid;
use crate::scene::daynight::DayNight;

/// Mouse look sensitivity, degrees per pixel of motion.
const LOOK_SENSITIVITY: f32 = 0.12;
const PITCH_LIMIT: f32 = 89.0;

#[derive(Resource, Default)]
pub struct MouseCaptured(pub bool);

/// Scratch for the colliders near the player, reused every tick so a walk does
/// not allocate. Which chunks are resident changes underfoot, so this is
/// regathered rather than kept.
#[derive(Resource, Default)]
pub struct NearbySolids(pub Vec<Solid>);

pub fn grab_mouse(
    mouse: Res<ButtonInput<MouseButton>>,
    keys: Res<ButtonInput<KeyCode>>,
    mut captured: ResMut<MouseCaptured>,
    menu: Res<SettingsMenu>,
    mut cursor: Query<&mut CursorOptions, With<PrimaryWindow>>,
    mut exit: MessageWriter<AppExit>,
) {
    let Ok(mut cursor) = cursor.single_mut() else {
        return;
    };

    // A click only takes the mouse back when the menu is closed -- while it is
    // open the pointer belongs to the menu, which is the whole reason Y frees
    // it in the first place.
    if mouse.just_pressed(MouseButton::Left) && !captured.0 && !menu.open {
        captured.0 = true;
    }
    if keys.just_pressed(KeyCode::Escape) {
        // ESC closes the menu, then releases the mouse, then quits. Three
        // steps, so a stray press while walking never closes the window.
        if menu.open {
            // handled in menu_input, which runs first
        } else if captured.0 {
            captured.0 = false;
        } else {
            exit.write(AppExit::Success);
        }
    }

    // THE POINTER IS VISIBLE EXACTLY WHEN IT IS NOT LOCKED, and those two have
    // to be derived from the same condition. They were not: `visible` was set
    // from `captured` alone while the grab also considered the menu, so opening
    // the menu released the pointer and left it INVISIBLE -- a menu you could
    // click but not aim.
    let locked = captured.0 && !menu.open;
    let want = if locked {
        CursorGrabMode::Locked
    } else {
        CursorGrabMode::None
    };
    if cursor.grab_mode != want {
        cursor.grab_mode = want;
    }
    if cursor.visible == locked {
        cursor.visible = !locked;
    }
}

pub fn look(
    motion: Res<AccumulatedMouseMotion>,
    mouse: Res<ButtonInput<MouseButton>>,
    captured: Res<MouseCaptured>,
    menu: Res<SettingsMenu>,
    mut player: ResMut<Player>,
) {
    // Right-drag looks even when the mouse is free, which is what v2 did and
    // what makes the overlay usable without giving up the camera. Not while the
    // menu is open, though: the pointer is aiming at rows.
    if menu.open {
        return;
    }
    if !captured.0 && !mouse.pressed(MouseButton::Right) {
        return;
    }
    let d = motion.delta;
    if d == Vec2::ZERO {
        return;
    }
    player.yaw += d.x * LOOK_SENSITIVITY;
    player.pitch = (player.pitch - d.y * LOOK_SENSITIVITY).clamp(-PITCH_LIMIT, PITCH_LIMIT);
}

pub fn walk(
    time: Res<Time>,
    keys: Res<ButtonInput<KeyCode>>,
    streamer: Res<ChunkStreamer>,
    menu: Res<SettingsMenu>,
    mut solids: ResMut<NearbySolids>,
    mut player: ResMut<Player>,
) {
    let dt = time.delta_secs().min(0.1); // a long hitch must not teleport anyone

    // Six metres reaches past the widest boulder plus a body, which is all the
    // player can touch before the next gather.
    streamer.colliders_near(player.pos, 6.0, &mut solids.0);
    let world = WalkWorld {
        terrain: &streamer.terrain,
        solids: &solids.0,
    };

    if keys.just_pressed(KeyCode::KeyF) && !menu.open {
        player.fly = !player.fly;
        if !player.fly {
            player.vy = 0.0;
        }
    }

    // Flattened to the ground plane: looking up must not make W climb.
    let yaw = player.yaw.to_radians();
    let forward = Vec3::new(yaw.cos(), 0.0, yaw.sin());
    let right = Vec3::new(-yaw.sin(), 0.0, yaw.cos());

    let mut m = Vec3::ZERO;
    // Taken and cleared, so a scripted walk lasts exactly as long as something
    // keeps setting it.
    let scripted = std::mem::take(&mut player.held_forward);
    if scripted || keys.pressed(KeyCode::KeyW) {
        m += forward;
    }
    if keys.pressed(KeyCode::KeyS) {
        m -= forward;
    }
    if keys.pressed(KeyCode::KeyD) {
        m += right;
    }
    if keys.pressed(KeyCode::KeyA) {
        m -= right;
    }
    if m != Vec3::ZERO {
        m = m.normalize();
    }

    let sprint = keys.pressed(KeyCode::ShiftLeft) || keys.pressed(KeyCode::ShiftRight);
    let jump = keys.pressed(KeyCode::Space);
    let down = keys.pressed(KeyCode::ControlLeft) || keys.pressed(KeyCode::KeyQ);

    // Still TICKED while the menu is open, with no input: gravity and the step
    // smoothing have to keep running or you would hang in the air mid-jump and
    // land the instant it closed. Only the controls are taken away.
    if menu.open {
        m = Vec3::ZERO;
        player.update(&world, m, false, false, false, dt);
        return;
    }

    player.update(&world, m, sprint, jump, down, dt);
}

// ---------------------------------------------------------------------------
// The settings menu owns the keyboard while it is open.
//
// This runs BEFORE clock_controls, and the two share the arrow keys: they scrub
// the sun when the menu is closed and choose a row when it is open. v2 solved
// that by having the menu report whether it had consumed a key; here it is
// simply an ordering plus a check on `menu.open`, which is the same rule with
// nothing to keep in sync.
// ---------------------------------------------------------------------------
pub fn menu_input(
    keys: Res<ButtonInput<KeyCode>>,
    mut wheel: MessageReader<MouseWheel>,
    mut menu: ResMut<SettingsMenu>,
    mut adjust: MessageWriter<MenuAdjust>,
    mut preset: MessageWriter<MenuPreset>,
) {
    if keys.just_pressed(KeyCode::KeyY) {
        menu.open = !menu.open;
    }
    if !menu.open {
        return;
    }
    if keys.just_pressed(KeyCode::Escape) {
        menu.open = false;
        return;
    }

    if keys.just_pressed(KeyCode::ArrowUp) {
        menu.row = (menu.row + ROW_COUNT - 1) % ROW_COUNT;
    }
    if keys.just_pressed(KeyCode::ArrowDown) {
        menu.row = (menu.row + 1) % ROW_COUNT;
    }
    if keys.just_pressed(KeyCode::ArrowLeft) {
        adjust.write(MenuAdjust {
            row: menu.row,
            dir: -1,
        });
    }
    if keys.just_pressed(KeyCode::ArrowRight) {
        adjust.write(MenuAdjust {
            row: menu.row,
            dir: 1,
        });
    }

    // 1-5 jump straight to a preset, wherever the cursor is. Pressing a preset
    // key also moves the cursor to the preset row, so the thing that changed is
    // the thing you are looking at.
    const DIGITS: [KeyCode; 5] = [
        KeyCode::Digit1,
        KeyCode::Digit2,
        KeyCode::Digit3,
        KeyCode::Digit4,
        KeyCode::Digit5,
    ];
    for (i, key) in DIGITS.iter().enumerate() {
        if keys.just_pressed(*key) {
            preset.write(MenuPreset(i));
        }
    }

    // The wheel adjusts whatever row is under the pointer, which hover has
    // already selected.
    for ev in wheel.read() {
        let dy = wheel_delta(ev);
        if dy != 0.0 {
            adjust.write(MenuAdjust {
                row: menu.row,
                dir: if dy > 0.0 { 1 } else { -1 },
            });
        }
    }
}

fn wheel_delta(ev: &MouseWheel) -> f32 {
    match ev.unit {
        MouseScrollUnit::Line => ev.y,
        MouseScrollUnit::Pixel => ev.y / 40.0,
    }
}

pub fn clock_controls(
    time: Res<Time>,
    keys: Res<ButtonInput<KeyCode>>,
    menu: Res<SettingsMenu>,
    mut wheel: MessageReader<MouseWheel>,
    mut clock: ResMut<DayNight>,
    mut hud: ResMut<HudState>,
) {
    if menu.open {
        // The menu has the keyboard and the wheel. Drained rather than left
        // queued, so closing the menu does not then replay a burst of scrolls
        // into the day/night speed.
        wheel.clear();
        return;
    }

    if keys.just_pressed(KeyCode::KeyP) {
        clock.paused = !clock.paused;
    }
    if keys.just_pressed(KeyCode::KeyH) {
        hud.visible = !hud.visible;
    }
    if keys.just_pressed(KeyCode::F1) {
        hud.print_help();
    }

    // Scrubbing is per second held, not per press, so a long hold sweeps the
    // day and a tap nudges it.
    let dt = time.delta_secs();
    let scrub = 1.0 * dt;
    if keys.pressed(KeyCode::ArrowLeft) {
        clock.scrub_hours(-scrub);
    }
    if keys.pressed(KeyCode::ArrowRight) {
        clock.scrub_hours(scrub);
    }
    if keys.pressed(KeyCode::ArrowUp) {
        clock.scrub_hours(scrub * 6.0);
    }
    if keys.pressed(KeyCode::ArrowDown) {
        clock.scrub_hours(-scrub * 6.0);
    }

    // X + wheel sets the day/night speed, and the wheel ALONE now does nothing.
    // It used to zoom the field of view, which was a poor use of the only
    // analogue control on the mouse: fov is a thing you set once and leave, and
    // having it on an axis you brush past meant arriving somewhere with the
    // wrong one and no idea why. It is a menu row now.
    let holding_x = keys.pressed(KeyCode::KeyX);
    for ev in wheel.read() {
        let dy = wheel_delta(ev);
        if dy != 0.0 && holding_x {
            clock.nudge_speed(dy > 0.0);
        }
    }
}

/// Put the camera where the player's eyes are. Runs after `walk`, so the
/// transform a frame renders with is the one this tick produced.
pub fn drive_camera(player: Res<Player>, mut camera: Query<&mut Transform, With<Camera3d>>) {
    let Ok(mut t) = camera.single_mut() else {
        return;
    };
    let eye = player.eye_position();
    *t = Transform::from_translation(eye).looking_at(eye + player.look_dir(), Vec3::Y);
}
