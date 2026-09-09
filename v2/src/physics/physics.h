// ---------------------------------------------------------------------------
// physics.h -- NVIDIA PhysX 5: the simulation under the wood.
//
// WHAT IT REPLACES. v2 inherited scene/collide.h, which resolves the player
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
// necessary. But v2's ground IS a height field by construction -- one height
// per column, from a pure function -- and PxHeightFieldGeometry is exactly that
// primitive: a regular grid of samples, no cooking, and a collision query that
// is two lookups and a lerp.
//
// The trees and rocks are not height fields, and they are not represented here
// at all yet. Walking into a trunk is still collide.h's job.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#if V2_HAS_PHYSX
#include <PxPhysicsAPI.h>
#include <characterkinematic/PxCapsuleController.h>
#include <characterkinematic/PxControllerManager.h>
#endif

#include "../core/vecmath.h"

namespace v2 {

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
#if !V2_HAS_PHYSX
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

    // -----------------------------------------------------------------------
    // THE GROUND, AS A HEIGHT FIELD.
    //
    // The header above says why this fits: v2's terrain IS one height per
    // column from a pure function, which is exactly PxHeightFieldGeometry's
    // primitive -- a regular grid, no BVH to cook, and a query that is two
    // lookups and a lerp. A general mesh of the resident wood would be 91
    // million triangles.
    //
    // A PATCH, NOT THE WORLD. It follows the player and is rebuilt when they
    // leave it, because what needs contacts is whatever is falling near them:
    // a chip off a rock lives about a second, and a felled tree lands beside
    // the person who felled it. Sized in COLUMNS.
    // -----------------------------------------------------------------------
    bool setGroundPatch(const int16_t *heightVox, int n, int i0, int j0, float voxelM) {
#if !V2_HAS_PHYSX
        (void)heightVox; (void)n; (void)i0; (void)j0; (void)voxelM;
        return false;
#else
        if (!ready_ || !heightVox || n <= 1) return false;
        std::vector<physx::PxHeightFieldSample> samples(size_t(n) * size_t(n));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                physx::PxHeightFieldSample &sm = samples[size_t(i) * size_t(n) + size_t(j)];
                sm.height = physx::PxI16(heightVox[size_t(i) + size_t(j) * size_t(n)]);
                sm.materialIndex0 = 0;
                sm.materialIndex1 = 0;
            }
        physx::PxHeightFieldDesc hd;
        hd.format = physx::PxHeightFieldFormat::eS16_TM;
        hd.nbColumns = physx::PxU32(n);
        hd.nbRows = physx::PxU32(n);
        hd.samples.data = samples.data();
        hd.samples.stride = sizeof(physx::PxHeightFieldSample);
        physx::PxHeightField *hf = PxCreateHeightField(hd, physics_->getPhysicsInsertionCallback());
        if (!hf) return false;

        if (groundActor_) {
            scene_->removeActor(*groundActor_);
            groundActor_->release();
            groundActor_ = nullptr;
        }
        // heightScale turns the int16 sample into metres and the two axis
        // scales turn a sample index into one. All three are the voxel here,
        // because the samples ARE voxel heights.
        const physx::PxHeightFieldGeometry geo(hf, physx::PxMeshGeometryFlags(), voxelM, voxelM,
                                               voxelM);
        groundActor_ = physics_->createRigidStatic(
            physx::PxTransform(physx::PxVec3(float(i0) * voxelM, 0.0f, float(j0) * voxelM)));
        physx::PxRigidActorExt::createExclusiveShape(*groundActor_, geo, *material_);
        scene_->addActor(*groundActor_);
        hf->release();   // the shape holds its own reference now
        groundI0_ = i0;
        groundJ0_ = j0;
        groundN_ = n;
        return true;
#endif
    }

    // Still inside the patch, with a margin so it is rebuilt before anything
    // can fall off the edge of it.
    bool groundCovers(float wx, float wz, float voxelM, float marginM) const {
        if (groundN_ <= 0) return false;
        const float x0 = float(groundI0_) * voxelM, z0 = float(groundJ0_) * voxelM;
        const float x1 = x0 + float(groundN_ - 1) * voxelM;
        const float z1 = z0 + float(groundN_ - 1) * voxelM;
        return wx > x0 + marginM && wx < x1 - marginM && wz > z0 + marginM && wz < z1 - marginM;
    }

    // -----------------------------------------------------------------------
    // A THING THAT BROKE OFF. Returns a handle, or -1.
    //
    // A BOX, NOT THE VOXELS. What detaches is a lump a few voxels across, and
    // a convex hull of its shell would cost a cook per chip to describe a
    // shape nobody can tell from its own bounding box while it tumbles. The
    // felled trunk is the one case where the difference would show, and a
    // trunk IS a box lying down.
    // -----------------------------------------------------------------------
    int addBox(const Vec3 &centre, const Vec3 &half, const Vec3 &vel, const Vec3 &spin,
               float density = 900.0f, bool kinematic = false) {
#if !V2_HAS_PHYSX
        (void)centre; (void)half; (void)vel; (void)spin; (void)density;
        return -1;
#else
        if (!ready_) return -1;
        physx::PxRigidDynamic *a = physics_->createRigidDynamic(
            physx::PxTransform(physx::PxVec3(centre.x, centre.y, centre.z)));
        if (!a) return -1;
        const physx::PxVec3 h(half.x < 0.02f ? 0.02f : half.x, half.y < 0.02f ? 0.02f : half.y,
                              half.z < 0.02f ? 0.02f : half.z);
        physx::PxRigidActorExt::createExclusiveShape(*a, physx::PxBoxGeometry(h), *material_);
        physx::PxRigidBodyExt::updateMassAndInertia(*a, density);
        if (kinematic) {
            a->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC, true);
        } else {
            a->setLinearVelocity(physx::PxVec3(vel.x, vel.y, vel.z));
            a->setAngularVelocity(physx::PxVec3(spin.x, spin.y, spin.z));
        }
        // A chip that has landed should stop looking busy; without this they
        // buzz against the height field for as long as they exist.
        a->setSleepThreshold(0.05f);
        scene_->addActor(*a);
        for (size_t k = 0; k < bodies_.size(); ++k)
            if (!bodies_[k]) { bodies_[k] = a; return int(k); }
        bodies_.push_back(a);
        return int(bodies_.size()) - 1;
#endif
    }

    // Where it is now: a position and a quaternion (x, y, z, w).
    bool poseOf(int h, Vec3 *p, float *quat) const {
#if !V2_HAS_PHYSX
        (void)h; (void)p; (void)quat;
        return false;
#else
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return false;
        const physx::PxTransform t = bodies_[size_t(h)]->getGlobalPose();
        if (p) *p = Vec3{t.p.x, t.p.y, t.p.z};
        if (quat) { quat[0] = t.q.x; quat[1] = t.q.y; quat[2] = t.q.z; quat[3] = t.q.w; }
        return true;
#endif
    }

    // THE ABSORB TAKES THE BODY OVER. Once a chunk is on its way to the player
    // the simulation has nothing further to say about it -- it is on a curve,
    // not in a fall -- so it goes kinematic and is driven by hand.
    // HANDED TO THE SOLVER. The other direction: a body that was being
    // driven by hand stops being, and takes the motion it had as its own.
    void makeDynamic(int h, const Vec3 &vel, const Vec3 &spin) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        physx::PxRigidDynamic *a = bodies_[size_t(h)];
        a->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC, false);
        a->setLinearVelocity(physx::PxVec3(vel.x, vel.y, vel.z));
        a->setAngularVelocity(physx::PxVec3(spin.x, spin.y, spin.z));
        a->wakeUp();
#else
        (void)h; (void)vel; (void)spin;
#endif
    }

    void makeKinematic(int h) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        bodies_[size_t(h)]->setRigidBodyFlag(physx::PxRigidBodyFlag::eKINEMATIC, true);
#else
        (void)h;
#endif
    }

    void setPose(int h, const Vec3 &p, const float *quat) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        const physx::PxQuat q = quat ? physx::PxQuat(quat[0], quat[1], quat[2], quat[3])
                                     : physx::PxQuat(physx::PxIdentity);
        bodies_[size_t(h)]->setKinematicTarget(physx::PxTransform(physx::PxVec3(p.x, p.y, p.z), q));
#else
        (void)h; (void)p; (void)quat;
#endif
    }

    // KEEP A BODY ABOVE A SURFACE THE SOLVER CANNOT SEE.
    //
    // The height field is a PATCH around the player, and the only things in the
    // scene are it and the loose bodies -- the boulders and the trunks are not
    // collision geometry here. So a chip carved off the top of a rock has
    // nothing under it and falls through the stone, and one thrown past the
    // edge of the patch falls through the world.
    //
    // The engine already answers "what is the surface under this point" exactly,
    // for the player, including the voxel column of any standable model. This
    // holds a body to that answer. It is a backstop and not a contact: the
    // solver still owns the tumble, this only refuses to let it end up
    // somewhere there is no floor.
    void clampAbove(int h, float minCentreY) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        physx::PxRigidDynamic *a = bodies_[size_t(h)];
        physx::PxTransform t = a->getGlobalPose();
        if (t.p.y >= minCentreY) return;
        t.p.y = minCentreY;
        a->setGlobalPose(t);
        if (!(a->getRigidBodyFlags() & physx::PxRigidBodyFlag::eKINEMATIC)) {
            physx::PxVec3 v = a->getLinearVelocity();
            if (v.y < 0.0f) v.y = 0.0f;
            // ...and shed some of the slide, or a chip skates along the surface
            // it was just stopped by.
            v.x *= 0.6f;
            v.z *= 0.6f;
            a->setLinearVelocity(v);
        }
#else
        (void)h; (void)minCentreY;
#endif
    }

    // -----------------------------------------------------------------------
    // A PLACED MODEL, AS SOMETHING THINGS BOUNCE OFF.
    //
    // A boulder's colTop is the height of every one of its voxel columns --
    // which is a height field, and the SAME surface the player is already
    // collided against. So it goes in as one, no cooking, and the two cannot
    // disagree about where the stone is.
    //
    // THE FIELD IS SHARED, THE ACTOR IS NOT. Twenty-five placements of a rock
    // are one PxHeightField and twenty-five static actors, exactly as they are
    // one BLAS and twenty-five instances. Keyed on the colTop pointer, so a
    // DAMAGED instance -- which owns a private colTop -- correctly gets its own
    // field rather than the pristine one.
    //
    // A height field is solid all the way down, which is what we want: a chip
    // cannot tunnel in from the side and end up inside the stone.
    // -----------------------------------------------------------------------
    int addStaticHeightField(const int16_t *colTop, int sx, int sz, float voxelM,
                             const Vec3 &origin, float yawRad) {
#if !V2_HAS_PHYSX
        (void)colTop; (void)sx; (void)sz; (void)voxelM; (void)origin; (void)yawRad;
        return -1;
#else
        if (!ready_ || !colTop || sx <= 1 || sz <= 1) return -1;
        // NOT CACHED BY POINTER, AND THAT IS THE WHOLE NOTE.
        //
        // This used to keep a PxHeightField per colTop address, so the twenty
        // five placements of one boulder shared a field. But a DAMAGED rock
        // re-measures its own colTop -- refitSolid assigns the vector, which
        // frees the old buffer -- and the cache went on holding an entry under
        // a dead address. The next allocation to land on that address brought
        // back a field of the wrong dimensions, and PhysX read off the end of
        // it. It crashed on the blow AFTER the one that broke the rock.
        //
        // Building one per actor costs a few hundred kilobytes for the handful
        // of models near the player and cannot alias anything.
        std::vector<physx::PxHeightFieldSample> samples(size_t(sx) * size_t(sz));
        int solidCols = 0;
        for (int z = 0; z < sz; ++z)
            for (int x = 0; x < sx; ++x) {
                physx::PxHeightFieldSample &sm = samples[size_t(x) * size_t(sz) + size_t(z)];
                // A column with nothing in it is a HOLE, not a floor at zero:
                // otherwise the model's bounding box becomes a slab you cannot
                // fall past.
                const int16_t h = colTop[size_t(x) + size_t(z) * size_t(sx)];
                sm.height = physx::PxI16(h);
                sm.materialIndex0 = (h > 0) ? 0 : physx::PxHeightFieldMaterial::eHOLE;
                sm.materialIndex1 = (h > 0) ? 0 : physx::PxHeightFieldMaterial::eHOLE;
                if (h > 0) ++solidCols;
            }
        // Nothing solid anywhere: not a surface, and a field of pure holes is
        // not worth an actor.
        if (solidCols == 0) return -1;

        physx::PxHeightFieldDesc hd;
        hd.format = physx::PxHeightFieldFormat::eS16_TM;
        hd.nbRows = physx::PxU32(sx);
        hd.nbColumns = physx::PxU32(sz);
        hd.samples.data = samples.data();
        hd.samples.stride = sizeof(physx::PxHeightFieldSample);
        physx::PxHeightField *hf = PxCreateHeightField(hd, physics_->getPhysicsInsertionCallback());
        if (!hf) return -1;

        const physx::PxHeightFieldGeometry geo(hf, physx::PxMeshGeometryFlags(), voxelM, voxelM,
                                               voxelM);
        physx::PxRigidStatic *a = physics_->createRigidStatic(physx::PxTransform(
            physx::PxVec3(origin.x, origin.y, origin.z),
            physx::PxQuat(yawRad, physx::PxVec3(0.0f, 1.0f, 0.0f))));
        if (!a) return -1;
        physx::PxRigidActorExt::createExclusiveShape(*a, geo, *material_);
        scene_->addActor(*a);
        hf->release();   // the shape holds its own reference now
        for (size_t k = 0; k < statics_.size(); ++k)
            if (!statics_[k]) { statics_[k] = a; return int(k); }
        statics_.push_back(a);
        return int(statics_.size()) - 1;
#endif
    }

    void removeStatic(int h) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= statics_.size() || !statics_[size_t(h)]) return;
        scene_->removeActor(*statics_[size_t(h)]);
        statics_[size_t(h)]->release();
        statics_[size_t(h)] = nullptr;
#else
        (void)h;
#endif
    }

    void releaseBody(int h) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        scene_->removeActor(*bodies_[size_t(h)]);
        bodies_[size_t(h)]->release();
        bodies_[size_t(h)] = nullptr;
#else
        (void)h;
#endif
    }

    int liveBodies() const {
        int k = 0;
#if V2_HAS_PHYSX
        for (auto *b : bodies_)
            if (b) ++k;
#endif
        return k;
    }

    // FIXED STEP, ACCUMULATED. A solver fed a variable dt gives answers that
    // change with the frame rate, and the only thing worse than a chip that
    // tumbles oddly is one that tumbles differently on two machines.
    void step(float dt) {
#if V2_HAS_PHYSX
        if (!ready_ || !enabled || !scene_) return;
        acc_ += (dt > 0.25f) ? 0.25f : dt;   // a long hitch is dropped, never caught up on
        const float h = 1.0f / 60.0f;
        int guard = 0;
        while (acc_ >= h && guard++ < 4) {
            scene_->simulate(h);
            scene_->fetchResults(true);
            acc_ -= h;
        }
#else
        (void)dt;
#endif
    }

    bool available() const { return ready_; }
    bool active() const { return ready_ && enabled; }
    const std::string &status() const { return status_; }

    void shutdown() {
#if V2_HAS_PHYSX
        for (auto *&b : bodies_) {
            if (b) { b->release(); b = nullptr; }
        }
        bodies_.clear();
        for (auto *&a : statics_) {
            if (a) { a->release(); a = nullptr; }
        }
        statics_.clear();
        if (groundActor_) { groundActor_->release(); groundActor_ = nullptr; }
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
#if V2_HAS_PHYSX
    physx::PxDefaultAllocator allocator_;
    physx::PxDefaultErrorCallback errors_;
    physx::PxFoundation *foundation_ = nullptr;
    physx::PxPhysics *physics_ = nullptr;
    physx::PxDefaultCpuDispatcher *dispatcher_ = nullptr;
    physx::PxScene *scene_ = nullptr;
    physx::PxMaterial *material_ = nullptr;
    physx::PxControllerManager *controllers_ = nullptr;
    physx::PxController *controller_ = nullptr;
    physx::PxRigidStatic *groundActor_ = nullptr;
    std::vector<physx::PxRigidStatic *> statics_;
    std::vector<physx::PxRigidDynamic *> bodies_;
#endif
    int groundI0_ = 0, groundJ0_ = 0, groundN_ = 0;
    float acc_ = 0.0f;
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v2
