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
#include "../scene/voxbox.h"

namespace v2 {

#if V2_HAS_PHYSX
// ---------------------------------------------------------------------------
// A PIECE COLLIDES WITH EVERYTHING, INCLUDING THE ROCK IT CAME OUT OF.
//
// This used to do the opposite, and the note that stood here explained at
// length why the pair had to be suppressed: the rock's collider was a convex
// hull, a hull cannot have a dent, and a piece born in the bite was therefore
// born inside solid stone and ground down through it. Suppressing the pair was
// the least bad way to live with a shape that was not the rock.
//
// The shape is the rock now -- scene/voxbox.h, a window of boxes merged off the
// damaged instance's own voxels, with the hole really in it -- so there is
// nothing left to suppress and the piece is simply a body among bodies. What
// this shader still does is ask for CCD on everything dynamic: a 30 cm chip
// falling off a 20 m boulder passes through more than its own thickness in a
// frame, and a discrete solver would let it through the face it should have
// bounced off.
// ---------------------------------------------------------------------------
inline physx::PxFilterFlags v2LooseFilter(physx::PxFilterObjectAttributes a0, physx::PxFilterData,
                                          physx::PxFilterObjectAttributes a1, physx::PxFilterData,
                                          physx::PxPairFlags &pairFlags, const void *, physx::PxU32) {
    pairFlags = physx::PxPairFlag::eCONTACT_DEFAULT;
    if (physx::PxFilterObjectIsKinematic(a0) && physx::PxFilterObjectIsKinematic(a1))
        return physx::PxFilterFlag::eSUPPRESS;
    pairFlags |= physx::PxPairFlag::eDETECT_CCD_CONTACT;
    return physx::PxFilterFlag::eDEFAULT;
}
#endif

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
        desc.filterShader = v2LooseFilter;   // see the note above it
        // CONTINUOUS, because the things falling here are small and the things
        // they fall past are thin. A chip is 30 cm and reaches 5 m/s inside a
        // second, which is more than a third of its own size in one 60 Hz step
        // -- discrete collision lets it pass through the lip it was resting on.
        desc.flags |= physx::PxSceneFlag::eENABLE_CCD;
        scene_ = physics_->createScene(desc);
        if (!scene_) {
            status_ = "createScene failed";
            return false;
        }

        material_ = physics_->createMaterial(0.6f, 0.5f, 0.1f);
        // STONE ON STONE, which is what makes it read as rock rather than as a
        // prop. High friction so a chip grips the face it lands on and does not
        // skate down it, and almost no restitution because granite does not
        // bounce -- a chip that bounces is the single loudest tell that a
        // physics body is not a rock.
        stone_ = physics_->createMaterial(0.85f, 0.72f, 0.03f);
        // WOOD ON EARTH. A felled trunk digs in and stays where it lands; it
        // does not skid and it certainly does not bounce.
        timber_ = physics_->createMaterial(0.75f, 0.62f, 0.0f);
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
    // `stepVox` is how many world voxels lie between two samples. It exists
    // because a FELLED TREE is twenty-six metres long: a patch at one sample
    // per voxel is sixteen metres across for 25,600 samples, so the crown end
    // of a falling pine swings straight off the edge of it and the tree drops
    // through the world. Sampling every fourth column covers sixty-four metres
    // for the SAME number of samples -- and because the patch is four times
    // wider, the player leaves it four times less often, so the rebuild that
    // was the reason to keep it small happens less than it used to.
    //
    // Nothing walks on this. The player's own ground is answered on the CPU by
    // Player::groundInfo against the voxel columns; this height field exists
    // only for the loose bodies, and a chip is happy on 40 cm samples of a
    // surface that was smooth to begin with.
    bool setGroundPatch(const int16_t *heightVox, int n, int i0, int j0, float voxelM,
                        int stepVox = 1) {
#if !V2_HAS_PHYSX
        (void)heightVox; (void)n; (void)i0; (void)j0; (void)voxelM; (void)stepVox;
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
        // heightScale turns the int16 sample into metres; the two axis scales
        // turn a sample index into one. The heights are still voxels, so that
        // scale stays the voxel -- but a sample is now `stepVox` of them apart.
        const float axisM = voxelM * float(stepVox < 1 ? 1 : stepVox);
        const physx::PxHeightFieldGeometry geo(hf, physx::PxMeshGeometryFlags(), voxelM, axisM,
                                               axisM);
        groundActor_ = physics_->createRigidStatic(
            physx::PxTransform(physx::PxVec3(float(i0) * voxelM, 0.0f, float(j0) * voxelM)));
        groundStep_ = (stepVox < 1) ? 1 : stepVox;
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
        const float axisM = voxelM * float(groundStep_);
        const float x0 = float(groundI0_) * voxelM, z0 = float(groundJ0_) * voxelM;
        const float x1 = x0 + float(groundN_ - 1) * axisM;
        const float z1 = z0 + float(groundN_ - 1) * axisM;
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
               float density = 900.0f, bool kinematic = false, float yawRad = 0.0f,
               int ignoreStatic = -1) {
#if !V2_HAS_PHYSX
        (void)centre; (void)half; (void)vel; (void)spin; (void)density;
        return -1;
#else
        if (!ready_) return -1;
        // BORN ALREADY TURNED. A piece cut out of a placed model is stored in
        // that model's voxel axes, so it lines up with the hole it came from
        // only if it starts wearing the model's own quarter turn.
        physx::PxRigidDynamic *a = physics_->createRigidDynamic(
            physx::PxTransform(physx::PxVec3(centre.x, centre.y, centre.z),
                               physx::PxQuat(yawRad, physx::PxVec3(0.0f, 1.0f, 0.0f))));
        if (!a) return -1;
        const physx::PxVec3 h(half.x < 0.02f ? 0.02f : half.x, half.y < 0.02f ? 0.02f : half.y,
                              half.z < 0.02f ? 0.02f : half.z);
        physx::PxShape *bs =
            physx::PxRigidActorExt::createExclusiveShape(*a, physx::PxBoxGeometry(h), *material_);
        // The rock this came out of, so the pair is suppressed -- see
        // v2LooseFilter. Everything else it meets is collided with normally.
        if (bs && ignoreStatic >= 0)
            bs->setSimulationFilterData(
                physx::PxFilterData(0, physx::PxU32(ignoreStatic + 1), 0, 0));
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
        // BORN INSIDE THE ROCK IT CAME OFF, so depenetration has real work to
        // do on the first frames. Left at its default the solver evicts the
        // piece at several metres a second, which reads as the chunk being
        // spat out. A slow push looks like it is coming loose.
        a->setMaxDepenetrationVelocity(0.6f);
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

    // -----------------------------------------------------------------------
    // A PLACED MODEL AS A CONVEX, WHICH IS WHAT REAL BODIES NEED.
    //
    // A height field is a SURFACE. PhysX generates contacts against it, so a
    // body a little under it is pushed out and a body well under it is not --
    // and a chip cut from the middle of a boulder's face starts well under it.
    // That is why every attempt to let the solver own a chip ended with the
    // chip inside the stone.
    //
    // A convex has no inside/outside problem: depenetration pushes a body out
    // along the nearest face however deep it starts, so a piece born in the
    // rock it was cut from simply comes out. That is the whole reason to pay
    // for a cook.
    //
    // THE HULL IS BUILT FROM colTop, subsampled. Every column that has
    // anything in it contributes its top and its base; PhysX takes the hull of
    // that cloud. A boulder is roughly convex to begin with, so the hull is
    // close, and the 255-vertex limit is why the columns are strided rather
    // than all handed over.
    // -----------------------------------------------------------------------
    int addStaticConvex(const int16_t *colTop, int sx, int sz, float voxelM, const Vec3 &origin,
                        float yawRad) {
#if !V2_HAS_PHYSX
        (void)colTop; (void)sx; (void)sz; (void)voxelM; (void)origin; (void)yawRad;
        return -1;
#else
        if (!ready_ || !colTop || sx <= 1 || sz <= 1) return -1;
        // FEED IT PLENTY. The 255-vertex limit is on the HULL PhysX computes,
        // not on the cloud it is given -- so sampling coarsely to stay under it
        // was solving a problem that does not exist, and solving it badly: a
        // boulder 198 voxels across was sampled every 19th column, a 1.9 m grid
        // over a 20 m rock, and the hull of those few points fell well inside
        // the stone near its edges. A chip cut there was born OUTSIDE the hull
        // and fell straight past it. Measured: two bites on one rock, one with
        // a static under it and one with nothing at all.
        //
        // 64 samples a side is a 30 cm grid on the biggest rock here, and PhysX
        // reduces whatever that yields to its own limit.
        const int stride = (sx > sz ? sx : sz) / 64 + 1;
        std::vector<physx::PxVec3> pts;
        pts.reserve(256);
        for (int z = 0; z < sz; z += stride)
            for (int x = 0; x < sx; x += stride) {
                const int h = int(colTop[size_t(x) + size_t(z) * size_t(sx)]);
                if (h <= 0) continue;
                pts.push_back(physx::PxVec3(float(x) * voxelM, float(h) * voxelM, float(z) * voxelM));
                pts.push_back(physx::PxVec3(float(x) * voxelM, 0.0f, float(z) * voxelM));
            }
        if (pts.size() < 8) return -1;   // nothing solid enough to be a shape
        ++convexTried_;

        physx::PxConvexMeshDesc cd;
        cd.points.count = physx::PxU32(pts.size());
        cd.points.stride = sizeof(physx::PxVec3);
        cd.points.data = pts.data();
        cd.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX;
        physx::PxCookingParams cp(physics_->getTolerancesScale());
        physx::PxConvexMesh *cm =
            PxCreateConvexMesh(cp, cd, physics_->getPhysicsInsertionCallback());
        if (!cm) return -1;

        physx::PxRigidStatic *a = physics_->createRigidStatic(physx::PxTransform(
            physx::PxVec3(origin.x, origin.y, origin.z),
            physx::PxQuat(yawRad, physx::PxVec3(0.0f, 1.0f, 0.0f))));
        if (!a) { cm->release(); return -1; }
        physx::PxShape *sh = physx::PxRigidActorExt::createExclusiveShape(
            *a, physx::PxConvexMeshGeometry(cm), *material_);
        // ITS OWN ID, so a piece cut out of it can say which rock to ignore.
        // Slot+1, because zero means "not a rock" to the filter.
        const physx::PxU32 rockId = physx::PxU32(statics_.size() + 1);
        if (sh) sh->setSimulationFilterData(physx::PxFilterData(rockId, 0, 0, 0));
        scene_->addActor(*a);
        cm->release();   // the shape holds its own reference now
        ++convexMade_;
        for (size_t k = 0; k < statics_.size(); ++k)
            if (!statics_[k]) { statics_[k] = a; return int(k); }
        statics_.push_back(a);
        return int(statics_.size()) - 1;
#endif
    }

    // -----------------------------------------------------------------------
    // A WINDOW OF STONE, AS BOXES.
    //
    // The collider that finally has the hole in it. scene/voxbox.h merges a few
    // metres cube of the DAMAGED model's own voxels into boxes -- exactly the
    // solid cells, no more and no less, verified cell by cell in scratch's
    // window_probe -- and they all go on one static actor.
    //
    // NO COOKING. Boxes are the one shape PhysX takes as-is, which is what
    // makes this affordable on the frame a blow lands: a couple of hundred
    // shapes created, no BVH built, no mesh cleaned. The actor sits at the
    // origin and every box carries its own world centre as a local pose, so
    // there is no transform to get wrong.
    // -----------------------------------------------------------------------
    int addStaticBoxes(const VoxBox *boxes, int n) {
#if !V2_HAS_PHYSX
        (void)boxes; (void)n;
        return -1;
#else
        if (!ready_ || !boxes || n <= 0) return -1;
        physx::PxRigidStatic *a = physics_->createRigidStatic(physx::PxTransform(physx::PxIdentity));
        if (!a) return -1;
        for (int k = 0; k < n; ++k) {
            const VoxBox &b = boxes[k];
            physx::PxShape *sh = physics_->createShape(
                physx::PxBoxGeometry(b.hx, b.hy, b.hz), *stone_, true);
            if (!sh) continue;
            sh->setLocalPose(physx::PxTransform(physx::PxVec3(b.cx, b.cy, b.cz)));
            a->attachShape(*sh);
            sh->release();   // the actor holds it now
        }
        if (a->getNbShapes() == 0) { a->release(); return -1; }
        scene_->addActor(*a);
        for (size_t k = 0; k < statics_.size(); ++k)
            if (!statics_[k]) { statics_[k] = a; return int(k); }
        statics_.push_back(a);
        return int(statics_.size()) - 1;
#endif
    }

    // -----------------------------------------------------------------------
    // THE CHUNK ITSELF, AS THE SHAPE IT ACTUALLY IS.
    //
    // A convex hull cooked from the piece's own voxel corners. A box would
    // tumble like a box -- flat faces, hard stops, a die rolling -- and a bite
    // is a rough ball, so the hull of its corners rolls and settles the way a
    // stone does. Cooking a few hundred points is well under a tenth of a
    // millisecond; this is not the cook that was ever expensive.
    //
    // Points come in RELATIVE TO THE CENTRE OF MASS, so the hull is centred on
    // the body's origin and the inertia PhysX computes is about the right axis.
    // A piece rotating about a point that is not its middle is the difference
    // between tumbling and orbiting.
    // -----------------------------------------------------------------------
    int addChunkBody(const Vec3 *pts, int n, const Vec3 &centre, float yawRad, float density) {
#if !V2_HAS_PHYSX
        (void)pts; (void)n; (void)centre; (void)yawRad; (void)density;
        return -1;
#else
        if (!ready_ || !pts || n < 8) return -1;
        ++convexTried_;
        // NOT `cloud(size_t(n))`. That is a function declaration -- the most
        // vexing parse -- and the error it gives names the subscript below it.
        std::vector<physx::PxVec3> cloud;
        cloud.resize(size_t(n));
        for (int k = 0; k < n; ++k) cloud[size_t(k)] = physx::PxVec3(pts[k].x, pts[k].y, pts[k].z);

        physx::PxConvexMeshDesc cd;
        cd.points.count = physx::PxU32(cloud.size());
        cd.points.stride = sizeof(physx::PxVec3);
        cd.points.data = cloud.data();
        cd.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX | physx::PxConvexFlag::eFAST_INERTIA_COMPUTATION;
        // A CHIP DOES NOT NEED A HUNDRED FACES. Twenty-four is enough to roll
        // convincingly and keeps both the cook and the contact generation
        // cheap; the shape is a rough ball either way.
        cd.vertexLimit = 24;
        physx::PxCookingParams cp(physics_->getTolerancesScale());
        cp.suppressTriangleMeshRemapTable = true;
        physx::PxConvexMesh *cm =
            PxCreateConvexMesh(cp, cd, physics_->getPhysicsInsertionCallback());
        if (!cm) return -1;

        // BORN ALREADY TURNED, because the hull is in the model's voxel axes
        // and the hole it has to fit is in the world's. Same quarter turn the
        // drawn mesh wears.
        physx::PxRigidDynamic *a = physics_->createRigidDynamic(
            physx::PxTransform(physx::PxVec3(centre.x, centre.y, centre.z),
                               physx::PxQuat(yawRad, physx::PxVec3(0.0f, 1.0f, 0.0f))));
        if (!a) { cm->release(); return -1; }
        physx::PxShape *sh = physx::PxRigidActorExt::createExclusiveShape(
            *a, physx::PxConvexMeshGeometry(cm), *stone_);
        cm->release();
        if (!sh) { a->release(); return -1; }
        physx::PxRigidBodyExt::updateMassAndInertia(*a, density);
        a->setRigidBodyFlag(physx::PxRigidBodyFlag::eENABLE_CCD, true);
        // IT NEVER GOES COMPLETELY STILL. A chip is collected half a second
        // after it comes loose, and a body that falls asleep in that time sits
        // frozen for the rest of it -- which is the "wobbles and then stops"
        // this used to do. Sleeping off, and barely any angular damping, so
        // what motion the fall gave it stays with it until it is picked up.
        a->setSleepThreshold(0.0f);
        a->setAngularDamping(0.02f);
        a->setLinearDamping(0.01f);
        // The piece is NOT born penetrating anything -- the window has the hole
        // in it -- so this is only a guard against a rebuilt window catching it
        // mid-fall. Slow enough that a correction reads as settling.
        a->setMaxDepenetrationVelocity(1.5f);
        a->setSolverIterationCounts(8, 4);
        scene_->addActor(*a);
        ++convexMade_;
        for (size_t k = 0; k < bodies_.size(); ++k)
            if (!bodies_[k]) { bodies_[k] = a; return int(k); }
        bodies_.push_back(a);
        return int(bodies_.size()) - 1;
#endif
    }

    // -----------------------------------------------------------------------
    // A WHOLE TREE, AS SOMETHING THAT CAN FALL OVER.
    //
    // A DYNAMIC COMPOUND OF BOXES, and the reason it is not a convex hull is
    // the shape of a tree: the hull of a pine is a fat cone, so a felled one
    // would come to rest balanced on its canopy with the trunk in the air. The
    // boxes are the tree's own voxels, coarsened until the count is affordable
    // -- see World::fellTree -- so it topples about its butt, lands along its
    // length, and lies there the way a felled tree does.
    //
    // THE BOXES ARE IN THE MODEL'S OWN FRAME, with the actor standing at the
    // model's origin corner wearing the model's quarter turn. That is exactly
    // the frame the drawn mesh is in, so the shape the solver holds and the
    // tree you can see are the same object and no offset has to be kept in step.
    //
    // updateMassAndInertia works the mass frame out from the shapes themselves,
    // which is what makes the fall look right: a tree is nearly all trunk and
    // the inertia of a twenty-six metre lever is what sets how slowly it goes
    // over.
    // -----------------------------------------------------------------------
    int addCompoundBody(const VoxBox *boxes, int n, const Vec3 &origin, float yawRad,
                        float density) {
#if !V2_HAS_PHYSX
        (void)boxes; (void)n; (void)origin; (void)yawRad; (void)density;
        return -1;
#else
        if (!ready_ || !boxes || n <= 0) return -1;
        physx::PxRigidDynamic *a = physics_->createRigidDynamic(
            physx::PxTransform(physx::PxVec3(origin.x, origin.y, origin.z),
                               physx::PxQuat(yawRad, physx::PxVec3(0.0f, 1.0f, 0.0f))));
        if (!a) return -1;
        for (int k = 0; k < n; ++k) {
            const VoxBox &b = boxes[k];
            physx::PxShape *sh =
                physics_->createShape(physx::PxBoxGeometry(b.hx, b.hy, b.hz), *timber_, true);
            if (!sh) continue;
            sh->setLocalPose(physx::PxTransform(physx::PxVec3(b.cx, b.cy, b.cz)));
            a->attachShape(*sh);
            sh->release();
        }
        if (a->getNbShapes() == 0) { a->release(); return -1; }
        physx::PxRigidBodyExt::updateMassAndInertia(*a, density);
        a->setRigidBodyFlag(physx::PxRigidBodyFlag::eENABLE_CCD, true);
        a->setSleepThreshold(0.05f);
        // A THUMP, NOT A CARTWHEEL. Traced landing a felled pine: the impact
        // put 12.67 rad/s into a body that had been turning at 1.5, and it
        // cartwheeled for seven seconds before it settled. A trunk is long and
        // thin, so a hard contact at one end is an enormous lever, and PhysX is
        // right about all of it -- it is just not what a tree does. Capping the
        // turn rate and damping it hard is the difference between a tree coming
        // down and a tree being thrown.
        a->setMaxAngularVelocity(2.5f);
        a->setAngularDamping(0.55f);
        a->setLinearDamping(0.10f);
        a->setMaxDepenetrationVelocity(1.0f);
        // MORE ITERATIONS THAN A CHIP GETS. A twenty-six metre body resting on a
        // height field along its whole length is a long chain of contacts, and
        // an under-solved one settles by shivering.
        a->setSolverIterationCounts(16, 8);
        scene_->addActor(*a);
        for (size_t k = 0; k < bodies_.size(); ++k)
            if (!bodies_[k]) { bodies_[k] = a; return int(k); }
        bodies_.push_back(a);
        return int(bodies_.size()) - 1;
#endif
    }

    // RAISE A BODY BY dy, KEEPING ITS TURN. The backstop for something too
    // long to be described by one clamp -- see the note in World::updateDebris
    // on why a felled tree needs its own floor.
    void nudgeUp(int h, float dy) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)] || dy <= 0.0f) return;
        physx::PxRigidDynamic *a = bodies_[size_t(h)];
        physx::PxTransform t = a->getGlobalPose();
        t.p.y += dy;
        a->setGlobalPose(t);
        if (!(a->getRigidBodyFlags() & physx::PxRigidBodyFlag::eKINEMATIC)) {
            physx::PxVec3 v = a->getLinearVelocity();
            if (v.y < 0.0f) v.y = 0.0f;
            a->setLinearVelocity(v);
        }
#else
        (void)h; (void)dy;
#endif
    }

    // WHAT IT WEIGHS, SAID OUTRIGHT RATHER THAN DERIVED FROM ITS SHAPE.
    //
    // updateMassAndInertia takes the mass from the VOLUME OF THE SHAPES, which
    // for a tree is badly wrong twice over: the collider is coarsened, so it is
    // several times the volume of the wood, and the wood itself is a canopy
    // that is mostly air being treated as solid timber. Measured on the real
    // models, that came out at 128 to 297 tonnes for a pine. This sets the mass
    // to what a tree actually weighs and lets the inertia be scaled to match,
    // so the shape still says how it turns and only the scale is corrected.
    void setBodyMass(int h, float kg) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)] || kg <= 0.0f) return;
        physx::PxRigidBodyExt::setMassAndUpdateInertia(*bodies_[size_t(h)], kg);
#else
        (void)h; (void)kg;
#endif
    }

    // A NUDGE OFF THE STUMP, and the only force this feature applies.
    //
    // A severed trunk standing exactly upright is in equilibrium and PhysX will
    // quite correctly leave it standing there for ever. A real one goes over
    // because the cut is deeper on one side and the remaining fibres hinge --
    // it falls AWAY from the axe. This is that hinge, as a small angular
    // velocity about the cut. Everything after it is gravity.
    void nudgeSpin(int h, const Vec3 &axis, float radPerSec) {
#if V2_HAS_PHYSX
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        physx::PxRigidDynamic *a = bodies_[size_t(h)];
        a->setAngularVelocity(physx::PxVec3(axis.x * radPerSec, axis.y * radPerSec,
                                            axis.z * radPerSec));
        a->wakeUp();
#else
        (void)h; (void)axis; (void)radPerSec;
#endif
    }

    // -----------------------------------------------------------------------
    // WHERE THE BODY ACTUALLY IS, as opposed to where its origin is.
    //
    // The two are the same thing for a chip, whose actor sits at its centre of
    // mass, and they are nowhere near each other for a felled tree, whose
    // origin is the model's corner -- metres below the wood and, once the tree
    // is lying down, metres to one side of it as well. A floor test written
    // against the origin is the reason a tree that had landed perfectly kept
    // being hoisted back into the air.
    //
    // This is the shapes' own world bounds, which is what any question about
    // "is it under the ground" has to be asked of.
    // -----------------------------------------------------------------------
    bool boundsOf(int h, Vec3 *lo, Vec3 *hi) const {
#if !V2_HAS_PHYSX
        (void)h; (void)lo; (void)hi;
        return false;
#else
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return false;
        const physx::PxBounds3 b = bodies_[size_t(h)]->getWorldBounds();
        if (b.isEmpty()) return false;
        if (lo) *lo = Vec3{b.minimum.x, b.minimum.y, b.minimum.z};
        if (hi) *hi = Vec3{b.maximum.x, b.maximum.y, b.maximum.z};
        return true;
#endif
    }

    // ...and lift it bodily by this much, keeping its attitude. Used only by
    // the backstop below the world -- see World::updateDebris.
    void liftBy(int h, float dy) {
#if !V2_HAS_PHYSX
        (void)h; (void)dy;
#else
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return;
        physx::PxRigidDynamic *a = bodies_[size_t(h)];
        physx::PxTransform t = a->getGlobalPose();
        t.p.y += dy;
        a->setGlobalPose(t);
        if (a->getRigidBodyFlags() & physx::PxRigidBodyFlag::eKINEMATIC) return;
        physx::PxVec3 v = a->getLinearVelocity();
        if (v.y < 0.0f) v.y = 0.0f;
        a->setLinearVelocity(v);
        // A body that has just been caught by the floor of the world has no
        // business still spinning at the rate that took it through.
        physx::PxVec3 w = a->getAngularVelocity();
        a->setAngularVelocity(w * 0.3f);
#endif
    }

    // What it is doing right now, for the hand-over to the absorb.
    bool velocityOf(int h, Vec3 *lin, Vec3 *ang) const {
#if !V2_HAS_PHYSX
        (void)h; (void)lin; (void)ang;
        return false;
#else
        if (h < 0 || size_t(h) >= bodies_.size() || !bodies_[size_t(h)]) return false;
        const physx::PxVec3 v = bodies_[size_t(h)]->getLinearVelocity();
        const physx::PxVec3 w = bodies_[size_t(h)]->getAngularVelocity();
        if (lin) *lin = Vec3{v.x, v.y, v.z};
        if (ang) *ang = Vec3{w.x, w.y, w.z};
        return true;
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

    // How many model hulls were asked for and how many cooked. If these
    // disagree the cook is failing, and nothing is stopping a falling chip.
    int convexMade() const { return convexMade_; }
    int convexTried() const { return convexTried_; }

    // IS THERE ANYTHING SOLID HERE AT ALL? Asked of the scene itself, because
    // every other way of answering it has been an assumption.
    int overlapCount(const Vec3 &at, float r) const {
#if !V2_HAS_PHYSX
        (void)at; (void)r;
        return -1;
#else
        if (!ready_ || !scene_) return -1;
        physx::PxOverlapBuffer hit;
        physx::PxQueryFilterData fd;
        fd.flags = physx::PxQueryFlag::eSTATIC;
        const bool any = scene_->overlap(physx::PxSphereGeometry(r),
                                         physx::PxTransform(physx::PxVec3(at.x, at.y, at.z)), hit,
                                         fd);
        return any ? int(hit.getNbAnyHits()) : 0;
#endif
    }

    // WHAT IS UNDER THIS POINT, and what KIND of thing is it. The overlap
    // count could not tell a boulder from the ground patch, and that is
    // exactly the distinction that matters: 4 = convex (a rock), 5 = height
    // field (the ground). Returns the geometry type, or -1 for nothing.
    int typeBelow(const Vec3 &at, float maxDist, float *dist) const {
#if !V2_HAS_PHYSX
        (void)at; (void)maxDist; (void)dist;
        return -1;
#else
        if (!ready_ || !scene_) return -1;
        physx::PxRaycastBuffer hit;
        // STATICS ONLY -- otherwise this finds the loose piece the caller just
        // created, at zero distance, and reports a box.
        physx::PxQueryFilterData fd;
        fd.flags = physx::PxQueryFlag::eSTATIC;
        const bool any = scene_->raycast(physx::PxVec3(at.x, at.y, at.z),
                                         physx::PxVec3(0.0f, -1.0f, 0.0f), maxDist, hit,
                                         physx::PxHitFlags(physx::PxHitFlag::eDEFAULT), fd);
        if (!any || !hit.hasBlock) return -1;
        if (dist) *dist = hit.block.distance;
        return int(hit.block.shape ? hit.block.shape->getGeometry().getType()
                                   : physx::PxGeometryType::eINVALID);
#endif
    }

    // The id addStaticConvex gave this actor, for a body that must ignore it.
    int staticIdOf(int h) const { return h; }

    int liveStatics() const {
        int k = 0;
#if V2_HAS_PHYSX
        for (auto *a : statics_)
            if (a) ++k;
#endif
        return k;
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
    physx::PxMaterial *stone_ = nullptr;
    physx::PxMaterial *timber_ = nullptr;
    physx::PxControllerManager *controllers_ = nullptr;
    physx::PxController *controller_ = nullptr;
    physx::PxRigidStatic *groundActor_ = nullptr;
    std::vector<physx::PxRigidStatic *> statics_;
    std::vector<physx::PxRigidDynamic *> bodies_;
#endif
    int convexMade_ = 0, convexTried_ = 0;
    int groundI0_ = 0, groundJ0_ = 0, groundN_ = 0, groundStep_ = 1;
    float acc_ = 0.0f;
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v2
