// ---------------------------------------------------------------------------
// kernels.cu -- the CUDA side of v2's world processing.
//
// COMPILED BY nvcc, LINKED INTO v2.exe. This is the only file in the engine
// that is not C++ or Slang, and it exists because there is a class of work --
// prefix sums, compaction, anything with a real dependency between threads --
// that a compute shader can be made to do and CUDA is simply better at.
//
// The buffers it writes are FALCOR'S. Nothing here allocates anything the
// renderer has to be told about: gpu/cuda.h imports a Falcor buffer into CUDA
// through the driver's external-memory interop, hands the pointer down, and the
// same memory is a structured buffer to the shaders a microsecond later. No
// copy, no staging, no readback.
// ---------------------------------------------------------------------------
#include <cstdint>

namespace v4 {
namespace cuda {

// ---------------------------------------------------------------------------
// The interop probe.
//
// Writes a pattern that could not plausibly arrive by accident -- a hash of the
// index rather than the index -- into a buffer that Falcor allocated and CUDA
// only borrowed. gpu/cuda.h then reads it back THROUGH FALCOR, not through
// CUDA, so a pass proves the two APIs are looking at the same physical memory
// rather than at two copies that happen to agree.
//
// A zero-fill would not prove that: fresh device memory is often already zero.
// ---------------------------------------------------------------------------
__global__ void probeKernel(uint32_t *out, uint32_t count, uint32_t seed)
{
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    // A cheap avalanche (Murmur3's finalizer). Deterministic, and every input
    // bit reaches every output bit, so a partial or misaligned write shows up
    // as garbage rather than as a plausible-looking number.
    uint32_t h = i ^ seed;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    out[i] = h;
}

void launchProbe(uint32_t *out, uint32_t count, uint32_t seed, void *stream)
{
    const uint32_t block = 256;
    const uint32_t grid = (count + block - 1) / block;
    probeKernel<<<grid, block, 0, (cudaStream_t)stream>>>(out, count, seed);
}

// Block until the kernel above has finished.
//
// LIVES HERE rather than in the header, so that gpu/cuda.h never has to include
// <cuda_runtime.h>. That header would otherwise be pulled into app.h and from
// there into every translation unit that touches the engine, which is a lot of
// compiler for one synchronisation.
void syncDevice()
{
    cudaDeviceSynchronize();
}

// The same hash on the host, so the check has something to compare against and
// the two definitions cannot drift apart.
uint32_t probeExpect(uint32_t i, uint32_t seed)
{
    uint32_t h = i ^ seed;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

}  // namespace cuda
}  // namespace v4
