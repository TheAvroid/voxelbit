// ---------------------------------------------------------------------------
// sky.rs -- Preetham's analytic daylight model, baked into an emissive dome.
//
// WHY AN ANALYTIC SKY AND NOT A CONSTANT: almost all of the light reaching the
// floor of a wood is sky light, not sun -- the canopy blocks the sun over most
// of the ground. Sky light is strongly directional (bright near the horizon in
// haze, deep blue at zenith) and strongly coloured, and a constant-colour dome
// loses the blue fill on the shaded side of every trunk, which is a large part
// of why an outdoor render looks outdoor.
//
// ---------------------------------------------------------------------------
// THE ONE REAL ARCHITECTURAL DIFFERENCE BETWEEN v5 AND v2
// ---------------------------------------------------------------------------
// v2 evaluated Preetham in its OptiX miss program: a ray that escaped the world
// asked the sky what colour it was, and that was the fill light. Bevy Solari
// has no miss shader and no environment light of any kind -- its light list is
// directional lights plus EMISSIVE TRIANGLES, and a ray that hits nothing
// contributes exactly zero. Ported naively, this world would be lit by the sun
// alone: black shadows, a black canopy interior, and nothing at all at dusk.
//
// So the sky is built as GEOMETRY. A sphere around the camera, one material,
// whose emissive map is an equirectangular image of the Preetham fit that is
// re-baked whenever the sun moves. Solari picks it up as an area light through
// the same path it picks up any emissive mesh, and `resolve_material` samples
// the emissive TEXTURE when it resolves a light-sampled triangle -- so the
// gradient is not merely visible, it is what the next-event estimator actually
// integrates.
//
// WHY A TEXTURE RATHER THAN MANY MATERIALS. The obvious alternative is a dome
// split into patches with a flat emissive each. That works, but the light list
// then holds one entry per patch and the visible sky bands at every seam;
// getting a smooth horizon needs hundreds of materials to update per frame. One
// material with a 256x128 texture is 128 KB, one light source, and no bands.
//
// WHY IT MOVES WITH THE CAMERA. The world is endless, so there is no fixed
// point to hang a dome on. It follows the camera at a radius far outside the
// resident ring; a tree at the ring's edge therefore sees the dome slightly
// off-centre, which at 1200 m against a 100 m ring is a percent or two of
// asymmetry in a fill term. Nothing about the picture can show that.
//
// THE SUN IS NOT IN THE DOME. It is a real `DirectionalLight`, because Solari
// samples those analytically as a disk of the right angular size -- which is
// both far less noisy than finding a small bright patch of dome by chance, and
// the thing that produces the soft-edged branch shadows this scene lives on.
// The dome's own texture therefore carries the sky WITHOUT the solar disk, or
// the sun would be counted twice.
// ---------------------------------------------------------------------------

use bevy::math::Vec3;
use std::f32::consts::PI;

/// The sun subtends about 0.53 degrees.
pub const SUN_ANGULAR_SIZE_RAD: f32 = 0.53 * PI / 180.0;

/// How far out the dome sits, in metres. Comfortably beyond any resident ring,
/// comfortably inside the far plane.
pub const DOME_RADIUS_M: f32 = 1200.0;

/// The equirectangular emissive map. 256x128 is finer than the Preetham field
/// has structure -- the sharpest thing in it is the horizon gradient, which is
/// a couple of degrees wide -- and it re-bakes in well under a millisecond.
pub const SKY_TEX_W: usize = 256;
pub const SKY_TEX_H: usize = 128;

// ---------------------------------------------------------------------------
// The fit
// ---------------------------------------------------------------------------
pub struct Sky {
    /// 2 is a clear alpine day; 6 is summer haze.
    pub turbidity: f32,
    pub sun_scale: f32,

    sun_dir: Vec3,
    az_deg: f32,
    el_deg: f32,

    a: [f32; 3],
    b: [f32; 3],
    c: [f32; 3],
    d: [f32; 3],
    e: [f32; 3],
    /// Y in cd/m^2, then x and y.
    zenith: [f32; 3],
    norm_f: [f32; 3],
    sky_scale: f32,
    ground_albedo: Vec3,
    sun_radiance: Vec3,
}

impl Default for Sky {
    fn default() -> Self {
        let mut s = Self {
            turbidity: 2.6,
            sun_scale: 1.0,
            sun_dir: Vec3::Y,
            az_deg: 38.0,
            el_deg: 24.0,
            a: [0.0; 3],
            b: [0.0; 3],
            c: [0.0; 3],
            d: [0.0; 3],
            e: [0.0; 3],
            zenith: [0.0; 3],
            norm_f: [1.0; 3],
            sky_scale: 0.0,
            ground_albedo: Vec3::new(0.16, 0.13, 0.10),
            sun_radiance: Vec3::ZERO,
        };
        s.set_sun(38.0, 24.0);
        s
    }
}

impl Sky {
    pub fn set_sun(&mut self, azimuth_deg: f32, elevation_deg: f32) {
        self.az_deg = azimuth_deg;
        self.el_deg = elevation_deg;
        let az = azimuth_deg.to_radians();
        let el = elevation_deg.to_radians();
        self.sun_dir = Vec3::new(el.cos() * az.cos(), el.sin(), el.cos() * az.sin()).normalize();
        self.build();
    }

    pub fn sun_dir(&self) -> Vec3 {
        self.sun_dir
    }

    pub fn elevation_deg(&self) -> f32 {
        self.el_deg
    }

    pub fn azimuth_deg(&self) -> f32 {
        self.az_deg
    }

    /// Irradiance of the solar disk, for the `DirectionalLight`. Bevy takes
    /// illuminance in lux and divides by the disk's solid angle itself, so this
    /// is the value BEFORE that division -- unlike v2, which handed OptiX a
    /// radiance and did the division by hand.
    pub fn sun_illuminance(&self) -> Vec3 {
        self.sun_radiance
    }

    fn build(&mut self) {
        let t = self.turbidity;
        self.ground_albedo = Vec3::new(0.16, 0.13, 0.10);

        // NIGHT IS A DIMMED HORIZON FIT, NOT AN EXTRAPOLATION.
        //
        // Preetham is fitted for a sun above the horizon. Below it the zenith
        // luminance term runs tan(chi) straight through its pole and the model
        // returns negative radiance -- so the fit is pinned at the horizon and
        // the whole dome is scaled down instead. The 2% floor is what stops
        // night being pure black, which is useless to look at and worse to
        // navigate; it reads as moonlight without pretending to model one.
        let el_deg = self.sun_dir.y.clamp(-1.0, 1.0).asin().to_degrees();
        let daylight = crate::core::sstep(crate::core::saturate((el_deg + 6.0) / 10.0));
        self.sky_scale = 0.00018 * crate::core::lerpf(0.02, 1.0, daylight);

        let theta_s = self.sun_dir.y.max(0.0).clamp(-1.0, 1.0).acos();

        // Distribution coefficients, per Preetham et al. 1999, table 1.
        self.a = [
            0.1787 * t - 1.4630,
            -0.0193 * t - 0.2592,
            -0.0167 * t - 0.2608,
        ];
        self.b = [
            -0.3554 * t + 0.4275,
            -0.0665 * t + 0.0008,
            -0.0950 * t + 0.0092,
        ];
        self.c = [
            -0.0227 * t + 5.3251,
            -0.0004 * t + 0.2125,
            -0.0079 * t + 0.2102,
        ];
        self.d = [
            0.1206 * t - 2.5771,
            -0.0641 * t - 0.8989,
            -0.0441 * t - 1.6537,
        ];
        self.e = [
            -0.0670 * t + 0.3703,
            -0.0033 * t + 0.0452,
            -0.0109 * t + 0.0529,
        ];

        let ts = theta_s;
        let ts2 = ts * ts;
        let ts3 = ts2 * ts;

        let chi = (4.0 / 9.0 - t / 120.0) * (PI - 2.0 * ts);
        self.zenith[0] =
            (((4.0453 * t - 4.9710) * chi.tan() - 0.2155 * t + 2.4192) * 1000.0).max(1.0);
        self.zenith[1] = (0.00166 * ts3 - 0.00375 * ts2 + 0.00209 * ts) * t * t
            + (-0.02903 * ts3 + 0.06377 * ts2 - 0.03202 * ts + 0.00394) * t
            + (0.11693 * ts3 - 0.21196 * ts2 + 0.06052 * ts + 0.25886);
        self.zenith[2] = (0.00275 * ts3 - 0.00610 * ts2 + 0.00317 * ts) * t * t
            + (-0.04214 * ts3 + 0.08970 * ts2 - 0.04153 * ts + 0.00516) * t
            + (0.15346 * ts3 - 0.26756 * ts2 + 0.06670 * ts + 0.26688);

        for i in 0..3 {
            self.norm_f[i] = perez_f(
                1.0, theta_s, self.a[i], self.b[i], self.c[i], self.d[i], self.e[i],
            );
        }

        // Extinction along the slant path, as a function of air mass. Kasten
        // and Young's formula rather than 1/cos, which diverges at the horizon
        // and would make a setting sun infinitely red.
        let elev = (90.0 - theta_s.to_degrees()).max(0.0);
        let am = 1.0 / (elev.to_radians().sin() + 0.50572 * (elev + 6.07995).powf(-1.6364));

        // Rayleigh optical depth is strongly wavelength dependent, which is the
        // whole reason a low sun is orange. Coefficients are at roughly 615,
        // 535 and 465 nm -- the sRGB primaries.
        let tau = Vec3::new(0.1170, 0.1900, 0.4200);
        let mie = Vec3::new(0.0295, 0.0330, 0.0380);
        let arg = -(tau + mie * (self.turbidity - 1.0)) * am;
        let trans = Vec3::new(arg.x.exp(), arg.y.exp(), arg.z.exp());

        // v2 divided this by the disk's solid angle to get a radiance for its
        // own cone sampler. Bevy's DirectionalLight takes an illuminance and
        // does that division inside Solari, so the division is NOT done here --
        // doing it in both places is a factor of 1.7e-5 and a black scene.
        let irradiance = 22.0 * self.sun_scale * crate::core::saturate(self.sun_dir.y * 4.0);
        self.sun_radiance = trans * irradiance;
    }

    /// Radiance of the dome in the renderer's working units, without the sun.
    fn perez(&self, dir: Vec3) -> Vec3 {
        let dn = dir.normalize();
        let cos_theta = dn.y.max(0.01);
        let gamma = dn.dot(self.sun_dir).clamp(-1.0, 1.0).acos();

        let mut v = [0.0f32; 3];
        for i in 0..3 {
            v[i] = self.zenith[i]
                * perez_f(
                    cos_theta, gamma, self.a[i], self.b[i], self.c[i], self.d[i], self.e[i],
                )
                / self.norm_f[i].max(1e-6);
        }

        // xyY -> XYZ -> linear sRGB.
        let big_y = v[0].max(0.0);
        let x = v[1];
        let y = v[2].max(1e-4);
        let big_x = (x / y) * big_y;
        let big_z = ((1.0 - x - y) / y) * big_y;
        let rgb = Vec3::new(
            3.2404542 * big_x - 1.5371385 * big_y - 0.4985314 * big_z,
            -0.9692660 * big_x + 1.8760108 * big_y + 0.0415560 * big_z,
            0.0556434 * big_x - 0.2040259 * big_y + 1.0572252 * big_z,
        );
        rgb.max(Vec3::ZERO) * self.sky_scale
    }

    /// The dome in a direction, sun disk excluded.
    ///
    /// Below the horizon there is no sky, but a hard cut at y = 0 draws a
    /// visible seam right where the eye is looking. Fade into a dim ground
    /// bounce instead -- this stands in for the world beyond the resident ring,
    /// and it keeps the far treeline sitting in something.
    pub fn dome_radiance(&self, d: Vec3) -> Vec3 {
        if d.y <= 0.0 {
            let t = crate::core::saturate(-d.y / 0.10);
            let horizon = self.perez(Vec3::new(d.x, 1e-4, d.z));
            let avg = (horizon.x + horizon.y + horizon.z) * (1.0 / 3.0);
            return horizon.lerp(self.ground_albedo * avg, crate::core::sstep(t));
        }
        self.perez(d)
    }

    // -----------------------------------------------------------------------
    // Bake the dome into an equirectangular RGBA16Float image.
    //
    // FLOAT, NOT 8-BIT, and it matters here more than anywhere else in the
    // engine: this texture IS the light. Sky radiance spans about four orders
    // of magnitude between a zenith at noon and the same zenith twenty minutes
    // after sunset, and an 8-bit map would quantise dusk into three or four
    // flat steps and then to black -- visible as banding in the picture and,
    // far worse, as a fill light that snaps rather than fades.
    //
    // The layout matches how the dome's UVs are generated in `dome_mesh`:
    // u wraps azimuth from -Z counter-clockwise, v runs from +Y at the top to
    // -Y at the bottom.
    // -----------------------------------------------------------------------
    pub fn bake(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(SKY_TEX_W * SKY_TEX_H * 8);
        for j in 0..SKY_TEX_H {
            // Sampled at the texel centre, so the poles are never evaluated
            // exactly on the singularity.
            let v = (j as f32 + 0.5) / SKY_TEX_H as f32;
            let theta = v * PI; // 0 at +Y
            let (st, ct) = theta.sin_cos();
            for i in 0..SKY_TEX_W {
                let u = (i as f32 + 0.5) / SKY_TEX_W as f32;
                let phi = u * std::f32::consts::TAU;
                let dir = Vec3::new(st * phi.sin(), ct, -st * phi.cos());
                let c = self.dome_radiance(dir);
                for ch in [c.x, c.y, c.z, 1.0] {
                    out.extend_from_slice(&half::f16::from_f32(ch).to_le_bytes());
                }
            }
        }
        out
    }
}

fn perez_f(cos_theta: f32, gamma: f32, a: f32, b: f32, c: f32, d: f32, e: f32) -> f32 {
    let ct = cos_theta.max(0.01);
    let cg = gamma.cos();
    (1.0 + a * (b / ct).exp()) * (1.0 + c * (d * gamma).exp() + e * cg * cg)
}
