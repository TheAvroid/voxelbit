import * as ort from './ort.webgpu.bundle.min.mjs';

// Loads and caches the converted ONNX models (replaces the old TZA weight loader).
// Models are large and hosted on a CDN (like the old tzas/); point `url`/`path`
// at where the .onnx files live. Cached by name + precision.
class Models {
    static instance;
    cache = new Map();
    /** Subdirectory under the site root (used when `url` is unset). */
    path;
    /** Remote source for the models. models-v2 = models-v1 + aux split-graph
     *  artifacts (tail/enc0) that splitAux fetches for the cleanAux models. */
    url = 'https://cdn.jsdelivr.net/gh/pmndrs/denoiser-weights@models-v2/models';
    /** fp32 (default) or fp16 (smaller/faster, needs the shader-f16 feature). */
    precision = 'fp32';
    static getInstance() {
        if (!Models.instance)
            Models.instance = new Models();
        return Models.instance;
    }
    fileFor(name) {
        const suffix = this.precision === 'fp16' ? '.fp16.onnx' : '.onnx';
        if (this.url)
            return `${this.url}/${name}${suffix}`;
        return `/${this.path ?? 'models'}/${name}${suffix}`;
    }
    async get(name, overrideUrl) {
        const key = `${name}.${this.precision}`;
        const cached = this.cache.get(key);
        if (cached)
            return cached;
        const url = overrideUrl ?? this.fileFor(name);
        const res = await fetch(url);
        if (!res.ok)
            throw new Error(`Denoiser: failed to load model from ${url} (${res.status})`);
        const bytes = new Uint8Array(await res.arrayBuffer());
        this.cache.set(key, bytes);
        return bytes;
    }
    has(name) {
        return this.cache.has(`${name}.${this.precision}`);
    }
}

// WGSL compute pre/post-processing on the shared GPUDevice — the GPU replacement
// for the old TensorFlow.js tensor math (normalization, layout, tiling, blend).
//
// Kernels:
//   extractTiles — pull a BATCH of square tiles (per-tile offsets, zero-padded)
//                  from up to three full RGBA8 images (color [+albedo +normal])
//                  into a planar [B,C,tile,tile] NCHW input in one dispatch
//                  (workgroup z = batch slot). color/albedo normalized to [0,1]
//                  (optionally sRGB->linear); normals encoded to [0,1] à la
//                  upstream OIDN (docs/specs/oidn-color-reference.md).
//   accumulate   — blend ONE model-output tile (by batch slot) into accum +
//                  weight buffers using the min-of-sigmoid overlap mask
//                  (matches the old tiler.ts). Overlapping tiles must land in
//                  separate compute passes: pass boundaries synchronize the
//                  read-modify-write on accum/weight; z-batching them would race.
//   resolve      — accum / weight -> RGBA8, optional linear->sRGB, LDR clamp,
//                  optional Y flip.
//
// Layout is NCHW; channels is 3 (color), 6 (+albedo) or 9 (+albedo+normal).
// OIDN's PU transfer function for HDR color (docs/specs/oidn-color-reference.md):
// the network is trained on PU-encoded values — inputs go through
// pu_forward(y * inputScale) * PU_NORM, outputs through
// pu_inverse(x * PU_XMAX) / inputScale. inputScale comes from autoexposure
// (key 0.18 over the geometric mean luminance) via a 1-float storage buffer.
const PU_WGSL = /* wgsl */ `
const PU_A: f32 = 1.41283765e+03;
const PU_B: f32 = 1.64593172e+00;
const PU_C: f32 = 4.31384981e-01;
const PU_D: f32 = -2.94139609e-03;
const PU_E: f32 = 1.92653254e-01;
const PU_F: f32 = 6.26026094e-03;
const PU_G: f32 = 9.98620152e-01;
const PU_Y0: f32 = 1.57945760e-06;
const PU_Y1: f32 = 3.22087631e-02;
const PU_X0: f32 = 2.23151711e-03;
const PU_X1: f32 = 3.70974749e-01;
const PU_XMAX: f32 = 3.13512325;  // pu_forward1(65504) = PU_E*log(65504+PU_F)+PU_G
const PU_NORM: f32 = 0.318966;    // 1 / PU_XMAX

fn pu_forward1(y: f32) -> f32 {
  if (y <= PU_Y0) { return PU_A * y; }
  if (y <= PU_Y1) { return PU_B * pow(y, PU_C) + PU_D; }
  return PU_E * log(y + PU_F) + PU_G;
}
fn pu_inverse1(x: f32) -> f32 {
  if (x <= PU_X0) { return x / PU_A; }
  if (x <= PU_X1) { return pow((x - PU_D) / PU_B, 1.0 / PU_C); }
  return exp((x - PU_G) / PU_E) - PU_F;
}
fn pu_forward(y: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(pu_forward1(y.x), pu_forward1(y.y), pu_forward1(y.z)) * PU_NORM;
}
fn pu_inverse(x: vec3<f32>) -> vec3<f32> {
  let xs = x * PU_XMAX;
  return vec3<f32>(pu_inverse1(xs.x), pu_inverse1(xs.y), pu_inverse1(xs.z));
}
`;
// `io` is the model IO element type: 'f32', or 'f16' for fp16 models (needs the
// shader-f16 device feature). Only the model-facing NCHW buffers change type;
// accum/weight/resolve stay f32.
const EXTRACT_TILES = (io) => /* wgsl */ `
${io === 'f16' ? 'enable f16;' : ''}
alias IOType = ${io};
${PU_WGSL}
struct P {
  imgW:u32, imgH:u32, tileW:u32, tileH:u32, channels:u32, srgb:u32, count:u32, hdr:u32,
};
@group(0) @binding(0) var<storage, read> color: array<u32>;
@group(0) @binding(1) var<storage, read> albedo: array<u32>;
@group(0) @binding(2) var<storage, read> normal: array<u32>;
@group(0) @binding(3) var<storage, read_write> dst: array<IOType>; // NCHW, count*channels*tileW*tileH
@group(0) @binding(4) var<uniform> p: P;
@group(0) @binding(5) var<storage, read> offsets: array<vec2<u32>>; // per-slot startX,startY
@group(0) @binding(6) var<storage, read> exposure: array<f32>; // [inputScale] (autoexposure)

fn srgbToLinear(c: vec3<f32>) -> vec3<f32> {
  let hi = pow((c + 0.055) / 1.055, vec3<f32>(2.4));
  let lo = c / 12.92;
  return select(lo, hi, c > vec3<f32>(0.04045));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.z >= p.count || gid.x >= p.tileW || gid.y >= p.tileH) { return; }
  let plane = p.tileW * p.tileH;
  let base = gid.z * p.channels * plane;
  let didx = gid.y * p.tileW + gid.x;
  let off = offsets[gid.z];
  let sx = off.x + gid.x;
  let sy = off.y + gid.y;
  let inside = sx < p.imgW && sy < p.imgH;
  let sidx = select(0u, sy * p.imgW + sx, inside);

  var col = vec3<f32>(0.0);
  if (inside) {
    col = unpack4x8unorm(color[sidx]).xyz;
    if (p.srgb == 1u) { col = srgbToLinear(col); }
    if (p.hdr == 1u) { col = pu_forward(max(col * exposure[0], vec3<f32>(0.0))); }
  }
  dst[base + 0u * plane + didx] = IOType(col.x);
  dst[base + 1u * plane + didx] = IOType(col.y);
  dst[base + 2u * plane + didx] = IOType(col.z);

  if (p.channels >= 6u) {
    var alb = vec3<f32>(0.0);
    if (inside) { alb = unpack4x8unorm(albedo[sidx]).xyz; }
    dst[base + 3u * plane + didx] = IOType(alb.x);
    dst[base + 4u * plane + didx] = IOType(alb.y);
    dst[base + 5u * plane + didx] = IOType(alb.z);
  }
  if (p.channels >= 9u) {
    // OIDN feeds the network normals ENCODED to [0,1] (clamp(n,-1,1)*0.5+0.5 —
    // see docs/specs/oidn-color-reference.md). RGBA8 bytes already hold that encoding;
    // pad with 0.5 (the encoded zero-normal).
    var nrm = vec3<f32>(0.5);
    if (inside) { nrm = unpack4x8unorm(normal[sidx]).xyz; }
    dst[base + 6u * plane + didx] = IOType(nrm.x);
    dst[base + 7u * plane + didx] = IOType(nrm.y);
    dst[base + 8u * plane + didx] = IOType(nrm.z);
  }
}
`;
// Texture-input variant: reads float textures (e.g. a path tracer's linear-HDR
// render target) instead of RGBA8 storage buffers — no CPU round-trip, no 8-bit
// quantization. Color passes through as-is (hdr) or sRGB->linear (srgb flag);
// albedo expected [0,1]; normal expected already [-1,1] (G-buffer convention).
// flipY reads the source bottom-up (WebGPU render targets).
const EXTRACT_TILES_TEX = (io) => /* wgsl */ `
${io === 'f16' ? 'enable f16;' : ''}
alias IOType = ${io};
${PU_WGSL}
struct P {
  imgW:u32, imgH:u32, tileW:u32, tileH:u32, channels:u32, srgb:u32, count:u32, flipY:u32,
  hdr:u32, auxFlipY:u32, _p1:u32, _p2:u32,
};
@group(0) @binding(0) var color: texture_2d<f32>;
@group(0) @binding(1) var albedo: texture_2d<f32>;
@group(0) @binding(2) var normal: texture_2d<f32>;
@group(0) @binding(3) var<storage, read_write> dst: array<IOType>; // NCHW, count*channels*tileW*tileH
@group(0) @binding(4) var<uniform> p: P;
@group(0) @binding(5) var<storage, read> offsets: array<vec2<u32>>; // per-slot startX,startY
@group(0) @binding(6) var<storage, read> exposure: array<f32>; // [inputScale] (autoexposure)

fn srgbToLinear(c: vec3<f32>) -> vec3<f32> {
  let hi = pow((c + 0.055) / 1.055, vec3<f32>(2.4));
  let lo = c / 12.92;
  return select(lo, hi, c > vec3<f32>(0.04045));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.z >= p.count || gid.x >= p.tileW || gid.y >= p.tileH) { return; }
  let plane = p.tileW * p.tileH;
  let base = gid.z * p.channels * plane;
  let didx = gid.y * p.tileW + gid.x;
  let off = offsets[gid.z];
  let sx = off.x + gid.x;
  let sy = off.y + gid.y;
  let inside = sx < p.imgW && sy < p.imgH;
  let ly = select(sy, p.imgH - 1u - sy, p.flipY == 1u);
  let coord = vec2<i32>(i32(sx), i32(ly));
  // aux sources can have the opposite vertical convention (e.g. raster G-buffer
  // vs compute-written tracer output) — separate flip
  let lyAux = select(sy, p.imgH - 1u - sy, p.auxFlipY == 1u);
  let coordAux = vec2<i32>(i32(sx), i32(lyAux));

  var col = vec3<f32>(0.0);
  if (inside) {
    // float texture inputs are linear by contract; p.srgb is output-side only
    col = textureLoad(color, coord, 0).xyz;
    if (p.hdr == 1u) { col = pu_forward(max(col * exposure[0], vec3<f32>(0.0))); }
  }
  dst[base + 0u * plane + didx] = IOType(col.x);
  dst[base + 1u * plane + didx] = IOType(col.y);
  dst[base + 2u * plane + didx] = IOType(col.z);

  if (p.channels >= 6u) {
    // albedo is [0,1] by OIDN contract — clamp defensively (HDR env colors
    // routed into an albedo G-buffer can exceed 1)
    var alb = vec3<f32>(0.0);
    if (inside) { alb = clamp(textureLoad(albedo, coordAux, 0).xyz, vec3<f32>(0.0), vec3<f32>(1.0)); }
    dst[base + 3u * plane + didx] = IOType(alb.x);
    dst[base + 4u * plane + didx] = IOType(alb.y);
    dst[base + 5u * plane + didx] = IOType(alb.z);
  }
  if (p.channels >= 9u) {
    // float G-buffer normals arrive [-1,1]; the network wants them encoded [0,1]
    var nrm = vec3<f32>(0.5);
    if (inside) {
      nrm = clamp(textureLoad(normal, coordAux, 0).xyz, vec3<f32>(-1.0), vec3<f32>(1.0)) * 0.5 + 0.5;
    }
    dst[base + 6u * plane + didx] = IOType(nrm.x);
    dst[base + 7u * plane + didx] = IOType(nrm.y);
    dst[base + 8u * plane + didx] = IOType(nrm.z);
  }
}
`;
// enc_conv0 workaround kernel. onnxruntime-web's WebGPU EP miscomputes the
// FIRST Conv that reduces the raw >3-channel graph input (see
// tools/ort-webgpu-aux-repro + ort-webgpu-aux-split: 9ch aux output ~1e-1 off,
// while the same net's later convs — even the 105ch dec_conv1a fed the raw input
// — are correct). We compute just that one conv here (Conv 3x3, pad 1, stride 1,
// CIN->COUT, + relu6) and run a re-exported "tail" model that starts at
// enc_conv1 on ORT-WebGPU. Verified to restore native quality (1.2e-6).
//
// Naive one-thread-per-(x,y,co) form (mirrors the kernel-spike correctness
// anchor). enc_conv0 is a single full-res layer (CIN=9, COUT=32) — trivially
// cheap — so speed doesn't matter; correctness does. Weights/bias are f32 for
// accuracy even when the model IO is f16; accumulation is f32.
const ENC_CONV0 = (io) => /* wgsl */ `
${io === 'f16' ? 'enable f16;' : ''}
alias IOType = ${io};
struct P { w:u32, h:u32, cin:u32, cout:u32, batch:u32, _p0:u32, _p1:u32, _p2:u32 };
@group(0) @binding(0) var<storage, read> src: array<IOType>;   // NCHW [B,CIN,H,W]
@group(0) @binding(1) var<storage, read> weights: array<f32>;  // OIHW [COUT,CIN,3,3]
@group(0) @binding(2) var<storage, read> bias: array<f32>;     // [COUT]
@group(0) @binding(3) var<storage, read_write> dst: array<IOType>; // NCHW [B,COUT,H,W]
@group(0) @binding(4) var<uniform> p: P;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  if (g.x >= p.w || g.y >= p.h || g.z >= p.batch * p.cout) { return; }
  let b = g.z / p.cout;
  let co = g.z % p.cout;
  let plane = p.w * p.h;
  let inBase = b * p.cin * plane;
  var acc = bias[co];
  for (var ci = 0u; ci < p.cin; ci++) {
    let wbase = (co * p.cin + ci) * 9u;
    let cbase = inBase + ci * plane;
    for (var ky = 0u; ky < 3u; ky++) {
      let sy = i32(g.y) + i32(ky) - 1;
      if (sy < 0 || sy >= i32(p.h)) { continue; }
      for (var kx = 0u; kx < 3u; kx++) {
        let sx = i32(g.x) + i32(kx) - 1;
        if (sx < 0 || sx >= i32(p.w)) { continue; }
        acc += f32(src[cbase + u32(sy) * p.w + u32(sx)]) * weights[wbase + ky * 3u + kx];
      }
    }
  }
  // relu6 (Clip 0..6) folded into the epilogue
  dst[b * p.cout * plane + co * plane + g.y * p.w + g.x] = IOType(clamp(acc, 0.0, 6.0));
}
`;
const ACCUMULATE_TILE = (io) => /* wgsl */ `
${io === 'f16' ? 'enable f16;' : ''}
alias IOType = ${io};
struct P {
  imgW:u32, imgH:u32, startX:u32, startY:u32, curW:u32, curH:u32,
  tileX:u32, tileY:u32, tilesX:u32, tilesY:u32, tileW:u32, tileH:u32,
  batchIdx:u32, overlap:f32,
};
@group(0) @binding(0) var<storage, read> src: array<IOType>;       // NCHW model output (B*3ch)
@group(0) @binding(1) var<storage, read_write> accum: array<f32>;  // 3*imgW*imgH
@group(0) @binding(2) var<storage, read_write> weight: array<f32>; // imgW*imgH
@group(0) @binding(3) var<uniform> p: P;

fn sig(x: f32) -> f32 { return 1.0 / (1.0 + exp(-12.0 * (x - 0.5))); }

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.x >= p.curW || gid.y >= p.curH) { return; }
  let tx = gid.x; let ty = gid.y;
  var yW = 1.0; var xW = 1.0;
  if (p.tileY > 0u)            { yW = min(yW, sig(f32(ty) / p.overlap)); }
  if (p.tileY < p.tilesY - 1u) { yW = min(yW, sig(f32(p.curH - 1u - ty) / p.overlap)); }
  if (p.tileX > 0u)            { xW = min(xW, sig(f32(tx) / p.overlap)); }
  if (p.tileX < p.tilesX - 1u) { xW = min(xW, sig(f32(p.curW - 1u - tx) / p.overlap)); }
  let w = min(yW, xW);

  let stile = p.tileW * p.tileH;
  let sbase = p.batchIdx * 3u * stile;
  let sidx = ty * p.tileW + tx;
  let gplane = p.imgW * p.imgH;
  let gidx = (p.startY + ty) * p.imgW + (p.startX + tx);
  accum[0u * gplane + gidx] = accum[0u * gplane + gidx] + w * f32(src[sbase + 0u * stile + sidx]);
  accum[1u * gplane + gidx] = accum[1u * gplane + gidx] + w * f32(src[sbase + 1u * stile + sidx]);
  accum[2u * gplane + gidx] = accum[2u * gplane + gidx] + w * f32(src[sbase + 2u * stile + sidx]);
  weight[gidx] = weight[gidx] + w;
}
`;
// Shared resolve math: accum/weight -> display rgb. tonemap = Narkowicz ACES
// (for HDR results headed straight to a canvas) applied before the sRGB encode.
// Autoexposure (upstream OIDN algorithm): per-16px-bin mean luminance, then
// inputScale = 0.18 / geometric-mean of the bin luminances.
const AUTOEXPOSURE_BINS = /* wgsl */ `
struct P { imgW:u32, imgH:u32, binsX:u32, binsY:u32 };
@group(0) @binding(0) var color: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> bins: array<f32>;
@group(0) @binding(2) var<uniform> p: P;
var<workgroup> partial: array<f32, 64>;

@compute @workgroup_size(8, 8)
fn main(@builtin(workgroup_id) wid: vec3<u32>,
        @builtin(local_invocation_id) lid: vec3<u32>,
        @builtin(local_invocation_index) li: u32) {
  let x0 = (wid.x * p.imgW) / p.binsX; let x1 = ((wid.x + 1u) * p.imgW) / p.binsX;
  let y0 = (wid.y * p.imgH) / p.binsY; let y1 = ((wid.y + 1u) * p.imgH) / p.binsY;
  var sum = 0.0;
  var yy = y0 + lid.y;
  while (yy < y1) {
    var xx = x0 + lid.x;
    while (xx < x1) {
      let c = clamp(textureLoad(color, vec2<i32>(i32(xx), i32(yy)), 0).xyz,
                    vec3<f32>(0.0), vec3<f32>(3.4e38));
      sum += 0.212671 * c.x + 0.715160 * c.y + 0.072169 * c.z;
      xx += 8u;
    }
    yy += 8u;
  }
  partial[li] = sum;
  workgroupBarrier();
  var s = 32u;
  while (s > 0u) {
    if (li < s) { partial[li] += partial[li + s]; }
    workgroupBarrier();
    s = s >> 1u;
  }
  if (li == 0u) {
    let count = f32(max((x1 - x0) * (y1 - y0), 1u));
    bins[wid.y * p.binsX + wid.x] = partial[0] / count;
  }
}
`;
const AUTOEXPOSURE_REDUCE = /* wgsl */ `
struct P { numBins:u32, _0:u32, _1:u32, _2:u32 };
@group(0) @binding(0) var<storage, read> bins: array<f32>;
@group(0) @binding(1) var<storage, read_write> exposure: array<f32>;
@group(0) @binding(2) var<uniform> p: P;

@compute @workgroup_size(1)
fn main() {
  var sum = 0.0;
  var count = 0.0;
  for (var i = 0u; i < p.numBins; i++) {
    if (bins[i] > 1e-8) { sum += log2(bins[i]); count += 1.0; }
  }
  exposure[0] = select(1.0, 0.18 / exp2(sum / count), count > 0.0);
}
`;
const RESOLVE_COMMON = /* wgsl */ `
${PU_WGSL}
struct P { imgW:u32, imgH:u32, srgb:u32, hdr:u32, flipY:u32, tonemap:u32, _p1:u32, _p2:u32 };

fn linearToSrgb(c: vec3<f32>) -> vec3<f32> {
  let hi = pow(c, vec3<f32>(1.0 / 2.4)) * 1.055 - 0.055;
  let lo = c * 12.92;
  return select(lo, hi, c > vec3<f32>(0.0031308));
}

fn acesTonemap(c: vec3<f32>) -> vec3<f32> {
  return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn resolveRgb(rgbIn: vec3<f32>, p: P) -> vec3<f32> {
  var rgb = rgbIn;
  if (p.tonemap == 1u) { rgb = linearToSrgb(acesTonemap(rgb)); }
  else {
    if (p.srgb == 1u) { rgb = linearToSrgb(rgb); }
    if (p.hdr == 0u) { rgb = clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)); }
  }
  return rgb; // NOTE: unclamped — hdr float outputs keep their range; unorm sinks clamp
}
`;
const RESOLVE = /* wgsl */ `
${RESOLVE_COMMON}
@group(0) @binding(0) var<storage, read> accum: array<f32>;
@group(0) @binding(1) var<storage, read> weight: array<f32>;
@group(0) @binding(2) var<storage, read_write> dst: array<u32>;
@group(0) @binding(3) var<uniform> p: P;
@group(0) @binding(4) var<storage, read> exposure: array<f32>; // [inputScale]

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.x >= p.imgW || gid.y >= p.imgH) { return; }
  let idx = gid.y * p.imgW + gid.x;
  let gplane = p.imgW * p.imgH;
  let w = weight[idx] + 1e-8;
  var rgb = vec3<f32>(accum[0u*gplane+idx], accum[1u*gplane+idx], accum[2u*gplane+idx]) / w;
  if (p.hdr == 1u) { rgb = pu_inverse(max(rgb, vec3<f32>(0.0))) / exposure[0]; }
  rgb = clamp(resolveRgb(rgb, p), vec3<f32>(0.0), vec3<f32>(1.0));
  let oy = select(gid.y, p.imgH - 1u - gid.y, p.flipY == 1u);
  dst[oy * p.imgW + gid.x] = pack4x8unorm(vec4<f32>(rgb, 1.0));
}
`;
// format: rgba8unorm (clamped, display-ready) or rgba16float (unclamped — keeps
// HDR range so e.g. three.js can tonemap in its own pipeline).
const RESOLVE_TEX = (format) => /* wgsl */ `
${RESOLVE_COMMON}
@group(0) @binding(0) var<storage, read> accum: array<f32>;
@group(0) @binding(1) var<storage, read> weight: array<f32>;
@group(0) @binding(2) var dst: texture_storage_2d<${format}, write>;
@group(0) @binding(3) var<uniform> p: P;
@group(0) @binding(4) var<storage, read> exposure: array<f32>; // [inputScale]

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  if (gid.x >= p.imgW || gid.y >= p.imgH) { return; }
  let idx = gid.y * p.imgW + gid.x;
  let gplane = p.imgW * p.imgH;
  let w = weight[idx] + 1e-8;
  var rgb = vec3<f32>(accum[0u*gplane+idx], accum[1u*gplane+idx], accum[2u*gplane+idx]) / w;
  if (p.hdr == 1u) { rgb = pu_inverse(max(rgb, vec3<f32>(0.0))) / exposure[0]; }
  rgb = resolveRgb(rgb, p);
  ${format === 'rgba8unorm' ? 'rgb = clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0));' : ''}
  let oy = select(gid.y, p.imgH - 1u - gid.y, p.flipY == 1u);
  textureStore(dst, vec2<i32>(i32(gid.x), i32(oy)), vec4<f32>(rgb, 1.0));
}
`;
class GpuImageOps {
    device;
    maxBatch;
    extractPipe;
    accumPipe;
    resolvePipe;
    extractTexPipe; // lazy — texture-input path
    resolveTexPipes = new Map(); // lazy, per format
    encConv0Pipe; // lazy — aux split-graph workaround
    encConv0Params; // 32B uniform
    extractParams; // 48B uniform (common; TEX variant uses 12 u32)
    extractOffsets; // maxBatch * 8B storage (per-slot startX/startY)
    accumParams; // one 64B uniform per batch slot
    resolveParams; // 32B
    exposureBuf; // [inputScale] — autoexposure result or manual value
    binsBuf; // autoexposure bin luminances
    binsCapacity = 0;
    aeBinsPipe;
    aeReducePipe;
    aeBinsParams;
    aeReduceParams;
    io;
    mk(code) {
        return this.device.createComputePipeline({
            layout: 'auto',
            compute: { module: this.device.createShaderModule({ code }), entryPoint: 'main' },
        });
    }
    constructor(device, maxBatch, ioF16 = false) {
        this.device = device;
        this.maxBatch = maxBatch;
        const mk = (code) => this.mk(code);
        const io = (this.io = ioF16 ? 'f16' : 'f32');
        this.extractPipe = mk(EXTRACT_TILES(io));
        this.accumPipe = mk(ACCUMULATE_TILE(io));
        this.resolvePipe = mk(RESOLVE);
        const u = (size) => device.createBuffer({ size, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        this.exposureBuf = device.createBuffer({
            size: 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC,
        });
        this.setExposure(1);
        this.extractParams = u(48);
        this.extractOffsets = device.createBuffer({
            size: Math.max(1, maxBatch) * 8,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
        });
        this.accumParams = Array.from({ length: Math.max(1, maxBatch) }, () => u(64));
        this.resolveParams = u(32);
    }
    run(enc, pipe, buffers, dx, dy, dz = 1) {
        this.runMixed(enc, pipe, buffers.map((buffer) => ({ buffer })), dx, dy, dz);
    }
    runMixed(enc, pipe, resources, dx, dy, dz = 1) {
        const bind = this.device.createBindGroup({
            layout: pipe.getBindGroupLayout(0),
            entries: resources.map((resource, i) => ({ binding: i, resource })),
        });
        const pass = enc.beginComputePass();
        pass.setPipeline(pipe);
        pass.setBindGroup(0, bind);
        pass.dispatchWorkgroups(Math.ceil(dx / 8), Math.ceil(dy / 8), dz);
        pass.end();
    }
    /** Set the HDR input scale manually (autoexposure overwrites it when encoded). */
    setExposure(inputScale) {
        this.device.queue.writeBuffer(this.exposureBuf, 0, new Float32Array([inputScale]));
    }
    /** OIDN autoexposure: computes inputScale from the color texture into the exposure buffer. */
    encodeAutoexposure(enc, color, imgW, imgH) {
        this.aeBinsPipe ??= this.mk(AUTOEXPOSURE_BINS);
        this.aeReducePipe ??= this.mk(AUTOEXPOSURE_REDUCE);
        const u = (size) => this.device.createBuffer({ size, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        this.aeBinsParams ??= u(16);
        this.aeReduceParams ??= u(16);
        const binsX = Math.ceil(imgW / 16);
        const binsY = Math.ceil(imgH / 16);
        if (binsX * binsY > this.binsCapacity) {
            this.binsBuf?.destroy();
            this.binsCapacity = binsX * binsY;
            this.binsBuf = this.device.createBuffer({
                size: this.binsCapacity * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
            });
        }
        this.device.queue.writeBuffer(this.aeBinsParams, 0, new Uint32Array([imgW, imgH, binsX, binsY]));
        this.device.queue.writeBuffer(this.aeReduceParams, 0, new Uint32Array([binsX * binsY, 0, 0, 0]));
        this.runMixed(enc, this.aeBinsPipe, [color, { buffer: this.binsBuf }, { buffer: this.aeBinsParams }], binsX * 8, binsY * 8);
        this.runMixed(enc, this.aeReducePipe, [{ buffer: this.binsBuf }, { buffer: this.exposureBuf }, { buffer: this.aeReduceParams }], 1, 1);
    }
    /** Extract `count` tiles (offsets = count pairs of startX,startY) into dst[B,C,tileH,tileW] in one dispatch. */
    encodeExtractTiles(enc, color, albedo, normal, dst, imgW, imgH, tileW, tileH, channels, srgb, hdr, offsets, count) {
        this.device.queue.writeBuffer(this.extractParams, 0, new Uint32Array([imgW, imgH, tileW, tileH, channels, srgb ? 1 : 0, count, hdr ? 1 : 0]));
        // See ArrayBuffer-vs-ArrayBufferLike note in engine.ts (TS 5.7+ lib.dom typings).
        this.device.queue.writeBuffer(this.extractOffsets, 0, offsets, 0, count * 2);
        this.run(enc, this.extractPipe, [color, albedo, normal, dst, this.extractParams, this.extractOffsets, this.exposureBuf], tileW, tileH, count);
    }
    /**
     * Blend one batch-slot's output tile into accum/weight. Each call encodes its
     * own compute pass (overlapping tiles RMW the same texels; pass boundaries
     * order them). `slot` selects a dedicated uniform buffer so a whole batch of
     * accumulates can be encoded before a single submit.
     */
    encodeAccumulateTile(enc, slot, outNCHW, accum, weight, p) {
        const ab = new ArrayBuffer(64);
        new Uint32Array(ab, 0, 13).set([
            p.imgW, p.imgH, p.startX, p.startY, p.curW, p.curH,
            p.tileX, p.tileY, p.tilesX, p.tilesY, p.tileW, p.tileH, p.batchIdx,
        ]);
        new Float32Array(ab, 52, 1)[0] = p.overlap;
        const params = this.accumParams[slot];
        this.device.queue.writeBuffer(params, 0, ab);
        this.run(enc, this.accumPipe, [outNCHW, accum, weight, params], p.curW, p.curH);
    }
    encodeResolve(enc, accum, weight, dst, imgW, imgH, srgb, hdr, flipY = false, tonemap = false) {
        this.device.queue.writeBuffer(this.resolveParams, 0, new Uint32Array([imgW, imgH, srgb ? 1 : 0, hdr ? 1 : 0, flipY ? 1 : 0, tonemap ? 1 : 0, 0, 0]));
        this.run(enc, this.resolvePipe, [accum, weight, dst, this.resolveParams, this.exposureBuf], imgW, imgH);
    }
    /** Texture-input extract: color/albedo/normal are float texture views. */
    encodeExtractTilesTex(enc, color, albedo, normal, dst, imgW, imgH, tileW, tileH, channels, srgb, flipY, hdr, auxFlipY, offsets, count) {
        this.extractTexPipe ??= this.mk(EXTRACT_TILES_TEX(this.io));
        this.device.queue.writeBuffer(this.extractParams, 0, new Uint32Array([imgW, imgH, tileW, tileH, channels, srgb ? 1 : 0, count, flipY ? 1 : 0,
            hdr ? 1 : 0, auxFlipY ? 1 : 0, 0, 0]));
        // See ArrayBuffer-vs-ArrayBufferLike note in engine.ts (TS 5.7+ lib.dom typings).
        this.device.queue.writeBuffer(this.extractOffsets, 0, offsets, 0, count * 2);
        this.runMixed(enc, this.extractTexPipe, [color, albedo, normal, { buffer: dst }, { buffer: this.extractParams },
            { buffer: this.extractOffsets }, { buffer: this.exposureBuf }], tileW, tileH, count);
    }
    /**
     * Compute enc_conv0 (Conv 3x3 pad1 stride1 CIN->COUT + relu6) from an NCHW
     * `src` [B,CIN,H,W] into `dst` [B,COUT,H,W], the aux split-graph workaround
     * for the ORT-web WebGPU Conv bug. `weights` is OIHW [COUT,CIN,3,3] f32,
     * `bias` is [COUT] f32 (both engine-owned, uploaded once).
     */
    encodeEncConv0(enc, src, weights, bias, dst, w, h, cin, cout, batch) {
        this.encConv0Pipe ??= this.mk(ENC_CONV0(this.io));
        this.encConv0Params ??= this.device.createBuffer({
            size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
        });
        this.device.queue.writeBuffer(this.encConv0Params, 0, new Uint32Array([w, h, cin, cout, batch, 0, 0, 0]));
        this.run(enc, this.encConv0Pipe, [src, weights, bias, dst, this.encConv0Params], w, h, batch * cout);
    }
    /** Resolve straight into a storage texture (no CPU readback). rgba8unorm or rgba16float. */
    encodeResolveToTexture(enc, accum, weight, dst, format, imgW, imgH, srgb, hdr, flipY = false, tonemap = false) {
        let pipe = this.resolveTexPipes.get(format);
        if (!pipe) {
            pipe = this.mk(RESOLVE_TEX(format));
            this.resolveTexPipes.set(format, pipe);
        }
        this.device.queue.writeBuffer(this.resolveParams, 0, new Uint32Array([imgW, imgH, srgb ? 1 : 0, hdr ? 1 : 0, flipY ? 1 : 0, tonemap ? 1 : 0, 0, 0]));
        this.runMixed(enc, pipe, [{ buffer: accum }, { buffer: weight }, dst, { buffer: this.resolveParams },
            { buffer: this.exposureBuf }], imgW, imgH);
    }
}

/** WebGPU / shader-f16 / device capability problems. */
class DenoiserUnsupportedError extends Error {
    constructor(message) { super(message); this.name = 'DenoiserUnsupportedError'; }
}
/** Bad or inconsistent inputs (sizes, formats, missing aux). */
class DenoiserInputError extends Error {
    constructor(message) { super(message); this.name = 'DenoiserInputError'; }
}

// DenoiseEngine: the WebGPU/ONNX-Runtime-Web inference core that replaces the old
// TensorFlow.js UNet + GPUTensorTiler. It owns the InferenceSession and the shared
// GPUDevice (ORT creates it; expose `device` to share with three.js — see #26107),
// does pre/post + tiling in WGSL (GpuImageOps), and keeps everything on the GPU
// except the final pixel readback.
//
// The ONNX models export with named free dims [batch, C, height, width]; the
// engine pins them per session via freeDimensionOverrides and keeps a small
// cache of sessions keyed by geometry. Per image it picks a plan:
//   - WHOLE-FRAME (preferred): pad W/H up to /16, batch=1, overlap=0 — one
//     session.run for the entire image, no overlap redundancy, no seams.
//   - TILED fallback (huge images / tight device limits): square tiles with
//     32px overlap, several tiles batched per run.
// The U-Net's full-res intermediates are large (up to ~96 channels × H × W), so
// whole-frame mode needs raised device limits — ORT requests a minimal device,
// so the FIRST session creation runs under a scoped requestAdapter patch that
// asks for the adapter's max limits/features (same trick as the pathtracer
// example, but restored immediately after).
// Conservative upper bound on the widest full-resolution tensor in any of the
// U-Net variants (decoder concat levels), in channels. Used to test a candidate
// geometry against the device's buffer limits before creating a session.
const WORST_FULLRES_CHANNELS = 96;
const pad16 = (x) => Math.ceil(x / 16) * 16;
class DenoiseEngine {
    device;
    inputName;
    outputName;
    channels = 3;
    tile;
    overlap;
    /** Max tiles per session.run in tiled mode. */
    batch;
    precision;
    maxRunPixels;
    /** Stage timings from the most recent denoise() call. */
    lastStats;
    /** True when sessions run with WebGPU graph capture enabled. */
    graphCaptured = false;
    modelBytes;
    baseSessionOpts;
    ops;
    geos = new Map();
    dynamicDims = true; // false for legacy static-dim models
    split;
    // per-image buffers
    imgW = 0;
    imgH = 0;
    color;
    albedo;
    normal;
    accum;
    weight;
    outPixels;
    readback;
    outTexture;
    constructor(opts) {
        this.channels = opts.channels;
        this.tile = opts.tile ?? 256;
        this.overlap = opts.overlap ?? 32;
        this.batch = Math.max(1, opts.batch ?? 8);
        this.precision = opts.precision ?? 'fp32';
        this.maxRunPixels = opts.maxRunPixels ?? 2048 * 1152;
        if (opts.split) {
            const s = opts.split;
            const expectW = s.encOutChannels * this.channels * 9;
            if (s.encWeights.length !== expectW) {
                throw new Error(`Denoiser: split encWeights must be ${expectW} floats ([${s.encOutChannels},${this.channels},3,3]), got ${s.encWeights.length}`);
            }
            if (s.encBias.length !== s.encOutChannels) {
                throw new Error(`Denoiser: split encBias must be ${s.encOutChannels} floats, got ${s.encBias.length}`);
            }
            this.split = {
                encWeights: s.encWeights,
                encBias: s.encBias,
                encOutChannels: s.encOutChannels,
                featInputName: s.featInputName ?? 'enc_conv0_relu6_2',
                rawInputName: s.rawInputName ?? 'input',
            };
        }
    }
    get bpe() { return this.precision === 'fp16' ? 2 : 4; }
    static async create(modelBytes, opts) {
        const e = new DenoiseEngine(opts);
        e.modelBytes = modelBytes;
        ort.env.wasm.numThreads = 1; // avoids needing cross-origin isolation
        ort.env.wasm.wasmPaths =
            opts.wasmPaths ?? 'https://cdn.jsdelivr.net/npm/onnxruntime-web@1.27.0/dist/';
        e.baseSessionOpts = {
            executionProviders: ['webgpu'],
            preferredOutputLocation: 'gpu-buffer',
            graphOptimizationLevel: 'all',
        };
        if (opts.graphCapture)
            e.baseSessionOpts.enableGraphCapture = true;
        e.graphCaptured = !!opts.graphCapture;
        // Create the default (tiled-fallback) geometry now — this is also what
        // makes ORT create the GPUDevice, under the scoped max-limits patch.
        const deviceMissing = !ort.env.webgpu.device;
        // WebGPU is required (no WebGL fallback in v2) — probe before ORT so a missing
        // adapter fails loudly here instead of as a cryptic WASM/session error deeper in.
        if (deviceMissing && (typeof navigator === 'undefined' || !('gpu' in navigator) || !(await navigator.gpu.requestAdapter()))) {
            throw new DenoiserUnsupportedError('denoiser 2.x requires WebGPU (no adapter available). See browser support: Chrome/Edge stable, Safari 26+. For WebGL environments use denoiser 0.x (v1).');
        }
        const unpatch = deviceMissing ? patchForMaxLimits() : undefined;
        try {
            await e.ensureGeo({ tileW: e.tile, tileH: e.tile, batch: e.batch, overlap: e.overlap });
        }
        finally {
            unpatch?.();
        }
        e.device = ort.env.webgpu.device;
        if (!e.device)
            throw new DenoiserUnsupportedError('Denoiser: ORT did not expose a WebGPU device');
        if (e.precision === 'fp16' && !e.device.features.has('shader-f16')) {
            // ORT requests shader-f16 on its device when the adapter has it; without
            // it our WGSL can't read/write the fp16 model IO buffers.
            e.destroy();
            throw new DenoiserUnsupportedError('Denoiser: fp16 needs the shader-f16 WebGPU feature (unavailable on this device)');
        }
        e.ops = new GpuImageOps(e.device, e.batch, e.precision === 'fp16');
        // Split mode: upload enc_conv0 weights/bias once (device-lifetime). f32 for
        // accuracy even when the model IO is fp16 (accumulation is f32 in the kernel).
        if (e.split) {
            const s = e.split;
            s.weightBuf = e.device.createBuffer({
                size: s.encWeights.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
            });
            s.biasBuf = e.device.createBuffer({
                size: s.encBias.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
            });
            // TS 5.7+ lib.dom typings narrowed GPUAllowSharedBufferSource views to ArrayBuffer-backed;
            // this engine never uses SharedArrayBuffer, so these casts are safe.
            e.device.queue.writeBuffer(s.weightBuf, 0, s.encWeights);
            e.device.queue.writeBuffer(s.biasBuf, 0, s.encBias);
        }
        return e;
    }
    /** Get (or create) the session + IO buffers for a geometry. */
    async ensureGeo(plan) {
        const key = `${plan.batch}|${plan.tileW}x${plan.tileH}`;
        const hit = this.geos.get(key);
        if (hit)
            return hit;
        let session;
        if (this.dynamicDims) {
            try {
                session = await ort.InferenceSession.create(this.modelBytes, {
                    ...this.baseSessionOpts,
                    freeDimensionOverrides: { batch: plan.batch, height: plan.tileH, width: plan.tileW },
                });
            }
            catch (err) {
                if (this.geos.size > 0)
                    throw err; // dynamic already proven -> real failure
                // Legacy static-dim model ([1, C, 256, 256]): no free dims to pin.
                console.warn('Denoiser: model has static dims, geometry planning disabled', err);
                this.dynamicDims = false;
                this.batch = 1;
                plan = { tileW: 256, tileH: 256, batch: 1, overlap: this.overlap };
                session = await ort.InferenceSession.create(this.modelBytes, this.baseSessionOpts);
            }
        }
        else {
            session = await ort.InferenceSession.create(this.modelBytes, this.baseSessionOpts);
        }
        this.outputName = session.outputNames[0];
        if (this.split) {
            // Tail model has two inputs: the enc_conv0 feature map and the raw image.
            for (const n of [this.split.featInputName, this.split.rawInputName]) {
                if (!session.inputNames.includes(n)) {
                    throw new Error(`Denoiser: split tail model missing input '${n}' (has: ${session.inputNames.join(', ')})`);
                }
            }
            this.inputName = this.split.rawInputName; // the raw image feed
        }
        else {
            this.inputName = session.inputNames[0];
        }
        const device = ort.env.webgpu.device;
        const els = plan.batch * plan.tileW * plan.tileH;
        const f16 = this.precision === 'fp16';
        const dtype = f16 ? 'float16' : 'float32';
        const nchwInput = device.createBuffer({
            size: els * this.channels * this.bpe,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC,
        });
        const outNCHW = device.createBuffer({
            size: els * 3 * this.bpe,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
        });
        const geo = {
            key, session, nchwInput, outNCHW, plan,
            inputTensor: ort.Tensor.fromGpuBuffer(nchwInput, {
                dataType: dtype,
                dims: [plan.batch, this.channels, plan.tileH, plan.tileW],
            }),
            outputTensor: ort.Tensor.fromGpuBuffer(outNCHW, {
                dataType: dtype,
                dims: [plan.batch, 3, plan.tileH, plan.tileW],
            }),
        };
        if (this.split) {
            const cout = this.split.encOutChannels;
            geo.encFeat = device.createBuffer({
                size: els * cout * this.bpe,
                usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
            });
            geo.encTensor = ort.Tensor.fromGpuBuffer(geo.encFeat, {
                dataType: dtype,
                dims: [plan.batch, cout, plan.tileH, plan.tileW],
            });
        }
        // Tiny LRU: sessions hold a GPU copy of the weights; keep a few geometries.
        if (this.geos.size >= 4) {
            const oldest = this.geos.keys().next().value;
            this.releaseGeo(this.geos.get(oldest));
            this.geos.delete(oldest);
        }
        this.geos.set(key, geo);
        return geo;
    }
    releaseGeo(g) {
        g.nchwInput.destroy();
        g.outNCHW.destroy();
        g.encFeat?.destroy();
        g.session.release?.();
    }
    /** Choose the run geometry for an image size (whole-frame when it fits). */
    planFor(w, h) {
        if (!this.dynamicDims)
            return { tileW: 256, tileH: 256, batch: 1, overlap: this.overlap };
        const limits = this.device?.limits;
        const bufferCap = limits
            ? Math.min(limits.maxBufferSize, limits.maxStorageBufferBindingSize)
            : 128 * 1024 * 1024;
        const worstBytes = (px) => px * WORST_FULLRES_CHANNELS * this.bpe;
        const pw = pad16(w);
        const ph = pad16(h);
        if (pw * ph <= this.maxRunPixels && worstBytes(pw * ph) <= bufferCap) {
            return { tileW: pw, tileH: ph, batch: 1, overlap: 0 };
        }
        for (const t of [1024, 512, this.tile]) {
            if (t > Math.max(pw, ph))
                continue; // pointless: bigger than the image
            const perTile = t * t;
            const batch = Math.min(this.batch, Math.max(1, Math.floor(this.maxRunPixels / perTile)), Math.max(1, Math.floor(bufferCap / worstBytes(perTile))));
            if (worstBytes(perTile * batch) <= bufferCap || batch === 1) {
                if (worstBytes(perTile) <= bufferCap)
                    return { tileW: t, tileH: t, batch, overlap: this.overlap };
            }
        }
        return { tileW: this.tile, tileH: this.tile, batch: 1, overlap: this.overlap };
    }
    ensureImageBuffers(w, h, cpuInput) {
        const haveInputs = !cpuInput || !!this.color;
        if (this.imgW === w && this.imgH === h && this.accum && haveInputs)
            return;
        [this.color, this.albedo, this.normal, this.accum, this.weight, this.outPixels, this.readback]
            .forEach((b) => b?.destroy());
        this.outTexture?.destroy();
        this.outTexture = undefined;
        const d = this.device;
        const px = w * h;
        const stor = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST;
        if (cpuInput) {
            this.color = d.createBuffer({ size: px * 4, usage: stor });
            this.albedo = this.channels >= 6 ? d.createBuffer({ size: px * 4, usage: stor }) : undefined;
            this.normal = this.channels >= 9 ? d.createBuffer({ size: px * 4, usage: stor }) : undefined;
        }
        else {
            this.color = this.albedo = this.normal = undefined;
        }
        this.accum = d.createBuffer({ size: 3 * px * 4, usage: stor });
        this.weight = d.createBuffer({ size: px * 4, usage: stor });
        this.outPixels = d.createBuffer({ size: px * 4, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
        this.readback = d.createBuffer({ size: px * 4, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        this.imgW = w;
        this.imgH = h;
    }
    ensureOutTexture(w, h) {
        if (!this.outTexture || this.outTexture.width !== w || this.outTexture.height !== h) {
            this.outTexture?.destroy();
            this.outTexture = this.device.createTexture({
                size: { width: w, height: h },
                format: 'rgba8unorm',
                usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING,
            });
        }
        return this.outTexture;
    }
    /** Denoise a full-resolution image (whole-frame or tiled+blended). Returns RGBA8 pixels (alpha = 255). */
    async denoise(color, w, h, opts = {}) {
        if (color.length !== w * h * 4)
            throw new Error(`Denoiser: expected ${w * h * 4} color bytes, got ${color.length}`);
        if (this.channels >= 6 && !opts.albedo)
            throw new DenoiserInputError('Denoiser: model requires an albedo input');
        if (this.channels >= 9 && !opts.normal)
            throw new DenoiserInputError('Denoiser: model requires a normal input');
        return this.process({ cpu: { color, albedo: opts.albedo, normal: opts.normal } }, w, h, opts);
    }
    /**
     * Zero-copy denoise: read float input textures on the shared device directly
     * (no CPU round-trip, no 8-bit quantization — feed HDR models real HDR).
     * With toTexture, also skips the readback and returns an rgba8unorm texture
     * (owned by the engine, valid until the next call / size change / dispose).
     */
    async denoiseTextures(inputs, opts = {}) {
        if (this.channels >= 6 && !inputs.albedo)
            throw new DenoiserInputError('Denoiser: model requires an albedo input');
        if (this.channels >= 9 && !inputs.normal)
            throw new DenoiserInputError('Denoiser: model requires a normal input');
        return this.process({ tex: inputs }, inputs.color.width, inputs.color.height, opts);
    }
    async process(src, w, h, opts) {
        const geo = await this.ensureGeo(this.planFor(w, h));
        const { tileW, tileH, batch: B, overlap } = geo.plan;
        this.ensureImageBuffers(w, h, !!src.cpu);
        const d = this.device;
        const strideX = tileW - overlap;
        const strideY = tileH - overlap;
        const tilesX = Math.max(1, Math.ceil((w - overlap) / strideX));
        const tilesY = Math.max(1, Math.ceil((h - overlap) / strideY));
        const tiles = [];
        for (let ty = 0; ty < tilesY; ty++) {
            for (let tx = 0; tx < tilesX; tx++) {
                const startX = tx * strideX;
                const startY = ty * strideY;
                tiles.push({
                    startX, startY, tx, ty,
                    curW: Math.min(tileW, w - startX),
                    curH: Math.min(tileH, h - startY),
                });
            }
        }
        const total = tiles.length;
        const batches = Math.ceil(total / B);
        const tStart = performance.now();
        if (src.cpu) {
            // See ArrayBuffer-vs-ArrayBufferLike note above.
            d.queue.writeBuffer(this.color, 0, src.cpu.color);
            if (src.cpu.albedo)
                d.queue.writeBuffer(this.albedo, 0, src.cpu.albedo);
            if (src.cpu.normal)
                d.queue.writeBuffer(this.normal, 0, src.cpu.normal);
        }
        const clr = d.createCommandEncoder();
        clr.clearBuffer(this.accum);
        clr.clearBuffer(this.weight);
        // HDR input scale (OIDN semantics): manual value, or autoexposure computed
        // on the GPU from the color texture. 8-bit inputs default to scale 1.
        if (opts.hdr && opts.inputScale === undefined && src.tex) {
            this.ops.encodeAutoexposure(clr, src.tex.color.createView(), w, h);
        }
        else {
            this.ops.setExposure(opts.inputScale ?? 1);
        }
        d.queue.submit([clr.finish()]);
        const tUpload = performance.now();
        const albedoBuf = this.albedo ?? this.color;
        const normalBuf = this.normal ?? this.color;
        const colorView = src.tex?.color.createView();
        const albedoView = src.tex ? (src.tex.albedo ?? src.tex.color).createView() : undefined;
        const normalView = src.tex ? (src.tex.normal ?? src.tex.color).createView() : undefined;
        const offsets = new Uint32Array(Math.max(this.ops.maxBatch, B) * 2);
        let encodeMs = 0;
        let runMs = 0;
        let done = 0;
        for (let b0 = 0; b0 < total; b0 += B) {
            const chunk = tiles.slice(b0, b0 + B);
            let t0 = performance.now();
            for (let i = 0; i < chunk.length; i++) {
                offsets[i * 2] = chunk[i].startX;
                offsets[i * 2 + 1] = chunk[i].startY;
            }
            const e1 = d.createCommandEncoder();
            if (src.tex) {
                this.ops.encodeExtractTilesTex(e1, colorView, albedoView, normalView, geo.nchwInput, w, h, tileW, tileH, this.channels, !!opts.srgb, !!opts.inputFlipY, !!opts.hdr, !!(opts.auxInputFlipY ?? opts.inputFlipY), offsets, chunk.length);
            }
            else {
                this.ops.encodeExtractTiles(e1, this.color, albedoBuf, normalBuf, geo.nchwInput, w, h, tileW, tileH, this.channels, !!opts.srgb, !!opts.hdr, offsets, chunk.length);
            }
            // Split mode: compute enc_conv0 (nchwInput -> encFeat) ourselves, then run
            // the tail with BOTH the feature map and the raw input (dec_conv1a skip).
            if (this.split) {
                this.ops.encodeEncConv0(e1, geo.nchwInput, this.split.weightBuf, this.split.biasBuf, geo.encFeat, tileW, tileH, this.channels, this.split.encOutChannels, B);
            }
            d.queue.submit([e1.finish()]);
            const t1 = performance.now();
            encodeMs += t1 - t0;
            // Unused slots of a short final batch still run through the model with
            // stale (valid float) contents; their outputs are simply never blended.
            const feeds = this.split
                ? {
                    [this.split.featInputName]: geo.encTensor,
                    [this.split.rawInputName]: geo.inputTensor,
                }
                : { [this.inputName]: geo.inputTensor };
            await geo.session.run(feeds, { [this.outputName]: geo.outputTensor });
            t0 = performance.now();
            runMs += t0 - t1;
            const e2 = d.createCommandEncoder();
            chunk.forEach((tl, i) => {
                this.ops.encodeAccumulateTile(e2, i, geo.outNCHW, this.accum, this.weight, {
                    imgW: w, imgH: h, startX: tl.startX, startY: tl.startY, curW: tl.curW, curH: tl.curH,
                    tileX: tl.tx, tileY: tl.ty, tilesX, tilesY, tileW, tileH, overlap,
                    batchIdx: i,
                });
            });
            d.queue.submit([e2.finish()]);
            encodeMs += performance.now() - t0;
            done += chunk.length;
            opts.onProgress?.(done / total);
        }
        const tTiles = performance.now();
        let out;
        if (opts.outputTexture || opts.toTexture) {
            let tex = opts.outputTexture;
            if (tex) {
                if (tex.width !== w || tex.height !== h) {
                    throw new DenoiserInputError(`Denoiser: outputTexture is ${tex.width}x${tex.height}, image is ${w}x${h}`);
                }
                if (!(tex.usage & GPUTextureUsage.STORAGE_BINDING)) {
                    throw new DenoiserInputError('Denoiser: outputTexture needs STORAGE_BINDING usage');
                }
                if (tex.format !== 'rgba8unorm' && tex.format !== 'rgba16float') {
                    throw new DenoiserInputError(`Denoiser: outputTexture must be rgba8unorm or rgba16float (got ${tex.format})`);
                }
            }
            else {
                tex = this.ensureOutTexture(w, h);
            }
            const e3 = d.createCommandEncoder();
            this.ops.encodeResolveToTexture(e3, this.accum, this.weight, tex.createView(), tex.format, w, h, !!opts.srgb, !!opts.hdr, !!opts.flipY, !!opts.tonemap);
            d.queue.submit([e3.finish()]);
            await d.queue.onSubmittedWorkDone();
            out = tex;
        }
        else {
            const e3 = d.createCommandEncoder();
            this.ops.encodeResolve(e3, this.accum, this.weight, this.outPixels, w, h, !!opts.srgb, !!opts.hdr, !!opts.flipY, !!opts.tonemap);
            e3.copyBufferToBuffer(this.outPixels, 0, this.readback, 0, w * h * 4);
            d.queue.submit([e3.finish()]);
            await this.readback.mapAsync(GPUMapMode.READ);
            out = new Uint8ClampedArray(this.readback.getMappedRange().slice(0));
            this.readback.unmap();
        }
        const tEnd = performance.now();
        this.lastStats = {
            width: w, height: h, tiles: total, batches,
            tileW, tileH, batchSize: B,
            uploadMs: tUpload - tStart,
            encodeMs, runMs,
            resolveMs: tEnd - tTiles,
            totalMs: tEnd - tStart,
        };
        return out;
    }
    tileGrid(w, h) {
        const plan = this.planFor(w, h);
        const strideX = plan.tileW - plan.overlap;
        const strideY = plan.tileH - plan.overlap;
        return {
            tilesX: Math.max(1, Math.ceil((w - plan.overlap) / strideX)),
            tilesY: Math.max(1, Math.ceil((h - plan.overlap) / strideY)),
        };
    }
    /** Free per-image buffers and all non-default geometry sessions. The default
     *  session (and therefore the shared GPUDevice) stays alive. */
    trim() {
        [this.color, this.albedo, this.normal, this.accum, this.weight, this.outPixels, this.readback]
            .forEach((b) => b?.destroy());
        this.color = this.albedo = this.normal = this.accum = this.weight =
            this.outPixels = this.readback = undefined;
        this.outTexture?.destroy();
        this.outTexture = undefined;
        this.imgW = this.imgH = 0;
        let first = true;
        for (const [key, g] of this.geos) {
            if (first) {
                first = false;
                continue;
            }
            this.releaseGeo(g);
            this.geos.delete(key);
        }
    }
    /** Full teardown. Releasing the last ORT session DESTROYS the shared
     *  GPUDevice — anything else using it (three.js, canvases) dies with it. */
    destroy() {
        this.trim();
        this.geos.forEach((g) => this.releaseGeo(g));
        this.geos.clear();
    }
}
/**
 * Temporarily patch requestAdapter so the device ORT is about to create gets
 * the adapter's FULL limits + features (ORT requests a minimal device, which
 * caps storage buffers at ~128-256MB — far too small for whole-frame U-Net
 * intermediates, and too small for three.js path tracers sharing the device).
 * Returns a restore function.
 */
function patchForMaxLimits() {
    if (!('gpu' in navigator))
        return () => undefined;
    const gpu = navigator.gpu;
    const origRequestAdapter = gpu.requestAdapter.bind(gpu);
    gpu.requestAdapter = async (adapterOpts) => {
        const adapter = await origRequestAdapter(adapterOpts);
        if (!adapter)
            return adapter;
        const origRequestDevice = adapter.requestDevice.bind(adapter);
        adapter.requestDevice = (desc = {}) => {
            const requiredLimits = {};
            const proto = Object.getPrototypeOf(adapter.limits);
            for (const name of Object.getOwnPropertyNames(proto)) {
                const v = adapter.limits[name];
                if (typeof v === 'number')
                    requiredLimits[name] = v;
            }
            return origRequestDevice({
                ...desc,
                requiredFeatures: [...adapter.features],
                requiredLimits: { ...requiredLimits, ...(desc.requiredLimits ?? {}) },
            });
        };
        return adapter;
    };
    return () => { gpu.requestAdapter = origRequestAdapter; };
}

// Resolve the OIDN model name + input channel count from the selector.
// Port of the old denoiserUtils.determineTensorMap — the resulting name maps
// directly to a converted ONNX file (e.g. rt_ldr_alb_nrm_small.onnx).
function determineModel(props) {
    let name = props.filterType; // 'rt'
    name += props.hdr ? '_hdr' : '_ldr';
    // cleanAux requires BOTH albedo and normal
    if (props.useAlbedo && props.useNormal && props.cleanAux)
        name += '_calb_cnrm';
    else {
        name += props.useAlbedo ? '_alb' : '';
        name += props.useNormal ? '_nrm' : '';
    }
    // quality -> size suffix (only available for some variants)
    const hasSmall = ['rt_hdr', 'rt_ldr', 'rt_hdr_alb', 'rt_ldr_alb', 'rt_hdr_alb_nrm',
        'rt_ldr_alb_nrm', 'rt_hdr_calb_cnrm', 'rt_ldr_calb_cnrm'];
    const hasLarge = ['rt_alb', 'rt_nrm', 'rt_hdr_calb_cnrm', 'rt_ldr_calb_cnrm'];
    let size = 'default';
    if (props.quality === 'fast' && hasSmall.includes(name))
        size = 'small';
    else if (props.quality === 'high' && hasLarge.includes(name))
        size = 'large';
    if (size !== 'default')
        name += `_${size}`;
    let channels = 0;
    if (props.useColor)
        channels += 3;
    if (props.useAlbedo)
        channels += 3;
    if (props.useNormal)
        channels += 3;
    return { name, channels };
}

//* Image helpers ----------------------------------
/** Decode any supported image input to RGBA8 bytes + dimensions via a 2D canvas. */
function imgToRGBA(input) {
    if (input instanceof ImageData) {
        return { data: input.data, width: input.width, height: input.height };
    }
    let width = 0;
    let height = 0;
    if (input instanceof HTMLImageElement) {
        width = input.naturalWidth || input.width;
        height = input.naturalHeight || input.height;
    }
    else if (typeof HTMLVideoElement !== 'undefined' && input instanceof HTMLVideoElement) {
        width = input.videoWidth;
        height = input.videoHeight;
    }
    else {
        width = input.width;
        height = input.height;
    }
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d');
    if (!ctx)
        throw new Error('Denoiser: could not get 2D canvas context');
    ctx.drawImage(input, 0, 0, width, height);
    const id = ctx.getImageData(0, 0, width, height);
    return { data: id.data, width, height };
}
/** A css-scaled <img> reports display size; redraw to get the true pixels. */
function getCorrectImageData(img) {
    const canvas = document.createElement('canvas');
    const ctx = canvas.getContext('2d');
    if (!ctx)
        throw new Error('Could not get canvas context');
    canvas.width = img.naturalWidth || img.width;
    canvas.height = img.naturalHeight || img.height;
    ctx.drawImage(img, 0, 0);
    return ctx.getImageData(0, 0, canvas.width, canvas.height);
}
function hasSizeMissmatch(img) {
    if (!img.naturalHeight || !img.naturalWidth)
        return true;
    return img.height !== img.naturalHeight || img.width !== img.naturalWidth;
}

/**
 * Browser OIDN denoiser running fully on WebGPU via onnxruntime-web (v2 API).
 *
 * Execution is stateless per call — everything about a run is in the call's
 * options. The instance owns identity only: models, sessions, and the shared
 * GPUDevice (ORT creates it; read `denoiser.device` to share with three.js).
 *
 * ```ts
 * const denoiser = await Denoiser.create({ precision: 'fp16' });
 * const img = await denoiser.denoise(noisyImage);                    // ImageData
 * const tex = await denoiser.denoiseTextures({ color, hdr: true });  // GPUTexture
 * ```
 */
class Denoiser {
    /** The shared GPUDevice ORT created — pass to three.js WebGPURenderer. */
    device;
    models;
    engine;
    activeModelName;
    aborted = false;
    opts;
    listeners = new Map();
    /** Per-stage wall-clock timings from the most recent run. */
    get stats() { return this.engine?.lastStats; }
    /** Model file the last denoise ran on (e.g. `rt_hdr_calb_cnrm`) — set after the
     *  first call; changes when the passed inputs select a different variant. */
    get modelName() { return this.activeModelName; }
    get quality() { return this.opts.quality; }
    set quality(q) { this.opts.quality = q; }
    constructor(opts) {
        // splitAux defaults ON so 9ch cleanAux "just works" (dodges the ORT-web
        // WebGPU Conv bug); falls back to the plain model if artifacts aren't hosted.
        this.opts = { quality: 'fast', splitAux: true, ...opts };
        this.models = Models.getInstance();
        if (opts.precision)
            this.models.precision = opts.precision;
        if (opts.weightsUrl)
            this.models.url = opts.weightsUrl;
    }
    /** Async construction: loads the default model and creates the GPUDevice. */
    static async create(opts = {}) {
        const d = new Denoiser(opts);
        await d.ensureEngine({ hdr: false, albedo: false, normal: false });
        return d;
    }
    // ---- execution ----------------------------------------------------------
    /** Denoise an image-like input. Returns ImageData (undefined when aborted). */
    async denoise(color, options = {}) {
        const c = toRGBA(color);
        const albedo = options.albedo !== undefined ? toRGBA(options.albedo) : undefined;
        const normal = options.normal !== undefined ? toRGBA(options.normal) : undefined;
        if ((albedo && sizeDiffers(albedo, c)) || (normal && sizeDiffers(normal, c))) {
            throw new DenoiserInputError('aux inputs must match the color input size');
        }
        await this.ensureEngine({ hdr: false, albedo: !!albedo, normal: !!normal });
        this.aborted = false;
        const srgb = options.srgb ?? true; // photographs/screens are sRGB-encoded
        const out = await this.engine.denoise(c.data, c.width, c.height, {
            albedo: albedo?.data,
            normal: normal?.data,
            // RGBA8 bytes are display-encoded; upstream treats that as identity
            // (srgb -> Linear). srgb:false means "my bytes are linear" -> encode.
            srgb: !srgb,
            hdr: false,
            flipY: options.flipY,
            onProgress: this.progressCb(options.onProgress),
        });
        if (this.aborted)
            return this.emitExecuted(undefined);
        // TS 5.7+ lib.dom typings narrowed ImageData's data param to Uint8ClampedArray<ArrayBuffer>;
        // this engine never uses SharedArrayBuffer, so the underlying buffer is always ArrayBuffer.
        const img = new ImageData(out, c.width, c.height);
        this.emitExecuted(img);
        return img;
    }
    /** Denoise, returning normalized RGBA floats instead of ImageData. */
    async denoiseToFloat(color, options = {}) {
        const img = await this.denoise(color, options);
        if (!img)
            return undefined;
        const f = new Float32Array(img.data.length);
        for (let i = 0; i < f.length; i++)
            f[i] = img.data[i] / 255;
        return f;
    }
    /**
     * Zero-copy denoise: float textures in, GPUTexture out. With `output` the
     * result lands in YOUR texture (rgba8unorm / rgba16float, STORAGE_BINDING);
     * otherwise an engine-owned rgba8unorm texture is returned (valid until the
     * next call / size change / teardown). Returns undefined when aborted.
     */
    async denoiseTextures(options) {
        const albedo = this.resolveAux(options, 'albedo');
        const normal = this.resolveAux(options, 'normal');
        await this.ensureEngine({
            hdr: !!options.hdr, albedo: !!albedo, normal: !!normal,
        });
        this.aborted = false;
        const transfer = options.transfer ?? 'linear';
        const out = await this.engine.denoiseTextures({ color: options.color, albedo, normal }, {
            hdr: options.hdr,
            inputScale: options.inputScale,
            srgb: false, // float texture inputs are linear
            tonemap: transfer === 'aces-srgb',
            // resolve's srgb flag = encode output; the extract side ignores it for hdr
            ...(transfer === 'srgb' ? { srgb: true } : {}),
            inputFlipY: options.inputFlipY,
            auxInputFlipY: options.auxInputFlipY,
            flipY: options.outputFlipY,
            toTexture: true,
            outputTexture: options.output,
            onProgress: this.progressCb(options.onProgress),
        });
        const tex = this.aborted ? undefined : out;
        this.emitExecuted(tex);
        return tex;
    }
    /** Drop the in-flight run's result (its GPU work still completes). */
    abort() { this.aborted = true; }
    // ---- lifecycle ----------------------------------------------------------
    /**
     * Release image buffers and extra sessions but KEEP the shared GPUDevice
     * alive (one model session is retained). Safe to call between workloads.
     */
    dispose() { this.engine?.trim(); }
    /**
     * Full teardown. Releasing the last ORT session DESTROYS the shared
     * GPUDevice — three.js renderers and canvases on it die too. Only call
     * this when the whole WebGPU stack is going away.
     */
    destroyDevice() { this.engine?.destroy(); }
    // ---- events -------------------------------------------------------------
    on(event, cb) {
        if (!this.listeners.has(event))
            this.listeners.set(event, new Set());
        this.listeners.get(event).add(cb);
        return () => this.listeners.get(event)?.delete(cb);
    }
    // ---- internals ----------------------------------------------------------
    progressCb(local) {
        const set = this.listeners.get('progress');
        if (!local && !set?.size)
            return undefined;
        return (p) => {
            local?.(p);
            set?.forEach((cb) => cb(p));
        };
    }
    emitExecuted(value) {
        this.listeners.get('executed')?.forEach((cb) => cb(value));
        return value;
    }
    /** (Re)build the engine when the required model changes. Overlaps creation
     *  with disposal so the ORT session count never hits zero (device survives). */
    async ensureEngine(sel) {
        const { name, channels } = determineModel({
            filterType: 'rt', quality: this.opts.quality, hdr: sel.hdr,
            useColor: true, useAlbedo: sel.albedo, useNormal: sel.normal,
            cleanAux: sel.albedo && sel.normal});
        if (this.engine && this.activeModelName === name)
            return;
        // Aux split-graph workaround: for 9ch cleanAux models, fetch a re-exported
        // tail + enc_conv0 weights and run enc_conv0 in WGSL (dodges the ORT-web
        // WebGPU Conv bug). The tail REPLACES the model bytes.
        const split = await this.loadSplitArtifacts(name, channels);
        const create = async () => DenoiseEngine.create(split ? split.tailBytes : await this.models.get(name), {
            channels,
            wasmPaths: this.opts.wasmPaths,
            graphCapture: this.opts.graphCapture,
            batch: this.opts.batch,
            maxRunPixels: this.opts.maxRunPixels,
            precision: this.models.precision,
            split: split?.engine,
        });
        const old = this.engine;
        try {
            this.engine = await create();
        }
        catch (err) {
            if (this.models.precision !== 'fp16')
                throw err;
            console.warn('Denoiser: fp16 unavailable, falling back to fp32', err);
            this.models.precision = 'fp32';
            this.engine = await create();
        }
        old?.destroy();
        this.activeModelName = name;
        this.device = this.engine.device;
    }
    /**
     * Fetch the split-graph artifacts (tail model + first-conv weights) for a
     * cleanAux model when splitAux is on. Returns undefined otherwise (incl. when
     * artifacts aren't hosted — falls back to the plain model with a warning, so
     * default-on splitAux never hard-fails). Artifacts live next to the model,
     * with the same precision suffix: `<name>[.fp16].tail.onnx` and
     * `<name>[.fp16].enc0.bin` (f32 OIHW weights [COUT,channels,3,3] then bias).
     */
    async loadSplitArtifacts(name, channels) {
        if (!this.opts.splitAux || channels < 9)
            return undefined;
        const m = this.models;
        const suffix = m.precision === 'fp16' ? '.fp16' : ''; // mirror Models.fileFor
        const stem = m.url ? `${m.url}/${name}${suffix}` : `/${m.path ?? 'models'}/${name}${suffix}`;
        const fetchBytes = async (url) => {
            const r = await fetch(url);
            // A dev server's SPA fallback (e.g. Vite's default appType) can answer a
            // missing artifact with a 200 + index.html rather than a 404 — treat any
            // HTML response as "not found" too, or the bogus bytes below throw a
            // confusing low-level error instead of the intended graceful fallback.
            const contentType = r.headers.get('content-type') ?? '';
            if (!r.ok || contentType.includes('text/html')) {
                throw new Error(`${url} (${r.status}${contentType ? `, ${contentType}` : ''})`);
            }
            return r.arrayBuffer();
        };
        let tail, encBuf;
        try {
            [tail, encBuf] = await Promise.all([
                fetchBytes(`${stem}.tail.onnx`),
                fetchBytes(`${stem}.enc0.bin`),
            ]);
        }
        catch (err) {
            // Artifacts missing/unreachable — run the plain (speckled) model instead of
            // failing. Aux on WebGPU has a known ORT bug; this is the fallback path.
            if (!this.splitWarned) {
                this.splitWarned = true;
                console.warn(`Denoiser: splitAux artifacts unavailable for ${name}${suffix}, aux will speckle (ORT-web WebGPU Conv bug). Host <name>.tail.onnx + <name>.enc0.bin next to the model, or set splitAux:false to silence.`, err);
            }
            return undefined;
        }
        const f = new Float32Array(encBuf);
        // f = [COUT*channels*9 weights][COUT bias]  ->  len = COUT*(channels*9 + 1)
        const cout = Math.round(f.length / (channels * 9 + 1));
        return {
            tailBytes: new Uint8Array(tail),
            engine: {
                encWeights: f.slice(0, cout * channels * 9),
                encBias: f.slice(cout * channels * 9, cout * channels * 9 + cout),
                encOutChannels: cout,
            },
        };
    }
    splitWarned = false;
    /** Aux textures are optional, but one PASSED yet resolving to undefined (e.g. a
     *  failed render-target unwrap) would silently degrade to color-only — warn
     *  once per input rather than dropping it quietly. */
    resolveAux(options, name) {
        const tex = options[name];
        if (tex)
            return tex;
        if (name in options && !this.warnedAux.has(name)) {
            this.warnedAux.add(name);
            console.warn(`Denoiser: ${name} texture could not be unwrapped — falling back to color-only model`);
        }
        return undefined;
    }
    warnedAux = new Set();
}
function toRGBA(input) {
    if (input && typeof input === 'object' && 'data' in input && input.data instanceof Uint8ClampedArray
        && !(input instanceof ImageData)) {
        const raw = input;
        if (raw.data.length !== raw.width * raw.height * 4) {
            throw new DenoiserInputError('raw input must be RGBA8 (width*height*4 bytes)');
        }
        return raw;
    }
    let source = input;
    if (source instanceof HTMLImageElement && hasSizeMissmatch(source)) {
        source = getCorrectImageData(source);
    }
    return imgToRGBA(source);
}
function sizeDiffers(a, b) {
    return a.width !== b.width || a.height !== b.height;
}

export { DenoiseEngine, Denoiser, DenoiserInputError, DenoiserUnsupportedError, Models, determineModel };
//# sourceMappingURL=index.mjs.map
