//! The crosshair's GPU pass, run on a headless device and read back -- the
//! only way to know the shader compiles and the blend inverts without opening
//! the window. A mistake here would otherwise crash the wallet at launch.
//!
//!     cargo test --release --test cross -- --ignored

use eframe::egui_wgpu::wgpu;

const SIZE: u32 = 64;
/// The page colour under the cross: something whose inverse is unmistakable.
const PAGE: [u8; 4] = [51, 102, 153, 255];

#[test]
#[ignore = "needs a GPU"]
fn the_cross_inverts_a_plus_and_nothing_else() {
    let instance = wgpu::Instance::default();
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default())).expect("adapter");
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default())).expect("device");

    let format = wgpu::TextureFormat::Rgba8Unorm;
    let pipeline = voxelbit_wallet::cross::pipeline(&device, format);
    let texture = device.create_texture(&wgpu::TextureDescriptor {
        label: None,
        size: wgpu::Extent3d { width: SIZE, height: SIZE, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    });
    let view = texture.create_view(&Default::default());
    let readback = device.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: (SIZE * SIZE * 4) as u64,
        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        mapped_at_creation: false,
    });

    let mut encoder = device.create_command_encoder(&Default::default());
    {
        let c = |v: u8| v as f64 / 255.0;
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: None,
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations {
                    load: wgpu::LoadOp::Clear(wgpu::Color { r: c(PAGE[0]), g: c(PAGE[1]), b: c(PAGE[2]), a: 1.0 }),
                    store: wgpu::StoreOp::Store,
                },
            })],
            ..Default::default()
        });
        // What egui does before the callback: the viewport is the cross's
        // 32-px box, here centred on the pixel corner (32, 32).
        pass.set_viewport(16.0, 16.0, 32.0, 32.0, 0.0, 1.0);
        pass.set_pipeline(&pipeline);
        pass.draw(0..18, 0..1);
    }
    encoder.copy_texture_to_buffer(
        texture.as_image_copy(),
        wgpu::TexelCopyBufferInfo {
            buffer: &readback,
            layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(SIZE * 4), rows_per_image: None },
        },
        wgpu::Extent3d { width: SIZE, height: SIZE, depth_or_array_layers: 1 },
    );
    queue.submit([encoder.finish()]);
    readback.slice(..).map_async(wgpu::MapMode::Read, |r| r.expect("map"));
    device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
    let px = readback.slice(..).get_mapped_range().expect("mapped").to_vec();

    let at = |x: u32, y: u32| {
        let i = ((y * SIZE + x) * 4) as usize;
        [px[i], px[i + 1], px[i + 2]]
    };
    let inverted = [255 - PAGE[0], 255 - PAGE[1], 255 - PAGE[2]];
    let page = [PAGE[0], PAGE[1], PAGE[2]];

    // The plus, exactly: bars 4 px thick (two either side of the corner at
    // 32) and 32 px long (sixteen either side).
    let mut count = 0;
    for y in 0..SIZE {
        for x in 0..SIZE {
            let on_h = (16..48).contains(&x) && (30..34).contains(&y);
            let on_v = (16..48).contains(&y) && (30..34).contains(&x);
            let want = if on_h || on_v { inverted } else { page };
            assert_eq!(at(x, y), want, "pixel ({x},{y})");
            count += (on_h || on_v) as u32;
        }
    }
    // 32x4 + 32x4 - the 4x4 they share: the centre inverted ONCE, not twice.
    assert_eq!(count, 240);
    assert_eq!(at(32, 32), inverted, "no hole where the bars cross");
    println!("cross: 240 pixels inverted, centre solid, page untouched");
}
