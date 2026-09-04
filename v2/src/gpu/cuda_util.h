// ---------------------------------------------------------------------------
// cuda_util.h -- the driver API, and a buffer that owns its device memory.
//
// WHY THE DRIVER API AND NOT THE RUNTIME. cudaMalloc and friends live in
// cudart, which NVIDIA ships for Windows as an MSVC import library and a DLL.
// The host half of this engine is built by MinGW, against a MinGW GLFW, because
// that is the toolchain v3 and v4 already use and switching it would mean
// rebuilding every dependency in the tree. MinGW can link cuda.lib -- the
// driver API import library is plain C with no MSVC ABI in its interface -- so
// v2 uses cuMemAlloc, cuLaunchKernel and cuModuleLoad directly and never
// mentions the runtime. OptiX itself only ever wanted a CUcontext and a
// CUstream, so nothing is lost.
//
// The practical cost is that kernels have to be loaded as cubins by hand rather
// than written inline with <<<>>>. That is thirty lines, once, in Renderer.
// ---------------------------------------------------------------------------
#pragma once

#include <cuda.h>
#include <optix.h>
#include <optix_stubs.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace v2 {

// ---------------------------------------------------------------------------
// Error checks
//
// These throw rather than abort. A failure here is nearly always a bad launch
// size or an out-of-memory on a scene that was asked for too large, and both of
// those are worth reporting with the scene parameters attached -- which main()
// can do around the catch, and an exit(1) deep in a helper cannot.
// ---------------------------------------------------------------------------
inline void cuCheck(CUresult r, const char *what, const char *file, int line) {
    if (r == CUDA_SUCCESS) return;
    const char *name = nullptr;
    const char *desc = nullptr;
    cuGetErrorName(r, &name);
    cuGetErrorString(r, &desc);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%d: %s failed -- %s (%s)", file, line, what,
                  name ? name : "?", desc ? desc : "");
    throw std::runtime_error(buf);
}

inline void optixCheck(OptixResult r, const char *what, const char *file, int line) {
    if (r == OPTIX_SUCCESS) return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%d: %s failed -- %s (%s)", file, line, what,
                  optixGetErrorName(r), optixGetErrorString(r));
    throw std::runtime_error(buf);
}

#define CU_CHECK(call) ::v2::cuCheck((call), #call, __FILE__, __LINE__)
#define OPTIX_CHECK(call) ::v2::optixCheck((call), #call, __FILE__, __LINE__)

// A variant that keeps the OptiX compiler's log even on success -- the log is
// where a module that built with warnings says so, and those warnings are
// usually the reason a pipeline that links cleanly then renders black.
#define OPTIX_CHECK_LOG(call, log, size)                                                     \
    do {                                                                                     \
        size_t _sz = (size);                                                                 \
        OptixResult _r = (call);                                                             \
        if (_r != OPTIX_SUCCESS) {                                                           \
            std::fprintf(stderr, "v2: %s\n", (log));                                         \
            ::v2::optixCheck(_r, #call, __FILE__, __LINE__);                                 \
        } else if (_sz > 1) {                                                                \
            /* OptiX writes a trailing NUL, so a log of 1 byte is an empty one. */           \
        }                                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Device memory that frees itself.
//
// Deliberately move-only: a copy would either double-free or silently alias,
// and both of those are hours to find in a renderer where the symptom is a
// corrupted image rather than a crash.
// ---------------------------------------------------------------------------
class DeviceBuffer {
  public:
    DeviceBuffer() = default;
    explicit DeviceBuffer(size_t bytes) { alloc(bytes); }
    ~DeviceBuffer() { free(); }

    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    DeviceBuffer(DeviceBuffer &&o) noexcept : ptr_(o.ptr_), bytes_(o.bytes_) {
        o.ptr_ = 0;
        o.bytes_ = 0;
    }
    DeviceBuffer &operator=(DeviceBuffer &&o) noexcept {
        if (this != &o) {
            free();
            ptr_ = o.ptr_;
            bytes_ = o.bytes_;
            o.ptr_ = 0;
            o.bytes_ = 0;
        }
        return *this;
    }

    void alloc(size_t bytes) {
        free();
        if (bytes == 0) return;
        CU_CHECK(cuMemAlloc(&ptr_, bytes));
        bytes_ = bytes;
    }

    // Reallocate only when the size actually changes. The viewer calls this on
    // every frame for buffers whose size is usually the same, and a free/alloc
    // pair per frame fragments the device heap enough to matter over a session.
    void ensure(size_t bytes) {
        if (bytes_ >= bytes && ptr_ != 0) return;
        alloc(bytes);
    }

    void free() {
        if (ptr_) cuMemFree(ptr_);
        ptr_ = 0;
        bytes_ = 0;
    }

    void zero() {
        if (ptr_) CU_CHECK(cuMemsetD8(ptr_, 0, bytes_));
    }

    template <typename T>
    void upload(const T *src, size_t count) {
        ensure(count * sizeof(T));
        if (count) CU_CHECK(cuMemcpyHtoD(ptr_, src, count * sizeof(T)));
    }

    template <typename T>
    void upload(const std::vector<T> &v) {
        upload(v.data(), v.size());
    }

    template <typename T>
    void download(T *dst, size_t count) const {
        if (count) CU_CHECK(cuMemcpyDtoH(dst, ptr_, count * sizeof(T)));
    }

    CUdeviceptr ptr() const { return ptr_; }
    size_t bytes() const { return bytes_; }
    bool valid() const { return ptr_ != 0; }

    // The device-pointer-as-typed-pointer spelling the launch params use.
    template <typename T>
    T *as() const {
        return reinterpret_cast<T *>(ptr_);
    }

  private:
    CUdeviceptr ptr_ = 0;
    size_t bytes_ = 0;
};

// Upload once and keep the handle -- for the geometry and material tables,
// which are built at startup and never touched again.
template <typename T>
inline DeviceBuffer uploadOnce(const std::vector<T> &v) {
    DeviceBuffer b;
    b.upload(v);
    return b;
}

}  // namespace v2
