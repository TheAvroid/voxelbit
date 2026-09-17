// ---------------------------------------------------------------------------
// store.h -- the host half of the seam. See shaders/Store.slang for the other.
//
// A STORE OWNS EVERYTHING ABOUT GEOMETRY AND NOTHING ELSE. It holds the voxels
// however it likes, uploads them however it likes, and answers rays through the
// two functions in its .slang file. The renderer knows five verbs:
//
//     build / shutdown     lifecycle
//     update(camPos)       a chance to stream, page or rebuild
//     bind(var)            put EVERYTHING the shader needs on this root var
//     addDefines(defs)     select the implementation, and any of its options
//
// bind() TAKES THE CONSTANTS TOO, AND THAT IS THE WHOLE DESIGN DECISION.
//
// The engine this was cut from had the renderer building the store's constant
// buffer by hand at three separate call sites -- a struct in the shared header,
// a setBlob per pass, and a field layout the tracer had to know. Every one of
// those is a place a new backend would have to edit code that has nothing to do
// with it. Here a store binds its own resources and its own constants in one
// call, so a backend with a wildly different shape -- a BLAS of brick AABBs, a
// dense bitmask, an SVO -- is a new pair of files and no edits at all.
//
// ---------------------------------------------------------------------------
// ready() DOES NOT GATE RENDERING, AND MUST NOT BE MADE TO.
//
// It reports whether there is anything to hit, for the banner and the overlay.
// The trace runs regardless: a store with nothing in it simply misses, the sky
// is what comes back, and every pass downstream keeps running. Gating the
// dispatch on it -- which is what the old vdbOk() checks did -- means an engine
// with no backend draws black, and black is indistinguishable from a broken
// atmosphere, a broken exposure or a broken swapchain. This engine's whole
// premise is that you can look at the lighting with no world behind it.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/Program/Program.h"
#include "Core/Program/ShaderVar.h"

#include "../core/vecmath.h"

namespace tpl {

using Falcor::ref;
using Falcor::Device;
using Falcor::DefineList;
using Falcor::RenderContext;
using Falcor::ShaderVar;

// ---------------------------------------------------------------------------
// THE NULL STORE. Holds nothing, binds nothing, misses everything.
//
// It is the reference implementation of the interface, not a placeholder: it
// stays once real backends exist, because it is the zero every measurement of
// them is taken against. Profile this and you have the frame cost with the
// geometry query removed entirely -- ray setup, the lighting passes, the
// denoiser. A backend's true cost is its profile minus this one.
//
// COPY THIS CLASS TO MAKE A BACKEND. Keep the five verbs, change nothing about
// their signatures, and put the new .slang beside stores/Null.slang.
// ---------------------------------------------------------------------------
class NullStore {
  public:
    bool build(const ref<Device> &device, RenderContext *ctx) {
        (void)device;
        (void)ctx;
        return true;
    }

    void shutdown() {}

    // The camera moved. A paged backend re-centres its window here; this one
    // has no window and no pages.
    void update(Vec3 camPos) { (void)camPos; }

    // EVERYTHING the shader side needs, on one root var: buffers, textures,
    // acceleration structures, constants. The null store needs none of it, and
    // an empty body here is the proof that the renderer is not secretly
    // binding something on a backend's behalf.
    void bind(const ShaderVar &var) const { (void)var; }

    // Which stores/*.slang the trace programs compile against. The quotes are
    // part of the value -- the shader does `#include TPL_STORE_IMPL`, so the
    // define has to expand to a quoted path, and an unquoted one fails inside
    // the preprocessor with an error naming neither this file nor that one.
    void addDefines(DefineList &defs) const { defs.add("TPL_STORE_IMPL", "\"stores/Null.slang\""); }

    // Informational ONLY. See the note at the top of this file: nothing in the
    // render path may branch on this.
    bool ready() const { return false; }

    const char *status() const { return "null -- no geometry, every ray misses"; }
};

// The engine builds against exactly one store. Swapping backends is this
// alias plus the include above it, which is deliberate: selecting at runtime
// would mean the trace shader could not specialise on the backend, and a
// software march that cannot inline its own traversal is not worth measuring.
using Store = NullStore;

}  // namespace tpl
