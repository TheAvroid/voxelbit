// ---------------------------------------------------------------------------
// nrd.h -- PHASE C, the auxiliary half: NVIDIA Real-Time Denoisers on ambient
//          occlusion, and ONLY on ambient occlusion.
//
// WHY NRD IS HERE AT ALL WHEN RAY RECONSTRUCTION ALREADY DENOISES.
//
// Because they denoise different things, and neither can do the other's job.
// Ray Reconstruction is a radiance reconstructor: it takes a noisy colour and
// the G-buffer that produced it and returns a clean colour. It has no input for
// a scalar visibility term and no way to return one. Ambient occlusion is
// exactly such a term -- one number per pixel, no colour, no lighting -- so it
// cannot be handed to RR at all.
//
// That makes AO the one signal in this engine that is traced, denoised and
// applied entirely outside Ray Reconstruction. Which is the whole of the rule
// "NRD only for what DLSS does not touch": not a stylistic preference, but the
// observation that the two denoisers have disjoint inputs, so no buffer is ever
// denoised twice and there is no question of them fighting.
//
// REBLUR_DIFFUSE_OCCLUSION is the right method precisely because it does not
// carry radiance. It takes IN_DIFF_HITDIST -- a normalised hit distance -- and
// returns OUT_DIFF_HITDIST, filtered. Distance rather than an occlusion factor
// is what lets it filter correctly across a depth edge: given "how far away is
// the thing occluding me", it can tell a contact shadow in a crevice from a
// trunk ten metres back and refuse to blend them. Given a binary "am I
// occluded" it could not.
//
// ---------------------------------------------------------------------------
// WHAT THIS FILE ACTUALLY DOES, because "call the denoiser" undersells it.
//
// NRD does not own a command list or any resources. It hands back a LIST OF
// DISPATCHES -- which shader, which resources bound where, what constants --
// and the integration executes them. So this file is a small driver:
//
//   createPipelines()  one root signature and one compute PSO per NRD pass,
//                      with the descriptor layout NRD describes. The shaders
//                      are NRD's own HLSL, compiled through Falcor's Slang.
//   createResources()  the permanent and transient texture pools NRD asks for,
//                      by format and size, plus its static samplers.
//   denoise()          fill CommonSettings, ask for the dispatches, and run
//                      them, binding whatever each one names.
//
// It is modelled on Falcor's own NRDPass, which is the reference integration --
// but that class is a RenderPass that needs a Falcor::Scene, and v6's voxel
// world is not one. It turns out the Scene is used for nothing but the camera
// matrices, and v6 already builds those for Streamline, so the dependency is
// removed rather than worked around.
//
// EVERY FAILURE ENDS IN "NO AO", NEVER IN A CRASH -- the same contract as every
// other phase here.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/ComputeStateObject.h"
#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Sampler.h"
#include "Core/API/Texture.h"
#include "Core/API/Shared/D3D12ConstantBufferView.h"
#include "Core/API/Shared/D3D12DescriptorSet.h"
#include "Core/API/Shared/D3D12RootSignature.h"
#include "Core/Pass/ComputePass.h"

#include <cstring>
#include <string>
#include <vector>

#if V6_HAS_NRD
#include <d3d12.h>
#include <NRD.h>
#include <NRDDescs.h>
#include <NRDSettings.h>
#endif

namespace v6 {

#if !V6_HAS_NRD
// ---------------------------------------------------------------------------
// Built without NRD: the same class, permanently unavailable. AO is then simply
// not offered, and the engine renders as it did before this file existed.
// ---------------------------------------------------------------------------
class NrdAo {
  public:
    bool init(const Falcor::ref<Falcor::Device> &) { return false; }
    bool available() const { return false; }
    const std::string &status() const { return status_; }
    bool resize(uint32_t, uint32_t) { return false; }
    bool denoise(Falcor::RenderContext *, const Falcor::ref<Falcor::Texture> &,
                 const Falcor::ref<Falcor::Texture> &, const Falcor::ref<Falcor::Texture> &,
                 const Falcor::ref<Falcor::Texture> &, const Falcor::ref<Falcor::Texture> &,
                 const float *, const float *, float, float, bool) {
        return false;
    }
    const float *hitDistParams() const { return kParams; }
    void shutdown() {}

  private:
    static constexpr float kParams[4] = {3.0f, 0.1f, 20.0f, -25.0f};
    std::string status_ = "built without NRD";
};
#else

class NrdAo {
  public:
    ~NrdAo() { shutdown(); }

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }

    // The hit-distance parameters REBLUR normalises against, and NOT a set of
    // numbers to invent. NRD computes
    //
    //     f = (A + |viewZ| * B) * lerp(1, C, saturate(exp2(D * roughness^2)))
    //     normalised = saturate(hitDist / f)
    //
    // so C and D are not optional padding: with C = 0 and D = 0 that lerp
    // evaluates to lerp(1, 0, 1) = 0, f becomes ZERO, and every pixel divides
    // by nothing and saturates to 1. The AO comes out uniformly white -- "no
    // occlusion anywhere" -- with no error from anything.
    //
    // C = 1 makes the lerp a no-op whatever the roughness, and D keeps the
    // exponent where NRD expects it, leaving f = A + |viewZ| * B. With B = 0
    // that is simply A, and A is set to the AO search radius so the ratio spans
    // 0..1 exactly over the distances that were actually traced.
    const float *hitDistParams() const { return hitDistParams_; }

    // Keep A in step with the radius the rays were cast over. The same four
    // numbers go to NrdPack.cs.slang, so there is one definition of the scale.
    void setAoRadius(float r) { hitDistParams_[0] = (r > 0.0f) ? r : 2.0f; }

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        if (!device_->getNativeHandle().as<ID3D12Device *>()) {
            status_ = "not a D3D12 device (NRD here is D3D12 only)";
            return false;
        }
        const nrd::LibraryDesc &lib = nrd::GetLibraryDesc();
        status_ = "NRD " + std::to_string(lib.versionMajor) + "." +
                  std::to_string(lib.versionMinor) + "." + std::to_string(lib.versionBuild);
        return true;
    }

    // -----------------------------------------------------------------------
    // Create (or recreate) the denoiser for a resolution.
    // -----------------------------------------------------------------------
    bool resize(uint32_t width, uint32_t height) {
        if (denoiser_ && width == w_ && height == h_) return true;
        destroyDenoiser();
        w_ = width;
        h_ = height;
        if (w_ == 0 || h_ == 0) return false;

        nrd::MethodDesc method{};
        method.method = nrd::Method::REBLUR_DIFFUSE_OCCLUSION;
        method.fullResolutionWidth = uint16_t(w_);
        method.fullResolutionHeight = uint16_t(h_);

        nrd::DenoiserCreationDesc desc{};
        desc.requestedMethods = &method;
        desc.requestedMethodNum = 1;

        if (nrd::CreateDenoiser(desc, denoiser_) != nrd::Result::SUCCESS) {
            denoiser_ = nullptr;
            status_ = "nrd::CreateDenoiser failed";
            return false;
        }
        if (!createResources() || !createPipelines()) {
            destroyDenoiser();
            return false;
        }
        frame_ = 0;
        ready_ = true;
        status_ = "ready";
        return true;
    }

    // -----------------------------------------------------------------------
    // Run the denoiser.
    //
    // The matrices come in row-major row-vector, the same convention the rest
    // of v6 uses and the same ones streamline.h is given. NRD wants
    // column-major, so they are transposed at the boundary below -- one
    // description of the camera in this engine, converted where it has to be.
    // -----------------------------------------------------------------------
    bool denoise(Falcor::RenderContext *ctx, const Falcor::ref<Falcor::Texture> &mv,
                 const Falcor::ref<Falcor::Texture> &normRough,
                 const Falcor::ref<Falcor::Texture> &viewZ,
                 const Falcor::ref<Falcor::Texture> &inHitDist,
                 const Falcor::ref<Falcor::Texture> &outHitDist, const float *viewToClip,
                 const float *worldToView, float jitterX, float jitterY, bool reset) {
        if (!ready_) return false;

        // NRD WANTS COLUMN-MAJOR. Everything else in this engine is
        // row-major row-vector -- the ray construction, the motion vectors, the
        // matrices handed to Streamline -- so the transpose happens here, at
        // the one boundary that needs it, rather than by keeping a second set
        // of matrices around. Falcor's own NRD integration does exactly this.
        nrd::CommonSettings cs{};
        transpose16(viewToClip, cs.viewToClipMatrix);
        transpose16(worldToView, cs.worldToViewMatrix);
        transpose16(havePrev_ ? prevViewToClip_ : viewToClip, cs.viewToClipMatrixPrev);
        transpose16(havePrev_ ? prevWorldToView_ : worldToView, cs.worldToViewMatrixPrev);

        cs.motionVectorScale[0] = 1.0f;
        cs.motionVectorScale[1] = 1.0f;
        cs.motionVectorScale[2] = 0.0f;
        // v6's vectors are in RENDER PIXELS -- see the note on the motion guide
        // in Trace.cs.slang. Saying so is what makes the scale above a 1 rather
        // than a reciprocal of the resolution.
        cs.isMotionVectorInWorldSpace = false;
        cs.cameraJitter[0] = jitterX;
        cs.cameraJitter[1] = jitterY;
        cs.resolutionScale[0] = 1.0f;
        cs.resolutionScale[1] = 1.0f;
        cs.denoisingRange = kDenoisingRange;
        cs.frameIndex = frame_;
        // A reset throws the temporal history away. Given on the first frame
        // and on a teleport -- NOT on ordinary camera motion, which is what the
        // motion vectors exist to describe.
        cs.accumulationMode =
            (reset || !havePrev_) ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                  : nrd::AccumulationMode::CONTINUE;

        if (nrd::SetMethodSettings(*denoiser_, nrd::Method::REBLUR_DIFFUSE_OCCLUSION,
                                   &reblur_) != nrd::Result::SUCCESS) {
            status_ = "nrd::SetMethodSettings failed";
            return false;
        }

        const nrd::DispatchDesc *dispatches = nullptr;
        uint32_t dispatchNum = 0;
        if (nrd::GetComputeDispatches(*denoiser_, cs, dispatches, dispatchNum) !=
            nrd::Result::SUCCESS) {
            status_ = "nrd::GetComputeDispatches failed";
            return false;
        }

        inputs_ = {mv, normRough, viewZ, inHitDist, outHitDist};
        for (uint32_t i = 0; i < dispatchNum; ++i) dispatch(ctx, dispatches[i]);

        // NRD leaves its own root signature and descriptor heaps on the list.
        // Falcor's next pass assumes what it left behind, so the two are
        // separated by a submit -- the same contract dlss.h and ddgi.h observe.
        ctx->submit(false);

        std::memcpy(prevViewToClip_, viewToClip, sizeof(float) * 16);
        std::memcpy(prevWorldToView_, worldToView, sizeof(float) * 16);
        havePrev_ = true;
        ++frame_;
        return true;
    }

    void shutdown() { destroyDenoiser(); }

  private:
    // Anything further away than this is not denoised. The world runs to
    // kilometres but AO is a two-metre effect, so a tight range keeps REBLUR
    // from trying to find temporal correspondence across the whole valley.
    static constexpr float kDenoisingRange = 200.0f;

    Falcor::ref<Falcor::Device> device_;
    nrd::Denoiser *denoiser_ = nullptr;
    nrd::ReblurSettings reblur_{};
    float hitDistParams_[4] = {2.0f, 0.0f, 1.0f, -25.0f};

    std::vector<Falcor::ref<Falcor::Sampler>> samplers_;
    std::vector<Falcor::ref<Falcor::Texture>> permanent_, transient_;
    std::vector<Falcor::ref<Falcor::ComputePass>> passes_;
    std::vector<Falcor::ref<const Falcor::ProgramKernels>> kernels_;
    std::vector<Falcor::ref<Falcor::ComputeStateObject>> csos_;
    std::vector<Falcor::ref<Falcor::D3D12RootSignature>> rootSigs_;
    std::vector<Falcor::D3D12DescriptorSetLayout> setLayouts_;
    Falcor::D3D12DescriptorSetLayout samplerLayout_;
    Falcor::ref<Falcor::D3D12DescriptorSet> samplerSet_;

    struct Inputs {
        Falcor::ref<Falcor::Texture> mv, normRough, viewZ, inHitDist, outHitDist;
    } inputs_;

    uint32_t w_ = 0, h_ = 0, frame_ = 0;
    bool ready_ = false, havePrev_ = false;
    float prevViewToClip_[16]{}, prevWorldToView_[16]{};
    std::string status_ = "not initialised";

    static void transpose16(const float *src, float *dst) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) dst[c * 4 + r] = src[r * 4 + c];
    }

    void destroyDenoiser() {
        if (denoiser_) {
            nrd::DestroyDenoiser(*denoiser_);
            denoiser_ = nullptr;
        }
        passes_.clear();
        kernels_.clear();
        csos_.clear();
        rootSigs_.clear();
        setLayouts_.clear();
        samplers_.clear();
        permanent_.clear();
        transient_.clear();
        samplerSet_ = nullptr;
        ready_ = false;
        havePrev_ = false;
    }

    // NRD's format enum to Falcor's. Only the formats REBLUR_DIFFUSE_OCCLUSION
    // actually asks for are listed; anything else is a loud failure rather than
    // a silent guess, because a texture created in the wrong format decodes as
    // noise rather than erroring.
    static Falcor::ResourceFormat falcorFormat(nrd::Format f) {
        using F = Falcor::ResourceFormat;
        switch (f) {
            case nrd::Format::R8_UNORM: return F::R8Unorm;
            case nrd::Format::R8_UINT: return F::R8Uint;
            case nrd::Format::RG8_UNORM: return F::RG8Unorm;
            case nrd::Format::RGBA8_UNORM: return F::RGBA8Unorm;
            case nrd::Format::R16_UNORM: return F::R16Unorm;
            case nrd::Format::R16_UINT: return F::R16Uint;
            case nrd::Format::R16_SFLOAT: return F::R16Float;
            case nrd::Format::RG16_UNORM: return F::RG16Unorm;
            case nrd::Format::RG16_SFLOAT: return F::RG16Float;
            case nrd::Format::RGBA16_UNORM: return F::RGBA16Unorm;
            case nrd::Format::RGBA16_SFLOAT: return F::RGBA16Float;
            case nrd::Format::R32_SFLOAT: return F::R32Float;
            case nrd::Format::R32_UINT: return F::R32Uint;
            case nrd::Format::RG32_SFLOAT: return F::RG32Float;
            case nrd::Format::RGBA32_SFLOAT: return F::RGBA32Float;
            case nrd::Format::R10_G10_B10_A2_UNORM: return F::RGB10A2Unorm;
            case nrd::Format::R11_G11_B10_UFLOAT: return F::R11G11B10Float;
            default: return F::Unknown;
        }
    }

    bool createResources() {
        const nrd::DenoiserDesc &d = nrd::GetDenoiserDesc(*denoiser_);

        for (uint32_t i = 0; i < d.staticSamplerNum; ++i) {
            const nrd::StaticSamplerDesc &ss = d.staticSamplers[i];
            Falcor::Sampler::Desc sd;
            const bool nearest = ss.sampler == nrd::Sampler::NEAREST_CLAMP ||
                                 ss.sampler == nrd::Sampler::NEAREST_MIRRORED_REPEAT;
            const auto f = nearest ? Falcor::TextureFilteringMode::Point
                                   : Falcor::TextureFilteringMode::Linear;
            sd.setFilterMode(f, f, Falcor::TextureFilteringMode::Point);
            const bool clamp = ss.sampler == nrd::Sampler::NEAREST_CLAMP ||
                               ss.sampler == nrd::Sampler::LINEAR_CLAMP;
            const auto a = clamp ? Falcor::TextureAddressingMode::Clamp
                                 : Falcor::TextureAddressingMode::Mirror;
            sd.setAddressingMode(a, a, a);
            samplers_.push_back(device_->createSampler(sd));
        }

        const Falcor::ResourceBindFlags rw = Falcor::ResourceBindFlags::ShaderResource |
                                             Falcor::ResourceBindFlags::UnorderedAccess;
        const uint32_t poolSize = d.permanentPoolSize + d.transientPoolSize;
        for (uint32_t i = 0; i < poolSize; ++i) {
            const bool permanent = i < d.permanentPoolSize;
            const nrd::TextureDesc &td =
                permanent ? d.permanentPool[i] : d.transientPool[i - d.permanentPoolSize];
            const Falcor::ResourceFormat fmt = falcorFormat(td.format);
            if (fmt == Falcor::ResourceFormat::Unknown) {
                status_ = "NRD asked for a texture format v6 does not map (" +
                          std::to_string(int(td.format)) + ")";
                return false;
            }
            auto t = device_->createTexture2D(td.width, td.height, fmt, 1u, td.mipNum,
                                              nullptr, rw);
            t->setName(permanent ? "v6::nrdPermanent" : "v6::nrdTransient");
            (permanent ? permanent_ : transient_).push_back(t);
        }
        return true;
    }

    bool createPipelines() {
        const nrd::DenoiserDesc &d = nrd::GetDenoiserDesc(*denoiser_);

        samplerLayout_ = Falcor::D3D12DescriptorSetLayout{};
        for (uint32_t i = 0; i < d.staticSamplerNum; ++i)
            samplerLayout_.addRange(Falcor::ShaderResourceType::Sampler,
                                    d.staticSamplers[i].registerIndex, 1);
        samplerSet_ = Falcor::D3D12DescriptorSet::create(
            device_, samplerLayout_, Falcor::D3D12DescriptorSetBindingUsage::ExplicitBind);
        for (uint32_t i = 0; i < d.staticSamplerNum; ++i)
            samplerSet_->setSampler(0, i, samplers_[i].get());

        for (uint32_t i = 0; i < d.pipelineNum; ++i) {
            const nrd::PipelineDesc &pd = d.pipelines[i];

            Falcor::D3D12DescriptorSetLayout layout;
            // Range 0 is always the constant buffer; every resource range is
            // therefore offset by one when it is bound. Getting this wrong
            // binds every texture one slot early.
            layout.addRange(Falcor::ShaderResourceType::Cbv,
                            d.constantBufferDesc.registerIndex, 1);
            for (uint32_t j = 0; j < pd.descriptorRangeNum; ++j) {
                const nrd::DescriptorRangeDesc &r = pd.descriptorRanges[j];
                layout.addRange(r.descriptorType == nrd::DescriptorType::TEXTURE
                                    ? Falcor::ShaderResourceType::TextureSrv
                                    : Falcor::ShaderResourceType::TextureUav,
                                r.baseRegisterIndex, r.descriptorNum);
            }
            setLayouts_.push_back(layout);

            Falcor::D3D12RootSignature::Desc rsDesc;
            rsDesc.addDescriptorSet(samplerLayout_);
            rsDesc.addDescriptorSet(layout);
            auto rootSig = Falcor::D3D12RootSignature::create(device_, rsDesc);
            rootSigs_.push_back(rootSig);

            // NRD's own HLSL, compiled through Falcor's Slang. The defines must
            // match what NrdPack.cs.slang packs with -- particularly the
            // octahedral normal encoding, which changes the layout of
            // IN_NORMAL_ROUGHNESS.
            const std::string file =
                "nrd/Shaders/Source/" + std::string(pd.shaderFileName) + ".hlsl";
            Falcor::ProgramDesc pdesc;
            pdesc.addShaderLibrary(file).csEntry(pd.shaderEntryPointName);
            pdesc.setCompilerFlags(Falcor::SlangCompilerFlags::MatrixLayoutColumnMajor);
            pdesc.setCompilerArguments({"-Wno-30056"});
            Falcor::DefineList defines;
            defines.add("NRD_COMPILER_DXC");
            defines.add("NRD_USE_OCT_NORMAL_ENCODING", "1");
            defines.add("NRD_USE_MATERIAL_ID", "0");

            auto pass = Falcor::ComputePass::create(device_, pdesc, defines);
            auto kernels = pass->getProgram()->getActiveVersion()->getKernels(
                device_.get(), pass->getVars().get());

            Falcor::ComputeStateObjectDesc csoDesc;
            csoDesc.pProgramKernels = kernels;
            csoDesc.pD3D12RootSignatureOverride = rootSig;

            passes_.push_back(pass);
            kernels_.push_back(kernels);
            csos_.push_back(device_->createComputeStateObject(csoDesc));
        }
        return true;
    }

    Falcor::ref<Falcor::Texture> resolve(const nrd::Resource &r) {
        switch (r.type) {
            case nrd::ResourceType::IN_MV: return inputs_.mv;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return inputs_.normRough;
            case nrd::ResourceType::IN_VIEWZ: return inputs_.viewZ;
            case nrd::ResourceType::IN_DIFF_HITDIST: return inputs_.inHitDist;
            case nrd::ResourceType::OUT_DIFF_HITDIST: return inputs_.outHitDist;
            case nrd::ResourceType::TRANSIENT_POOL: return transient_[r.indexInPool];
            case nrd::ResourceType::PERMANENT_POOL: return permanent_[r.indexInPool];
            default: return nullptr;
        }
    }

    void dispatch(Falcor::RenderContext *ctx, const nrd::DispatchDesc &dd) {
        const nrd::DenoiserDesc &d = nrd::GetDenoiserDesc(*denoiser_);
        const nrd::PipelineDesc &pd = d.pipelines[dd.pipelineIndex];

        rootSigs_[dd.pipelineIndex]->bindForCompute(ctx);

        auto cb = device_->getUploadHeap()->allocate(dd.constantBufferDataSize,
                                                    Falcor::ResourceBindFlags::Constant);
        std::memcpy(cb.pData, dd.constantBufferData, dd.constantBufferDataSize);

        auto set = Falcor::D3D12DescriptorSet::create(
            device_, setLayouts_[dd.pipelineIndex],
            Falcor::D3D12DescriptorSetBindingUsage::ExplicitBind);
        auto cbv = Falcor::D3D12ConstantBufferView::create(device_, cb.getGpuAddress(),
                                                           cb.size);
        set->setCbv(0, d.constantBufferDesc.registerIndex, cbv.get());

        uint32_t resourceIndex = 0;
        for (uint32_t ri = 0; ri < pd.descriptorRangeNum; ++ri) {
            const nrd::DescriptorRangeDesc &range = pd.descriptorRanges[ri];
            for (uint32_t k = 0; k < range.descriptorNum; ++k) {
                if (resourceIndex >= dd.resourceNum) return;
                const nrd::Resource &res = dd.resources[resourceIndex++];
                auto tex = resolve(res);
                if (!tex) return;

                const auto state = res.stateNeeded == nrd::DescriptorType::TEXTURE
                                       ? Falcor::Resource::State::ShaderResource
                                       : Falcor::Resource::State::UnorderedAccess;
                for (uint16_t mip = 0; mip < res.mipNum; ++mip) {
                    const Falcor::ResourceViewInfo vi(res.mipOffset + mip, 1, 0, 1);
                    ctx->resourceBarrier(tex.get(), state, &vi);
                }

                if (range.descriptorType == nrd::DescriptorType::TEXTURE)
                    set->setSrv(ri + 1, range.baseRegisterIndex + k,
                                tex->getSRV(res.mipOffset, res.mipNum, 0, 1).get());
                else
                    set->setUav(ri + 1, range.baseRegisterIndex + k,
                                tex->getUAV(res.mipOffset, 0, 1).get());
            }
        }

        samplerSet_->bindForCompute(ctx, rootSigs_[dd.pipelineIndex].get(), 0);
        set->bindForCompute(ctx, rootSigs_[dd.pipelineIndex].get(), 1);

        ID3D12GraphicsCommandList *cmd =
            ctx->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList *>();
        ID3D12PipelineState *pso =
            csos_[dd.pipelineIndex]->getNativeHandle().as<ID3D12PipelineState *>();
        cmd->SetPipelineState(pso);
        cmd->Dispatch(dd.gridWidth, dd.gridHeight, 1);

        // Falcor did not issue that dispatch and does not know what it touched,
        // so its own state tracking has to be told the command list moved on.
        ctx->uavBarrier(inputs_.outHitDist.get());
    }
};
#endif  // V6_HAS_NRD

}  // namespace v6
