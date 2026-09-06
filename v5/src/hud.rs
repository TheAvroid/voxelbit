// ---------------------------------------------------------------------------
// hud.rs -- what the engine is doing, in the corner of the window.
//
// v2 drew its settings menu with GDI into its own GL texture, at window
// resolution, so that it stayed sharp when the render scale was low. None of
// that is needed here: bevy_ui rasterises on top of the raytraced image at full
// resolution after DLSS has already upscaled it, so the text is native
// regardless of what the renderer is doing underneath.
// ---------------------------------------------------------------------------

use bevy::diagnostic::{DiagnosticsStore, FrameTimeDiagnosticsPlugin};
use bevy::prelude::*;

use crate::menu::SettingsMenu;

#[derive(Resource)]
pub struct HudState {
    pub visible: bool,
    /// What the renderer settled on, filled in once at startup so the HUD can
    /// say whether DLSS actually engaged rather than whether it was asked for.
    pub dlss: String,
}

impl Default for HudState {
    fn default() -> Self {
        Self {
            visible: true,
            dlss: "off".into(),
        }
    }
}

impl HudState {
    pub fn print_help(&self) {
        println!(
            "\n\
             v5 controls\n\
             \x20 Y          SETTINGS MENU -- every render knob, live\n\
             \x20 W A S D    walk            shift  sprint\n\
             \x20 space      jump / fly up   Q ctrl fly down\n\
             \x20 F          toggle fly      mouse  look (right-drag when free)\n\
             \x20 X + wheel  day/night speed -- the wheel alone does nothing\n\
             \x20 arrows     scrub the clock (up/down are the fast ones)\n\
             \x20 P          pause the clock H      toggle this overlay\n\
             \x20 ESC        close the menu, release the mouse, then quit\n"
        );
    }
}

#[derive(Component)]
pub struct HudText;

pub fn spawn_hud(mut commands: Commands) {
    commands.spawn((
        HudText,
        Text::default(),
        TextFont {
            font_size: bevy::text::FontSize::Px(13.0),
            ..default()
        },
        Node {
            position_type: PositionType::Absolute,
            top: px(8.0),
            left: px(10.0),
            ..default()
        },
    ));
}

pub fn update_hud(
    hud: Res<HudState>,
    menu: Res<SettingsMenu>,
    diagnostics: Res<DiagnosticsStore>,
    mut text: Query<(&mut Text, &mut Node), With<HudText>>,
) {
    let Ok((mut t, mut node)) = text.single_mut() else {
        return;
    };
    // Hidden behind the settings menu, which carries its own fps readout. Two
    // overlapping copies of the same number is what v2 avoided by only ever
    // drawing one panel.
    let show = hud.visible && !menu.open;
    node.display = if show { Display::Flex } else { Display::None };
    if !show {
        return;
    }

    let fps = diagnostics
        .get(&FrameTimeDiagnosticsPlugin::FPS)
        .and_then(|d| d.smoothed())
        .unwrap_or(0.0);

    // JUST THE FRAME RATE. This carried four lines of clock, sun angles,
    // position, mode and chunk counts, and all of it was debug output living
    // permanently in the corner of a game. What is genuinely useful while
    // playing is the frame rate; everything else that was here is either in the
    // settings menu, which is where you go when you care, or was only ever
    // interesting to whoever was building the thing.
    t.0 = format!("{fps:.0} fps");
}
