//! The mouse cursor is the game's crosshair (user 2026-09-23: "for the
//! cursor, copy the cross from the game engine"). Same plus, same sizes, same
//! trick -- see engine/shaders/Crosshair.ps.slang and drawCrosshair() in
//! engine/src/ui/app_hud.inl:
//!
//! * IT INVERTS WHAT IS UNDER IT instead of being drawn in a colour, so it
//!   is visible on the white page, on the dark text and on the green or
//!   orange line alike. A blend of OneMinusDst against a white source is
//!   `1 - dst`: on eight-bit channels, a bitwise NOT of the page.
//! * 32 px across and 4 px thick, scaled only in WHOLE steps so the bars
//!   never land half on a pixel and go grey. The engine steps on 1080-line
//!   multiples of the frame; a desktop window steps on the display's scale.
//!
//! egui can only paint in colours, so the cross is its own tiny wgpu pass,
//! run through egui's paint-callback hook on the top layer.

use eframe::egui::{self, Rect, pos2, vec2};
use eframe::egui_wgpu::{self, wgpu};

struct CrossPipeline(wgpu::RenderPipeline);

// Three rects in the callback's viewport, which egui sets to the cross's own
// box: NDC -1..1 is the full 32-px span, so the 2-px half-thickness is 1/8.
// The vertical bar is TWO stubs, above and below the horizontal one, never
// crossing it: inverting the centre twice would put the page back and punch
// a hole through the middle -- the bug the engine's v2 had under XOR.
const SHADER: &str = r#"
const H: f32 = 0.125;

@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    var rects = array<vec4<f32>, 3>(
        vec4<f32>(-1.0, -H, 1.0, H),
        vec4<f32>(-H, H, H, 1.0),
        vec4<f32>(-H, -1.0, H, -H),
    );
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0),
    );
    let r = rects[i / 6u];
    return vec4<f32>(mix(r.xy, r.zw, corners[i % 6u]), 0.0, 1.0);
}

@fragment
fn fs() -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
"#;

/// `1 * (1 - dst) + 0 * dst`, colour only; the page's alpha is untouched.
const INVERT: wgpu::BlendState = wgpu::BlendState {
    color: wgpu::BlendComponent {
        src_factor: wgpu::BlendFactor::OneMinusDst,
        dst_factor: wgpu::BlendFactor::Zero,
        operation: wgpu::BlendOperation::Add,
    },
    alpha: wgpu::BlendComponent::REPLACE,
};

/// Build the pass once, against the window's surface format. Returns false
/// with no GPU state to build it on, and then the system cursor stays.
pub fn install(rs: Option<&egui_wgpu::RenderState>) -> bool {
    let Some(rs) = rs else { return false };
    let pipeline = pipeline(&rs.device, rs.target_format);
    rs.renderer.write().callback_resources.insert(CrossPipeline(pipeline));
    true
}

/// The pass itself: 18 vertices, no buffers, no bindings. Public so
/// tests/cross.rs can run the very same pipeline on a headless device.
pub fn pipeline(device: &wgpu::Device, format: wgpu::TextureFormat) -> wgpu::RenderPipeline {
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("cross"),
        source: wgpu::ShaderSource::Wgsl(SHADER.into()),
    });
    let layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("cross"),
        bind_group_layouts: &[],
        immediate_size: 0,
    });
    device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("cross"),
        layout: Some(&layout),
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("vs"),
            compilation_options: Default::default(),
            buffers: &[],
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("fs"),
            compilation_options: Default::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format,
                blend: Some(INVERT),
                write_mask: wgpu::ColorWrites::COLOR,
            })],
        }),
        multiview_mask: None,
        cache: None,
    })
}

struct Draw;

impl egui_wgpu::CallbackTrait for Draw {
    fn paint(
        &self,
        _info: egui::PaintCallbackInfo,
        render_pass: &mut wgpu::RenderPass<'static>,
        resources: &egui_wgpu::CallbackResources,
    ) {
        // Absent under the offscreen test renderer, which never installs it.
        if let Some(CrossPipeline(pipeline)) = resources.get::<CrossPipeline>() {
            render_pass.set_pipeline(pipeline);
            render_pass.draw(0..18, 0..1);
        }
    }
}

/// Draw the cross at the pointer and hide the system cursor, over the whole
/// window -- tabs, fields and chart alike, as a crosshair has no other shapes.
/// Call last in the frame: widgets set their own cursor while they are drawn,
/// and the last call wins.
pub fn show(ctx: &egui::Context) {
    let Some(p) = ctx.input(|i| i.pointer.hover_pos()) else { return };
    let ppp = ctx.pixels_per_point();
    let scale = ppp.floor().max(1.0);
    let arm = 16.0 * scale;
    // The centre on a pixel CORNER, so the 4-px bars cover two whole pixels
    // either side of the hotspot; the box on whole pixels, so egui's viewport
    // for it is exact and no bar is resampled.
    let (cx, cy) = ((p.x * ppp).round(), (p.y * ppp).round());
    let rect = Rect::from_min_size(pos2((cx - arm) / ppp, (cy - arm) / ppp), vec2(2.0 * arm / ppp, 2.0 * arm / ppp));
    let layer = egui::LayerId::new(egui::Order::Debug, egui::Id::new("crosshair"));
    ctx.layer_painter(layer).add(egui_wgpu::Callback::new_paint_callback(rect, Draw));
    ctx.set_cursor_icon(egui::CursorIcon::None);
}
