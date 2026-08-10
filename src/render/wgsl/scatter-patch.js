  const SCATTER_SRC = () => /* wgsl */`
    struct SP { g0 : u32, pad0 : u32, pad1 : u32, pad2 : u32 }
    @group(0) @binding(0) var<uniform> sp : SP;
    @group(0) @binding(1) var<storage, read> stag : array<u32>;
    @group(0) @binding(2) var<storage, read_write> world : array<u32>;
    @compute @workgroup_size(256)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      let i = gid.x;
      if (i >= ${STRIPW}u) { return; }
      let r = i >> 1u; let sxw = i & 1u;
      let z = r / ${WY}u; let y = r % ${WY}u;
      world[(sp.g0 >> 2u) + sxw + y * ${WX >> 2}u + z * ${(WX * WY) >> 2}u] = stag[i];
    }
  `;

  // voxel patch scatter — applies a staged (wordIndex, value) list to the world buffer in one dispatch.
  // Duplicate indices are harmless: values are read from the CPU world at flush time, so every writer of
  // a given address stores the SAME final word (a benign race, not a nondeterministic one).
  const PATCHW_SRC = () => /* wgsl */`
    struct PC { n : u32, pad0 : u32, pad1 : u32, pad2 : u32 }
    @group(0) @binding(0) var<uniform> pc : PC;
    @group(0) @binding(1) var<storage, read> pdata : array<u32>;   // NOT named 'patch': that is a RESERVED WGSL keyword, and the failed
    @group(0) @binding(2) var<storage, read_write> world : array<u32>;   // module compile invalidates every command buffer it is encoded into
    @compute @workgroup_size(64)
    fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
      let i = gid.x;
      if (i >= pc.n) { return; }
      world[pdata[i * 2u]] = pdata[i * 2u + 1u];
    }
  `;

