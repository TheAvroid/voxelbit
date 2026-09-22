// ---------------------------------------------------------------------------
// cuda.h -- CUDA alongside the renderer, on the same device, sharing the same
//           memory.
//
// WHAT THIS IS FOR. The diagram this engine is built to has a CUDA box feeding
// the geometry pipeline -- world processing, culling, generation, streaming --
// and the reason it is CUDA and not another compute shader is the kind of work
// it is. Meshing a chunk is a compaction: every column produces a variable
// number of faces and they all have to end up packed in one array. That is a
// prefix sum and a scatter, which a compute shader can be made to do and CUDA
// does natively, with primitives that are already written and already fast.
//
// ---------------------------------------------------------------------------
// THE POINT IS THAT THERE IS NO COPY.
//
// The naive arrangement is CUDA computing into its own memory and the result
// being uploaded to the renderer. That would be slower than doing it on the CPU
// for anything the size of a chunk -- 326 MB of triangle pool does not want to
// cross the bus twice.
//
// Falcor can hand a buffer to CUDA instead. A buffer created with
// ResourceBindFlags::Shared has an OS-level shared handle; cudaImportExternal-
// Memory turns that handle into a device pointer, and from then on the same
// physical memory is a device pointer to CUDA and a structured buffer to the
// shaders. A kernel writes it, the next dispatch reads it, and nothing moves.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE IS A GATE FIRST AND A WORKLOAD SECOND.
//
// Interop is exactly the kind of thing that appears to work and is not working:
// two APIs, two views of memory, and a failure mode where each is looking at
// its own copy and both look plausible. So the first thing v2 does with CUDA is
// prove the sharing -- a kernel writes a hash that could not arrive by
// accident, and the check reads it back THROUGH FALCOR rather than through
// CUDA. Passing that means the two really are the same memory.
//
// It is the same pattern gpu/neural.h uses for cooperative vectors, for the
// same reason: establish the capability, report it plainly, and let everything
// downstream assume it only once it has been demonstrated.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#if FALCOR_HAS_CUDA
#include "Utils/CudaUtils.h"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace v2 {

// Defined in cuda/kernels.cu. Declared rather than included, because a header
// that pulled in <cuda_runtime.h> would force nvcc's view of the world onto
// every translation unit that touches the app.
namespace cuda {
void launchProbe(uint32_t *out, uint32_t count, uint32_t seed, void *stream);
void syncDevice();
uint32_t probeExpect(uint32_t i, uint32_t seed);
}  // namespace cuda

class Cuda {
  public:
    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;

#if !FALCOR_HAS_CUDA
        status_ = "Falcor was built without CUDA";
        return false;
#else
        // D3D12 AND VULKAN BOTH WORK, by different handle types -- a
        // D3D12Resource on one, an opaque Win32 handle on the other -- and
        // Falcor's importExternalMemory picks between them. So unlike the
        // neural path there is nothing to gate on the backend here.
        try {
            // Touching the device is what actually creates the CUDA context;
            // Falcor makes it lazily on first use.
            if (device_->getCudaDevice() == nullptr) {
                status_ = "no CUDA device on this adapter";
                return false;
            }
        } catch (const std::exception &e) {
            status_ = std::string("CUDA device creation failed: ") + e.what();
            return false;
        }

        if (!probe()) return false;

        ready_ = true;
        status_ = "shared-memory interop verified";
        return true;
#endif
    }

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }

  private:
#if FALCOR_HAS_CUDA
    // ---------------------------------------------------------------------
    // Write from CUDA, read back through Falcor. See the header.
    // ---------------------------------------------------------------------
    bool probe() {
        static constexpr uint32_t kCount = 4096;
        static constexpr uint32_t kSeed = 0x5EED1234u;
        try {
            using Falcor::ResourceBindFlags;
            // Shared is the flag that makes the buffer importable at all;
            // without it importExternalMemory refuses and says so.
            auto buf = device_->createBuffer(
                kCount * sizeof(uint32_t),
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess |
                    ResourceBindFlags::Shared,
                Falcor::MemoryType::DeviceLocal);

            Falcor::cuda_utils::ExternalMemory ext(buf);
            auto *ptr = reinterpret_cast<uint32_t *>(ext.getMappedData());
            if (!ptr) {
                status_ = "buffer imported but mapped to nothing";
                return false;
            }

            cuda::launchProbe(ptr, kCount, kSeed, nullptr);
            // The kernel runs on CUDA's default stream and the readback below
            // goes through Falcor, so the two need an actual barrier between
            // them rather than the assumption that one finished first.
            cuda::syncDevice();

            std::vector<uint32_t> got(kCount);
            buf->getBlob(got.data(), 0, kCount * sizeof(uint32_t));
            for (uint32_t i = 0; i < kCount; ++i) {
                if (got[i] != cuda::probeExpect(i, kSeed)) {
                    status_ = "interop check failed: CUDA and Falcor are not "
                              "looking at the same memory";
                    return false;
                }
            }
            return true;
        } catch (const std::exception &e) {
            status_ = std::string("interop unavailable: ") + e.what();
            return false;
        }
    }
#else
    bool probe() { return false; }
#endif

    Falcor::ref<Falcor::Device> device_;
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v2
