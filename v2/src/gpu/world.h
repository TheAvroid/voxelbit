// ---------------------------------------------------------------------------
// world.h -- the endless wood, as DXR acceleration structures.
//
// THE DESIGN QUESTION IS STILL INSTANCING. Nine pine models meshed to their
// exposed faces come to millions of triangles between them; twenty-six rocks
// and six flowers more. A stand flattened would be unbuildable, and every tree
// would still be one of the nine shapes actually authored. One bottom-level
// structure per model, referred to through a top-level instance, keeps the
// memory at one copy of each while the scene draws thousands.
//
// The terrain is instanced the same way: not one structure built at startup but
// a ring of chunk structures that follows the camera. See scene/chunks.h for
// why that is affordable; this file owns the GPU half.
//
// WHAT THE MOVE OFF OPTIX ACTUALLY CHANGED, in one place:
//
//   THE SHADER BINDING TABLE IS GONE, AND WITH IT THE SLOT ALLOCATOR. Under
//   OptiX every resident chunk had to own a hit-group record so its closest-hit
//   program could find that chunk's triangle attributes. Records are addressed
//   by index, so residency became a slot-allocation problem: a fixed pool of
//   1024 records, a free list, a hand-written publish step whenever a chunk took
//   or released a slot, and a rule that the pool must be at least as large as
//   the ring or chunks silently stop drawing.
//
//   Inline ray tracing has no table to bind, so the question "which triangle
//   array does this instance read" is answered by a NUMBER THE INSTANCE
//   CARRIES. Every instance already needed a record in a structured buffer for
//   its tint; that record now also holds an offset into one shared pool of
//   packed triangle attributes. The slot pool, the free list, the publish
//   callback and the fixed ceiling all deleted, and the thing that replaced
//   them is an integer that is written when the instance array is built and
//   never touched again.
//
//   THE INSTANCE ARRAY IS ALSO REBUILT WHOLE rather than appended to. v2 pushed
//   one entry per tree onto a vector that was never trimmed, so a long session
//   walking through the wood grew it without bound -- a slow leak that nothing
//   would ever surface, because it is only a few bytes per tree. Here the array
//   is assembled from the resident chunks each time the top-level structure is
//   rebuilt, which is exactly when it can change, so an evicted chunk's entries
//   go with it.
//
// ROTATION IS IN QUARTER TURNS ONLY, for everything. An arbitrary yaw would put
// a voxel model off the lattice its own faces are aligned to, and the crisp
// axis-aligned silhouette that makes a voxel tree look like a voxel tree would
// turn into stair-stepped mush. It also keeps every transform orthonormal,
// which is what lets the shader transform a normal with the plain 3x3 instead
// of an inverse transpose.
//
// EVERY BOTTOM-LEVEL STRUCTURE IS COMPACTED. The driver builds into a
// conservatively sized buffer and reports afterwards how much it actually
// needed; for millions of small coplanar quads the compacted structure is
// routinely half the size. With hundreds of chunks resident that is the
// difference between fitting in 12 GB and not.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Fence.h"
#include "Core/API/RtAccelerationStructure.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "../../shaders/Shared.slang"
#include "../core/noise.h"
#include "../scene/chunks.h"
#include <cstdio>
#include <climits>
#include <functional>

#include "../physics/physics.h"
#include "../scene/collide.h"
#include "../scene/voxbox.h"
#include "../scene/sky.h"
#include "../scene/vox.h"
#include "../scene/voxelworld.h"

// The shader carries its own copies of these two, because Shared.slang has to
// compile as Slang and cannot include a C++ header. Neither has ever changed,
// but a silent disagreement about the voxel size would move every model
// relative to the ground it stands on, so it is checked rather than trusted.
static_assert(v2::VOXEL_M == v2::kVoxelM, "voxel size disagrees with the shader");
static_assert(v2::mat::GRASS_0 == v2::kGrass0, "grass ramp disagrees with the shader");
static_assert(v2::mat::GRASS_COUNT == v2::kGrassCount, "grass ramp disagrees with the shader");
static_assert(v2::mat::SOIL_0 == v2::kSoil0, "soil ramp disagrees with the shader");
static_assert(v2::mat::SOIL_COUNT == v2::kSoilCount, "soil ramp disagrees with the shader");
static_assert(v2::mat::LITTER_0 == v2::kLitter0, "litter ramp disagrees with the shader");
static_assert(v2::mat::LITTER_COUNT == v2::kLitterCount, "litter ramp disagrees with the shader");
// The packed triangle word. A disagreement here would not paint the floor the
// wrong colour, it would hand the shader the wrong NORMAL for every face whose
// direction bits moved -- so it is checked with the rest.
static_assert(v2::TRI_DIR_SHIFT == int(v2::kTriDirShift), "tri layout disagrees with the shader");
static_assert(v2::TRI_DIR_MASK == int(v2::kTriDirMask), "tri layout disagrees with the shader");
static_assert(v2::TRI_STRAND_SHIFT == int(v2::kTriStrandShift),
              "tri layout disagrees with the shader");
static_assert(v2::TRI_STRAND_MASK == int(v2::kTriStrandMask),
              "tri layout disagrees with the shader");
static_assert(v2::STRAND_MAX_ROWS == int(v2::kStrandRowMask) + 1,
              "strand height disagrees with the shader");
static_assert(v2::SUN_COS_THETA_MAX == v2::kSunCosThetaMax, "sun size disagrees with the shader");

namespace v2 {

using Falcor::Buffer;
using Falcor::Fence;
using Falcor::Device;
using Falcor::RenderContext;
using Falcor::ref;
using Falcor::ResourceBindFlags;
using Falcor::RtAccelerationStructure;
using Falcor::RtAccelerationStructureBuildFlags;
using Falcor::RtAccelerationStructureBuildInputs;
using Falcor::RtAccelerationStructureKind;
using Falcor::RtAccelerationStructurePostBuildInfoDesc;
using Falcor::RtAccelerationStructurePostBuildInfoPool;
using Falcor::RtAccelerationStructurePostBuildInfoQueryType;
using Falcor::RtGeometryDesc;
using Falcor::RtGeometryFlags;
using Falcor::RtGeometryInstanceFlags;
using Falcor::RtGeometryType;
using Falcor::RtInstanceDesc;

// ---------------------------------------------------------------------------
// The packed-triangle pool.
//
// Every instance in the scene reads its per-triangle (material, face) uint16
// out of ONE buffer, at an offset the instance carries. That is the whole of
// what replaced the shader binding table, and it needs an allocator because
// chunks come and go at different sizes.
//
// First fit with coalescing, over a std::map keyed by offset. Deliberately the
// dumbest thing that works: allocations happen a couple of times a frame at
// most, the map never holds more than a few hundred entries, and a fancier
// allocator would be untestable code sitting under a renderer.
//
// UNITS ARE UINT16, NOT BYTES, throughout -- an offset here is added directly
// to a primitive index in the shader.
// ---------------------------------------------------------------------------
class TriPool {
  public:
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    void init(const ref<Device> &device, size_t units) {
        device_ = device;
        units_ = units;
        buffer_ = device_->createBuffer(units_ * sizeof(uint16_t), ResourceBindFlags::ShaderResource,
                                        Falcor::MemoryType::DeviceLocal);
        buffer_->setName("v2::TriPool");
        free_.clear();
        free_[0] = units_;
    }

    const ref<Buffer> &buffer() const { return buffer_; }
    size_t capacityUnits() const { return units_; }
    size_t usedUnits() const {
        size_t f = 0;
        for (const auto &kv : free_) f += kv.second;
        return units_ - f;
    }

    // Reserve `count` uint16s and copy `data` into them. Grows the pool if it
    // has to, which costs a device-side copy of everything already in it -- so
    // the initial size is chosen from the ring radius to make that rare rather
    // than to make it impossible.
    uint32_t upload(RenderContext *ctx, const std::vector<uint16_t> &data) {
        if (data.empty()) return kInvalid;
        uint32_t off = take(data.size());
        if (off == kInvalid) {
            grow(ctx, data.size());
            off = take(data.size());
            if (off == kInvalid) return kInvalid;  // asked for more than the pool can ever hold
        }
        ctx->updateBuffer(buffer_.get(), data.data(), size_t(off) * sizeof(uint16_t),
                          data.size() * sizeof(uint16_t));
        return off;
    }

    void release(uint32_t offset, size_t count) {
        if (offset == kInvalid || count == 0) return;
        auto it = free_.emplace(offset, count).first;

        // Coalesce with the block after, then the one before. Without this a
        // few hours of walking fragments the pool into unusable slivers even
        // though the total free space never falls.
        auto next = std::next(it);
        if (next != free_.end() && it->first + it->second == next->first) {
            it->second += next->second;
            free_.erase(next);
        }
        if (it != free_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first) {
                prev->second += it->second;
                free_.erase(it);
            }
        }
    }

  private:
    ref<Device> device_;
    ref<Buffer> buffer_;
    size_t units_ = 0;
    std::map<uint32_t, size_t> free_;  // offset -> length, in uint16 units

    uint32_t take(size_t count) {
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second < count) continue;
            const uint32_t off = it->first;
            const size_t rest = it->second - count;
            free_.erase(it);
            if (rest > 0) free_[uint32_t(off + count)] = rest;
            return off;
        }
        return kInvalid;
    }

    void grow(RenderContext *ctx, size_t need) {
        const size_t want = std::max(units_ * 2, units_ + need * 2);
        ref<Buffer> nb = device_->createBuffer(want * sizeof(uint16_t),
                                              ResourceBindFlags::ShaderResource,
                                              Falcor::MemoryType::DeviceLocal);
        nb->setName("v2::TriPool");
        ctx->copyBufferRegion(nb.get(), 0, buffer_.get(), 0, units_ * sizeof(uint16_t));
        ctx->submit(true);  // the old buffer dies with this scope; it must be done being read
        release(uint32_t(units_), want - units_);
        buffer_ = nb;
        units_ = want;
        std::printf("v2: triangle pool grown to %.0f MB\n",
                    double(units_ * sizeof(uint16_t)) / (1024.0 * 1024.0));
    }
};

// THE TWO RAY MASKS, host side. Shared.slang carries the shader's copy and the
// note on what they are for; these are the same two bits, and the assert below
// is what stops them drifting.
constexpr uint8_t kMaskWorld = 0x01;
constexpr uint8_t kMaskHeld = 0x02;

// HOW MANY SHAFTS CAN BE IN THE AIR, as instances the structure reserves. It
// is here rather than in render/arrows.h because the STRUCTURE is what the
// number is really about: an update may not change how many instances there
// are, so the slots have to exist from the first build whether or not anything
// has been shot. See the note over the arrow block in rebuildTlas.
constexpr int kArrowInstances = 12;

// ...AND HOW MANY BUTTERFLIES, for exactly the same reason and at exactly the
// same cost. The flock is a fixed pool of slots that are filled and emptied as
// their homes come into range and leave it (render/butterflies.h), so the
// number that matters to the STRUCTURE is the ceiling, not how many are flying:
// every slot exists from the first build, masked off until something is in it.
//
// SIXTY-FOUR, against a scene of about forty-two thousand instances. The whole
// band is a rounding error in the top-level structure and its refit, and it is
// three times the flock the viewer opens with -- so `--butterflies` can be
// turned up on the command line without the structure having to be told.
// -- ONE BAND, TWO POPULATIONS ----------------------------------------------
//
// The butterflies and the perched songbirds are the same KIND of thing to the
// structure -- an orthonormal basis under a uniform scale, that moves -- so
// they share the flyer band rather than each reserving one. A band is reserved
// at a FIXED size because a TLAS update may not change the instance count (see
// refitTlas), so a second band would cost its slots whether or not anything
// used them, on every frame, for ever.
//
// Divided rather than pooled: each population owns a contiguous run and writes
// every slot in it, empty ones included. A shared free list would be smaller
// and would also mean a butterfly could take a bird's slot mid-flight, which is
// a motion vector between two unrelated objects -- the one thing this band
// exists to get right.
constexpr int kButterflySlots = 64;
constexpr int kBirdSlots = 48;
constexpr int kFlyerInstances = kButterflySlots + kBirdSlots;

// ---------------------------------------------------------------------------
// RE-MESHING A DAMAGED MODEL, OFF THE FRAME THAT DAMAGED IT.
//
// MEASURED, because this was guessed at twice before it was measured. One pick
// blow on the biggest boulder costs, on the main thread:
//
//     re-mesh   38.5 ms      colTop   3.0 ms
//     fill       9.8 ms      collider 0.0 ms      -- 51 ms, every blow
//
// A frame is 16.7 ms. That is the hitch, and three quarters of it is the
// re-mesh: 3.2 million voxels walked to emit 1,081,436 triangles, to change the
// nine hundred voxels a pick took out.
//
// IT CANNOT BE MADE CHEAPER IN PLACE. Skipping blocks that cannot show a face
// was tried and measured at 36.7 -> 34.3 ms: a boulder is porous, so almost no
// block is fully interior and there is nothing to skip. Greedy-merging the
// surface would cut the triangle count several-fold and is ruled out by the
// renderer -- this engine has no textures and shades every voxel by hashing its
// own position, so a merged quad would flatten the grain it exists to show.
//
// So it moves. The volume is edited on the frame the blow lands, which is
// cheap; the mesh is built on a worker and swapped in when it is ready, one or
// two frames later. Nothing waits for it: the chunk that flies at the player is
// meshed from the SPOIL and appears immediately, so the blow still lands the
// instant it is struck -- what arrives late is only the hole, and a hole that
// appears 30 ms after the sound is not something anybody has ever noticed.
//
// A SEQUENCE NUMBER, not a lock. Blows land every 570 ms and a mesh takes 40,
// so a second job for the same rock while the first is in flight is unlikely
// rather than impossible -- and when it happens the older result is simply
// dropped, because the instance's seq has moved past it.
// ---------------------------------------------------------------------------
class Remesher {
  public:
    struct Job {
        std::pair<long long, int> key;
        std::vector<uint8_t> vol;
        int sx = 0, sy = 0, sz = 0;
        uint32_t seq = 0;
    };
    struct Done {
        std::pair<long long, int> key;
        VoxMesh mesh;
        uint32_t seq = 0;
    };

    void start() {
        if (worker_.joinable()) return;
        stop_ = false;
        worker_ = std::thread([this] { run(); });
    }

    ~Remesher() {
        {
            std::lock_guard<std::mutex> lk(mx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void submit(Job &&j) {
        {
            std::lock_guard<std::mutex> lk(mx_);
            in_.push_back(std::move(j));
        }
        cv_.notify_one();
    }

    bool take(Done *out) {
        std::lock_guard<std::mutex> lk(outMx_);
        if (out_.empty()) return false;
        *out = std::move(out_.front());
        out_.pop_front();
        return true;
    }

    size_t pending() {
        std::lock_guard<std::mutex> lk(mx_);
        return in_.size();
    }

  private:
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [this] { return stop_ || !in_.empty(); });
                if (stop_) return;
                j = std::move(in_.front());
                in_.pop_front();
            }
            Done d;
            d.key = j.key;
            d.seq = j.seq;
            d.mesh = meshVolume(j.vol, j.sx, j.sy, j.sz, VOXEL_M);
            {
                std::lock_guard<std::mutex> lk(outMx_);
                out_.push_back(std::move(d));
            }
        }
    }

    std::thread worker_;
    std::mutex mx_, outMx_;
    std::condition_variable cv_;
    std::deque<Job> in_;
    std::deque<Done> out_;
    bool stop_ = false;
};

// ---------------------------------------------------------------------------
// WHAT HAS COME LOOSE, and how much of it may be loose at once.
//
// Everything that breaks away from the static world becomes one of these: a
// chip off a rock, a bite of hillside, and -- when it is felled -- a tree. They
// are rigid bodies in PhysX and instances in the top-level structure, and the
// only thing that separates a chip from a log is its size and what happens at
// the end of its life. A chip is small enough to pick up and comes to you; a
// log is not and stays where it fell.
//
// A FIXED BAND, like the flyers above and for the same reason: a slot that
// always exists is a refit, and a slot that appears is a rebuild.
// ---------------------------------------------------------------------------
constexpr int kDebrisInstances = 64;

// HOW LONG IT LIES THERE BEFORE IT COMES TO YOU. v1 uses 450 ms (itself halved
// from 900); a full second was tried and was too long -- a chip that has
// settled sits there doing nothing for most of it. Half a second is enough to
// watch it come loose and not enough for it to go dead.
//
// absorbFly is v1's and is a DURATION rather than a rate: smoothstepped, so a
// chunk leaves the ground gently and arrives fast.
constexpr double kAbsorbWaitMs = 500.0;

// ---------------------------------------------------------------------------
// THE STONE A LOOSE PIECE FALLS AGAINST.
//
// A window of the DAMAGED model's own voxels, merged into boxes -- see
// scene/voxbox.h for why that and not a hull, a height field, or the whole
// model as a mesh. 32 cells is 3.2 m a side, which is a couple of hundred boxes
// through the face of a boulder and enough room for a chip to roll about in for
// the second it exists. It follows the piece: once the piece is this far from
// where its window was centred, the next few metres of stone are built.
constexpr int kWindowCells = 32;
constexpr float kWindowFollowM = 0.8f;

// Granite, near enough. It matters: mass and inertia are what make a chip
// settle like a rock rather than drift like a prop, and PhysX derives both from
// this and the hull's volume.
constexpr float kStoneDensity = 2600.0f;

// See the note where the hull is built: the convex hull of a digital sphere
// bulges past the voxels it was made from, and a body born penetrating gets
// pushed.
constexpr float kHullShrink = 0.95f;

// HOW FAR THE DRAWN SHIVER LEANS. Six degrees: enough to read as a stone that
// has not finished moving, small enough that it never looks like the piece is
// somewhere it is not. See the note where it is applied.
constexpr float kWobbleRad = 0.105f;

// UNDER THIS MANY VOXELS A MODEL IS RE-MESHED ON THE SPOT. Measured: a 220,000
// voxel rock meshes in 4.7 ms and a 1,530 voxel one in 0.1, while the 3.2
// million voxel boulders take 38.5 -- so the line is drawn where the work stops
// fitting in a frame, and everything under it keeps the simpler path and no
// latency at all.
constexpr size_t kInlineMeshVox = 400000;


// ---------------------------------------------------------------------------
// FELLING A TREE.
//
// THE COARSENESS IS MEASURED, not chosen. A pine is 31,000 solid voxels in a
// 71 x 264 x 69 box, and greedily merging those at their own 10 cm gives 10,758
// boxes -- a branchy model merges badly, and no dynamic body wants ten thousand
// shapes. The same tree at 40 cm cells is 630 boxes and at 60 cm is 276. So the
// collider is built at 40 cm and coarsened again if that still comes out too
// heavy, which for the biggest birch (105,000 voxels) it does.
constexpr float kFellCellM = 0.4f;
constexpr size_t kFellMaxBoxes = 420;

// A FELLED TREE COLLIDES AS ITS TRUNK, AND AS NOTHING ELSE.
//
// The collider was the whole model coarsened to kFellCellM, marking a cell
// solid if ANY voxel fell in it. That is the right rule for a boulder, which is
// dense; a tree is a thin trunk inside a huge sparse canopy, and it turned that
// canopy into a nearly solid cone -- MEASURED at 3.8 to 5.7 times the volume of
// the wood, and hundreds of tonnes of it.
//
// Requiring a fill fraction helped and did not fix it. The headless trace (see
// App::runFellTest) showed why: a cone still cannot LIE DOWN. Once the tree is
// over, a large part of that shape is under the terrain, the solver pushes it
// out every step, and the energy goes into spin -- 0.79 rad/s at the landing,
// then 0.99, 1.13, 1.34, 1.64, until the tree tumbled out of the world at
// 28 m/s. A body gaining angular velocity while lying on the ground is a body
// being fed energy by its own contacts.
//
// So the collider is the TRUNK: the cells inside the model's own base footprint
// -- what measureCollider already measures as the part that meets the ground,
// offset included, so it follows a birch's trunk out from under its leaning
// crown. That is a long thin column of boxes, which is what a felled tree is:
// it lies flat, it penetrates shallowly, and it settles. The canopy is not
// given to the solver at all, and branches that pass through the ground are
// what branches do when a tree comes down on them.
constexpr float kFellFillFrac = 0.15f;
// How far past the base footprint the trunk collider reaches, so it is a log
// rather than a wire. Never below this, for a sapling with a 2-voxel stem.
constexpr float kTrunkPadM = 0.20f;


// AND SO IS THE SEVER TEST. A flood fill from the model's bottom rows says what
// is still connected to the ground, and it is run after every axe blow -- so it
// runs on a COARSE grid: 20 cm cells cost between 0.02 and 0.79 ms on the trees
// here, and the same fill at 10 cm would be eight times that on the frame a
// blow already re-meshes and rebuilds a structure.
constexpr int kSeverCell = 2;   // voxels per cell

// ...AND HOW MUCH OF IT HAS TO COME AWAY BEFORE IT IS A FELL RATHER THAN A
// NICK. Measured on the real models: a glancing blow leaves 0% of the tree
// disconnected and a cut through the trunk leaves 99%, so anything in the
// middle of that range works and none of it is delicate.
constexpr float kFellFraction = 0.40f;

// ...OR, FOR SOMETHING WITHOUT A TRUNK, simply enough of it to see. A boulder
// undercut at the base sheds a piece that is a small fraction of the rock and
// is still a rock hanging in the air.
//
// THIS WAS SIXTEEN CELLS -- about a tenth of a cubic metre -- AND A BRANCH FELL
// STRAIGHT THROUGH THE GAP. The old floor was reasoned about boulder flecks,
// where "you could not see the difference anyway" is true. It is not true of a
// branch: a two-metre limb fifteen centimetres thick is 0.045 m3, about seven
// cells, so it was under the floor. fellTree declined to give it a body, the
// hanger sweep only looks in a box around the bite, and nothing else removes
// it -- so a severed branch HUNG IN THE AIR. Reported twice.
//
// Four cells is about a thirty-centimetre cube. That is above a fleck and
// below any limb worth the name, which is the whole range that has to be
// separated here. The debris band is still protected: what this floor is for
// is stopping three stray voxels from each taking one of the sixty-four slots,
// and three voxels is well under four cells.
//
// THE RULE THIS SERVES: anything that can be broken off the static grid is
// subject to gravity. There is no size at which floating becomes acceptable --
// only a size below which the piece should be REMOVED rather than given a body
// of its own.
constexpr int kMinLooseCells = 4;

// Green timber. Roughly what a living conifer weighs once it is wet.
constexpr float kTimberDensity = 750.0f;

// How hard it is pushed off the stump. Small on purpose: the fall is gravity
// acting on a twenty-six metre lever, and this only has to break the standing
// equilibrium -- see Physics::nudgeSpin.
constexpr float kFellNudge = 0.25f;   // rad/s

// ---- WHAT COUNTS AS HAVING THE GROUND DUG OUT FROM UNDER YOU --------------
//
// How far a placement's base may stand above the ground before it is standing
// on air. A model is SUNK into the terrain when it is placed and its underside
// is a staircase against a height field, so a centimetre or two either way is
// meshing rather than daylight. Same 15 cm the rooted-tree probe calls level.
constexpr float kUndermineTolM = 0.15f;
// How far below a column's generated top the walk looks for stone before
// calling it a shaft. 6.4 m is deeper than any bite the pick can take.
constexpr int kUndermineDepthVox = 64;
// How far from a blow to go looking for something the blow undermined. A big
// boulder's centre can be ten metres from the hole its edge is standing over.
constexpr float kUndermineReachM = 24.0f;

// HOW FAR UNDER THE GROUND THE WHOLE OF A FELLED BODY HAS TO BE before it is
// treated as having gone through the world rather than as resting in a hollow.
// A trunk lying in a dip has its underside below the ground at the corners of
// its own bounding box every frame; nothing that is still partly above the
// surface is ever touched.
constexpr float kUnderWorldM = 0.5f;

// ---- EVERYTHING NEARBY IS SOLID, AND THIS IS WHAT THAT COSTS --------------
//
// "I saw a tree clip into a rock when it felled." It did, and it had to: a
// felled tree was given ONE 3.2 m cube of stone to fall against, built at its
// stump the moment it came down and never moved again -- see buildWindow, which
// is right for a 30 cm chip and useless for a 26 m trunk. Everything past that
// cube was empty air to the solver.
//
// So a falling body is given the models around it as real colliders: each one's
// own voxels, coarsened to this cell and merged into boxes, placed by the
// instance transform. MEASURED per model at 0.4 m -- the same cell the falling
// side uses, so the two are described at one resolution: 808 boxes for the
// largest boulder in the set, 696 for the next, 630-754 for a pine, 167 for a
// mid rock, 7 for a small one. A dozen of those is a few thousand shapes, which
// is a static actor PhysX carries without noticing.
constexpr float kStaticCellM = 0.4f;
// How much of a cell has to be wood before a standing tree is solid there. The
// boulder rule -- "any voxel" -- turns a canopy into a filled cone, and a
// felled trunk would come to rest on a needle. Same fraction the falling side
// uses on itself.
constexpr float kStaticFillFrac = 0.15f;
// The ceiling on one window. Past this the window has been asked for somewhere
// impossible -- a clearing packed with big rock -- and a partial collider is
// still a collider.
constexpr size_t kStaticMaxBoxes = 6000;
// How far past a body's own bounds the window reaches, and how far inside those
// bounds the body may travel before it is rebuilt. The gap between the two is
// what stops a rebuild every frame.
constexpr float kStaticPadM = 3.0f;

// ---- AND THE GROUND ITSELF LEAVES NOTHING HANGING ------------------------
//
// How far around a bite to look for terrain the bite cut loose. A hole is a
// 30 cm sphere, so anything it disconnects is within a voxel or two of it and
// this is generous; the cost is a flood over the box, once per blow.
// MEASURED before it existed: 55% of dig sites left terrain standing on air,
// 448 voxels over 300 sites, the worst site 26.
constexpr int kHangBoxVox = 13;
// ...and WIDER for a model, because a model's flood is pure array work while
// the terrain's evaluates several octaves of noise per column. MEASURED over
// 1,224 blows on the real models, voxels left hanging per blow: 6.2 with no
// sweep at all, 1.6 at 13, 0.6 at 19, 0.3 at 25, 0.0 at 33 -- and the cost is
// the cube of it, so 33 is fifteen times the work of 13 to catch the last
// needle. 19 is where that curve stops being worth paying.
constexpr int kHangBoxModelVox = 19;

constexpr double kAbsorbFlyMs = 672.0;
// Above this many voxels a piece is scenery rather than loot: it stays where it
// landed until it is broken down. 600 in v1, measured against what a felled
// pine actually yields.
constexpr int kAbsorbSize = 600;
// The chunk arrives at the chest, not at the eye -- 12 voxels under it.
constexpr float kAbsorbY = -1.2f;
// Nothing loose lives forever. A piece too big to absorb still stops being a
// rigid body eventually, or a morning's chopping is a thousand live actors.
constexpr double kDebrisLifeMs = 30000.0;

// A FELLED TREE IS NOT DEBRIS. Half an hour of it lying where it fell, rather
// than the thirty seconds a chip gets -- it is scenery now, and scenery that
// blinks out while you are looking at it is worse than scenery that was never
// there. It still goes eventually: a debris slot is a fixed band of 64 and the
// wood is endless.
constexpr double kFelledLifeMs = 1800000.0;

// ...AND WHAT HAS BEEN PUT DOWN. Eight, which is the JS engine's own cap on
// dropped items, reserved for the same reason every other band here is: an
// update may not change how many instances there are, so the slots exist from
// the first build whether or not anything has been thrown.
constexpr int kDropInstances = 8;
static_assert(kMaskWorld == uint8_t(MASK_WORLD), "ray masks disagree with the shader");
// THERE IS NO LID, and the shader is written on that assumption: it takes the
// hit point as the surface (kWaveSwellM == 0). Water meshed any higher than
// the waterline stands proud of its own beach -- see VoxelTerrain::waveVoxMax.
static_assert(v2::VoxelTerrain{}.waveVoxMax == 0,
              "the water lid is back; the tracer assumes there is none");
static_assert(kMaskHeld == uint8_t(MASK_HELD), "ray masks disagree with the shader");

// A bottom-level structure and the buffer under it.
struct Blas {
    ref<Buffer> buffer;
    ref<RtAccelerationStructure> as;
    bool valid() const { return as != nullptr; }
};

// One instanced model: a pine, a rock, a flower. They differ only in what
// scatters them and how deep it sinks them.
struct ModelTemplate {
    Blas blas;
    uint32_t triOffset = TriPool::kInvalid;
    size_t tris = 0;
    int sx = 0, sy = 0, sz = 0;
    // What a body runs into, measured off this model's own voxels at load time.
    // See scene/collide.h for why it is measured rather than chosen.
    ModelCollider col;
    // Only filled for pines: every voxel in the crown a cone could rest on.
    std::vector<Perch> perches;
    // Anchors wide enough for a beehive -- see the clearW note on
    // collectPerches. Only filled for the birch wood; a pine has no use for
    // them and computing them is a 5x5 sweep per candidate voxel.
    std::vector<Perch> hivePerches;
    // The top of every column, for the collider. See columnTops.
    std::vector<int16_t> colTop;

    // ---------------------------------------------------------------------
    // THE MODEL'S VOXELS, AND ALL OF THEM -- the inside as well as the shell.
    //
    // The mesher takes this asset, emits the faces where solid meets air, and
    // drops everything else on the floor; that is why a rock is a hollow shell
    // and why breaking into one has nothing to show. The .vox file has always
    // held the interior. This is simply keeping it.
    //
    // ONE ARRAY PER MODEL, NOT PER INSTANCE. Twenty-five pines share one pine.
    // The last attempt at voxels copied every placement into a world grid --
    // 28,800 models, 4.58M voxels, re-copied whenever the window moved. An
    // instance here is a transform and a pointer, and stays that way until
    // something damages it and it needs a private copy.
    //
    // Stored as GLOBAL material ids, not palette entries: idOfEntry is local to
    // the load and every consumer wants an id.
    // ---------------------------------------------------------------------
    std::vector<uint8_t> volume;   // sx*sy*sz, 0 = empty, VoxAsset layout

    // ---------------------------------------------------------------------
    // HOW MUCH OF THIS MODEL WAS ALREADY LOOSE WHEN IT WAS DRAWN.
    //
    // The sever rule asks how many coarse cells are no longer connected to the
    // model's bottom, and a floor on that count is what makes an undercut
    // boulder drop its top. That floor was measured against ZERO, and no model
    // starts at zero: MEASURED with the shipped 2-voxel fill, before any axe
    // lands, pine_2 is already 21 cells adrift, pine_3 is 51 and pine_9 is 16 --
    // against a floor of 16. Those three pines were felled by their FIRST blow,
    // wherever it landed, and pine_8 sat one needle short at 15.
    //
    // It is not damage, it is how the models are drawn: a pine is 774 separate
    // six-connected pieces, most of them needle clusters hanging in air, and
    // BIG_1_BiG_0 is a boulder resting on its own base plate across a one-voxel
    // gap -- 95% of it adrift at voxel resolution, which is what the coarse
    // cells are there to bridge.
    //
    // So the floor is measured against THIS, and the question becomes what the
    // BLOW disconnected rather than what the artist did. Computed once, on the
    // first blow the model ever takes, and kept for the life of the world.
    // ---------------------------------------------------------------------
    mutable int bornLoose = -1;    // < 0 until asked for
    mutable long bornVoxLoose = -1;   // ...and the same count per voxel, for the probe

    // THIS MODEL AS A COLLIDER, in its own frame, metres from its (0,0,0)
    // voxel corner. Built once, on the first body that has to fall past one of
    // these, and then shared by every placement of it -- a quarter turn takes
    // an axis-aligned box to an axis-aligned box, so an instance is this list
    // with its centres turned and its half extents swapped. See kStaticCellM.
    mutable std::vector<VoxBox> staticBoxes;
    mutable bool staticBuilt = false;
};

// ---------------------------------------------------------------------------
// A recycling pool for the buffers a structure build needs and then discards.
//
// WHY IT EXISTS. Four device buffers are created per chunk -- vertices,
// indices, build scratch and the uncompacted result -- and a profile of a
// flight put 2.5 seconds of a 4-second streaming cost in the RECORDING pass,
// not in either of the two device waits everyone assumes is the problem.
// Creating a buffer is a driver-side heap allocation, twelve hundred of them
// get made while crossing a wood, and every one is the same handful of sizes
// because every chunk is the same 65 536 columns of the same terrain.
//
// So they are kept and handed back out. SIZES ARE ROUNDED UP TO A GRANULE so
// the pool converges on a few dozen buffers instead of one per distinct byte
// count -- without that it is a leak with extra steps.
//
// NOTHING HERE EVER BLOCKS TO FIND OUT WHETHER A BUFFER IS FREE. A slot
// carries the fence value its last user was submitted under, and it is
// reusable once the device has passed that value -- which is a poll, not a
// wait. That is the whole reason this can sit on the frame's own thread.
// ---------------------------------------------------------------------------
class TransientPool {
  public:
    void init(const ref<Device> &d, ResourceBindFlags flags, const char *name) {
        device_ = d;
        flags_ = flags;
        name_ = name;
    }

    ref<Buffer> acquire(size_t bytes, uint64_t deviceDone) {
        bytes = granule(bytes);
        Slot *best = nullptr;
        for (Slot &s : slots_) {
            if (s.busy || s.freeAt > deviceDone || s.size < bytes) continue;
            // Smallest that fits, so a 12 MB request cannot consume the one
            // 40 MB buffer the next model needs and force a new allocation.
            if (!best || s.size < best->size) best = &s;
        }
        if (!best) {
            slots_.push_back({});
            best = &slots_.back();
            best->buf = device_->createBuffer(bytes, flags_, Falcor::MemoryType::DeviceLocal);
            best->buf->setName(name_);
            best->size = bytes;
            bytes_ += bytes;
        }
        best->busy = true;
        return best->buf;
    }

    // Hand a buffer back, unusable again until the device has passed `at`.
    // `frameNo` is only ever read by trim, which ages on frames rather than on
    // the fence -- see the note where it is called.
    void release(const ref<Buffer> &b, uint64_t at, uint64_t frameNo) {
        if (!b) return;
        for (Slot &s : slots_)
            if (s.buf.get() == b.get()) {
                s.busy = false;
                s.freeAt = at;
                s.freeFrame = frameNo;
                return;
            }
    }

    size_t bytes() const { return bytes_; }
    size_t count() const { return slots_.size(); }

  private:
    struct Slot {
        ref<Buffer> buf;
        size_t size = 0;
        uint64_t freeAt = 0;     // device fence value that makes it reusable
        uint64_t freeFrame = 0;  // frame it was handed back, for ageing out
        bool busy = false;
    };
    // THREE SIGNIFICANT BITS, in megabytes. A buffer is at most an eighth
    // bigger than it needed to be, and -- far more usefully -- there are only
    // a handful of distinct sizes for the pool to keep one of. At a flat
    // megabyte the classes were as fine-grained as the mesh sizes themselves,
    // so almost every request missed and the pool became a list of every size
    // a chunk had ever happened to be.
    static size_t granule(size_t b) {
        size_t mb = (b + (1u << 20) - 1) >> 20;
        if (mb == 0) mb = 1;
        size_t step = 1;
        while ((mb >> 3) >= step) step <<= 1;  // keep the top three bits
        mb = ((mb + step - 1) / step) * step;
        return mb << 20;
    }

  public:
    // Give back what nobody has asked for in a while. Called once a frame, so
    // the pool is sized by what the streamer is CURRENTLY doing rather than by
    // the worst thing it ever did -- the prime asks for far more at once than
    // flying ever does, and without this the whole prime's peak stayed resident
    // for the session.
    void trim(uint64_t beforeFrame) {
        for (size_t i = slots_.size(); i-- > 0;) {
            const Slot &s = slots_[i];
            if (s.busy || s.freeFrame == 0 || s.freeFrame > beforeFrame) continue;
            bytes_ -= s.size;
            slots_.erase(slots_.begin() + ptrdiff_t(i));
        }
    }

  private:

    ref<Device> device_;
    ResourceBindFlags flags_ = ResourceBindFlags::None;
    const char *name_ = "v2::transient";
    std::vector<Slot> slots_;
    size_t bytes_ = 0;
};

// A chunk that is resident on the GPU. Its mesh is already in world space, so
// its own instance is an identity transform that exists only to carry the
// triangle offset; the decor it holds is placed.
// One decor placement, as the rest of the engine needs to find it again.
struct DecorAt {
    uint8_t kind = 0;   // 0 pine, 1 rock, 2 flower, 3 mushroom, 4 cone, 5 hive
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Chunk {
    int cx = 0, cz = 0;
    Blas blas;
    uint32_t triOffset = TriPool::kInvalid;
    size_t tris = 0;
    std::vector<RtInstanceDesc> decorDesc;
    std::vector<V6Instance> decorInfo;
    // WHAT EACH SLOT IS AND WHERE IT STANDS, parallel to decorDesc.
    //
    // decorDesc is a transform and decorInfo is a shading record; neither says
    // "this one is a pinecone". Something that removes a TREE has to be able to
    // find the cones and the hive that were hung in it, and they are separate
    // placements with no link back -- so this is the link, and it is three
    // floats and a byte per placement.
    std::vector<DecorAt> decorAt;
    // The trees and rocks of this chunk as things to walk into. Held per chunk
    // rather than in one world-wide list so eviction is free: the colliders go
    // when the chunk does, and nothing has to be searched to remove them.
    std::vector<Solid> solids;
    // Placements by kind -- pine, rock, flower. Held per chunk so eviction
    // keeps the totals honest without anything having to be searched.
    int decorKind[6] = {0, 0, 0, 0, 0, 0};  // pine, rock, flower, mushroom, cone, hive
    // Whether this chunk drew water. The wave scheduler walks only these, and
    // on this world under one chunk in a hundred is wet.
    bool hasWater = false;
};

inline long long chunkKey(int cx, int cz) {
    // Packed so a std::map can order them; 21 bits each side is +/- a million
    // chunks, which at 25.6 m is a world about 50 000 km across.
    return (static_cast<long long>(cx) << 21) ^ static_cast<long long>(cz);
}

// ---------------------------------------------------------------------------
class World {
  public:
    VoxelTerrain terrain;
    Sky sky;
    Palette palette;

    std::string pineDir = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decorDir = "C:/voxelbit/game/assets/decoration";
    // The birch wood's sixteen trees. Same 10 cm grid as the pines.
    std::string birchDir = "C:/voxelbit/game/assets/foilage/birch_trees";
    int viewChunks = 12;  // ring radius, in chunks
    float treeDensity = 0.3210f;
    float rockDensity = 0.010f;
    float flowerDensity = 0.45f;
    float mushroomDensity = 0.015f;
    int pineconesPerTree = 14;
    uint32_t seed = 20260904u;
    int meshThreads = 0;

    int loadedPines = 0, loadedRocks = 0, loadedFlowers = 0, loadedMushrooms = 0;
    int loadedPinecones = 0;
    int mushroomBig0 = 0;
    size_t uniqueTris = 0;

    // A ref rather than a raw pointer: ShaderVar::setAccelerationStructure
    // takes one, and it is the only thing the tracer needs from here.
    const ref<RtAccelerationStructure> &tlasRef() const { return tlas_; }
    const ref<Buffer> &triPool() const { return pool_.buffer(); }
    const ref<Buffer> &instanceBuffer() const { return instanceInfo_; }
    const ref<Buffer> &materialBuffer() const { return materials_; }

    // What the kept volumes cost, per kind. Reported at load rather than
    // guessed: the rocks are upscaled twice and are the whole budget.
    void reportVolumes() const {
        struct Row { const char *name; const std::vector<ModelTemplate> *v; };
        const Row rows[] = {{"pines/birches", &pines_}, {"rocks", &rocks_},
                            {"flowers", &flowers_},     {"mushrooms", &mushrooms_},
                            {"pinecones", &pinecones_}, {"hives", &hives_}};
        size_t total = 0, solidTotal = 0;
        for (const Row &r : rows) {
            size_t bytes = 0, solid = 0;
            for (const ModelTemplate &m : *r.v) {
                bytes += m.volume.size();
                for (uint8_t v : m.volume)
                    if (v) ++solid;
            }
            total += bytes;
            solidTotal += solid;
            if (!r.v->empty())
                std::printf("  volume   %-14s %2zu models %8.2f MB  %4.1f%% solid\n",
                            r.name, r.v->size(), double(bytes) / 1048576.0,
                            bytes ? 100.0 * double(solid) / double(bytes) : 0.0);
        }
        std::printf("  volume   %-14s          %8.2f MB  (%.1f M solid voxels)\n",
                    "TOTAL", double(total) / 1048576.0, double(solidTotal) / 1e6);
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // THE TOOL IN THE PLAYER'S HAND, as geometry.
    //
    // Loaded exactly the way a rock is -- the same .vox reader, the same
    // Palette::forModelColor for every colour it uses, the same mesher, the
    // same triangle pool, the same bottom-level structure. It is a decor model
    // that happens to be placed by the camera instead of by the scatter, and
    // everything downstream of here treats it as one.
    //
    // MESHED AT ONE UNIT PER VOXEL, not at VOXEL_M. A held tool's voxels are a
    // few millimetres, the size depends on the pose and on the field of view,
    // and both move on a slider -- so the size lives in the instance transform
    // as a uniform scale rather than baked into vertices that would have to be
    // re-meshed every time the slider twitched. It is the only instance in this
    // scene carrying a scale, which is why KIND_HELD exists: the shader has to
    // normalise the normal it transforms. See Trace.cs.slang.
    //
    // Returns the grid's extents so the caller can build a pose around the
    // model's own box; false if the file will not load, which is not fatal --
    // an engine that refuses to start over a missing viewmodel would be a worse
    // bug than the missing viewmodel.
    // -----------------------------------------------------------------------
    int addHeldModel(const std::string &path, int *sx, int *sy, int *sz) {
        VoxModel mo;
        std::string err;
        if (!voxLoad(path, &mo, &err)) {
            std::fprintf(stderr, "v2: held item %s: %s -- skipped\n", path.c_str(), err.c_str());
            return -1;
        }
        return addHeldVox(mo, path, sx, sy, sz);
    }

    // The same, for a model that was COMPOSED rather than read. The bow's draw
    // frames are cut out of one file by render/bow.h and never exist on disk as
    // models of their own, so there is nothing for addHeldModel to open; `what`
    // is only ever printed.
    int addHeldVox(const VoxModel &mo, const std::string &path, int *sx, int *sy, int *sz) {
        // WHOLE, NOT TRIMMED. A held model's pose is measured from the middle
        // of its grid, and a strip's frames have to share one -- see
        // toWorldWhole for what trimming does to a bow.
        VoxAsset a = toWorldWhole(mo);
        if (a.sx <= 0) {
            std::fprintf(stderr, "v2: held item %s is empty -- skipped\n", path.c_str());
            return -1;
        }

        // ONLY THE ENTRIES THE MODEL USES, exactly as loadDecor does:
        // registering all 255 of a file's palette floods a table that is 255
        // entries for the whole world, and past the end forModelColor returns
        // AIR and real voxels stop being drawn.
        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        std::vector<bool> used(256, false);
        for (uint8_t v : a.a) used[v] = true;
        int minted = 0;
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)]) {
                idOfEntry[size_t(e)] = palette.forModelColor(mo.pal[size_t(e) - 1]);
                if (idOfEntry[size_t(e)] != mat::AIR) ++minted;
            }
        // The entries this just minted are not on the GPU yet: init() uploaded
        // the table before this was called, because the world has to exist
        // before anything can be held in front of it.
        uploadMaterials();

        const VoxMesh mesh = meshAsset(a, idOfEntry, 1.0f);
        if (mesh.triCount() == 0) {
            std::fprintf(stderr, "v2: held item %s meshed to nothing -- skipped\n", path.c_str());
            return -1;
        }

        HeldModel hm;
        hm.tri = pool_.upload(ctx_, mesh.tri);
        hm.blas = buildBlas(mesh);
        if (!hm.blas.valid()) return -1;
        hm.sx = a.sx;
        hm.sy = a.sy;
        hm.sz = a.sz;
        held_.push_back(std::move(hm));
        // How many triangle-pool units this model owns, so replaceHeldVox can
        // give them back. Kept beside held_ rather than in HeldModel because
        // every other reader of that struct wants only the three fields it
        // already has.
        heldTris_.push_back(mesh.tri.size());
        heldGen_.push_back(0);

        if (sx) *sx = a.sx;
        if (sy) *sy = a.sy;
        if (sz) *sz = a.sz;
        std::printf("v2: held item %s  %dx%dx%d, %zu tris, %d materials\n", path.c_str(), a.sx,
                    a.sy, a.sz, mesh.triCount(), minted);
        std::fflush(stdout);
        // The slot has to exist in the structure from the next rebuild on --
        // see setHeldInstance for why it is present even while nothing is held.
        // Rebuilt on the FIRST model only: the slot is one instance whatever is
        // in it, so a second tool changes nothing about the structure's shape.
        if (held_.size() == 1) rebuildTlas();
        return int(held_.size()) - 1;
    }

    // -----------------------------------------------------------------------
    // Rebuild a held model that already exists, keeping its index.
    //
    // FOR TUNING, and the bow's arrow is the only caller: nudging it recomposes
    // all fourteen frames of the strip, and adding fourteen more models on
    // every nudge would grow the pool and the structure list for the rest of
    // the session. Same index in, same index out, so every Tool that names it
    // and every instance that points at it stay correct.
    //
    // THE NEW SLICE IS UPLOADED BEFORE THE OLD ONE IS RELEASED. Released first,
    // the allocator is free to hand the same bytes straight back -- and a frame
    // still in flight would then read this model's NEW attributes at the old
    // offset. Doing it in this order costs one slice of pool for the length of
    // the call and cannot alias.
    //
    // The structures underneath are Falcor resources, and Falcor defers a
    // resource's destruction to its frame fence, so dropping the old one here
    // does not pull it out from under a frame that is still reading it.
    // -----------------------------------------------------------------------
    bool replaceHeldVox(int index, const VoxModel &mo, const std::string &what, int *sx, int *sy,
                        int *sz) {
        // BOTH lists, though they are pushed together and cannot differ: the
        // one that is indexed without being checked is the one that reads off
        // the end the day somebody adds a second way to make a held model.
        if (index < 0 || index >= int(held_.size()) || index >= int(heldTris_.size()) ||
            index >= int(heldGen_.size()))
            return false;
        VoxAsset a = toWorldWhole(mo);
        if (a.sx <= 0) return false;

        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        std::vector<bool> used(256, false);
        for (uint8_t v : a.a) used[v] = true;
        const int before = palette.minted();
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)]) idOfEntry[size_t(e)] = palette.forModelColor(mo.pal[size_t(e) - 1]);
        // ONLY IF THE MODEL ACTUALLY BROUGHT A COLOUR. A rebuild is the same
        // file it was first read from, so every entry is already in the table
        // and forModelColor hands back the id it minted at load. Re-uploading
        // anyway would create a fresh materials buffer for every frame of the
        // strip, on every nudge, to say what the table already said.
        if (palette.minted() != before) uploadMaterials();

        const VoxMesh mesh = meshAsset(a, idOfEntry, 1.0f);
        if (mesh.triCount() == 0) {
            std::fprintf(stderr, "v2: %s meshed to nothing -- kept the old one\n", what.c_str());
            return false;
        }
        const uint32_t tri = pool_.upload(ctx_, mesh.tri);
        if (tri == TriPool::kInvalid) return false;
        // A NEW GENERATION FIRST, then the build that belongs to it: any
        // compaction still in flight for this slot is now stale and will be
        // dropped when it lands. See PendingCompact::heldGen.
        ++heldGen_[size_t(index)];
        Blas blas = recordHeldBuild(mesh, index);
        if (!blas.valid()) return false;

        HeldModel &hm = held_[size_t(index)];
        const uint32_t oldTri = hm.tri;
        hm.tri = tri;
        hm.blas = std::move(blas);
        hm.sx = a.sx;
        hm.sy = a.sy;
        hm.sz = a.sz;
        if (oldTri != TriPool::kInvalid) pool_.release(oldTri, heldTris_[size_t(index)]);
        heldTris_[size_t(index)] = mesh.tri.size();

        // THE SLOT IS CACHING THE OLD OFFSET. setHeldInstance only re-uploads
        // an instance's record when the MODEL INDEX changes, which it has not
        // -- so without this the structure would go on pointing the bow at the
        // slice that has just been freed. Forgetting which model is in the slot
        // makes the next write re-state it.
        heldModel_ = -1;
        if (sx) *sx = a.sx;
        if (sy) *sy = a.sy;
        if (sz) *sz = a.sz;
        return true;
    }

    bool heldLoaded() const { return !held_.empty(); }
    int heldModelCount() const { return int(held_.size()); }

    // -----------------------------------------------------------------------
    // A FLYER'S FRAME, as geometry, and it is deliberately not the held path.
    //
    // Everything a tool loads through -- the reader, the palette, the mesher,
    // the pool, the structure -- is the same. ONE number differs and it is the
    // one that decides which of two worlds the model lives in:
    //
    //   A HELD MODEL IS MESHED AT ONE UNIT PER VOXEL and shrunk by its instance
    //   transform, because a viewmodel's size is a slider and re-meshing on
    //   every twitch of it is absurd.
    //
    //   A FLYER IS MESHED AT VOXEL_M, so its vertices are already in metres and
    //   the instance transform is a TURN AND A TRANSLATION -- the same shape
    //   every tree and rock in this world carries. That is what keeps it on the
    //   terrain's own path: the tracer's note over the normal transform says
    //   every 3x3 in this scene is orthonormal, and a butterfly's is, where a
    //   voxel-scaled one would not have been.
    //
    // (The fade a flyer materialises through does put a scale back into that
    // 3x3 -- see KIND_FLYER. It is a uniform one, so the transform stays a
    // rotation times a number, and one normalise in the tracer covers it.)
    //
    // WHOLE, NOT TRIMMED, for the same reason a bow's draw frames are: the
    // eight flap frames are one animation and have to share an origin, and
    // trimming each to its own occupied bounds would shift the body sideways
    // every time a wing came up.
    // -----------------------------------------------------------------------
    int addFlyerModel(const VoxModel &mo, const std::string &what, int *sx, int *sy, int *sz) {
        VoxAsset a = toWorldWhole(mo);
        if (a.sx <= 0) {
            std::fprintf(stderr, "v2: flyer %s is empty -- skipped\n", what.c_str());
            return -1;
        }
        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        std::vector<bool> used(256, false);
        for (uint8_t v : a.a) used[v] = true;
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)])
                // NOT A CONIFER: see the note over forModelColor for what the
                // green rule does to a lime butterfly's wing.
                idOfEntry[size_t(e)] = palette.forModelColor(mo.pal[size_t(e) - 1], false);
        uploadMaterials();

        const VoxMesh mesh = meshAsset(a, idOfEntry, VOXEL_M);
        if (mesh.triCount() == 0) {
            std::fprintf(stderr, "v2: flyer %s meshed to nothing -- skipped\n", what.c_str());
            return -1;
        }
        HeldModel fm;
        fm.tri = pool_.upload(ctx_, mesh.tri);
        fm.blas = buildBlas(mesh);
        if (!fm.blas.valid()) return -1;
        fm.sx = a.sx;
        fm.sy = a.sy;
        fm.sz = a.sz;
        flyers_.push_back(std::move(fm));

        if (sx) *sx = a.sx;
        if (sy) *sy = a.sy;
        if (sz) *sz = a.sz;
        // The band has to exist in the structure from the next rebuild on --
        // see the note over kFlyerInstances. On the FIRST model only: the band
        // is the same size whatever is loaded into it.
        if (flyers_.size() == 1) rebuildTlas();
        return int(flyers_.size()) - 1;
    }

    bool flyersLoaded() const { return !flyers_.empty(); }

    // -----------------------------------------------------------------------
    // Where a dropped item is this frame.
    //
    // It draws a HELD model -- the axe you were carrying is the axe on the
    // ground -- so `model` indexes held_, not flyers_. Same batching as the
    // flock: written into the host's copy here and sent in one range by
    // flushDropInstances, because eight instances that all move is eight
    // driver calls a frame otherwise.
    // -----------------------------------------------------------------------
    //  is {axisX, axisY, axisZ, radians}, as setFlyerInstance takes it: a
    // dropped item TURNS while it hovers, and a turn about its own axis moves
    // its surface without moving the transform enough to describe it. Optional,
    // so a caller with nothing to say passes nothing.
    void setDropInstance(int slot, int model, const float *m, float tx, float ty, float tz,
                         bool show, const float *spin = nullptr) {
        if (held_.empty() || dropBase_ < 0 || slot < 0 || slot >= kDropInstances) return;
        const size_t idx = size_t(dropBase_ + slot);
        if (idx >= instanceDescs_.size()) return;
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const bool ok = show && m && model >= 0 && model < int(held_.size());

        const size_t mi = size_t(ok ? model : 0);
        // Its half-box in the MODEL'S OWN UNITS -- a held-item mesh is one unit
        // per voxel and the transform carries VOXEL_M, so this is voxels. See
        // place(), which is where the motion vector comes from.
        // A DROP TURNS WHILE IT HOVERS, and a turn about its own axis is not
        // in its transform's translation -- which is all place() differences.
        // Same channel the songbirds use; see V6Instance::flapPad.
        instanceInfos_[idx].flap =
            (ok && spin) ? float3(spin[0], spin[1], spin[2]) : float3(0.0f, 0.0f, 0.0f);
        instanceInfos_[idx].flapPad = (ok && spin) ? spin[3] : 0.0f;
        place(idx, ok ? m : kI, tx, ty, tz, kMaskWorld, ok, 0.5f * float(held_[mi].sx),
              0.5f * float(held_[mi].sy), 0.5f * float(held_[mi].sz));
        instanceDescs_[idx].accelerationStructure = held_[mi].blas.as->getGpuAddress();
        instanceInfos_[idx].triOffset = held_[mi].tri;
        dropsDirty_ = true;
    }

    void flushDropInstances() {
        if (!dropsDirty_ || dropBase_ < 0 || !instanceDescBuf_ || !instanceInfo_) return;
        dropsDirty_ = false;
        const size_t base = size_t(dropBase_);
        const size_t n = std::min(size_t(kDropInstances), instanceDescs_.size() - base);
        if (n == 0) return;
        ctx_->updateBuffer(instanceDescBuf_.get(), &instanceDescs_[base],
                           base * sizeof(RtInstanceDesc), n * sizeof(RtInstanceDesc));
        ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[base], base * sizeof(V6Instance),
                           n * sizeof(V6Instance));
    }
    int flyerModelCount() const { return int(flyers_.size()); }
    // How many slots of the flyer band the structure actually reserved. Not the
    // constant: a write past the end is dropped silently by setFlyerInstance,
    // so the two being equal is worth being able to check rather than assume.
    int flyerBandSlots() const {
        if (flyerBase_ < 0 || size_t(flyerBase_) >= instanceDescs_.size()) return 0;
        return int(std::min(size_t(kFlyerInstances), instanceDescs_.size() - size_t(flyerBase_)));
    }

    // -----------------------------------------------------------------------
    // Where one butterfly is this frame.
    //
    // `m` is the 3x3 row-major, the fade scale folded in; `tx/ty/tz` the
    // translation; `px/py/pz` how far it moved since the last frame, which the
    // motion vector needs and nothing else reads (see KIND_FLYER); `flap` how
    // far its wings rose inside that -- inner, tips, and the model's own centre
    // to measure "how far out along the wing" from (see V6Instance::flap).
    //
    // NOTHING GOES UP THE BUS HERE. Unlike the tool and the shafts, which are
    // one instance each and upload themselves, a flock is up to sixty-four
    // instances that ALL move on every frame -- and sixty-four ten-byte-ish
    // writes into a mapped buffer is a hundred and twenty-eight driver calls a
    // frame to move six kilobytes. So these write the host's own copy and
    // flushFlyerInstances sends the whole band in one go, twice.
    // -----------------------------------------------------------------------
    // `spin`, when given, is {axisX, axisY, axisZ, radians}: the instance turned
    // in place about that world axis by that angle since the last frame, which
    // no transform of it describes. See V6Instance::flapPad. It replaces the
    // `flap` reading rather than adding to it -- nothing has both.
    void setFlyerInstance(int slot, int model, const float *m, float tx, float ty, float tz,
                          const float *flap, bool show, const float *spin = nullptr) {
        if (flyers_.empty() || flyerBase_ < 0 || slot < 0 || slot >= kFlyerInstances) return;
        const size_t idx = size_t(flyerBase_ + slot);
        if (idx >= instanceDescs_.size()) return;
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const bool ok = show && m && model >= 0 && model < int(flyers_.size());

        const size_t mi = size_t(ok ? model : 0);
        // METRES here, not voxels: a flyer is meshed at VOXEL_M so its object
        // space already is metres and its transform is a turn and a fade. See
        // addFlyerModel for why, and place() for what the half-box is for.
        place(idx, ok ? m : kI, tx, ty, tz, kMaskWorld, ok,
              0.5f * float(flyers_[mi].sx) * VOXEL_M, 0.5f * float(flyers_[mi].sy) * VOXEL_M,
              0.5f * float(flyers_[mi].sz) * VOXEL_M);
        instanceDescs_[idx].accelerationStructure = flyers_[mi].blas.as->getGpuAddress();
        instanceInfos_[idx].triOffset = flyers_[mi].tri;
        instanceInfos_[idx].flap =
            (ok && spin)   ? float3(spin[0], spin[1], spin[2])
            : (ok && flap) ? float3(flap[0], flap[1], flap[2])
                           : float3(0.0f, 0.0f, 0.0f);
        instanceInfos_[idx].flapPad = (ok && spin) ? spin[3] : 0.0f;
        flyersDirty_ = true;
    }

    // The loose band, in two writes, once a frame and before the refit.
    void flushDebrisInstances() {
        if (!debrisDirty_ || debrisBase_ < 0 || !instanceDescBuf_ || !instanceInfo_) return;
        debrisDirty_ = false;
        const size_t base = size_t(debrisBase_);
        if (base >= instanceDescs_.size()) return;
        const size_t n = std::min(size_t(kDebrisInstances), instanceDescs_.size() - base);
        if (n == 0) return;
        ctx_->updateBuffer(instanceDescBuf_.get(), &instanceDescs_[base],
                           base * sizeof(RtInstanceDesc), n * sizeof(RtInstanceDesc));
        ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[base], base * sizeof(V6Instance),
                           n * sizeof(V6Instance));
    }

    // One loose body's instance: the solver's pose, as a transform.
    void setDebrisInstance(int slot, const Vec3 &p, const float *q) {
        if (debrisBase_ < 0 || slot < 0 || slot >= kDebrisInstances) return;
        const size_t idx = size_t(debrisBase_ + slot);
        if (idx >= instanceDescs_.size()) return;
        const Debris &d = debris_[slot];
        const bool own = d.blas.valid();
        if (!d.live || (!own && !d.borrowAs)) {
            instanceDescs_[idx].instanceMask = 0;
            debrisDirty_ = true;
            return;
        }
        // THE SOLVER'S OWN ROTATION, as a matrix. The piece tumbles freely now
        // -- three axes, not the single tilt the hand-written version had --
        // so there is no yaw to compose and nothing to keep in step: whatever
        // PhysX says the body is doing is what gets drawn.
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float m[9] = {
            1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w),        2.0f * (x * z + y * w),
            2.0f * (x * y + z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w),
            2.0f * (x * z - y * w),        2.0f * (y * z + x * w),        1.0f - 2.0f * (x * x + y * y),
        };
        const float ox = d.originOff.x, oy = d.originOff.y, oz = d.originOff.z;
        const float tx = p.x + (m[0] * ox + m[1] * oy + m[2] * oz);
        const float ty = p.y + (m[3] * ox + m[4] * oy + m[5] * oz);
        const float tz = p.z + (m[6] * ox + m[7] * oy + m[8] * oz);
        place(idx, m, tx, ty, tz, kMaskWorld, true, d.halfM[0], d.halfM[1], d.halfM[2]);
        instanceDescs_[idx].accelerationStructure = own ? d.blas.as->getGpuAddress() : d.borrowAs;
        instanceInfos_[idx].triOffset = own ? d.triOffset : d.borrowTri;
        // A FELLED TREE IS STILL A TREE. KIND_LOOSE tells the device the shades
        // were resolved once and must not be re-rolled per world cell, which is
        // right for a chip cut out of a rock and wrong for a tree that was
        // meshed as scenery -- it would flatten the bark.
        instanceInfos_[idx].kind = d.felled ? KIND_TREE : KIND_LOOSE;
        // ...AND ITS OWN HUE. Every tree carries a few percent of its own,
        // which is what stops nine models over thousands of trees reading as a
        // repeat -- so a felled one has to keep the tint it was standing with,
        // or it changes colour as it goes over.
        instanceInfos_[idx].tint = d.tint;
        debrisDirty_ = true;
    }

    void retireDebris(Physics &ph, int slot) {
        Debris &d = debris_[slot];
        d.borrowAs = 0;
        d.borrowTri = TriPool::kInvalid;
        d.tint = float3(1.0f, 1.0f, 1.0f);
        d.felled = false;
        // Structure and triangles both go back, once the device is done with
        // them -- a chunk that vanished this frame was still being drawn last
        // frame. See retireLoose.
        retireLoose(std::move(d.blas), d.triOffset, d.tris);
        d.triOffset = TriPool::kInvalid;
        d.tris = 0;
        if (d.phys >= 0) ph.releaseBody(d.phys);
        d.phys = -1;
        // ...and the stone that was standing by for it.
        if (d.window >= 0) ph.removeStatic(d.window);
        d.window = -1;
        d.live = false;
        d.absorbing = false;
        if (debrisBase_ >= 0 && size_t(debrisBase_ + slot) < instanceDescs_.size())
            instanceDescs_[size_t(debrisBase_ + slot)].instanceMask = 0;
        debrisDirty_ = true;
    }

    // The whole band, in two writes. Called once a frame, before refitTlas.
    void flushFlyerInstances() {
        if (!flyersDirty_ || flyerBase_ < 0 || !instanceDescBuf_ || !instanceInfo_) return;
        flyersDirty_ = false;
        const size_t base = size_t(flyerBase_);
        const size_t n = std::min(size_t(kFlyerInstances), instanceDescs_.size() - base);
        if (n == 0) return;
        ctx_->updateBuffer(instanceDescBuf_.get(), &instanceDescs_[base],
                           base * sizeof(RtInstanceDesc), n * sizeof(RtInstanceDesc));
        ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[base], base * sizeof(V6Instance),
                           n * sizeof(V6Instance));
    }

    // -----------------------------------------------------------------------
    // Where the tool is this frame, and a REFIT rather than a rebuild.
    //
    // `m` is the 3x3 in row-major order, scale included; `tx/ty/tz` the
    // translation. `show` false hides it by zeroing the instance mask, which is
    // how a hidden tool costs nothing: the traversal rejects it before it looks
    // at the geometry, and the slot stays in the structure so the refit below
    // still has the same instance count it was built with.
    //
    // A REFIT, AND THAT IS THE WHOLE REASON THIS IS AFFORDABLE. Rebuilding the
    // top-level structure costs 1.15 ms on this scene -- measured, over 31
    // rebuilds at 42 000 instances -- against a 3.28 ms median frame, so doing
    // it every frame to move one 19-voxel model would have been a third of the
    // frame rate. An update reuses the tree and only moves the box that
    // changed, which is exactly the case it is designed for: one instance
    // moving a little, every other one standing still.
    //
    // AND ONLY 64 BYTES GO UP THE BUS. The held item is instance ZERO, always,
    // so its descriptor sits at offset 0 and the other forty-two thousand are
    // left alone. Uploading the whole array every frame would have cost more
    // than the refit.
    // -----------------------------------------------------------------------
    void setHeldInstance(int model, const float *m, float tx, float ty, float tz, bool show) {
        if (held_.empty() || !tlas_ || instanceDescs_.empty()) return;
        if (model < 0 || model >= int(held_.size())) show = false;

        // NO MOTION TRACKED FOR THE TOOL, and that is not an omission: it is
        // bolted to the camera, so its world point moves with the eye while its
        // PIXEL does not move at all. The tracer says so where it writes the
        // guide -- a held primary gets a zero motion vector outright -- and
        // leaving prevOffset at zero here means the two agree instead of one
        // quietly computing a number the other throws away.
        place(0, m, tx, ty, tz, kMaskHeld, show, 0.0f, 0.0f, 0.0f, /*track*/ false);
        RtInstanceDesc &inst = instanceDescs_[0];
        // WHICH MODEL IS IN THE SLOT, AND WHY AN UPDATE MAY CHANGE IT. A
        // top-level update is allowed to rewrite the instance descriptors; what
        // it may not change is how MANY there are. So swapping tools -- and the
        // bow stepping through its draw frames -- is the same 64-byte write as
        // moving the tool, and costs the same refit. The instance's triOffset
        // lives in a separate buffer that the structure never reads, so it is
        // updated alongside rather than through the build.
        if (show && model != heldModel_) {
            heldModel_ = model;
            instanceInfos_[0].triOffset = held_[size_t(model)].tri;
            ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[0], 0, sizeof(V6Instance));
        }
        inst.accelerationStructure =
            held_[size_t(model >= 0 && model < int(held_.size()) ? model : 0)]
                .blas.as->getGpuAddress();
        ctx_->updateBuffer(instanceDescBuf_.get(), &inst, 0, sizeof(RtInstanceDesc));
    }

    // -----------------------------------------------------------------------
    // One shaft, in the slot it was given. `m` null hides it.
    //
    // The arrow slots follow the held one, so slot i is instance 1 + i and its
    // descriptor is at a fixed offset like the tool's. Same 64-byte write, same
    // refit -- see setHeldInstance for why that is what makes any of this
    // affordable.
    //
    // `px/py/pz` is how far the shaft travelled since the last frame, and it is
    // the whole of what stops it ghosting: see the note over the reserve below
    // for what the static-world formula does to something moving this fast, and
    // V6Instance::prevOffset for what the tracer does with this. Zero while it
    // stands in the ground, which is the honest answer there.
    // -----------------------------------------------------------------------
    void setArrowInstance(int slot, int model, const float *m, float tx, float ty, float tz,
                          bool show) {
        if (held_.empty() || !tlas_ || slot < 0 || slot >= kArrowInstances) return;
        const size_t idx = size_t(1 + slot);
        if (idx >= instanceDescs_.size()) return;
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const size_t mi = size_t(model >= 0 && model < int(held_.size()) ? model : 0);

        // Voxels, like the dropped tools above and for the same reason: a shaft
        // is a held-item mesh with VOXEL_M in its transform.
        place(idx, m ? m : kI, tx, ty, tz, kMaskWorld, show && m, 0.5f * float(held_[mi].sx),
              0.5f * float(held_[mi].sy), 0.5f * float(held_[mi].sz));
        RtInstanceDesc &inst = instanceDescs_[idx];
        if (model >= 0 && model < int(held_.size()))
            inst.accelerationStructure = held_[mi].blas.as->getGpuAddress();

        // THE INFO RECORD GOES UP EVERY FRAME THE SHAFT IS IN THE AIR, not only
        // when the model changes. It used to be written on a triOffset change
        // alone, which was true while the record held nothing that moved -- a
        // shaft keeps one model for its whole flight, so that test was false on
        // every frame of it and the motion vector place() just worked out would
        // never have reached the GPU.
        instanceInfos_[idx].triOffset = held_[mi].tri;
        ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[idx], idx * sizeof(V6Instance),
                           sizeof(V6Instance));
        ctx_->updateBuffer(instanceDescBuf_.get(), &inst, idx * sizeof(RtInstanceDesc),
                           sizeof(RtInstanceDesc));
    }

    // -----------------------------------------------------------------------
    // Fold this frame's writes into the structure.
    //
    // ONE UPDATE FOR ALL OF THEM. The tool and every shaft are written first
    // and the tree is walked once afterwards; refitting per instance would pay
    // the traversal thirteen times for the same frame.
    // -----------------------------------------------------------------------
    void refitTlas() {
        // EITHER MOVER IS ENOUGH. This used to ask only whether a tool was
        // loaded, which was the whole truth while the tool was the only thing
        // in this world that moved. `--noaxe` with a flock in the air is now a
        // world with nothing in the hand and sixty-four instances that move
        // every frame, and refusing the refit there would have left every
        // butterfly frozen at the origin.
        if ((held_.empty() && flyers_.empty()) || !tlas_ || instanceDescs_.empty() ||
            !tlasUpdateScratch_)
            return;

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace |
                       RtAccelerationStructureBuildFlags::AllowUpdate |
                       RtAccelerationStructureBuildFlags::PerformUpdate;
        inputs.descCount = uint32_t(instanceDescs_.size());
        inputs.instanceDescs = instanceDescBuf_->getGpuAddress();

        RtAccelerationStructure::BuildDesc bd = {};
        bd.inputs = inputs;
        // IN PLACE: source and destination are the same structure, which the
        // API allows for an update and which is what makes this cost one pass
        // over the tree rather than a copy of it.
        bd.source = tlas_.get();
        bd.dest = tlas_.get();
        bd.scratchData = tlasUpdateScratch_->getGpuAddress();
        ctx_->buildAccelerationStructure(bd, 0, nullptr);
        ctx_->uavBarrier(tlasBuffer_.get());
    }

    // -----------------------------------------------------------------------
    // The palette, as the shader's material table.
    //
    // A SEPARATE FUNCTION BECAUSE THE TABLE CAN GAIN ENTRIES AFTER init(). Every
    // model registers its colours through Palette::forModelColor as it loads,
    // and until the axe there was nothing that loaded outside this class -- so
    // the upload sat inline at the end of init() and the table was final the
    // moment it ran. render/helditem.h registers the held model's colours the
    // same way and then asks for this, which is the whole of what it needs from
    // the world: the tool indexes the same table as every rock, so it is shaded
    // by the same materials rather than by a set of its own.
    //
    // The palette is a fixed 255 entries whose fields line up one for one with
    // the shader's material record, but they are separate structs on purpose:
    // MaterialLook belongs to the world builder and V6Material to the pipeline,
    // and the day one of them gains a field the other does not need, this loop
    // is the only thing that has to know.
    void uploadMaterials() {
        std::vector<V6Material> mats(palette.table().size());
        for (size_t i = 0; i < mats.size(); ++i) {
            const MaterialLook &m = palette.table()[i];
            mats[i].albedo = float3(m.albedo.x, m.albedo.y, m.albedo.z);
            mats[i].roughness = m.roughness;
            mats[i].specular = m.specular;
            mats[i].translucency = m.translucency;
            mats[i].pad0 = mats[i].pad1 = 0.0f;
        }
        materials_ = device_->createStructuredBuffer(sizeof(V6Material), uint32_t(mats.size()),
                                                     ResourceBindFlags::ShaderResource,
                                                     Falcor::MemoryType::DeviceLocal, mats.data());
        materials_->setName("v2::materials");
    }

    size_t chunkCount() const { return chunks_.size(); }

    // WHERE ONE LOOSE PIECE IS AND WHAT IT IS DOING. For the headless fell
    // test, which is the only way this engine can be watched without opening a
    // window over whatever the user is doing -- see App::runFellTest.
    bool debrisPose(int slot, Vec3 *p, float *quat) const {
        if (slot < 0 || slot >= kDebrisInstances || !debris_[slot].live) return false;
        if (p) *p = debris_[slot].pos;
        if (quat)
            for (int k = 0; k < 4; ++k) quat[k] = debris_[slot].quat[k];
        return true;
    }

    void debrisVel(Physics &ph, int slot, Vec3 *lin, Vec3 *ang) const {
        if (slot < 0 || slot >= kDebrisInstances || !debris_[slot].live) return;
        ph.velocityOf(debris_[slot].phys, lin, ang);
    }

    int looseCount() const {
        int k = 0;
        for (const Debris &d : debris_)
            if (d.live) ++k;
        return k;
    }

    // -----------------------------------------------------------------------
    // SET A PIECE OF THE WORLD LOOSE.
    //
    // A BALL OF ONE MATERIAL, not a copy of the voxels that were removed. What
    // a swing takes out is a sphere three voxels across of whatever it hit, and
    // rebuilding the exact removed set would mean carrying a second volume
    // through the carve for a difference nobody can see on something that is
    // tumbling and gone in a second. The shade still varies per voxel -- that
    // happens on the device, in groundShade, from the voxel coordinate.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // ONE BODY PER BITE, AND ONLY ONE.
    //
    // v1 splits a bite into 6-connected pieces so an axe through a canopy
    // throws one chunk per branch. A pick taking a single bite out of solid
    // stone is not that case: the rim of the sphere clips a stray voxel across
    // a gap and a second little body flies out beside the real one. The
    // splitting rule belongs with felling, where there are genuinely two
    // branches to separate.
    //
    // A LONE VOXEL IS STILL NOT A CHUNK -- v1's rule, and that one does apply:
    // single specks tumbling off every swing read as litter, not debris.
    // -----------------------------------------------------------------------
    int spawnDebris(Physics &ph, const std::vector<uint8_t> &vol, int n, const Vec3 &centre,
                    const Vec3 &vel, const Vec3 &spin, double nowMs, float yawRad = 0.0f,
                    const Solid *src = nullptr) {
        // `src` is no longer read. buildWindow asks the world for everything
        // solid near the bite, which of course includes the rock it came out
        // of -- and also the one beside it, which naming a single source could
        // never have covered. Kept on the signature because the caller has it
        // and a tree being felled will want it.
        (void)src;
        if (n < 1 || vol.size() != size_t(n) * size_t(n) * size_t(n)) return -1;
        int count = 0;
        for (uint8_t v : vol)
            if (v != mat::AIR) ++count;
        if (count < 2) return -1;
        return spawnPiece(ph, vol, n, count, centre, vel, spin, nowMs, yawRad);
    }

    // One connected piece, as a body. See spawnDebris for the split above.
    int spawnPiece(Physics &ph, const std::vector<uint8_t> &vol, int n, int count,
                   const Vec3 &centre, const Vec3 &vel, const Vec3 &spin, double nowMs,
                   float yawRad) {
        if (count <= 0) return -1;
        int slot = -1;
        for (int i = 0; i < kDebrisInstances; ++i)
            if (!debris_[i].live) { slot = i; break; }
        if (slot < 0) return -1;   // the world is already as busy as it is allowed to be

        // -------------------------------------------------------------------
        // THE TIGHT BOX AND THE CENTRE OF MASS, which is how v1 builds a body
        // (sim/physics.js, phBuildBody0: com, the folded-in bbox, rMax).
        //
        // The carve hands over a (2r+1) cube with the piece somewhere inside
        // it, and most of that cube is air. Using the CUBE's middle as the
        // body's centre spins the piece about a point that is not its own --
        // it orbits instead of tumbling -- and using the cube's extent as the
        // collider makes a 30 cm chip collide like a 70 cm one.
        //
        // So the volume is trimmed to what is actually in it, the body is
        // placed at the voxels' centre of mass, and the mesh is offset back to
        // meet it.
        // -------------------------------------------------------------------
        int x0 = n, x1 = -1, y0 = n, y1 = -1, z0 = n, z1 = -1;
        double sxm = 0.0, sym = 0.0, szm = 0.0;
        for (int y = 0; y < n; ++y)
            for (int z = 0; z < n; ++z)
                for (int x = 0; x < n; ++x) {
                    if (vol[size_t(x) + size_t(z) * size_t(n) + size_t(y) * size_t(n) * size_t(n)]
                        == mat::AIR)
                        continue;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                    if (z < z0) z0 = z;
                    if (z > z1) z1 = z;
                    sxm += double(x) + 0.5;
                    sym += double(y) + 0.5;
                    szm += double(z) + 0.5;
                }
        if (x1 < 0) return -1;
        const int tx = x1 - x0 + 1, ty = y1 - y0 + 1, tz = z1 - z0 + 1;
        std::vector<uint8_t> trimmed(size_t(tx) * size_t(ty) * size_t(tz), mat::AIR);
        for (int y = 0; y < ty; ++y)
            for (int z = 0; z < tz; ++z)
                for (int x = 0; x < tx; ++x)
                    trimmed[size_t(x) + size_t(z) * size_t(tx) +
                            size_t(y) * size_t(tx) * size_t(tz)] =
                        vol[size_t(x + x0) + size_t(z + z0) * size_t(n) +
                            size_t(y + y0) * size_t(n) * size_t(n)];

        const VoxMesh mesh = meshVolume(trimmed, tx, ty, tz, VOXEL_M, /*resolveShades=*/true);
        if (mesh.triCount() == 0) return -1;
        Blas b = recordLooseBuild(mesh);   // NOT buildBlas: see the note there
        if (!b.valid()) return -1;

        // The centre of mass, in the cube's voxels and then in metres from the
        // cube's own middle -- which is where the caller said the bite was.
        const double cmx = sxm / double(count), cmy = sym / double(count),
                     cmz = szm / double(count);
        const float half = 0.5f * float(n);
        const Vec3 comOff{float(cmx - double(half)) * VOXEL_M,
                          float(cmy - double(half)) * VOXEL_M,
                          float(cmz - double(half)) * VOXEL_M};

        Debris &d = debris_[slot];
        d.blas = std::move(b);
        d.triOffset = pool_.upload(ctx_, mesh.tri);
        d.tris = mesh.tri.size();
        d.voxels = count;
        d.halfM[0] = 0.5f * float(tx) * VOXEL_M;
        d.halfM[1] = 0.5f * float(ty) * VOXEL_M;
        d.halfM[2] = 0.5f * float(tz) * VOXEL_M;
        // The trimmed mesh's corner, relative to the centre of mass.
        d.originOff = Vec3{float(double(x0) - cmx) * VOXEL_M, float(double(y0) - cmy) * VOXEL_M,
                           float(double(z0) - cmz) * VOXEL_M};
        // ...and the body itself stands at the centre of mass, not at the
        // middle of the cube the carve happened to hand over.
        // THE OFFSET IS IN THE MODEL'S AXES, NOT THE WORLD'S.
        //
        // A boulder is placed with a quarter turn, and carveModel records the
        // spoil by walking THAT MODEL'S voxel grid. So the step from the bite's
        // centre to the piece's centre of mass means nothing in the world until
        // it is turned the same way -- and the body has to be born wearing that
        // turn too, or the piece is the right voxels in the wrong orientation,
        // which is what makes it read as a different chunk from the hole.
        const float cyaw = std::cos(yawRad), syaw = std::sin(yawRad);
        const Vec3 com{centre.x + comOff.x * cyaw + comOff.z * syaw, centre.y + comOff.y,
                       centre.z - comOff.x * syaw + comOff.z * cyaw};
        d.bornMs = nowMs;
        d.absorbing = false;
        d.live = true;
        // ---- IT BECOMES A RIGID BODY -------------------------------------
        //
        // A convex hull of this piece's own voxel corners, at granite's
        // density, born exactly where its voxels were and wearing the model's
        // own quarter turn so it lines up with the hole. No velocity, no spin,
        // no impulse: gravity and contacts are the only things that touch it
        // from here until the player collects it.
        //
        // The cloud is in the MODEL'S axes relative to the centre of mass,
        // which is the same frame the drawn mesh is in -- so the shape the
        // solver holds and the shape you can see are the same shape, and the
        // body turns about its middle rather than orbiting a corner.
        std::vector<Vec3> cloud;
        cloud.reserve(size_t(count) * 8);
        {
            const int stride = 1 + count / 512;   // a big chip need not send every corner
            int seen = 0;
            for (int y = 0; y < ty; ++y)
                for (int z = 0; z < tz; ++z)
                    for (int x = 0; x < tx; ++x) {
                        if (trimmed[size_t(x) + size_t(z) * size_t(tx) +
                                    size_t(y) * size_t(tx) * size_t(tz)] == mat::AIR)
                            continue;
                        if ((seen++ % stride) != 0) continue;
                        for (int c = 0; c < 8; ++c) {
                            const float px = float(x0 + x + ((c & 1) ? 1 : 0)) - float(cmx);
                            const float py = float(y0 + y + ((c & 2) ? 1 : 0)) - float(cmy);
                            const float pz = float(z0 + z + ((c & 4) ? 1 : 0)) - float(cmz);
                            // A HAIR SMALLER THAN ITS VOXELS. A bite is a
                            // digital sphere and its convex hull cuts the
                            // corners off that staircase -- so parts of the
                            // hull lie OUTSIDE the voxels that were removed,
                            // which is to say inside the stone that was not.
                            // Born like that the piece is penetrating on frame
                            // one and gets pushed, which is the pop this whole
                            // exercise is about not having. Five per cent is
                            // two centimetres on a chip and it is invisible.
                            cloud.push_back(Vec3{px * VOXEL_M * kHullShrink,
                                                 py * VOXEL_M * kHullShrink,
                                                 pz * VOXEL_M * kHullShrink});
                        }
                    }
        }
        (void)vel;
        (void)spin;
        d.phys = ph.addChunkBody(cloud.data(), int(cloud.size()), com, yawRad, kStoneDensity);
        {
            // A golden-angle turn per slot, so neighbours lean differently.
            const float wa = float(slot) * 2.39996323f;
            d.wobbleAxis = Vec3{std::cos(wa), 0.35f, std::sin(wa)};
            const float wl = sqrtf(d.wobbleAxis.x * d.wobbleAxis.x + 0.1225f +
                                   d.wobbleAxis.z * d.wobbleAxis.z);
            d.wobbleAxis = Vec3{d.wobbleAxis.x / wl, d.wobbleAxis.y / wl, d.wobbleAxis.z / wl};
        }
        d.pos = com;
        d.quat[0] = 0.0f;
        d.quat[1] = std::sin(yawRad * 0.5f);
        d.quat[2] = 0.0f;
        d.quat[3] = std::cos(yawRad * 0.5f);
        // ...AND THE STONE AROUND IT BECOMES SOMETHING TO FALL AGAINST.
        d.window = buildWindow(ph, com);
        d.winCentre = com;
        debrisDirty_ = true;
        return slot;
    }

    // -----------------------------------------------------------------------
    // THE STONE NEAR A POINT, AS A STATIC BODY.
    //
    // Everything nearby that has voxels -- the boulder the piece came out of
    // and any of its neighbours -- sampled over a world-aligned grid and merged
    // into boxes. World-aligned works because placements are snapped to the
    // voxel grid and turned only by quarter turns, so one world grid lines up
    // with every model's own.
    //
    // THE DAMAGED ARRAY IS WHAT IS SAMPLED, through Solid::vol, so the bite is
    // absent from the collider the way it is absent from the rock. That one
    // sentence is the whole difference from every previous attempt at this.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // A MODEL AS BOXES, IN ITS OWN FRAME.
    //
    // The same greedy merge the falling side uses on itself, so a trunk and the
    // rock it lands on are described to the solver at one resolution and in one
    // language. Trees need a fill fraction and rocks do not -- see
    // kStaticFillFrac.
    // -----------------------------------------------------------------------
    static void modelBoxes(const std::vector<uint8_t> &vol, int sx, int sy, int sz,
                           const ModelCollider *trunk, std::vector<VoxBox> *out) {
        out->clear();
        if (vol.empty() || sx <= 0 || sy <= 0 || sz <= 0) return;
        const int q = maxi(1, int(kStaticCellM / VOXEL_M + 0.5f));
        const int nx = (sx + q - 1) / q, ny = (sy + q - 1) / q, nz = (sz + q - 1) / q;
        const int need = trunk ? maxi(1, int(float(q * q * q) * kStaticFillFrac)) : 1;
        // A STANDING TREE IS ITS TRUNK, exactly as a falling one is -- see
        // kFellFillFrac, which is the same argument from the other side. The
        // first version of this gave the canopy to the solver on a fill
        // fraction alone and MEASURED two felled pines in three hanging up at
        // 26 and 30 degrees, caught on a neighbour's needles. What stops a
        // falling log is another trunk.
        float bcx = 0.0f, bcz = 0.0f, bhx = 0.0f, bhz = 0.0f;
        if (trunk) {
            bcx = float(sx) * VOXEL_M * 0.5f + trunk->baseCX;
            bcz = float(sz) * VOXEL_M * 0.5f + trunk->baseCZ;
            bhx = maxf(kTrunkPadM, float(trunk->baseX) * VOXEL_M * 0.5f + kTrunkPadM);
            bhz = maxf(kTrunkPadM, float(trunk->baseZ) * VOXEL_M * 0.5f + kTrunkPadM);
        }
        greedyBoxes(
            nx, ny, nz, Vec3{0.0f, 0.0f, 0.0f}, kStaticCellM,
            [&](int i, int j, int k) {
                if (trunk) {
                    const float mx = (float(i) + 0.5f) * kStaticCellM;
                    const float mz = (float(k) + 0.5f) * kStaticCellM;
                    if (fabsf(mx - bcx) > bhx || fabsf(mz - bcz) > bhz) return false;
                }
                int n = 0;
                for (int b = 0; b < q; ++b)
                    for (int c = 0; c < q; ++c)
                        for (int e = 0; e < q; ++e) {
                            const int x = i * q + e, y = j * q + b, z = k * q + c;
                            if (x >= sx || y >= sy || z >= sz) continue;
                            if (vol[size_t(x) + size_t(z) * size_t(sx) +
                                    size_t(y) * size_t(sx) * size_t(sz)] != mat::AIR)
                                if (++n >= need) return true;
                        }
                return false;
            },
            out, kStaticMaxBoxes);
    }

    // ...and this instance\'s copy of them, which is the DAMAGED array when
    // there is one. A rock somebody has been digging into must not put the
    // stone back for the tree coming down beside it.
    const std::vector<VoxBox> &boxesFor(const Solid &s) {
        static const std::vector<VoxBox> none;
        if (s.modelKind < 0) return none;
        const ModelTemplate &t = templateFor(s.modelKind, s.modelIndex);
        if (t.volume.empty()) return none;
        const auto dit = damaged_.find({s.ownerChunk, int(s.decorSlot)});
        if (dit != damaged_.end()) {
            Damaged &dm = dit->second;
            if (dm.boxSeq != dm.seq) {
                modelBoxes(dm.vol, t.sx, t.sy, t.sz, s.modelKind == 0 ? &t.col : nullptr,
                           &dm.boxes);
                dm.boxSeq = dm.seq;
            }
            return dm.boxes;
        }
        if (!t.staticBuilt) {
            modelBoxes(t.volume, t.sx, t.sy, t.sz, s.modelKind == 0 ? &t.col : nullptr,
                       &t.staticBoxes);
            t.staticBuilt = true;
        }
        return t.staticBoxes;
    }

    // -----------------------------------------------------------------------
    // EVERYTHING SOLID INSIDE A BOX, AS ONE STATIC ACTOR.
    //
    // For a body big enough that a 3.2 m window is a joke. The models around it
    // are placed whole rather than resampled onto a world grid: their boxes are
    // already merged, a quarter turn keeps them axis aligned, and the cost of
    // an instance is a copy rather than a hundred thousand voxel lookups.
    // -----------------------------------------------------------------------
    int buildSolidWindow(Physics &ph, const Vec3 &lo, const Vec3 &hi) {
        if (!ph.available()) return -1;
        const Vec3 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        const float reach = 0.5f * maxf(hi.x - lo.x, hi.z - lo.z) + 24.0f;
        collidersNear(c, reach, &winSolids_);
        winBoxes_.clear();
        for (const Solid &sl : winSolids_) {
            if (!sl.vol || sl.hx <= 0.0f) continue;
            if (sl.baseY > hi.y || sl.top < lo.y) continue;
            const std::vector<VoxBox> &mb = boxesFor(sl);
            const bool swap = (sl.yaw & 1) != 0;
            for (const VoxBox &b : mb) {
                VoxBox w;
                solidWorldSpace(sl, b.cx, b.cz, &w.cx, &w.cz);
                w.cy = sl.baseY + b.cy;
                w.hx = swap ? b.hz : b.hx;
                w.hz = swap ? b.hx : b.hz;
                w.hy = b.hy;
                if (w.cx + w.hx < lo.x || w.cx - w.hx > hi.x) continue;
                if (w.cy + w.hy < lo.y || w.cy - w.hy > hi.y) continue;
                if (w.cz + w.hz < lo.z || w.cz - w.hz > hi.z) continue;
                winBoxes_.push_back(w);
                if (winBoxes_.size() >= kStaticMaxBoxes) break;
            }
            if (winBoxes_.size() >= kStaticMaxBoxes) break;
        }
        if (winBoxes_.empty()) return -1;
        return ph.addStaticBoxes(winBoxes_.data(), int(winBoxes_.size()));
    }

    // -----------------------------------------------------------------------
    // HOW MUCH OF A FALLEN BODY IS INSIDE SOMETHING IT SHOULD NOT BE.
    //
    // "I saw a tree clip into a rock when it felled." This is that question,
    // asked of the shape the solver was given: every box of the body's own
    // collider, put where the body is now, and its centre asked of the voxels
    // of every model nearby. A number greater than zero is a trunk inside a
    // boulder. Diagnostic only -- --fell-test prints it.
    // -----------------------------------------------------------------------
    int debrisClip(int slot) {
        if (slot < 0 || slot >= kDebrisInstances) return 0;
        Debris &d = debris_[slot];
        if (!d.live || d.boxes.empty()) return 0;
        collidersNear(d.pos, 60.0f, &underSolids_);
        // The body's rotation, as a matrix, from its quaternion.
        const float x = d.quat[0], y = d.quat[1], z = d.quat[2], w = d.quat[3];
        const float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
        int inside = 0;
        for (const VoxBox &b : d.boxes) {
            const float wx = m[0] * b.cx + m[1] * b.cy + m[2] * b.cz + d.pos.x;
            const float wy = m[3] * b.cx + m[4] * b.cy + m[5] * b.cz + d.pos.y;
            const float wz = m[6] * b.cx + m[7] * b.cy + m[8] * b.cz + d.pos.z;
            for (const Solid &sl : underSolids_) {
                if (!sl.vol) continue;
                if (solidAtWorld(sl, wx, wy, wz, VOXEL_M)) {
                    ++inside;
                    break;
                }
            }
        }
        return inside;
    }

    // -----------------------------------------------------------------------
    // HOW MANY OF THIS INSTANCE'S VOXELS ARE STANDING ON NOTHING -- per voxel,
    // and as a DELTA against how the model was drawn.
    //
    // Diagnostic, and slow on a big rock by design: it floods the whole model
    // rather than a box, which is what makes it an independent check on
    // dropModelHangers rather than a copy of it. Only --float-test calls it.
    //
    // The delta matters. BIG_1_BiG_0 is TWO six-connected pieces as authored --
    // a base plate, a one-voxel gap, and the boulder on top -- so 95% of an
    // untouched one is "loose" to a per-voxel rule. What a blow DID is the
    // difference between now and then.
    // -----------------------------------------------------------------------
    long looseVoxelsNow(const Solid &s) {
        if (s.modelKind < 0 || !s.vol) return 0;
        const ModelTemplate &t = templateFor(s.modelKind, s.modelIndex);
        if (t.volume.empty()) return 0;
        const auto dit = damaged_.find({s.ownerChunk, int(s.decorSlot)});
        const std::vector<uint8_t> &vol = (dit == damaged_.end()) ? t.volume : dit->second.vol;
        if (t.bornVoxLoose < 0) t.bornVoxLoose = long(fineLoose(t.volume, t.sx, t.sy, t.sz));
        const long now = long(fineLoose(vol, t.sx, t.sy, t.sz));
        return now - t.bornVoxLoose;
    }

    static size_t fineLoose(const std::vector<uint8_t> &vol, int sx, int sy, int sz) {
        if (vol.empty()) return 0;
        auto at = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(sx) + size_t(y) * size_t(sx) * size_t(sz);
        };
        size_t solid = 0;
        for (uint8_t v : vol)
            if (v != mat::AIR) ++solid;
        if (!solid) return 0;
        std::vector<uint8_t> seen(vol.size(), 0);
        std::vector<int> st;
        for (int z = 0; z < sz; ++z)
            for (int x = 0; x < sx; ++x) {
                const size_t i = at(x, 0, z);
                if (vol[i] == mat::AIR || seen[i]) continue;
                seen[i] = 1;
                st.push_back(int(i));
            }
        size_t reached = st.size();
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        while (!st.empty()) {
            const int p = st.back();
            st.pop_back();
            const int x = p % sx, z = (p / sx) % sz, y = p / (sx * sz);
            for (const int *o : off) {
                const int a = x + o[0], b = y + o[1], c = z + o[2];
                if (a < 0 || b < 0 || c < 0 || a >= sx || b >= sy || c >= sz) continue;
                const size_t q = at(a, b, c);
                if (vol[q] == mat::AIR || seen[q]) continue;
                seen[q] = 1;
                ++reached;
                st.push_back(int(q));
            }
        }
        return solid - reached;
    }

    // How many boxes the static window round a body is holding -- diagnostic.
    int debrisWindowBoxes() const { return int(winBoxes_.size()); }

    int buildWindow(Physics &ph, const Vec3 &centre) {
        if (!ph.available()) return -1;
        const float span = 0.5f * float(kWindowCells) * VOXEL_M;
        const Vec3 o{std::floor((centre.x - span) / VOXEL_M) * VOXEL_M,
                     std::floor((centre.y - span) / VOXEL_M) * VOXEL_M,
                     std::floor((centre.z - span) / VOXEL_M) * VOXEL_M};
        // GENEROUSLY, AND THEN STRICTLY. A boulder twenty metres across can
        // have its centre well outside a three-metre window and still fill it,
        // so the search is wide -- and then every model is tested against the
        // window's actual box, because the sampling lambda below asks each
        // survivor 32,768 times. Without this filter a clearing with forty bits
        // of scenery in it costs a million voxel lookups per blow.
        collidersNear(centre, 40.0f, &winSolids_);
        const float wx1 = o.x + span * 2.0f, wy1 = o.y + span * 2.0f, wz1 = o.z + span * 2.0f;
        winNear_.clear();
        for (const Solid &sl : winSolids_) {
            if (!sl.vol || sl.hx <= 0.0f) continue;
            if (sl.baseY > wy1 || sl.top < o.y) continue;
            // The model's own box, put into the world. A quarter turn takes a
            // box to a box, so its two opposite corners are the whole answer.
            float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
            solidWorldSpace(sl, 0.0f, 0.0f, &ax, &az);
            solidWorldSpace(sl, float(sl.msx) * VOXEL_M, float(sl.msz) * VOXEL_M, &bx, &bz);
            const float mx0 = minf(ax, bx), mx1 = maxf(ax, bx);
            const float mz0 = minf(az, bz), mz1 = maxf(az, bz);
            if (mx1 < o.x || mx0 > wx1 || mz1 < o.z || mz0 > wz1) continue;
            winNear_.push_back(&sl);
        }
        if (winNear_.empty()) return -1;

        const bool fit = greedyBoxes(
            kWindowCells, kWindowCells, kWindowCells, o, VOXEL_M,
            [&](int i, int j, int k) {
                const float x = o.x + (float(i) + 0.5f) * VOXEL_M;
                const float y = o.y + (float(j) + 0.5f) * VOXEL_M;
                const float z = o.z + (float(k) + 0.5f) * VOXEL_M;
                for (const Solid *sl : winNear_)
                    if (solidAtWorld(*sl, x, y, z, VOXEL_M)) return true;
                return false;
            },
            &winBoxes_);
        (void)fit;   // over budget is still a usable collider, just a partial one
        if (winBoxes_.empty()) return -1;
        return ph.addStaticBoxes(winBoxes_.data(), int(winBoxes_.size()));
    }

    // -----------------------------------------------------------------------
    // EVERY LOOSE PIECE, ONCE A FRAME -- WHICH IS NOW A READ-BACK.
    //
    // NOTHING IS INTEGRATED HERE ANY MORE. The piece is a PxRigidDynamic with
    // granite's mass and inertia, falling against the stone around it, and this
    // asks the solver where it ended up and hands that to the renderer. The
    // five hand-written versions of this that came before -- each with its own
    // gravity, its own footprint test, its own idea of a lip -- are gone.
    //
    // WHAT MADE THAT POSSIBLE was giving the solver a shape with the hole in
    // it. See buildWindow: a few metres of the DAMAGED model's own voxels,
    // merged into boxes. Every earlier attempt handed PhysX a convex hull of
    // the whole boulder, a hull cannot have a dent, and so the piece was always
    // born buried in solid stone and always ground its way down through it.
    //
    // THE ONE FORCE LEFT is the player pulling it in. That is not a fall and
    // the solver has nothing useful to say about it, so at that moment the body
    // goes kinematic and is driven along a curve by hand -- and the drag of the
    // curve is the last thing in this function that is not gravity or contact.
    // -----------------------------------------------------------------------
    void updateDebris(Physics &ph, const Vec3 &eye, double nowMs,
                      const std::function<float(float, float)> &terrainAt) {
        sweepLoose();    // free what the device has finished with
        pumpRemesh();    // ...and take whatever the re-mesher finished

        for (int i = 0; i < kDebrisInstances; ++i) {
            Debris &d = debris_[i];
            if (!d.live) continue;

            if (!d.absorbing) {
                // Where the solver put it.
                ph.poseOf(d.phys, &d.pos, d.quat);

                // ...and a floor under it wherever the ground patch does not
                // reach. A BACKSTOP, not a contact: it only fires when a piece
                // is already well below the surface, which means it has left
                // the part of the world the simulation covers.
                // A FLOOR WHEREVER THE GROUND PATCH DOES NOT REACH -- a
                // backstop, not a contact: it only fires when a piece is
                // already well below the surface, which means it has left the
                // part of the world the simulation covers.
                //
                // NOT FOR A TREE. This clamps the body's ORIGIN by the body's
                // half height, and a tree's half height is thirteen metres --
                // it would hold the thing in the sky. A felled tree's origin is
                // its butt end, which is on the ground where it was cut.
                if (terrainAt && !d.felled) {
                    const float floorY = terrainAt(d.pos.x, d.pos.z) + d.halfM[1];
                    if (d.pos.y < floorY - 0.5f) {
                        ph.clampAbove(d.phys, floorY);
                        ph.poseOf(d.phys, &d.pos, d.quat);
                    }
                }
                // ...AND THE SOLID WORLD FOLLOWS IT DOWN.
                //
                // A window built once at the stump is a window the far end of
                // the tree leaves in the first second of the fall. The bounds
                // are asked every frame -- they are free, PhysX keeps them --
                // and the window is rebuilt only when the body has actually
                // reached the edge of what it covers.
                if (d.felled) {
                    Vec3 bl{0, 0, 0}, bh{0, 0, 0};
                    if (ph.boundsOf(d.phys, &bl, &bh) &&
                        (bl.x < d.winLo.x || bl.y < d.winLo.y || bl.z < d.winLo.z ||
                         bh.x > d.winHi.x || bh.y > d.winHi.y || bh.z > d.winHi.z)) {
                        d.winLo = Vec3{bl.x - kStaticPadM, bl.y - kStaticPadM, bl.z - kStaticPadM};
                        d.winHi = Vec3{bh.x + kStaticPadM, bh.y + kStaticPadM, bh.z + kStaticPadM};
                        const int nw = buildSolidWindow(ph, d.winLo, d.winHi);
                        if (d.window >= 0) ph.removeStatic(d.window);
                        d.window = nw;
                    }
                }

                // ...AND A FELLED TREE GETS A FLOOR TEST TOO, MEASURED RIGHT.
                //
                // Not the one that was here before -- that one is described
                // below and it was the bug. This asks the SHAPES where they
                // are, through their world bounds, instead of asking the
                // actor's origin: a felled tree's origin is the model's
                // corner, which is metres below the wood and, once the tree is
                // down, metres to one side of it as well.
                //
                // It fires only when the whole body is under the ground, which
                // is not a contact the solver has lost -- it is a body that
                // has gone THROUGH the floor, and the only cure is to put it
                // back. MEASURED with --float-test: an undermined pine tumbled
                // to 170 degrees and then fell 40 m in two seconds at 27 m/s.
                if (terrainAt && d.felled) {
                    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
                    if (ph.boundsOf(d.phys, &lo, &hi)) {
                        // The ground under the body's own footprint, at its
                        // corners and middle -- a 26 m trunk lies across a lot
                        // of hillside and the lowest of it is what matters.
                        float g = terrainAt(lo.x, lo.z);
                        g = maxf(g, terrainAt(hi.x, lo.z));
                        g = maxf(g, terrainAt(lo.x, hi.z));
                        g = maxf(g, terrainAt(hi.x, hi.z));
                        g = maxf(g, terrainAt((lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f));
                        // The TOP of it has to be under the ground, not just
                        // the bottom: a trunk resting in a hollow has its
                        // underside below the ground at the corners of its own
                        // bounding box, every frame, and lifting for that is
                        // the bobbing all over again.
                        // ...AND IT COMES BACK OUT, AND THEN IT STAYS PUT.
                        //
                        // Two attempts at doing this with forces alone are
                        // worth recording, because both oscillate. Lifting to
                        // the TRIGGER height leaves the body buried with its
                        // top just under the surface, so it fires again the
                        // next frame -- 4 m/s of rise, forever. Lifting to put
                        // its UNDERSIDE on the surface throws a 26 m trunk
                        // nine metres into the air, and it falls through again.
                        //
                        // The honest reading is that a body which has gone
                        // through the floor is one the solver has already lost:
                        // PhysX's CCD is a linear sweep and says nothing about
                        // ROTATION, and a 26 m trunk turning at 2.5 rad/s has
                        // ends moving at thirty metres a second whatever its
                        // centre is doing. So it is put back on the surface and
                        // parked there. A felled tree is scenery by then, and
                        // scenery that lies still beats scenery that tumbles
                        // through the world.
                        if (hi.y < g - kUnderWorldM) {
                            ph.liftBy(d.phys, g - lo.y);
                            ph.makeKinematic(d.phys);
                            ph.poseOf(d.phys, &d.pos, d.quat);
                        }
                    }
                }
                // THE BACKSTOP THAT WAS HERE BEFORE, AND THE TRACE IS WHY NOT.
                //
                // There was one: six points down the trunk axis, lift the body
                // if the worst of them was far enough under the terrain. It was
                // measuring the body's ORIGIN, and for a felled tree that is the
                // model's base corner -- which sits well BELOW the ground by
                // construction, because the collider starts at the CUT and the
                // cut is over a metre up the trunk. So a tree that had landed
                // perfectly looked, to the backstop, like one that had sunk, and
                // it was hoisted back into the air.
                //
                // Headless trace (App::runFellTest), 60 Hz, a felled pine:
                //
                //      400 ms   y 29.96   falling at 4.0 m/s
                //      500 ms   y 29.93   LANDED -- spin damped 0.23 -> 0.06
                //     1000 ms   y 31.24   lifted 1.4 m, falling again
                //     1500 ms   y 31.28   ...and again, every half second
                //
                // That sawtooth IS "it bobs up and down in place". The ground
                // height field catches the tree perfectly well on its own -- the
                // landing at 500 ms is it doing exactly that -- and what the
                // tree needed was to be left alone.
            }

            // ---- and then it comes to you ---------------------------------
            if (!d.absorbing && !d.felled && d.voxels <= kAbsorbSize &&
                nowMs - d.bornMs > kAbsorbWaitMs) {
                d.absorbing = true;
                d.absorbT0 = nowMs;
                d.from = d.pos;
                // The solver stops owning it. It is on a curve now, not in a
                // fall, and the two would argue.
                ph.makeKinematic(d.phys);
            }

            if (d.absorbing) {
                const double kk = (nowMs - d.absorbT0) / kAbsorbFlyMs;
                const float k = kk >= 1.0 ? 1.0f : (kk <= 0.0 ? 0.0f : float(kk));
                const float e = k * k * (3.0f - 2.0f * k);   // leaves gently, arrives fast
                const Vec3 to{eye.x, eye.y + kAbsorbY - d.halfM[1], eye.z};
                d.pos.x = d.from.x + (to.x - d.from.x) * e;
                d.pos.y = d.from.y + (to.y - d.from.y) * e + sinf(e * 3.14159265f) * 0.3f;
                d.pos.z = d.from.z + (to.z - d.from.z) * e;
                ph.setPose(d.phys, d.pos, d.quat);
                if (k >= 1.0f) { retireDebris(ph, i); continue; }
            } else if (nowMs - d.bornMs > (d.felled ? kFelledLifeMs : kDebrisLifeMs)) {
                retireDebris(ph, i);
                continue;
            }

            // ...AND IT IS NEVER QUITE STILL.
            //
            // DRAWN ONLY, on top of whatever the solver says: it moves nothing,
            // applies nothing, and cannot push the piece into stone. It exists
            // because a chip that has landed and settled is, correctly, a rock
            // lying on the ground -- and a rock lying on the ground for the
            // half second before you pick it up reads as frozen.
            //
            // IT DOES NOT DECAY TO NOTHING. The version before this faded out
            // over half a second and then stopped, which is exactly the "wobble
            // and then freeze" it was meant to cure. This one keeps a small
            // amplitude for as long as the piece is on the ground.
            float wq[4] = {d.quat[0], d.quat[1], d.quat[2], d.quat[3]};
            if (!d.absorbing && !d.felled) {
                const float t = float((nowMs - d.bornMs) * 0.001);
                const float a = kWobbleRad * (0.55f + 0.45f * expf(-2.0f * t));
                const float w = sinf(t * 21.0f) * a;
                // A small turn about the piece's own leaning axis, composed
                // onto the solver's quaternion: q = wobble * q.
                const float hw = w * 0.5f;
                const float sw = sinf(hw), cw = cosf(hw);
                const float ax = d.wobbleAxis.x * sw, ay = d.wobbleAxis.y * sw,
                            az = d.wobbleAxis.z * sw;
                const float bx = d.quat[0], by = d.quat[1], bz = d.quat[2], bw = d.quat[3];
                wq[0] = cw * bx + ax * bw + ay * bz - az * by;
                wq[1] = cw * by - ax * bz + ay * bw + az * bx;
                wq[2] = cw * bz + ax * by - ay * bx + az * bw;
                wq[3] = cw * bw - ax * bx - ay * by - az * bz;
            }
            setDebrisInstance(i, d.pos, wq);
        }
    }

    // -----------------------------------------------------------------------
    // HAS THE AXE CUT THROUGH IT, AND IF SO, DOWN IT COMES.
    //
    // WHAT "CUT THROUGH" MEANS, exactly: flood fill the model's voxels from its
    // bottom rows, and ask whether its HIGHEST solid voxel is still reachable.
    // That is the honest question -- a trunk with a hole in it is still a tree,
    // and a trunk with no wood left across any horizontal line is not -- and it
    // needs no rule about how many blows or how wide the cut is. The fill runs
    // on 20 cm cells because it runs after every axe blow; see kSeverCell for
    // what that costs.
    //
    // NOT "anything disconnected falls", which is the version that looks right
    // and is not: measured on these models, the FIRST blow anywhere on a pine
    // disconnects seven cells out of ten thousand -- a twig -- and a rule that
    // fired on that would drop a whole tree for a scratch.
    //
    // WHAT FALLS IS THE WHOLE TREE, geometry and all, and it costs nothing to
    // draw because it borrows the structure and triangles the standing tree was
    // already using. Meshing the felled half would be a quarter of a million
    // triangles and a structure build on the frame the blow lands.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHATEVER IS NO LONGER STANDING ON ANYTHING COMES DOWN.
    //
    // NOT A TREE RULE. It began as one -- cut a trunk through and the tree
    // falls -- but the rule was never about trees: it is that a voxel not
    // connected to the ground has nothing holding it up. A boulder undercut at
    // its base should drop the same way, and so should the top of one carved
    // through the middle, and neither of those was happening: they hung in the
    // air exactly where the pick left them.
    //
    // So the flood fill runs after EVERY carve, on whatever was carved. What it
    // finds still connected to the model's bottom stays as the placed instance;
    // what it does not becomes a rigid body. A tree is then just the case where
    // the disconnected part is most of the model.
    //
    // TERRAIN IS NOT COVERED BY THIS AND CANNOT BE. The ground is an endless
    // height field with an edit layer over it: "connected to the ground" has no
    // bottom to fill from, so a dug overhang stays put. That is a different
    // problem and it is worth saying plainly rather than half-solving.
    // -----------------------------------------------------------------------
    bool fellTree(Physics &ph, const Solid &so, const Vec3 &swingDir, double nowMs,
                  bool whole = false) {
        if (so.decorSlot < 0 || so.modelKind < 0) return false;
        const auto ch = chunks_.find(so.ownerChunk);
        if (ch == chunks_.end()) return false;
        Chunk &c = ch->second;
        if (size_t(so.decorSlot) >= c.decorDesc.size()) return false;
        if (!c.decorDesc[size_t(so.decorSlot)].instanceMask) return false;   // already down

        const ModelTemplate &t = templateFor(so.modelKind, so.modelIndex);
        if (t.volume.empty() || t.sx <= 0) return false;
        const std::pair<long long, int> key{so.ownerChunk, int(so.decorSlot)};
        auto dit = damaged_.find(key);
        const std::vector<uint8_t> &vol = (dit == damaged_.end()) ? t.volume : dit->second.vol;

        // The fill leaves sevSolid_ and sevSeen_ describing this volume: which
        // coarse cells hold wood, and which of those are still standing on the
        // ground. That is the cut line, and it is the only thing that decides
        // what falls -- no height, no guess about where the axe was.
        // HOW MUCH HAS TO COME AWAY BEFORE IT IS WORTH A BODY.
        //
        // Two thresholds, because they answer two different questions. The
        // FRACTION is what stops a tree falling over because a twig came off:
        // measured on the real models, a glancing blow leaves 0.4% of a pine
        // disconnected and a cut through the trunk leaves 99%.
        //
        // The SIZE is what makes an undercut boulder drop its top. A rock does
        // not have a trunk, so a piece coming off it is often a small part of
        // the whole -- but it is still a piece that is standing on nothing, and
        // the user's rule is that nothing floats. The floor under it exists
        // only so that three stray voxels do not each take one of the sixty-four
        // debris slots.
        // WHAT THIS BLOW DID, and not what the model was drawn like -- see
        // ModelTemplate::bornLoose, which is what three pines falling over on
        // their first blow cost to find.
        if (t.bornLoose < 0) {
            looseFraction(t.volume, t.sx, t.sy, t.sz);
            t.bornLoose = sevLooseCells_;
        }
        looseFraction(vol, t.sx, t.sy, t.sz);
        if (whole) {
            // NOTHING IS CONNECTED TO THE GROUND, because the ground is gone --
            // see dropUndermined. The fill's answer is about the model's own
            // bottom row, which is exactly the assumption that has stopped
            // being true, so it is thrown away and all of it falls.
            sevSeen_.assign(sevSeen_.size(), 0);
        } else {
            const int cut = sevLooseCells_ - t.bornLoose;
            if (cut < kMinLooseCells && sevLooseFrac_ < kFellFraction) return false;
        }

        int slot = -1;
        for (int i = 0; i < kDebrisInstances; ++i)
            if (!debris_[i].live) { slot = i; break; }
        if (slot < 0) return false;

        // ---- SPLIT IT IN TWO ----------------------------------------------
        //
        // THE STUMP STAYS BOLTED DOWN. It is still the placed instance: same
        // slot, same transform, just with everything above the cut gone. Only
        // the severed part becomes a body, which is what a felled tree does --
        // the butt end does not walk off with the trunk.
        const int f = kSeverCell;
        const int nx = (t.sx + f - 1) / f, nz = (t.sz + f - 1) / f;
        auto cellIx = [&](int x, int y, int z) {
            return size_t(x / f) + size_t(z / f) * size_t(nx) +
                   size_t(y / f) * size_t(nx) * size_t(nz);
        };
        fallVol_.assign(vol.size(), mat::AIR);
        stumpVol_.assign(vol.size(), mat::AIR);
        for (int y = 0; y < t.sy; ++y)
            for (int z = 0; z < t.sz; ++z)
                for (int x = 0; x < t.sx; ++x) {
                    const size_t i = size_t(x) + size_t(z) * size_t(t.sx) +
                                     size_t(y) * size_t(t.sx) * size_t(t.sz);
                    if (vol[i] == mat::AIR) continue;
                    if (sevSeen_[cellIx(x, y, z)])
                        stumpVol_[i] = vol[i];   // still connected to the ground
                    else
                        fallVol_[i] = vol[i];    // no longer standing on anything
                }

        // ---- the half that falls, as geometry ------------------------------
        const VoxMesh fallMesh = meshVolume(fallVol_, t.sx, t.sy, t.sz, VOXEL_M);
        if (fallMesh.triCount() == 0) return false;
        Blas fallBlas = recordLooseBuild(fallMesh);
        if (!fallBlas.valid()) return false;

        // ---- ...and as a collider ------------------------------------------
        //
        // A convex hull is not an option and it is worth saying why: the hull
        // of a pine is a fat cone, so a felled one would come to rest balanced
        // on its canopy with the trunk in the air. Boxes off its own voxels,
        // coarsened until the count is one a dynamic body can carry -- see
        // kFellCellM, which is measured rather than chosen.
        // WHAT KIND OF THING THIS IS, asked before the collider rather than
        // after it: a trunk and a boulder want different shapes out of the same
        // voxels, and the loop below is where that first matters.
        const bool isTree = (so.modelKind == 0);

        float cell = kFellCellM;
        for (int tries = 0; tries < 4; ++tries) {
            const int q = maxi(1, int(cell / VOXEL_M + 0.5f));
            const int cx = (t.sx + q - 1) / q, cy = (t.sy + q - 1) / q, cz = (t.sz + q - 1) / q;
            // THE TRUNK'S COLUMN, IN THE MODEL'S OWN FRAME -- see
            // kFellFillFrac. baseCX/baseCZ is where the model MEETS THE GROUND
            // relative to the middle of its box, which for a birch is metres
            // away, so this follows the trunk rather than the crown.
            const float bcx = float(t.sx) * VOXEL_M * 0.5f + t.col.baseCX;
            const float bcz = float(t.sz) * VOXEL_M * 0.5f + t.col.baseCZ;
            const float bhx = maxf(kTrunkPadM, float(t.col.baseX) * VOXEL_M * 0.5f + kTrunkPadM);
            const float bhz = maxf(kTrunkPadM, float(t.col.baseZ) * VOXEL_M * 0.5f + kTrunkPadM);
            const int need = maxi(1, int(float(q * q * q) * kFellFillFrac));
            greedyBoxes(
                cx, cy, cz, Vec3{0.0f, 0.0f, 0.0f}, cell,
                [&](int i, int j, int k) {
                    // Outside the trunk's own column: the canopy, which the
                    // solver is not told about.
                    //
                    // A TREE RULE, and only a tree's. A boulder is dense all
                    // the way out, so clipping one to its base footprint would
                    // hand the solver a narrow post where a twenty-metre rock
                    // is -- and the undercut boulder that this same function
                    // drops would then fall through everything beside it.
                    if (isTree) {
                        const float mx = (float(i) + 0.5f) * cell;
                        const float mz = (float(k) + 0.5f) * cell;
                        if (fabsf(mx - bcx) > bhx || fabsf(mz - bcz) > bhz) return false;
                    }
                    int n = 0;
                    for (int b2 = 0; b2 < q; ++b2)
                        for (int a2 = 0; a2 < q; ++a2)
                            for (int e2 = 0; e2 < q; ++e2) {
                                const int x = i * q + e2, y = j * q + b2, z = k * q + a2;
                                if (x >= t.sx || y >= t.sy || z >= t.sz) continue;
                                if (fallVol_[size_t(x) + size_t(z) * size_t(t.sx) +
                                             size_t(y) * size_t(t.sx) * size_t(t.sz)] != mat::AIR)
                                    if (++n >= need) return true;
                            }
                    return false;
                },
                &winBoxes_, kFellMaxBoxes * 4);
            if (winBoxes_.size() <= kFellMaxBoxes) break;
            cell *= 1.5f;
        }
        if (winBoxes_.empty()) return false;

        // THE ACTOR STANDS WHERE THE MODEL DOES, at its origin corner, wearing
        // the model's own quarter turn -- which is the frame the boxes are in
        // and the frame the mesh is in, so the solver and the renderer describe
        // the same object with the same numbers, and the tree does not jump on
        // the frame it is cut.
        const float yawRad = float(so.yaw & 3) * 1.57079633f;
        const int phys = ph.addCompoundBody(winBoxes_.data(), int(winBoxes_.size()),
                                            Vec3{so.tx, so.baseY, so.tz}, yawRad,
                                            isTree ? kTimberDensity : kStoneDensity);
        if (phys < 0) return false;

        // ...AND IT WEIGHS WHAT A TREE WEIGHS. Left to the shapes it came out
        // at over a hundred tonnes -- see Physics::setBodyMass.
        if (isTree) {
            // FROM THE BOXES THE SOLVER ACTUALLY HAS, which are the trunk. Left
            // to PhysX this came from the shape VOLUME at timber density and
            // measured 128 to 297 tonnes; a felled pine is nearer a tonne.
            double logVol = 0.0;   // not `vol`: the model's voxels are already that
            for (const VoxBox &b : winBoxes_) logVol += 8.0 * double(b.hx) * b.hy * b.hz;
            ph.setBodyMass(phys, maxf(50.0f, float(logVol * kTimberDensity)));
        }

        // AWAY FROM THE AXE, AND ONLY FOR A TREE. A severed trunk standing
        // upright is in equilibrium and would stay there; a real one hinges on
        // the fibres the cut has not reached and goes over away from whoever
        // swung. That is a felling nudge and it is the one force this applies.
        //
        // A piece of rock gets none of it. It was never balanced -- it was
        // resting on stone that is no longer there -- so gravity is the whole
        // story and anything else would be a shove.
        const float dl = sqrtf(swingDir.x * swingDir.x + swingDir.z * swingDir.z);
        if (isTree && dl > 1e-4f) {
            const float dx = swingDir.x / dl, dz = swingDir.z / dl;
            ph.nudgeSpin(phys, Vec3{-dz, 0.0f, dx}, kFellNudge);
        }

        Debris &d = debris_[slot];
        d = Debris{};
        d.live = true;
        d.felled = true;
        d.boxes = winBoxes_;   // before buildSolidWindow reuses the scratch
        d.phys = phys;
        d.bornMs = nowMs;
        d.lastMs = nowMs;
        d.blas = std::move(fallBlas);
        d.triOffset = pool_.upload(ctx_, fallMesh.tri);
        d.tris = fallMesh.tri.size();
        d.tint = c.decorInfo[size_t(so.decorSlot)].tint;
        // Its voxels are a whole tree's, which is far past kAbsorbSize: a
        // felled tree is scenery, not loot, and is never collected.
        d.voxels = t.sx * t.sy * t.sz;
        d.halfM[0] = 0.5f * float(t.sx) * VOXEL_M;
        d.halfM[1] = 0.5f * float(t.sy) * VOXEL_M;
        d.halfM[2] = 0.5f * float(t.sz) * VOXEL_M;
        // The mesh's origin IS the actor's origin here -- both are the model's
        // corner -- so there is no offset to carry, unlike a chip, whose body
        // sits at its centre of mass.
        d.originOff = Vec3{0.0f, 0.0f, 0.0f};
        d.pos = Vec3{so.tx, so.baseY, so.tz};
        d.quat[0] = 0.0f;
        d.quat[1] = std::sin(yawRad * 0.5f);
        d.quat[2] = 0.0f;
        d.quat[3] = std::cos(yawRad * 0.5f);
        d.winCentre = d.pos;

        // ---- AND WHAT WAS HUNG ON IT GOES WITH IT --------------------------
        //
        // Cones and hives are their own placements, hung in the canopy by
        // hangPinecones -- so a tree that leaves without them leaves them in
        // the air where its branches used to be.
        if (isTree) dropHangers(c, so, t);

        // ---- the stump: the same instance, with the top gone ---------------
        if (dit == damaged_.end()) {
            Damaged nd;
            nd.vol = t.volume;
            dit = damaged_.emplace(key, std::move(nd)).first;
        }
        Damaged &dm = dit->second;
        dm.vol = stumpVol_;

        // ---- AND THE WHOLE TREE STOPS BEING ON ITS WAY BACK ----------------
        //
        // THE BLOW THAT FELLED IT ALREADY QUEUED A MESH OF IT. carveModel runs
        // first and hands the worker a copy of the volume as it was then --
        // which is the entire tree, minus the last bite. This function then
        // replaces that volume with the STUMP and meshes it here and now.
        //
        // Without bumping the sequence, the worker's result comes back a frame
        // or two later still carrying the seq the instance has, so pumpRemesh
        // adopts it and the standing tree is back -- while the half that fell
        // is lying on the ground beside it. That is the second copy, and
        // because a tree is always over kInlineMeshVox it happened every time.
        //
        // `meshed` moves with it, so the stump built below is what the instance
        // is recorded as already having.
        ++dm.seq;
        dm.meshed = dm.seq;

        const VoxMesh stump = meshVolume(dm.vol, t.sx, t.sy, t.sz, VOXEL_M);
        if (stump.triCount() == 0) {
            // CUT OFF AT THE VERY GROUND: there is no stump to leave, so the
            // slot is hidden -- and remembered, because chunk decor is
            // regenerated from the deterministic scatter and would otherwise
            // put the whole tree back on the next adopt.
            c.decorDesc[size_t(so.decorSlot)].instanceMask = 0;
            dropSolid(c, so);
            fellSlots_.insert({so.ownerChunk, int(so.decorSlot)});
        } else if (Blas sb = recordLooseBuild(stump); sb.valid()) {
            retireLoose(std::move(dm.blas), dm.triOffset, dm.tris);
            dm.triOffset = pool_.upload(ctx_, stump.tri);
            dm.tris = stump.tri.size();
            dm.blas = std::move(sb);
            c.decorDesc[size_t(so.decorSlot)].accelerationStructure = dm.blas.as->getGpuAddress();
            c.decorInfo[size_t(so.decorSlot)].triOffset = dm.triOffset;
            refitSolid(c, so, t, dm);
        }

        // ---- ...AND ONLY NOW IS THERE STONE TO FALL AGAINST ----------------
        //
        // AFTER THE STUMP, AND THAT ORDER IS THE WHOLE OF IT. buildWindow reads
        // the world's CURRENT voxels through Solid::vol -- so built one line
        // earlier, before the stump was committed, it sampled the tree that is
        // in the act of leaving and handed PhysX a solid box for every cell of
        // the standing trunk. The falling body was then born entirely inside a
        // collider of its own former self: the solver pushed it out, gravity
        // pulled it back, and the result was a tree glitching up and down as it
        // sank slowly to the ground. Built here it sees the stump and the
        // neighbours, which is what is actually still there.
        // EVERYTHING AROUND IT IS SOLID, ALONG ITS WHOLE LENGTH.
        //
        // Not the 3.2 m cube buildWindow makes. That is the right window for a
        // chip and it is a joke for a trunk: a 26 m tree was given stone to
        // fall against for the first three metres of itself and clipped through
        // every rock past that, which is exactly what was reported. This covers
        // the body's own bounds with room to turn in, and updateDebris rebuilds
        // it as the tree comes down.
        {
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            if (ph.boundsOf(phys, &lo, &hi)) {
                d.winLo = Vec3{lo.x - kStaticPadM, lo.y - kStaticPadM, lo.z - kStaticPadM};
                d.winHi = Vec3{hi.x + kStaticPadM, hi.y + kStaticPadM, hi.z + kStaticPadM};
                d.window = buildSolidWindow(ph, d.winLo, d.winHi);
            }
        }
        d.winCentre = d.pos;

        debrisDirty_ = true;
        rebuildTlas();
        return true;
    }

    // Hide the cones and hives that were hung in this tree's canopy.
    //
    // They are separate placements with no link back to the tree they belong
    // to, so they are found the way you would find them by eye: a cone-or-hive
    // slot standing inside this model's footprint and above its base was hung
    // on this tree. Recorded in fellSlots_ with the tree, so a chunk that is
    // evicted and re-adopted does not put them back in the air.
    void dropHangers(Chunk &c, const Solid &so, const ModelTemplate &t) {
        const float ex = float(t.sx) * VOXEL_M, ez = float(t.sz) * VOXEL_M;
        float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
        solidWorldSpace(so, 0.0f, 0.0f, &ax, &az);
        solidWorldSpace(so, ex, ez, &bx, &bz);
        const float x0 = minf(ax, bx), x1 = maxf(ax, bx);
        const float z0 = minf(az, bz), z1 = maxf(az, bz);
        for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
            const DecorAt &q = c.decorAt[i];
            if (q.kind != 4 && q.kind != 5) continue;   // cones and hives
            if (!c.decorDesc[i].instanceMask) continue;
            if (q.x < x0 || q.x > x1 || q.z < z0 || q.z > z1) continue;
            if (q.y < so.baseY) continue;
            c.decorDesc[i].instanceMask = 0;
            fellSlots_.insert({so.ownerChunk, int(i)});
        }
    }

    // HOW MUCH OF THE TREE IS NO LONGER STANDING ON ANYTHING    // HOW MUCH OF THE TREE IS NO LONGER STANDING ON ANYTHING -- as a fraction
    // of it, from 0 (all connected to the ground) to 1 (none of it is).
    //
    // NOT "IS THE TOPMOST VOXEL REACHABLE", which is the version this shipped
    // with for one build and which felled a tree on its FIRST blow. The top of
    // a birch is a leaf cluster with air around it, so it is not six-connected
    // to the trunk at any coarseness and the answer was already "no" before the
    // axe ever landed. The same is true of every isolated needle and twig in
    // the model.
    //
    // A FRACTION SEPARATES THEM CLEANLY, and measurably so: on these models a
    // blow that merely nicks a pine disconnects 7 cells out of 10,068 -- 0% --
    // while a cut through a birch's trunk disconnects 3,818 out of 3,846, which
    // is 99%. Nothing lands between those, so the threshold is not delicate.
    //
    // Six-connected over cells of kSeverCell voxels, and coarse in the SAFE
    // direction: a cell counts as solid if any voxel in it is, so the fill can
    // only ever find MORE connection than there really is. The failure mode is
    // a tree that needs one more blow, not one that falls when it should not.
    float looseFraction(const std::vector<uint8_t> &vol, int sx, int sy, int sz) const {
        const int f = kSeverCell;
        const int nx = (sx + f - 1) / f, ny = (sy + f - 1) / f, nz = (sz + f - 1) / f;
        const size_t n = size_t(nx) * size_t(ny) * size_t(nz);
        if (!n) return 0.0f;
        sevSolid_.assign(n, 0);
        auto ix = [&](int i, int j, int k) {
            return size_t(i) + size_t(k) * size_t(nx) + size_t(j) * size_t(nx) * size_t(nz);
        };
        size_t solid = 0;
        for (int y = 0; y < sy; ++y)
            for (int z = 0; z < sz; ++z)
                for (int x = 0; x < sx; ++x)
                    if (vol[size_t(x) + size_t(z) * size_t(sx) +
                            size_t(y) * size_t(sx) * size_t(sz)] != mat::AIR) {
                        uint8_t &cell = sevSolid_[ix(x / f, y / f, z / f)];
                        if (!cell) { cell = 1; ++solid; }
                    }
        if (!solid) return 0.0f;

        sevSeen_.assign(n, 0);
        sevStack_.clear();
        size_t reached = 0;
        for (int k = 0; k < nz; ++k)
            for (int i = 0; i < nx; ++i)
                if (sevSolid_[ix(i, 0, k)] && !sevSeen_[ix(i, 0, k)]) {
                    sevSeen_[ix(i, 0, k)] = 1;
                    ++reached;
                    sevStack_.push_back(int(ix(i, 0, k)));
                }
        while (!sevStack_.empty()) {
            const int p = sevStack_.back();
            sevStack_.pop_back();
            const int i = p % nx;
            const int k = (p / nx) % nz;
            const int j = p / (nx * nz);
            static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            for (const int *o : off) {
                const int a = i + o[0], b = j + o[1], e = k + o[2];
                if (a < 0 || b < 0 || e < 0 || a >= nx || b >= ny || e >= nz) continue;
                const size_t q = ix(a, b, e);
                if (!sevSolid_[q] || sevSeen_[q]) continue;
                sevSeen_[q] = 1;
                ++reached;
                sevStack_.push_back(int(q));
            }
        }
        sevLooseCells_ = int(solid - reached);
        sevLooseFrac_ = float(double(solid - reached) / double(solid));
        return sevLooseFrac_;
    }

    // Hand a loose structure (and its slice of the triangle pool) over to be
    // freed once the device is finished with it -- see RetiredLoose.
    void retireLoose(Blas &&b, uint32_t triOffset, size_t tris) {
        if (!b.valid() && triOffset == TriPool::kInvalid) return;
        RetiredLoose r;
        r.blas = std::move(b);
        r.triOffset = triOffset;
        r.tris = tris;
        r.at = epoch_;
        retiredLoose_.push_back(std::move(r));
    }

    // ...and actually free the ones it has passed. Once a frame.
    void sweepLoose() {
        for (size_t i = 0; i < retiredLoose_.size();) {
            if (retiredLoose_[i].at > deviceDone_) { ++i; continue; }
            if (retiredLoose_[i].triOffset != TriPool::kInvalid && retiredLoose_[i].tris)
                pool_.release(retiredLoose_[i].triOffset, retiredLoose_[i].tris);
            retiredLoose_[i] = std::move(retiredLoose_.back());
            retiredLoose_.pop_back();
        }
    }

    // Put every remembered break back onto a chunk that has just been rebuilt.
    // A chunk returning from the mesher is built from the TEMPLATES, so every
    // boulder in it is pristine again -- and chunks come back for reasons that
    // have nothing to do with the player. Decor slots are positional and the
    // scatter is deterministic, so a slot means the same instance across a
    // rebuild, which makes this a re-point rather than a search.
    void reapplyDamage(Chunk &c, long long key) {
        // A TREE THAT WAS FELLED STAYS FELLED.
        //
        // Chunk decor is regenerated from the deterministic scatter every time
        // a chunk is adopted, so without this a tree you cut down comes back
        // standing the moment you walk far enough away and return -- and the
        // one lying on the ground is still lying there, which is two of it.
        // Damage survives eviction because damaged_ is keyed by chunk and slot
        // and outlives the chunk; this is the same trick for the fact of being
        // down at all.
        for (auto it = fellSlots_.lower_bound({key, INT_MIN});
             it != fellSlots_.end() && it->first == key; ++it) {
            const int slot = it->second;
            if (slot >= 0 && size_t(slot) < c.decorDesc.size()) {
                c.decorDesc[size_t(slot)].instanceMask = 0;
                for (Solid &sl : c.solids)
                    if (sl.decorSlot == slot) { sl.hx = 0.0f; sl.hz = 0.0f; sl.col = nullptr;
                                                sl.vol = nullptr; }
            }
        }
        if (damaged_.empty()) return;
        for (auto it = damaged_.lower_bound({key, INT_MIN});
             it != damaged_.end() && it->first.first == key; ++it) {
            const int slot = it->first.second;
            if (slot < 0 || size_t(slot) >= c.decorDesc.size()) continue;
            Damaged &d = it->second;
            if (!d.blas.valid()) continue;
            c.decorDesc[size_t(slot)].accelerationStructure = d.blas.as->getGpuAddress();
            c.decorInfo[size_t(slot)].triOffset = d.triOffset;
            for (Solid &sl : c.solids) {
                if (sl.decorSlot != slot) continue;
                if (!d.colTop.empty()) sl.col = d.colTop.data();
                // THE HOLE HAS TO SURVIVE THE RE-ADOPT TOO. Without this the
                // instance goes back to answering out of the pristine template
                // and the rock you broke is whole again to everything that
                // asks, while still LOOKING broken.
                if (!d.vol.empty()) sl.vol = d.vol.data();
            }
        }
    }

    void clearDebris(Physics &ph) {
        for (int i = 0; i < kDebrisInstances; ++i)
            if (debris_[i].live) retireDebris(ph, i);
    }

    // ---------------------------------------------------------------------
    // TAKE A BITE OUT OF THE WORLD, at a point in world METRES.
    //
    // This is the whole edit path, and it is three steps: write the hole into
    // the edit layer, ask the mesher for the chunks it touched, and let the
    // ordinary streaming machinery carry the result home. There is no second
    // renderer and no device-side mutation anywhere in it -- a dig is the
    // same work the engine already does every time you walk into a new chunk,
    // triggered by a swing instead of by a footstep.
    //
    // The old chunk keeps drawing until the new one lands, which is what
    // makes this safe to do mid-frame: nothing is torn down here.
    // ---------------------------------------------------------------------
    size_t dig(const Vec3 &p, int radiusVox, std::vector<uint8_t> *spoil = nullptr,
               int *spoilN = nullptr, Vec3 *spoilAt = nullptr) {
        const int ci = int(std::floor(p.x / VOXEL_M));
        const int cj = int(std::floor(p.z / VOXEL_M));
        const int cy = int(std::floor(p.y / VOXEL_M));
        // Snapped to the voxel that was actually carved, so the piece leaves
        // the hole rather than the point the ray happened to cross.
        if (spoilAt)
            *spoilAt = Vec3{(float(ci) + 0.5f) * VOXEL_M, (float(cy) + 0.5f) * VOXEL_M,
                            (float(cj) + 0.5f) * VOXEL_M};

    // WHAT CAME OUT, AS VOXELS. A chip has to be made of the same cubes the
    // thing it came off is made of -- the boulders carry their own minted
    // palette entries, so a chip built out of mat::ROCK is terrain-coloured
    // stone flying off a rock that is not that colour. Filled with the material
    // of every voxel actually removed, in the same (2r+1) cube layout
    // meshVolume reads.
        // Sampled BEFORE the carve, because after it they are all air.
        if (spoil && spoilN) {
            const int n = radiusVox * 2 + 1;
            const int r2 = radiusVox * radiusVox;
            *spoilN = n;
            spoil->assign(size_t(n) * size_t(n) * size_t(n), mat::AIR);
            // GENERATED, THEN DUG -- and the second term is what stops a pit
            // paying out twice. The sphere of a second bite overlaps the first,
            // and asking the generator alone reports those voxels as the soil
            // they were BORN as rather than the air they are now: the chunk
            // that came out was bigger than the hole that appeared, and a
            // shovel working one pit is nothing but overlapping bites. One
            // probe for the whole sphere, so the generator's octave cache is
            // shared across the columns -- see TerrainProbe.
            TerrainProbe probe(&terrain, &mesher_.edits);
            for (int dz = -radiusVox; dz <= radiusVox; ++dz)
                for (int dx = -radiusVox; dx <= radiusVox; ++dx) {
                    const int i = ci + dx, j = cj + dz;
                    for (int dy = -radiusVox; dy <= radiusVox; ++dy) {
                        if (dx * dx + dy * dy + dz * dz > r2) continue;
                        const uint8_t m = probe.material(i, j, cy + dy);
                        if (m == mat::AIR) continue;
                        const size_t x = size_t(dx + radiusVox), y = size_t(dy + radiusVox),
                                     z = size_t(dz + radiusVox);
                        (*spoil)[x + z * size_t(n) + y * size_t(n) * size_t(n)] = m;
                    }
                }
        }
        const std::vector<std::pair<int, int>> touched =
            mesher_.edits.carve(ci, cj, cy, radiusVox);
        groundDirty_ = true;   // the floor the solver stands on has moved
        // ...AND NOTHING THE BITE CUT LOOSE IS LEFT IN THE AIR.
        dropTerrainHangers(ci, cj, cy, radiusVox, spoil, spoil && spoilN ? *spoilN : 0);
        size_t asked = 0;
        for (const auto &c : touched) {
            const long long k = chunkKey(c.first, c.second);
            // NOT RESIDENT IS NOT A PROBLEM. The edit is already stored, so a
            // chunk that streams in later meshes WITH the hole in it.
            if (!chunks_.count(k)) continue;
            requested_.insert(k);
            mesher_.request(c.first, c.second);
            ++asked;
        }
        return asked;
    }

    // -----------------------------------------------------------------------
    // ...AND THE GROUND THE BITE CUT LOOSE COMES DOWN WITH IT.
    //
    // The old note here said the terrain could not be asked this, because a
    // height field has no bottom to flood from. It has one: everything below
    // the LOWEST hole in a column is contiguous stone all the way to bedrock,
    // because that is what a height field with a hole map over it means. So
    // those voxels are the seeds, the flood runs six ways through the stone
    // around the bite, and whatever it does not reach is standing on nothing.
    //
    // WHAT HAPPENS TO IT. It is taken out of the world and put into the SPOIL
    // -- the piece the blow already frees, which is a rigid body under gravity
    // a moment later. That is the honest answer to "subject to gravity" for a
    // voxel or two of dirt: it comes away with the pick rather than being left
    // in the air, and it is not worth a body of its own. MEASURED: a dig site
    // leaves 1.5 such voxels on average, and the worst of 300 left 26.
    //
    // BOUNDED, and it has to be: this runs on every blow. The box is
    // kHangBoxVox around the bite, which is wider than anything a 30 cm sphere
    // can cut loose.
    // -----------------------------------------------------------------------
    void dropTerrainHangers(int ci, int cj, int cy, int radiusVox, std::vector<uint8_t> *spoil,
                            int spoilN) {
        const int r = kHangBoxVox;
        const int n = r * 2 + 1;
        const int i0 = ci - r, j0 = cj - r, y0 = cy - r;
        TerrainMemo memo;
        hangSolid_.assign(size_t(n) * size_t(n) * size_t(n), 0);
        hangSeen_.assign(hangSolid_.size(), 0);
        auto ix = [&](int a, int b, int c) {
            return size_t(a) + size_t(c) * size_t(n) + size_t(b) * size_t(n) * size_t(n);
        };
        // THE EDIT SETS ONCE PER CHUNK, NOT ONCE PER VOXEL. EditStore::get
        // takes a lock, and this box holds 6,859 of them on every blow -- which
        // would be a new hitch bolted onto the one that was just measured out.
        // A 1.9 m box touches four chunks at worst.
        std::shared_ptr<const ChunkEdits> ceCache[4];
        long long ceKey[4] = {0, 0, 0, 0};
        int ceN = 0;
        auto editsAt = [&](int wi, int wj) -> const ChunkEdits * {
            const int cx = floorDiv(wi, CHUNK_VOX), cz = floorDiv(wj, CHUNK_VOX);
            const long long k = chunkKey(cx, cz);
            for (int e = 0; e < ceN; ++e)
                if (ceKey[e] == k) return ceCache[e].get();
            if (ceN < 4) {
                ceKey[ceN] = k;
                ceCache[ceN] = mesher_.edits.get(cx, cz);
                return ceCache[ceN++].get();
            }
            return mesher_.edits.get(cx, cz).get();   // never reached for this box size
        };
        size_t solid = 0;
        for (int c = 0; c < n; ++c)
            for (int a = 0; a < n; ++a) {
                const int wi = i0 + a, wj = j0 + c;
                const int h = terrain.heightVox(wi, wj, memo);
                const ChunkEdits *ce = editsAt(wi, wj);
                for (int b = 0; b < n; ++b) {
                    const int wy = y0 + b;
                    if (wy > h) continue;
                    if (ce) {
                        uint8_t m = mat::AIR;
                        if (ce->voxel(wi, wj, wy, &m) && m == mat::AIR) continue;
                    }
                    hangSolid_[ix(a, b, c)] = 1;
                    ++solid;
                }
            }
        if (!solid) return;

        // SEEDED FROM WHAT IS PROVABLY STANDING ON BEDROCK: the lowest solid
        // voxel of each column inside the box, and the whole floor of the box.
        // A column that is solid at the bottom of the box is solid from there
        // down, because nothing above has been dug out below it.
        hangStack_.clear();
        for (int c = 0; c < n; ++c)
            for (int a = 0; a < n; ++a) {
                if (!hangSolid_[ix(a, 0, c)]) continue;
                hangSeen_[ix(a, 0, c)] = 1;
                hangStack_.push_back(int(ix(a, 0, c)));
            }
        size_t reached = hangStack_.size();
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        while (!hangStack_.empty()) {
            const int p = hangStack_.back();
            hangStack_.pop_back();
            const int a = p % n, c = (p / n) % n, b = p / (n * n);
            for (const int *o : off) {
                const int x = a + o[0], y = b + o[1], z = c + o[2];
                if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) continue;
                const size_t q = ix(x, y, z);
                if (!hangSolid_[q] || hangSeen_[q]) continue;
                hangSeen_[q] = 1;
                ++reached;
                hangStack_.push_back(int(q));
            }
        }
        if (reached == solid) return;   // the ordinary case, and it costs nothing more

        // ...AND OUT THEY COME. A voxel on the box's WALL is not judged: the
        // flood could not see the stone next to it, so it may be perfectly well
        // supported from outside. Only the interior is decided here.
        for (int b = 1; b < n - 1; ++b)
            for (int c = 1; c < n - 1; ++c)
                for (int a = 1; a < n - 1; ++a) {
                    const size_t q = ix(a, b, c);
                    if (!hangSolid_[q] || hangSeen_[q]) continue;
                    const int wi = i0 + a, wj = j0 + c, wy = y0 + b;
                    // Into the spoil, if the piece the blow freed reaches it.
                    if (spoil && spoilN > 0) {
                        const int dx = wi - ci + radiusVox, dy = wy - cy + radiusVox,
                                  dz = wj - cj + radiusVox;
                        if (dx >= 0 && dy >= 0 && dz >= 0 && dx < spoilN && dy < spoilN &&
                            dz < spoilN) {
                            const int h = terrain.heightVox(wi, wj, memo);
                            const uint8_t top = terrain.topMaterial(wi, wj, h);
                            const uint8_t m = terrain.materialAt(wi, wj, wy, h, top);
                            if (m != mat::AIR)
                                (*spoil)[size_t(dx) + size_t(dz) * size_t(spoilN) +
                                         size_t(dy) * size_t(spoilN) * size_t(spoilN)] = m;
                        }
                    }
                    for (const auto &t : mesher_.edits.carve(wi, wj, wy, 0)) {
                        const long long k = chunkKey(t.first, t.second);
                        if (!chunks_.count(k)) continue;
                        requested_.insert(k);
                        mesher_.request(t.first, t.second);
                    }
                }
    }

    // -----------------------------------------------------------------------
    // ...AND THE CHIPS THE BITE CUT LOOSE COME AWAY WITH IT.
    //
    // The sever fill is the answer for a piece big enough to be a body: 16
    // coarse cells, or 40% of the model. Below that the disconnected voxels
    // were simply left in the air. MEASURED with float_probe over 1,224 blows
    // on the real models, counting only what each blow itself disconnected:
    // SIX VOXELS PER BLOW left hanging, nearly all of it needles and leaf
    // clusters off the trees -- the rocks leave none worth naming.
    //
    // Same shape as dropTerrainHangers and for the same reason: a blow can only
    // cut loose what is near it, so the flood is a box around the bite rather
    // than the whole model. A boulder is three million voxels and flooding one
    // per swing would put the hitch straight back.
    //
    // ANCHORED IS THE BOX WALL. Anything solid on the surface of the box is
    // assumed to be held from outside, which is the safe direction: the failure
    // mode is a chip that stays a moment longer, not a rock that dissolves.
    // -----------------------------------------------------------------------
    void dropModelHangers(std::vector<uint8_t> &vol, int sx, int sy, int sz, int mx, int my,
                          int mz, std::vector<uint8_t> *spoil, int spoilN, int radiusVox) {
        const int r = kHangBoxModelVox;
        const int n = r * 2 + 1;
        const int i0 = mx - r, j0 = mz - r, y0 = my - r;
        auto at = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(sx) + size_t(y) * size_t(sx) * size_t(sz);
        };
        auto ix = [&](int a, int b, int c) {
            return size_t(a) + size_t(c) * size_t(n) + size_t(b) * size_t(n) * size_t(n);
        };
        hangSolid_.assign(size_t(n) * size_t(n) * size_t(n), 0);
        hangSeen_.assign(hangSolid_.size(), 0);
        size_t solid = 0;
        for (int b = 0; b < n; ++b)
            for (int c = 0; c < n; ++c)
                for (int a = 0; a < n; ++a) {
                    const int x = i0 + a, y = y0 + b, z = j0 + c;
                    if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) continue;
                    if (vol[at(x, y, z)] == mat::AIR) continue;
                    hangSolid_[ix(a, b, c)] = 1;
                    ++solid;
                }
        if (!solid) return;

        // ---- WHAT COUNTS AS SUPPORT, AND THE MODEL'S OWN FLOOR IS SOME -----
        //
        // The box wall stands in for "the rest of the model continues out
        // there, so this is still attached". That is right for a pine, whose
        // trunk leaves the box in every direction, and it is WRONG for a
        // mushroom or a pebble: a model smaller than the 39-voxel box never
        // touches the wall at all, so nothing seeds, every one of its voxels
        // comes back unsupported, and one tap turns the whole thing to AIR.
        // It does not fall, it does not break -- it DISAPPEARS.
        //
        // The tuning note on kHangBoxModelVox could not see this. It measured
        // "voxels left hanging per blow", and a model that vanishes entirely
        // leaves nothing hanging: it scores a perfect zero.
        //
        // So the model's own bottom layer -- y == 0 in model space, the part
        // actually resting on the ground -- seeds as well. For a small model
        // that is its real anchor and only the piece that was hit comes away.
        // For a big one it changes nothing, because the wall already seeded
        // everything the trunk touches.
        //
        // NOT a whole-model flood seeded only from the base, which is the
        // obvious "fix" and is a trap twice over: a pristine pine has foliage
        // that is not connected to its trunk in voxel space at all (see
        // ModelTemplate::bornLoose), so it would strip every needle on the
        // first blow -- and carveModel runs BEFORE fellTree, so it would also
        // delete a severed branch before fellTree could spawn a body for it.
        hangStack_.clear();
        for (int b = 0; b < n; ++b)
            for (int c = 0; c < n; ++c)
                for (int a = 0; a < n; ++a) {
                    const bool onWall = !(a && b && c && a < n - 1 && b < n - 1 && c < n - 1);
                    const bool onModelFloor = (y0 + b) == 0;
                    if (!onWall && !onModelFloor) continue;
                    const size_t q = ix(a, b, c);
                    if (!hangSolid_[q] || hangSeen_[q]) continue;
                    hangSeen_[q] = 1;
                    hangStack_.push_back(int(q));
                }
        size_t reached = hangStack_.size();
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        while (!hangStack_.empty()) {
            const int p = hangStack_.back();
            hangStack_.pop_back();
            const int a = p % n, c = (p / n) % n, b = p / (n * n);
            for (const int *o : off) {
                const int x = a + o[0], y = b + o[1], z = c + o[2];
                if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) continue;
                const size_t q = ix(x, y, z);
                if (!hangSolid_[q] || hangSeen_[q]) continue;
                hangSeen_[q] = 1;
                ++reached;
                hangStack_.push_back(int(q));
            }
        }
        if (reached == solid) return;

        for (int b = 1; b < n - 1; ++b)
            for (int c = 1; c < n - 1; ++c)
                for (int a = 1; a < n - 1; ++a) {
                    const size_t q = ix(a, b, c);
                    if (!hangSolid_[q] || hangSeen_[q]) continue;
                    const int x = i0 + a, y = y0 + b, z = j0 + c;
                    uint8_t &v = vol[at(x, y, z)];
                    if (spoil && spoilN > 0) {
                        const int dx = x - mx + radiusVox, dy = y - my + radiusVox,
                                  dz = z - mz + radiusVox;
                        if (dx >= 0 && dy >= 0 && dz >= 0 && dx < spoilN && dy < spoilN &&
                            dz < spoilN)
                            (*spoil)[size_t(dx) + size_t(dz) * size_t(spoilN) +
                                     size_t(dy) * size_t(spoilN) * size_t(spoilN)] = v;
                    }
                    v = mat::AIR;
                }
    }

    // ---------------------------------------------------------------------
    // BREAK ONE BOULDER, AND ONLY THAT ONE.
    //
    // A rock is not terrain. Twenty-five placements of the same model share a
    // single acceleration structure and differ only by a transform, which is
    // exactly what makes a forest of them affordable -- and exactly what makes
    // damaging one awkward: editing the template would chip every boulder in
    // the world in the same place.
    //
    // So the instance leaves the template on FIRST DAMAGE and not before. It
    // takes a private copy of the model's voxels, loses the bite out of that,
    // is re-meshed on its own, and gets its own structure. Everything nobody
    // has touched keeps sharing, and the cost is bounded by what the player has
    // actually broken rather than by the size of the world. This is what
    // ModelTemplate::volume was kept for.
    // ---------------------------------------------------------------------
    bool carveModel(const Solid &so, const Vec3 &eye, const Vec3 &dir, float reach,
                    int radiusVox, std::vector<uint8_t> *spoil = nullptr,
                    int *spoilN = nullptr, Vec3 *spoilAt = nullptr,
                    float *spoilYaw = nullptr) {
        if (so.decorSlot < 0 || so.modelKind < 0) return false;
        const auto ch = chunks_.find(so.ownerChunk);
        if (ch == chunks_.end()) return false;
        Chunk &c = ch->second;
        if (size_t(so.decorSlot) >= c.decorDesc.size()) return false;

        const ModelTemplate &t = templateFor(so.modelKind, so.modelIndex);
        if (t.volume.empty() || t.sx <= 0) return false;

        // THE COPY IS NOT MADE YET, and that is deliberate. A big boulder's
        // volume is nine megabytes; taking it on every swing that turns out to
        // MISS would spend that on nothing, over and over, for as long as
        // somebody kept swinging at thin air beside a rock. So the march below
        // reads whatever this instance already has -- its private copy if it has
        // been hit before, the shared template if it has not -- and the copy is
        // only taken once a bite is known to land.
        const std::pair<long long, int> key{so.ownerChunk, int(so.decorSlot)};
        auto it = damaged_.find(key);
        const std::vector<uint8_t> &src = (it == damaged_.end()) ? t.volume : it->second.vol;

        // WALK THE RAY UNTIL IT MEETS THE ROCK -- the SAME walk the swing made.
        //
        // Literally the same function over literally the same array, which is
        // the point. The swing decided a rock was hit by marching these voxels;
        // if the carve then went looking with a different rule it could come
        // back empty on a blow the player was told had landed, and that is
        // exactly what "it does not break on every hit" was. One march, one
        // answer.
        //
        // A `probe` rather than `so` itself because `so` is a COPY taken when
        // the ray was cast, and its pointer could in principle be a frame stale;
        // `src` below is what this function is about to edit. Pointing the march
        // at that removes the question.
        //
        // Bounded by the tool's own reach, so a rock whose real surface is
        // further away than the tool can stretch is still a miss.
        //
        // Read from whatever this instance currently is, so a second blow
        // marches through the hole the first one made and bites deeper.
        auto voxelOn = [&](const Vec3 &w, int *ox, int *oy, int *oz) -> uint8_t {
            float px = 0.0f, pz = 0.0f;
            solidModelSpace(so, w.x, w.z, &px, &pz);
            *ox = int(std::floor(px / VOXEL_M));
            *oz = int(std::floor(pz / VOXEL_M));
            *oy = int(std::floor((w.y - so.baseY) / VOXEL_M));
            if (*ox < 0 || *oy < 0 || *oz < 0 || *ox >= t.sx || *oy >= t.sy || *oz >= t.sz)
                return mat::AIR;
            return src[size_t(*ox) + size_t(*oz) * size_t(t.sx) +
                       size_t(*oy) * size_t(t.sx) * size_t(t.sz)];
        };
        Solid probe = so;
        probe.vol = src.data();
        probe.msx = int16_t(t.sx);
        probe.msz = int16_t(t.sz);
        probe.vsy = int16_t(t.sy);

        int mx = 0, my = 0, mz = 0;
        bool found = false;
        {
            float th = 0.0f;
            int hv[3] = {0, 0, 0};
            if (rayModelVoxels(probe, eye, dir, maxf(0.0f, reach), VOXEL_M, &th, hv)) {
                mx = hv[0];
                my = hv[1];
                mz = hv[2];
                found = true;
            }
        }
        // EVERY BLOW TAKES SOMETHING. NO EXCEPTIONS.
        //
        // If the march found nothing, the swing still LANDED -- swingRay said
        // so, the animation played and the tool rang. Refusing to carve then is
        // the engine arguing with the player about whether they hit the rock
        // they can see they hit, and it loses that argument every time: the
        // collider is a coarse cylinder and the disagreement is its fault, not
        // theirs.
        //
        // Since the swing and the carve now walk the same voxels, this should
        // never fire -- but "should never" is what the last four versions of
        // this said. So the ray is walked once more with the reach limit lifted
        // -- a tall boulder read as hit at its crown is still a hit -- and if
        // even that finds nothing, the bite goes to the nearest solid COLUMN.
        // colTop is the model's own column heightfield, one entry per (x, z)
        // and already loaded, so this is a walk over 40,000 int16s rather than
        // a search through nine million voxels. It cannot fail on a model that
        // has any solid voxel at all -- and one that does not has no collider
        // and was never hittable.
        if (!found) {
            // NOT `far`: windows.h still defines that as a 16-bit-era macro that expands to nothing,
            // and the error it causes names the type, not the name.
            const float farM = reach + float(t.sx + t.sy + t.sz) * VOXEL_M;
            float th = 0.0f;
            int hv[3] = {0, 0, 0};
            if (rayModelVoxels(probe, eye, dir, farM, VOXEL_M, &th, hv)) {
                mx = hv[0];
                my = hv[1];
                mz = hv[2];
                found = true;
            }
        }
        if (!found && !t.colTop.empty()) {
            // Where the swing was pointed, as a column: the ray's closest
            // approach to the collider's own centre.
            const float ccx = so.cx, ccz = so.cz;
            const float ty = ((ccx - eye.x) * dir.x + (ccz - eye.z) * dir.z);
            const float tc = maxf(0.0f, ty);
            int ax = 0, ay = 0, az = 0;
            voxelOn(Vec3{eye.x + dir.x * tc, eye.y + dir.y * tc, eye.z + dir.z * tc}, &ax, &ay,
                    &az);
            ax = mini(maxi(ax, 0), t.sx - 1);
            az = mini(maxi(az, 0), t.sz - 1);
            int best = -1, bestD = 0;
            for (int z = 0; z < t.sz; ++z)
                for (int x = 0; x < t.sx; ++x) {
                    const int h = int(t.colTop[size_t(x) + size_t(z) * size_t(t.sx)]);
                    if (h <= 0) continue;
                    const int d = (x - ax) * (x - ax) + (z - az) * (z - az);
                    if (best < 0 || d < bestD) { best = x + z * t.sx; bestD = d; mx = x; mz = z; my = h - 1; }
                }
            found = best >= 0;
        }
        if (!found) return false;   // a model with no solid voxel in it at all

        // IT BITES. Now the instance leaves the template it was sharing.
        if (it == damaged_.end()) {
            Damaged nd;
            nd.vol = t.volume;
            it = damaged_.emplace(key, std::move(nd)).first;
        }
        Damaged &d = it->second;

    // WHAT CAME OUT, AS VOXELS. A chip has to be made of the same cubes the
    // thing it came off is made of -- the boulders carry their own minted
    // palette entries, so a chip built out of mat::ROCK is terrain-coloured
    // stone flying off a rock that is not that colour. Filled with the material
    // of every voxel actually removed, in the same (2r+1) cube layout
    // meshVolume reads.
        const int sn = radiusVox * 2 + 1;
        if (spoil && spoilN) {
            *spoilN = sn;
            spoil->assign(size_t(sn) * size_t(sn) * size_t(sn), mat::AIR);
        }

        const int r2 = radiusVox * radiusVox;
        size_t removed = 0;
        for (int dy = -radiusVox; dy <= radiusVox; ++dy)
            for (int dz = -radiusVox; dz <= radiusVox; ++dz)
                for (int dx = -radiusVox; dx <= radiusVox; ++dx) {
                    if (dx * dx + dy * dy + dz * dz > r2) continue;
                    const int x = mx + dx, y = my + dy, z = mz + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= t.sx || y >= t.sy || z >= t.sz) continue;
                    uint8_t &v = d.vol[size_t(x) + size_t(z) * size_t(t.sx) +
                                       size_t(y) * size_t(t.sx) * size_t(t.sz)];
                    if (v == mat::AIR) continue;
                    if (spoil && spoilN)
                        (*spoil)[size_t(dx + radiusVox) + size_t(dz + radiusVox) * size_t(sn) +
                                 size_t(dy + radiusVox) * size_t(sn) * size_t(sn)] = v;
                    v = mat::AIR;
                    ++removed;
                }
        if (removed == 0) return false;   // the swing missed the model's own voxels

        // ...AND NOTHING THE BITE CUT LOOSE IS LEFT IN THE AIR EITHER.
        dropModelHangers(d.vol, t.sx, t.sy, t.sz, mx, my, mz, spoil, spoil && spoilN ? *spoilN : 0,
                         radiusVox);

        // WHERE THE BITE ACTUALLY LANDED, in world metres.
        //
        // NOT the point the swing reported: that is where the ray met the
        // collider's CYLINDER, and the march above walked on from there to find
        // the first real voxel -- which on a big rock is metres further in. A
        // chunk spawned at the reported point breaks off somewhere the hole is
        // not, which reads as it teleporting.
        // Which way this model stands, so the piece can be born wearing it.
        if (spoilYaw) *spoilYaw = float(so.yaw & 3) * 1.57079633f;
        if (spoilAt) {
            const float pmx = (float(mx) + 0.5f) * VOXEL_M;
            const float pmz = (float(mz) + 0.5f) * VOXEL_M;
            float wx = 0.0f, wz = 0.0f;
            solidWorldSpace(so, pmx, pmz, &wx, &wz);
            *spoilAt = Vec3{wx, so.baseY + (float(my) + 0.5f) * VOXEL_M, wz};
        }

        // ---- THE COLLIDER NOW, THE MESH SHORTLY --------------------------
        //
        // What you can WALK INTO is updated on this frame, because it is cheap
        // and because being stopped by stone that is no longer there is felt
        // immediately. What you can SEE is queued -- see Remesher, and the
        // measurements in the note above it. The instance keeps drawing its
        // previous mesh for a frame or two, which nobody has ever noticed,
        // because the thing that makes a blow feel like it landed is the chunk
        // flying at you and that is meshed from the spoil and appears now.
        refitSolid(c, so, t, d);

        ++d.seq;
        // SMALL ONES STAY ON THE FRAME. A pebble meshes in a tenth of a
        // millisecond, and going through a worker for that would only add a
        // frame of latency to the one case that never needed it.
        if (size_t(t.sx) * size_t(t.sy) * size_t(t.sz) <= kInlineMeshVox) {
            const VoxMesh mesh = meshVolume(d.vol, t.sx, t.sy, t.sz, VOXEL_M);
            adoptMesh(c, so.decorSlot, d, mesh);
        } else {
            Remesher::Job j;
            j.key = key;
            j.vol = d.vol;   // a copy: the volume keeps being edited behind it
            j.sx = t.sx;
            j.sy = t.sy;
            j.sz = t.sz;
            j.seq = d.seq;
            remesh_.submit(std::move(j));
        }
        rebuildTlas();
        return true;
    }

    size_t instanceCount() const { return instanceDescs_.size(); }
    size_t decorCount(int kind) const {
        size_t n = 0;
        for (const auto &kv : chunks_) n += size_t(kv.second.decorKind[kind]);
        return n;
    }
    size_t residentTris() const { return residentTris_; }
    size_t pendingChunks() { return mesher_.inFlight(); }

    // Where the deck sits in the world. Far from the wood so nothing streams
    // into view behind it, and at a round height so a model's own numbers are
    // easy to read off it.
    static Vec3 stageOrigin() { return Vec3(kStageAtX, kStageAtY, kStageAtZ); }
    static Vec3 stageCentre() {
        return Vec3(kStageAtX + float(kStageVox) * VOXEL_M * 0.5f, kStageAtY + VOXEL_M,
                    kStageAtZ + float(kStageVox) * VOXEL_M * 0.5f);
    }

    // In the editor, or in the wood.
    void setStage(bool on) {
        if (on == stage_) return;
        if (on) buildStage();
        stage_ = on;
        rebuildTlas();
    }
    bool staged() const { return stage_; }

    double buildMs() const { return buildMs_; }

    // -- where the main thread goes while the world streams -------------------
    // Meshing is off-thread and cannot stall a frame; everything below is
    // recorded on the frame's own thread and therefore can.
    struct Profile {
        double blasMs = 0.0;   // structure builds, including any device wait
        double poolMs = 0.0;   // triangle-attribute uploads
        double tlasMs = 0.0;   // top-level rebuilds
        double meshMs = 0.0;   // worker time, for context only
        size_t blasCalls = 0, tlasCalls = 0, adopted = 0, meshed = 0;
        double uncompactedMb = 0.0, compactedMb = 0.0;
        // What the recycling pools ended up holding, which is the price of not
        // allocating per chunk and the number to watch if it ever looks wrong.
        double poolMb = 0.0;
        size_t poolBuffers = 0, pendingCompactions = 0;
        double drainMs = 0.0, takeMs = 0.0, reringMs = 0.0;
        size_t drains = 0, forcedDrains = 0;
    };
    Profile profile() const {
        Profile p = prof_;
        p.meshMs = const_cast<ChunkMesher &>(mesher_).meshMs.load(std::memory_order_relaxed);
        p.meshed = const_cast<ChunkMesher &>(mesher_).meshCount.load(std::memory_order_relaxed);
        p.poolMb = double(vertPool_.bytes() + idxPool_.bytes() + scratchPool_.bytes() +
                          rawPool_.bytes()) /
                   (1024.0 * 1024.0);
        p.poolBuffers =
            vertPool_.count() + idxPool_.count() + scratchPool_.count() + rawPool_.count();
        p.pendingCompactions = pendingItems();
        return p;
    }
    void resetProfile() {
        prof_ = Profile{};
        mesher_.meshMs.store(0.0, std::memory_order_relaxed);
        mesher_.meshCount.store(0, std::memory_order_relaxed);
    }
    size_t poolBytes() const { return pool_.capacityUnits() * sizeof(uint16_t); }

    // -----------------------------------------------------------------------
    bool build(const ref<Device> &device, RenderContext *ctx) {
        device_ = device;
        ctx_ = ctx;

        // The pool has to hold the resident ring plus the models. A chunk over
        // this terrain meshes to roughly 200k triangles; the budget below is
        // that with a margin, and the pool grows if the ground turns out to be
        // rougher than that. Models are a rounding error beside it.
        const size_t ring = size_t(2 * std::max(1, viewChunks) + 1);
        pool_.init(device_, ring * ring * 260000 + (8u << 20));
        // The worker that re-meshes broken models -- see Remesher.
        remesh_.start();

        fence_ = device_->createFence();
        vertPool_.init(device_, ResourceBindFlags::ShaderResource, "v2::blasVerts");
        idxPool_.init(device_, ResourceBindFlags::ShaderResource, "v2::blasIndices");
        scratchPool_.init(device_, ResourceBindFlags::UnorderedAccess, "v2::blasScratch");
        rawPool_.init(device_, ResourceBindFlags::AccelerationStructure, "v2::blasRaw");

        // Templates first: the terrain's grass and soil colours are sampled
        // from the palette the models bring with them.
        if (!loadPines()) return false;
        // The ground's colours come from the PINES; everything after this line
        // is rock and flower, and neither is a soil.
        palette.markPinesLoaded();
        loadRocks();
        loadFlowers();
        loadMushrooms();
        loadPinecones();
        loadHives();
        palette.deriveGroundFromTrees();
        buildWater();

        uploadMaterials();

        mesher_.seed = seed;
        mesher_.treeDensity = treeDensity;
        mesher_.rockDensity = rockDensity;
        mesher_.flowerDensity = flowerDensity;
        mesher_.mushroomDensity = mushroomDensity;
        for (const ModelTemplate &t : pines_)
            mesher_.pineFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : rocks_)
            mesher_.rockFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : flowers_)
            mesher_.flowerFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : mushrooms_)
            mesher_.mushroomFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : pinecones_)
            mesher_.pineconeFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : hives_)
            mesher_.hiveFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : pines_) mesher_.pinePerch.push_back(t.perches);
        for (const ModelTemplate &t : pines_) mesher_.pineHivePerch.push_back(t.hivePerches);
        mesher_.pineconesPerTree = pineconesPerTree;
        mesher_.mushroomBig0 = mushroomBig0;

        const int hw = int(std::thread::hardware_concurrency());
        mesher_.start(terrain, meshThreads > 0 ? meshThreads : maxi(2, hw - 2));
        return true;
    }

    // -----------------------------------------------------------------------
    // Bring the resident ring in line with the camera, then take delivery of
    // however many finished chunks the budget allows.
    //
    // The budget is on STRUCTURE BUILDS, not on meshes taken: the build is the
    // part recorded on this thread and therefore the part that can stall a
    // frame. Returns true if the set changed and the launch needs a new
    // top-level handle.
    // -----------------------------------------------------------------------
    bool update(Vec3 camPos, int buildBudget = 2, bool rebuildTop = true) {
        const int cx = floorDiv(int(floorf(camPos.x / VOXEL_M)), CHUNK_VOX);
        const int cz = floorDiv(int(floorf(camPos.z / VOXEL_M)), CHUNK_VOX);

        // WHERE THE DEVICE HAS ACTUALLY GOT TO. Polled once, at the top, and
        // never waited on -- everything downstream that needs to know whether
        // some earlier frame's work has finished compares against this.
        if (fence_) {
            const uint64_t v = fence_->getCurrentValue();
            if (v > deviceDone_) deviceDone_ = v;
        }

        // IDLE MEANS: nothing arrived last frame and nothing is being meshed.
        // Measured before the drain, because the drain is what it gates -- and
        // taken from LAST frame's delivery rather than this one's, since this
        // frame's has not happened yet.
        streamIdle_ = (lastAdopted_ == 0 && mesher_.inFlight() == 0);

        bool changed = false;
        // Compacting a structure REPLACES it, so the top level has to be told:
        // the instance still describes the same geometry, but at a new address.
        if (drainCompactions(false)) changed = true;
        releaseBuildInputs();

        // Hand back pooled buffers nothing has wanted for a few hundred frames.
        // The prime asks for far more at once than flying ever does, and
        // without this its peak would stay resident for the whole session.
        // Aged on FRAMES, not on the fence: the fence only moves when a frame
        // records something, so a standing camera would never age anything out
        // -- which is precisely when the prime's peak has least reason to stay.
        ++frameNo_;
        if (frameNo_ > kPoolIdle) {
            const uint64_t cutoff = frameNo_ - kPoolIdle;
            vertPool_.trim(cutoff);
            idxPool_.trim(cutoff);
            scratchPool_.trim(cutoff);
            rawPool_.trim(cutoff);
        }
        if (!primed_ || cx != lastCx_ || cz != lastCz_) {
            lastCx_ = cx;
            lastCz_ = cz;
            primed_ = true;
            const auto tr = std::chrono::steady_clock::now();
            changed |= rering(cx, cz);
            prof_.reringMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr)
                    .count();
        }

        // Collect a batch first, then build them together. See adoptMany.
        const auto tk = std::chrono::steady_clock::now();
        std::vector<ChunkBuild> batch;
        ChunkBuild b;
        while (int(batch.size()) < buildBudget && mesher_.take(&b)) {
            requested_.erase(chunkKey(b.cx, b.cz));
            if (wanted_.count(chunkKey(b.cx, b.cz)) == 0) continue;  // evicted while queued
            batch.push_back(std::move(b));
        }
        prof_.takeMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tk).count();
        lastAdopted_ = batch.size();
        if (!batch.empty()) {
            adoptMany(std::move(batch));
            changed = true;
        }

        // Deferred while priming: the top-level structure is rebuilt WHOLE
        // every time it changes, so doing it once per batch of adopted chunks
        // meant hundreds of full rebuilds to reach the same state one rebuild
        // at the end would have produced.
        if (changed && rebuildTop) rebuildTlas();

        // Close the frame's recording and put a fence value behind it. This is
        // a submit and NOT a wait: it costs a driver call, and it is what makes
        // "has the device finished with this buffer" answerable next frame
        // without anybody stopping to ask.
        if (recorded_) {
            ctx_->submit(false);
            ctx_->signal(fence_.get(), epoch_);
            ++epoch_;
            recorded_ = false;
        }
        return changed;
    }

    // -----------------------------------------------------------------------
    // Everything solid within `reach` metres of a point.
    //
    // Gathered into a list once a tick rather than answered per test: the
    // player asks six times a tick, a chunk holds about forty solids, and at
    // that size any structure worth building costs more than the scan it saves.
    //
    // The CHUNKS looked at reach further than `reach` does, because a solid
    // belongs to the chunk holding its centre and the big rocks are four metres
    // across -- one centred just over the border can still be underfoot.
    // -----------------------------------------------------------------------
    // IS THE CHUNK UNDER THIS POINT ACTUALLY HERE? Asked by the spawn, which
    // otherwise checks itself against a world that has not been streamed.
    bool chunkAt(Vec3 p) const {
        const int cx = floorDiv(int(floorf(p.x / VOXEL_M)), CHUNK_VOX);
        const int cz = floorDiv(int(floorf(p.z / VOXEL_M)), CHUNK_VOX);
        return chunks_.find(chunkKey(cx, cz)) != chunks_.end();
    }

    // -----------------------------------------------------------------------
    // THE TOP OF A COLUMN AS IT IS NOW, which is not what the height field says.
    //
    // heightVox is the terrain as generated. The edit layer is the holes people
    // have dug in it, and a column whose top has been dug away holds nothing up
    // any more -- so the walk starts at the generated top and steps down past
    // every voxel the edits have turned to air.
    //
    // BOUNDED, because it is asked per column of a footprint and an unbounded
    // walk down a mine shaft would be paid on every one. Past the cap the
    // answer is "far below whatever is standing here", which is all any caller
    // needs to know.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE GROUND PATCH PhysX FALLS ONTO, WITH THE HOLES IN IT.
    //
    // The patch was sampled straight from heightVox, which is the terrain as
    // GENERATED -- so every hole the player has dug was invisible to the
    // solver. Two things came of that, and both were measured with
    // --float-test: a chip that rolls into a pit rests on the pit's phantom
    // lid, and a model whose ground has just been dug away is born INSIDE that
    // lid and is thrown upward out of it. The undermined pine climbed three
    // metres in five seconds instead of dropping into its own hole.
    //
    // So the patch asks the same question the renderer and the player's feet
    // ask. The edit set is fetched ONCE PER CHUNK rather than once per column:
    // EditStore::get takes a lock, and 25,600 of those per rebuild is a
    // different cost from 25,600 array reads.
    // -----------------------------------------------------------------------
    void groundPatch(int16_t *out, int n, int i0, int j0, int step) const {
        TerrainMemo memo;
        long long haveKey = 0;
        bool have = false;
        std::shared_ptr<const ChunkEdits> ce;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int wi = i0 + i * step, wj = j0 + j * step;
                const int h = terrain.heightVox(wi, wj, memo);
                const long long k = chunkKey(floorDiv(wi, CHUNK_VOX), floorDiv(wj, CHUNK_VOX));
                if (!have || k != haveKey) {
                    ce = mesher_.edits.get(floorDiv(wi, CHUNK_VOX), floorDiv(wj, CHUNK_VOX));
                    haveKey = k;
                    have = true;
                }
                int y = h;
                if (ce)
                    for (int d = 0; d < kUndermineDepthVox; ++d, --y) {
                        uint8_t m = mat::AIR;
                        if (!ce->voxel(wi, wj, y, &m) || m != mat::AIR) break;
                    }
                out[size_t(i) + size_t(j) * size_t(n)] = int16_t(y);
            }
    }

    // Has anything been dug since the patch was last built? Read-and-clear:
    // the floor the solver stands on has to be rebuilt when the ground under
    // it changes, and a dig is the only thing that changes it.
    bool takeGroundDirty() {
        const bool d = groundDirty_;
        groundDirty_ = false;
        return d;
    }

    // WHERE THE HOLES ARE, for anything that has to ask outside this class.
    //
    // The swing ray is the caller that made this necessary: it lives in
    // render/helditem.h, marches the ground itself, and was doing it against
    // the generator alone -- so it could not see a pit the player had just dug.
    // It takes this through WalkWorld now and reads it with TerrainProbe.
    //
    // CONST, AND THAT IS THE WHOLE CONTRACT. Carving goes through dig() so the
    // re-mesh and the physics rebuild cannot be skipped; this hands out the
    // right to LOOK and nothing else.
    const EditStore &editStore() const { return mesher_.edits; }

    // Is there ground at this voxel right now -- generated, then edited.
    // The one place anything asks that question, so nothing can disagree
    // about where the holes are.
    //
    // THE ANSWER MOVED to TerrainProbe (scene/voxelworld.h) when the swing ray
    // needed it too and could not reach into this class for it. This keeps the
    // signature its callers were written against and delegates, so there is
    // still one implementation and the sentence above is still true.
    bool terrainSolidAt(int i, int j, int y, TerrainMemo &memo) const {
        TerrainProbe probe(&terrain, &mesher_.edits, memo);
        return probe.solid(i, j, y);
    }

    int terrainTopAt(int i, int j, TerrainMemo &memo) const {
        const int h = terrain.heightVox(i, j, memo);
        const std::shared_ptr<const ChunkEdits> ce =
            mesher_.edits.get(floorDiv(i, CHUNK_VOX), floorDiv(j, CHUNK_VOX));
        if (!ce) return h;
        int y = h;
        for (int n = 0; n < kUndermineDepthVox; ++n, --y) {
            uint8_t m = mat::AIR;
            if (!ce->voxel(i, j, y, &m)) return y;   // no edit here: still stone
            if (m != mat::AIR) return y;            // filled back in
        }
        return y;
    }

    // -----------------------------------------------------------------------
    // ...AND WHETHER THIS PLACEMENT STILL HAS ANY OF IT UNDER IT.
    //
    // Every column the model's own row zero stands on, against the ground as it
    // is now. ONE column still holding it up is enough -- so the loop is an
    // early-out, and in the ordinary case where the player has dug a hole
    // beside a boulder rather than under it, it costs one lookup.
    //
    // The expensive path is the one where the answer is "nothing", and that is
    // the path where the model is about to become a rigid body anyway.
    // -----------------------------------------------------------------------
    bool standsOnNothing(const Solid &s) const {
        if (!s.vol || s.msx <= 0 || s.msz <= 0) return false;
        TerrainMemo memo;
        const float need = s.baseY - kUndermineTolM;
        for (int mz = 0; mz < s.msz; ++mz)
            for (int mx = 0; mx < s.msx; ++mx) {
                if (s.vol[size_t(mx) + size_t(mz) * size_t(s.msx)] == mat::AIR) continue;
                float wx = 0.0f, wz = 0.0f;
                solidWorldSpace(s, (float(mx) + 0.5f) * VOXEL_M, (float(mz) + 0.5f) * VOXEL_M, &wx,
                                &wz);
                const int top = terrainTopAt(int(std::floor(wx / VOXEL_M)),
                                             int(std::floor(wz / VOXEL_M)), memo);
                if (float(top + 1) * VOXEL_M >= need) return false;   // still standing on this
            }
        return true;
    }

    // -----------------------------------------------------------------------
    // DIG THE GROUND OUT FROM UNDER SOMETHING AND IT COMES DOWN.
    //
    // The sever fill answers what a carve left standing on nothing WITHIN one
    // model, and it seeds from that model's own bottom row -- which quietly
    // assumes there is ground under that row. Dig the ground away and the
    // assumption is false, and nothing anywhere asked: MEASURED with
    // float_probe, digging a metre out from under forty trees left forty of
    // them hanging, the worst by a full metre, and thirty of thirty-seven
    // boulders. The model's own rule cannot fire, because the dig never touched
    // its voxels.
    //
    // So a terrain dig asks the placements around it, and any that have nothing
    // left under them fall as one body -- the same path a felled tree takes,
    // with the sever step skipped because there is no stump to leave.
    //
    // ALL OF IT OR NONE. A half-undermined boulder should lean and topple, and
    // this does not do that: it waits until the last column under the model has
    // gone. That is the conservative direction -- a rock stands a little longer
    // than it might -- and toppling wants a contact model this does not have.
    // -----------------------------------------------------------------------
    bool dropUndermined(Physics &ph, const Vec3 &at, double nowMs) {
        if (!ph.available()) return false;
        collidersNear(at, kUndermineReachM, &underSolids_);
        bool any = false;
        for (const Solid &s : underSolids_) {
            if (s.decorSlot < 0 || s.modelKind < 0 || !s.vol) continue;
            if (fellSlots_.count({s.ownerChunk, int(s.decorSlot)})) continue;   // already down
            if (!standsOnNothing(s)) continue;
            if (fellTree(ph, s, Vec3{0.0f, 0.0f, 0.0f}, nowMs, true)) any = true;
        }
        return any;
    }

    void collidersNear(Vec3 p, float reach, std::vector<Solid> *out) const {
        out->clear();
        const float span = reach + 8.0f;
        const int x0 = floorDiv(int(floorf((p.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(floorf((p.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(floorf((p.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(floorf((p.z + span) / VOXEL_M)), CHUNK_VOX);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                auto it = chunks_.find(chunkKey(cx, cz));
                if (it == chunks_.end()) continue;
                for (const Solid &s : it->second.solids)
                    if (fabsf(s.cx - p.x) < reach + s.hx && fabsf(s.cz - p.z) < reach + s.hz)
                        out->push_back(s);
            }
    }

    // Block until the ring around the camera is fully resident. Used once at
    // startup so the first frame is not a hole in the ground.
    void primeBlocking(Vec3 camPos) {
        update(camPos, 0, false);
        while (mesher_.inFlight() > 0 || !requested_.empty()) {
            // No cap worth the name here and no top-level rebuild: nothing is
            // being displayed yet, so the only thing that matters is draining
            // the queue as fast as the workers can fill it. 48 at a time keeps
            // the uncompacted structures held simultaneously well inside video
            // memory.
            if (!update(camPos, 48, false))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Nothing may still be waiting to be compacted when the first frame is
        // drawn: those entries are holding an uncompacted structure each, and
        // the whole ring's worth would not fit.
        drainCompactions(true);
        rebuildTlas();
    }

  private:
    // -- what the streaming pipeline is made of --------------------------------
    // Declared here rather than beside the code that uses them, because the
    // member block below names them and a member declaration cannot look ahead.

    // One recorded build and everything it borrowed to get recorded.
    struct Staged {
        ref<Buffer> verts, idx, scratch, uncompacted;
        ref<RtAccelerationStructure> as;
        uint64_t uncompactedSize = 0;
        uint32_t query = 0;
    };

    // A structure that is built and in use, but not yet compacted.
    struct PendingCompact {
        Staged staged;
        long long key = 0;       // the chunk to hand the compacted structure to
        Blas *direct = nullptr;  // ...or, at load time, this Blas
        // ...or a held model, by index, REBUILT WHILE THE GAME RAN. See
        // recordHeldBuild. The generation is what makes that safe: the same
        // index can be rebuilt again before this compaction lands, and handing
        // a slot the compacted form of geometry it no longer has would put the
        // model back a version. A stale one is dropped instead.
        int held = -1;
        uint32_t heldGen = 0;
    };

    // A key no chunk can have, for a build whose compacted form nobody wants.
    static constexpr long long kNoOwner = (-9223372036854775807LL - 1);

    // One generation: the builds that share a query pool, and the fence value
    // by which all of them have run.
    struct CompactGroup {
        ref<RtAccelerationStructurePostBuildInfoPool> pool;
        std::vector<PendingCompact> items;
        uint64_t epoch = 0;    // highest epoch any item was recorded under
        uint64_t opened = 0;   // the epoch it started, so age can close it
        bool sealed = false;
        bool inputsFreed = false;  // vertices, indices and scratch handed back
    };

    // How many builds share a pool, and how many epochs an unfilled one may
    // stay open before it is sealed and becomes eligible to be read.
    static constexpr size_t kGroupSize = 16;
    static constexpr uint64_t kGroupAge = 6;

    // HOW MUCH UNCOMPACTED STRUCTURE IS WORTH HOLDING RATHER THAN STALLING FOR.
    //
    // Reading a compacted size costs a device drain -- Falcor's query pool
    // flushes on its first read and there is no way to ask it not to -- and a
    // drain waits for the frame currently on the GPU. Measured at about eleven
    // milliseconds a time, which is a whole frame, and doing it every eight
    // chunks was 3.7 seconds of a 5.6-second streaming cost on a long flight:
    // once the buffer allocations were fixed, THIS became the stutter.
    //
    // So compaction stopped being scheduled and became opportunistic. It
    // happens when the streamer has nothing else to do -- which in a game is
    // most of the time, because people stop and look at things -- and otherwise
    // only when the uncompacted structures have piled up this far. Sprinting in
    // a straight line for a minute is the case that pays, and it pays in memory
    // instead of in frames.
    static constexpr size_t kUncompactedBudget = size_t(320) << 20;

    // How many epochs a pooled buffer may sit unwanted before it is released.
    // An epoch is a frame that recorded something, so this is a few seconds of
    // walking -- long enough that crossing a chunk boundary never re-allocates,
    // short enough that standing still gives the prime's peak back.
    static constexpr uint64_t kPoolIdle = 240;

    ref<Device> device_;
    RenderContext *ctx_ = nullptr;
    ChunkMesher mesher_;
    TriPool pool_;
    Remesher remesh_;

    std::vector<ModelTemplate> pines_, rocks_, flowers_, mushrooms_, pinecones_;
    // The beehives. One model, and only the birch wood plants it -- see
    // loadHives and the hive pass in scene/chunks.h.
    std::vector<ModelTemplate> hives_;
    Blas waterBlas_;
    // The editor's floor -- see buildStage.
    static constexpr int kStageVox = 160;          // 16 m square
    static constexpr float kStageAtX = 4096.0f;    // well clear of the wood
    static constexpr float kStageAtY = 512.0f;
    static constexpr float kStageAtZ = 4096.0f;
    Blas stageBlas_;
    uint32_t stageTri_ = TriPool::kInvalid;
    bool stage_ = false;
    uint32_t waterTriOffset_ = TriPool::kInvalid;

    std::map<long long, Chunk> chunks_;

    // ONE ENTRY PER BROKEN INSTANCE, and none at all for a world nobody has
    // swung at. Keyed by the chunk that holds it and its decor slot inside
    // that chunk -- see Solid::decorSlot.
    // One loose thing: its own geometry, its own body, and the clock that
    // decides when it stops being the simulation's problem and becomes loot.
    struct Debris {
        Blas blas;
        uint32_t triOffset = TriPool::kInvalid;
        size_t tris = 0;           // pool units held, to hand back on retire
        int phys = -1;             // the PhysX handle, or -1
        float halfM[3] = {0, 0, 0};
        int voxels = 0;
        double bornMs = 0.0;
        bool live = false;
        bool absorbing = false;
        double absorbT0 = 0.0;
        Vec3 from{0, 0, 0};
        // WHERE THE MESH SITS RELATIVE TO THE BODY. The body's position is
        // the voxels' CENTRE OF MASS -- see spawnDebris -- and the mesh's own
        // origin is its corner, so this carries one to the other. Getting it
        // wrong is not subtle: the piece rotates about a point that is not its
        // middle, which reads as orbiting rather than tumbling.
        Vec3 originOff{0, 0, 0};
        double lastMs = 0.0;
        // IT IS A RIGID BODY. `phys` is a PxRigidDynamic -- a convex hull of
        // this piece's own voxels, with granite's density, friction and
        // inertia -- and `window` is the static it falls against: the stone
        // around it, WITH the hole it just made. Nothing here is integrated by
        // hand any more; pos and quat are read back off the solver each frame
        // and exist only to draw with.
        // GEOMETRY IT DOES NOT OWN. A felled tree is the tree that was already
        // standing there -- same model, same structure, same triangles -- so it
        // borrows them rather than meshing a quarter of a million triangles and
        // building a second structure over them on the frame the axe lands.
        // Borrowed from the TEMPLATE, which lives for the run: the damaged copy
        // of an instance goes when its chunk does, and this outlives that.
        uint64_t borrowAs = 0;
        uint32_t borrowTri = TriPool::kInvalid;
        float3 tint{1.0f, 1.0f, 1.0f};
        bool felled = false;
        // Which way the drawn shiver leans -- its own axis per slot, so two
        // chips off the same swing never rock together.
        Vec3 wobbleAxis{1, 0, 0};

        int window = -1;
        Vec3 winCentre{0, 0, 0};
        // WHAT THE STATIC WINDOW ACTUALLY COVERS, for a body too big for a
        // fixed cube. Rebuilt when the body leaves the inner box -- see
        // kStaticPadM.
        Vec3 winLo{0, 0, 0}, winHi{0, 0, 0};
        // THE BODY'S OWN COLLIDER, kept so "is any of it inside a rock" can be
        // asked of the shape the solver has rather than of a bounding box.
        // See debrisClip, and --fell-test, which prints it.
        std::vector<VoxBox> boxes;
        Vec3 pos{0, 0, 0};
        float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct Damaged {
        std::vector<uint8_t> vol;
        Blas blas;
        uint32_t triOffset = TriPool::kInvalid;
        // HOW MUCH OF THE TRIANGLE POOL THIS IS HOLDING, so the previous copy
        // can be given back before the next one is taken. Without it every blow
        // on a boulder leaked a whole re-meshed rock -- 151,000 triangles a
        // swing -- until the pool had to GROW, and growing does a blocking
        // submit and swaps the buffer the shader is reading from, in the middle
        // of a frame. That is the device removal.
        size_t tris = 0;
        // THE COLLIDER HAS TO BE BROKEN TOO. Solid::col is a bare pointer that
        // normally borrows the TEMPLATE's column heightfield -- fine while every
        // instance is identical, and wrong the moment one of them is not. This
        // is the damaged instance's own, and the Solid points here instead.
        // It must outlive the Solid, which is why it lives in the map and not
        // on the stack of the blow that made it.
        std::vector<int16_t> colTop;
        // WHICH EDIT THIS MESH IS OF. Bumped on every bite; a result that comes
        // back off the worker carrying an older one is a mesh of a rock that
        // has been hit again since, and is dropped.
        uint32_t seq = 0;
        uint32_t meshed = 0;
        // ...AND ITS OWN COLLIDER BOXES, so a rock with a bite out of it is a
        // rock with a bite out of it to everything falling past. Rebuilt when
        // `seq` moves past `boxSeq`.
        std::vector<VoxBox> boxes;
        uint32_t boxSeq = 0xffffffffu;
    };
    std::map<std::pair<long long, int>, Damaged> damaged_;

    // BELOW `Damaged`, AND THAT IS NOT A STYLE CHOICE. A member function's BODY
    // is compiled as though it came after the whole class, so it may name
    // anything the class declares; its PARAMETER TYPES are not, and these take
    // a `Damaged &`. Declared above the struct they do not parse, and the error
    // says "'d': undeclared identifier" pointing at the body.
    // Put a freshly built mesh onto an instance, whoever built it.
    void adoptMesh(Chunk &c, int slot, Damaged &d, const VoxMesh &mesh) {
        if (slot < 0 || size_t(slot) >= c.decorDesc.size()) return;
        if (mesh.triCount() == 0) {
            // BROKEN TO NOTHING. Not an error and not a special case: an empty
            // mask is how this engine already hides an instance, and building a
            // structure for no triangles is what would be the special case.
            c.decorDesc[size_t(slot)].instanceMask = 0;
            for (Solid &sl : c.solids)
                if (sl.decorSlot == slot) {
                    sl.hx = 0.0f;
                    sl.hz = 0.0f;
                    sl.col = nullptr;
                    sl.vol = nullptr;
                }
            return;
        }
        Blas nb = recordLooseBuild(mesh);   // NOT buildBlas: see the note there
        if (!nb.valid()) return;
        // The structure this instance was drawn with a moment ago is still
        // being read by whatever frame is in flight -- see retireLoose. It is
        // handed over to be freed later, NOT dropped here.
        retireLoose(std::move(d.blas), d.triOffset, d.tris);
        d.triOffset = pool_.upload(ctx_, mesh.tri);
        d.tris = mesh.tri.size();
        d.blas = std::move(nb);
        c.decorDesc[size_t(slot)].accelerationStructure = d.blas.as->getGpuAddress();
        c.decorInfo[size_t(slot)].triOffset = d.triOffset;
    }

    // ...AND THE ONES THE WORKER FINISHED. Once a frame, from updateDebris.
    void pumpRemesh() {
        Remesher::Done done;
        int adopted = 0;
        while (adopted < 2 && remesh_.take(&done)) {
            const auto dit = damaged_.find(done.key);
            if (dit == damaged_.end()) continue;   // the rock is gone
            Damaged &d = dit->second;
            // A MESH OF A ROCK THAT HAS BEEN HIT AGAIN SINCE. Dropped: another
            // job is already in flight for the newer state, and drawing this
            // one would put a hole back that a later blow had widened.
            if (done.seq != d.seq || done.seq <= d.meshed) continue;
            const auto ch = chunks_.find(done.key.first);
            if (ch == chunks_.end()) continue;   // the chunk was evicted
            d.meshed = done.seq;
            adoptMesh(ch->second, done.key.second, d, done.mesh);
            ++adopted;
            rebuildTlas();
        }
    }

    // Scratch for buildWindow. Members rather than locals: a blow is not the
    // frame to be allocating a thousand boxes and a solid list from scratch.
    std::vector<Solid> winSolids_;
    std::vector<Solid> underSolids_;
    bool groundDirty_ = false;
    // Scratch for dropTerrainHangers, kept so a blow allocates nothing.
    std::vector<uint8_t> hangSolid_, hangSeen_;
    std::vector<int> hangStack_;
    std::vector<const Solid *> winNear_;
    std::vector<VoxBox> winBoxes_;
    // Scratch for the sever test, which runs on every axe blow.
    mutable std::vector<uint8_t> sevSolid_, sevSeen_;
    // The two halves a fell makes. Members so cutting a tree does not allocate
    // two megabytes on the frame the axe lands.
    std::vector<uint8_t> fallVol_, stumpVol_;
    mutable std::vector<int> sevStack_;
    // What the last fill found loose: as cells, and as a fraction of the model.
    mutable int sevLooseCells_ = 0;
    mutable float sevLooseFrac_ = 0.0f;
    // Which placements are on the ground. Outlives the chunks they were in --
    // see reapplyDamage.
    std::set<std::pair<long long, int>> fellSlots_;
    // -----------------------------------------------------------------------
    // NOTHING LOOSE IS FREED WHILE THE DEVICE MIGHT STILL BE READING IT.
    //
    // A structure the top-level acceleration structure pointed at last frame is
    // still being read for as long as that frame is in flight. Dropping a Blas
    // the moment it is replaced destroys its buffer underneath the GPU, and the
    // device goes -- which surfaces later, at whatever API call happens next,
    // wearing a stack that has nothing to do with the cause.
    //
    // That is the same rule the buffer pools already keep with `freeAt >
    // deviceDone`; loose builds simply do not go through those pools, so they
    // need their own version of it. Stamped with the epoch they were retired
    // in and released once the device has passed it.
    // -----------------------------------------------------------------------
    struct RetiredLoose {
        Blas blas;
        uint32_t triOffset = TriPool::kInvalid;
        size_t tris = 0;
        uint64_t at = 0;
    };
    std::vector<RetiredLoose> retiredLoose_;

    Debris debris_[kDebrisInstances];
    int debrisBase_ = -1;        // first instance of the band, -1 while unbuilt
    bool debrisDirty_ = false;
    std::map<long long, int> wanted_;
    std::set<long long> requested_;

    ref<Buffer> materials_, instanceInfo_, instanceDescBuf_, tlasBuffer_, tlasScratch_;
    ref<RtAccelerationStructure> tlas_;
    std::vector<RtInstanceDesc> instanceDescs_;
    // EVERY tool that can be in the hand: one bottom-level structure each, its
    // slice of the triangle pool, and the grid it was meshed from. They share a
    // single top-level slot -- see setHeldInstance -- because only one of them
    // is ever held at a time, and an instance count that changed with the
    // selection would cost a full rebuild on every scroll of the wheel.
    struct HeldModel {
        Blas blas;
        uint32_t tri = TriPool::kInvalid;
        int sx = 0, sy = 0, sz = 0;
    };
    std::vector<HeldModel> held_;
    std::vector<size_t> heldTris_;  // pool units per model -- see replaceHeldVox
    std::vector<uint32_t> heldGen_;  // ...and which rebuild it is on
    int heldModel_ = -1;  // which of them the slot currently points at
    // EVERY FRAME OF EVERY COLOUR the flock can wear: six colours of eight, so
    // forty-eight structures of ten voxels each. They are separate structures
    // rather than one model posed, because that is what a voxel animation IS --
    // see render/butterflies.h on why the flap is on the grid and the turn is
    // not. Which one a slot points at is a 64-byte write, exactly as swapping
    // the tool in the hand is.
    // WHERE EVERY PLACEABLE INSTANCE WAS LAST FRAME, and whether it was on
    // screen at all. Only the dynamic prefix -- the hand, the shafts, the
    // dropped tools and the flock, which are pushed before the water and the
    // chunks and so keep their indices whatever the ring does. The forty-odd
    // thousand chunk and decor instances behind them never move, so the
    // static-world reprojection is already exact for every one of them and
    // there is nothing to remember. See place().
    std::vector<float3> wasAt_;
    std::vector<uint8_t> wasShown_;
    int dynEnd_ = 0;  // one past the last instance place() may be called for

    std::vector<HeldModel> flyers_;
    int flyerBase_ = -1;        // first instance of the band, -1 while unbuilt
    bool flyersDirty_ = false;  // anything written since the last flush
    int dropBase_ = -1;         // ...and the same pair for what has been put down
    bool dropsDirty_ = false;
    ref<Buffer> tlasUpdateScratch_;
    std::vector<V6Instance> instanceInfos_;

    size_t residentTris_ = 0;
    double buildMs_ = 0.0;
    Profile prof_;
    int lastCx_ = 0, lastCz_ = 0;
    bool primed_ = false;

    // -- the streaming pipeline's clock ---------------------------------------
    //
    // ONE FENCE, TICKED ONCE PER FRAME THAT RECORDS ANYTHING. `epoch_` is the
    // value this frame's work will signal; `deviceDone_` is the value the queue
    // has actually reached, polled and never waited on. Everything else in the
    // streamer -- when a pooled buffer is reusable, when a post-build query may
    // be read -- is a comparison between those two numbers.
    ref<Fence> fence_;
    uint64_t epoch_ = 1;
    uint64_t deviceDone_ = 0;
    uint64_t frameNo_ = 0;
    size_t lastAdopted_ = 0;
    bool streamIdle_ = false;
    bool recorded_ = false;  // did this frame put anything on the queue

    std::deque<CompactGroup> groups_;
    std::vector<ref<RtAccelerationStructurePostBuildInfoPool>> freePools_;
    TransientPool vertPool_, idxPool_, scratchPool_, rawPool_;

    // ------------------------------------------------------------ structures
    //
    // THE STREAMING HITCH LIVED HERE, and the shape of the fix is the shape of
    // what was actually wrong rather than what it looked like.
    //
    // The old path built a chunk's structure the way the textbook describes:
    // record the build, WAIT, read back how small the compacted form would be,
    // record the compact, WAIT again. Two full device drains, on the frame's
    // own thread, every time a chunk arrived. A drain is not merely a submit --
    // it waits for everything already queued, which includes the path trace and
    // the reconstruction of the frame being drawn. So taking delivery of a
    // chunk cost a whole frame of rendering, paid twice, in whichever frame was
    // unlucky enough to be holding the parcel.
    //
    // Measured on a flight across the wood: 4.0 s of a 9 s run inside this
    // function, a QUARTER of all frames over twice the median, and a p99 of
    // 38 ms against a 7 ms median. That is the stutter, exactly.
    //
    // Three things changed, and only the last is the one anybody expects:
    //
    //   1. THE BUFFERS ARE RECYCLED. 2.5 of those 4 seconds were in neither
    //      wait -- they were in CREATING four device buffers per chunk, which
    //      is four driver-side heap allocations of the same few sizes, twelve
    //      hundred times over. See TransientPool.
    //
    //   2. COMPACTION IS DEFERRED, not abandoned. It is worth 60% of the
    //      structure memory here -- a radius-12 ring is 2.9 GB compacted
    //      against 7.3 GB raw, and the second number does not fit on the card
    //      -- so it has to happen. Nothing forces it to happen in the frame the
    //      chunk arrives, though: the chunk enters the world immediately in its
    //      uncompacted form and is compacted a few frames later, when the
    //      answer is already sitting in memory and reading it costs nothing.
    //
    //   3. NOTHING WAITS. Readiness is a FENCE VALUE THAT GETS POLLED. Work
    //      recorded during a frame is stamped with the value the queue will
    //      reach once it has run, and anything stamped at or below what the
    //      device has actually reached is finished. That is what makes it safe
    //      both to recycle a buffer and to read a post-build query without
    //      draining anything, and it is the difference between asking the
    //      device a question and stopping to hear the answer.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Record one build. Nothing is waited on and nothing is read back.
    //
    // The geometry description is a local because the build is recorded before
    // this returns -- the API reads it during the call and not afterwards.
    // -----------------------------------------------------------------------
    // The generation currently being written. A pool is written once and read
    // once, so a full or elderly one is sealed and a fresh one opened.
    CompactGroup &openGroup() {
        if (!groups_.empty() && !groups_.back().sealed &&
            groups_.back().items.size() < kGroupSize)
            return groups_.back();
        if (!groups_.empty() && !groups_.back().sealed) groups_.back().sealed = true;

        CompactGroup g;
        g.pool = acquireQueryPool();
        g.opened = epoch_;
        groups_.push_back(std::move(g));
        return groups_.back();
    }

    // Pools are reset and reused rather than recreated. `reset` is what puts a
    // pool back into the write-once state its results depend on.
    ref<RtAccelerationStructurePostBuildInfoPool> acquireQueryPool() {
        if (!freePools_.empty()) {
            ref<RtAccelerationStructurePostBuildInfoPool> q = freePools_.back();
            freePools_.pop_back();
            q->reset(ctx_);
            return q;
        }
        RtAccelerationStructurePostBuildInfoPool::Desc qd;
        qd.queryType = RtAccelerationStructurePostBuildInfoQueryType::CompactedSize;
        qd.elementCount = uint32_t(kGroupSize);
        return RtAccelerationStructurePostBuildInfoPool::create(device_.get(), qd);
    }

    // `ownResult`: put the finished structure in a buffer of ITS OWN rather
    // than one from the recycling pool. See recordLooseBuild, which is the only
    // caller that needs it and explains why.
    Staged recordBuild(const VoxMesh &m, CompactGroup &group, bool ownResult = false) {
        Staged g;
        g.query = uint32_t(group.items.size());

        const size_t vbytes = m.position.size() * sizeof(Vec3);
        const size_t ibytes = m.index.size() * sizeof(uint32_t);
        g.verts = vertPool_.acquire(vbytes, deviceDone_);
        g.idx = idxPool_.acquire(ibytes, deviceDone_);
        ctx_->updateBuffer(g.verts.get(), m.position.data(), 0, vbytes);
        ctx_->updateBuffer(g.idx.get(), m.index.data(), 0, ibytes);
        // AND THE BARRIER BACK. updateBuffer leaves a buffer in CopyDest, and
        // buildAccelerationStructure adds no barriers of its own -- it is
        // handed raw device addresses and cannot know which resources they
        // belong to. Reading a CopyDest resource as a build input is undefined,
        // and on this driver it removes the device. Creating the buffer WITH
        // its data hid this, because that path leaves it in Common; filling a
        // recycled one by hand does not.
        ctx_->resourceBarrier(g.verts.get(), Falcor::Resource::State::NonPixelShader);
        ctx_->resourceBarrier(g.idx.get(), Falcor::Resource::State::NonPixelShader);

        RtGeometryDesc geom{};
        geom.type = RtGeometryType::Triangles;
        // OPAQUE is what lets the shader's Proceed() return without ever
        // re-entering traversal: there is no any-hit to run. Foliage is cut
        // out by its voxels, not by an alpha test, so nothing is lost.
        geom.flags = RtGeometryFlags::Opaque;
        geom.content.triangles.transform3x4 = 0;
        geom.content.triangles.vertexFormat = Falcor::ResourceFormat::RGB32Float;
        geom.content.triangles.indexFormat = Falcor::ResourceFormat::R32Uint;
        geom.content.triangles.vertexCount = uint32_t(m.position.size());
        geom.content.triangles.indexCount = uint32_t(m.index.size());
        geom.content.triangles.vertexData = g.verts->getGpuAddress();
        geom.content.triangles.indexData = g.idx->getGpuAddress();
        geom.content.triangles.vertexStride = sizeof(Vec3);

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::BottomLevel;
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace |
                       RtAccelerationStructureBuildFlags::AllowCompaction;
        inputs.descCount = 1;
        inputs.geometryDescs = &geom;

        const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), inputs);
        g.uncompactedSize = pre.resultDataMaxSize;
        g.scratch = scratchPool_.acquire(pre.scratchDataSize, deviceDone_);
        g.uncompacted =
            ownResult ? device_->createBuffer(pre.resultDataMaxSize,
                                              ResourceBindFlags::AccelerationStructure,
                                              Falcor::MemoryType::DeviceLocal)
                      : rawPool_.acquire(pre.resultDataMaxSize, deviceDone_);

        RtAccelerationStructure::Desc cd;
        cd.setKind(RtAccelerationStructureKind::BottomLevel);
        cd.setBuffer(g.uncompacted, 0, pre.resultDataMaxSize);
        g.as = RtAccelerationStructure::create(device_, cd);

        RtAccelerationStructure::BuildDesc bd = {};
        bd.inputs = inputs;
        bd.source = nullptr;
        bd.dest = g.as.get();
        bd.scratchData = g.scratch->getGpuAddress();

        RtAccelerationStructurePostBuildInfoDesc pbi = {};
        pbi.type = RtAccelerationStructurePostBuildInfoQueryType::CompactedSize;
        pbi.index = g.query;
        pbi.pool = group.pool.get();
        ctx_->buildAccelerationStructure(bd, 1, &pbi);
        recorded_ = true;
        return g;
    }

    // -----------------------------------------------------------------------
    // Turn a finished build into its compacted form and give it to its owner.
    //
    // The size is READ, not waited for: the caller has already established that
    // the device passed the build. The plausibility test is not paranoia about
    // that -- a driver is entitled to report a compacted size no smaller than
    // the original, and cloning is the right answer when it does.
    // -----------------------------------------------------------------------
    void finishCompact(PendingCompact &p, RtAccelerationStructurePostBuildInfoPool *pool) {
        Staged &g = p.staged;
        const uint64_t compacted = pool->getElement(ctx_, g.query);
        const uint64_t finalSize =
            (compacted > 0 && compacted < g.uncompactedSize) ? compacted : g.uncompactedSize;

        Blas b;
        b.buffer = device_->createBuffer(finalSize, ResourceBindFlags::AccelerationStructure,
                                         Falcor::MemoryType::DeviceLocal);
        b.buffer->setName("v2::blas");
        RtAccelerationStructure::Desc bdc;
        bdc.setKind(RtAccelerationStructureKind::BottomLevel);
        bdc.setBuffer(b.buffer, 0, finalSize);
        b.as = RtAccelerationStructure::create(device_, bdc);

        ctx_->copyAccelerationStructure(
            b.as.get(), g.as.get(),
            (finalSize == compacted) ? RenderContext::RtAccelerationStructureCopyMode::Compact
                                     : RenderContext::RtAccelerationStructureCopyMode::Clone);
        ctx_->uavBarrier(b.buffer.get());
        recorded_ = true;

        prof_.uncompactedMb += double(g.uncompactedSize) / (1024.0 * 1024.0);
        prof_.compactedMb += double(finalSize) / (1024.0 * 1024.0);

        if (p.direct) {
            *p.direct = std::move(b);
        } else if (p.held >= 0) {
            if (p.held < int(held_.size()) && p.held < int(heldGen_.size()) &&
                heldGen_[size_t(p.held)] == p.heldGen) {
                held_[size_t(p.held)].blas = std::move(b);
                // The instance record names a model index, not a structure, so
                // nothing here has to be told -- but the slot caches WHICH model
                // it last wrote, and the address it wrote has just changed under
                // it. Forgetting makes the next write re-state it.
                heldModel_ = -1;
            }
            // ...otherwise this is the compacted form of a model that has since
            // been rebuilt. Dropping it costs a buffer and a descriptor and
            // nothing else ever knew about it -- the same argument the chunk
            // branch below makes for an evicted chunk.
        } else {
            // The chunk may have been evicted while this was in flight, in
            // which case the compacted structure is simply dropped -- it is a
            // buffer and a descriptor, and nothing else ever knew about it.
            auto it = chunks_.find(p.key);
            if (it != chunks_.end()) it->second.blas = std::move(b);
        }
    }

    // THE BUILD INPUTS DIE BEFORE THE STRUCTURE DOES.
    //
    // Vertices, indices and scratch are read by the build and by nothing after
    // it, so they are free the moment the device has passed the build -- which
    // is several frames before the group is sealed and compacted. Holding them
    // until then was three quarters of what the pools were carrying, for no
    // reason beyond it being the tidier place to write the code.
    void releaseBuildInputs() {
        for (CompactGroup &g : groups_) {
            if (g.inputsFreed || g.epoch > deviceDone_) continue;
            for (PendingCompact &p : g.items) {
                vertPool_.release(p.staged.verts, epoch_, frameNo_);
                idxPool_.release(p.staged.idx, epoch_, frameNo_);
                scratchPool_.release(p.staged.scratch, epoch_, frameNo_);
                p.staged.verts = p.staged.idx = p.staged.scratch = nullptr;
            }
            g.inputsFreed = true;
        }
    }

    // What is left once a structure has been compacted: the uncompacted form it
    // was copied from, reusable once the device passes THIS frame -- the
    // compacting copy is still only recorded.
    void recyclePending(Staged &g) {
        vertPool_.release(g.verts, epoch_, frameNo_);
        idxPool_.release(g.idx, epoch_, frameNo_);
        scratchPool_.release(g.scratch, epoch_, frameNo_);
        rawPool_.release(g.uncompacted, epoch_, frameNo_);
        g.verts = g.idx = g.scratch = g.uncompacted = nullptr;
        g.as = nullptr;
    }

    // Draw a line under everything recorded so far and wait for it. The fence
    // is moved with it, so the pools and the pending list go on measuring
    // readiness the same way whether the line was drawn here or reached
    // naturally by frames going past.
    //
    // Load time and the memory valve only -- never while a frame is displayed.
    void syncPoint() {
        ctx_->submit(true);
        ctx_->signal(fence_.get(), epoch_);
        deviceDone_ = epoch_;
        ++epoch_;
        recorded_ = false;
    }

    // -----------------------------------------------------------------------
    // Compact whatever the device has finished building.
    //
    // `force` is the memory valve and the load-time path: it drains the device
    // first, which makes every outstanding build finished by definition. It is
    // never taken while a frame is being displayed.
    // -----------------------------------------------------------------------
    // The uncompacted structures currently being held, in bytes.
    size_t uncompactedBytes() const {
        size_t n = 0;
        for (const CompactGroup &g : groups_)
            for (const PendingCompact &p : g.items) n += size_t(p.staged.uncompactedSize);
        return n;
    }

    bool drainCompactions(bool force) {
        if (groups_.empty()) return false;
        const auto td = std::chrono::steady_clock::now();
        struct Timer {
            World *w;
            std::chrono::steady_clock::time_point t;
            ~Timer() {
                w->prof_.drainMs +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
                        .count();
            }
        } timer{this, td};
        ++prof_.drains;
        if (force) ++prof_.forcedDrains;
        // An open generation is sealed once it has aged, so a trickle of chunks
        // still gets compacted rather than sitting uncompacted indefinitely.
        if (!groups_.back().sealed && (force || epoch_ - groups_.back().opened >= kGroupAge))
            groups_.back().sealed = true;

        // The gate. Reading a size drains the device, so it waits for a moment
        // when that costs nothing -- the streamer being idle -- unless the
        // structures being held have grown past what the memory is worth.
        if (!force && !streamIdle_ && uncompactedBytes() < kUncompactedBudget) return false;
        if (force) syncPoint();  // every outstanding build has now certainly run

        bool any = false;
        while (!groups_.empty()) {
            CompactGroup &grp = groups_.front();
            if (!grp.sealed || grp.epoch > deviceDone_) break;
            for (PendingCompact &p : grp.items) {
                finishCompact(p, grp.pool.get());
                recyclePending(p.staged);
            }
            freePools_.push_back(grp.pool);
            groups_.pop_front();
            any = true;
        }
        return any;
    }

    size_t pendingItems() const {
        size_t n = 0;
        for (const CompactGroup &g : groups_) n += g.items.size();
        return n;
    }

    // -----------------------------------------------------------------------
    // Record a batch of chunk builds. Returns immediately with the UNCOMPACTED
    // structures, which are perfectly good structures -- they are simply bigger
    // than they need to be, and that is a problem for three frames from now.
    // -----------------------------------------------------------------------
    Blas recordChunkBuild(const VoxMesh &m, long long key) {
        const auto t0 = std::chrono::steady_clock::now();
        CompactGroup &grp = openGroup();
        Staged g = recordBuild(m, grp);

        Blas raw;
        raw.buffer = g.uncompacted;
        raw.as = g.as;

        PendingCompact p;
        p.staged = std::move(g);
        p.key = key;
        grp.epoch = epoch_;
        grp.items.push_back(std::move(p));

        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        buildMs_ += ms;
        prof_.blasMs += ms;
        ++prof_.blasCalls;
        return raw;
    }

  public:
    // -----------------------------------------------------------------------
    // THE SWELL, ONE CHUNK AT A TIME.
    //
    // A wave step re-meshes every water chunk, and the meshing is not the cost:
    // the heights never move, so a re-mesh reuses the cached ColumnStack and
    // only recomputes the crests -- two sines a column, about 0.04 ms a brick.
    // What is expensive is the STRUCTURE. Measured at load, 655 ms to build 625
    // chunks, so **1.05 ms per chunk BLAS**, and it is main-thread work.
    //
    // THAT IS WHY THIS IS STAGGERED RATHER THAN SYNCHRONOUS. A 5,000 m2 lake
    // covers about eight chunks; re-meshing all of them on one tick is 8.4 ms
    // of BLAS in a single frame -- half a 60 fps budget, arriving ten times a
    // second as a visible hitch. Spread over frames at chunksPerTick each, the
    // same work costs about a millisecond a frame and the whole lake still
    // completes a step at waveHz.
    //
    // The swell then travels across a big lake instead of the entire surface
    // snapping between two shapes, which is both cheaper and more like water.
    //
    // NOT A NEW PATH. A wave tick is exactly a re-request, the same call an
    // edit makes -- so it inherits the worker pool, the adoption batching, the
    // compaction queue and every guard those already have.
    // -----------------------------------------------------------------------
    // ----------------------------------------------------------------
    // OFF, AND THE REASON IS NOT THE COST.
    //
    // This works and is cheap enough -- 8.17 ms of worker mesh and 1.05 ms of
    // main-thread BLAS a chunk, about 6% of the main thread staggered at one
    // chunk a frame. It is off because of how it LOOKS.
    //
    // A crest is 0..waveVoxMax voxels, so at 10 cm a column has three
    // possible heights. Moving the phase does not animate the lake, it cuts
    // between still frames of it, and no update rate changes that -- the
    // states are discrete. Worse, staggering across chunks means neighbours
    // hold different phases, so the surface tears at every chunk seam.
    //
    // The continuous motion lives in the shading normal instead (see the
    // water branch in Trace.cs.slang), which is free and cannot tear. The
    // geometry's job is the silhouette, and a static crest does that just as
    // well as a moving one.
    //
    // Kept rather than deleted: if the voxels ever get small enough, or an
    // amplitude of eight-plus voxels is wanted, this is the machinery and it
    // is measured.
    // ----------------------------------------------------------------
    float waveHz = 9.7f;      // v4 derived this from the wave spectrum
    int chunksPerTick = 1;    // how much BLAS to spend on the swell per frame
    bool wavesAnimate = false;

    void tickWaves(float dt) {
        if (!wavesAnimate || mesher_.waveVoxMaxIsZero()) return;
        waveClock_ += dt;
        mesher_.waveTime.store(waveClock_, std::memory_order_relaxed);

        // Collect the wet chunks once per cycle rather than every frame: the
        // set only changes when the ring does.
        if (waveRing_.empty() || waveCursor_ >= waveRing_.size()) {
            waveRing_.clear();
            for (const auto &kv : chunks_)
                if (kv.second.hasWater) waveRing_.push_back({kv.second.cx, kv.second.cz});
            waveCursor_ = 0;
            if (waveRing_.empty()) return;
        }
        for (int n = 0; n < chunksPerTick && waveCursor_ < waveRing_.size(); ++n) {
            const auto cc = waveRing_[waveCursor_++];
            const long long k = chunkKey(cc.first, cc.second);
            if (!chunks_.count(k)) continue;
            if (requested_.count(k)) continue;  // already in flight
            requested_.insert(k);
            mesher_.request(cc.first, cc.second);
        }
    }

    size_t waterChunks() const {
        size_t n = 0;
        for (const auto &kv : chunks_) if (kv.second.hasWater) ++n;
        return n;
    }

  private:
    float waveClock_ = 0.0f;
    std::vector<std::pair<int, int>> waveRing_;
    size_t waveCursor_ = 0;

    // -----------------------------------------------------------------------
    // A held model's structure, RECORDED RATHER THAN BUILT, so it is legal
    // inside a frame.
    //
    // This is recordChunkBuild with a different destination, and it exists
    // because buildBlas below is not usable here and says so: it is the load
    // path, it forces a compaction drain and two blocking submits, and the note
    // over drainCompactions is explicit that the forced path "is never taken
    // while a frame is being displayed". Calling it from a running frame while
    // the streamer also had work in flight took the device out -- present
    // returned E_FAIL after about a hundred rebuilds.
    //
    // What comes back is the UNCOMPACTED structure, which is a perfectly good
    // structure and is exactly what a chunk is handed for its first few frames.
    // The compacted one replaces it whenever the group it belongs to is drained,
    // by the ordinary unforced path, on a frame that can afford it.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // A STRUCTURE FOR SOMETHING THAT CAME LOOSE, BUILT DURING A FRAME.
    //
    // buildBlas below CANNOT be called here and says so: it forces a compaction
    // drain and two blocking submits, and doing that from a running frame while
    // the streamer had work in flight took the device out -- present returned
    // E_FAIL after about a hundred rebuilds. A chip off a rock happens several
    // times a second, so that path was a crash with a fuse on it.
    //
    // This records exactly as a chunk does and hands back the UNCOMPACTED
    // structure, which is a perfectly good structure -- it is what every chunk
    // draws with for its first few frames. The compaction still happens on a
    // frame that can afford it, and its result is DROPPED: the item carries no
    // owner, so the drain finds no chunk for it and releases it, which that
    // code already treats as ordinary. The cost is a little memory for a body
    // that is about to be absorbed anyway.
    // -----------------------------------------------------------------------
    Blas recordLooseBuild(const VoxMesh &m) {
        CompactGroup &grp = openGroup();
        // ITS OWN BUFFER, AND THAT IS THE WHOLE POINT.
        //
        // recordBuild normally takes the structure's buffer from rawPool_, and
        // the drain hands it BACK to that pool once the device is done with it
        // -- which is correct for a chunk or a held model, because compaction
        // replaces their Blas with the compacted one before that happens.
        //
        // A loose build keeps the UNCOMPACTED structure and drops the compacted
        // copy, so nothing ever replaces it. Taking a pooled buffer meant the
        // pool reissued it to the next chunk build and wrote a different
        // structure over the top of a rock that was still being drawn: the
        // boulder vanished, and reading it took the device out. It showed up as
        // "I was moving while hitting the rock" because moving is what makes
        // the streamer build chunks, which is what consumes the pool.
        Staged g = recordBuild(m, grp, /*ownResult=*/true);
        Blas raw;
        raw.buffer = g.uncompacted;
        raw.as = g.as;
        PendingCompact p;
        p.staged = std::move(g);
        p.key = kNoOwner;   // nothing will claim the compacted copy
        grp.epoch = epoch_;
        grp.items.push_back(std::move(p));
        return raw;
    }

    Blas recordHeldBuild(const VoxMesh &m, int index) {
        CompactGroup &grp = openGroup();
        Staged g = recordBuild(m, grp);
        Blas raw;
        raw.buffer = g.uncompacted;
        raw.as = g.as;

        PendingCompact p;
        p.staged = std::move(g);
        p.held = index;
        p.heldGen = heldGen_[size_t(index)];
        grp.epoch = epoch_;
        grp.items.push_back(std::move(p));
        return raw;
    }

    // -----------------------------------------------------------------------
    // One structure, synchronously. The models and the water only.
    //
    // Load time has no frame to protect, so this takes the simple road: record,
    // drain, compact. It goes through the same recording and compaction code as
    // a chunk so there is one build path and not two that can drift apart.
    // -----------------------------------------------------------------------
    Blas buildBlas(const VoxMesh &m) {
        Blas result;
        CompactGroup &grp = openGroup();
        PendingCompact p;
        p.staged = recordBuild(m, grp);
        p.direct = &result;
        grp.epoch = epoch_;
        grp.items.push_back(std::move(p));
        drainCompactions(true);
        // The compacting copy is only RECORDED by the drain, and the buffers it
        // reads have already gone back to the pools. A second sync point is
        // what makes their release honest now rather than at whatever frame
        // boundary happens to come next.
        syncPoint();
        return result;
    }

    // ------------------------------------------------------------ templates
    void loadModelSet(const std::vector<std::string> &paths, std::vector<ModelTemplate> *out,
                      bool multiModel, bool quiet, uint32_t mossSeed = 0,
                      bool perches = false, int upscale = 0,
                      // Every colour these models actually use, for the caller
                      // that wants to paint something else in the same stone.
                      std::vector<std::array<uint8_t, 4>> *colourSink = nullptr) {
        for (const std::string &path : paths) {
            std::vector<VoxModel> models;
            std::string err;
            if (multiModel) {
                if (!voxLoadAll(path, &models, &err)) {
                    if (!quiet) std::fprintf(stderr, "v2: %s\n", err.c_str());
                    continue;
                }
            } else {
                VoxModel mo;
                if (!voxLoad(path, &mo, &err)) {
                    if (!quiet) std::fprintf(stderr, "v2: %s\n", err.c_str());
                    continue;
                }
                models.push_back(std::move(mo));
            }

            for (const VoxModel &mo : models) {
                VoxAsset a = toWorld(mo, 0, mo.sx);
                if (a.sx <= 0) continue;
                // A COUNT, NOT A FLAG. Each pass is 2x on a side, so 8x the
                // voxels and about 4x the surface a mesher has to emit. One
                // pass is the large mushrooms and the mid stones; two is the
                // big five, which are meant to read as landmarks rather than
                // as rocks you walk past.
                for (int u = 0; u < upscale; ++u) a = upscale2x(a);

                // ONLY THE ENTRIES THE MODEL USES. Registering all 255 of a
                // file's palette floods the shared table, and past 255
                // forModelColor returns AIR and real voxels stop being drawn.
                std::vector<uint8_t> idOfEntry(256, mat::AIR);
                std::vector<bool> used(256, false);
                for (uint8_t v : a.a) used[v] = true;
                for (int e = 1; e <= 255; ++e)
                    if (used[e]) idOfEntry[e] = palette.forModelColor(mo.pal[e - 1]);

                // The moss goes into the ASSET, before anything reads it, so
                // the mesh and the collider below cannot disagree about where
                // the rock's surface is.
                growMoss(&a, &idOfEntry, mossSeed);
                // BEFORE the moss, in spirit: only entries the file itself
                // used, so an unused palette slot cannot tint the ramp.
                if (colourSink)
                    for (int e = 1; e <= 255; ++e)
                        if (idOfEntry[size_t(e)] != mat::AIR)
                            colourSink->push_back(mo.pal[size_t(e) - 1]);

                VoxMesh mesh = meshAsset(a, idOfEntry, VOXEL_M);
                if (mesh.triCount() == 0) continue;

                ModelTemplate t;
                if (perches) {
                    t.perches = collectPerches(a, idOfEntry);
                    // 5 for the 5 x 5 x 5 beehive. Only the birches carry them,
                    // and they are the models loaded at or past birchBase --
                    // out_ is being appended to, so its CURRENT size is the
                    // index this model is about to take.
                    if (int(out->size()) >= mesher_.birchBase && &pines_ == out)
                        t.hivePerches = collectPerches(a, idOfEntry, 5);
                }
                t.colTop = columnTops(a, idOfEntry);
                // ALWAYS, for every object. `a` goes out of scope with this
                // loop and is the only place the model's voxels exist.
                t.volume.assign(a.a.size(), mat::AIR);
                for (size_t k = 0; k < a.a.size(); ++k)
                    t.volume[k] = idOfEntry[size_t(a.a[k])];
                t.sx = a.sx;
                t.sy = a.sy;
                t.sz = a.sz;
                // Here and not later: `a` is the only place the model's voxels
                // exist on the host, and it goes out of scope with this loop.
                t.col = measureCollider(a, VOXEL_M, kBodyHeightM);
                t.tris = mesh.triCount();
                t.triOffset = pool_.upload(ctx_, mesh.tri);
                t.blas = buildBlas(mesh);
                uniqueTris += mesh.triCount();
                out->push_back(std::move(t));
            }
        }
    }

    // ------------------------------------------------------------------------
    // THE TREES, WHICHEVER WOOD THIS IS.
    //
    // Birches load into pines_ and that is not a shortcut -- it is the point.
    // Everything downstream of this vector is model agnostic: the scatter picks
    // an index, the collider is measured off the asset, the perch finder walks
    // the voxels looking for somewhere a thing could hang. None of it knows
    // what species it is holding, so a second vector would need a second copy
    // of all of it to say the same thing twice.
    //
    // perches=true for both. In a pine those anchors carry cones; in a birch
    // they carry the beehives -- same question asked of the crown ("a solid
    // voxel with air under it"), different thing hung from the answer.
    // ------------------------------------------------------------------------
    bool loadPines() {
        // ONE VECTOR, TWO RANGES. Pines occupy [0, birchBase) and birches
        // [birchBase, size) -- so the scatter picks a SPECIES by choosing which
        // range to index, and everything downstream stays exactly as it was.
        // templateFor, the footprints, the colliders and the perch lists are
        // all positional into this one array and none of them needs to know a
        // species exists.
        //
        // Both are loaded whenever the bands are live, because a chunk near a
        // seam contains both. --birch and --pine pin the world to one, and then
        // only that one is worth the load time and the triangles.
        std::vector<std::string> paths;
        const bool wantPine = !terrain.forced || terrain.biome == Biome::Pine;
        const bool wantBirch = !terrain.forced || terrain.biome == Biome::Birch;
        if (wantPine)
            for (int i = 1; i <= 9; ++i)
                paths.push_back(pineDir + "/pine_" + std::to_string(i) + ".vox");
        mesher_.birchBase = wantPine ? 9 : 0;
        if (wantBirch)
            // 1.vox .. 16.vox, authored at 10 cm like everything else -- 18.2 m
            // to 30.5 m, the tallest a 100 ft tree exactly. The set was scaled by
            // ONE factor so the saplings stayed saplings in proportion, which makes
            // the spread wide: the shortest birch is barely half the tallest and
            // stands well under the pines, every one of which is 30.5 m.
            //
            // Anything past 25.5 m is TWO STACKED MODELS in one file, that being
            // the tallest a .vox coordinate byte can address -- see the scene-graph
            // note at the top of scene/vox.h, and tools/revoxel_trees_tall.py,
            // which writes them.
            for (int i = 1; i <= 16; ++i)
                paths.push_back(birchDir + "/" + std::to_string(i) + ".vox");
        loadModelSet(paths, &pines_, false, false, 0u, /*perches=*/true);
        if (pines_.empty()) {
            std::fprintf(stderr, "v2: no pine models loaded from %s -- pass --pines\n",
                         pineDir.c_str());
            return false;
        }
        loadedPines = int(pines_.size());
        return true;
    }

    // The 26 rocks, each its own file. The names are listed rather than
    // globbed: a fixed list means a missing file is a warning about that file
    // rather than a scene that silently has fewer rocks in it than it should.
    void loadRocks() {
        // THE ORDER OF THESE TWO LISTS IS LOAD-BEARING. decorSink in
        // scene/collide.h decides how deep a rock sits from its INDEX -- big
        // below kRockBigEnd, mid below kRockBigMidEnd -- so the big five have
        // to land at 0..4 and the mid six at 5..10. loadModelSet appends, so
        // two calls in this order give exactly that.
        static const char *kBigNames[] = {
            "BIG_1_BiG_0", "Big_2_BiG_0", "Big_3_BiG_0", "Big_4_BiG_0", "Big_5_BiG_0"};
        // THE MID SIX ARE THEIR OWN LIST NOW, purely so they can be loaded at a
        // different scale from the ones after them. The ORDER and the POSITION
        // are unchanged and must stay that way: decorSink reads the index, so
        // mid still has to occupy 5..10 with Runic_1 landing at 11.
        static const char *kMidNames[] = {"Mid_1_MID_0",     "Mid_2_MID_0", "Mid_3_MID_0",
                                          "Mid_4_MID_0",     "Mid_4_MID_0_001", "Mid_5_MID_0"};
        static const char *kRestNames[] = {
            "Runic_1_Runic_0", "Runic_2_Runic_0",
            "Runic_3_Runic_0", "Runic_4_Runic_0", "Runic_5_Runic_0", "Runic_6_Runic_0",
            "Runic_7_Runic_0", "Small_1_SMall_0", "Small_2_SMall_0", "Small_3_SMall_0",
            "Small_4_SMall_0", "Small_5_SMall_0", "Small_6_SMall_0", "Small_7_SMall_0",
            "Small_8_SMall_0"};

        std::vector<std::string> big, mid, rest;
        for (const char *n : kBigNames) big.push_back(decorDir + "/rocks/" + n + ".vox");
        for (const char *n : kMidNames) mid.push_back(decorDir + "/rocks/" + n + ".vox");
        for (const char *n : kRestNames) rest.push_back(decorDir + "/rocks/" + n + ".vox");

        // NOTHING IS UPSCALED HERE ANY MORE, and that is the point.
        //
        // Big and mid used to be grown with upscale2x -- 4x and 2x on a side --
        // which is not the same thing as making them bigger, however much it
        // looks like it. That call replaces each voxel with a block of copies
        // of itself: the mesh stays at 10 cm, but the SHAPE still only has the
        // detail the source had, so at 4x every surface feature was 40 cm and
        // the boulders were visibly built from blocks four times the size of
        // the terrain they stood on.
        //
        // They are now revoxelised from the sculpt instead, by
        // tools/revoxel_rocks_scaled.py, which samples the SAME mesh on a
        // finer grid and ships the result. The detail was always in the .glb;
        // it was the .vox that was too coarse to carry it.
        //
        //     big  122-228 voxels tall  (12.2-22.8 m)  from a 4x sampling
        //     mid   57- 77              ( 5.7- 7.7 m)  from a 2x sampling
        //     rest   7- 23              ( 0.7- 2.3 m)  untouched
        //
        // Two of the big five are sampled slightly under 4x -- Big_2 at 3.42
        // and Big_5 at 3.78 -- because a .vox XYZI record packs each coordinate
        // in a single byte and no axis may exceed 255. That is a limit of the
        // file format, and the tool clamps to it rather than silently wrapping.
        //
        // The only model set that grows moss -- see mossFace in voxelworld.h.
        // THE STONE THE GROUND IS MADE OF COMES FROM THESE FILES. Terrain rock
        // used to be one flat grey beside boulders carrying real stone tones,
        // so every hole dug into a hillside looked like a slab. The ramp is
        // filled from the same palettes the rocks are drawn with -- see
        // Palette::setStoneBand and mat::STONE_0.
        std::vector<std::array<uint8_t, 4>> rockCols;
        loadModelSet(big, &rocks_, false, false, 0x4D055EEDu, false, 0, &rockCols);
        loadModelSet(mid, &rocks_, false, false, 0x4D055EEDu, false, 0, &rockCols);
        loadModelSet(rest, &rocks_, false, false, 0x4D055EEDu, false, 0, &rockCols);
        palette.setStoneBand(rockCols);
        std::printf("v2: stone ramp from %zu rock colours (%d usable)\n",
                    rockCols.size(), palette.stoneSampleCount());
        loadedRocks = int(rocks_.size());
    }

    void loadFlowers() {
        loadModelSet({decorDir + "/flowers.vox"}, &flowers_, true, false);
        loadedFlowers = int(flowers_.size());
    }

    // TWO SETS FROM ONE FILE: the models as authored, then the same models
    // revoxelised at 2x. The small ones come first and mushroomBig0 is the
    // boundary, so the scatter can weight the draw between them.
    void loadMushrooms() {
        loadModelSet({decorDir + "/mushroom.vox"}, &mushrooms_, true, false);
        mushroomBig0 = int(mushrooms_.size());
        loadModelSet({decorDir + "/mushroom.vox"}, &mushrooms_, true, false, 0u, false,
                     /*upscale=*/1);
        loadedMushrooms = int(mushrooms_.size());
    }

    // The beehive. Birch only -- see the hive pass in scene/chunks.h, which
    // hangs one in a hundredth of the trees.
    void loadHives() {
        if (!terrain.birch()) return;
        loadModelSet({decorDir + "/beehive.vox"}, &hives_, false, true);
    }

    void loadPinecones() {
        loadModelSet({decorDir + "/pinecone.vox"}, &pinecones_, true, false);
        loadedPinecones = int(pinecones_.size());
    }

    // -----------------------------------------------------------------------
    // THE ASSET EDITOR'S STAGE -- a white floor under an empty sky.
    //
    // Built once, on the first U, and kept: it is a fixed object with no
    // streaming behind it, which is the whole point of the editor. The wood is
    // not unloaded when you step onto it -- the ring keeps its chunks resident
    // so that coming back is instant -- it is simply not put in the structure.
    // See rebuildTlas.
    //
    // MADE OF 10 cm VOXELS, like everything else here, and that is not
    // decoration: the editor exists to look at models at the size they will be
    // in the world, so its floor has to be the same lattice they sit on. A
    // quad with a grid texture would have been a tenth of the triangles and a
    // lie about scale.
    //
    // THE GRIDLINES ARE VOXELS TOO, one metre apart -- every tenth column, in
    // both directions. A metre is the unit a model gets judged in ("is that
    // bird too big for a branch"), and ten voxels is what a metre IS here, so
    // the line falls on the lattice rather than across it.
    // -----------------------------------------------------------------------
    void buildStage() {
        if (stageBlas_.valid()) return;

        // Its own two colours, allocated the way every model's are. White for
        // the deck and a mid grey for the rules -- dark enough to read against
        // white under a bright sky, light enough not to look like a hole.
        const uint8_t white = palette.forModelColor({255, 255, 255, 255}, false);
        const uint8_t grey = palette.forModelColor({120, 120, 120, 255}, false);
        uploadMaterials();

        VoxAsset a;
        a.sx = kStageVox;
        a.sz = kStageVox;
        a.sy = 1;  // a deck, not a block: one voxel thick
        a.a.assign(size_t(kStageVox) * size_t(kStageVox), 1);
        for (int z = 0; z < kStageVox; ++z)
            for (int x = 0; x < kStageVox; ++x)
                if (x % 10 == 0 || z % 10 == 0)
                    a.a[size_t(x) + size_t(z) * size_t(kStageVox)] = 2;

        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        idOfEntry[1] = white;
        idOfEntry[2] = grey;
        const VoxMesh mesh = meshAsset(a, idOfEntry, VOXEL_M);
        if (mesh.triCount() == 0) return;
        stageTri_ = pool_.upload(ctx_, mesh.tri);
        stageBlas_ = buildBlas(mesh);
        std::printf("  stage    %d x %d voxels (%.1f m square), %zu tris\n", kStageVox, kStageVox,
                    float(kStageVox) * VOXEL_M, mesh.triCount());
    }


    void buildWater() {
        // ---------------------------------------------------------------
        // THERE IS NO GLOBAL WATER PLANE ANY MORE, and removing it is not a
        // tidy-up -- leaving it in would put an ocean over the world.
        //
        // This built ONE flat quad 16 km across at the waterline. That was
        // survivable only while the line sat at 2.6 m, below every piece of
        // ground anywhere, where the quad was buried and nobody saw it. The
        // line is 33 m in the pine wood now, so the same quad would cut
        // through the hills and roof the birch wood -- which has no water at
        // all -- in a sheet of it.
        //
        // Water is mat::WATER on the terrain itself now: it exists exactly
        // where lakeColumn says a lake stands, and nowhere else. A plane
        // cannot express that, which is the whole reason the design moved.
        //
        // The function and its BLAS are kept rather than deleted so the
        // instance push below has something to test, and so KIND_WATER in
        // Shared.slang stays where it is -- its value is written into instance
        // data host-side, and renumbering it shifts KIND_TREE and everything
        // after it, silently.
        // ---------------------------------------------------------------
        if (terrain.anyWater()) return;  // voxel water: no plane
        VoxMesh wm;
        const float s = 8000.0f;
        const float y = terrain.pineWater;
        wm.addQuad({-s, y, -s}, {-s, y, s}, {s, y, s}, {s, y, -s}, mat::AIR, face::POS_Y);
        waterTriOffset_ = pool_.upload(ctx_, wm.tri);
        waterBlas_ = buildBlas(wm);
    }

    // -------------------------------------------------------------- the ring
    bool rering(int cx, int cz) {
        const int R = maxi(1, viewChunks);
        wanted_.clear();
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) wanted_[chunkKey(cx + i, cz + j)] = 1;

        bool changed = false;
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            if (wanted_.count(it->first) == 0) {
                pool_.release(it->second.triOffset, it->second.tris);
                residentTris_ -= it->second.tris;
                it = chunks_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }

        // Nearest first: the chunk you are standing on matters more than the
        // one at the edge of the ring, and at speed you may never reach that.
        std::vector<std::pair<int, std::pair<int, int>>> order;
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) {
                const long long k = chunkKey(cx + i, cz + j);
                if (chunks_.count(k) || requested_.count(k)) continue;
                order.push_back({i * i + j * j, {cx + i, cz + j}});
            }
        std::sort(order.begin(), order.end(),
                  [](const std::pair<int, std::pair<int, int>> &a,
                     const std::pair<int, std::pair<int, int>> &b) { return a.first < b.first; });
        for (const auto &o : order) {
            requested_.insert(chunkKey(o.second.first, o.second.second));
            mesher_.request(o.second.first, o.second.second);
        }
        return changed;
    }

    // NOTHING IS COPIED HERE ANY MORE. This used to build a vector of MESHES
    // to hand to the batch builder -- about seven megabytes a chunk of
    // positions and indices, memcpy'd on the frame's own thread purely to pass
    // them to something that reads them once and never keeps them.
    void adoptMany(std::vector<ChunkBuild> &&batch) {
        for (size_t k = 0; k < batch.size(); ++k) {
            ChunkBuild &b = batch[k];
            const long long key = chunkKey(b.cx, b.cz);
            Chunk c;
            c.cx = b.cx;
            c.cz = b.cz;
            c.tris = b.mesh.triCount();
            c.hasWater = b.hasWater;
            // A CHUNK OF PURE AIR IS A REAL CHUNK. It has no faces, so there is
            // no structure to build and nothing to upload -- but it is resident,
            // and it has to say so or the streamer waits for it forever. That is
            // exactly what emptying the world exposed: every chunk came back
            // with no triangles and residency never completed.
            //
            // Legitimate long before an empty world, too: fly high enough over
            // any terrain and the chunks above it are air.
            if (c.tris > 0) {
                c.blas = recordChunkBuild(b.mesh, key);
                const auto tp = std::chrono::steady_clock::now();
                c.triOffset = pool_.upload(ctx_, b.mesh.tri);
                prof_.poolMs +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp)
                        .count();
            }
            ++prof_.adopted;

            for (const Placement &p : b.decor) {
                // Flowers are knee-high and are walked through, so they are
                // drawn and not collided with. Trees and rocks are both.
                Solid s;
                V6Instance info{};
                if (p.kind >= 0 && p.kind < 6) ++c.decorKind[p.kind];
                // A HIVE IS NOT WALKED INTO EITHER. It hangs several metres up
                // in a crown, so a ground collider for it would be an invisible
                // wall under the tree.
                const bool walkThrough = (p.kind == 2 || p.kind == 4 || p.kind == 5);
                // The slot this instance is about to take, recorded on its
                // Solid so a blow can find its way back here. solids is a
                // FILTERED subset of decorDesc, so its own index is no use.
                s.ownerChunk = key;
                s.decorSlot = int32_t(c.decorDesc.size());
                c.decorDesc.push_back(makeInstance(p, &info, walkThrough ? nullptr : &s));
                c.decorInfo.push_back(info);
                c.decorAt.push_back(DecorAt{uint8_t(p.kind), c.decorDesc.back().transform[0][3],
                                            c.decorDesc.back().transform[1][3],
                                            c.decorDesc.back().transform[2][3]});
                if (!walkThrough && s.hx > 0.0f) c.solids.push_back(s);
            }

            // DAMAGE OUTLIVES THE CHUNK IT IS IN.
            //
            // A chunk that comes back from the mesher is built from the
            // TEMPLATES, so every boulder in it is pristine again -- and a
            // chunk comes back for all sorts of reasons that have nothing to do
            // with the player: the streamer finishing a build that was
            // requested a while ago, a dig, a re-ring. The voxels the player
            // broke are still in damaged_, so the hole reappeared on the next
            // blow and vanished again on the next adopt, which is a boulder
            // that will not stay broken.
            //
            // The decor slots are positional and the scatter is deterministic,
            // so a slot means the same instance across a rebuild. That is what
            // makes this a re-point rather than a re-search.
            reapplyDamage(c, key);

            residentTris_ += c.tris;
            // REPLACE, NOT INSERT. emplace() keeps the value already at a key,
            // so a chunk that came back because it was DUG would have been
            // built, uploaded and then silently thrown away -- the hole would
            // never appear and nothing would report an error. The old chunk is
            // destroyed here, which releases its structure exactly as eviction
            // does, and the new one draws from the next frame.
            chunks_.insert_or_assign(key, std::move(c));

            // THE VALVE, and it is checked HERE -- with the chunk already in
            // the world -- because a drain hands the compacted structure to
            // whichever chunk owns it, and a chunk that is not there yet gets
            // its structure thrown away and its buffer recycled underneath it.
            //
            // A MEMORY bound, not a scheduling one. It is reached by the
            // opening prime, where blocking is free, and by sustained flight in
            // one direction, where it is the price of not stalling sooner.
            if (uncompactedBytes() >= kUncompactedBudget) drainCompactions(true);
        }
    }

    const ModelTemplate &templateFor(int kind, int index) const {
        const std::vector<ModelTemplate> &v =
            (kind == 0)   ? pines_
            : (kind == 1) ? rocks_
            : (kind == 2) ? flowers_
            : (kind == 3) ? mushrooms_
            : (kind == 5) ? hives_
                          : pinecones_;
        return v[size_t(index) % v.size()];
    }

    // The instance the renderer draws, the record the shader reads, and --
    // through `solidOut` -- the collider the player walks into. ONE function on
    // purpose: the collider is read back out of the transform this just wrote,
    // so there is no second copy of the placement arithmetic that could drift
    // away from the first.
    RtInstanceDesc makeInstance(const Placement &p, V6Instance *info, Solid *solidOut) {
        static const float kRot[4][9] = {
            { 1, 0, 0,  0, 1, 0,  0, 0, 1},
            { 0, 0, 1,  0, 1, 0, -1, 0, 0},
            {-1, 0, 0,  0, 1, 0,  0, 0,-1},
            { 0, 0,-1,  0, 1, 0,  1, 0, 0},
        };
        const ModelTemplate &t = templateFor(p.kind, p.index);
        const float *m = kRot[p.yaw & 3];
        const int fx = (p.yaw & 1) ? t.sz : t.sx;
        const int fz = (p.yaw & 1) ? t.sx : t.sz;

        // A tree standing exactly on the surface looks like it is on tiptoe, so
        // it is sunk half a metre. Rocks are sunk by category rather than by
        // height -- the big and mid boulders a metre and a half, everything
        // else half a metre -- so that a boulder reads as embedded in the
        // ground rather than set down on it. See decorSink in scene/collide.h,
        // which the scatter shares so the two cannot disagree.
        const int sink = decorSink(p.kind, p.index, t.sy, seed, p.cell);

        // SNAPPED TO THE VOXEL GRID, and it is not cosmetic.
        //
        // halfOf is half the model's width, so a model an ODD number of voxels
        // across lands on a HALF voxel -- Big_1 is 199 wide and does exactly
        // that. Its voxels then straddle the world's voxel cells, and the
        // device picks a voxel's shade by hashing floor(p / kVoxelM): one face
        // of one voxel spans two cells, draws two different greens, and the
        // moss comes out cut in half along the triangle diagonal.
        //
        // It shows on moss and not on stone because only the hashed families --
        // grass, soil, litter -- vary per cell; rock is one id and returns it.
        // The shift is at most five centimetres and nothing else can see it.
        const float tx = std::round((float(p.ci) * VOXEL_M - halfOf(fx)) / VOXEL_M) * VOXEL_M;
        const float tz = std::round((float(p.cj) * VOXEL_M - halfOf(fz)) / VOXEL_M) * VOXEL_M;
        // yOff is ZERO for everything that stands on the ground and is the
        // whole story for a pinecone, which does not: it is the height of the
        // branch the cone was perched on, measured from the tree's own base, so
        // the cone rides the tree rather than the terrain under it.
        // extraSink is what the SCATTER measured for this particular site --
        // how much further down this model has to go before its underside stops
        // showing daylight over the ground it spans. Zero on flat ground, and
        // zero for a pinecone, which is hung on a branch and never touches the
        // terrain at all. See Placement in scene/chunks.h for why it is carried
        // here rather than recomputed: this function has no height field, and
        // the collider below is derived from this same transform, so measuring
        // it once is also what stops the two disagreeing.
        const float ty = float(p.h + 1 - sink - p.extraSink + p.yOff) * VOXEL_M;

        const float cx = float(t.sx) * VOXEL_M * 0.5f, cz = float(t.sz) * VOXEL_M * 0.5f;
        RtInstanceDesc inst = {};
        writeTransform(inst, m, tx + halfOf(fx) - (m[0] * cx + m[2] * cz), ty,
                       tz + halfOf(fz) - (m[6] * cx + m[8] * cz));

        if (solidOut && t.col.solid()) {
            // The collider's centre in the model's own frame, put through the
            // transform above -- rows 0 and 2 of it, since a quarter turn about
            // Y leaves the height alone.
            const float qx = cx + t.col.cx, qz = cz + t.col.cz;
            solidOut->cx = inst.transform[0][0] * qx + inst.transform[0][2] * qz + inst.transform[0][3];
            solidOut->cz = inst.transform[2][0] * qx + inst.transform[2][2] * qz + inst.transform[2][3];
            // A quarter turn swaps the extents rather than rotating them, which
            // is the whole reason the placement is restricted to quarter turns.
            solidOut->hx = (p.yaw & 1) ? t.col.hz : t.col.hx;
            solidOut->hz = (p.yaw & 1) ? t.col.hx : t.col.hz;
            solidOut->top = inst.transform[1][3] + t.col.top;
            // Rocks and mushrooms are floors; a trunk is a wall whose top is a
            // canopy twenty metres up and must never be stood on.
            solidOut->standable = (p.kind == 1 || p.kind == 3);
            solidOut->modelKind = int16_t(p.kind);
            solidOut->modelIndex = int16_t(p.index);
            solidOut->bouncy = (p.kind == 3);

            // The voxel-accurate surface. `col` is borrowed from the template,
            // which lives for the run; the three numbers beside it are the
            // instance transform this function just wrote, kept so the collider
            // can invert it rather than reconstructing the placement from
            // scratch and drifting away from it.
            if (!t.colTop.empty()) {
                solidOut->col = t.colTop.data();
                solidOut->msx = int16_t(t.sx);
                solidOut->msz = int16_t(t.sz);
                solidOut->yaw = uint8_t(p.yaw & 3);
                solidOut->tx = inst.transform[0][3];
                solidOut->tz = inst.transform[2][3];
                solidOut->baseY = inst.transform[1][3];
            }
            // ...AND AT THE VOXELS THEMSELVES, which is what a blow, an arrow
            // and a body all actually ask. Borrowed exactly as colTop is: the
            // templates are built once at load and never touched, and a damaged
            // instance is re-pointed at its own copy -- see refitSolid.
            if (!t.volume.empty()) {
                solidOut->vol = t.volume.data();
                solidOut->msx = int16_t(t.sx);
                solidOut->msz = int16_t(t.sz);
                solidOut->vsy = int16_t(t.sy);
                solidOut->yaw = uint8_t(p.yaw & 3);
                solidOut->tx = inst.transform[0][3];
                solidOut->tz = inst.transform[2][3];
                solidOut->baseY = inst.transform[1][3];
            }
        }

        info->triOffset = t.triOffset;
        info->kind = (p.kind == 0) ? KIND_TREE : KIND_TERRAIN;
        info->tint = (p.kind == 0) ? tintFor(p.cell) : float3(1.0f, 1.0f, 1.0f);
        // A tree does not move, so the motion vector's static-world formula is
        // exactly right for it and there is nothing to subtract -- and nothing
        // on it flaps, so there is nothing to add either.
        info->prevOffset = float3(0.0f, 0.0f, 0.0f);
        info->flap = float3(0.0f, 0.0f, 0.0f);

        inst.instanceMask = kMaskWorld;
        inst.instanceContributionToHitGroupIndex = 0;
        inst.flags = RtGeometryInstanceFlags::None;
        inst.accelerationStructure = t.blas.as->getGpuAddress();
        // instanceID is filled in by rebuildTlas, because it is the position in
        // the instance array and that is only known once the array is laid out.
        inst.instanceID = 0;
        return inst;
    }

    // -----------------------------------------------------------------------
    // RE-MEASURE A BROKEN INSTANCE'S COLLIDER, so what you can walk into
    // matches what you can see.
    //
    // measureCollider and columnTops both work off nothing but VoxAsset::at and
    // the three dimensions, and a damaged volume has exactly that shape -- so
    // they are REUSED over a view of it rather than reimplemented. A second
    // copy of this measurement would be free to disagree with the one every
    // undamaged rock in the world is using.
    // -----------------------------------------------------------------------
    void refitSolid(Chunk &c, const Solid &so, const ModelTemplate &t, Damaged &d) {
        VoxAsset view;
        view.sx = t.sx;
        view.sy = t.sy;
        view.sz = t.sz;
        // BORROWED, NOT COPIED. A big boulder is nine megabytes and this runs on
        // every blow; swapping it in and back out again costs three pointers.
        view.a.swap(d.vol);
        static const std::vector<uint8_t> kIdentity = [] {
            std::vector<uint8_t> v(256);
            for (int i = 0; i < 256; ++i) v[size_t(i)] = uint8_t(i);
            return v;
        }();
        d.colTop = columnTops(view, kIdentity);
        const ModelCollider mc = measureCollider(view, VOXEL_M, kBodyHeightM);
        view.a.swap(d.vol);   // and back, before anything can observe the gap
        if (mc.hx <= 0.0f || mc.hz <= 0.0f) { dropSolid(c, so); return; }

        // Rows 0 and 2 of kRot, forward this time -- solidModelSpace is the
        // transpose of exactly this.
        static const float R[4][4] = {
            { 1.0f,  0.0f,  0.0f,  1.0f},
            { 0.0f,  1.0f, -1.0f,  0.0f},
            {-1.0f,  0.0f,  0.0f, -1.0f},
            { 0.0f, -1.0f,  1.0f,  0.0f},
        };
        const float *r = R[so.yaw & 3];
        const float qx = float(t.sx) * VOXEL_M * 0.5f + mc.cx;
        const float qz = float(t.sz) * VOXEL_M * 0.5f + mc.cz;
        for (Solid &sl : c.solids) {
            if (sl.decorSlot != so.decorSlot) continue;
            sl.cx = r[0] * qx + r[1] * qz + so.tx;
            sl.cz = r[2] * qx + r[3] * qz + so.tz;
            sl.hx = (so.yaw & 1) ? mc.hz : mc.hx;
            sl.hz = (so.yaw & 1) ? mc.hx : mc.hz;
            sl.top = so.baseY + mc.top;
            sl.col = d.colTop.data();   // ITS OWN, not the template's
            sl.vol = d.vol.data();      // ...and its own voxels, with the hole
            sl.vsy = int16_t(t.sy);
        }
    }

    // Nothing left to walk into.
    void dropSolid(Chunk &c, const Solid &so) {
        for (Solid &sl : c.solids)
            if (sl.decorSlot == so.decorSlot) {
                sl.hx = 0.0f;
                sl.hz = 0.0f;
                sl.col = nullptr;
                sl.vol = nullptr;
            }
    }

    // A few percent of per-tree hue. Nine models over thousands of trees would
    // otherwise show their repeat.
    float3 tintFor(uint32_t cell) const {
        const float t = hashUnit(seed + 19u, cell);
        const float v = 0.90f + 0.20f * hashUnit(seed + 20u, cell);
        return float3(lerpf(0.94f, 1.06f, t) * v, 1.0f * v, lerpf(1.05f, 0.92f, t) * v);
    }

    // -----------------------------------------------------------------------
    // PLACE AN INSTANCE -- AND DERIVE ITS MOTION VECTOR FROM HAVING DONE SO.
    //
    // THIS EXISTS BECAUSE THE OTHER WAY ROUND DOES NOT WORK. Every moving thing
    // in this scene used to hand its own motion in: the flock passed p - prev,
    // and the shafts and the dropped tools passed nothing at all, because
    // nobody remembered they had to. That is not a bug those two subsystems
    // had, it is a bug the API had -- a parameter you can leave out is a
    // parameter that will be left out, and what you get for leaving it out is
    // an object reprojected as though it were nailed to the world. An arrow at
    // forty-eight metres a second was, and it smeared the length of the shot.
    //
    // So it is not asked for any more. The instance's transform IS where it is,
    // this array still holds where it WAS -- nothing has overwritten it yet --
    // and the difference of those two is the motion vector, every time, for
    // everything that comes through here. There is no longer a way to place a
    // moving object and forget to say that it moved.
    //
    // THE CENTRE, NOT THE CORNER. A voxel mesh runs from its own corner, so the
    // translation handed in is the object's centre less its half-box carried
    // through the turn -- and differencing THAT would fold the turn into the
    // vector: a butterfly banking on the spot would be reported as having
    // travelled. Carrying the half-box back through the same matrix recovers
    // the centre, which is the point the object actually turns about. The
    // half-box is in the MODEL'S OWN units, because the matrix is what converts
    // them -- voxels for a held-item mesh, metres for a flyer's. Each caller
    // has already computed exactly this to place the thing at all.
    //
    // WHAT IT STILL DOES NOT COVER, and neither did the hand-written version:
    // the turn itself. A point off the centre moves by (M - Mprev) * its offset
    // as well, which no single translation can express -- see the measurements
    // in render/arrows.h and render/butterflies.h for why that term is small
    // against the travel in every case this scene has. Nor does it cover a
    // model whose GEOMETRY changed under a fixed transform; that is the flap,
    // and it has its own vector -- see V6Instance::flap.
    //
    // A SLOT THAT WAS HIDDEN LAST FRAME GETS NOTHING, and that is the right
    // answer rather than a guard: an arrow slot recycled for a new shot, or a
    // butterfly that has just materialised, was not on screen last frame at
    // all. It has no history to describe, and telling Ray Reconstruction it
    // flew in from wherever the last tenant died would be worse than telling it
    // nothing.
    // -----------------------------------------------------------------------
    void place(size_t idx, const float *m, float tx, float ty, float tz, uint32_t mask, bool show,
               float hx, float hy, float hz, bool track = true) {
        RtInstanceDesc &inst = instanceDescs_[idx];
        writeTransform(inst, m, tx, ty, tz);
        inst.instanceMask = show ? mask : 0;
        inst.instanceID = uint32_t(idx);

        const float3 at(tx + m[0] * hx + m[1] * hy + m[2] * hz,
                        ty + m[3] * hx + m[4] * hy + m[5] * hz,
                        tz + m[6] * hx + m[7] * hy + m[8] * hz);
        const bool had = track && show && idx < wasShown_.size() && wasShown_[idx] != 0;
        instanceInfos_[idx].prevOffset = had ? (at - wasAt_[idx]) : float3(0.0f, 0.0f, 0.0f);
        if (idx < wasShown_.size()) {
            wasAt_[idx] = at;
            wasShown_[idx] = (track && show) ? uint8_t(1) : uint8_t(0);
        }
    }

    static void writeTransform(RtInstanceDesc &inst, const float *m, float tx, float ty, float tz) {
        inst.transform[0][0] = m[0]; inst.transform[0][1] = m[1]; inst.transform[0][2] = m[2];
        inst.transform[0][3] = tx;
        inst.transform[1][0] = m[3]; inst.transform[1][1] = m[4]; inst.transform[1][2] = m[5];
        inst.transform[1][3] = ty;
        inst.transform[2][0] = m[6]; inst.transform[2][1] = m[7]; inst.transform[2][2] = m[8];
        inst.transform[2][3] = tz;
    }

    static float halfOf(int voxels) { return float(voxels) * VOXEL_M * 0.5f; }

    // ------------------------------------------------------------------ TLAS
    void rebuildTlas() {
        const auto tt0 = std::chrono::steady_clock::now();
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        instanceDescs_.clear();
        instanceInfos_.clear();

        auto push = [&](RtInstanceDesc d, V6Instance info) {
            d.instanceID = uint32_t(instanceInfos_.size());
            instanceDescs_.push_back(d);
            instanceInfos_.push_back(info);
        };

        // -- THE HELD ITEM IS INSTANCE ZERO, ALWAYS ------------------------
        //
        // First, before the water, and present on every rebuild from the moment
        // the model loads -- whether or not anything is in the hand. Two things
        // depend on that and both are in setHeldInstance: its descriptor is at
        // a FIXED offset, so moving it is a 64-byte write rather than a 2.7 MB
        // one; and the instance COUNT never changes, which is what an update is
        // allowed to assume. A slot that came and went would force a full
        // rebuild on every draw and put away.
        //
        // The transform is identity here and is overwritten before the first
        // trace that could see it. The mask is 0 so it cannot be hit in the
        // meantime.
        if (!held_.empty()) {
            const size_t m = size_t(heldModel_ >= 0 && heldModel_ < int(held_.size()) ? heldModel_
                                                                                      : 0);
            RtInstanceDesc held = {};
            writeTransform(held, kI, 0.0f, 0.0f, 0.0f);
            held.instanceMask = 0;
            held.accelerationStructure = held_[m].blas.as->getGpuAddress();
            V6Instance info{};
            info.triOffset = held_[m].tri;
            info.kind = KIND_HELD;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(held, info);

            // -- AND THE SHAFTS, RESERVED ----------------------------------
            //
            // Every arrow slot, present from this build on and masked off until
            // something is actually in it. The count is what an update is not
            // allowed to change, so a pool that grew with the shooting would
            // force a full rebuild on every loose and every landing -- the two
            // moments in the frame least able to afford one.
            //
            // NOT KIND_HELD: a shaft in the air is an ordinary object in the
            // world. It is lit like one and it casts like one, and it is not
            // bolted to the camera, so it wants neither the tool's second ray
            // mask nor the tool's zeroed motion vector.
            //
            // KIND_FLYER, AND IT WAS KIND_TERRAIN, WHICH WAS WRONG TWICE. The
            // note that stood here said the static-world motion vector was
            // "exactly right for it, because it really does move" -- which is
            // the argument backwards. That formula IS the static world: it
            // reprojects this frame's hit point through last frame's camera and
            // so claims the surface was always where it is now. True of a tree.
            // False of a shaft crossing the screen at forty-eight metres a
            // second, which Ray Reconstruction was therefore told had never
            // moved -- and it ghosted exactly as hard as that lie is big.
            //
            // The second thing the kind buys is the one the drops below spell
            // out: an arrow is a HELD model, meshed at one unit per voxel, so
            // its instance carries VOXEL_M as a scale and its face normals come
            // out a tenth of unit length without KIND_FLYER's normalise.
            for (int i = 0; i < kArrowInstances; ++i) {
                RtInstanceDesc arrow = {};
                writeTransform(arrow, kI, 0.0f, 0.0f, 0.0f);
                arrow.instanceMask = 0;
                arrow.accelerationStructure = held_[m].blas.as->getGpuAddress();
                V6Instance ai{};
                ai.triOffset = held_[m].tri;
                ai.kind = KIND_FLYER;
                ai.tint = float3(1.0f, 1.0f, 1.0f);
                push(arrow, ai);
            }
        }

        // -- AND WHAT HAS BEEN DROPPED ------------------------------------
        //
        // KIND_FLYER, and not because it flies. That kind means "a transform
        // with a uniform scale in it, and it may have moved" -- which is
        // exactly a dropped item: it is a HELD model, meshed at one unit per
        // voxel, so its instance carries VOXEL_M as a scale and its normals
        // come out that factor short without the normalise KIND_FLYER buys.
        dropBase_ = -1;
        if (!held_.empty()) {
            dropBase_ = int(instanceDescs_.size());
            for (int i = 0; i < kDropInstances; ++i) {
                RtInstanceDesc dd = {};
                writeTransform(dd, kI, 0.0f, 0.0f, 0.0f);
                dd.instanceMask = 0;
                dd.accelerationStructure = held_[0].blas.as->getGpuAddress();
                V6Instance di{};
                di.triOffset = held_[0].tri;
                di.kind = KIND_FLYER;
                di.tint = float3(1.0f, 1.0f, 1.0f);
                push(dd, di);
            }
            dropsDirty_ = true;
        }

        // -- AND THE FLOCK, RESERVED THE SAME WAY -------------------------
        //
        // AFTER the hand and its shafts and BEFORE the water, so the band is
        // contiguous and its base is the only thing setFlyerInstance has to
        // know. The base is recorded rather than computed because it depends on
        // whether anything is in the hand at all -- a `--noaxe` run puts the
        // first butterfly at instance zero -- and the two loads can happen in
        // either order.
        flyerBase_ = -1;
        if (!flyers_.empty()) {
            flyerBase_ = int(instanceDescs_.size());
            for (int i = 0; i < kFlyerInstances; ++i) {
                RtInstanceDesc fly = {};
                writeTransform(fly, kI, 0.0f, 0.0f, 0.0f);
                fly.instanceMask = 0;
                fly.accelerationStructure = flyers_[0].blas.as->getGpuAddress();
                V6Instance fi{};
                fi.triOffset = flyers_[0].tri;
                fi.kind = KIND_FLYER;
                fi.tint = float3(1.0f, 1.0f, 1.0f);
                push(fly, fi);
            }
            // Whatever the flock had written into the old band is gone with it,
            // so the next frame's publish has to go up whether or not a
            // butterfly moved. Without this a rebuild -- which is to say every
            // time the ring steps -- would leave the whole flock parked at the
            // origin, masked off, until something happened to touch it.
            flyersDirty_ = true;
        }

        // ---- and the loose band, on the same terms -----------------------
        //
        // A SLOT THAT ALWAYS EXISTS IS A REFIT. Debris appears and vanishes
        // constantly -- every swing makes some -- and growing the instance
        // array for each one would mean rebuilding the top-level structure
        // several times a second. So the band is reserved whole, masked off,
        // and each slot is filled in place.
        //
        // The placeholder structure is any real one: a slot with mask 0 is
        // never traversed, but the address still has to be valid for the build.
        debrisBase_ = -1;
        {
            const Blas *anyBlas = nullptr;
            if (!rocks_.empty() && rocks_[0].blas.valid()) anyBlas = &rocks_[0].blas;
            else if (!pines_.empty() && pines_[0].blas.valid()) anyBlas = &pines_[0].blas;
            else if (!flyers_.empty() && flyers_[0].blas.valid()) anyBlas = &flyers_[0].blas;
            if (anyBlas) {
                debrisBase_ = int(instanceDescs_.size());
                for (int i = 0; i < kDebrisInstances; ++i) {
                    RtInstanceDesc dd = {};
                    writeTransform(dd, kI, 0.0f, 0.0f, 0.0f);
                    dd.instanceMask = 0;
                    dd.accelerationStructure = anyBlas->as->getGpuAddress();
                    V6Instance di{};
                    di.triOffset = 0;
                    di.kind = KIND_TERRAIN;
                    di.tint = float3(1.0f, 1.0f, 1.0f);
                    push(dd, di);
                }
                // Whatever the old band held went with it, so the next frame
                // has to publish whether or not anything moved.
                debrisDirty_ = true;
            }
        }

        // THE DYNAMIC PREFIX ENDS HERE, and what follows it -- the water, the
        // chunks, the decor -- is placed once and never again. Kept ACROSS a
        // rebuild when the layout is unchanged, which it is on every ring step:
        // the bands above are a fixed size and sit at the front, so a butterfly
        // in slot 40 is instance 40 before and after. Only a late model load
        // can move them, and that is what the size test catches -- everything
        // then starts again with no history, which is true, because the
        // instances it would have described are not the same instances.
        dynEnd_ = int(instanceDescs_.size());
        if (wasAt_.size() != size_t(dynEnd_)) {
            wasAt_.assign(size_t(dynEnd_), float3(0.0f, 0.0f, 0.0f));
            wasShown_.assign(size_t(dynEnd_), uint8_t(0));
        }

        // -- THE EDITOR REPLACES THE WORLD, IT DOES NOT HIDE IT ---------------
        //
        // On the stage the wood is not in the structure at all -- no water, no
        // chunks, no decor -- so the editor is genuinely a separate place
        // rather than the wood with things switched off. A ray that misses the
        // deck hits the sky, which is why the atmosphere is all that is left
        // behind it.
        //
        // The dynamic bands ABOVE this line stay, and that is deliberate: they
        // are what carries the models you came here to look at, and their fixed
        // layout is what lets an update work at all.
        //
        // The chunks stay RESIDENT while you are here. Nothing is evicted and
        // nothing re-streams, so U back into the wood is one rebuild and no
        // wait -- it costs their memory for as long as the editor is open,
        // which is the right trade for a key you press to check a model.
        if (stage_) {
            if (stageBlas_.valid()) {
                RtInstanceDesc deck = {};
                writeTransform(deck, kI, kStageAtX, kStageAtY, kStageAtZ);
                deck.instanceMask = kMaskWorld;
                deck.accelerationStructure = stageBlas_.as->getGpuAddress();
                V6Instance info{};
                info.triOffset = stageTri_;
                info.kind = KIND_TERRAIN;
                info.tint = float3(1.0f, 1.0f, 1.0f);
                push(deck, info);
            }
        } else {
        if (waterBlas_.as) {
            RtInstanceDesc water = {};
            writeTransform(water, kI, 0.0f, 0.0f, 0.0f);
            water.instanceMask = kMaskWorld;
            water.accelerationStructure = waterBlas_.as->getGpuAddress();
            V6Instance info{};
            info.triOffset = waterTriOffset_;
            info.kind = KIND_WATER;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(water, info);
        }

        for (const auto &kv : chunks_) {
            const Chunk &c = kv.second;
            // NOTHING TO POINT AT. A chunk of pure air built no structure, so
            // there is no address to give the instance -- and a TLAS entry with
            // a null one is not an empty instance, it is a fault. Its decor
            // below is still placed: a tree stands in the air above a chunk
            // with no ground in it perfectly well.
            if (!c.blas.as) {
                for (size_t di = 0; di < c.decorDesc.size(); ++di)
                    push(c.decorDesc[di], c.decorInfo[di]);
                continue;
            }
            RtInstanceDesc ci = {};
            // Chunk meshes are already in world space, so the transform is
            // identity -- the instance exists to carry the triangle offset, not
            // to place anything.
            writeTransform(ci, kI, 0.0f, 0.0f, 0.0f);
            ci.instanceMask = kMaskWorld;
            ci.accelerationStructure = c.blas.as->getGpuAddress();
            V6Instance info{};
            info.triOffset = c.triOffset;
            info.kind = KIND_TERRAIN;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(ci, info);

            for (size_t i = 0; i < c.decorDesc.size(); ++i) push(c.decorDesc[i], c.decorInfo[i]);
        }
        }  // ...and the end of the "not on the stage" branch above.

        const uint32_t n = uint32_t(instanceDescs_.size());
        ensureBuffer(instanceDescBuf_, n * sizeof(RtInstanceDesc), ResourceBindFlags::ShaderResource,
                     "v2::instanceDescs");
        ctx_->updateBuffer(instanceDescBuf_.get(), instanceDescs_.data(), 0,
                           n * sizeof(RtInstanceDesc));

        if (!instanceInfo_ || instanceInfo_->getElementCount() < n) {
            instanceInfo_ = device_->createStructuredBuffer(
                sizeof(V6Instance), std::max(n, 1u), ResourceBindFlags::ShaderResource,
                Falcor::MemoryType::DeviceLocal);
            instanceInfo_->setName("v2::instances");
        }
        if (n > 0)
            ctx_->updateBuffer(instanceInfo_.get(), instanceInfos_.data(), 0,
                               n * sizeof(V6Instance));

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        // No compaction here, unlike the bottom-level structures: this is
        // rebuilt every time the ring moves, and the compaction pass costs a
        // second flush for a structure that is a few megabytes at most.
        //
        // ALLOW_UPDATE, so the held item can be moved with a refit instead of a
        // rebuild -- see setHeldInstance for the measurement that makes that
        // the difference between affordable and not. It is asked for only when
        // there is something to move: the flag costs a little build time and a
        // little traversal speed on a structure that would otherwise never be
        // updated, and a world with nothing in the hand should not pay it.
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
        if (!held_.empty() || !flyers_.empty())
            inputs.flags = inputs.flags | RtAccelerationStructureBuildFlags::AllowUpdate;
        inputs.descCount = n;
        inputs.instanceDescs = instanceDescBuf_->getGpuAddress();

        const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), inputs);
        ensureBuffer(tlasScratch_, pre.scratchDataSize, ResourceBindFlags::UnorderedAccess,
                     "v2::tlasScratch");
        // Its own buffer rather than sharing the build's: an update is issued
        // in the middle of a frame that may also have rebuilt, and the driver
        // is entitled to still be reading the build scratch.
        // ...and EITHER MOVER, matching the flag above it. This asked only
        // about the hand, which was the whole truth until something else in
        // this world moved: an --out render loads no viewmodel at all, so a
        // flock published into a structure with no update scratch was a refit
        // that returned at its first line and sixty-four butterflies that were
        // never in the picture.
        if (!held_.empty() || !flyers_.empty())
            ensureBuffer(tlasUpdateScratch_, pre.updateScratchDataSize,
                         ResourceBindFlags::UnorderedAccess, "v2::tlasUpdateScratch");

        // The structure object wraps a buffer and does not own it, so a grown
        // buffer means a new object as well.
        if (!tlasBuffer_ || tlasBuffer_->getSize() < pre.resultDataMaxSize) {
            tlasBuffer_ = device_->createBuffer(pre.resultDataMaxSize,
                                                ResourceBindFlags::AccelerationStructure,
                                                Falcor::MemoryType::DeviceLocal);
            tlasBuffer_->setName("v2::tlas");
            tlas_ = nullptr;
        }
        if (!tlas_) {
            RtAccelerationStructure::Desc d;
            d.setKind(RtAccelerationStructureKind::TopLevel);
            d.setBuffer(tlasBuffer_, 0, tlasBuffer_->getSize());
            tlas_ = RtAccelerationStructure::create(device_, d);
        } else {
            ctx_->uavBarrier(tlasBuffer_.get());
            ctx_->uavBarrier(tlasScratch_.get());
        }

        RtAccelerationStructure::BuildDesc bd = {};
        bd.inputs = inputs;
        bd.source = nullptr;
        bd.dest = tlas_.get();
        bd.scratchData = tlasScratch_->getGpuAddress();
        ctx_->buildAccelerationStructure(bd, 0, nullptr);
        ctx_->uavBarrier(tlasBuffer_.get());
        prof_.tlasMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tt0)
                .count();
        ++prof_.tlasCalls;
    }

    void ensureBuffer(ref<Buffer> &b, size_t bytes, ResourceBindFlags flags, const char *name) {
        bytes = std::max<size_t>(bytes, 256);
        if (b && b->getSize() >= bytes) return;
        b = device_->createBuffer(bytes, flags, Falcor::MemoryType::DeviceLocal);
        b->setName(name);
    }
};

}  // namespace v2
