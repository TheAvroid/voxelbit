// ---------------------------------------------------------------------------
// neural.h -- RTX neural shading: the capability gate.
//
// WHAT "RTX NEURAL SHADERS" ACTUALLY IS, underneath the name: the ability to
// evaluate a small neural network INSIDE a shader, on the tensor hardware,
// through a type the shading language understands. The type is a cooperative
// vector -- a vector whose elements live across a subgroup so a matrix multiply
// can be issued as one instruction instead of a loop -- and the language
// construct is coopVecMatMul.
//
// It is not a library v2 links. It is a capability of the compiler, the driver
// and the device together, and all three have to agree.
//
// ---------------------------------------------------------------------------
// THE FOUR THINGS THAT HAVE TO BE TRUE, and where each of them was won:
//
//   THE COMPILER MUST KNOW THE TYPE. Falcor 8.0's packman Slang is 2024.1.34
//   and contains no CoopVec at all -- not a stub, not an error message, the
//   identifier simply does not exist. v2 ships its own Slang 2026.13.1 instead,
//   overridden per BUILD TREE so that v6, which shares the Falcor checkout, goes
//   on compiling against the old one. build.bat has the long version.
//
//   IT MUST LOWER TO THE RIGHT EXTENSION. On SPIR-V that is
//   SPV_NV_cooperative_vector, which 2026.13.1 emits and selects automatically
//   -- nothing to configure. On DXIL it is Shader Model 6.10 plus a
//   dx/linalg.h that ships only with the preview DirectX compiler, and the
//   generated HLSL includes that header itself, so the path has to be handed to
//   the DOWNSTREAM compiler rather than to Slang. Both roads are wired up; see
//   init() for which one this device took.
//
//   THE RUNTIME MUST ACCEPT IT. On Vulkan that means VK_NV_cooperative_vector
//   enabled at device creation -- which slang-gfx does itself, so a Falcor
//   Vulkan device already carries it and no hand-built device is needed.
//   (VK_NV_cluster_acceleration_structure, for Mega Geometry, is NOT in
//   gfx.dll's list, which is why clusters will need the device-adoption path in
//   Device::Desc and this does not.)
//
//   On D3D12 it means the PREVIEW Agility SDK -- 1.721.2-preview, D3D12 SDK
//   version 620, in place of the 1.4.10 Falcor ships -- because the stock
//   runtime will not load a 6.10 shader at all. That runtime additionally
//   refuses to load unless Windows Developer Mode is on, which is a machine
//   setting and not something the engine can arrange for itself.
//
//   THE DRIVER MUST EXPOSE IT. 610.88 does, at revision 4, on every RTX card
//   from Turing up. Ada runs the matmuls through the DP4a/FP16 path rather than
//   Blackwell's in-shader tensor cores -- correct results, a fraction of the
//   throughput -- so this is a capability to be spent carefully on a 4070, not
//   a free one.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE IS A PROBE AND NOT A NETWORK.
//
// Four moving parts across two vendors and a preview-grade language feature is
// exactly the situation where "it should work" is worth nothing. So the first
// thing v2 does with cooperative vectors is COMPILE ONE, at startup, and say
// plainly whether it worked -- before any of the renderer depends on it.
//
// It follows the pattern every optional subsystem in this engine already uses:
// try, fail politely, report, and let the frame carry on without it. A missing
// capability that turns into a wall of shader errors mid-frame is worse than
// one that is simply not there.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/Pass/ComputePass.h"
#include "Core/Program/Program.h"
#include "Core/Platform/OS.h"

#include <filesystem>

#include <string>

namespace v2 {

// Ask for the cooperative-vector capability BY NAME instead of letting Slang
// discover it.
//
// Without this every one of these shaders compiles with a warning -- "profile
// implicitly upgraded ... spvCooperativeVectorNV" -- printed at startup, three
// times, in an engine whose startup output is otherwise all load-bearing. The
// warning is correct and harmless; it is saying the shader needed a capability
// the profile did not name, and Slang added it. Naming it is the fix.
//
// VULKAN ONLY. The DXIL path reaches cooperative vectors through Shader Model
// 6.10 rather than a SPIR-V capability, and asking for a SPIR-V capability on a
// D3D12 target is an error rather than a no-op.
inline void nrcAddCapability(const Falcor::ref<Falcor::Device> &device, Falcor::ProgramDesc &d)
{
    if (device->getType() == Falcor::Device::Type::Vulkan)
        d.addCompilerArguments({"-capability", "spvCooperativeVectorNV"});
}

class Neural {
  public:
    // Try to bring cooperative vectors up. Never throws, never fatal.
    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;

        // ---------------------------------------------------------------
        // THE TWO BACKENDS REACH THIS BY COMPLETELY DIFFERENT ROADS, and the
        // shader source is identical on both -- which is the argument for
        // writing it in Slang rather than against either extension directly.
        //
        //   VULKAN   VK_NV_cooperative_vector, enabled by slang-gfx itself.
        //            Nothing to ask for and nothing to configure: the SPIR-V
        //            path just works on a production driver.
        //
        //   D3D12    Shader Model 6.10, which is NOT the default. Falcor's own
        //            enum stopped at 6.7 (patched, see Core/API/Types.h), the
        //            runtime needs the preview Agility SDK, and the generated
        //            HLSL includes dx/linalg.h from the preview DXC. All three
        //            are wired up in v2's Falcor fork; what remains is asking.
        // ---------------------------------------------------------------
        Falcor::ProgramDesc d;
        d.addShaderLibrary("v2/shaders/NeuralProbe.cs.slang").csEntry("main");
        nrcAddCapability(device_, d);

        if (device_->getType() == Falcor::Device::Type::D3D12) {
            // 6.10 rather than 6.9, and the distinction is not academic:
            // dx/linalg.h gates its whole body on __SHADER_TARGET_MINOR >= 10,
            // so a 6.9 target finds the header and then compiles nothing out of
            // it -- "no member named 'linalg' in namespace 'dx'", which reads
            // like a missing file and is not one.
            if (!device_->isShaderModelSupported(Falcor::ShaderModel::SM6_10)) {
                status_ = "D3D12: device reports no SM 6.10 -- cooperative vectors "
                          "need it (is the preview Agility SDK deployed, and "
                          "Windows Developer Mode on?)";
                return false;
            }
            d.setShaderModel(Falcor::ShaderModel::SM6_10);

            // dx/linalg.h is included by the HLSL Slang GENERATES, not by
            // anything in this repository, so the search path has to reach the
            // downstream compiler rather than Slang's own include resolver --
            // hence -Xdxc. The header ships with the preview DXC and is staged
            // beside it in the fork.
            const std::filesystem::path inc =
                Falcor::getRuntimeDirectory() / "dxc-inc" / "hlsl";
            d.addCompilerArguments({"-Xdxc", "-I" + inc.string()});
        }

        // THE COMPILE IS THE TEST. Everything above is a precondition; this is
        // the only line that actually proves the chain holds.
        try {
            probe_ = Falcor::ComputePass::create(device_, d);
        } catch (const std::exception &e) {
            status_ = std::string("cooperative vector shader did not compile: ") + e.what();
            return false;
        }
        if (!probe_) {
            status_ = "cooperative vector shader did not compile";
            return false;
        }

        ready_ = true;
        status_ = device_->getType() == Falcor::Device::Type::D3D12
                      ? "cooperative vectors ready (D3D12, Shader Model 6.10)"
                      : "cooperative vectors ready (VK_NV_cooperative_vector)";
        return true;
    }

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }

  private:
    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> probe_;
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v2
