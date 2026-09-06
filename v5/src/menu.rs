// ---------------------------------------------------------------------------
// menu.rs -- the settings menu, on Y. Ported from v2's render/menu.h.
//
// Every knob here is also a launch flag, and that is deliberate: the flags are
// unusable as a way to FIND a setting, because choosing between them means
// knowing the answer already. Quitting, editing a command line and reloading a
// hundred and fifty million triangles to try one number is not a way to learn
// what the number does. In here the change lands on the next frame and the fps
// counter at the top reacts, so the trade is visible while you make it.
//
// WHAT CHANGED IN THE PORT. v2 drew this with GDI into its own GL texture,
// because its renderer had no UI of any kind and the menu had to stay sharp
// while the render scale was low. bevy_ui rasterises on top of the image after
// DLSS has already upscaled it, so none of that machinery is needed -- but the
// LAYOUT problem came back in a new form. v2 could align columns by counting
// characters because it chose a monospace font; the default font here is
// proportional, so the columns are fixed-width Nodes instead. Padding with
// spaces would have drifted a little on every row.
//
// The rows themselves are different because the ENGINE is different. v2's
// "render scale" and "samples per frame" were about brute-force accumulation
// and have no meaning against a ReSTIR integrator; what replaced them are the
// knobs that actually cost something here -- render distance, and the
// world-cache cell size that turned out to be worth 2x on its own.
//
// Rows that would need the world rebuilt -- seed, densities, asset paths -- are
// deliberately absent, exactly as they were in v2. A menu that silently does
// nothing is worse than one that does not offer the control.
// ---------------------------------------------------------------------------

use bevy::camera::{Exposure, Projection};
use bevy::diagnostic::{DiagnosticsStore, FrameTimeDiagnosticsPlugin};
use bevy::picking::hover::Hovered;
use bevy::ecs::system::SystemParam;
use bevy::prelude::*;
use bevy::solari::prelude::SolariLighting;
use bevy::ui_widgets::{observe, Activate, Button};

use crate::hud::HudState;
use crate::player::Player;
use crate::scene::chunks::ChunkStreamer;
use crate::scene::daynight::DayNight;
use crate::world::SkyState;

#[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))]
use bevy::{
    anti_alias::contrast_adaptive_sharpening::ContrastAdaptiveSharpening,
    anti_alias::dlss::{Dlss, DlssRayReconstructionFeature, DlssRayReconstructionSupported},
    render::camera::{MipBias, TemporalJitter},
};

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------
pub const ROW_PRESET: usize = 0;
pub const ROW_DENOISER: usize = 1;
pub const ROW_VIEW: usize = 2;
pub const ROW_BOUNCES: usize = 3;
pub const ROW_GI_CELL: usize = 4;
pub const ROW_RESTIR: usize = 5;
pub const ROW_EXPOSURE: usize = 6;
pub const ROW_FOV: usize = 7;
pub const ROW_SPEED: usize = 8;
pub const ROW_TIME: usize = 9;
pub const ROW_CYCLE: usize = 10;
pub const ROW_BAKE: usize = 11;
pub const ROW_COUNT: usize = 12;

const LABELS: [&str; ROW_COUNT] = [
    "Preset",
    "Denoiser",
    "Render distance",
    "Bounces",
    "GI cell size",
    "ReSTIR reuse",
    "Exposure",
    "Field of view",
    "Walk speed",
    "Time of day",
    "Cycle speed",
    "Bake as default",
];

/// The presets, and the numbers on them are MEASURED on this machine and this
/// scene rather than guessed -- see the README for the table they came from.
///
/// What they encode is that RENDER DISTANCE IS THE EXPENSIVE ONE, because
/// Solari's primary visibility is rasterised and a rasteriser is linear in the
/// triangles you hand it. Bounces are comparatively cheap, which is the
/// opposite of the instinct, so "Deep GI" spends its budget there instead of on
/// distance.
pub struct Preset {
    pub name: &'static str,
    pub view: i32,
    pub bounces: u32,
    pub gi_cell: f32,
    pub note: &'static str,
}

pub const PRESETS: [Preset; 5] = [
    Preset {
        name: "Max FPS",
        view: 2,
        bounces: 2,
        gi_cell: 1.2,
        note: "51 m of wood",
    },
    Preset {
        name: "Fast",
        view: 3,
        bounces: 3,
        gi_cell: 0.8,
        note: "77 m",
    },
    Preset {
        name: "Balanced",
        view: 4,
        bounces: 3,
        gi_cell: 0.6,
        note: "the default -- 102 m",
    },
    Preset {
        name: "Far",
        view: 6,
        bounces: 3,
        gi_cell: 0.6,
        note: "154 m, and it costs",
    },
    Preset {
        name: "Deep GI",
        view: 4,
        bounces: 5,
        gi_cell: 0.35,
        note: "sharper indirect light",
    },
];

#[derive(Resource, Default)]
pub struct SettingsMenu {
    pub open: bool,
    pub row: usize,
    pub bake_status: String,
}

/// One adjustment, from a key or from a click on a stepper. Routed through a
/// message rather than applied where it was raised, so the one system that
/// knows how to change a setting is the only thing that ever does -- a click
/// and a key press cannot drift apart if neither of them does the work.
#[derive(Message)]
pub struct MenuAdjust {
    pub row: usize,
    pub dir: i32,
}

/// Jump straight to a preset by index, which the 1-5 keys need and a direction
/// cannot express.
#[derive(Message)]
pub struct MenuPreset(pub usize);

// ---------------------------------------------------------------------------
// Building the panel
// ---------------------------------------------------------------------------
#[derive(Component)]
pub struct MenuRoot;

#[derive(Component)]
pub struct MenuHeader;

#[derive(Component)]
pub struct MenuStats;

#[derive(Component)]
pub struct RowNode(usize);

#[derive(Component)]
pub struct RowMarker(usize);

#[derive(Component)]
pub struct RowLabel(usize);

#[derive(Component)]
pub struct RowValue(usize);

const DIM: Color = Color::srgb(0.59, 0.62, 0.67);
const TEXT: Color = Color::srgb(0.89, 0.91, 0.94);
const HOT: Color = Color::srgb(0.49, 0.86, 1.0);
const NOTE: Color = Color::srgb(0.50, 0.55, 0.61);
const TITLE: Color = Color::srgb(1.0, 0.84, 0.47);

fn font(size: f32) -> TextFont {
    TextFont {
        font_size: bevy::text::FontSize::Px(size),
        ..default()
    }
}

pub fn spawn_menu(mut commands: Commands) {
    // TWO NODES, NOT ONE. The panel is centred by a full-screen container that
    // centres its child, which is the only arrangement bevy_ui actually
    // centres: an absolutely-positioned node with all four insets set takes its
    // SIZE from those insets, so pinning the panel to every edge and hoping
    // `margin: auto` would centre it instead stretched its background across
    // the viewport. The container is transparent and does nothing but the
    // layout; the panel is the thing you see.
    let root = commands
        .spawn((
            MenuRoot,
            Node {
                position_type: PositionType::Absolute,
                top: px(0.0),
                left: px(0.0),
                width: percent(100.0),
                height: percent(100.0),
                align_items: AlignItems::Center,
                justify_content: JustifyContent::Center,
                display: Display::None,
                ..default()
            },
            // Nothing is drawn for the container itself, so a click that misses
            // the panel falls through to the world rather than being swallowed
            // by an invisible full-screen surface.
            Pickable::IGNORE,
        ))
        .id();

    let root = commands
        .spawn((
            Node {
                width: px(620.0),
                flex_direction: FlexDirection::Column,
                padding: px(16.0).all(),
                row_gap: px(2.0),
                border_radius: BorderRadius::all(px(10.0)),
                ..default()
            },
            BackgroundColor(Color::srgba(0.04, 0.06, 0.09, 0.94)),
            ChildOf(root),
        ))
        .id();

    commands.spawn((
        MenuHeader,
        Text::new("v5  settings"),
        font(16.0),
        TextColor(TITLE),
        ChildOf(root),
    ));
    commands.spawn((
        MenuStats,
        Text::default(),
        font(11.0),
        TextColor(NOTE),
        Node {
            margin: UiRect::bottom(px(6.0)),
            ..default()
        },
        ChildOf(root),
    ));

    for i in 0..ROW_COUNT {
        let row = commands
            .spawn((
                RowNode(i),
                Hovered::default(),
                Node {
                    flex_direction: FlexDirection::Row,
                    align_items: AlignItems::Center,
                    column_gap: px(4.0),
                    padding: UiRect::vertical(px(1.0)),
                    ..default()
                },
                ChildOf(root),
            ))
            .id();

        commands.spawn((
            RowMarker(i),
            Text::new(" "),
            font(12.0),
            TextColor(HOT),
            Node {
                width: px(12.0),
                ..default()
            },
            ChildOf(row),
        ));
        commands.spawn((
            RowLabel(i),
            Text::new(LABELS[i]),
            font(12.0),
            TextColor(DIM),
            Node {
                width: px(130.0),
                ..default()
            },
            ChildOf(row),
        ));
        commands.spawn((
            RowValue(i),
            Text::default(),
            font(12.0),
            TextColor(TEXT),
            Node {
                flex_grow: 1.0,
                ..default()
            },
            ChildOf(row),
        ));

        // The steppers are on every row, not just the selected one: they are
        // the click target, and a target that only appears once you have
        // already hit it is not a target.
        for (dir, glyph) in [(-1, "<"), (1, ">")] {
            commands.spawn((
                Button,
                Hovered::default(),
                Node {
                    width: px(22.0),
                    justify_content: JustifyContent::Center,
                    ..default()
                },
                children![(Text::new(glyph), font(13.0), TextColor(DIM))],
                observe(move |_: On<Activate>, mut w: MessageWriter<MenuAdjust>| {
                    w.write(MenuAdjust { row: i, dir });
                }),
                ChildOf(row),
            ));
        }
    }

    commands.spawn((
        Text::new(
            "up/down or hover choose     left/right, click < >, wheel change\n\
             1-5 preset                  Y or ESC close",
        ),
        font(11.0),
        TextColor(NOTE),
        Node {
            margin: UiRect::top(px(8.0)),
            ..default()
        },
        ChildOf(root),
    ));
}

// ---------------------------------------------------------------------------
// Hover selects the row under the pointer, exactly as v2 did.
// ---------------------------------------------------------------------------
pub fn menu_hover(mut menu: ResMut<SettingsMenu>, rows: Query<(&RowNode, &Hovered)>) {
    if !menu.open {
        return;
    }
    for (row, hovered) in &rows {
        if hovered.0 {
            menu.row = row.0;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Applying an adjustment
// ---------------------------------------------------------------------------
#[derive(SystemParam)]
pub struct MenuTargets<'w, 's> {
    pub streamer: ResMut<'w, ChunkStreamer>,
    pub clock: ResMut<'w, DayNight>,
    pub player: ResMut<'w, Player>,
    pub camera: Query<
        'w,
        's,
        (
            Entity,
            &'static mut SolariLighting,
            &'static mut Exposure,
            &'static mut Projection,
        ),
    >,
}

#[allow(clippy::too_many_arguments)]
pub fn apply_menu_adjust(
    mut reader: MessageReader<MenuAdjust>,
    mut presets: MessageReader<MenuPreset>,
    mut menu: ResMut<SettingsMenu>,
    mut t: MenuTargets,
    mut hud: ResMut<HudState>,
    mut commands: Commands,
    #[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))] dlss_on: Query<
        Has<Dlss<DlssRayReconstructionFeature>>,
        With<SolariLighting>,
    >,
    #[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))] dlss_supported: Option<
        Res<DlssRayReconstructionSupported>,
    >,
) {
    // Absolute preset picks first, so a 1-5 press and a stepper click in the
    // same frame resolve in the order they were meant.
    for MenuPreset(i) in presets.read() {
        let Some(p) = PRESETS.get(*i) else { continue };
        let Ok((_, mut solari, _, _)) = t.camera.single_mut() else {
            continue;
        };
        set_view(&mut t.streamer, p.view);
        solari.max_bounces = p.bounces;
        solari.world_cache_position_base_cell_size = p.gi_cell;
        menu.row = ROW_PRESET;
    }

    for adjust in reader.read() {
        let dir = adjust.dir;
        let row = adjust.row;
        menu.row = row;

        let Ok((cam_entity, mut solari, mut exposure, mut projection)) = t.camera.single_mut()
        else {
            continue;
        };

        match row {
            ROW_PRESET => {
                // Stepped from where the settings ACTUALLY are, so the first
                // press moves one preset rather than jumping to a remembered
                // cursor position.
                let n = PRESETS.len() as i32;
                let cur = current_preset(&t.streamer, &solari);
                let next = match cur {
                    Some(i) => (i as i32 + dir).rem_euclid(n),
                    None => {
                        if dir > 0 {
                            0
                        } else {
                            n - 1
                        }
                    }
                };
                let p = &PRESETS[next as usize];
                set_view(&mut t.streamer, p.view);
                solari.max_bounces = p.bounces;
                solari.world_cache_position_base_cell_size = p.gi_cell;
            }
            ROW_DENOISER => {
                #[cfg(all(feature = "dlss", not(feature = "force_disable_dlss")))]
                {
                    if dlss_supported.is_none() {
                        hud.dlss = "unsupported".into();
                        continue;
                    }
                    let on = dlss_on.single().unwrap_or(false);
                    if on {
                        // TemporalJitter and MipBias go with it: DLSS inserts
                        // both, and leaving them behind would keep jittering
                        // the camera for a denoiser that is no longer there.
                        commands.entity(cam_entity).remove::<(
                            Dlss<DlssRayReconstructionFeature>,
                            ContrastAdaptiveSharpening,
                            TemporalJitter,
                            MipBias,
                        )>();
                        hud.dlss = "off".into();
                    } else {
                        commands.entity(cam_entity).insert((
                            Dlss::<DlssRayReconstructionFeature>::default(),
                            ContrastAdaptiveSharpening {
                                sharpening_strength: 0.4,
                                ..default()
                            },
                        ));
                        hud.dlss = "DLSS-RR".into();
                    }
                }
                #[cfg(not(all(feature = "dlss", not(feature = "force_disable_dlss"))))]
                {
                    let _ = &mut commands;
                    hud.dlss = "not compiled in".into();
                }
            }
            ROW_VIEW => {
                let v = (t.streamer.view_chunks + dir).clamp(1, 16);
                set_view(&mut t.streamer, v);
            }
            ROW_BOUNCES => {
                solari.max_bounces = (solari.max_bounces as i32 + dir).clamp(1, 8) as u32;
            }
            ROW_GI_CELL => {
                // Multiplicative: the useful range spans more than an order of
                // magnitude and a linear step is useless at one end or the other.
                let c = solari.world_cache_position_base_cell_size
                    * if dir > 0 { 1.25 } else { 1.0 / 1.25 };
                solari.world_cache_position_base_cell_size = c.clamp(0.05, 4.0);
            }
            ROW_RESTIR => solari.restir = !solari.restir,
            ROW_EXPOSURE => {
                // Exposure is perceived multiplicatively, so it is stepped that
                // way. Bevy's ev100 is a log scale already, so this is a plain
                // subtraction there.
                exposure.ev100 -= 0.25 * dir as f32;
                exposure.ev100 = exposure.ev100.clamp(-8.0, 8.0);
            }
            ROW_FOV => {
                if let Projection::Perspective(p) = &mut *projection {
                    let fov = p.fov.to_degrees() + 2.0 * dir as f32;
                    p.fov = fov.clamp(10.0, 110.0).to_radians();
                }
            }
            ROW_SPEED => {
                t.player.walk = (t.player.walk * if dir > 0 { 1.3 } else { 1.0 / 1.3 })
                    .clamp(0.2, 200.0);
            }
            ROW_TIME => t.clock.scrub_hours(0.25 * dir as f32),
            ROW_CYCLE => t.clock.nudge_speed(dir > 0),
            ROW_BAKE => {
                // An action, not a value -- either direction fires it.
                // Fields passed individually rather than as `&t`: the camera
                // query inside it is already mutably borrowed here, and a
                // whole-struct borrow would collide with that even though the
                // fields it reads are disjoint.
                menu.bake_status = bake_defaults(
                    &solari,
                    &exposure,
                    &projection,
                    t.player.eye,
                    t.player.walk,
                    t.clock.tday,
                    t.clock.cycle_speed,
                    t.streamer.view_chunks,
                    t.streamer.terrain.water_level,
                );
                info!("v5: {}", menu.bake_status);
            }
            _ => {}
        }
    }
}

/// Changing the ring radius has to force a re-ring, because the streamer only
/// reconsiders when the camera crosses a chunk boundary -- otherwise a bigger
/// view distance would not arrive until you happened to walk 25 m.
fn set_view(streamer: &mut ChunkStreamer, view: i32) {
    if streamer.view_chunks == view {
        return;
    }
    streamer.view_chunks = view;
    streamer.force_rering();
}

/// Which preset, if any, the live settings currently ARE.
///
/// Without this the row shows whatever preset the cursor last sat on, which on
/// startup means advertising one while the renderer is on whatever the flags
/// said. A menu that misreports the current state is worse than one with no
/// preset row at all.
fn current_preset(streamer: &ChunkStreamer, solari: &SolariLighting) -> Option<usize> {
    PRESETS.iter().position(|p| {
        p.view == streamer.view_chunks
            && p.bounces == solari.max_bounces
            && (p.gi_cell - solari.world_cache_position_base_cell_size).abs() < 0.005
    })
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------
/// Rewrite `src/defaults.rs` from the live settings, so the thing you tuned is
/// what v5 opens with next time. The same idea as v2's bake, and for the same
/// reason: it writes SOURCE rather than a config file, so the defaults are
/// visible in the diff, travel with the branch, and cost nothing at runtime.
///
/// `CARGO_MANIFEST_DIR` is baked in at compile time, which is exactly v2's
/// V2_SOURCE_DIR trick without the build system having to pass it.
#[allow(clippy::too_many_arguments)]
fn bake_defaults(
    solari: &SolariLighting,
    exposure: &Exposure,
    projection: &Projection,
    eye: f32,
    speed: f32,
    tday: f32,
    cycle_speed: f32,
    view_chunks: i32,
    water_level: f32,
) -> String {
    let dir = env!("CARGO_MANIFEST_DIR");
    let path = std::path::Path::new(dir).join("src").join("defaults.rs");

    let fov = match projection {
        Projection::Perspective(p) => p.fov.to_degrees(),
        _ => crate::defaults::FOV,
    };
    // Bevy's exposure multiplier is 1 / (1.2 * 2^ev100); --exposure is that
    // multiplier, so this inverts the mapping main.rs applies on the way in.
    let exposure_mul = 1.0 / (1.2 * exposure.ev100.exp2());

    let existing = match std::fs::read_to_string(&path) {
        Ok(s) => s,
        Err(e) => return format!("could not read {}: {e}", path.display()),
    };
    let src = crate::defaults::rewrite_source(&existing, &crate::defaults::Baked {
        exposure: exposure_mul,
        fov,
        eye,
        speed,
        time_of_day: tday,
        cycle_speed,
        view_chunks,
        water_level,
        bounces: solari.max_bounces,
        gi_cell: solari.world_cache_position_base_cell_size,
        restir: solari.restir,
    });

    match std::fs::write(&path, src) {
        Ok(_) => format!("baked to {} -- rebuild to apply", path.display()),
        Err(e) => format!("could not write {}: {e}", path.display()),
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
#[allow(clippy::too_many_arguments)]
pub fn update_menu(
    menu: Res<SettingsMenu>,
    streamer: Res<ChunkStreamer>,
    clock: Res<DayNight>,
    sky: Res<SkyState>,
    player: Res<Player>,
    hud: Res<HudState>,
    diagnostics: Res<DiagnosticsStore>,
    camera: Query<(&SolariLighting, &Exposure, &Projection)>,
    mut root: Query<&mut Node, With<MenuRoot>>,
    mut header: Query<&mut Text, (With<MenuHeader>, Without<MenuStats>)>,
    mut stats: Query<&mut Text, (With<MenuStats>, Without<MenuHeader>)>,
    mut markers: Query<(&RowMarker, &mut Text, &mut TextColor), (Without<MenuHeader>, Without<MenuStats>, Without<RowLabel>, Without<RowValue>)>,
    mut labels: Query<(&RowLabel, &mut TextColor), (Without<RowMarker>, Without<RowValue>)>,
    mut values: Query<(&RowValue, &mut Text, &mut TextColor), (Without<RowMarker>, Without<RowLabel>, Without<MenuHeader>, Without<MenuStats>)>,
) {
    for mut node in &mut root {
        node.display = if menu.open {
            Display::Flex
        } else {
            Display::None
        };
    }
    if !menu.open {
        return;
    }

    let Ok((solari, exposure, projection)) = camera.single() else {
        return;
    };
    let fps = diagnostics
        .get(&FrameTimeDiagnosticsPlugin::FPS)
        .and_then(|d| d.smoothed())
        .unwrap_or(0.0);

    for mut t in &mut header {
        t.0 = format!("v5  settings                                  {fps:5.1} fps");
    }
    for mut t in &mut stats {
        t.0 = format!(
            "{} chunks, {} instances, {:.0} M triangles resident   denoise: {}",
            streamer.chunk_count(),
            streamer.resident_instances,
            streamer.resident_tris as f64 / 1e6,
            hud.dlss,
        );
    }

    for (m, mut text, mut colour) in &mut markers {
        let sel = m.0 == menu.row;
        text.0 = if sel { ">" } else { " " }.into();
        colour.0 = HOT;
    }
    for (l, mut colour) in &mut labels {
        colour.0 = if l.0 == menu.row { TEXT } else { DIM };
    }
    for (v, mut text, mut colour) in &mut values {
        text.0 = row_value(
            v.0, &streamer, &clock, &sky, &player, solari, exposure, projection, &menu,
            &hud.dlss,
        );
        colour.0 = if v.0 == menu.row { HOT } else { TEXT };
    }
}

#[allow(clippy::too_many_arguments)]
fn row_value(
    row: usize,
    streamer: &ChunkStreamer,
    clock: &DayNight,
    sky: &SkyState,
    player: &Player,
    solari: &SolariLighting,
    exposure: &Exposure,
    projection: &Projection,
    menu: &SettingsMenu,
    dlss: &str,
) -> String {
    let on_off = |b: bool| if b { "on" } else { "off" };
    match row {
        ROW_PRESET => match current_preset(streamer, solari) {
            Some(i) => {
                let p = &PRESETS[i];
                if p.note.is_empty() {
                    p.name.into()
                } else {
                    format!("{}   {}", p.name, p.note)
                }
            }
            None => "Custom   left/right to pick a preset".into(),
        },
        ROW_DENOISER => format!("{dlss}   DLSS Ray Reconstruction"),
        ROW_VIEW => format!(
            "{}   {:.0} m of wood",
            streamer.view_chunks,
            streamer.view_chunks as f32 * crate::scene::terrain::CHUNK_M
        ),
        ROW_BOUNCES => format!("{}", solari.max_bounces),
        ROW_GI_CELL => format!(
            "{:.2} m   smaller is sharper GI, and far slower",
            solari.world_cache_position_base_cell_size
        ),
        ROW_RESTIR => format!(
            "{}   reuse across frames and pixels",
            on_off(solari.restir)
        ),
        ROW_EXPOSURE => format!("{:.2}", 1.0 / (1.2 * exposure.ev100.exp2())),
        ROW_FOV => match projection {
            Projection::Perspective(p) => format!("{:.0} deg", p.fov.to_degrees()),
            _ => "-".into(),
        },
        ROW_SPEED => format!("{:.1} m/s", player.walk),
        ROW_TIME => format!(
            "{}   sun az {:.0} el {:.0}{}",
            clock.clock(),
            sky.sky.azimuth_deg(),
            sky.sky.elevation_deg(),
            // Worth saying out loud: below the horizon the sky fit is pinned
            // and scaled down rather than extrapolated, so night is a different
            // regime and not merely a darker one. See sky.rs.
            if clock.is_night() { "  (night)" } else { "" }
        ),
        ROW_CYCLE => format!("{}   (X + wheel)", clock.speed_label()),
        ROW_BAKE => {
            if menu.bake_status.is_empty() {
                "click > to write src/defaults.rs".into()
            } else {
                menu.bake_status.clone()
            }
        }
        _ => String::new(),
    }
}
