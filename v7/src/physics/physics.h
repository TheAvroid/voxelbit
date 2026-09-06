// ---------------------------------------------------------------------------
// physics.h -- NVIDIA PhysX 5: the simulation under the wood.
//
// WHAT IT REPLACES. v7 inherited scene/collide.h, which resolves the player
// against the voxel field directly: sample the height under the capsule, push
// it out, done. That is exact, it is fast, and for a person walking on terrain
// it is genuinely hard to beat -- which is why it is still here and still the
// default.
//
// What it cannot do is anything ELSE moving. A rock that falls, a branch that
// is knocked loose, a body that tumbles down a slope: all of that needs a
// solver with contacts and constraints, and writing one is not a weekend. So
// PhysX comes in beside the existing collision rather than on top of it, and
// the two are switched between rather than blended -- see `enabled`.
//
// ---------------------------------------------------------------------------
// CPU ONLY, AND ON PURPOSE.
//
// PhysX has a GPU solver, and this build does not include it. The GPU build
// pulls in its own CUDA toolchain and a long list of architectures, and what
// the wood needs is a character controller and some rigid bodies -- work that
// is nowhere near the scale where GPU rigid bodies pay for themselves. The
// preset that builds it is in build_physx.bat and says the same thing.
//
// ---------------------------------------------------------------------------
// THE TERRAIN IS A HEIGHT FIELD, WHICH IS THE WHOLE REASON THIS FITS.
//
// A general triangle mesh of the resident wood would be 91 million triangles
// and PhysX would have to cook a BVH over it, which is neither fast nor
// necessary. But v7's ground IS a height field by construction -- one height
// per column, from a pure function -- and PxHeightFieldGeometry is exactly that
// primitive: a regular grid of samples, no cooking, and a collision query that
// is two lookups and a lerp.
//
// The trees and rocks are not height fields, and they are not represented here
// at all yet. Walking into a trunk is still collide.h's job.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#if V7_HAS_PHYSX
#include <PxPhysicsAPI.h>
#include <characterkinematic/PxCapsuleController.h>
#include <characterkinematic/PxControllerManager.h>
#endif

#include "../core/vecmath.h"

namespace v7 {

class Physics {
  public:
    // Off by default. It is the newest moving part in the engine and it changes
    // how the PLAYER moves, which is the one thing a person notices instantly.
    // ON. There is no longer a switch for this: PhysX comes up with the engine
    // or it does not come up at all, and which of those happened is in the
    // startup line rather than in a setting anybody has to find.
    bool enabled = true;

    // Metres. The capsule the player is, and the step it can walk up without
    // jumping -- a voxel is 10 cm, so a step height below that would catch on
    // the lattice the ground is made of.
    float radius = 0.35f;
    float height = 1.3f;
    float stepOffset = 0.35f;
    float slopeLimitDeg = 55.0f;

    bool init() {
#if !V7_HAS_PHYSX
        status_ = "built without PhysX";
        return false;
#else
        foundation_ = PxCreateFoundation(PX_PHYSICS_VERSION, allocator_, errors_);
        if (!foundation_) {
            status_ = "PxCreateFoundation failed";
            return false;
        }
        // No PVD: it is a debugger connection over a socket, and this build
        // does not include the runtime for it (see build_physx.bat).
        physics_ = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation_, physx::PxTolerancesScale(),
                                   false, nullptr);
        if (!physics_) {
            status_ = "PxCreatePhysics failed";
            return false;
        }

        physx::PxSceneDesc desc(physics_->getTolerancesScale());
        desc.gravity = physx::PxVec3(0.0f, -9.81f, 0.0f);
        // TWO WORKER THREADS, not as many as the machine has. The chunk mesher
        // already owns a pool and is the thing actually competing for cores;
        // handing PhysX a thread per core would have the two fighting over the
        // same frame, and a character controller is not the load that needs it.
        dispatcher_ = physx::PxDefaultCpuDispatcherCreate(2);
        desc.cpuDispatcher = dispatcher_;
        desc.filterShader = physx::PxDefaultSimulationFilterShader;
        scene_ = physics_->createScene(desc);
        if (!scene_) {
            status_ = "createScene failed";
            return false;
        }

        material_ = physics_->createMaterial(0.6f, 0.5f, 0.1f);
        controllers_ = PxCreateControllerManager(*scene_);
        if (!controllers_) {
            status_ = "PxCreateControllerManager failed";
            return false;
        }

        ready_ = true;
        status_ = "scene up, CPU solver, 2 worker threads";
        return true;
#endif
    }

    bool available() const { return ready_; }
    bool active() const { return ready_ && enabled; }
    const std::string &status() const { return status_; }

    void shutdown() {
#if V7_HAS_PHYSX
        if (controller_) { controller_->release(); controller_ = nullptr; }
        if (controllers_) { controllers_->release(); controllers_ = nullptr; }
        if (scene_) { scene_->release(); scene_ = nullptr; }
        if (dispatcher_) { dispatcher_->release(); dispatcher_ = nullptr; }
        if (physics_) { physics_->release(); physics_ = nullptr; }
        if (foundation_) { foundation_->release(); foundation_ = nullptr; }
        ready_ = false;
#endif
    }

    ~Physics() { shutdown(); }

  private:
#if V7_HAS_PHYSX
    physx::PxDefaultAllocator allocator_;
    physx::PxDefaultErrorCallback errors_;
    physx::PxFoundation *foundation_ = nullptr;
    physx::PxPhysics *physics_ = nullptr;
    physx::PxDefaultCpuDispatcher *dispatcher_ = nullptr;
    physx::PxScene *scene_ = nullptr;
    physx::PxMaterial *material_ = nullptr;
    physx::PxControllerManager *controllers_ = nullptr;
    physx::PxController *controller_ = nullptr;
#endif
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v7
