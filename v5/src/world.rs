// ---------------------------------------------------------------------------
// world.rs -- the shared materials, the sky, and the sun.
//
// ---------------------------------------------------------------------------
// WHY THERE IS NO DirectionalLight IN THIS ENGINE
// ---------------------------------------------------------------------------
// This is the one place where Solari's design forced a real departure from v2,
// and it is worth writing down because the obvious implementation is broken in
// a way that does not look like a bug.
//
// Solari lights a scene from two things: `DirectionalLight`s, which it samples
// analytically as a disk of the right angular size, and EMISSIVE TRIANGLES. It
// has no environment map, no sky model and no miss shader -- a ray that escapes
// the world contributes exactly zero. v2 evaluated Preetham in its OptiX miss
// program, and on a forest floor that sky term is most of the light: the canopy
// blocks the sun over nearly all of the ground. Ported without a sky, this
// world would have black shadows, a black canopy interior, and nothing at all
// at dusk.
//
// So the sky is GEOMETRY: a shell around the camera whose emissive map is the
// Preetham fit, re-baked whenever the sun moves. Solari picks it up as an area
// light through the same path as any emissive mesh.
//
// AND THAT SHELL CANNOT COEXIST WITH A DirectionalLight. Solari's shadow ray
// for a directional light runs from the surface to RAY_T_MAX -- 100 km -- with
// the cull mask wide open (`trace_visibility` in sampling.wesl). Any geometry
// in the way blocks it, and a shell around the camera is in the way of every
// ray that leaves it. A sun light and a sky shell together give a permanently
// eclipsed sun. Moving the shell beyond 100 km does not help either: the same
// constant bounds the indirect rays, so a shell out there is unreachable and
// lights nothing.
//
// The resolution is to stop treating the sun as a special case: it is a disk of
// sky, so it is emissive geometry too -- `sun_disk_mesh` below, sized to the
// real 0.53 degrees and carrying the radiance the Preetham fit already computes
// for it. Solari picks a light source UNIFORMLY (`generate_random_light_sample`),
// so with exactly two of them the sun and the sky each get half of every
// pixel's direct-lighting samples, and the sun's half always lands on the disk
// because that mesh is nothing but the disk. That is a better split than a
// third alternative -- baking the sun into the shell's texture -- would give:
// there the sun is 5 parts per million of the shell's area and would be found
// by chance roughly never.
// ---------------------------------------------------------------------------

use bevy::asset::RenderAssetUsages;
use bevy::camera::Camera3d;
use bevy::image::{Image, ImageSampler};
use bevy::mesh::{Indices, Mesh, PrimitiveTopology};
use bevy::pbr::{MeshMaterial3d, StandardMaterial};
use bevy::prelude::*;
use bevy::render::render_resource::{Extent3d, TextureDimension, TextureFormat};
use bevy::solari::prelude::RaytracingMesh3d;

use crate::scene::daynight::DayNight;
use crate::scene::palette::Palette;
use crate::scene::scatter::{tint_color, TINT_VARIANTS};
use crate::scene::sky::{Sky, DOME_RADIUS_M, SKY_TEX_H, SKY_TEX_W, SUN_ANGULAR_SIZE_RAD};

/// The sun disk sits just inside the shell, so a shadow ray toward it is never
/// stopped by the sky behind it.
const SUN_DIST_M: f32 = DOME_RADIUS_M * 0.98;

/// The shell's tessellation. Rings are spaced by EQUAL AREA rather than equal
/// angle, which does two good things at once: every quad has the same area, so
/// Solari's uniform-over-triangles light sampling is also uniform over solid
/// angle; and the rings bunch up near the horizon, which is exactly where the
/// Preetham gradient is steepest and where a coarse dome would band.
const DOME_RINGS: usize = 64;
const DOME_SEGMENTS: usize = 128;

#[derive(Resource)]
pub struct WorldMaterials {
    /// The shared palette material -- terrain, rocks and flowers all use it.
    pub terrain: Handle<StandardMaterial>,
    /// The same material in `TINT_VARIANTS` hues, for the pines.
    pub tints: Vec<Handle<StandardMaterial>>,
    pub sky: Handle<StandardMaterial>,
    pub sun: Handle<StandardMaterial>,
    /// Reflective rather than refractive -- see `build_materials`.
    pub water: Handle<StandardMaterial>,
    pub sky_image: Handle<Image>,
}

/// Tags the two entities that follow the camera.
#[derive(Component)]
pub struct SkyDome;

#[derive(Component)]
pub struct SunDisk;

/// The live sky fit, and the clock driving it.
#[derive(Resource, Default)]
pub struct SkyState {
    pub sky: Sky,
    /// The sun elevation the emissive map was last baked at. Re-baking is cheap
    /// but not free, and at 1x a day takes twenty minutes -- so it is done on a
    /// movement threshold rather than every frame.
    baked_el: f32,
    baked_az: f32,
    pub first_bake: bool,
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

pub fn build_materials(
    palette: &Palette,
    materials: &mut Assets<StandardMaterial>,
    images: &mut Assets<Image>,
) -> WorldMaterials {
    let base = palette_image(
        palette.base_color_bytes(),
        TextureFormat::Rgba8UnormSrgb,
        images,
    );
    let mr = palette_image(
        palette.metallic_roughness_bytes(),
        TextureFormat::Rgba8Unorm,
        images,
    );

    // reflectance 0.5 is F0 = 0.04, the dielectric default. v2 carried a
    // per-material specular of 0.020..0.045 around this value; Solari's
    // reflectance is a scalar with no texture, and the spread it replaces is
    // narrow enough that no pixel in this scene can show the difference.
    let make = |tint: Vec3, materials: &mut Assets<StandardMaterial>| {
        materials.add(StandardMaterial {
            base_color: Color::linear_rgb(tint.x, tint.y, tint.z),
            base_color_texture: Some(base.clone()),
            metallic_roughness_texture: Some(mr.clone()),
            perceptual_roughness: 1.0, // the texture's G is the whole of it
            metallic: 0.0,
            reflectance: 0.5,
            ..default()
        })
    };

    let terrain = make(Vec3::ONE, materials);
    let tints = (0..TINT_VARIANTS)
        .map(|k| make(tint_color(k), materials))
        .collect();

    // The sky and sun materials carry emissive only. base_color is black so
    // nothing bounces off them -- they are lights, not surfaces.
    //
    // NOT `unlit`. It would be the obvious flag to set on something that is
    // pure emission, but Solari's primary visibility comes from the DEFERRED
    // G-buffer, and an unlit material is a forward-rendered special case there.
    // Black base colour already buys everything unlit would: nothing is
    // reflected off a surface that absorbs all of it.
    let sky_image = images.add(sky_image_placeholder());
    let sky = materials.add(StandardMaterial {
        base_color: Color::BLACK,
        emissive: LinearRgba::WHITE, // the texture is the whole of the value
        emissive_texture: Some(sky_image.clone()),
        ..default()
    });
    let sun = materials.add(StandardMaterial {
        base_color: Color::BLACK,
        emissive: LinearRgba::BLACK, // filled in by `update_sky`
        ..default()
    });

    // -----------------------------------------------------------------------
    // Water.
    //
    // v2 shaded it as a true dielectric: IOR 1.333, roughness 0, with a teal
    // attenuation through the body of it. Solari's material has no transmission
    // term at all -- base colour, roughness, metallic, emissive and a scalar
    // reflectance is the whole of it -- so refraction is not available and this
    // is a REFLECTIVE surface instead of a refractive one.
    //
    // That is less of a loss than it sounds for a tarn in a wood. Almost
    // everything you see in still water is the sky and the treeline reflected
    // off it, because the Fresnel term at the grazing angles a lake is viewed
    // from is near total; what refraction buys is the bottom, which at this
    // depth and through v2's teal is nearly black anyway. So: a very dark base
    // colour carrying that teal, near-mirror roughness, and reflectance 0.354 --
    // which is Bevy's parameterisation (F0 = 0.16 * r^2) of water's real F0 of
    // 0.02.
    let water_normal = images.add(ripple_normal_map());
    let water = materials.add(StandardMaterial {
        base_color: Color::linear_rgb(0.008, 0.020, 0.017),
        perceptual_roughness: 0.06,
        metallic: 0.0,
        reflectance: 0.354,
        normal_map_texture: Some(water_normal),
        ..default()
    });

    WorldMaterials {
        terrain,
        tints,
        sky,
        sun,
        water,
        sky_image,
    }
}

/// How many metres of world the ripple texture spans before it repeats.
pub const WATER_TILE_M: f32 = 32.0;

/// v2's ripple, baked into a tiling tangent-space normal map.
///
/// The shape is a straight port: two CROSSED wave trains rather than one, so it
/// does not read as corduroy, at v2's amplitude of 0.014. What had to change is
/// that v2 evaluated this per hit from the world position, with whatever
/// frequencies looked right (1.7, 0.9, 0.6, 2.1 ...). A texture has to REPEAT,
/// and those frequencies are not commensurate with any tile, so each is rounded
/// to the nearest whole number of cycles across the tile. The largest nudge is
/// under 4% -- far too small to change how the water looks, and the difference
/// between a seamless lake and a visible 32 m grid across it.
fn ripple_normal_map() -> Image {
    const N: usize = 256;
    // Whole cycles per tile, nearest to v2's 1.7, 0.9, 0.6, 2.1 / 1.9, 0.4, 0.8, 1.5.
    const H: [f32; 8] = [9.0, 5.0, 3.0, 11.0, 10.0, 2.0, 4.0, 8.0];
    let k = std::f32::consts::TAU / WATER_TILE_M;

    let mut data = Vec::with_capacity(N * N * 4);
    for j in 0..N {
        let z = (j as f32 + 0.5) / N as f32 * WATER_TILE_M;
        for i in 0..N {
            let x = (i as f32 + 0.5) / N as f32 * WATER_TILE_M;
            let w1 =
                (x * H[0] * k + z * H[1] * k).sin() + (x * H[2] * k - z * H[3] * k).sin() * 0.7;
            let w2 =
                (z * H[4] * k - x * H[5] * k).cos() + (z * H[6] * k + x * H[7] * k).cos() * 0.7;
            // Tangent space is (T, B, N) = (+X, +Z, +Y) in world, so v2's
            // world-space (w1 * 0.014, 1, w2 * 0.014) becomes this.
            let n = Vec3::new(w1 * 0.014, w2 * 0.014, 1.0).normalize();
            data.push(((n.x * 0.5 + 0.5) * 255.0 + 0.5) as u8);
            data.push(((n.y * 0.5 + 0.5) * 255.0 + 0.5) as u8);
            data.push(((n.z * 0.5 + 0.5) * 255.0 + 0.5) as u8);
            data.push(255);
        }
    }

    let mut img = Image::new(
        Extent3d {
            width: N as u32,
            height: N as u32,
            depth_or_array_layers: 1,
        },
        TextureDimension::D2,
        data,
        // Linear, NOT sRGB: these are vector components, and decoding them as
        // colour would bend every one of them toward the surface normal.
        TextureFormat::Rgba8Unorm,
        RenderAssetUsages::all(),
    );
    img.sampler = ImageSampler::linear();
    img
}

/// A 255x1 palette strip. NEAREST on both axes and no mip chain, so the UV a
/// vertex carries fetches its own material id and nothing next to it -- see
/// palette.rs for why the id travels in the UV at all.
fn palette_image(
    data: Vec<u8>,
    format: TextureFormat,
    images: &mut Assets<Image>,
) -> Handle<Image> {
    let mut img = Image::new(
        Extent3d {
            width: crate::scene::palette::COUNT as u32,
            height: 1,
            depth_or_array_layers: 1,
        },
        TextureDimension::D2,
        data,
        format,
        RenderAssetUsages::RENDER_WORLD,
    );
    img.sampler = ImageSampler::nearest();
    images.add(img)
}

fn sky_image_placeholder() -> Image {
    let mut img = Image::new(
        Extent3d {
            width: SKY_TEX_W as u32,
            height: SKY_TEX_H as u32,
            depth_or_array_layers: 1,
        },
        TextureDimension::D2,
        vec![0u8; SKY_TEX_W * SKY_TEX_H * 8],
        // Rgba16Float, not 8-bit: this texture IS the light. Sky radiance spans
        // four orders of magnitude between noon and twenty minutes after
        // sunset, and an 8-bit map would quantise dusk into a few flat steps
        // and then to black -- visible as banding, and worse as a fill light
        // that snaps rather than fades. Rgba16Float is filterable on every
        // backend, which Rgba32Float is not.
        TextureFormat::Rgba16Float,
        RenderAssetUsages::all(),
    );
    img.sampler = ImageSampler::linear();
    img
}

// ---------------------------------------------------------------------------
// The two pieces of sky geometry
// ---------------------------------------------------------------------------

/// The shell. Normals point INWARD: `cos_theta_light` in Solari's light
/// sampling is `dot(-wi, world_normal)`, and -wi points from the light back
/// down at the world, so an outward-facing dome would emit nothing at all.
pub fn dome_mesh() -> Mesh {
    let mut position = Vec::with_capacity((DOME_RINGS + 1) * (DOME_SEGMENTS + 1));
    let mut normal = Vec::with_capacity(position.capacity());
    let mut uv = Vec::with_capacity(position.capacity());

    for r in 0..=DOME_RINGS {
        // Equal area: cos(theta) uniform from +1 (zenith) to -1 (nadir).
        let cos_theta = 1.0 - 2.0 * (r as f32 / DOME_RINGS as f32);
        let theta = cos_theta.clamp(-1.0, 1.0).acos();
        let sin_theta = theta.sin();
        for s in 0..=DOME_SEGMENTS {
            // The seam column is duplicated so u can reach 1.0 there rather
            // than wrapping to 0 across one quad, which would smear the whole
            // texture backwards over it.
            let u = s as f32 / DOME_SEGMENTS as f32;
            let phi = u * std::f32::consts::TAU;
            // Matches Sky::bake exactly. If these two ever disagree the sky
            // rotates against the sun and nothing else looks wrong.
            let dir = Vec3::new(sin_theta * phi.sin(), cos_theta, -sin_theta * phi.cos());
            position.push((dir * DOME_RADIUS_M).to_array());
            normal.push((-dir).to_array());
            uv.push([u, theta / std::f32::consts::PI]);
        }
    }

    let stride = DOME_SEGMENTS + 1;
    let mut index: Vec<u32> = Vec::with_capacity(DOME_RINGS * DOME_SEGMENTS * 6);
    for r in 0..DOME_RINGS {
        for s in 0..DOME_SEGMENTS {
            let a = (r * stride + s) as u32;
            let b = a + 1;
            let c = ((r + 1) * stride + s) as u32;
            let d = c + 1;
            // The pole rings collapse to a point, so one triangle of each quad
            // there is degenerate. Skipping them keeps zero-area triangles out
            // of the light list, where they would be sampled as often as any
            // other and contribute nothing.
            if r > 0 {
                index.extend_from_slice(&[a, c, b]);
            }
            if r + 1 < DOME_RINGS {
                index.extend_from_slice(&[b, c, d]);
            }
        }
    }

    let n = position.len();
    Mesh::new(PrimitiveTopology::TriangleList, RenderAssetUsages::default())
        .with_inserted_attribute(Mesh::ATTRIBUTE_POSITION, position)
        .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, normal)
        .with_inserted_attribute(Mesh::ATTRIBUTE_UV_0, uv)
        .with_inserted_attribute(Mesh::ATTRIBUTE_TANGENT, vec![[1.0f32, 0.0, 0.0, 1.0]; n])
        .with_inserted_indices(Indices::U32(index))
}

/// The solar disk, in the XY plane with its normal along -Z, so that
/// `Transform::looking_at(camera)` -- which points an entity's -Z at its target
/// -- turns the lit face toward the world.
///
/// A polygon rather than a quad, because the radius is chosen to give the disk
/// the SOLID ANGLE the sun actually subtends and a square would overshoot it by
/// 4/pi at the corners. 24 sides is within a tenth of a percent of a circle.
pub fn sun_disk_mesh() -> Mesh {
    const SIDES: usize = 24;
    // Solid angle of a disk of radius r at distance d is pi r^2 / d^2, and the
    // sun's is 2 pi (1 - cos(half angle)).
    let half = SUN_ANGULAR_SIZE_RAD * 0.5;
    let solid_angle = std::f32::consts::TAU * (1.0 - half.cos());
    let radius = SUN_DIST_M * (solid_angle / std::f32::consts::PI).sqrt();

    let mut position = vec![[0.0f32, 0.0, 0.0]];
    for i in 0..SIDES {
        let a = i as f32 / SIDES as f32 * std::f32::consts::TAU;
        position.push([radius * a.cos(), radius * a.sin(), 0.0]);
    }
    let mut index: Vec<u32> = Vec::with_capacity(SIDES * 3);
    for i in 0..SIDES {
        // Wound so the face is front-facing from -Z, matching the normal.
        index.extend_from_slice(&[0, 1 + ((i + 1) % SIDES) as u32, 1 + i as u32]);
    }

    let n = position.len();
    Mesh::new(PrimitiveTopology::TriangleList, RenderAssetUsages::default())
        .with_inserted_attribute(Mesh::ATTRIBUTE_POSITION, position)
        .with_inserted_attribute(Mesh::ATTRIBUTE_NORMAL, vec![[0.0f32, 0.0, -1.0]; n])
        .with_inserted_attribute(Mesh::ATTRIBUTE_UV_0, vec![[0.5f32, 0.5]; n])
        .with_inserted_attribute(Mesh::ATTRIBUTE_TANGENT, vec![[1.0f32, 0.0, 0.0, 1.0]; n])
        .with_inserted_indices(Indices::U32(index))
}

/// The sun's solid angle, for turning an irradiance into the disk's radiance.
pub fn sun_solid_angle() -> f32 {
    std::f32::consts::TAU * (1.0 - (SUN_ANGULAR_SIZE_RAD * 0.5).cos())
}

pub fn spawn_sky(
    commands: &mut Commands,
    meshes: &mut Assets<Mesh>,
    mats: &WorldMaterials,
) {
    // Mesh3d as well as RaytracingMesh3d, for the same reason the terrain
    // carries both: without it the sky is a light nobody can see.
    let dome = meshes.add(dome_mesh());
    commands.spawn((
        SkyDome,
        Mesh3d(dome.clone()),
        RaytracingMesh3d(dome),
        MeshMaterial3d(mats.sky.clone()),
        Transform::IDENTITY,
    ));
    let disk = meshes.add(sun_disk_mesh());
    commands.spawn((
        SunDisk,
        Mesh3d(disk.clone()),
        RaytracingMesh3d(disk),
        MeshMaterial3d(mats.sun.clone()),
        Transform::IDENTITY,
    ));
}

// ---------------------------------------------------------------------------
// Per-frame upkeep
// ---------------------------------------------------------------------------

/// Run the clock and, when the sun has moved far enough to matter, re-fit the
/// sky and re-bake its emissive map.
pub fn update_sky(
    time: Res<Time>,
    mut clock: ResMut<DayNight>,
    mut state: ResMut<SkyState>,
    mats: Res<WorldMaterials>,
    mut materials: ResMut<Assets<StandardMaterial>>,
    mut images: ResMut<Assets<Image>>,
) {
    clock.advance(time.delta_secs());

    let el = clock.elevation_deg();
    let az = clock.azimuth_deg();

    // A tenth of a degree is well under the width of the sun itself, so the
    // sky never visibly steps -- and at 1x that is a re-bake about every four
    // seconds rather than sixty times a second.
    if state.first_bake
        && (el - state.baked_el).abs() < 0.1
        && (az - state.baked_az).abs() < 0.1
    {
        return;
    }
    state.first_bake = true;
    state.baked_el = el;
    state.baked_az = az;

    state.sky.set_sun(az, el);

    if let Some(mut img) = images.get_mut(&mats.sky_image) {
        img.data = Some(state.sky.bake());
    }

    if let Some(mut m) = materials.get_mut(&mats.sun) {
        // The fit produces an irradiance; the disk needs a radiance, which is
        // that divided by the solid angle it is spread over. v2 did the same
        // division for its cone sampler.
        let e = state.sky.sun_illuminance() / sun_solid_angle();
        m.emissive = LinearRgba::rgb(e.x, e.y, e.z);
    }
}

/// Keep the shell and the disk centred on the camera. The world is endless, so
/// there is no fixed point to hang a sky on.
///
/// This also gives DLSS Ray Reconstruction the right motion vectors for the
/// sky, and by accident rather than design: a shell that translates with the
/// camera has exactly the camera's world motion, so its SCREEN motion under a
/// pure translation is zero -- which is what a sky at infinity does.
pub fn follow_camera(
    camera: Query<&GlobalTransform, With<Camera3d>>,
    state: Res<SkyState>,
    mut dome: Query<&mut Transform, (With<SkyDome>, Without<SunDisk>)>,
    mut sun: Query<&mut Transform, (With<SunDisk>, Without<SkyDome>)>,
) {
    let Ok(cam) = camera.single() else {
        return;
    };
    let p = cam.translation();

    for mut t in &mut dome {
        t.translation = p;
    }
    for mut t in &mut sun {
        let at = p + state.sky.sun_dir() * SUN_DIST_M;
        *t = Transform::from_translation(at).looking_at(p, Vec3::Y);
    }
}
