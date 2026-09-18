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
#include <cstdlib>
#include <deque>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <unordered_set>   // wanted_ -- see the note over it
#include <string>
#include <vector>

#include "../../shaders/Shared.slang"
#include "core/noise.h"
#include "world/chunks.h"
#include <cstdio>
#include <climits>
#include <functional>

#include "player/physics.h"
#include "player/collide.h"
#include "voxel/voxbox.h"
#include "world/sky.h"
#include "voxel/vox.h"
#include "world/voxelworld.h"

// The shader carries its own copies of these two, because Shared.slang has to
// compile as Slang and cannot include a C++ header. Neither has ever changed,
// but a silent disagreement about the voxel size would move every model
// relative to the ground it stands on, so it is checked rather than trusted.
static_assert(v2::VOXEL_M == v2::kVoxelM, "voxel size disagrees with the shader");
static_assert(v2::mat::GRASS_0 == v2::kGrass0, "grass ramp disagrees with the shader");
static_assert(v2::mat::GRASS_COUNT == v2::kGrassCount, "grass ramp disagrees with the shader");
static_assert(v2::mat::BGRASS_0 == v2::kBGrass0, "birch grass ramp disagrees with the shader");
static_assert(v2::mat::BGRASS_COUNT == v2::kBGrassCount,
              "birch grass ramp disagrees with the shader");
static_assert(v2::mat::BGRASS_FLOOR_COUNT == v2::kBGrassFloorCount,
              "the oak floor's share of the broadleaf ramp disagrees with the shader");
static_assert(v2::kBladeW0 == int(v2::kBGrassFloorW0) &&
                  v2::kBladeW1 == int(v2::kBGrassFloorW1) &&
                  v2::kBladeW2 == int(v2::kBGrassFloorW2),
              "the floor's shade odds disagree with the blade heights they are counted from");
// THE TWO STRAW RAMPS. Worth checking with the rest for a sharper reason than
// the others: they are the last families before mat::TREE_BASE, so if either
// COUNT moves without TREE_BASE moving with it, the model palette starts at a
// slot the blades are using and tall grass gets painted with whatever a tree
// registered. The chain below pins all three numbers to each other.
static_assert(v2::mat::WHEAT_0 == v2::kWheat0, "wheat ramp disagrees with the shader");
static_assert(v2::mat::WHEAT_COUNT == v2::kWheatCount, "wheat ramp disagrees with the shader");
static_assert(v2::mat::BWHEAT_0 == v2::kBWheat0, "birch wheat ramp disagrees with the shader");
static_assert(v2::mat::BWHEAT_COUNT == v2::kBWheatCount,
              "birch wheat ramp disagrees with the shader");
static_assert(v2::mat::WHEAT_0 + v2::mat::WHEAT_COUNT == v2::mat::BWHEAT_0,
              "the two wheat ramps overlap");
static_assert(v2::mat::BWHEAT_0 + v2::mat::BWHEAT_COUNT == v2::mat::TILLED,
              "the tilled-earth id overlaps the wheat ramps");
// ...AND THE HOE'S ONE ENTRY SITS BETWEEN THEM AND THE MODELS. v1 mints tilled
// earth at runtime and has to cope with a full table; here it is a fixed id, so
// the only thing that can go wrong is somebody adding a second fixed id and
// forgetting to move TREE_BASE -- which is what this line is.
static_assert(v2::mat::TILLED + 1 == v2::mat::SEED_0,
              "the seed ramp overlaps the tilled-earth id");
static_assert(v2::mat::SEED_0 + v2::mat::SEED_COUNT == v2::mat::GROUND_0,
              "the ground ramp overlaps the seed ramp");
// ...AND THE GROUND'S TEN COLOURS SIT BETWEEN THE SEEDS AND THE MODELS. Added
// with the aerial imagery: bare ground used to be one flat mat::ROCK, which is
// why Colorado came out grey. The chain of asserts above and below is the whole
// reason this was caught at compile time rather than as a wrong colour on a
// hillside -- the first attempt tried to squat on BWHEAT's dead range instead,
// and nothing would have complained, because isWheat() tests the RANGE.
static_assert(v2::mat::GROUND_0 + v2::mat::GROUND_COUNT == v2::mat::SNOW_0,
              "the snow band follows the ground ramp -- see mat::SNOW_0");
static_assert(v2::mat::SNOW_0 + v2::mat::SNOW_COUNT == v2::mat::TREE_BASE,
              "the model palette overlaps the terrain's own ids");
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
static_assert(v2::STRAND_MAX_ROWS ==
                  (int(v2::kStrandRowMask) + 1) * v2::STRAND_ROW_STEP,
              "strand height disagrees with the shader");
static_assert(v2::STRAND_ROW_STEP == (1 << int(v2::kStrandRowShift)),
              "the host and the shader disagree about the ramp's step");
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
// DOUBLED (user 2026-09-18: "double the frequency of the perched song birds").
// This is the whole knob -- birds_ is assigned kBirdSlots and the perch search
// fills every one it can, so the band grows and the wood gets twice the birds.
// The band is a sum of these constants rather than a fixed total, so raising
// one does not have to be paid for by lowering another.
constexpr int kBirdSlots = 96;
// ...AND THE LAKE: salmon, lily pads and dragonflies, in that order. They are
// one population for the band's purposes because they are born and recycled
// together off one water field -- see render/lake.h -- but each still owns a
// contiguous run inside it, for the reason the note above gives.
// THIS NUMBER IS NOT A PREFERENCE, IT IS A CONTRACT. It has to equal exactly
// what LakeLife::publish writes, because the band is a fixed layout and every
// population after this one is addressed from the end of it. Landing the lake's
// own counts without landing this left the lake writing 44 slots into a 26-slot
// reservation: it silently overwrote nine songbirds and all three pause-room
// buttons, and the only symptom was "I dont see the buttons in the room
// anymore". A static_assert in lake.h now makes the two agree at compile time.
constexpr int kLakeSlots = 90;   // + the blue gill; lake.h static_asserts the exact sum
                                 // -- the static_assert in lake.h is the authority
// ...and the songbirds that are IN THE AIR, which are a different population
// from the ones in the trees and share nothing with them but their species --
// see render/birdflock.h.
constexpr int kFlockSlots = 9;   // kFlockBirds
// ...AND THE PAUSE ROOM'S THREE BUTTONS, WHICH ARE NOT ALIVE AT ALL.
//
// They are here because the band is the engine's only route to an instance
// whose TRANSFORM changes every frame without a structure rebuild, and a button
// that presses in when you click it is exactly that. Modelling them as part of
// the room's static mesh would mean re-meshing and re-building a 68,000
// triangle box to move three of them two centimetres.
//
// THEY COST NOTHING WHEN THE ROOM IS SHUT: a slot with mask 0 is never
// traversed, and publishButtons switches them off the moment you leave.
// ...THE BUNNIES, which are the first thing in this band that walks on the
// ground. They are in it for the same reason everything else is: it is the one
// route to an instance whose transform changes every frame, and a bunny in the
// band is a bunny lit by exactly the shader that lights a pine.
// TEN, AND kBunnyCount IS FIVE. The band is a RESERVATION, not a population:
// the asset editor draws its three gizmo arrows through kBunnySlot0 + 1..3
// (see assetedit.h) and the spare descriptors cost one instance mask each.
constexpr int kBunnySlots = 10;   // >= kBunnyCount, and >= 4 for the editor
// ...AND THE SKUNKS, WHICH ARE THE SECOND LAND MAMMAL AND NOT A SECOND KIND OF
// RABBIT. They share the rabbits' file, their sensors and their spawn lattice
// and nothing else -- one hops and one marches -- so they share the band the
// same way: a run of their own, contiguous, written every frame including the
// empty ones. See kSkunkCount in render/bunnies.h, which this has to be at
// least as large as.
// TWELVE, AND THE POPULATION IS EIGHT. A reservation is not a population: the
// band is one fixed layout, so raising a species' count later would otherwise
// cost a structure rebuild, and a slot nobody writes is one instance mask the
// traversal skips. The relationship is checked at compile time where the counts
// live -- see the static_assert over Bunnies::publishSkunks.
// ...AND IT CARRIES SIX SPECIES NOW, NOT FOUR. The worm and the grass snake
// joined the march (user 2026-09-14: "import the worm in both forests",
// "import the snake to the birch"), and neither is a mammal -- what the band
// means is "an animal that walks a strip of frames along the ground", which is
// exactly what both of them do.
//
// FORTY, AND THE POPULATION IS THIRTY-THREE. A reservation is not a population,
// and raising a count must not cost a structure rebuild -- which is exactly
// what it just did cost when the four mammals went from two to six (see the
// note over kBunnyCount: at two, the median distance to the nearest one was
// 58 m and nobody ever saw one). The margin is deliberately wide enough to take
// the next raise without this number moving again. The relationship is checked
// at compile time where the counts live -- see the static_assert over
// Bunnies::publishSkunks.
constexpr int kMarchSlots = 40;
// ...AND THE BEES, which are the first thing in this band that belongs to a
// PLACEMENT rather than to the ground: a bee exists because a beehive does, and
// the hives are 5% of the birches. Ten is two hives' worth at v1's five to a
// hive -- see render/bees.h.
constexpr int kBeeSlots = 10;
constexpr int kButtonSlots = 3;
// ...AND THE FIVE SMALL ONES -- the firefly, the ant's column, the fly's
// bunch, the ladybug and the frog. They are one reservation rather than five
// because they are one file (render/critters.h) and one publish; inside it each
// still owns a contiguous run, in the order the counts are declared there, for
// the reason the note at the top of this band gives.
//
// -- IT SAID 26 "AGAINST A POPULATION OF TWENTY-TWO" AND THE POPULATION WAS
//    TWENTY-EIGHT (user 2026-09-14: "I dont see the frog on the field") ------
//
// The count in that sentence added the ant, the fly, the ladybug and the frog
// and left out the SIX FIREFLIES -- the note says "the four small ones" and
// there are five. So the run had been two slots short since the day the
// firefly was added, and Critters::publish walks its populations in order:
// firefly, ant, fly, ladybug, frog. The frog is LAST, so the frog is what fell
// off the end, and setFlyerInstance drops a slot past the band without a word.
//
// THE FROG HAD THEREFORE NEVER BEEN DRAWN, not once, in any wood. It spawned,
// it hopped, it answered /locate, it turned up in --clip-test's census -- every
// one of which reads the HOST's copy of the population. The only thing it never
// did was appear, and raising kFrogCount from 2 to 6 made that worse rather
// than better by pushing four more of them past the same edge.
//
// 40 NOW, AND THE POPULATION IS 32. Same margin the marchers carry, and
// render/critters.h static_asserts the sum against it, so the next species
// added to that file cannot repeat this quietly.
constexpr int kCritterSlots = 40;
// -- ...AND THE PARTICLES, WHICH ARE THE ONLY THING IN THIS BAND THAT IS NOT
//    AN OBJECT IN THE WORLD --------------------------------------------------
//
// v1's spark and smoke pools, ported (user 2026-09-14: "when life is killing 4
// spark voxels play", "on death the life plays smoke that rises", "everytime a
// tool hits something, play 4 sparks just like in v1").
//
// TWENTY, WHICH IS v1'S OWN NUMBER AND ITS OWN SPLIT: four sparks and sixteen
// smoke voxels. The four are stamped outright by every burst -- a second blow
// inside half a second replaces the first blow's embers, which is what v1 does
// and is invisible at that rate -- and the sixteen are taken free-first, so a
// death mid-plume adds to the column rather than deleting it.
//
// IN THE FLYER BAND rather than a pool of their own, for the reason the note at
// the top of this band gives: it is the engine's one route to an instance whose
// transform changes every frame, and that is the whole of what a particle is.
// A spark is a single voxel wearing an emissive material -- see
// V6Params::emitters -- so it costs one instance and no light.
constexpr int kParticleSparks = 4;
constexpr int kParticleSmoke = 16;
// -- ...AND FOUR TEARS (user 2026-09-15: "the babies should cry") -----------
//
// v1's own band, TEAR_LO 16 to TEAR_HI 20, and v1's own reason for the size:
// "three ducklings at CRY_GAP never need more". A tear lives 0.7 s and one is
// emitted every 260 ms, so a single weeping duckling has at most three in the
// air; three of them at once would overrun four slots, and the overflow rule is
// the same one the smoke uses -- skip this tear rather than cut a live one
// short. A missing droplet cannot be seen; one that blinks out half way down a
// cheek can.
constexpr int kParticleTears = 4;
constexpr int kParticleSlots = kParticleSparks + kParticleSmoke + kParticleTears;
// -- ...AND THE ROUNDS THE RIFLE THROWS (see render/bullets.h) -------------
//
// The band lives here with the others rather than in bullets.h because THIS is
// the file that decides how long the instance array is, and a population whose
// count is declared somewhere else is how the critter band ended up two slots
// short -- a write past the end of the flyer array is dropped without a word,
// and the frog was never drawn in any wood for as long as that lasted.
//
// The rifle fires every kBulletIntervalMs and a round lives kBulletLifeS, so at
// most about a dozen are ever up at once; twenty-four is that with room, and
// the pool takes the oldest slot rather than refusing a shot.
constexpr int kBulletSlots = 24;
constexpr int kFlyerInstances = kButterflySlots + kBirdSlots + kLakeSlots + kFlockSlots +
                                kBunnySlots + kMarchSlots + kBeeSlots + kButtonSlots +
                                kCritterSlots + kParticleSlots + kBulletSlots;
constexpr int kBunnySlot0 = kButterflySlots + kBirdSlots + kLakeSlots + kFlockSlots;
constexpr int kMarchSlot0 = kBunnySlot0 + kBunnySlots;
constexpr int kBeeSlot0 = kMarchSlot0 + kMarchSlots;
constexpr int kButtonSlot0 = kBeeSlot0 + kBeeSlots;
constexpr int kCritterSlot0 = kButtonSlot0 + kButtonSlots;
constexpr int kParticleSlot0 = kCritterSlot0 + kCritterSlots;
constexpr int kBulletSlot0 = kParticleSlot0 + kParticleSlots;

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
// How red a corpse piece stays once v1's half-second blink has run -- see
// corpseFade. Not zero, because zero is a grey lump of an animal lying in the
// wood, which is the one thing a corpse must never look like.
constexpr float kCorpseRedFloor = 0.55f;

// ---------------------------------------------------------------------------
// WHAT A LOOSE BODY IS MADE OF -- the one thing a tool has to know about it.
//
// A Solid says what it is by being standable or not; a body cannot, because a
// felled tree is lying down and a boulder that rolled is as well. So the body
// remembers what it was cut from, and toolTakes asks that. Three classes
// because the kit has three heads -- see Takes in render/helditem.h, which the
// first three mirror exactly.
//
// ...AND A FOURTH THAT MIRRORS NO HEAD AT ALL. A mushroom is neither stone nor
// wood, and the two things that follow from that are both things the kit cannot
// express:
//
//   * BOTH EDGED TOOLS CUT IT (user 2026-09-14: "let the axe take a chunk out
//     of the mushrooms just like the pick can"). Takes is one value per tool,
//     so a material that answers to two of them cannot be said in that enum.
//     toolTakes reads it off Swing::soft instead, which a standing cap already
//     carried from Solid::bouncy -- this is how a FALLEN one carries the same
//     fact. See looseSwing.
//
// AND THAT IS THE WHOLE OF WHAT IT SAYS. It is a MATERIAL, not a temperament:
// whether a given mushroom body is collected or left lying is Debris::scenery,
// which is set per body rather than per material. Both exist because the answer
// differs between two bodies made of the same thing --
//
//     the CHUNK a blow knocks out   tumbles, then flies to the player
//     the MUSHROOM cut off the ground   falls, and stays where it lands
//
// -- which the user separated in as many words (2026-09-14): "the chunks
// themselves obey the physics temporarily before getting absorbed from the
// player, just like it was before", against "when the mushroom breaks from the
// static terrain ... it should obey physics". An earlier pass had this enum
// answering both questions and so made the chunks uncollectable too.
// ---------------------------------------------------------------------------
enum DebrisTakes : uint8_t {
    kDebrisStone = 0,
    kDebrisWood = 1,
    kDebrisSoil = 2,
    kDebrisSoft = 3,
};

// The first loose body under a ray, and the voxel of it that was struck. The
// VOXEL and not the point: re-deriving one from the other is a rounding away
// from the cell in front or the one behind -- carveModel's own note.
struct DebrisHit {
    int slot = -1;
    float t = 0.0f;            // metres from the eye
    Vec3 point{0, 0, 0};       // world
    int vox[3] = {0, 0, 0};    // in the body's own grid
    uint8_t mat = mat::AIR;
    uint8_t takes = kDebrisStone;
};

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
// HOW FAR A LOOSE BODY MAY DRIFT FROM ITS COLLISION WINDOW BEFORE IT GETS A
// NEW ONE, and how many of those may be built in a frame.
//
// The window is kWindowCells voxels on a side -- 3.2 m -- so its half span is
// 1.6 m and a body at 1.0 m from the centre still has at least 0.6 m of solid
// world described around it in every direction. That margin is the point: the
// rebuild happens while the piece is still supported, not after it has already
// fallen out of the only geometry it knew about.
// ===========================================================================
// THE RULE: ANYTHING SEVERED FROM THE STATIC WORLD BECOMES A RIGID BODY.
//
// (user 2026-09-17: "whenever something breaks apart from the static terrain,
//  it becomes a rigid body. This needs to become a RULE in the codebase. no
//  matter what.")
//
// It applies to every kind of static geometry this engine has, and the list is
// closed: the TERRAIN (dig), a MODEL -- boulder, trunk, decor -- (carveModel),
// a body that is itself already loose (carveDebris), and the LEVEL
// (carveLevel). There is no fifth thing to cut.
//
// WHY IT KEEPS NEEDING TO BE RE-STATED. The rule is not enforced by the type
// system: each carve hands back a spoil cube and it is the CALLER that turns
// it into a body, so the rule is one deletable line away from being broken
// every time one of these sites is edited. It has been broken exactly that way
// three times now --
//
//   * terrain hangers were carved and never spawned (severed stone vanished);
//   * model hangers had the same bug in the model's own frame, which is the
//     one a player actually sees, on a boulder;
//   * the LEVEL's spawn line was deleted outright by a concurrent edit on
//     2026-09-17 and had to be put back -- see arrowChip.
//
// Each time the symptom was identical and is worth knowing by heart: THE HOLE
// APPEARS AND NOTHING COMES OUT OF IT.
//
// SO THE THREE THINGS THAT HOLD IT UP ARE:
//
//   1. kMinBodyVoxels == 1, below. There is no size at which a severed piece
//      is allowed to be discarded instead of embodied.
//   2. carveLevelToBody(), which fuses the carve and the spawn into one call
//      so the level path -- the one that has actually regressed -- has no
//      separate spawn line for an edit to drop.
//   3. --fire-frame, which shoots a real round at the map and fails loudly
//      with "CUT BUT NO BODY -- THE VOXELS JUST VANISHED" if voxels came out
//      and no slot was filled. That test is the regression alarm; run it after
//      touching any of this.
//
// See also the NOTHING FLOATS rule it is the other half of: that one says a
// severed piece may not hang in the air, this one says it may not disappear.
// ===========================================================================
constexpr int kMinBodyVoxels = 1;

// -- AND WHAT THE LEVEL'S HANGER SWEEP IS ALLOWED TO SPEND ----------------
//
// See dropLevelHangers. The flood asks whether a piece can reach the map's
// foundation, which for anything still attached to a building means walking
// most of that building -- so it is bounded, and a piece that exhausts the
// bound is treated as ATTACHED rather than cut loose.
//
// 24,000 voxels is 24 cubic metres of solid, which is far more than any pole,
// railing, sign or fence in the map and far less than a house. The SPAN is the
// second bound and the one that actually fires on walls: 96 voxels is 9.6 m,
// so a piece wider than that in any axis is a piece of building.
// TIGHTENED (user 2026-09-17: "things are glitching when they become rigid
// bodies"). A 9.6 m rigid body cut out of the middle of a building is going to
// intersect the building on the frame it is born, and a solver's answer to a
// body that starts penetrating is an impulse proportional to the depth -- so
// the bigger the piece, the worse it behaves. 4.8 m and 8,000 voxels still
// covers every pole, railing, sign and fence in the map, which is what a shot
// actually knocks down; anything larger is structure and is left standing.
constexpr int kLevelHangCap = 8000;
// -- 48 -> 12, AND THIS IS A DELIBERATE TRADE ------------------------------
//
// (user 2026-09-17, twice: "its breaking things vertically, it should only
//  leave the initial shot mark.")
//
// MEASURED: at 48 a round was still freeing a 2 x 2 x 25 piece -- a 2.5 m rod,
// 75 voxels -- which fell out of a wall and left the long vertical scar in the
// report. Every other guard passed it honestly: it WAS grounded before the
// shot, it IS disconnected now, and it is free-standing rather than let into
// the wall, so the embedded test cleared it too. It is a real severed object.
//
// IT IS ALSO NOT A SHOT MARK, and that is the instruction. 12 voxels is 1.2 m
// -- four times the bite's own diameter and no more -- so what a round can
// shake loose is now debris rather than architecture.
//
// WHAT THIS GAVE UP, AND WHY IT NO LONGER HAS TO (2026-09-18) ---------------
//
// (user: "on the nuketown map, the light pole is not being subject to gravity
//  when cut from the static terrain".)
//
// The note that stood here said the two requests -- "break a light pole and it
// falls" and "only leave the initial shot mark" -- genuinely pull opposite
// ways, and that this constant was the dial between them. That was wrong, and
// it was wrong in a specific way: THE SPAN WAS DOING TWO JOBS. It bounded what
// the flood may COST, and exhausting that bound was also taken as the VERDICT.
// A light pole is 34 voxels tall, so it blew the budget every time and was
// called attached without any of the guards below ever looking at it.
//
// Split the two and the trade disappears. kLevelIslandSpan is the cost bound;
// the verdict is taken afterwards, from what the piece turns out to BE. This
// constant keeps its old value and its old job -- the size of a shot mark --
// and now applies only to pieces that are still TOUCHING the map.
//
// MEASURED over the shipped map by tests/level_pole_test.cpp:
//
//                        posts dropped     600 bursts into walls
//   span as verdict        1 of 7          33 pieces, 41 voxels, worst span 9
//   island rule            7 of 7          33 pieces, 41 voxels, worst span 9
//
// Identical on walls, because everything a round shakes out of masonry is
// already tiny and already an island; the rule only ADDS the case walls do not
// produce. The 2 x 2 x 25 rod that caused the vertical scar is still refused,
// and now for a reason that does not depend on a threshold: it touches the
// wall it sits against, so it is not an island, and its span is over this.
constexpr int kLevelHangSpan = 12;
// -- WHAT A FLOOD MAY COST, WHICH IS NOT THE SAME QUESTION -----------------
//
// 4.8 m, which is what the span was before it was pressed into service as a
// verdict. Big enough to hold any post, sign or railing in the map whole;
// small enough that a flood escaping into a building still stops quickly, with
// kLevelHangCap behind it for the case that runs sideways instead of up.
//
// A piece that exhausts THIS is still treated as attached, for the reason the
// cap's own note gives: leaving a big piece standing is the status quo, cutting
// a house loose because a flood ran out of budget is a building falling out of
// the world.
constexpr int kLevelIslandSpan = 48;
// HOW SOLID A SEVERED PIECE HAS TO BE TO SURVIVE AS ONE BODY -- see the split
// in dropLevelHangers. Voxels over the bounding box that holds them.
//
// 0.45 is the line between a fragment whose convex hull IS the fragment and one
// whose hull is mostly the air around it. A solid brick is 1.0; an L-bracket is
// about 0.5; a thin rail running corner to corner of its own box is under 0.1,
// and a hull of that is a wedge of solid map.
constexpr float kHullFillOk = 0.45f;
// HOW MUCH OF A SEVERED PIECE MAY STILL BE TOUCHING THE MAP BEFORE IT COUNTS
// AS PART OF IT -- see the test in dropLevelHangers.
//
// A pole standing in the open has air on every side and one contact under its
// foot: a few per cent. A strip let into a wall touches that wall down its
// whole length: a third of its voxels or more. 0.15 sits well clear of both,
// and the failure it chooses is the safe one -- a thing left standing rather
// than a wall taken apart.
constexpr float kEmbeddedFrac = 0.15f;
// HOW FAR OFF THE AIM LINE A FRUIT MAY BE AND STILL BE THE ONE YOU MEANT --
// see takeFruitAlong.
//
// 0.9 m is a little over two fruit widths. That sounds slack and is not: this
// is asked at the IMPACT FRAME OF A SWING, a quarter of a second after the
// button went down and with the view carried by the animation, so the aim it
// judges is not the aim the player took. The nearest fruit still wins, so a
// wide gate costs nothing where there is only one.
constexpr float kFruitAimM = 0.9f;
// ...and how many separate pieces one shot may free. A round cuts a hole with
// two sides; more than a few components coming loose from one bite means the
// flood is finding things the shot did not cause.
constexpr int kLevelHangMax = 4;
// ...and how big a gap under the map is a gap the voxeliser LEFT rather than
// one that is there on purpose. See seatLevelOnItsFoundation, which found
// every house in nuketown hovering 20 cm over its own lawn.
constexpr int kLevelSeatVox = 4;

constexpr float kWinRecentreM = 1.0f;
constexpr int kWinRebuildsPerFrame = 2;

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
// ...AND HOW DENSE A CELL OF PURE CANOPY HAS TO BE TO COUNT AS ONE.
//
// THE CROWN IS BACK IN THE COLLIDER, and the note above says why it was left
// out, so this says why that had to change. MEASURED with --fell-test's new
// sink column: with the wood alone, 5717 of a felled pine's 64377 voxels came
// to rest UNDER THE GROUND -- 8.9% of the tree, the deepest 2.02 m down. That
// is the whole crown ploughed into the hillside, and it is what "the tree clips
// through the terrain" is. Nothing was holding it up, because a felled conifer
// does not rest on its trunk: it rests on its BRANCHES, with the trunk held
// clear of the ground.
//
// A HIGHER BAR THAN THE WOOD'S, and that is the part that keeps the old
// warning honest. The recorded failure was a convex HULL of the whole tree --
// "a fat cone that cannot lie down" -- and the way not to rebuild that is to
// take only the parts of the canopy that are actually DENSE. At this fraction
// the inner crown, where the branches are, gives boxes; the wispy outer needles
// do not, and a needle passing a few centimetres into a bank is not what
// anybody means by clipping.
//
// THE FALL IS UNAFFECTED. The trace shows 0.0% of the tree inside the ground at
// every step of the topple -- PhysX's linear CCD was never the problem here --
// so this changes only where the thing comes to rest.
constexpr float kFellCrownFrac = 0.25f;
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
// -- THE GROUND A FELLED BODY CARRIES WITH IT -- see buildSolidWindow -------
//
// Sampled every kFellGroundStep voxels rather than per column: a 15 m window
// is 150 columns square and one box each would be 22,500 before merging. Four
// voxels is 40 cm, which is finer than anything a 26 m trunk can notice, and
// the run-merge along x takes the usual hillside down to a few dozen boxes.
//
// A METRE THICK, and that is a ceiling as much as a floor: deep enough that
// nothing at rest can be pushed through it, shallow enough that a body which
// is ALREADY buried is not sealed under a slab it can never climb out of.
constexpr int kFellGroundStep = 4;
constexpr float kFellGroundThickM = 1.0f;
// How hard a shot bulb's pieces leave, in metres per second. Enough to read as
// breaking rather than as one object falling, small enough that no fragment is
// thrown across the room -- see takeLevelBulbNear.
constexpr float kBulbBurstMs = 1.0f;
// How far past a body's own bounds the window reaches, and how far inside those
// bounds the body may travel before it is rebuilt. The gap between the two is
// what stops a rebuild every frame.
constexpr float kStaticPadM = 3.0f;
// -- HOW BIG A BODY MAY BE AND STILL BE TREATED AS A CHIP ------------------
//
// (user 2026-09-18: "when things become rigid bodies on nuketown, they fall
//  endlessly and glitch out on the floor".)
//
// A chip gets a FIXED 3.2 m window of static world centred on it (kWindowCells)
// and a floor clamp that holds its ORIGIN half its own height above the ground.
// Both are right for something a few voxels across and both fail for anything
// long, in the two ways the felled-tree path was built to avoid:
//
//   * the body is wider than the window that is supposed to hold it up, so its
//     ends have no collider and it sinks through the floor;
//   * the clamp pins its origin at half-height, which for an upright 3.4 m post
//     is 1.7 m -- so it is held in the sky and fights the solver there.
//
// MEASURED, before this existed: of seven light poles cut loose, two were still
// falling after four seconds and the other five "settled" 1.5 to 2.4 m above
// the floor, which is the clamp holding them, not the ground.
//
// 1.6 m is the window's half-span. Anything longer than that cannot fit inside
// its own collider with room for the floor underneath, so it takes the same
// route a felled tree does: a window sized to its BOUNDS, rebuilt when it
// leaves them, and no origin clamp at all.
constexpr float kLongBodyM = 1.6f;
// How many cells a side buildSolidWindow may sample the LEVEL over. A 3.4 m
// post padded by kStaticPadM asks for a 9.4 m cube, which is 830,000 samples on
// the frame the post is cut -- so it is capped, and the cap is what the body
// can actually reach before its window is rebuilt around it.
constexpr int kLevelWinCells = 96;

// ---- AND THE GROUND ITSELF LEAVES NOTHING HANGING ------------------------
//
// How far around a bite to look for terrain the bite cut loose. A hole is a
// 30 cm sphere, so anything it disconnects is within a voxel or two of it and
// this is generous; the cost is a flood over the box, once per blow.
// MEASURED before it existed: 55% of dig sites left terrain standing on air,
// 448 voxels over 300 sites, the worst site 26.
constexpr int kHangBoxVox = 13;

// -- WHEN A LOOSE PIECE IS AN OBJECT RATHER THAN LITTER ---------------------
//
// (user 2026-09-17: "audit the game for floating objects".)
//
// 27 voxels is a 3x3x3 block, 30 cm on a side. Under that a piece is a needle
// cluster, a leaf, a chip of bark -- things the .vox author drew as separate
// islands and that no player has ever pointed at. At or over it, it is
// something with a shape you can see hanging in the air.
//
// NOT A LICENCE TO LEAVE THE SMALL ONES. kMinBodyVoxels is still 1 and every
// severed voxel still becomes a body. This is the AUDIT's line, so a report can
// say "four hundred needles" and "one branch" in different columns instead of
// adding them up into a number that means nothing.
constexpr int kFloatPieceVox = 27;

// ===========================================================================
// THE FLOAT WATCH -- the layer that asks again.
//
// (user 2026-09-17: "I need you to audit the game for floating objects ... we
//  have attempted this multiple times and it is not working ... create a
//  mechanic layer for this ... then when a floating voxel is detected, turns
//  into a rigid body.")
//
// IT IS NOT AN AI AND IT DOES NOT NEED TO BE. The question "is this piece of
// geometry still joined to the ground" has an exact answer -- a flood fill --
// and the only reason this engine kept getting it wrong is that it was asking
// in the wrong PLACE and at the wrong TIME. Both of those are cheap to fix and
// neither wants a model.
//
// WHAT WAS ACTUALLY BROKEN, measured by --float-sweep on the shipped build:
// hollow a room under a hillside, cut a ring through the crust around it, and
// 26,431 voxels of ground -- a disc seven metres across -- hang in the air for
// ever. Every existing sweep missed it, for two reasons that compound:
//
//   1. THEY ARE BOXES, AND THE BOX IS SMALLER THAN THE PIECE.
//      dropTerrainHangers floods 2.7 m (kHangBoxVox 13). dropModelHangers
//      floods a box round the bite. Both call anything touching the wall
//      "held from outside", which is the right call for a box that small and
//      is wrong for every piece bigger than one.
//   2. THEY RUN ONCE, IN THE FRAME OF THE BLOW, AND NEVER AGAIN. A piece
//      correctly called "held from outside" is never reconsidered when the
//      thing holding it is cut ten seconds later, because the later blow
//      floods a box that no longer contains the earlier piece. A player takes
//      a hillside apart over a hundred blows and no single one of them ever
//      sees what the hundred did.
//
// SO THIS ASKS AGAIN, BIGGER, AFTERWARDS, AND ON A BUDGET. A region marked
// dirty by an edit is re-flooded at kFloatRegionVox -- 9.6 m, wide enough to
// hold the piece that defeated the box -- some thousands of cells per frame,
// across as many frames as it takes. The piece falls a fraction of a second
// after it was cut free rather than in the same frame, which is not a
// compromise: it is what a hillside giving way looks like.
//
// IT DOES NOT REPLACE THE PER-BLOW SWEEPS. They are still the fast path and
// they still catch the ordinary case in the frame it happens. This is the
// backstop that catches what they cannot see, and the two do not conflict:
// anything they already dropped is air by the time this looks.
// ===========================================================================

// HOW WIDE THE WATCH LOOKS.
//
// 128 voxels is 12.8 m, and the margin is the point rather than the width. The
// measured failure is a 7.4 m disc -- 80 voxels -- and at 96 that left four
// voxels of slack a side, so a region centred anywhere but dead on the piece
// clipped it, the piece touched a wall, and the wall rule (correctly) declined
// to judge it. MEASURED: at 96 the second look converged at two spawns in four
// and never at the other two, because the centroid of a CLIPPED piece is not
// the centroid of the piece.
//
// At 128 an 80-voxel piece has 24 voxels of slack a side, which is more than
// the 12 a dedupe cell can be off by -- so one re-centre always lands it. The
// cost is the CUBE of this and it is not free: 2.1 M cells against 884 k. What
// makes that affordable is that the sampler works by COLUMN (16,384 of them,
// not 2.1 M voxels) -- see stepFloatSample.
constexpr int kFloatRegionVox = 128;

// HOW FAR APART TWO EDITS HAVE TO BE TO EARN THEIR OWN LOOK. A pick swing is
// ten blows in one spot and they all want the same region; snapping the dirty
// point to a 2.4 m grid collapses them into one job.
constexpr int kFloatDedupeVox = 24;

// HOW MANY REGIONS MAY BE WAITING. Past this the oldest is dropped -- a player
// digging continuously is always generating work, and a queue that grows
// without limit is a queue that never gets to the thing they just did.
constexpr int kFloatQueueMax = 24;

// HOW LONG A REGION HAS TO BE LEFT ALONE BEFORE IT IS WORTH LOOKING AT.
//
// (user 2026-09-17: "when shoveling the ground causes glitches in the terrain
//  ... whenever Im hitting the meshed terrain, it glitches out.")
//
// MEASURED: a hundred shovel bites over five metres queued ELEVEN jobs and
// cost 455 FRAMES of flooding -- 0.58 ms on every one of them, spikes of
// 4.7 ms -- to discover that a hillside is connected to itself. Eleven floods
// of the same ground, because the bites were 2.4 m apart and each one claimed
// its own region.
//
// THE ANSWER ONLY MATTERS ONCE THE DIGGING STOPS. Nothing can be hanging while
// you are still cutting, and anything that is will still be hanging in half a
// second. So a dig pushes the region's start time out instead of starting it,
// and a player holding the button down pays NOTHING until they let go -- which
// is exactly the moment the cost stopped being felt.
//
// 400 ms is about three bites at the swing rate: long enough to swallow a burst
// and short enough that a piece falls while you are still looking at the hole.
constexpr double kFloatSettleMs = 400.0;

// THE FRAME BUDGET, in columns sampled and cells flooded per step. Both were
// set so one step stays near a tenth of a millisecond on the measured region;
// see --float-sweep, which prints what a whole job costs.
constexpr int kFloatSampleCols = 700;
constexpr int kFloatFloodCells = 70000;

// HOW BIG A PIECE OF THE FALLING SLAB ONE BODY IS.
//
// EVERY BODY IN THIS ENGINE IS A CONVEX HULL -- see the note in
// dropLevelHangers -- so a seven-metre disc handed over whole would be a
// seven-metre lens, and the volume its hull invents is ground that is still
// there. Diced into 3.2 m cubes each body is nearly its own bounding box, the
// hull tells no lies, and a slab that comes apart as it falls is what a
// collapsing hillside actually does.
constexpr int kFloatDiceVox = 32;

// AND HOW MANY OF THOSE ONE PIECE MAY SPEND. kDebrisInstances is 64 for the
// whole world; one collapse must not take all of them, or nothing else in the
// game can break until it is cleaned up.
//
// ALL OF A PIECE OR NONE OF IT, AND THAT IS NOT A PREFERENCE. The first cut of
// this capped the SPAWN at six dice and then carved the whole component, so a
// 26,431-voxel lid left the world as 13,806 voxels of rigid body and 12,625
// voxels of nothing at all. That is the vanishing bug this entire rule exists
// to answer, rebuilt from scratch inside the thing built to enforce it -- see
// the RULE block over kMinBodyVoxels, which has now been broken three times by
// three different routes.
//
// So the slots are counted BEFORE anything is spawned or carved. Not enough
// free, or more dice than one piece may have: nothing happens, the region goes
// back on the queue, and the ground stays where it is until the pool drains.
// Still standing is recoverable. Deleted is not.
constexpr int kFloatDicePerPiece = 24;

// ...and how many PIECES one frame may deal with, so a collapse that frees a
// dozen of them is spread out rather than spent in one step.
constexpr int kFloatPiecesPerStep = 2;

// HOW A VOXEL IS MARKED ONCE THE TWO FLOODS HAVE RUN. 0 is the answer that
// matters: nothing reached it, so it is standing on nothing.
constexpr uint8_t kFloatGrounded = 1;   // the floor of the region reaches it
constexpr uint8_t kFloatOutside = 2;    // only a side wall does -- see stepFloatFlood

// ...AND HOW MANY SECOND LOOKS ONE REGION MAY ASK FOR. A cut hillside can
// present several ambiguous masses at once and each one is a whole job; two is
// enough for every case measured and stops a pathological region from filling
// the queue with itself.
constexpr int kFloatRecentreMax = 2;

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
// -- HOW CLOSE YOU HAVE TO BE FOR A CHIP TO COME TO YOU ---------------------
//
// (user 2026-09-15: "dont have the chipped chunk caused by the arrow get
// absorbed by the player. only when the player is close enough".)
//
// v1's ARROW_ABSORB_R, which is 16 voxels, and v1's note is worth having in
// full because it took that engine several passes: the radius started at 30 --
// three metres, WIDER than an ordinary chunk's -- and that is backwards. "A
// piece knocked off from across the clearing should be no easier to collect
// than one you cut standing over it."
//
// A SWING NEEDS NO GATE AND DOES NOT GET ONE. Chipping with a tool puts you
// within arm's reach of the chip by construction, so every chip in the game
// until now flew to the player and that read correctly. A SHAFT breaks that
// assumption and is the only thing that does: it knocks a piece off something
// twenty-five metres away, and the piece then sailed the whole way back. So
// the default stays "no gate" and the arrow states its own reach -- see
// Debris::absorbR.
constexpr float kArrowAbsorbM = 1.6f;
// The sentinel for "from anywhere", which is every chip a tool makes.
constexpr float kAbsorbAnywhere = 1e9f;

// ...AND THE OTHER END OF THE SAME DIAL: NEVER, HOWEVER CLOSE YOU STAND.
//
// (user 2026-09-17: "in the fps map, dont have the player absorb the chunks
//  caused by the bullet. just leave them there.")
//
// A RADIUS OF ZERO IS NOT THIS, which is why it is a named sentinel rather
// than 0.0f. The reach test is `dist2 <= absorbR * absorbR`, and at zero that
// is still true for a chip lying exactly under the player -- so a piece shot
// out of a wall you then walk onto would vanish, once in a while, for no
// reason anybody could see. Negative cannot be reached by a squared distance.
constexpr float kAbsorbNever = -1.0f;

// HOW LONG A PIECE SHOT OUT OF THE LEVEL LIES THERE.
//
// (user 2026-09-17: "have them be removed after 100 seconds.")
//
// Between the two that already exist and deliberately so: kDebrisLifeMs is 30 s
// -- the lifetime of something you were going to walk over and pick up, which
// these are not any more -- and kFelledLifeMs is half an hour, which for the
// rubble of a firefight is a map that fills with litter and never empties.
constexpr double kLevelChipLifeMs = 100000.0;
// -- ...AND WHAT MAY BE RETIRED TO MAKE ROOM FOR A BIGGER PIECE -----------
//
// (user 2026-09-18: "the pole just deleted itself".)
//
// THERE IS NO CONSTANT HERE, AND ONE WAS TRIED. See World::makeRoomForBodies:
// what a severed piece may evict is anything NO BIGGER THAN ITSELF, which
// scales by construction. A fixed 32-voxel line looked ample against a
// radius-1 chip and measured wrong -- the pool does not fill with chips, it
// fills with 40-to-74-voxel fragments shaken out of the walls, none of which
// the line would have allowed to be retired.
// Nothing loose lives forever. A piece too big to absorb still stops being a
// rigid body eventually, or a morning's chopping is a thousand live actors.
// HOW SMALL A PIECE IS NOT WORTH BEING A PIECE.
//
// v1's "a lone voxel is still not a chunk", applied where a body comes apart
// rather than where one is born: the rim of a bite clips a stray voxel across a
// gap, and without a floor every third blow on a log would spend one of the
// sixty-four debris slots on a speck the size of a thumbnail. Eight voxels is
// eight cubic decimetres.
constexpr int kMinPieceVox = 8;

// -- WHEN A TRUNK IS CUT THROUGH ENOUGH TO GO OVER --------------------------
//
// (user 2026-09-17: "when cutting down a tree, the top half just floats.
//  theres something in the middle blocking it?")
//
// IT WAS ONE VOXEL. Measured with --float-sweep on an oak: 703,068 voxels in
// the model, and after twenty-six landed axe blows exactly ONE of them left
// across the cut row. The sever test is pure connectivity, so that one voxel
// is a six-connected bridge and the whole crown counts as attached -- by the
// letter of the test, correctly. The renderer draws the hole the axe made, the
// tree stands on a thread, and "something in the middle" is that thread.
//
// CONNECTIVITY IS THE WRONG QUESTION FOR A TRUNK. A real tree is felled by
// cutting until the HINGE is too thin to hold, not until the last fibre parts,
// and that is what these two say.
//
// RELATIVE TO HOW THE TREE WAS DRAWN, which is the part that has to be right.
// A pine trunk is about 0.3 m through, so its rows are only a few dozen voxels
// to begin with -- an absolute threshold would fell a birch the moment it
// loaded. bornHinge is the narrowest row the model has UNDAMAGED, and the rule
// is a fraction of that.
constexpr float kHingeFrac = 0.25f;   // a quarter of the trunk left = it goes over
// ...and a floor, so a tree drawn with a naturally thin row does not qualify on
// the frame it appears. Four voxels is 4 cubic decimetres of wood.
constexpr int kHingeFloorVox = 4;
// HOW MUCH HAS TO BE STANDING ON THE HINGE before this is worth firing. A twig
// snapping off the bottom of a sapling is not a felling.
constexpr long kHingeAboveVox = 400;

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
static_assert(v2::VoxelTerrain::kWaveVoxMaxDefault == 0,
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
    // The narrowest row this model has UNDAMAGED -- see kHingeFrac. Measured
    // once, on the template, because the rule is a fraction of it.
    mutable int bornHinge = -1;

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
    // -- ...AND WHICH OF THAT KIND, so a placement can be rebuilt from the
    //    model it was stamped from. See dropScatterUndermined: a flower left
    //    hanging in the air has to become a body, and a body is made of
    //    voxels, and the only copy of those is the template's.
    uint16_t index = 0;
    // -- AND THIS IS THE MODEL'S CORNER, WHICH IS THE TRAP -----------------
    //
    // makeInstance puts the model's (0,0,0) voxel at `placement - half the
    // model`, and this is that translation. So a caller asking "is this
    // placement inside my disc" is really asking about a point up to half a
    // model away from where the thing stands.
    //
    // IT HAS BITTEN AGAIN (user 2026-09-14: "the flowers are still not
    // dissapering when being tilled under"). The bees get away with it because
    // their radii are metres; a hoe's bite is half a metre, which is the same
    // order as the offset, so most of the flowers in a tilled disc tested as
    // being outside it. The old note said "a caller that wants the middle of
    // the thing has to say so itself, because decorAt does not carry the model
    // and cannot know how big it is" -- so now it carries it.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float hx = 0.0f, hz = 0.0f;   // half the model, in world metres
    // Where it actually stands.
    float midX() const { return x + hx; }
    float midZ() const { return z + hz; }
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
    // pine, rock, flower, mushroom, cone, hive, fruit
    int decorKind[7] = {0, 0, 0, 0, 0, 0, 0};
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
    // ...and the oak wood's seven, from v1's own asset folder.
    std::string oakDir = "C:/voxelbit/game/assets/foilage/oak_trees";
    int viewChunks = 12;  // ring radius, in chunks -- app_load.inl sets it

    // HOW MANY CHUNKS A RADIUS ACTUALLY HOLDS, counted rather than approximated.
    //
    // rering walks a DISC (i*i + j*j <= R*R), so anything that sizes itself off
    // the view radius has to count the same lattice points or it is describing a
    // different world than the one that will be resident. The square (2R+1)^2
    // over-states it by 21% and pi*R*R is wrong in BOTH directions depending on
    // R -- 452 against 441 at R=12, 707 against 709 at R=15 -- so neither is a
    // substitute for the count.
    //
    // It is a couple of thousand iterations, run once at build and once per
    // chunk crossing, against a hash insert per point in the loop it sizes.
    // What one chunk is budgeted in the tri pool, in uint16 units. A chunk over
    // this terrain meshes to roughly 200k triangles and carries one unit each,
    // so this is that with about 30% on top.
    static constexpr size_t kChunkUnits = 260000;
    // ...and what the WHOLE pool carries on top of the disc it is sized for.
    // Growing means copying the pool device-side, so this buys "rare" -- the
    // per-chunk margin above is what buys "a rough chunk still fits".
    static constexpr double kPoolSlack = 1.15;

    static size_t discChunks(int r) {
        const int R = r < 1 ? 1 : r;
        const int R2 = R * R;
        size_t n = 0;
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i)
                if (i * i + j * j <= R2) ++n;
        return n;
    }
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
                            {"pinecones", &pinecones_}, {"hives", &hives_},
                            {"fruit", &fruit_}};
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
    int addHeldModel(const std::string &path, int *sx, int *sy, int *sz,
                     int selfMergeTol = 0) {
        VoxModel mo;
        std::string err;
        if (!voxLoad(path, &mo, &err)) {
            std::fprintf(stderr, "v2: held item %s: %s -- skipped\n", path.c_str(), err.c_str());
            return -1;
        }
        return addHeldVox(mo, path, sx, sy, sz, selfMergeTol);
    }

    // The same, for a model that was COMPOSED rather than read. The bow's draw
    // frames are cut out of one file by render/bow.h and never exist on disk as
    // models of their own, so there is nothing for addHeldModel to open; `what`
    // is only ever printed.
    // -- ...AND FOOD IS ALLOWED TO SHARE -------------------------------------
    //
    // `matchTol` of 0 is the old behaviour and is what every TOOL wants: a held
    // colour never merges, because the two dozen voxels in front of the eye are
    // the ones a player looks at closest. A tolerance instead lets a model snap
    // onto colours the table already holds, exactly as the flyer band does.
    //
    // IT EXISTS BECAUSE THE STEAK WOULD NOT FIT. The raw meat is nine colours
    // in two smooth ramps -- five reds twelve apart and four pinks -- and
    // registering all nine exactly took the palette to 255 of 255 and started
    // handing AIR to whatever loaded after it. Which model that was depended on
    // the world seed, which is the worst possible form of this bug: it moved.
    // At kModelMatch the ramps collapse onto each other and onto reds the wood
    // already owns, and a twelve-unit shift on a lump of meat is not a thing
    // anybody can see. See [[v2-palette-is-full]].
    // -- ...AND A MODEL MAY MERGE ITS OWN RAMPS, WHICH IS NOT THE SAME THING --
    //
    // `selfMergeTol` of 0 is the old behaviour and is what every TOOL wants: a
    // held colour never merges, because the two dozen voxels in front of the
    // eye are the ones a player looks at closest.
    //
    // ABOVE 0 IT MERGES WITHIN THIS MODEL AND ONLY WITHIN IT, then registers
    // each survivor EXACTLY. That is the important distinction, and getting it
    // wrong is what this parameter replaced: asking forModelColor for a
    // tolerance merges against the WHOLE TABLE, so a steak's red can be handed
    // a colour the wood already owns -- a shared entry, with whatever alpha or
    // emission that entry carries, and no way to tell from the outside.
    //
    // THE STEAK IS WHY. Raw meat is nine colours in two smooth ramps -- five
    // reds twelve apart and four pinks -- and nine exact entries is more than
    // this palette can spare for a thing the size of a hand. Merged against the
    // world it cost five and wore borrowed colours; merged against ITSELF it
    // costs the same five and wears its own. The ramp loses a step it did not
    // need: twelve units is under half of what the eye resolves on a surface
    // that small.
    int addHeldVox(const VoxModel &mo, const std::string &path, int *sx, int *sy, int *sz,
                   int selfMergeTol = 0) {
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
        // -------------------------------------------------------------------
        // THE TOOLS' GREYS ARE THEIR OWN, AND A TRY AT CHANGING THAT FAILED.
        //
        // "the axe doesnt show the right pallette" / "keep pursuing a fix for
        // the grey pallette on the stone tools. it is not right" (user
        // 2026-09-13). The measurement, so nobody has to take it again:
        //
        //   * The three heads are authored as SEVEN PURE NEUTRALS, 107 to 147,
        //     every channel equal.
        //   * Every colour they ask for they GET. Eight of the axe's fourteen
        //     are byte-exact; the worst shift is 7 of 255 on one grey, where
        //     kQuantStep merges it with a neighbour. Roughness, specular and
        //     translucency are bark's on all fourteen. Nothing is foliage,
        //     nothing comes back AIR, the table is not full.
        //   * Rendered on the asset deck -- white floor, open sky -- the head
        //     measures sRGB (164, 165, 167). Neutral, and bright. The palette
        //     and the renderer are both doing their job.
        //   * Rendered in the birch wood it measures (51, 66, 61). The light
        //     there is green: a WHITE birch trunk in the same frame measures
        //     (147, 160, 133) and the grass (17, 37, 23), while a cloud
        //     measures (235, 235, 236), which is the white balance being right.
        //
        // So the grey is grey and the wood is green. WHAT WAS TRIED: lending
        // the heads the world's own stone, the way the floor borrows the pines'
        // greens and the dug stone borrows the boulders' (see GRASS_0,
        // STONE_0). It is the right instinct and it measured WORSE -- the
        // boulders' ramp is bottom-heavy (50 49 43 ... 142 138 136, four of its
        // six shades under 72), so a head mapped across it rendered at 54
        // against the authored 66 and read as a black slab rather than as
        // stone. There are not enough light shades in this world's stone to
        // carry a tool head's ramp. Reverted.
        //
        // WHAT IS LEFT is the light, and v1 answers it with a VIEW-MODEL LIGHT:
        // heldLight() in its PRE block, "ONE shading model for everything
        // carried in front of the eye", built from sun, sky and a WARM GROUND
        // BOUNCE -- whose own note records the same symptom from the other
        // side, "without it side faces are sky-only (cool + dark) and the axe
        // handle read grey-brown". v2 has no such thing on purpose: the user's
        // rule is that everything is hit in the same shader pass with the same
        // lighting, and a held item lit by its own lamp is exactly the
        // exception that rule forbids. That trade is the user's to make.
        // -------------------------------------------------------------------
        // -------------------------------------------------------------------
        // A HELD ITEM KEEPS ITS OWN SHADING RAMP.
        //
        // See Palette::kQuantStep. The world's colours merge when they are
        // within ten of each other, because 372 colours do not fit in a table
        // of 255 -- and the stone axe's head is a SEVEN STEP grey ramp whose
        // steps are six and seven apart. Merging took two of them away and
        // flattened nineteen hand-painted voxels into five shades. That is what
        // "the stone tools' palette is not right" was, and it arrived with the
        // fix for the overflow rather than before it.
        //
        // TWO WAYS TO BE WRONG, AND THE FIRST FIX ONLY CAUGHT ONE OF THEM.
        //
        // A held colour can lose a step against ITSELF -- two of its own shades
        // in one bucket -- and it can be bent to a colour the WORLD already
        // minted. An earlier pass here fixed only the first, so the axe kept
        // all seven of its greys and then three of them rendered as somebody
        // else's stone: 113 came back as 111, 134 as 130, 140 as 144.
        //
        // v1 HIT THIS AND ITS NOTE IS THE SPECIFICATION. Beside its own held
        // axe loader, word for word:
        //
        //     "PIN THE HELD TOOL'S COLOURS INTO THE PALETTE, EXACTLY ... the
        //      noTol exemption that keeps the rest of the stone kit exact
        //      cannot reach it ... At PAL_TOL 8 that put one axe shade 7/255
        //      off ... the tool the player stares at stays byte-accurate."
        //
        // PAL_TOL 8 is this file's kQuantStep 10, "one axe shade 7/255 off" is
        // the same measurement taken here, and the answer is the same answer:
        // the held kit is exempt. Not the colours that happen to collide -- ALL
        // of them, which is the only description of a held item's colour worth
        // having.
        //
        // AND IT IS EVERY HELD ITEM, not only the stone kit v1 names, because
        // the measurement says the rest is free: the table reads 250 of 255
        // with the three tools exempt and 250 of 255 with all of them, the
        // bow's own colours being already distinct at this tolerance. There is
        // no reason to leave a bowstring four units off for nothing.
        //
        // WHAT IT COSTS is ten entries against the world's merged 240, and the
        // report prints the total at every start. The world's 372 colours still
        // quantise; it is only the two dozen voxels in front of the eye that do
        // not.
        // -------------------------------------------------------------------
        // -- THE MODEL'S OWN RAMPS, FOLDED FIRST -- see selfMergeTol -------
        //
        // Each used entry either becomes a representative or points at an
        // earlier one within tolerance. Only representatives reach the palette,
        // and they reach it EXACTLY.
        std::vector<int> repOf(256, 0);
        if (selfMergeTol > 0) {
            std::vector<int> reps;
            for (int e = 1; e <= 255; ++e) {
                if (!used[size_t(e)]) continue;
                const std::array<uint8_t, 4> &c = mo.pal[size_t(e) - 1];
                int hit = 0;
                for (int r : reps) {
                    const std::array<uint8_t, 4> &q = mo.pal[size_t(r) - 1];
                    const int dr = int(q[0]) - int(c[0]), dg = int(q[1]) - int(c[1]),
                              db = int(q[2]) - int(c[2]);
                    if (dr * dr + dg * dg + db * db <= selfMergeTol * selfMergeTol) {
                        hit = r;
                        break;
                    }
                }
                if (hit) {
                    repOf[size_t(e)] = hit;
                } else {
                    reps.push_back(e);
                    repOf[size_t(e)] = e;
                }
            }
        }
        int minted = 0, lost = 0;
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)]) {
                // A MERGED ENTRY TAKES ITS REPRESENTATIVE'S ID, which has
                // already been minted because reps are met first in this walk.
                const int src = (selfMergeTol > 0 && repOf[size_t(e)] != e) ? repOf[size_t(e)] : e;
                if (src != e) {
                    idOfEntry[size_t(e)] = idOfEntry[size_t(src)];
                    continue;
                }
                // NOT A CONIFER, AND THE SEEDS ARE HOW THAT WAS FOUND.
                // forModelColor's second argument turns any GREEN-DOMINANT
                // colour into pine foliage: albedo x 1.7, translucency 0.45,
                // waxy roughness -- which is right for a needle and is nonsense
                // for anything else. The kit passed `true` since it was written
                // and it never mattered, because a stone axe and a wooden bow
                // have no green in them.
                //
                // seeds.vox IS THREE GREENS. Measured: "worst colour shift
                // 47/255" on a model registered EXACTLY, which is a
                // contradiction until you see that the entry is not being
                // stored as asked. The held seeds were being drawn as three
                // translucent pine needles, and that is also why they did not
                // match the seeds planted in the ground -- those come from the
                // fixed palette range, which no foliage rule touches.
                //
                // addFlyerModel already passes false and says why: "what the
                // green rule does to a lime butterfly's wing".
                idOfEntry[size_t(e)] = palette.forModelColor(
                    mo.pal[size_t(e) - 1], /*conifer=*/false, /*exact=*/true);
                if (idOfEntry[size_t(e)] != mat::AIR)
                    ++minted;
                else
                    ++lost;
            }
        // THE SHIFT REPORT BELOW CANNOT SEE THIS. It compares the colours that
        // resolved, and skips the ones that did not -- so a held model starved
        // by a full table reported "worst colour shift 0/255" while its voxels
        // quietly stopped being drawn. Count them here, where the answer is
        // still in hand.
        if (lost)
            std::fprintf(stderr,
                         "v2: held item %s -- PALETTE FULL, %d of its colours came back AIR; "
                         "those voxels will not be drawn\n",
                         path.c_str(), lost);
        // -- AND THE REPORT IS A MEASUREMENT, NOT A CLAIM ------------------
        //
        // Every colour is read back OUT of the table it was just written to and
        // compared with the one the artist picked. "Byte-accurate" is then
        // something this line has checked rather than something the code above
        // believes about itself -- which is the difference between catching the
        // 7/255 shift and shipping it twice.
        {
            int worst = 0;
            for (int e = 1; e <= 255; ++e) {
                if (!used[size_t(e)] || idOfEntry[size_t(e)] == mat::AIR) continue;
                const std::array<uint8_t, 4> &want = mo.pal[size_t(e) - 1];
                const Vec3 &got = palette[idOfEntry[size_t(e)]].albedo;
                const float lin[3] = {got.x, got.y, got.z};
                for (int k = 0; k < 3; ++k) {
                    // Linear back to sRGB -- the inverse of what forModelColor
                    // stored, so the comparison is in the units the file is in.
                    const float l = maxf(0.0f, minf(1.0f, lin[k]));
                    const float srgb = l <= 0.0031308f
                                           ? l * 12.92f
                                           : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
                    const int d = int(srgb * 255.0f + 0.5f) - int(want[size_t(k)]);
                    worst = maxi(worst, d < 0 ? -d : d);
                }
            }
            std::printf("  held     %-58s worst colour shift %d/255\n",
                        path.c_str() + (path.size() > 58 ? path.size() - 58 : 0), worst);
        }
        // The entries this just minted are not on the GPU yet: init() uploaded
        // the table before this was called, because the world has to exist
        // before anything can be held in front of it.
        uploadMaterials();

        // -- AND THESE IDS MUST MEAN THE SAME THING IN EVERY PLACE ----------
        //
        // The kit is the one thing that travels: you carry it through [O] into
        // a level that has a palette table of its own, so every entry a held
        // model wears has to be reserved out of that table's allocator or the
        // axe in your hand comes out wearing a wall. Recorded here, where the
        // ids are in hand, rather than inferred later from the models --
        // World::buildLevelPalette only has to read the mask.
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)] && idOfEntry[size_t(e)] != mat::AIR)
                heldMtl_[size_t(idOfEntry[size_t(e)])] = 1;

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
        // -------------------------------------------------------------------
        // `exact=true`, AND IT IS THE WHOLE OF THIS FUNCTION'S CORRECTNESS.
        //
        // THE BOW LOST VOXELS THE FIRST TIME ITS ARROW WAS NUDGED (user
        // 2026-09-14, "the bow is broken, missing voxels"), and this line was
        // it. addHeldVox asks for every held colour with exact=true; an exact
        // request is keyed `kExactKeyBit | rgb` and a quantised one by the
        // colour over kQuantStep, and **those are two different key spaces on
        // purpose** -- see the note over forModelColor, where the separation is
        // what stops an exact request landing in a rough colour's bucket.
        //
        // So asking here WITHOUT exact did not find the entries the load had
        // minted. It missed all twelve and minted eleven of them a SECOND time
        // in the rough space. Measured on the real file, from an empty table:
        //
        //     bow loads          38 -> 50 entries   (12 distinct colours)
        //     one arrow nudge    50 -> 61 entries   (+11, the same colours)
        //     a second nudge     61 -> 61           (the rough keys cache)
        //
        // A plain start already leaves the table at 254 of 255, so those eleven
        // do not fit: forModelColor returns mat::AIR, meshAsset builds no face
        // for those voxels, and the bow comes back with holes in it -- once,
        // permanently, on the first nudge. The colours that DID resolve came
        // back quantised, which is separately the exact thing the note in
        // addHeldVox spends eighty lines making sure never happens to a model
        // held in front of the eye.
        //
        // The comment below was always right about what SHOULD happen. It is
        // true now.
        // -------------------------------------------------------------------
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)])
                idOfEntry[size_t(e)] =
                    palette.forModelColor(mo.pal[size_t(e) - 1], true, /*exact=*/true);
        // ONLY IF THE MODEL ACTUALLY BROUGHT A COLOUR. A rebuild is the same
        // file it was first read from, so every entry is already in the table
        // and forModelColor hands back the id it minted at load. Re-uploading
        // anyway would create a fresh materials buffer for every frame of the
        // strip, on every nudge, to say what the table already said.
        if (palette.minted() != before) uploadMaterials();
        // AND IF ANYTHING STILL CAME BACK AIR, SAY SO. Silence is how this bug
        // shipped: a held colour served AIR is skipped by the shift report in
        // addHeldVox, so the start-up line read "worst colour shift 0/255"
        // while voxels were going missing. A model that cannot have its colours
        // is a model with holes in it, and that is worth a line.
        {
            int lost = 0;
            for (int e = 1; e <= 255; ++e)
                if (used[size_t(e)] && idOfEntry[size_t(e)] == mat::AIR) ++lost;
            if (lost)
                std::fprintf(stderr,
                             "v2: %s -- PALETTE FULL, %d of its colours came back AIR; "
                             "those voxels will not be drawn\n",
                             what.c_str(), lost);
        }

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
    int addFlyerModel(const VoxModel &mo, const std::string &what, int *sx, int *sy, int *sz,
                      bool keepVoxels = false) {
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
                //
                // ...AND IT MAY SHARE. Every model in this band is an animal a
                // few voxels across; between them they wanted 158 of the ~194
                // entries this palette has to give, which is what put it at 255
                // of 255 and started handing AIR to whatever loaded last -- "the
                // newly imported life is missing voxels". A band colour within
                // kModelMatch of one the table already holds takes that entry;
                // anything else is minted exactly as authored, which is what
                // keeps a black animal black.
                idOfEntry[size_t(e)] = palette.forModelColor(mo.pal[size_t(e) - 1], false, false,
                                                             Palette::kModelMatch);
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
        if (keepVoxels) fm.vol = a.a;   // see HeldModel::vol
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
        // A SPIN WITH NO ANGLE IS NOT A SPIN -- see setFlyerInstance, where the
        // same hole cost the perched songbirds three frames in four. A drop
        // that happens to be at rest would otherwise publish its axis with a
        // zero angle, and the tracer reads that as a wing measurement.
        const bool spinning = ok && spin && spin[3] != 0.0f;
        instanceInfos_[idx].flap =
            spinning ? float3(spin[0], spin[1], spin[2]) : float3(0.0f, 0.0f, 0.0f);
        instanceInfos_[idx].flapPad = spinning ? spin[3] : 0.0f;
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
    // `anchor`, when given, is a world point FIXED TO THE ANIMAL that the
    // motion vector is measured at instead of the model's own box centre --
    // which on a strip whose frames are different sizes is not fixed to
    // anything. See place().
    void setFlyerInstance(int slot, int model, const float *m, float tx, float ty, float tz,
                          const float *flap, bool show, const float *spin = nullptr,
                          const float *anchor = nullptr) {
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
              0.5f * float(flyers_[mi].sz) * VOXEL_M, true, ok ? anchor : nullptr);
        instanceDescs_[idx].accelerationStructure = flyers_[mi].blas.as->getGpuAddress();
        instanceInfos_[idx].triOffset = flyers_[mi].tri;
        // -- THE TWO CHANNELS ARE EXCLUSIVE, AND THIS IS WHERE THAT IS MADE
        //    TRUE RATHER THAN ASSUMED ---------------------------------------
        //
        // `flap` carries ONE of two completely different readings and
        // `flapPad` is the discriminator: non-zero means "this instance spun by
        // this many radians and flap is the world AXIS", zero means "flap is a
        // wing measurement". Shared.slang says the two are exclusive by
        // construction; they were not.
        //
        // A PERCHED SONGBIRD PUBLISHES ITS AXIS EVERY FRAME AND TURNS ON ONE IN
        // FOUR. Its eleven frames step every 66 ms, so at 60 fps three frames
        // in four hold a pose and report dth = 0 -- and a zero angle left
        // flapPad at zero while flap still held the perch, which the tracer
        // then read as a wing step. The arithmetic of that is not subtle:
        //
        //     outAlong = |xObj - flap.z| / kVoxelM      flap.z is the perch's
        //                                               world Z, so this is
        //                                               thousands, not units
        //     rise     = flap.y                         ...which is the perch's
        //                                               world Y, in METRES
        //     h.moved.y += rise
        //
        // So on three frames in four every perched bird reported that it had
        // moved its own altitude -- tens of metres -- straight up. Ray
        // Reconstruction fetched history from somewhere off the screen, got
        // nothing usable, and rebuilt those pixels from whatever the current
        // frame had: at DLAA that is a full sample per pixel and it very nearly
        // gets away with it, at Balanced it is 42% of one. Reported twice as
        // "the birds are flickering", the second time as "it's like the SHADING
        // is flickering", which is exactly what a rejected history looks like.
        //
        // A spin with no angle is not a spin. One test, in the one place that
        // writes both fields, so no caller can get this wrong again.
        const bool spinning = ok && spin && spin[3] != 0.0f;
        instanceInfos_[idx].flap =
            spinning       ? float3(spin[0], spin[1], spin[2])
            : (ok && flap) ? float3(flap[0], flap[1], flap[2])
                           : float3(0.0f, 0.0f, 0.0f);
        instanceInfos_[idx].flapPad = spinning ? spin[3] : 0.0f;
        // -- ...AND WHAT THIS SLOT IS, WHICH NOTHING USED TO ASK ------------
        //
        // The band is the one place every animal in the world is drawn, which
        // makes it the one place to ask "what is under the crosshair" without
        // asking seven populations the same question seven ways. Two numbers
        // are all that takes: which model (so a kill can rebuild the body from
        // its voxels) and how big it is (so a ray can be tested against it).
        // WHERE it is needs nothing new -- place() already records the model's
        // world centre in wasAt_, because a motion vector needs exactly that.
        flyerModel_[size_t(slot)] = ok ? int16_t(model) : int16_t(-1);
        flyerR_[size_t(slot)] =
            ok ? (float(flyers_[mi].sx + flyers_[mi].sy + flyers_[mi].sz) * VOXEL_M / 6.0f) : 0.0f;
        flyersDirty_ = true;
    }

    // -----------------------------------------------------------------------
    // WHERE A DRAWN FLYER IS AND HOW BIG IT IS.
    //
    // `r` is the MEAN half-extent, which is v1's own choice for its aim test
    // and it says why: "a sphere drawn around the corners of the box is far
    // wider than the animal actually looks, and that generosity is the
    // complaint being fixed."
    //
    // A SLOT WITH MASK 0 ANSWERS FALSE. That is the whole of "is this animal
    // on the field" as far as anything outside the population is concerned --
    // it is what --locate-test already reads through flyerShown.
    // -----------------------------------------------------------------------
    bool flyerAt(int slot, Vec3 *at, float *r) const {
        if (slot < 0 || slot >= kFlyerInstances || flyerBase_ < 0) return false;
        const size_t idx = size_t(flyerBase_ + slot);
        if (idx >= instanceDescs_.size() || !instanceDescs_[idx].instanceMask) return false;
        if (flyerModel_[size_t(slot)] < 0) return false;
        if (at) *at = Vec3(wasAt_[idx].x, wasAt_[idx].y, wasAt_[idx].z);
        if (r) *r = flyerR_[size_t(slot)];
        return true;
    }

    // -----------------------------------------------------------------------
    // THIS ANIMAL HAS JUST BEEN STRUCK, AND BY HOW MUCH IT IS STILL GLOWING.
    //
    // See V6Instance::hurt for what the shader does with it. Written straight
    // into the host's copy and picked up by the next flushFlyerInstances, which
    // the population's own publish already calls every frame -- so a flash
    // costs one float and no extra upload.
    //
    // WRITTEN AFTER THE POPULATION'S PUBLISH, not before: setFlyerInstance does
    // not touch this field, but a slot that stops being drawn keeps whatever it
    // last held, and a recycled slot would then be born bleeding. LifeHits
    // clears it to zero when the blink ends for exactly that reason.
    // -----------------------------------------------------------------------
    void setFlyerHurt(int slot, float k) {
        if (flyerBase_ < 0 || slot < 0 || slot >= kFlyerInstances) return;
        const size_t idx = size_t(flyerBase_ + slot);
        if (idx >= instanceInfos_.size()) return;
        if (instanceInfos_[idx].hurt == k) return;   // nothing changed: no upload
        instanceInfos_[idx].hurt = k;
        flyersDirty_ = true;
    }

    // The quarter turn the slot is drawn with. Only the yaw: see shatterFlyer.
    float flyerYaw(int slot) const {
        if (slot < 0 || slot >= kFlyerInstances || flyerBase_ < 0) return 0.0f;
        const size_t idx = size_t(flyerBase_ + slot);
        if (idx >= instanceDescs_.size()) return 0.0f;
        return std::atan2(instanceDescs_[idx].transform[0][2], instanceDescs_[idx].transform[0][0]);
    }

    // -----------------------------------------------------------------------
    // AND IT COMES APART -- v1'S phShatter, ON THE BAND.
    //
    // (user 2026-09-14: "when killing life, the life breaks apart into multiple
    // pieces".)
    //
    // EIGHT OCTANTS OF THE MODEL'S OWN BOX, which is v1's rule and its own
    // reason: "the 2x2x2 split of the body's own local bbox is what makes them
    // read as broken parts rather than confetti -- each piece is a contiguous
    // quadrant of the animal." A lone voxel is litter, not a piece, and
    // spawnDebris already refuses those (count < 2).
    //
    // THE CUBE IS THE MODEL, CENTRED. spawnDebris takes an n-cube and places
    // the body at the voxels' own centre of mass measured from that cube's
    // middle -- so handing it the SAME centre for all eight, with each octant's
    // voxels in their own part of the cube, puts every piece exactly where that
    // part of the animal was. Nothing has to work out where a quadrant goes.
    //
    // YAW ONLY, AND THAT IS A SIMPLIFICATION worth naming. A walking animal is
    // drawn with a yaw and nothing else, so for the mammals, the frog and the
    // ant this is exact. A butterfly or a fish also rolls and pitches, and its
    // pieces are born square to the world instead. They are tumbling within a
    // frame -- omega is up to 3.5 rad/s -- so what it costs is the first
    // frame's orientation, and what it buys is not carrying a second rotation
    // convention through a path that has never needed one.
    //
    // Returns how many pieces actually made it into the world.
    // -----------------------------------------------------------------------
    // The two rolls a shatter needs. A corpse coming apart is the one thing in
    // this file that WANTS to differ run to run, so it takes the ordinary
    // generator rather than the world hash everything else here is built on:
    // the hash exists so a place looks the same every time you walk to it, and
    // a death is not a place.
    static float unitRand() { return float(std::rand()) / float(RAND_MAX); }
    static float jitter(float span) { return (unitRand() - 0.5f) * span; }

    int shatterFlyer(Physics &ph, int slot, const Vec3 &awayFrom, double nowMs,
                     uint8_t takesAs = kDebrisSoft) {
        lastDebrisMs_ = nowMs;
        Vec3 at{0.0f, 0.0f, 0.0f};
        float r = 0.0f;
        if (!flyerAt(slot, &at, &r)) return 0;
        const int mi = flyerModel_[size_t(slot)];
        if (mi < 0 || size_t(mi) >= flyers_.size()) return 0;
        const HeldModel &fm = flyers_[size_t(mi)];
        if (fm.vol.empty()) return 0;   // a model loaded without its voxels -- see HeldModel::vol
        const int sx = fm.sx, sy = fm.sy, sz = fm.sz;
        const int n = maxi(maxi(sx, sy), sz);
        if (n < 2) return 0;
        // -- AND NOT ONE OF THEM IS BORN INSIDE THE GROUND -----------------
        //
        // (user 2026-09-15: "there is a peice of the life that gets launched
        // upwards".)
        //
        // THIS IS WHERE THE LAUNCH COMES FROM. The pieces are born about the
        // animal's CENTRE, and an animal stands ON the ground -- so the lower
        // octants start at or below the surface, which means they start inside
        // the collision height field. A solver's first duty to a body that is
        // already penetrating is to get it out, and it does that with an
        // impulse proportional to how deep it is: one piece leaves like a
        // rocket while its neighbours fall normally.
        //
        // It is also why they went THROUGH the floor -- a body that starts
        // below a height field is not resting on it, it is under it, and
        // nothing below a height field is ever pushed back up by contact.
        //
        // So the whole burst is lifted until the model's own box clears the
        // ground. A corpse coming apart half a voxel higher than the animal
        // stood is not a thing anybody can see; being fired into the sky is.
        Vec3 born = at;
        {
            TerrainMemo memo;
            const int gi = int(std::floor(born.x / VOXEL_M));
            const int gj = int(std::floor(born.z / VOXEL_M));
            const float ground = float(terrainTopAt(gi, gj, memo) + 1) * VOXEL_M;
            const float halfY = 0.5f * float(sy) * VOXEL_M;
            if (born.y - halfY < ground) born.y = ground + halfY;
        }
        // The model, centred in an n-cube. Rounded DOWN so the offset matches
        // the half-box setFlyerInstance hands place(), which is what put the
        // instance where it is.
        const int ox = (n - sx) / 2, oy = (n - sy) / 2, oz = (n - sz) / 2;
        const float yaw = flyerYaw(slot);
        const float cy = std::cos(yaw), sy2 = std::sin(yaw);
        int made = 0;
        std::vector<uint8_t> cube(size_t(n) * size_t(n) * size_t(n), mat::AIR);
        for (int q = 0; q < 8; ++q) {
            std::fill(cube.begin(), cube.end(), uint8_t(mat::AIR));
            int count = 0;
            for (int y = 0; y < sy; ++y)
                for (int z = 0; z < sz; ++z)
                    for (int x = 0; x < sx; ++x) {
                        const uint8_t v =
                            fm.vol[size_t(x) + size_t(z) * size_t(sx) + size_t(y) * size_t(sx) *
                                                                            size_t(sz)];
                        if (v == mat::AIR) continue;
                        const int oct = (x * 2 >= sx ? 1 : 0) | (y * 2 >= sy ? 2 : 0) |
                                        (z * 2 >= sz ? 4 : 0);
                        if (oct != q) continue;
                        cube[size_t(x + ox) + size_t(z + oz) * size_t(n) +
                             size_t(y + oy) * size_t(n) * size_t(n)] = v;
                        ++count;
                    }
            if (count < 2) continue;
            // OUTWARD FROM THE ANIMAL'S OWN MIDDLE, turned into the world by
            // the same yaw the body is born with -- v1: "so the pieces open up
            // rather than all going one way". Its 7 and 6 voxels a second are
            // 0.7 and 0.6 m/s here.
            const float mx = (q & 1) ? 1.0f : -1.0f, mz = (q & 4) ? 1.0f : -1.0f;
            const float wx = mx * cy + mz * sy2, wz = -mx * sy2 + mz * cy;
            // ...and the blow that killed it carries the whole body away from
            // whoever struck it, which is the other half of v1's creatureRagdoll.
            const float dx = at.x - awayFrom.x, dz = at.z - awayFrom.z;
            const float d = maxf(0.001f, std::sqrt(dx * dx + dz * dz));
            // -- AND THEY POP (user 2026-09-15: "have the red peices of the
            //    killed life bounce upwards slightly? it could be a nice
            //    effect", then "have the life pop up more upon death. its not
            //    really noticable") --------------------------------------
            //
            // v1's 6 + rand*4 voxels a second is 0.6-1.0 m/s, which against
            // this engine's gravity is a rise of FOUR CENTIMETRES -- the pieces
            // essentially fall apart in place. A rise is v*v/2g and that is
            // what has to be aimed at, not the speed: tripling the speed to
            // 1.9-2.9 bought 0.18-0.43 m, which is a hop you have to be looking
            // for, and being looked for is exactly what it failed at.
            //
            //     0.6-1.0 m/s   v1's own          0.02-0.05 m   invisible
            //     1.9-2.9 m/s   the first cut     0.18-0.43 m   "not really
            //                                                    noticable"
            //     3.8-5.1 m/s   here              0.74-1.32 m   a clear pop
            //
            // A METRE IS THE CEILING AND IT IS DELIBERATE: a piece that clears
            // the player's own height reads as debris thrown by an explosion,
            // and the thing this must never become again is the rocket the
            // birth-inside-the-ground bug produced -- that was metres, straight
            // up, one piece out of eight. --kill-test still watches for it.
            const Vec3 vel{wx * 0.7f + (dx / d) * 0.9f + jitter(0.4f),
                           3.8f + 1.3f * unitRand(),
                           wz * 0.7f + (dz / d) * 0.9f + jitter(0.4f)};
            const Vec3 spin{jitter(7.0f), jitter(7.0f), jitter(7.0f)};
            const int got =
                spawnDebris(ph, cube, n, born, vel, spin, nowMs, yaw, nullptr, takesAs);
            if (got < 0) continue;
            // A CORPSE IS NOT LOOT -- v1's own words, and its own reason: "the
            // player seems to absorb the life when it breaks into chunks". This
            // is what that flag is for.
            markScenery(got);
            // ...AND IT IS STILL BLEEDING. See corpseFade.
            debris_[got].hurtT0 = nowMs;
            ++made;
        }
        return made;
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
    // A BODY'S ROTATION AS A MATRIX, row major, body axes into world.
    //
    // Written out three times before this: here to draw with, in debrisClip to
    // ask where the shapes are, and in the swing ray to go the other way. One
    // of the three being wrong is a thing you see rather than a thing you can
    // debug, so there is one of them now.
    static void quatMat3(const float *q, float *m) {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        m[0] = 1.0f - 2.0f * (y * y + z * z);
        m[1] = 2.0f * (x * y - z * w);
        m[2] = 2.0f * (x * z + y * w);
        m[3] = 2.0f * (x * y + z * w);
        m[4] = 1.0f - 2.0f * (x * x + z * z);
        m[5] = 2.0f * (y * z - x * w);
        m[6] = 2.0f * (x * z - y * w);
        m[7] = 2.0f * (y * z + x * w);
        m[8] = 1.0f - 2.0f * (x * x + y * y);
    }

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
        float m[9];
        quatMat3(q, m);
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
        // -- ...AND A PIECE OF A CORPSE IS STILL RED WHILE IT FLIES ---------
        //
        // (user 2026-09-14: "when hitting life, it turns an emmisive red ...
        // when killing life, the life breaks apart into multiple pieces".)
        //
        // v1 IS EXPLICIT THAT THESE TWO OVERLAP, and it got there the same way:
        // "the chunks should play AS the red animation plays ... the chunks
        // should get the red emissive voxels". Its first cut shattered the body
        // when the flash ENDED, so the animal stayed whole for the whole half
        // second and burst into ordinary grey pieces afterwards -- the two
        // never shared a frame.
        //
        // Here the animal's own instance goes dark on the frame it dies -- it
        // is retired from its population -- so without this the flash is
        // something you can only see on a blow that does NOT kill. The pieces
        // carry it instead, on the same clock and the same fade, which is both
        // v1's picture and the only place left to put it.
        instanceInfos_[idx].hurt = corpseFade(d.hurtT0, lastDebrisMs_);
        debrisDirty_ = true;
    }

    // v1's blink, as a fade: full at the cut, out after kHurtMs, and nothing at
    // all for every ordinary chip -- hurtT0 stays at its birth sentinel unless
    // shatterFlyer sets it.
    // THE TIME RATHER THAN THE BODY, because a member function's SIGNATURE has
    // to name types that are already declared and Debris is four thousand lines
    // below this. The body could have said d.hurtT0 quite happily.
    // -- RED FOR AS LONG AS IT EXISTS, AND THAT IS THE WHOLE RULE --------
    //
    // (user 2026-09-15: "you also seems to have 2 different versions of the
    // dead life, you have the red peices but also regular peices. there should
    // be no regular peices when a life dies. just red ones.")
    //
    // THE TWO VERSIONS WERE THE SAME PIECES, BEFORE AND AFTER. This faded to
    // ZERO at 500 ms -- v1's blink -- but v1 also DELETES its pieces at 500 ms,
    // so the fade ending and the piece ending are one moment there. Here the
    // pieces now lie on the ground for half a second after they settle, which
    // is longer: so for the rest of that time they were ordinary grey debris
    // sitting where an animal died. Two versions of the dead life, exactly as
    // described, and both of them this one.
    //
    // So the blink runs as v1 steps it -- twelve phases over the first half
    // second -- and then HOLDS rather than reaching zero. A corpse piece is
    // never not red; it is only ever less red.
    static float corpseFade(double hurtT0, double nowMs) {
        if (hurtT0 < -1e8) return 0.0f;
        const double e = (nowMs - hurtT0) / 500.0;   // kHurtMs, and see lifehit.h
        if (e < 0.0) return 0.0f;
        if (e >= 1.0) return kCorpseRedFloor;
        const int phase = int(e * 12.0);   // twelve phases, as v1 steps it
        const float f = 1.0f - float(phase) / 12.0f;
        return f < kCorpseRedFloor ? kCorpseRedFloor : f;
    }

    void retireDebris(Physics &ph, int slot) {
        Debris &d = debris_[slot];
        d.borrowAs = 0;
        d.borrowTri = TriPool::kInvalid;
        d.tint = float3(1.0f, 1.0f, 1.0f);
        d.felled = false;
        d.scenery = false;
        // Structure and triangles both go back, once the device is done with
        // them -- a chunk that vanished this frame was still being drawn last
        // frame. See retireLoose.
        retireLoose(std::move(d.blas), d.triOffset, d.tris);
        d.triOffset = TriPool::kInvalid;
        d.tris = 0;
        // A COUPLE OF MEGABYTES ON A PINE -- the whole model box, most of it
        // air -- and a slot is reused the moment the next thing falls, so this
        // is the one place it has to go back.
        d.vox.clear();
        d.vox.shrink_to_fit();
        d.vsx = d.vsy = d.vsz = 0;
        d.boxes.clear();
        d.takesAs = kDebrisStone;
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
    // Set one entry's alpha and push the table again. The fly's wing is the
    // only caller -- see Critters::load.
    void setMaterialAlpha(uint8_t id, float a) {
        palette.setAlpha(id, a);
        uploadMaterials();
    }

    // ...and the whole dielectric surface at once -- see Palette::setGlass.
    void setMaterialGlass(uint8_t id, float a, float rough, float spec, float diffuse) {
        palette.setGlass(id, a, rough, spec, diffuse);
        uploadMaterials();
    }

    // -- WHICH TABLE THE DEVICE IS HOLDING ----------------------------------
    //
    // The wood's, or the level's -- see Palette::beginLevelTable. Swapped at
    // the door by setLevel and nowhere else, because the two places are never
    // in the acceleration structure together and one upload is 255 entries.
    //
    // EVERY OTHER CALLER OF THIS STILL WORKS. A model loaded while the level is
    // open would re-upload the level's table, which is correct: it is the table
    // in force. Nothing loads models in there today.
    void uploadMaterials() {
        const std::vector<MaterialLook> &src =
            (levelTableActive_ && palette.hasLevelTable()) ? palette.levelTable() : palette.table();
        std::vector<V6Material> mats(src.size());
        for (size_t i = 0; i < mats.size(); ++i) {
            const MaterialLook &m = src[i];
            mats[i].albedo = float3(m.albedo.x, m.albedo.y, m.albedo.z);
            mats[i].roughness = m.roughness;
            mats[i].specular = m.specular;
            mats[i].translucency = m.translucency;
            mats[i].alpha = m.alpha;
            // See V6Material::ior -- 0 for everything that is not the smoke.
            mats[i].ior = m.ior;
        }
        // -- IN PLACE IF IT CAN BE, AND IT ALWAYS CAN ------------------------
        //
        // THIS USED TO REPLACE THE BUFFER EVERY TIME AND THAT IS A BUG THE
        // MOMENT ANYTHING CALLS IT AFTER START-UP. V2Tracer binds
        // `gMaterials = world_->materialBuffer()` when it sets its program
        // vars, so a NEW buffer object here leaves every one of those bindings
        // pointing at the old one -- the host's table is right, the device's is
        // whatever it was.
        //
        // It went unnoticed for as long as it did because every previous caller
        // ran while the world was loading, before the tracer had bound
        // anything. Swapping the level's table at the door is the first caller
        // that does not, and the symptom was the map rendering in the WOOD's
        // colours: concrete came out at the wood's entry 243, which is a red.
        // The host print said the level's table had been uploaded, and it had.
        //
        // The table is mat::COUNT entries whichever place is open, so the
        // capacity never changes and the update is a straight overwrite.
        const uint32_t n = uint32_t(mats.size());
        if (materials_ && materials_->getElementCount() == n) {
            ctx_->updateBuffer(materials_.get(), mats.data(), 0, n * sizeof(V6Material));
            return;
        }
        materials_ = device_->createStructuredBuffer(sizeof(V6Material), n,
                                                     ResourceBindFlags::ShaderResource,
                                                     Falcor::MemoryType::DeviceLocal, mats.data());
        materials_->setName("v2::materials");
    }

    // Ask for a re-mesh of everything an edit moved. A chunk that is not
    // resident is not a problem -- the edit is stored, so it meshes WITH the
    // change when it streams in.
    // -----------------------------------------------------------------------
    // AN EDIT'S CHUNKS ARE URGENT, AND THAT IS NOT WHAT STREAMING IS.
    //
    // (user 2026-09-17: "the tilling with the hoe is still glitching out the
    //  terrain".)
    //
    // update() adopts `buildBudget` chunks a frame -- TWO -- which is right for
    // streaming, where the chunks coming in are at the edge of sight and
    // nobody can tell what order they land in. An EDIT is the opposite case:
    // it is under the crosshair, the player caused it, and its chunks are a
    // single shape that has to appear at once. A hoe bite is 2.8 m across and
    // straddles chunk seams, and writeCells also republishes the NEIGHBOUR of
    // every cell within a voxel of one -- so one swing asks for several chunks
    // and at two a frame the tilled circle arrives in PIECES over several
    // frames. That is the terrain "glitching out": not a hole, not a stall, a
    // disc that materialises a slab at a time.
    //
    // So chunks requested by an edit are remembered, and update() lifts its
    // budget by however many are still outstanding. It cannot run away: the set
    // only grows when the player edits something and every entry leaves it on
    // adoption or eviction.
    void remesh(const std::vector<std::pair<int, int>> &touched) {
        lastRemesh = 0;
        for (const auto &c : touched) {
            const long long k = chunkKey(c.first, c.second);
            if (!chunks_.count(k)) continue;
            requested_.insert(k);
            urgent_.insert(k);
            ++lastRemesh;
            // TO THE FRONT OF THE MESH QUEUE, not just the front of the adopt
            // budget -- see ChunkMesher::request. Raising the budget alone was
            // measured and changed nothing: the chunk had not been MESHED yet.
            mesher_.request(c.first, c.second, /*urgent=*/true);
        }
    }
    // How many chunks the last edit asked to be re-meshed -- for --swing-log.
    int lastRemesh = 0;

    // ONE TURNED COLUMN, and everything needed to put it back exactly -- v1's
    // `tilled` array by another name. `y` is the voxel that came away; y-1 is
    // the one that became tilled earth.
    struct Till {
        int i = 0, j = 0, y = 0;
        uint8_t prevTop = 0, prevBelow = 0;
        float at = 0.0f;
        // ...AND WHETHER A SEED WENT IN IT. See plantAt: this is the whole of
        // what planting means here, and it is what stops the revert.
        bool planted = false;
        // ...AND WHETHER A SEED IS OWED BACK WHEN THIS BED GOES. Set on ONE
        // column of a planting -- see plantAt.
        bool seeded = false;
        // The scatter this bite put away, as chunk key + decor slot. Only the
        // FIRST column of a bite carries the list -- one bite is one disc and
        // one set of flowers, and hanging a copy off all eighty-one columns
        // would show them again on the first column to come back rather than
        // the last. See hideScatterOn.
        std::vector<std::pair<long long, int>> hidden;
    };
    std::vector<Till> tilled_;
    // v1's TILL_MS, in seconds.
    static constexpr float kTillSec = 45.0f;

    // -----------------------------------------------------------------------
    // IS THIS FLYER SLOT ACTUALLY BEING DRAWN?
    //
    // THE ONE QUESTION EVERY TEST IN THIS ENGINE HAD BEEN UNABLE TO ASK. A
    // population lives on the host -- it spawns, it steps, /locate finds it,
    // --clip-test counts it -- and the ONLY thing that puts it on the screen is
    // a publish into this band. setFlyerInstance drops a slot outside the band
    // without a word, so a run that is two short is a species that exists
    // everywhere except in front of you, and nothing anywhere would say so.
    //
    // That is what happened to the frog. See kCritterSlots.
    // -----------------------------------------------------------------------
    bool flyerShown(int slot) const {
        if (flyers_.empty() || flyerBase_ < 0 || slot < 0 || slot >= kFlyerInstances) return false;
        const size_t idx = size_t(flyerBase_ + slot);
        if (idx >= instanceDescs_.size()) return false;
        return instanceDescs_[idx].instanceMask != 0;
    }

    size_t chunkCount() const { return chunks_.size(); }

    // WHERE ONE LOOSE PIECE IS AND WHAT IT IS DOING. For the headless fell
    // test, which is the only way this engine can be watched without opening a
    // window over whatever the user is doing -- see App::runFellTest.
    // IS THIS PIECE STILL FALLING, OR IS IT ON ITS WAY TO THE PLAYER.
    //
    // For --shaft-test, and it exists because the first run of that test
    // reported a chip "thrown to the surface" that was doing nothing of the
    // kind: it was being COLLECTED. An absorb is a kinematic lerp to the eye
    // with no collision in it, so a chip four metres down rises through solid
    // ground at fifteen metres a second and looks exactly like an ejection.
    bool debrisAbsorbing(int slot) const {
        if (slot < 0 || slot >= kDebrisInstances) return false;
        return debris_[slot].live && debris_[slot].absorbing;
    }

    // The solver's handle for a loose piece, so a test can ask the physics
    // about it directly. See --kill-test's corpse trace.
    // The piece's own half extents, for the corpse trace: a thin slab and a
    // cube behave very differently against a height field.
    void debrisHalf(int slot, float *hx, float *hy, float *hz) const {
        if (slot < 0 || slot >= kDebrisInstances) return;
        *hx = debris_[slot].halfM[0];
        *hy = debris_[slot].halfM[1];
        *hz = debris_[slot].halfM[2];
    }

    int debrisBody(int slot) const {
        return (slot < 0 || slot >= kDebrisInstances) ? -1 : debris_[slot].phys;
    }

    bool debrisPose(int slot, Vec3 *p, float *quat) const {
        if (slot < 0 || slot >= kDebrisInstances || !debris_[slot].live) return false;
        if (p) *p = debris_[slot].pos;
        if (quat)
            for (int k = 0; k < 4; ++k) quat[k] = debris_[slot].quat[k];
        return true;
    }

    // -----------------------------------------------------------------------
    // A POINT ON A BODY THAT IS ACTUALLY MADE OF SOMETHING.
    //
    // The bounding box of a felled tree is mostly air -- it is a long thin
    // thing lying at an angle inside a big box -- so "the middle of its bounds"
    // is a point in space beside the log about as often as it is the log. Same
    // trap runFellTest hit aiming at a standing birch, and it is solved the
    // same way: find where the model is SOLID and aim at that.
    //
    // The solid voxel nearest the solid centroid, as a world point. For the
    // test harness, which has to stand somewhere and swing.
    // -----------------------------------------------------------------------
    // `butt` NARROWS IT TO THE THICK END -- the model's own lowest solid rows,
    // which for a felled tree is the cut face and the bare trunk under the
    // branches. That is where a person chops a log, and it is the only aim that
    // asks the question "can this be cut IN HALF" rather than "can a branch be
    // knocked off": the centroid of a pine's voxels is out in the needles.
    bool debrisAim(int slot, Vec3 *outW, bool butt = false) const {
        if (slot < 0 || slot >= kDebrisInstances) return false;
        const Debris &d = debris_[slot];
        if (!d.live || d.vox.empty()) return false;
        int yLo = 0, yHi = d.vsy - 1;
        if (butt) {
            int first = -1, last = -1;
            for (int y = 0; y < d.vsy; ++y) {
                bool any = false;
                for (int z = 0; z < d.vsz && !any; ++z)
                    for (int x = 0; x < d.vsx; ++x)
                        if (d.vox[size_t(x) + size_t(z) * size_t(d.vsx) +
                                  size_t(y) * size_t(d.vsx) * size_t(d.vsz)] != mat::AIR) {
                            any = true;
                            break;
                        }
                if (!any) continue;
                if (first < 0) first = y;
                last = y;
            }
            if (first < 0) return false;
            yLo = first;
            yHi = first + maxi(1, (last - first) / 8);
        }
        double cx = 0.0, cy = 0.0, cz = 0.0;
        long n = 0;
        for (int y = yLo; y <= yHi; ++y)
            for (int z = 0; z < d.vsz; ++z)
                for (int x = 0; x < d.vsx; ++x)
                    if (d.vox[size_t(x) + size_t(z) * size_t(d.vsx) +
                              size_t(y) * size_t(d.vsx) * size_t(d.vsz)] != mat::AIR) {
                        cx += x;
                        cy += y;
                        cz += z;
                        ++n;
                    }
        if (!n) return false;
        (void)0;
        cx /= double(n);
        cy /= double(n);
        cz /= double(n);
        int bx = -1, by = 0, bz = 0;
        double best = 1e30;
        for (int y = yLo; y <= yHi; ++y)
            for (int z = 0; z < d.vsz; ++z)
                for (int x = 0; x < d.vsx; ++x) {
                    if (d.vox[size_t(x) + size_t(z) * size_t(d.vsx) +
                              size_t(y) * size_t(d.vsx) * size_t(d.vsz)] == mat::AIR)
                        continue;
                    const double e = (x - cx) * (x - cx) + (y - cy) * (y - cy) +
                                     (z - cz) * (z - cz);
                    if (e < best) { best = e; bx = x; by = y; bz = z; }
                }
        if (bx < 0) return false;
        float m[9];
        quatMat3(d.quat, m);
        const float lx = d.originOff.x + (float(bx) + 0.5f) * VOXEL_M;
        const float ly = d.originOff.y + (float(by) + 0.5f) * VOXEL_M;
        const float lz = d.originOff.z + (float(bz) + 0.5f) * VOXEL_M;
        if (outW)
            *outW = Vec3{d.pos.x + m[0] * lx + m[1] * ly + m[2] * lz,
                         d.pos.y + m[3] * lx + m[4] * ly + m[5] * lz,
                         d.pos.z + m[6] * lx + m[7] * ly + m[8] * lz};
        return true;
    }

    // HOW MUCH OF A BODY IS LEFT -- for --fell-test's chop phase, which is the
    // only way to watch a log being broken up without opening a window over
    // whatever the user is doing. -1 if that slot holds nothing.
    int debrisVoxels(int slot) const {
        if (slot < 0 || slot >= kDebrisInstances || !debris_[slot].live) return -1;
        int n = 0;
        for (uint8_t v : debris_[slot].vox)
            if (v != mat::AIR) ++n;
        return n;
    }

    void debrisVel(Physics &ph, int slot, Vec3 *lin, Vec3 *ang) const {
        if (slot < 0 || slot >= kDebrisInstances || !debris_[slot].live) return;
        ph.velocityOf(debris_[slot].phys, lin, ang);
    }

    // -- WHAT THE LOOSE POOL IS HOLDING, BY KIND -------------------------
    //
    // For --kill-test. "There should only be the broken up red peices" is a
    // claim about two populations in one pool, and counting them apart is the
    // only way to tell a corpse from a chip: a corpse piece is scenery (so the
    // player cannot absorb it) and carries hurtT0 (so it draws red), and an
    // ordinary chip is neither.
    void debrisKinds(int *red, int *plain) const {
        *red = 0;
        *plain = 0;
        for (const Debris &d : debris_) {
            if (!d.live) continue;
            if (d.hurtT0 > -1e8 && d.scenery) ++(*red);
            else ++(*plain);
        }
    }

    int looseCount() const {
        int k = 0;
        for (const Debris &d : debris_)
            if (d.live) ++k;
        return k;
    }

    // -----------------------------------------------------------------------
    // THE FIRST LOOSE BODY UNDER A RAY -- THE OTHER HALF OF THE SWING.
    //
    // "when a tree falls, or something that was static and is now a rigid body,
    // the player is unable to interact with that felled object."
    //
    // They could not, and no amount of aiming would have helped. swingRayModels
    // walks `w.solids`, which is the list of things PLACED in the world, and the
    // instant a tree is felled it stops being one of those: it is a rigid body
    // in the debris band, drawn from a different instance band, and it was never
    // under the crosshair as far as the tool was concerned. The tree you can see
    // lying in front of you was, to every question the swing asked, not there.
    //
    // So the bodies are asked directly, and asked THE SAME WAY the standing ones
    // are -- the ray is turned into each body's frame and marched through its
    // own voxels. That is what makes an axe land on the trunk and pass between
    // two branches, and it is the same function the standing tree gets: see
    // rayVoxelGrid, which was lifted out of rayModelVoxels for this.
    //
    // A BODY WEARS A FULL ROTATION and a placement wears a quarter turn, which
    // is the only real difference: a felled tree comes to rest at whatever angle
    // the ground gave it. The inverse of a rotation is its transpose, so the
    // change of frame is nine multiplies and no trigonometry.
    //
    // THE VOXELS AND NOT THE COLLIDER. The boxes the solver has are the trunk at
    // forty centimetre cells with the canopy left out -- swing at those and you
    // strike air a third of a metre off the bark, and cannot touch a branch at
    // all. See Debris::vox.
    // -----------------------------------------------------------------------
    bool debrisRay(const Vec3 &eye, const Vec3 &dir, float maxT, DebrisHit *out) const {
        if (!out || maxT <= 0.0f) return false;
        bool any = false;
        float best = maxT;
        for (int i = 0; i < kDebrisInstances; ++i) {
            const Debris &d = debris_[i];
            // A PIECE ON ITS WAY TO THE PLAYER IS NOT A TARGET. It is being
            // collected -- it has left the solver and is flying on a curve --
            // and hitting it would be hitting an animation.
            if (!d.live || d.absorbing || d.vox.empty()) continue;

            float m[9];
            quatMat3(d.quat, m);
            // World into body: the transpose, applied to the ray's origin
            // relative to the body and to its direction.
            const float rx = eye.x - d.pos.x, ry = eye.y - d.pos.y, rz = eye.z - d.pos.z;
            const float ex = m[0] * rx + m[3] * ry + m[6] * rz;
            const float ey = m[1] * rx + m[4] * ry + m[7] * rz;
            const float ez = m[2] * rx + m[5] * ry + m[8] * rz;
            const float dx = m[0] * dir.x + m[3] * dir.y + m[6] * dir.z;
            const float dy = m[1] * dir.x + m[4] * dir.y + m[7] * dir.z;
            const float dz = m[2] * dir.x + m[5] * dir.y + m[8] * dir.z;
            // ...and body into the grid, whose corner is originOff.
            const float o[3] = {(ex - d.originOff.x) / VOXEL_M, (ey - d.originOff.y) / VOXEL_M,
                                (ez - d.originOff.z) / VOXEL_M};
            const float g[3] = {dx / VOXEL_M, dy / VOXEL_M, dz / VOXEL_M};
            const int n[3] = {d.vsx, d.vsy, d.vsz};

            float th = 0.0f;
            int hv[3] = {0, 0, 0};
            // Bounded by what is already the nearest hit, so a body behind one
            // that has already answered costs six divisions and stops.
            if (!rayVoxelGrid(d.vox.data(), n, o, g, best, &th, hv)) continue;
            if (th >= best) continue;
            best = th;
            any = true;
            out->slot = i;
            out->t = th;
            out->point = Vec3{eye.x + dir.x * th, eye.y + dir.y * th, eye.z + dir.z * th};
            out->vox[0] = hv[0];
            out->vox[1] = hv[1];
            out->vox[2] = hv[2];
            out->mat = d.vox[size_t(hv[0]) + size_t(hv[2]) * size_t(d.vsx) +
                             size_t(hv[1]) * size_t(d.vsx) * size_t(d.vsz)];
            out->takes = d.takesAs;
        }
        return any;
    }

    // -----------------------------------------------------------------------
    // A BITE OUT OF A LOOSE BODY.
    //
    // carveModel, asked of a body instead of an instance, and deliberately the
    // same shape of function: a sphere of radiusVox around the struck voxel,
    // the material of everything removed kept as spoil, and the spoil handed
    // back so the caller can throw it as a chunk the player can collect. What
    // an axe does to a standing trunk it now does to the one on the ground, and
    // it does it with the same numbers.
    // -----------------------------------------------------------------------
    bool carveDebris(Physics &ph, const DebrisHit &h, int radiusVox, double nowMs,
                     std::vector<uint8_t> *spoil = nullptr, int *spoilN = nullptr,
                     Vec3 *spoilAt = nullptr, float *spoilYaw = nullptr) {
        if (h.slot < 0 || h.slot >= kDebrisInstances) return false;
        Debris &d = debris_[h.slot];
        if (!d.live || d.absorbing || d.vox.empty()) return false;

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
                    const int x = h.vox[0] + dx, y = h.vox[1] + dy, z = h.vox[2] + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= d.vsx || y >= d.vsy || z >= d.vsz)
                        continue;
                    uint8_t &v = d.vox[size_t(x) + size_t(z) * size_t(d.vsx) +
                                       size_t(y) * size_t(d.vsx) * size_t(d.vsz)];
                    if (v == mat::AIR) continue;
                    if (spoil && spoilN)
                        (*spoil)[size_t(dx + radiusVox) + size_t(dz + radiusVox) * size_t(sn) +
                                 size_t(dy + radiusVox) * size_t(sn) * size_t(sn)] = v;
                    v = mat::AIR;
                    ++removed;
                }
        if (removed == 0) return false;   // the swing missed the body's own voxels

        // WHERE THE BITE LANDED, in world metres: the struck voxel's centre
        // carried out through the body's own rotation. Not the point the swing
        // reported -- carveModel's note applies here word for word.
        float m[9];
        quatMat3(d.quat, m);
        const float lx = d.originOff.x + (float(h.vox[0]) + 0.5f) * VOXEL_M;
        const float ly = d.originOff.y + (float(h.vox[1]) + 0.5f) * VOXEL_M;
        const float lz = d.originOff.z + (float(h.vox[2]) + 0.5f) * VOXEL_M;
        if (spoilAt)
            *spoilAt = Vec3{d.pos.x + m[0] * lx + m[1] * ly + m[2] * lz,
                            d.pos.y + m[3] * lx + m[4] * ly + m[5] * lz,
                            d.pos.z + m[6] * lx + m[7] * ly + m[8] * lz};
        // THE CHIP KEEPS THE BODY'S YAW AND NOT ITS TILT, which is the one
        // place this is less exact than a carve on a standing model. A spoil
        // cube is spawned as a yaw-only placement (spawnPiece), a log lying on
        // the ground is a quarter turn out of that, and the difference is the
        // orientation of a three-voxel blob that is tumbling before the frame
        // is over. Carrying the tilt would mean a quaternion through
        // spawnDebris and addChunkBody to describe a pebble.
        if (spoilYaw) *spoilYaw = std::atan2(-m[6], m[0]);

        return breakDebris(ph, h.slot, nowMs, h.vox, radiusVox);
    }

    // -----------------------------------------------------------------------
    // WHAT IS LEFT OF A BODY AFTER A BITE -- AND HOW MANY THINGS IT IS.
    //
    // "...and it break even when felled on the ground." This is the break.
    //
    // A bite is only a hole until it reaches the far side of the trunk. On the
    // blow that does, the wood is two six-connected pieces, and they are made
    // into two bodies wearing the pose and the motion of the one they came out
    // of -- so the log is in half, both halves keep rolling if it was rolling,
    // and neither of them snaps upright.
    //
    // THE SHAPE DECIDES, NOT A COUNT OF BLOWS. Exactly as fellTree decides what
    // comes down by what is no longer standing on the ground rather than by how
    // many times it has been hit. A log chopped at one end gives a short piece
    // and a long one; chopped in the middle it gives two of a length; chopped
    // where a branch forks it gives you the branch. Nothing anywhere holds a
    // hit point.
    //
    // A LONE VOXEL IS STILL NOT A CHUNK -- v1's rule, and the reason the floor
    // below exists: the rim of a sphere clips a stray voxel across a gap, and
    // without a floor every third blow would spend a debris slot on a speck.
    //
    // -----------------------------------------------------------------------
    // AND A PIECE ONLY COMES OFF WHERE THE AXE CUT IT.
    //
    // This is the whole of `cutVox`, and it is not a refinement -- without it
    // the feature is broken. MEASURED: one blow at the base of a felled pine
    // turned it into EIGHTEEN BODIES. The model is not one six-connected solid
    // as authored; its canopy is a scatter of leaf clumps that touch the
    // branches diagonally or not at all, and they had been travelling with the
    // tree only because nothing had ever asked. The first bite asked, and
    // seventeen clumps of forty voxels each took seventeen of the sixty-four
    // debris slots and rained out of the sky.
    //
    // fellTree meets the same fact and answers it the same way -- see
    // ModelTemplate::bornLoose, which is what three pines falling over on their
    // first blow cost to find: what a BLOW did is the difference between now
    // and how the model was drawn, never the state of the model on its own.
    //
    // So a piece separates only if the bite is touching it. Everything else
    // stays in the body it was in, still disconnected, exactly as it has been
    // since it was authored. A log cut through gives two logs because both ends
    // are at the cut; a pine chopped at the butt keeps its canopy, because a
    // clump twelve metres up is not something an axe at the butt has touched.
    // -----------------------------------------------------------------------
    bool breakDebris(Physics &ph, int slot, double nowMs, const int cutVox[3], int cutR) {
        if (slot < 0 || slot >= kDebrisInstances) return false;
        Debris &d = debris_[slot];
        if (!d.live || d.vox.empty()) return false;
        const int nx = d.vsx, ny = d.vsy, nz = d.vsz;
        auto ix = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(nx) + size_t(y) * size_t(nx) * size_t(nz);
        };

        // ---- the six-connected pieces --------------------------------------
        struct Piece {
            int count = 0;
            int lo[3] = {0, 0, 0};
            int hi[3] = {0, 0, 0};
            bool cut = false;   // does the bite touch this piece
        };
        // A voxel the bite could have been in contact with. The bite is a
        // sphere of cutR around cutVox, so anything it separated has a voxel
        // within one of that rim; two is the same answer with a margin.
        const int reach2 = (cutR + 2) * (cutR + 2);
        auto atCut = [&](int x, int y, int z) {
            const int dx = x - cutVox[0], dy = y - cutVox[1], dz = z - cutVox[2];
            return dx * dx + dy * dy + dz * dz <= reach2;
        };
        std::vector<Piece> pieces;
        // ONE LABEL PER VOXEL, and it is worth the megabyte: two pieces can
        // share a bounding box -- a branch lying across a log does -- so
        // copying a piece out by its box would put each of them inside both.
        std::vector<int32_t> lab(d.vox.size(), -1);
        std::vector<int> st;
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (int y = 0; y < ny; ++y)
            for (int z = 0; z < nz; ++z)
                for (int x = 0; x < nx; ++x) {
                    const size_t i0 = ix(x, y, z);
                    if (d.vox[i0] == mat::AIR || lab[i0] >= 0) continue;
                    const int32_t id = int32_t(pieces.size());
                    Piece p;
                    p.lo[0] = p.hi[0] = x;
                    p.lo[1] = p.hi[1] = y;
                    p.lo[2] = p.hi[2] = z;
                    lab[i0] = id;
                    st.clear();
                    st.push_back(int(i0));
                    while (!st.empty()) {
                        const int q = st.back();
                        st.pop_back();
                        const int qx = q % nx, qz = (q / nx) % nz, qy = q / (nx * nz);
                        ++p.count;
                        if (!p.cut && atCut(qx, qy, qz)) p.cut = true;
                        p.lo[0] = mini(p.lo[0], qx);
                        p.hi[0] = maxi(p.hi[0], qx);
                        p.lo[1] = mini(p.lo[1], qy);
                        p.hi[1] = maxi(p.hi[1], qy);
                        p.lo[2] = mini(p.lo[2], qz);
                        p.hi[2] = maxi(p.hi[2], qz);
                        for (const int *o : off) {
                            const int ax = qx + o[0], ay = qy + o[1], az = qz + o[2];
                            if (ax < 0 || ay < 0 || az < 0 || ax >= nx || ay >= ny || az >= nz)
                                continue;
                            const size_t a = ix(ax, ay, az);
                            if (d.vox[a] == mat::AIR || lab[a] >= 0) continue;
                            lab[a] = id;
                            st.push_back(int(a));
                        }
                    }
                    pieces.push_back(p);
                }

        // ---- CHOPPED TO NOTHING --------------------------------------------
        if (pieces.empty()) {
            retireDebris(ph, slot);
            rebuildTlas();
            return true;
        }

        // ---- STILL ONE THING: THE HOLE IS JUST A HOLE ----------------------
        //
        // Re-meshed, and the collider left alone. The solver's shape is this
        // body's voxels at forty centimetre cells and a bite is three voxels
        // across, so rebuilding it would hand PhysX the same shape again having
        // thrown away every contact the log was resting on -- which is a log
        // that twitches every time you hit it.
        {
            // WHAT THE BLOW SEPARATED: the pieces the bite is touching, big
            // enough to be pieces. One of those is a hole; two or more is a
            // cut. Everything else stays where it was -- see the note above.
            std::vector<size_t> at;
            for (size_t k = 0; k < pieces.size(); ++k)
                if (pieces[k].cut && pieces[k].count >= kMinPieceVox) at.push_back(k);
            if (at.size() < 2) {
                int live = 0;
                for (const Piece &p : pieces) live += p.count;
                d.voxels = live;
                return remeshDebris(slot);
            }
            // The biggest of them keeps the slot AND everything the blow did
            // not touch; the rest leave. Sorted so that if the debris slots run
            // out it is the smallest offcut that is lost.
            std::sort(at.begin(), at.end(),
                      [&](size_t a, size_t b) { return pieces[a].count > pieces[b].count; });

            // ---- IT CAME APART -------------------------------------------
            //
            // Everything the new bodies inherit is read BEFORE the old one is
            // retired, because retiring it clears all of it.
            const bool felled = d.felled;
            const bool scenery = d.scenery;
            const uint8_t takesAs = d.takesAs;
            const float3 tint = d.tint;
            const double born = d.bornMs;
            const Vec3 pos = d.pos, oOff = d.originOff;
            float quat[4];
            for (int k = 0; k < 4; ++k) quat[k] = d.quat[k];
            Vec3 lin{0, 0, 0}, ang{0, 0, 0};
            if (d.phys >= 0) ph.velocityOf(d.phys, &lin, &ang);

            // WHICH LABEL GOES WHERE. Group 0 is the body that stays -- the
            // biggest piece at the cut, plus every piece the blow never
            // touched, which is the canopy of a pine and the far half of
            // nothing else. Group n is the nth offcut.
            std::vector<int> group(pieces.size(), 0);
            for (size_t k = 1; k < at.size(); ++k) group[at[k]] = int(k);

            // The volumes, cut out and trimmed to their own extents while the
            // parent's voxels are still there to read.
            struct Cut {
                std::vector<uint8_t> vox;
                int sx = 0, sy = 0, sz = 0;
                Vec3 off{0, 0, 0};
            };
            std::vector<Cut> cuts;
            for (size_t g = 0; g < at.size(); ++g) {
                int glo[3] = {nx, ny, nz}, ghi[3] = {-1, -1, -1};
                for (size_t k = 0; k < pieces.size(); ++k) {
                    if (group[k] != int(g)) continue;
                    for (int a = 0; a < 3; ++a) {
                        glo[a] = mini(glo[a], pieces[k].lo[a]);
                        ghi[a] = maxi(ghi[a], pieces[k].hi[a]);
                    }
                }
                if (ghi[0] < 0) continue;
                Cut c;
                c.sx = ghi[0] - glo[0] + 1;
                c.sy = ghi[1] - glo[1] + 1;
                c.sz = ghi[2] - glo[2] + 1;
                c.off = Vec3{oOff.x + float(glo[0]) * VOXEL_M, oOff.y + float(glo[1]) * VOXEL_M,
                             oOff.z + float(glo[2]) * VOXEL_M};
                c.vox.assign(size_t(c.sx) * size_t(c.sy) * size_t(c.sz), mat::AIR);
                for (int y = glo[1]; y <= ghi[1]; ++y)
                    for (int z = glo[2]; z <= ghi[2]; ++z)
                        for (int x = glo[0]; x <= ghi[0]; ++x) {
                            const size_t a = ix(x, y, z);
                            const int32_t id = lab[a];
                            if (id < 0 || group[size_t(id)] != int(g)) continue;
                            c.vox[size_t(x - glo[0]) + size_t(z - glo[2]) * size_t(c.sx) +
                                  size_t(y - glo[1]) * size_t(c.sx) * size_t(c.sz)] = d.vox[a];
                        }
                cuts.push_back(std::move(c));
            }
            if (cuts.empty()) {
                retireDebris(ph, slot);
                rebuildTlas();
                return true;
            }

            retireDebris(ph, slot);
            int made = 0;
            for (size_t k = 0; k < cuts.size(); ++k) {
                int use = slot;
                if (k > 0) {
                    use = -1;
                    for (int i = 0; i < kDebrisInstances; ++i)
                        if (!debris_[i].live) { use = i; break; }
                    if (use < 0) break;   // the world is as busy as it is allowed to be
                }
                if (makeLooseBody(ph, use, std::move(cuts[k].vox), cuts[k].sx, cuts[k].sy,
                                  cuts[k].sz, cuts[k].off, pos, quat, lin, ang, felled, scenery,
                                  takesAs, tint, born, nowMs))
                    ++made;
            }
            rebuildTlas();
            return made > 0;
        }
    }

    // -----------------------------------------------------------------------
    // THE MESH FOLLOWS THE VOXELS, and nothing else changes.
    //
    // The bite has to be VISIBLE on the frame it lands -- that is the whole of
    // a tool feeling like it works -- and it is the only thing this does.
    //
    // IT COSTS WHAT FELLING COSTS, and it is worth writing the number down:
    // --fell-test measures 22.7 ms for a blow on a felled pine, bite, re-mesh
    // and break together. That is one frame and a half, once per swing, and it
    // is the same work fellTree does on the blow that brings the tree down --
    // the whole volume meshed and a structure built over it.
    //
    // A SMALL BODY IS FREE. The cost is the model BOX, and a pine's is 99 x 260
    // x 95 whatever is left in it; a piece chopped off one is trimmed to its own
    // extents when it is born (see breakDebris) and re-meshes in microseconds.
    // So the expensive case is the first few blows on a whole tree, and it gets
    // cheaper as the tree comes apart.
    //
    // carveModel's answer to the same problem is Remesher -- queue it, keep
    // drawing the previous mesh for a frame. That is not available here: a job
    // is keyed by chunk and decor slot and adopted into a placed INSTANCE, and
    // a body is neither. Worth doing if a hitch per swing is ever felt.
    // -----------------------------------------------------------------------
    bool remeshDebris(int slot) {
        Debris &d = debris_[slot];
        // resolveShades follows the KIND the instance is drawn with -- see
        // setDebrisInstance. A felled tree is KIND_TREE and re-rolls its shades
        // per world cell the way the standing one did; a chip is KIND_LOOSE and
        // has them baked. Meshing one the other way round flattens its bark.
        const VoxMesh mesh = meshVolume(d.vox, d.vsx, d.vsy, d.vsz, VOXEL_M, !d.felled);
        if (mesh.triCount() == 0) return false;
        Blas b = recordLooseBuild(mesh);
        if (!b.valid()) return false;
        retireLoose(std::move(d.blas), d.triOffset, d.tris);
        d.blas = std::move(b);
        d.triOffset = pool_.upload(ctx_, mesh.tri);
        d.tris = mesh.tri.size();
        // The instance is pointed at the new structure NOW rather than on the
        // next updateDebris, so the TLAS built below is not built over an
        // address that has just been handed back.
        setDebrisInstance(slot, d.pos, d.quat);
        rebuildTlas();
        return true;
    }

    // -----------------------------------------------------------------------
    // A BODY BORN OUT OF ANOTHER ONE.
    //
    // fellTree makes the first body of a tree's life out of an INSTANCE; this
    // makes the ones after that out of a body, which is a different set of
    // knowns: there is no placement to read a quarter turn off, no chunk to
    // leave a stump in, and a rotation that is whatever the ground gave it.
    // What it shares with fellTree is everything that matters -- greedy boxes
    // off the piece's own voxels rather than a convex hull, timber's density
    // and a mass taken from the boxes, and the surrounding world as a static to
    // rest on.
    // -----------------------------------------------------------------------
    bool makeLooseBody(Physics &ph, int slot, std::vector<uint8_t> &&vox, int sx, int sy, int sz,
                       const Vec3 &originOff, const Vec3 &pos, const float *quat, const Vec3 &lin,
                       const Vec3 &ang, bool felled, bool scenery, uint8_t takesAs,
                       const float3 &tint, double bornMs, double nowMs) {
        if (slot < 0 || slot >= kDebrisInstances || sx <= 0 || sy <= 0 || sz <= 0) return false;
        int count = 0;
        for (uint8_t v : vox)
            if (v != mat::AIR) ++count;
        if (count <= 0) return false;

        const VoxMesh mesh = meshVolume(vox, sx, sy, sz, VOXEL_M, !felled);
        if (mesh.triCount() == 0) return false;
        Blas b = recordLooseBuild(mesh);
        if (!b.valid()) return false;

        // ---- ...and as a collider ------------------------------------------
        //
        // THE CELL IS HALVED WHEN IT IS TOO COARSE TO HOLD ANYTHING, which is
        // the case felling never had: a tree is always bigger than kFellCellM
        // and a two metre offcut can be thinner than one of its cells. Without
        // it a chopped-off branch comes back with no boxes and therefore no
        // body, and simply disappears.
        const bool wood = takesAs == kDebrisWood;
        float cell = kFellCellM;
        for (int tries = 0; tries < 8; ++tries) {
            const int q = maxi(1, int(cell / VOXEL_M + 0.5f));
            const int cx = (sx + q - 1) / q, cy = (sy + q - 1) / q, cz = (sz + q - 1) / q;
            const int need = maxi(1, int(float(q * q * q) * kFellFillFrac));
            greedyBoxes(
                cx, cy, cz, originOff, cell,
                [&](int i, int j, int k) {
                    // A TREE'S COLLIDER IS ITS WOOD -- fellTree's rule, and the
                    // same reason: a crown collider is a fat cone that cannot
                    // lie down. The palette classified every colour as foliage
                    // or bark when the model was loaded.
                    int n = 0;
                    for (int b2 = 0; b2 < q; ++b2)
                        for (int a2 = 0; a2 < q; ++a2)
                            for (int e2 = 0; e2 < q; ++e2) {
                                const int x = i * q + e2, y = j * q + b2, z = k * q + a2;
                                if (x >= sx || y >= sy || z >= sz) continue;
                                const uint8_t mv = vox[size_t(x) + size_t(z) * size_t(sx) +
                                                       size_t(y) * size_t(sx) * size_t(sz)];
                                if (mv == mat::AIR) continue;
                                if (wood && palette.isFoliage(mv)) continue;
                                if (++n >= need) return true;
                            }
                    return false;
                },
                &winBoxes_, kFellMaxBoxes * 4);
            if (winBoxes_.empty() && cell > VOXEL_M * 1.01f) {
                cell *= 0.5f;
                continue;
            }
            if (winBoxes_.size() <= kFellMaxBoxes) break;
            cell *= 1.5f;
        }
        if (winBoxes_.empty()) {
            retireLoose(std::move(b), TriPool::kInvalid, 0);
            return false;
        }

        // IT IS BORN WEARING THE ROTATION ITS PARENT HAD. A half of a log that
        // was lying down has to be lying down -- see addCompoundBody's quat.
        const int phys =
            ph.addCompoundBody(winBoxes_.data(), int(winBoxes_.size()), pos, 0.0f,
                               wood ? kTimberDensity : kStoneDensity, quat);
        if (phys < 0) {
            retireLoose(std::move(b), TriPool::kInvalid, 0);
            return false;
        }
        if (wood) {
            // FROM THE BOXES THE SOLVER ACTUALLY HAS -- fellTree's note: left
            // to the shape volume at timber density a pine weighed 128 tonnes.
            double logVol = 0.0;
            for (const VoxBox &bx : winBoxes_) logVol += 8.0 * double(bx.hx) * bx.hy * bx.hz;
            ph.setBodyMass(phys, maxf(10.0f, float(logVol * kTimberDensity)));
        }
        // ...AND THE MOTION IT HAD. A log cut while it is still rolling keeps
        // rolling; both halves do.
        ph.setVelocity(phys, lin, ang);

        Debris &d = debris_[slot];
        d = Debris{};
        d.live = true;
        d.felled = felled;
        // HALF A CAP IS STILL A CAP. Chopping scenery into two makes two pieces
        // of scenery, not two collectables -- the way to collect a mushroom is
        // the CHUNKS, which come off the spoil path and never through here.
        d.scenery = scenery;
        d.takesAs = takesAs;
        d.tint = tint;
        // ITS PARENT'S BIRTHDAY, NOT ITS OWN. Otherwise a log could be kept in
        // the world for ever by chopping a piece off it every half hour.
        d.bornMs = bornMs;
        d.lastMs = nowMs;
        d.boxes = winBoxes_;   // before buildSolidWindow reuses the scratch
        d.phys = phys;
        d.blas = std::move(b);
        d.triOffset = pool_.upload(ctx_, mesh.tri);
        d.tris = mesh.tri.size();
        d.voxels = count;
        d.vox = std::move(vox);
        d.vsx = sx;
        d.vsy = sy;
        d.vsz = sz;
        d.originOff = originOff;
        d.halfM[0] = 0.5f * float(sx) * VOXEL_M;
        d.halfM[1] = 0.5f * float(sy) * VOXEL_M;
        d.halfM[2] = 0.5f * float(sz) * VOXEL_M;
        d.pos = pos;
        for (int k = 0; k < 4; ++k) d.quat[k] = quat[k];
        d.winCentre = pos;
        {
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            if (ph.boundsOf(phys, &lo, &hi)) {
                d.winLo = Vec3{lo.x - kStaticPadM, lo.y - kStaticPadM, lo.z - kStaticPadM};
                d.winHi = Vec3{hi.x + kStaticPadM, hi.y + kStaticPadM, hi.z + kStaticPadM};
                d.window = buildSolidWindow(ph, d.winLo, d.winHi);
            }
        }
        setDebrisInstance(slot, d.pos, d.quat);
        debrisDirty_ = true;
        return true;
    }

    // -----------------------------------------------------------------------
    // SET A PIECE OF THE WORLD LOOSE.
    //
    // A BALL OF ONE MATERIAL, not a copy of the voxels that were removed. What
    // a swing takes out is a sphere three voxels across of whatever it hit, and
    // rebuilding the exact removed set would mean carrying a second volume
    // through the carve for a difference nobody can see on something that is
    // tumbling and gone in a second.
    //
    // -- AND THE SHADE HAS TO BE RESOLVED HERE, WHICH THIS NOTE USED TO DENY --
    //
    // It said "the shade still varies per voxel -- that happens on the device,
    // in groundShade, from the voxel coordinate". It does not, and the line
    // that stops it is three words long: Trace.cs.slang picks
    //
    //     (inst.kind == KIND_LOOSE) ? triMaterial(packed) : groundShade(...)
    //
    // and it is RIGHT to -- groundShade hashes the WORLD voxel a face is on, so
    // a tumbling body would re-roll its own colours every frame it moved.
    //
    // So a chip of terrain stone was drawn at mat::ROCK's own entry, which is
    // one flat mid grey, while the hole it came out of wears the six-shade
    // ramp taken from the boulders' own palettes (309 usable colours at last
    // count). Against that, flat grey reads as WHITE -- reported as "when
    // hitting stone in the ground with a pick, it turns white, instead of the
    // rock thats in the terrain".
    //
    // ONE ROLL, AT THE SPAWN, from the bite's own cell -- so the piece is a
    // fixed colour off the same ramp for its whole life, which is what a chunk
    // of stone is.
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
    // `takesAs` IS WHAT THE PIECE IS MADE OF, and it is on the signature
    // because only the caller knows. The materials in the spoil are the
    // MODEL's palette ids -- a boulder's grey is not mat::ROCK -- so nothing
    // here could work it out, and a body has no `standable` to fall back on
    // the way a placement does. It decides one thing: which tool can break the
    // piece once it is lying there. See DebrisTakes.
    int spawnDebris(Physics &ph, const std::vector<uint8_t> &vol, int n, const Vec3 &centre,
                    const Vec3 &vel, const Vec3 &spin, double nowMs, float yawRad = 0.0f,
                    const Solid *src = nullptr, uint8_t takesAs = kDebrisStone,
                    float absorbR = kAbsorbAnywhere) {
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
        // -- THE RULE, AT THE ONE DOOR IT CAN BE ENFORCED AT ---------------
        //
        // (user 2026-09-17: "whenever something breaks apart from the static
        //  terrain, it becomes a rigid body. This needs to become a RULE in
        //  the codebase. no matter what.")
        //
        // THIS LINE USED TO READ `count < 2` and it was the last place in the
        // engine where severed voxels could still simply cease to exist. The
        // comment defending it was v1's "a lone voxel is still not a chunk",
        // and as an argument about LITTER that is sound -- but it is an
        // argument about tidiness, and the rule above is not. A voxel that
        // left the static world and did not become a body is a voxel that
        // vanished, which is the exact complaint this rule exists to answer.
        //
        // kMinBodyVoxels is 1: whatever comes out, comes out as something.
        // See the RULE block over it for what is and is not covered.
        if (count < kMinBodyVoxels) return -1;
        // -- THE TWO FAMILY IDS, RESOLVED -- see the note above.
        //
        // ROCK AND SAND ARE THE ONLY TWO. Every other ground family is stored
        // as a concrete shade already (the grass, soil, litter and wheat ramps
        // are ids the mesher writes one of); these two are stored as ONE id and
        // spread on the device, which is exactly the spreading a loose body
        // does not get.
        std::vector<uint8_t> shaded = vol;
        const uint32_t cell = hashU32(uint32_t(int(std::floor(centre.x / VOXEL_M))),
                                      uint32_t(int(std::floor(centre.z / VOXEL_M))));
        const uint8_t stone =
            uint8_t(mat::STONE_0 + int(cell % uint32_t(mat::STONE_COUNT)));
        const uint8_t grain =
            uint8_t(mat::SAND_0 + int((cell >> 8) % uint32_t(mat::SAND_COUNT)));
        for (uint8_t &v : shaded) {
            if (v == mat::ROCK) v = stone;
            else if (v == mat::SAND) v = grain;
        }
        return spawnPiece(ph, shaded, n, count, centre, vel, spin, nowMs, yawRad, takesAs,
                          absorbR);
    }

    // -----------------------------------------------------------------------
    // THAT BODY IS SCENERY -- said AFTER it is spawned, and on purpose.
    //
    // spawnDebris already carries nine arguments and the last three are
    // positional bools and enums that the compiler cannot tell apart; the note
    // over loadModelSet's `solidify` records what inserting one more into that
    // kind of list costs. This takes the slot spawnDebris just returned, so it
    // cannot be passed in the wrong position and cannot be passed silently.
    //
    // See Debris::scenery for what it means and why it is not `felled`.
    // -----------------------------------------------------------------------
    // HOW MANY BODIES THE WORLD CAN STILL TAKE. The float watch asks before it
    // commits: see kFloatDicePerPiece, and the 12,625 voxels that were deleted
    // when it did not.
    int debrisFree() const {
        int n = 0;
        for (int i = 0; i < kDebrisInstances; ++i)
            if (!debris_[i].live) ++n;
        return n;
    }

    // -----------------------------------------------------------------------
    // LITTER MAKES WAY FOR A BIGGER THING. Frees slots until `want` of them
    // are, or until there is nothing small enough left to retire. Returns how
    // many are free.
    //
    // (user 2026-09-18: "on nuketown, the pole just deleted itself, instead of
    // being subject to physics.")
    //
    // A FULL POOL USED TO BE A DELETION. The level's debris is the whole of the
    // problem: nobody collects it (markLeftLying) and it lasts
    // kLevelChipLifeMs, a hundred seconds, so a couple of magazines of rifle
    // fire owns all sixty-four slots for the next minute and a half. The next
    // thing severed -- a light pole, a sign, a railing -- was carved out of the
    // grid and then refused a body. See the reservation in dropLevelHangers.
    //
    // `maxVox` IS THE SIZE OF THE THING ASKING, AND THAT IS THE WHOLE RULE: a
    // piece may take the slot of anything no bigger than itself. It scales by
    // construction and needs no threshold --
    //
    //   * a 248-voxel post takes the slot of a bullet chip or a wall fragment;
    //   * a 7-voxel chip takes the slot of another chip, oldest first, so the
    //     rubble of a firefight is a rolling window rather than the first
    //     sixty-four holes for ever;
    //   * nothing ever takes the slot of a piece of the map BIGGER than it, so
    //     a post that has already come down cannot be deleted out from under
    //     the player to land the next one.
    //
    // A FIXED THRESHOLD WAS TRIED FIRST AND MEASURED WRONG. `kLitterVox = 32`
    // looked ample against a radius-1 chip's seven voxels -- and the pool does
    // not fill with chips. It fills with what the rounds shake out of the
    // walls: measured over 256 rounds into the map, every one of the 64 live
    // bodies was a 40-to-74-voxel fragment, so nothing was evictable and the
    // post was refused a slot exactly as before.
    //
    // OLDEST FIRST, AND WHAT IS ALREADY STILL BEFORE WHAT IS IN THE AIR. These
    // pieces are on a clock already, so the oldest is the one nearest to going
    // of its own accord -- and a piece that has come to rest disappearing is
    // the thing the player is least likely to be looking at, whereas one still
    // bouncing is exactly the thing they are.
    //
    // NOT THE WOOD'S DEBRIS: absorbR of kAbsorbNever is what says nobody is
    // coming to collect this, and a chip the player is walking over to pick up
    // has a radius and is left alone. `absorbing` is the same fact once it is
    // already on its way; `felled` and `longBody` are trees and posts, which
    // are not litter at any size.
    // -----------------------------------------------------------------------
    int makeRoomForBodies(Physics &ph, int want, int maxVox) {
        int free = debrisFree();
        // Two passes: settled litter first, then any litter at all.
        for (int pass = 0; pass < 2 && free < want; ++pass) {
            for (;;) {
                if (free >= want) break;
                int victim = -1;
                double oldest = 0.0;
                for (int i = 0; i < kDebrisInstances; ++i) {
                    const Debris &d = debris_[i];
                    if (!d.live || d.felled || d.longBody || d.absorbing) continue;
                    if (d.absorbR != kAbsorbNever || d.lifeMs <= 0.0) continue;
                    if (d.voxels > maxVox) continue;
                    // Pass 0 takes only what has stopped moving -- restT0 is
                    // the sentinel until updateDebris has seen it still.
                    if (pass == 0 && d.restT0 < -1e8) continue;
                    if (victim < 0 || d.bornMs < oldest) {
                        victim = i;
                        oldest = d.bornMs;
                    }
                }
                if (victim < 0) break;
                retireDebris(ph, victim);
                ++free;
            }
        }
        return free;
    }

    void markScenery(int slot) {
        if (slot < 0 || slot >= kDebrisInstances) return;
        debris_[slot].scenery = true;
    }

    // -----------------------------------------------------------------------
    // THIS ONE IS NOT PICKED UP, AND IT GOES ON ITS OWN CLOCK.
    //
    // (user 2026-09-17: "in the fps map, dont have the player absorb the chunks
    //  caused by the bullet. just leave them there. have them be removed after
    //  100 seconds.")
    //
    // A SETTER RATHER THAN TWO MORE ARGUMENTS, for the reason markScenery gives
    // above it: spawnDebris already carries nine, the last three of which are
    // positional bools and enums the compiler cannot tell apart. This takes the
    // slot spawnDebris just returned, so it cannot be passed in the wrong
    // position and cannot be passed silently.
    // -----------------------------------------------------------------------
    void markLeftLying(int slot, double lifeMs) {
        if (slot < 0 || slot >= kDebrisInstances) return;
        debris_[slot].absorbR = kAbsorbNever;
        debris_[slot].lifeMs = lifeMs;
    }

    // One connected piece, as a body. See spawnDebris for the split above.
    int spawnPiece(Physics &ph, const std::vector<uint8_t> &vol, int n, int count,
                   const Vec3 &centre, const Vec3 &vel, const Vec3 &spin, double nowMs,
                   float yawRad, uint8_t takesAs = kDebrisStone,
                   float absorbR = kAbsorbAnywhere) {
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
        d.absorbR = absorbR;
        // -- AND IT IS NOT A CORPSE UNTIL SOMETHING SAYS SO -----------------
        //
        // (user 2026-09-15: "when hitting objects with the stone tools, the
        // chunks are just dissapearing instead of floating towards the
        // player".)
        //
        // A FRESH BODY IN A USED SLOT. hurtT0 is what marks a piece of a killed
        // animal -- it draws the red and, half a second later, retires it. The
        // slots are a pool of sixty-four and a kill takes up to eight of them,
        // so after one rabbit those eight slots carried a hurtT0 from the last
        // thing that died: the next chip of stone cut in one of them inherited
        // it and was retired at 500 ms.
        //
        // WHICH IS EXACTLY WHEN THE ABSORB STARTS (kAbsorbWaitMs), so what the
        // player saw was a chunk that vanished at the precise moment it should
        // have lifted off toward them -- and only after they had killed
        // something, which is what made it look like a different feature.
        //
        // Beside `absorbing`, because it is the same kind of fact: state that
        // belongs to the last occupant and must not outlive it.
        d.hurtT0 = -1e9;
        d.restT0 = -1e9;
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
        d.phys = ph.addChunkBody(cloud.data(), int(cloud.size()), com, yawRad, kStoneDensity);
        // -- ...AND IT LEAVES WITH WHATEVER IT WAS THROWN AT -----------------
        //
        // (user 2026-09-15: "have the life pop up more upon death. its not
        // really noticable".)
        //
        // THE VELOCITY WAS BEING DISCARDED. These two lines were `(void)vel;
        // (void)spin;` -- correct for the caller this function was written for,
        // and only for that one. A swing's chip is deliberately given no throw
        // at all ("NO THROW, NO SPIN, NO NUDGE CLEAR" -- see the note at the
        // swing's own spawnDebris), so dropping the arguments cost that path
        // nothing and nobody noticed.
        //
        // shatterFlyer SHARES THIS FUNCTION AND DOES WANT A LAUNCH. So the
        // corpse's pop has never once been applied: the burst velocity was
        // computed, passed down, and thrown away here. Raising it from v1's
        // 0.6-1.0 m/s to 1.9-2.9 and then to 3.8-5.1 changed nothing either
        // time, which is exactly what "its not really noticable" was reporting
        // -- the pieces were only ever coming apart and falling, and the small
        // rise a test could see was the birth-lift, not a throw.
        //
        // GUARDED ON ZERO rather than made a parameter, so the chip keeps its
        // no-throw by saying what it means -- it passes a zero vector -- and no
        // caller has to opt in to being obeyed.
        if (d.phys >= 0 && (vel.x != 0.0f || vel.y != 0.0f || vel.z != 0.0f ||
                            spin.x != 0.0f || spin.y != 0.0f || spin.z != 0.0f))
            ph.setVelocity(d.phys, vel, spin);
        {
            // A golden-angle turn per slot, so neighbours lean differently.
            const float wa = float(slot) * 2.39996323f;
            d.wobbleAxis = Vec3{std::cos(wa), 0.35f, std::sin(wa)};
            const float wl = sqrtf(d.wobbleAxis.x * d.wobbleAxis.x + 0.1225f +
                                   d.wobbleAxis.z * d.wobbleAxis.z);
            d.wobbleAxis = Vec3{d.wobbleAxis.x / wl, d.wobbleAxis.y / wl, d.wobbleAxis.z / wl};
        }
        // ...AND IT KEEPS ITS OWN VOXELS, so a tool can hit the chip where the
        // chip actually is. The trimmed volume, whose corner is originOff --
        // the same frame the mesh and the hull are in. See Debris::vox.
        d.vox = std::move(trimmed);
        d.vsx = tx;
        d.vsy = ty;
        d.vsz = tz;
        d.takesAs = takesAs;
        d.pos = com;
        d.quat[0] = 0.0f;
        d.quat[1] = std::sin(yawRad * 0.5f);
        d.quat[2] = 0.0f;
        d.quat[3] = std::cos(yawRad * 0.5f);
        // ...AND THE STONE AROUND IT BECOMES SOMETHING TO FALL AGAINST.
        //
        // WHICH KIND OF WINDOW DEPENDS ON HOW BIG IT IS -- see kLongBodyM. A
        // chip takes the fixed 3.2 m cube; anything longer than that cube's own
        // half-span takes a window cut to its bounds, the way a felled tree
        // does, because a body wider than its collider has no floor at its ends.
        d.longBody = 2.0f * maxf(d.halfM[0], maxf(d.halfM[1], d.halfM[2])) > kLongBodyM;
        if (d.longBody) {
            d.winLo = Vec3{com.x - d.halfM[0] - kStaticPadM, com.y - d.halfM[1] - kStaticPadM,
                           com.z - d.halfM[2] - kStaticPadM};
            d.winHi = Vec3{com.x + d.halfM[0] + kStaticPadM, com.y + d.halfM[1] + kStaticPadM,
                           com.z + d.halfM[2] + kStaticPadM};
            d.window = buildSolidWindow(ph, d.winLo, d.winHi);
        } else {
            d.window = buildWindow(ph, com);
        }
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
        // -- THE LEVEL, AND IT HAS TO BE SAMPLED RATHER THAN LOOKED UP ------
        //
        // (user 2026-09-18: "when things become rigid bodies on nuketown, they
        //  fall endlessly and glitch out on the floor".)
        //
        // buildWindow has pushed the level into its solid list since chips were
        // first cut out of the map; THIS function never did, because the only
        // thing routed through it was a felled tree and there are no trees in
        // the level. So anything long cut loose in there got a collider made of
        // the models near it (none) and the TERRAIN (640 m below): no floor.
        //
        // AND PUSHING IT INTO winSolids_ ABOVE DOES NOTHING, which is worth
        // recording because it looks like it should work and it is silent. The
        // loop over winSolids_ turns each one into boxes through boxesFor, and
        // boxesFor returns an EMPTY list for anything whose modelKind is < 0 --
        // it decomposes MODEL TEMPLATES. levelSolid never sets modelKind, so
        // the level is not a model and has no template to decompose. The level
        // is reached the way buildWindow reaches it: sampled through
        // solidAtWorld over a grid, exactly as the walk samples it.
        if (level_) {
            Solid lv;
            if (levelSolid(&lv)) {
                // The window in cells, capped so a long body cannot ask for a
                // sample count that stalls the frame it spawns on.
                const int nx = mini(kLevelWinCells, maxi(1, int((hi.x - lo.x) / VOXEL_M)));
                const int ny = mini(kLevelWinCells, maxi(1, int((hi.y - lo.y) / VOXEL_M)));
                const int nz = mini(kLevelWinCells, maxi(1, int((hi.z - lo.z) / VOXEL_M)));
                std::vector<VoxBox> lvBoxes;
                greedyBoxes(
                    nx, ny, nz, lo, VOXEL_M,
                    [&](int i, int j, int k) {
                        return solidAtWorld(lv, lo.x + (float(i) + 0.5f) * VOXEL_M,
                                            lo.y + (float(j) + 0.5f) * VOXEL_M,
                                            lo.z + (float(k) + 0.5f) * VOXEL_M, VOXEL_M);
                    },
                    &lvBoxes, kStaticMaxBoxes);
                for (const VoxBox &b : lvBoxes) {
                    if (winBoxes_.size() >= kStaticMaxBoxes) break;
                    winBoxes_.push_back(b);
                }
            }
        }

        // -- ...AND THE GROUND, WHICH WAS NEVER IN HERE AT ALL --------------
        //
        // (user 2026-09-17: "I cut down a tree and it fell right through the
        //  terrain.")
        //
        // collidersNear GATHERS MODELS. Trunks, boulders, decor -- every solid
        // in the world EXCEPT the world. So the collider a felled tree was
        // handed described the other trees around it and nothing to land on,
        // and a felled tree is deliberately excluded from the terrain clamp in
        // updateDebris (its half height is thirteen metres, and clamping a
        // body's ORIGIN by that holds it in the sky). Between the two it had
        // no floor of any kind -- only the last-resort rescue below, which
        // fires when the whole trunk is already under the world.
        //
        // IT WORKED BY ACCIDENT, WHICH IS WHY IT IS INTERMITTENT. The player
        // carries a 64 m ground patch (kGroundPatchCols), a separate static
        // actor that a falling body does collide with -- so a tree felled at
        // your feet rests on it and --fell-test reports 0% of its voxels under
        // the ground. Walk away and the patch re-centres on YOU: the tree is
        // outside it, has nothing beneath it, and sinks.
        //
        // SAMPLED, MERGED ALONG X, AND ONLY AS DEEP AS IT NEEDS TO BE. One
        // box per run of equal-height columns, a metre thick -- enough to stop
        // anything resting on it and not so deep that a body already buried is
        // trapped under a slab.
        {
            TerrainMemo memo;
            const int i0 = int(std::floor(lo.x / VOXEL_M));
            const int i1 = int(std::floor(hi.x / VOXEL_M));
            const int j0 = int(std::floor(lo.z / VOXEL_M));
            const int j1 = int(std::floor(hi.z / VOXEL_M));
            const int step = kFellGroundStep;
            for (int j = j0; j <= j1 && winBoxes_.size() < kStaticMaxBoxes; j += step) {
                int runStart = i0;
                int runTop = INT_MIN;
                for (int i = i0; i <= i1 + step; i += step) {
                    const int top = (i <= i1) ? terrainTopAt(i, j, memo) : INT_MIN;
                    if (top == runTop) continue;
                    if (runTop != INT_MIN) {
                        // One box over [runStart, i), a metre deep.
                        const float x0 = float(runStart) * VOXEL_M;
                        const float x1 = float(i) * VOXEL_M;
                        const float surf = float(runTop + 1) * VOXEL_M;
                        VoxBox b;
                        b.cx = 0.5f * (x0 + x1);
                        b.hx = 0.5f * (x1 - x0);
                        b.cz = (float(j) + 0.5f * float(step)) * VOXEL_M;
                        b.hz = 0.5f * float(step) * VOXEL_M;
                        b.cy = surf - 0.5f * kFellGroundThickM;
                        b.hy = 0.5f * kFellGroundThickM;
                        if (b.cy + b.hy >= lo.y && b.cy - b.hy <= hi.y)
                            winBoxes_.push_back(b);
                        if (winBoxes_.size() >= kStaticMaxBoxes) break;
                    }
                    runStart = i;
                    runTop = top;
                }
            }
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
    // -----------------------------------------------------------------------
    // HOW MUCH OF A LOOSE BODY IS INSIDE THE MAP.
    //
    // (user 2026-09-17: "the physics is completely bugged on nuketown. audit
    //  the physics system on nuketown. things are glitching when they become
    //  rigid bodies.")
    //
    // debrisClip beside this asks the same question of MODELS -- it walks
    // collidersNear, which is trunks and boulders, and in the level there are
    // none. So it answers 0 for every body in nuketown however buried it is,
    // which is a true number about the wrong world.
    //
    // A BODY THAT IS BORN PENETRATING IS THE WHOLE OF THE GLITCH. A solver's
    // first duty to one is to get it out, and it does that with an impulse
    // proportional to how deep it is -- so a piece that starts a voxel inside a
    // wall twitches and a piece that starts a metre inside one is fired across
    // the room. Counting the overlap is the only way to tell those apart from
    // the outside, and to tell either from a body that is simply falling.
    //
    // THE BODY'S OWN VOXELS, TURNED THE WAY IT IS TURNED, asked of levelVol_.
    // Not the bounding box: a bounding box round a tilted piece overlaps walls
    // it is nowhere near, which would report a glitch on every body that ever
    // rested against anything.
    // -----------------------------------------------------------------------
    int debrisInLevel(int slot) const {
        if (slot < 0 || slot >= kDebrisInstances || levelVol_.empty()) return 0;
        const Debris &d = debris_[slot];
        if (!d.live || d.vox.empty()) return 0;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        float m[9];
        quatMat3(d.quat, m);
        int inside = 0;
        for (int y = 0; y < d.vsy; ++y)
            for (int z = 0; z < d.vsz; ++z)
                for (int x = 0; x < d.vsx; ++x) {
                    if (d.vox[size_t(x) + size_t(z) * size_t(d.vsx) +
                              size_t(y) * size_t(d.vsx) * size_t(d.vsz)] == mat::AIR)
                        continue;
                    // The voxel's centre in the body's own frame, from the
                    // trimmed volume's corner -- the same originOff the mesh
                    // and the hull are measured from.
                    const float bx = d.originOff.x + (float(x) + 0.5f) * VOXEL_M;
                    const float by = d.originOff.y + (float(y) + 0.5f) * VOXEL_M;
                    const float bz = d.originOff.z + (float(z) + 0.5f) * VOXEL_M;
                    const float wx = m[0] * bx + m[1] * by + m[2] * bz + d.pos.x;
                    const float wy = m[3] * bx + m[4] * by + m[5] * bz + d.pos.y;
                    const float wz = m[6] * bx + m[7] * by + m[8] * bz + d.pos.z;
                    const int lx = int((wx - kLevelAtX) / VOXEL_M);
                    const int ly = int((wy - kLevelAtY) / VOXEL_M);
                    const int lz = int((wz - kLevelAtZ) / VOXEL_M);
                    if (lx < 0 || ly < 0 || lz < 0 || lx >= SX || ly >= SY || lz >= SZ) continue;
                    if (levelVol_[size_t(lx) + size_t(lz) * size_t(SX) +
                                  size_t(ly) * size_t(SX) * size_t(SZ)] != mat::AIR)
                        ++inside;
                }
        return inside;
    }

    // Every live body, and how deep into the map they are between them. For
    // --fire-frame: one number that says whether the bodies a shot made are
    // resting on the world or fighting it.
    void levelPenetration(int *bodies, int *voxelsInside, int *worst, int *worstSlot = nullptr,
                          Vec3 *worstAt = nullptr, int *worstVox = nullptr,
                          bool *worstFelled = nullptr, int *worstDims = nullptr) const {
        int b = 0, v = 0, w = 0, ws = -1;
        for (int i = 0; i < kDebrisInstances; ++i) {
            if (!debris_[i].live) continue;
            ++b;
            const int n = debrisInLevel(i);
            v += n;
            if (n > w) {
                w = n;
                ws = i;
            }
        }
        if (bodies) *bodies = b;
        if (voxelsInside) *voxelsInside = v;
        if (worst) *worst = w;
        if (worstSlot) *worstSlot = ws;
        if (ws >= 0) {
            if (worstAt) *worstAt = debris_[ws].pos;
            if (worstVox) *worstVox = debris_[ws].voxels;
            if (worstDims) {
                worstDims[0] = debris_[ws].vsx;
                worstDims[1] = debris_[ws].vsy;
                worstDims[2] = debris_[ws].vsz;
            }
            if (worstFelled) *worstFelled = debris_[ws].scenery || debris_[ws].felled;
        }
    }

    int debrisClip(int slot) {
        if (slot < 0 || slot >= kDebrisInstances) return 0;
        Debris &d = debris_[slot];
        if (!d.live || d.boxes.empty()) return 0;
        collidersNear(d.pos, 60.0f, &underSolids_);
        // The body's rotation, as a matrix, from its quaternion.
        float m[9];
        quatMat3(d.quat, m);
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
    // HOW MUCH OF A BODY IS UNDER THE GROUND -- ASKED OF WHAT YOU CAN SEE.
    //
    // debrisClip above asks the SOLVER's shape whether it is inside a rock.
    // This asks the DRAWN one whether it is inside the hillside, and the
    // difference between those two questions is the whole of the reported bug:
    // a felled tree's collider is its TRUNK, with the canopy deliberately left
    // out (see kFellFillFrac), so the solver can be perfectly happy while half
    // the branches are buried in the slope. Nothing was ever asking about the
    // branches.
    //
    // PER VOXEL, AND PER COLUMN. The terrain is a height field, so "is this
    // voxel under the ground" is one comparison at that voxel's own (x, z) --
    // which is what makes this immune to the mistake the first reading of
    // --fell-test made. Taking the ground at the CORNERS of a 26 m body's
    // bounding box and comparing it with the body's lowest point measures the
    // SLOPE, not the sinking: a log lying across a hillside is metres below the
    // ground at its uphill corner and perfectly on the surface all along its
    // length.
    //
    // Diagnostic, and slow by design -- it walks every voxel of the model box.
    // Only --fell-test calls it.
    // -----------------------------------------------------------------------
    struct Sink {
        int under = 0;      // voxels whose centre is below the surface
        int solid = 0;      // voxels there are
        float worst = 0.0f; // metres, the deepest one
        float worstAt[3] = {0.0f, 0.0f, 0.0f};
    };

    Sink debrisSink(int slot, const std::function<float(float, float)> &terrainAt) const {
        Sink r;
        if (slot < 0 || slot >= kDebrisInstances || !terrainAt) return r;
        const Debris &d = debris_[slot];
        if (!d.live || d.vox.empty()) return r;
        float m[9];
        quatMat3(d.quat, m);
        for (int y = 0; y < d.vsy; ++y)
            for (int z = 0; z < d.vsz; ++z)
                for (int x = 0; x < d.vsx; ++x) {
                    if (d.vox[size_t(x) + size_t(z) * size_t(d.vsx) +
                              size_t(y) * size_t(d.vsx) * size_t(d.vsz)] == mat::AIR)
                        continue;
                    ++r.solid;
                    const float lx = d.originOff.x + (float(x) + 0.5f) * VOXEL_M;
                    const float ly = d.originOff.y + (float(y) + 0.5f) * VOXEL_M;
                    const float lz = d.originOff.z + (float(z) + 0.5f) * VOXEL_M;
                    const float wx = d.pos.x + m[0] * lx + m[1] * ly + m[2] * lz;
                    const float wy = d.pos.y + m[3] * lx + m[4] * ly + m[5] * lz;
                    const float wz = d.pos.z + m[6] * lx + m[7] * ly + m[8] * lz;
                    const float g = terrainAt(wx, wz);
                    if (wy >= g) continue;
                    ++r.under;
                    if (g - wy > r.worst) {
                        r.worst = g - wy;
                        r.worstAt[0] = wx;
                        r.worstAt[1] = wy;
                        r.worstAt[2] = wz;
                    }
                }
        return r;
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

    // =======================================================================
    // THE FLOAT WATCH. See the block over kFloatRegionVox for why it exists.
    // =======================================================================

    // AN EDIT HAPPENED HERE, SO ASK AGAIN ABOUT IT. Cheap by design -- it
    // snaps to a grid and pushes a key -- because it is called from inside
    // every dig and must not add to the cost of a swing.
    static long long floatKeyOf(const Vec3 &at) {
        const int gi = int(std::floor(at.x / (VOXEL_M * float(kFloatDedupeVox))));
        const int gj = int(std::floor(at.z / (VOXEL_M * float(kFloatDedupeVox))));
        const int gy = int(std::floor(at.y / (VOXEL_M * float(kFloatDedupeVox))));
        return (long long(gi) & 0x1FFFFF) | ((long long(gj) & 0x1FFFFF) << 21) |
               ((long long(gy) & 0x1FFFFF) << 42);
    }

    void markFloatDirty(const Vec3 &at) {
        const long long key = floatKeyOf(at);
        // ALREADY WAITING: push its start time out rather than adding a second
        // entry -- see kFloatSettleMs. This is what collapses a burst of bites
        // into one look.
        for (FloatWait &w : fQueue_)
            if (w.key == key) {
                w.readyMs = floatNowMs_ + kFloatSettleMs;
                return;
            }
        if (fJobKey_ == key && fJob_.live) return;   // already being looked at
        if (fQueue_.size() >= size_t(kFloatQueueMax)) fQueue_.erase(fQueue_.begin());
        fQueue_.push_back(FloatWait{key, floatNowMs_ + kFloatSettleMs});
    }

    struct FloatWatchStat {
        long jobsDone = 0;
        long piecesDropped = 0;
        long voxelsDropped = 0;
        long bodies = 0;
        long queued = 0;
        long biggest = 0;
        // Pieces that ran out of region and were sent round for a second look
        // centred on themselves -- see the re-centre in stepFloatHarvest.
        long deferred = 0;
        // ...and pieces that could not be spawned this time and were left
        // STANDING rather than deleted. A non-zero here is the pool being
        // busy, not geometry being lost.
        long refused = 0;
        // Pieces that would want more than kFloatDicePerPiece bodies. These
        // are the honest limit of the layer and the one thing it does not fix.
        long tooBig = 0;
        // Where the time actually goes. The first build of this cost 170 ms a
        // step and the flood was not the reason -- see the note in
        // dropFloatPiece.
        double spawnMs = 0.0;
        double fillMs = 0.0;
    };
    FloatWatchStat floatStat() const {
        FloatWatchStat s = fStat_;
        s.queued = long(fQueue_.size()) + (fJob_.live ? 1 : 0);
        return s;
    }

    // ONE STEP. Called every frame; does a bounded slice of whatever job is
    // open, or opens the next one. Returns how many bodies it spawned.
    int stepFloatWatch(Physics &ph, double nowMs) {
        floatNowMs_ = nowMs;
        if (!fJob_.live && !fQueue_.empty()) {
            // THE OLDEST ONE THAT HAS SETTLED. Not simply the front: a region
            // somebody is still digging in keeps having its time pushed out,
            // and the ones behind it must not be stuck waiting for it.
            size_t pick = fQueue_.size();
            for (size_t i = 0; i < fQueue_.size(); ++i)
                if (nowMs >= fQueue_[i].readyMs) {
                    pick = i;
                    break;
                }
            if (pick == fQueue_.size()) return 0;   // all of them still warm
            const long long key = fQueue_[pick].key;
            fQueue_.erase(fQueue_.begin() + long(pick));
            beginFloatJob(key);
        }
        if (!fJob_.live) return 0;
        switch (fJob_.phase) {
            case 0: return stepFloatSample();
            case 1: return stepFloatFlood();
            case 3: return stepFloatDrop(ph, nowMs);
            default: return stepFloatHarvest(ph, nowMs);
        }
    }

  private:
    struct FloatJob {
        bool live = false;
        int i0 = 0, j0 = 0, y0 = 0, n = 0;
        std::vector<uint8_t> sol, seen;
        std::vector<int> st;
        std::vector<uint8_t> piece;   // the cube handed to spawnDebris
        TerrainMemo memo;
        int phase = 0;   // 0 sample, 1 flood, 2 harvest, 3 drop
        bool wallPass = false;   // which of the two floods is running
        int cur = 0;     // how far the sample/harvest cursor has walked
        long solid = 0;
        // ---- phase 3: ONE PIECE, DROPPED A DICE AT A TIME ----------------
        //
        // Staged rather than done in the harvest step, and MEASURED is why: a
        // 26,431-voxel lid spawned in one go cost 153 ms, which is nine frames
        // of hitch at the exact moment the player is watching a hillside give
        // way. One body per step is 8 ms.
        //
        // The component is bucketed into dice ONCE. Written as a scan of the
        // whole component per dice it was O(dice * voxels) -- 317,000 passes
        // for this piece, and the shape of it is invisible until the piece is
        // big, which is the only time this code runs at all.
        std::vector<int> comp;
        std::vector<std::vector<int>> dice;
        std::vector<int> diceAt;   // packed cx | cy<<10 | cz<<20
        size_t diceCur = 0;
        int diceMade = 0;
        long diceVox = 0;
    };
    FloatJob fJob_;
    long long fJobKey_ = -1;
    int fPieces_ = 0;   // pieces dealt with in this step -- see kFloatPiecesPerStep
    // A region and the moment it is worth looking at -- see kFloatSettleMs.
    struct FloatWait {
        long long key = 0;
        double readyMs = 0.0;
    };
    std::vector<FloatWait> fQueue_;
    // The last time stepFloatWatch was called. markFloatDirty is reached from
    // inside dig(), which has no clock of its own and must not grow one for
    // this -- a few milliseconds of staleness cannot matter to a 400 ms wait.
    double floatNowMs_ = 0.0;
    FloatWatchStat fStat_;

    size_t fIx(int a, int b, int c) const {
        return size_t(a) + size_t(c) * size_t(fJob_.n) +
               size_t(b) * size_t(fJob_.n) * size_t(fJob_.n);
    }

    // IS THERE ANY EDIT IN THIS REGION AT ALL?
    //
    // (user 2026-09-17: "theres missing terrain in the sand banks now".)
    //
    // GENERATED TERRAIN CANNOT FLOAT, and that is not an approximation -- it is
    // a height field, so a column of height h is solid for every row at or
    // below h, and every solid column therefore reaches the bottom of any
    // region that contains its top. The ONLY thing that can disconnect ground
    // from the ground is a carve.
    //
    // So a region nobody has dug in has nothing for this to find, and any
    // answer it produces there is a bug by construction. Checking first is both
    // the safety rail and the fast path: it is four ints per chunk against a
    // rectangle -- see ChunkEdits::touchesRect, which the ground patch already
    // uses for the same reason -- and it skips the 16,384-column sample
    // entirely for every region that has never been touched.
    bool floatRegionHasEdits(int i0, int j0, int n) const {
        const int cx0 = floorDiv(i0, CHUNK_VOX), cx1 = floorDiv(i0 + n - 1, CHUNK_VOX);
        const int cz0 = floorDiv(j0, CHUNK_VOX), cz1 = floorDiv(j0 + n - 1, CHUNK_VOX);
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx) {
                const std::shared_ptr<const ChunkEdits> ce = mesher_.edits.get(cx, cz);
                if (ce && ce->touchesRect(i0, j0, i0 + n - 1, j0 + n - 1)) return true;
            }
        return false;
    }

    void beginFloatJob(long long key) {
        auto un21 = [](long long v) {
            v &= 0x1FFFFF;
            return int(v >= 0x100000 ? v - 0x200000 : v);
        };
        const int gi = un21(key), gj = un21(key >> 21), gy = un21(key >> 42);
        const int n = kFloatRegionVox;
        // CENTRED ON THE EDIT, not on a fixed lattice: a piece that straddled a
        // lattice boundary would be cut in half by the region and both halves
        // would touch a wall, which is the one answer that is always "attached".
        fJob_ = FloatJob{};
        fJob_.n = n;
        fJob_.i0 = gi * kFloatDedupeVox + kFloatDedupeVox / 2 - n / 2;
        fJob_.j0 = gj * kFloatDedupeVox + kFloatDedupeVox / 2 - n / 2;
        fJob_.y0 = gy * kFloatDedupeVox + kFloatDedupeVox / 2 - n / 2;
        // NOTHING HAS BEEN DUG HERE, so there is nothing this could have cut
        // loose -- see floatRegionHasEdits. Dropped before the sample, which is
        // the expensive part.
        if (!floatRegionHasEdits(fJob_.i0, fJob_.j0, n)) {
            fJob_ = FloatJob{};
            fJobKey_ = -1;
            return;
        }
        fJob_.sol.assign(size_t(n) * size_t(n) * size_t(n), 0);
        fJob_.seen.assign(fJob_.sol.size(), 0);
        fJob_.live = true;
        fJob_.phase = 0;
        fJob_.cur = 0;
        fJobKey_ = key;
    }

    // ---- phase 0: what is solid in here, a slice of columns at a time -----
    //
    // BY COLUMN, NOT BY VOXEL, AND THAT IS THE DIFFERENCE BETWEEN THIS BEING
    // AFFORDABLE AND NOT. Written as a TerrainProbe::solid per voxel it was
    // 884,736 lookups a region and 8 ms a step -- half a frame, every frame,
    // for ever.
    //
    // AN UNEDITED COLUMN IS SOLID FROM BEDROCK TO ITS HEIGHT, by construction:
    // nothing has been taken out of it, so there is nothing to look up. One
    // heightVox answers the whole column. Only the rows somebody has actually
    // dug need asking, and ChunkEdits::col already records exactly that span
    // per column -- it is what meshChunk uses to keep unedited columns on the
    // fast heightmap path, and it does the same job here.
    //
    // THE EDIT POINTER IS CACHED PER CHUNK, not fetched per voxel:
    // EditStore::get takes a lock, and a 9.6 m region touches four chunks at
    // worst. dropTerrainHangers found the same thing and says so.
    int stepFloatSample() {
        const int n = fJob_.n;
        const int end = mini(n * n, fJob_.cur + kFloatSampleCols);
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
            return mesher_.edits.get(cx, cz).get();
        };
        for (; fJob_.cur < end; ++fJob_.cur) {
            const int a = fJob_.cur % n, c = fJob_.cur / n;
            const int wi = fJob_.i0 + a, wj = fJob_.j0 + c;
            const int h = terrain.heightVox(wi, wj, fJob_.memo);
            // The rows of this column that anybody has edited, or an empty
            // span. Outside it the generator's answer is the whole answer.
            int e0 = 1, e1 = 0;
            const ChunkEdits *ce = editsAt(wi, wj);
            if (ce) {
                const auto it = ce->col.find(ChunkEdits::ckey(wi, wj));
                if (it != ce->col.end()) {
                    e0 = it->second.first;
                    e1 = it->second.second - 1;
                }
            }
            for (int b = 0; b < n; ++b) {
                const int wy = fJob_.y0 + b;
                if (wy > h) {
                    // ...unless somebody PUT something up here. The hoe is the
                    // first writer that adds rather than removes and there will
                    // be others, so an edited row above the surface is asked
                    // rather than assumed empty.
                    if (wy < e0 || wy > e1 || !ce) continue;
                    uint8_t m = mat::AIR;
                    if (!ce->voxel(wi, wj, wy, &m) || m == mat::AIR) continue;
                } else if (wy >= e0 && wy <= e1 && ce) {
                    uint8_t m = mat::AIR;
                    if (ce->voxel(wi, wj, wy, &m) && m == mat::AIR) continue;
                }
                fJob_.sol[fIx(a, b, c)] = 1;
                ++fJob_.solid;
            }
        }
        if (fJob_.cur < n * n) return 0;
        // ---- seed: THE FLOOR, AND ONLY THE FLOOR --------------------------
        //
        // TWO FLOODS, NOT ONE, AND THE FIRST VERSION MERGED THEM AND WENT
        // BLIND. It seeded the floor AND all four walls together, reasoning
        // that anything leaving the region might be held from outside it and
        // so must not be judged. That reasoning is right and the implementation
        // of it was not: a seeded voxel is a REACHED voxel, so a piece touching
        // a wall never appeared as a component at all -- it could not be
        // dropped, and it could not be noticed either. MEASURED: at two spawns
        // in four the 26,000-voxel lid was invisible, 24 regions found nothing,
        // and the watch reported a clean sweep over a hillside hanging in the
        // air.
        //
        // So the floor is flooded on its own first. What it reaches is GROUND,
        // full stop. See stepFloatFlood for the second pass, which is where
        // the walls come in.
        //
        // THE CEILING IS NEVER A SEED, in either pass. A voxel at the top of
        // the region hanging over a hole is exactly what this is looking for,
        // and seeding it would declare it supported by the sky.
        fJob_.st.clear();
        for (int c = 0; c < n; ++c)
            for (int a = 0; a < n; ++a) {
                const size_t q = fIx(a, 0, c);
                if (!fJob_.sol[q] || fJob_.seen[q]) continue;
                fJob_.seen[q] = kFloatGrounded;
                fJob_.st.push_back(int(q));
            }
        // -- NO GROUND ON THE FLOOR MEANS THIS LOOK CANNOT JUDGE ANYTHING ---
        //
        // (user 2026-09-17: "theres missing terrain in the sand banks now".)
        //
        // THIS IS HOW A BACKSTOP AGAINST FLOATING GROUND DELETES GROUND. With
        // no seed on the region floor nothing is ever marked grounded, so pass
        // one finds nothing, and any mass that also happens not to touch a side
        // wall -- a mound smaller than the region, sitting on ground that falls
        // away below the floor row -- comes out of pass two unmarked. Unmarked
        // is the verdict "floating", and the harvest carves it. A sand bank is
        // exactly that shape.
        //
        // The honest answer is that a region with no floor has no reference to
        // measure against, and "I cannot tell" must never carve. Abandoned, not
        // deferred: re-centring would build a region with the same floor.
        if (fJob_.st.empty()) {
            ++fStat_.refused;
            fJob_ = FloatJob{};
            fJobKey_ = -1;
            return 0;
        }
        fJob_.phase = 1;
        fJob_.wallPass = false;
        return 0;
    }

    // ---- phase 1: two floods -- what the GROUND reaches, then what the
    //      WALLS reach of whatever is left ---------------------------------
    //
    // PASS ONE IS THE VERDICT. Anything the floor reaches is standing on the
    // world and is nobody's problem.
    //
    // PASS TWO IS THE ADMISSION OF IGNORANCE. Of what is left, whatever
    // touches a side wall may be held by something just outside the region --
    // this look cannot say, and dropping it would be the one unrecoverable
    // mistake here: a hillside cut in half because the ruler was too short.
    // Those are marked kFloatOutside, left alone, and sent round for a second
    // look centred on themselves.
    //
    // WHAT IS LEFT AFTER BOTH is unreachable from the ground and touches
    // nothing outside the region. That is floating, with no qualification.
    int stepFloatFlood() {
        const int n = fJob_.n;
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        const uint8_t mark = fJob_.wallPass ? kFloatOutside : kFloatGrounded;
        int budget = kFloatFloodCells;
        while (!fJob_.st.empty() && budget-- > 0) {
            const int p = fJob_.st.back();
            fJob_.st.pop_back();
            const int a = p % n, c = (p / n) % n, b = p / (n * n);
            for (const int *o : off) {
                const int x = a + o[0], y = b + o[1], z = c + o[2];
                if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) continue;
                const size_t q = fIx(x, y, z);
                if (!fJob_.sol[q] || fJob_.seen[q]) continue;
                fJob_.seen[q] = mark;
                fJob_.st.push_back(int(q));
            }
        }
        if (!fJob_.st.empty()) return 0;
        if (!fJob_.wallPass) {
            // ---- and now the walls, over what the ground could not reach ---
            fJob_.wallPass = true;
            for (int b = 0; b < n; ++b)
                for (int a = 0; a < n; ++a) {
                    for (int c : {0, n - 1}) {
                        const size_t q = fIx(a, b, c);
                        if (!fJob_.sol[q] || fJob_.seen[q]) continue;
                        fJob_.seen[q] = kFloatOutside;
                        fJob_.st.push_back(int(q));
                    }
                }
            for (int b = 0; b < n; ++b)
                for (int c = 0; c < n; ++c) {
                    for (int a : {0, n - 1}) {
                        const size_t q = fIx(a, b, c);
                        if (!fJob_.sol[q] || fJob_.seen[q]) continue;
                        fJob_.seen[q] = kFloatOutside;
                        fJob_.st.push_back(int(q));
                    }
                }
            if (!fJob_.st.empty()) return 0;
        }
        fJob_.phase = 2;
        fJob_.cur = 0;
        return 0;
    }

    // ---- phase 2: and out they come ---------------------------------------
    int stepFloatHarvest(Physics &ph, double nowMs) {
        const int n = fJob_.n;
        const size_t total = fJob_.sol.size();
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        int spawned = 0;
        while (size_t(fJob_.cur) < total) {
            const size_t q0 = size_t(fJob_.cur);
            if (!fJob_.sol[q0] || fJob_.seen[q0]) {
                ++fJob_.cur;
                continue;
            }
            // ONE COMPONENT, WHOLE. Gathered in one go rather than across
            // frames: it has to be carved and spawned as a unit, and a piece
            // half of which has fallen is worse than either answer.
            std::vector<int> comp;
            fJob_.seen[q0] = 1;
            fJob_.st.clear();
            fJob_.st.push_back(int(q0));
            int lo[3] = {n, n, n}, hi[3] = {-1, -1, -1};
            while (!fJob_.st.empty()) {
                const int p = fJob_.st.back();
                fJob_.st.pop_back();
                comp.push_back(p);
                const int a = p % n, c = (p / n) % n, b = p / (n * n);
                if (a < lo[0]) lo[0] = a;
                if (b < lo[1]) lo[1] = b;
                if (c < lo[2]) lo[2] = c;
                if (a > hi[0]) hi[0] = a;
                if (b > hi[1]) hi[1] = b;
                if (c > hi[2]) hi[2] = c;
                for (const int *o : off) {
                    const int x = a + o[0], y = b + o[1], z = c + o[2];
                    if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) continue;
                    const size_t q = fIx(x, y, z);
                    if (!fJob_.sol[q] || fJob_.seen[q]) continue;
                    fJob_.seen[q] = 1;
                    fJob_.st.push_back(int(q));
                }
            }
            ++fJob_.cur;
            const int made = stageFloatPiece(comp, lo, hi);
            if (made > 0) {
                fJob_.phase = 3;   // and the dice go one per step from here
                return spawned;
            }
            if (!made) {
                // IT DID NOT FIT. Put the region back and stop: the piece is
                // still standing and still disconnected, and the next pass
                // over a drained debris pool will take it. Re-queued by its
                // own centre so the second look is at least as good as this
                // one -- and only if that is a different cell, or this is a
                // loop. (fJobKey_ is cleared when the job ends, so the mark
                // has to happen before that.)
                double sx = 0, sy = 0, sz = 0;
                for (int q : comp) {
                    sx += double(q % n);
                    sz += double((q / n) % n);
                    sy += double(q / (n * n));
                }
                const double inv = 1.0 / double(comp.size());
                const Vec3 mid{(float(fJob_.i0) + float(sx * inv)) * VOXEL_M,
                               (float(fJob_.y0) + float(sy * inv)) * VOXEL_M,
                               (float(fJob_.j0) + float(sz * inv)) * VOXEL_M};
                const long long here = fJobKey_;
                fJobKey_ = -1;              // so the dedupe does not refuse it
                markFloatDirty(mid);
                fJobKey_ = here;
                ++fStat_.refused;
                return spawned;
            }
            if (++fPieces_ >= kFloatPiecesPerStep) {
                fPieces_ = 0;
                return spawned;   // the rest of this region next frame
            }
        }
        // ---- ...AND THE ONES THIS REGION COULD NOT ANSWER FOR -------------
        //
        // Marked kFloatOutside by the wall pass: not grounded in here, but
        // touching the edge, so something just outside may be holding them.
        // Each gets ONE second look centred on itself.
        //
        // THE MEASURED CASE IS EXACTLY THIS. The lid is freed by a ring cut
        // and every blow of that ring is four metres from the lid's centre, so
        // the region a ring blow queues is four metres off and the lid runs
        // out of it on the far side. Centred on the piece it fits with room to
        // spare and the second look answers properly -- at two spawns in four
        // this is the only thing that finds it at all.
        //
        // IT TERMINATES. A re-centre must land in a DIFFERENT dedupe cell from
        // the one being looked at: the same cell would build the same region
        // and reach the same answer, so re-queueing it is a loop. A piece too
        // big to ever fit therefore stops after one hop and is left standing,
        // which is the safe direction, and fStat_.deferred says it happened
        // rather than hiding it.
        {
            std::vector<uint8_t> done(fJob_.seen.size(), 0);
            int hops = 0;
            for (size_t i = 0; i < fJob_.sol.size() && hops < kFloatRecentreMax; ++i) {
                if (fJob_.seen[i] != kFloatOutside || done[i]) continue;
                std::vector<int> comp;
                done[i] = 1;
                fJob_.st.clear();
                fJob_.st.push_back(int(i));
                // nb, not off: stepFloatHarvest already has an `off` in scope
                // and /WX turns the shadow into an error.
                static const int nb[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                             {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
                double sx = 0, sy = 0, sz = 0;
                while (!fJob_.st.empty()) {
                    const int p = fJob_.st.back();
                    fJob_.st.pop_back();
                    comp.push_back(p);
                    const int a = p % n, c = (p / n) % n, b = p / (n * n);
                    sx += double(a);
                    sz += double(c);
                    sy += double(b);
                    for (const int *o : nb) {
                        const int x = a + o[0], y = b + o[1], z = c + o[2];
                        if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) continue;
                        const size_t q = fIx(x, y, z);
                        if (fJob_.seen[q] != kFloatOutside || done[q]) continue;
                        done[q] = 1;
                        fJob_.st.push_back(int(q));
                    }
                }
                const double inv = 1.0 / double(comp.size());
                const Vec3 mid{(float(fJob_.i0) + float(sx * inv)) * VOXEL_M,
                               (float(fJob_.y0) + float(sy * inv)) * VOXEL_M,
                               (float(fJob_.j0) + float(sz * inv)) * VOXEL_M};
                if (floatKeyOf(mid) == fJobKey_) continue;   // the same question
                ++fStat_.deferred;
                ++hops;
                markFloatDirty(mid);
            }
        }
        ++fStat_.jobsDone;
        fJob_ = FloatJob{};
        fJobKey_ = -1;
        return spawned;
    }

    // ONE FLOATING COMPONENT, BUCKETED INTO DICE AND QUEUED.
    //
    // Returns the number of dice it will take, or 0 if the piece cannot be
    // dropped now -- too big, or the debris pool is busy. NOTHING IS SPAWNED
    // AND NOTHING IS CARVED HERE: see the note on FloatJob::comp.
    int stageFloatPiece(const std::vector<int> &comp, const int lo[3], const int hi[3]) {
        const int n = fJob_.n;
        if (comp.empty()) return 0;
        const int dn = kFloatDiceVox;
        const int cx0 = lo[0] / dn, cx1 = hi[0] / dn;
        const int cy0 = lo[1] / dn, cy1 = hi[1] / dn;
        const int cz0 = lo[2] / dn, cz1 = hi[2] / dn;
        const int wx = cx1 - cx0 + 1, wy = cy1 - cy0 + 1, wz = cz1 - cz0 + 1;
        if (wx * wy * wz > kFloatDicePerPiece) {
            ++fStat_.tooBig;
            return 0;
        }
        std::vector<std::vector<int>> buck(size_t(wx) * size_t(wy) * size_t(wz));
        for (int q : comp) {
            const int a = q % n, c = (q / n) % n, b = q / (n * n);
            buck[size_t(a / dn - cx0) + size_t(b / dn - cy0) * size_t(wx) +
                 size_t(c / dn - cz0) * size_t(wx) * size_t(wy)]
                .push_back(q);
        }
        int need = 0;
        for (const auto &v : buck)
            if (!v.empty()) ++need;
        // ---- ALL OF A PIECE OR NONE OF IT -------------------------------
        //
        // See kFloatDicePerPiece. The slots are counted before anything is
        // spawned or carved, because half a piece leaving the world is the
        // vanishing bug this whole rule exists to answer -- and it was rebuilt
        // from scratch inside this very function once already.
        if (need > kFloatDicePerPiece || need > debrisFree()) {
            if (need > kFloatDicePerPiece) ++fStat_.tooBig;
            return 0;
        }
        fJob_.comp = comp;
        fJob_.dice.clear();
        fJob_.diceAt.clear();
        for (int cz = 0; cz < wz; ++cz)
            for (int cy = 0; cy < wy; ++cy)
                for (int cx = 0; cx < wx; ++cx) {
                    auto &v = buck[size_t(cx) + size_t(cy) * size_t(wx) +
                                   size_t(cz) * size_t(wx) * size_t(wy)];
                    if (v.empty()) continue;
                    fJob_.dice.push_back(std::move(v));
                    fJob_.diceAt.push_back((cx0 + cx) | ((cy0 + cy) << 10) | ((cz0 + cz) << 20));
                }
        fJob_.diceCur = 0;
        fJob_.diceMade = 0;
        fJob_.diceVox = 0;
        return need;
    }

    // ---- phase 3: one body per step, then the hole ------------------------
    int stepFloatDrop(Physics &ph, double nowMs) {
        const int n = fJob_.n, dn = kFloatDiceVox;
        if (fJob_.diceCur < fJob_.dice.size()) {
            const std::vector<int> &cell = fJob_.dice[fJob_.diceCur];
            const int packed = fJob_.diceAt[fJob_.diceCur];
            const int cx = packed & 0x3FF, cy = (packed >> 10) & 0x3FF, cz = (packed >> 20) & 0x3FF;
            ++fJob_.diceCur;
            if (fJob_.piece.size() != size_t(dn) * size_t(dn) * size_t(dn))
                fJob_.piece.assign(size_t(dn) * size_t(dn) * size_t(dn), mat::AIR);
            std::fill(fJob_.piece.begin(), fJob_.piece.end(), mat::AIR);
            int nvox = 0;
            for (int q : cell) {
                const int a = q % n, c = (q / n) % n, b = q / (n * n);
                const int wi = fJob_.i0 + a, wj = fJob_.j0 + c, wy = fJob_.y0 + b;
                // THE MATERIAL COMES FROM THE GENERATOR, SAMPLED BEFORE THE
                // CARVE -- exactly as dropTerrainHangers and the spoil do it.
                // After the carve every one of these is air and the body would
                // be invisible.
                const int h = terrain.heightVox(wi, wj, fJob_.memo);
                const uint8_t top = terrain.topMaterial(wi, wj, h);
                const uint8_t m = terrain.materialAt(wi, wj, wy, h, top);
                if (m == mat::AIR) continue;
                // meshVolume's layout is x + z*n + y*n*n.
                fJob_.piece[size_t(a - cx * dn) + size_t(c - cz * dn) * size_t(dn) +
                            size_t(b - cy * dn) * size_t(dn) * size_t(dn)] = m;
                ++nvox;
            }
            if (!nvox) return 0;
            const Vec3 centre{(float(fJob_.i0 + cx * dn) + float(dn) * 0.5f) * VOXEL_M,
                              (float(fJob_.y0 + cy * dn) + float(dn) * 0.5f) * VOXEL_M,
                              (float(fJob_.j0 + cz * dn) + float(dn) * 0.5f) * VOXEL_M};
            const Vec3 still{0.0f, 0.0f, 0.0f};
            const auto ts = std::chrono::steady_clock::now();
            const int sl = spawnDebris(ph, fJob_.piece, dn, centre, still, still, nowMs, 0.0f,
                                       nullptr, uint8_t(kDebrisStone), kAbsorbAnywhere);
            fStat_.spawnMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts)
                    .count();
            if (sl >= 0) {
                ++fJob_.diceMade;
                fJob_.diceVox += nvox;
            }
            return sl >= 0 ? 1 : 0;
        }
        // ---- every dice is a body now, so the ground goes ------------------
        //
        // ONE CARVE FOR THE WHOLE PIECE. EditStore::carve is copy-on-write --
        // it rebuilds the chunk's whole edit map per call -- which is right for
        // a tool bite of one or two voxels and quadratic for twenty-six
        // thousand. MEASURED at 95 SECONDS before this used carveCells, which
        // takes the lock once, copies once per chunk, and hands back the chunks
        // to re-mesh so the request is made once each rather than once a voxel.
        if (fJob_.diceMade > 0) {
            std::vector<std::array<int, 3>> cells;
            cells.reserve(fJob_.comp.size());
            for (int q : fJob_.comp) {
                const int a = q % n, c = (q / n) % n, b = q / (n * n);
                cells.push_back({fJob_.i0 + a, fJob_.j0 + c, fJob_.y0 + b});
            }
            for (const auto &t : mesher_.edits.carveCells(cells)) {
                const long long k = chunkKey(t.first, t.second);
                if (!chunks_.count(k)) continue;
                requested_.insert(k);
                mesher_.request(t.first, t.second);
            }
            ++fStat_.piecesDropped;
            fStat_.bodies += fJob_.diceMade;
            fStat_.voxelsDropped += fJob_.diceVox;
            if (long(fJob_.comp.size()) > fStat_.biggest) fStat_.biggest = long(fJob_.comp.size());
        }
        fJob_.comp.clear();
        fJob_.dice.clear();
        fJob_.diceAt.clear();
        fJob_.phase = 2;   // back to looking for the next piece in this region
        return 0;
    }

  public:
    // WHAT THE SEVER TEST ITSELF SEES, as opposed to what the mesher draws.
    //
    // (user 2026-09-17: "when cutting down a tree, the top half just floats.
    //  theres something in the middle blocking it? maybe by the axe hitting the
    //  wood, makes it dissapear on the renderer but is still there in memory?")
    //
    // THE TWO ANSWERS ARE ALLOWED TO DIFFER AND THAT IS THE SUSPECT.
    // looseFraction floods COARSE cells of kSeverCell voxels, and a cell counts
    // as solid if ANY voxel in it is -- so a one-voxel bridge the mesher has
    // already stopped drawing still carries the coarse flood across, fellTree
    // says the trunk is attached, and the top hangs there looking severed
    // because it IS severed everywhere except in the test.
    //
    // --float-test has warned about this gap for weeks and calls it never
    // attempted. This is the accessor that lets a harness put the two numbers
    // on one line and find out whether it is actually what is happening here.
    bool coarseLoose(const Solid &s, int *cells, int *born, float *frac) {
        if (s.modelKind < 0 || !s.vol) return false;
        const ModelTemplate &t = templateFor(s.modelKind, s.modelIndex);
        if (t.volume.empty()) return false;
        const auto dit = damaged_.find({s.ownerChunk, int(s.decorSlot)});
        const std::vector<uint8_t> &vol = (dit == damaged_.end()) ? t.volume : dit->second.vol;
        if (t.bornLoose < 0) {
            looseFraction(t.volume, t.sx, t.sy, t.sz);
            t.bornLoose = sevLooseCells_;
        }
        looseFraction(vol, t.sx, t.sy, t.sz);
        if (cells) *cells = sevLooseCells_;
        if (born) *born = t.bornLoose;
        if (frac) *frac = sevLooseFrac_;
        return true;
    }

    // HOW MUCH OF THIS INSTANCE IS LEFT, AND HOW MUCH OF IT IS IN THE WAY.
    //
    // (user 2026-09-17: "when cutting down a tree, the top half just floats.
    //  theres something in the middle blocking it?")
    //
    // `cutY` is a world height; the row it lands on is the one the axe has been
    // working at. A trunk that still has a hundred voxels across that row has
    // not been cut through, however deep the notch looks from outside -- and
    // that is a different fault from a sever test that cannot see a cut, so the
    // two have to be told apart before either is fixed.
    bool modelSolidProfile(const Solid &s, float cutY, int *total, int *atCut, int *row) {
        if (s.modelKind < 0 || !s.vol) return false;
        const ModelTemplate &t = templateFor(s.modelKind, s.modelIndex);
        if (t.volume.empty()) return false;
        const auto dit = damaged_.find({s.ownerChunk, int(s.decorSlot)});
        const std::vector<uint8_t> &vol = (dit == damaged_.end()) ? t.volume : dit->second.vol;
        const int ry = mini(t.sy - 1, maxi(0, int((cutY - s.baseY) / VOXEL_M)));
        int tot = 0, cut = 0;
        for (int y = 0; y < t.sy; ++y)
            for (int z = 0; z < t.sz; ++z)
                for (int x = 0; x < t.sx; ++x)
                    if (vol[size_t(x) + size_t(z) * size_t(t.sx) +
                            size_t(y) * size_t(t.sx) * size_t(t.sz)] != mat::AIR) {
                        ++tot;
                        if (y == ry) ++cut;
                    }
        if (total) *total = tot;
        if (atCut) *atCut = cut;
        if (row) *row = ry;
        return true;
    }

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

    // -----------------------------------------------------------------------
    // ...AND WHAT THOSE LOOSE VOXELS ACTUALLY ARE.
    //
    // (user 2026-09-17: "I need you to audit the game for floating objects".)
    //
    // A COUNT CANNOT TELL A SEVERED BRANCH FROM LEAF LITTER, and until now that
    // is all there was. 31,450 loose voxels is a canopy that came off its trunk
    // if it is one piece and a scatter of needles if it is four hundred, and
    // the two want opposite responses -- one is the bug the nothing-floats rule
    // exists for, the other is how the .vox was drawn.
    //
    // So this labels the components instead: how many, how big the worst is,
    // and how many are big enough to be worth calling an object. Everything
    // else in this file that decides whether a piece should FALL can then be
    // argued about with the right number in hand.
    // -----------------------------------------------------------------------
    struct LooseReport {
        long voxels = 0;     // loose voxels in total
        int pieces = 0;      // ...in this many six-connected components
        long largest = 0;    // the biggest one
        long chunky = 0;     // pieces of kFloatPieceVox or more -- real objects
        long chunkyVox = 0;  // and how many voxels are in those
    };

    static LooseReport fineLooseReport(const std::vector<uint8_t> &vol, int sx, int sy, int sz) {
        LooseReport r;
        if (vol.empty()) return r;
        auto at = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(sx) + size_t(y) * size_t(sx) * size_t(sz);
        };
        std::vector<uint8_t> seen(vol.size(), 0);
        std::vector<int> st;
        // ---- 1. everything the ground can reach, from the model's own floor
        for (int z = 0; z < sz; ++z)
            for (int x = 0; x < sx; ++x) {
                const size_t i = at(x, 0, z);
                if (vol[i] == mat::AIR || seen[i]) continue;
                seen[i] = 1;
                st.push_back(int(i));
            }
        static const int off[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        auto flood = [&](long *count) {
            while (!st.empty()) {
                const int p = st.back();
                st.pop_back();
                if (count) ++*count;
                const int x = p % sx, z = (p / sx) % sz, y = p / (sx * sz);
                for (const int *o : off) {
                    const int a = x + o[0], b = y + o[1], c = z + o[2];
                    if (a < 0 || b < 0 || c < 0 || a >= sx || b >= sy || c >= sz) continue;
                    const size_t q = at(a, b, c);
                    if (vol[q] == mat::AIR || seen[q]) continue;
                    seen[q] = 1;
                    st.push_back(int(q));
                }
            }
        };
        flood(nullptr);
        // ---- 2. ...and every island the ground could not, one at a time
        for (size_t i = 0; i < vol.size(); ++i) {
            if (vol[i] == mat::AIR || seen[i]) continue;
            seen[i] = 1;
            st.push_back(int(i));
            long n = 0;
            flood(&n);
            r.voxels += n;
            ++r.pieces;
            if (n > r.largest) r.largest = n;
            if (n >= kFloatPieceVox) {
                ++r.chunky;
                r.chunkyVox += n;
            }
        }
        return r;
    }

    // WHAT THIS INSTANCE LOOKS LIKE NOW AND WHAT IT LOOKED LIKE WHEN IT WAS
    // DRAWN. Both, because only the difference is anybody's fault -- see the
    // note over looseVoxelsNow, and the boulder that is 95% loose untouched.
    bool looseReportNow(const Solid &s, LooseReport *born, LooseReport *now) {
        if (s.modelKind < 0 || !s.vol) return false;
        const ModelTemplate &t = templateFor(s.modelKind, s.modelIndex);
        if (t.volume.empty()) return false;
        const auto dit = damaged_.find({s.ownerChunk, int(s.decorSlot)});
        const std::vector<uint8_t> &vol = (dit == damaged_.end()) ? t.volume : dit->second.vol;
        if (born) *born = fineLooseReport(t.volume, t.sx, t.sy, t.sz);
        if (now) *now = fineLooseReport(vol, t.sx, t.sy, t.sz);
        return true;
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

    // Where a loose body's shapes actually are, in the world -- diagnostic.
    // The pose is the model's ORIGIN CORNER, which for a felled tree is metres
    // from the wood and says nothing about whether the thing is on the ground;
    // this is what does.
    bool debrisBounds(Physics &ph, int slot, Vec3 *lo, Vec3 *hi) const {
        if (slot < 0 || slot >= kDebrisInstances) return false;
        const Debris &d = debris_[slot];
        if (!d.live || d.phys < 0) return false;
        return ph.boundsOf(d.phys, lo, hi);
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
        // THE LEVEL IS NOT IN A CHUNK, so collidersNear cannot find it -- the
        // same gap App::walkWorld fills by hand for the walk. Without this a
        // chip shot out of a wall in the level has nothing under it and falls
        // the whole 640 m to a wood it is not even in. See levelSolid.
        if (level_) {
            Solid lv;
            if (levelSolid(&lv)) winSolids_.push_back(lv);
        }
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
        // THE CLOCK THE CORPSE FADE READS. setDebrisInstance is handed no time
        // of its own -- it is called from here and from the spawn -- so the one
        // place that has it every frame keeps it. See corpseFade.
        lastDebrisMs_ = nowMs;
        sweepLoose();    // free what the device has finished with
        pumpRemesh();    // ...and take whatever the re-mesher finished

        // How many collision windows may be rebuilt this frame -- see the
        // re-centre block below for why there is a ceiling on it at all.
        int winBudget = kWinRebuildsPerFrame;
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
                // ...AND IT ASKS ABOUT THE HOLE, NOT ABOUT THE WALL BESIDE
                // IT (user 2026-09-14, twice: "when diging deep tin the ground
                // dirt voxels teleport to the surface instantly").
                //
                // MEASURED, WITH --shaft-test. A chip cut four metres down sat
                // still for fourteen frames and then went from 26.52 m to
                // 31.20 m in ONE -- and 31.20 m is the RIM plus the chip's own
                // half height, which is this clamp firing with the ground under
                // the wall it had rolled against. One column is the wrong
                // question to ask on behalf of a body that has width: a pick
                // bite is three voxels across and a chip is nearly two, so the
                // column under its origin is not reliably the column it is
                // standing in.
                //
                // SO THE LOWEST GROUND IT OVERLAPS. This is a backstop for a
                // body that has fallen out of the world, and the two ways of
                // being wrong are not equal -- see the same argument over
                // groundPatch. Too low is a chip that stays in a pit half a
                // metre longer than it might have; too high is the report.
                //
                // AND AT LEAST TWO VOXELS OUT, because half of a chip is one
                // and a half and the shaft it was cut from is three: probing
                // its own width alone can miss the hole it is in.
                // -- ...AND INDOORS IT IS A DIFFERENT QUESTION ------------
                //
                // ASKED OF THE WORLD ITSELF, NOT THROUGH THE CALLBACK. The
                // level is World's own asset, so routing this back out to a
                // lambda in App that then calls a World method was a loop
                // through the one object that already had the answer -- and
                // the lambda could only be handed (x, z), which is precisely
                // the information that is NOT sufficient inside a building.
                // See levelFloorBelowM for what that cost.
                // ...AND NOT FOR A LONG ONE EITHER (see kLongBodyM). The
                // exclusion above is written for the felled tree -- "its half
                // height is thirteen metres, and clamping a body's ORIGIN by
                // that holds it in the sky" -- and that argument is about
                // LENGTH, not about being a tree. A 3.4 m light pole clamped by
                // its 1.7 m half height hangs exactly as wrongly.
                if (!d.felled && !d.longBody && (level_ || terrainAt)) {
                    const float px = maxf(d.halfM[0], VOXEL_M * 2.0f);
                    const float pz = maxf(d.halfM[2], VOXEL_M * 2.0f);
                    // THE LOWEST FLOOR IT OVERLAPS, at its own corners -- the
                    // shaft-test argument, unchanged: one column is the wrong
                    // question on behalf of a body that has width.
                    const auto at = [&](float x, float z) {
                        return level_ ? levelFloorBelowM(x, d.pos.y, z) : terrainAt(x, z);
                    };
                    float g = at(d.pos.x, d.pos.z);
                    g = minf(g, at(d.pos.x - px, d.pos.z - pz));
                    g = minf(g, at(d.pos.x + px, d.pos.z - pz));
                    g = minf(g, at(d.pos.x - px, d.pos.z + pz));
                    g = minf(g, at(d.pos.x + px, d.pos.z + pz));
                    const float floorY = g + d.halfM[1];
                    if (d.pos.y < floorY - 0.5f) {
                        ph.clampAbove(d.phys, floorY);
                        ph.poseOf(d.phys, &d.pos, d.quat);
                    }
                }
                // ---- THE HINGE FIRES ON THE FRAME IT LANDS ---------------
                //
                // Not at the cut. A trunk severed halfway up has to DROP onto
                // its own stump first -- see the note over the stop in
                // fellTree -- and only then go over. "Landed" is the frame its
                // descent is arrested: it was born falling, so a downward speed
                // that is no longer downward means something is underneath it.
                //
                // The 100 ms floor is so the very first frame, before gravity
                // has moved it at all, is not read as a landing; a tree cut at
                // the base has nothing to fall and arms on the next one.
                if (d.felled && d.tipArmed && nowMs - d.bornMs > 100.0) {
                    Vec3 lin{0, 0, 0}, ang{0, 0, 0};
                    if (ph.velocityOf(d.phys, &lin, &ang) && lin.y > -0.35f) {
                        ph.nudgeSpin(d.phys, d.tipAxis, kFellNudge);
                        d.tipArmed = false;
                    }
                }

                // ...AND THE SOLID WORLD FOLLOWS IT DOWN.
                //
                // A window built once at the stump is a window the far end of
                // the tree leaves in the first second of the fall. The bounds
                // are asked every frame -- they are free, PhysX keeps them --
                // and the window is rebuilt only when the body has actually
                // reached the edge of what it covers.
                if (d.felled || d.longBody) {
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

                // -- ...AND SO DOES EVERY OTHER FALLING PIECE ------------
                //
                // (user 2026-09-17, on the level: "the chunks ... proceed to
                //  endlessly fall forever".)
                //
                // A CHIP'S WINDOW WAS BUILT ONCE, AT THE HOLE, AND NEVER
                // AGAIN. It is 3.2 m of solid world around where the piece was
                // cut, which is everything a chip that drops onto the floor
                // below needs -- and nothing at all for one that falls further
                // than that. Past the edge of its own window a body has no
                // collider anywhere near it, so it accelerates through every
                // floor, wall and roof in its path until the backstop above
                // catches it. In the wood that is invisible: the terrain clamp
                // is only a metre or two down. In a building it is the whole
                // bug -- there is real geometry it should have landed on.
                //
                // The felled branch above has had this since trees were felled
                // and the argument is identical: a window the body has left is
                // not a window. The difference is only how the test is put --
                // a tree is long and is tracked by its bounds, a chip is small
                // and its centre is enough.
                //
                // BUDGETED, because buildWindow samples 32,768 cells and sixty
                // pieces coming off a wall at once would spend all of it in one
                // frame. Two a frame is plenty: a body only leaves its window
                // after falling a metre, which at terminal speed is several
                // frames apart, and a piece that waits one frame for its
                // collider falls 5 mm further than it should have.
                else if (winBudget > 0) {
                    const float dx = d.pos.x - d.winCentre.x;
                    const float dy = d.pos.y - d.winCentre.y;
                    const float dz = d.pos.z - d.winCentre.z;
                    if (dx * dx + dy * dy + dz * dz > kWinRecentreM * kWinRecentreM) {
                        const int nw = buildWindow(ph, d.pos);
                        // ONLY IF THE NEW ONE IS REAL. buildWindow returns -1
                        // where there is nothing solid nearby, and swapping a
                        // good window for that would take the floor out from
                        // under a piece that was resting on it.
                        if (nw >= 0) {
                            if (d.window >= 0) ph.removeStatic(d.window);
                            d.window = nw;
                            d.winCentre = d.pos;
                            --winBudget;
                        } else {
                            // Nothing near it: re-centre the bookkeeping anyway
                            // so this is not re-asked every frame for a piece
                            // falling through open air.
                            d.winCentre = d.pos;
                        }
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
                // A LONG BODY NEEDS THIS AS MUCH AS A TREE DOES, and inside
                // the level it has to ask the LEVEL where the floor is: this
                // used to read terrainAt unconditionally, which in nuketown is
                // the wood 640 m below and is never reached, so the backstop
                // could not fire at all.
                if ((terrainAt || level_) && (d.felled || d.longBody)) {
                    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
                    if (ph.boundsOf(d.phys, &lo, &hi)) {
                        // The ground under the body's own footprint, at its
                        // corners and middle -- a 26 m trunk lies across a lot
                        // of hillside and the lowest of it is what matters.
                        // -- AND IT IS THE LOWEST, WHICH IS WHAT THE LINE
                        //    ABOVE ALREADY SAID -------------------------------
                        //
                        // (user 2026-09-17: "I cut down a tree and it fell right
                        //  through the terrain ... and then it teleported to the
                        //  top of another tree.")
                        //
                        // THE COMMENT SAID LOWEST AND THE CODE TOOK THE HIGHEST.
                        // Both halves of that are wrong and they compound:
                        //
                        //   * the TRIGGER fires when the body is under the
                        //     highest ground its bounding box touches, which on
                        //     any slope is true long before the body is
                        //     actually under the world;
                        //   * the LIFT then puts it at that highest ground -- so
                        //     a trunk lying at the foot of a rise is thrown up
                        //     to the top of the rise, in one frame, which is
                        //     the teleport in the report.
                        //
                        // A 14 m bounding box on a hillside spans several metres
                        // of relief, so the two readings are not close. Lowest
                        // makes the trigger mean what it says (the WHOLE body is
                        // under the world) and the lift conservative.
                        const auto ground = [&](float x, float z) {
                            return level_ ? levelFloorBelowM(x, hi.y, z)
                                          : (terrainAt ? terrainAt(x, z) : -1.0e9f);
                        };
                        float g = ground(lo.x, lo.z);
                        g = minf(g, ground(hi.x, lo.z));
                        g = minf(g, ground(lo.x, hi.z));
                        g = minf(g, ground(hi.x, hi.z));
                        g = minf(g, ground((lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f));
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
            //
            // ...UNLESS IT IS A MUSHROOM, WHICH IS THE ONE THING HERE THAT IS
            // SMALL AND STILL SCENERY. This is the whole of "when cutting the
            // mushroom from the static terrain it just flies": a severed cap is
            // a couple of hundred voxels, comfortably under kAbsorbSize, so it
            // fell correctly for half a second and was then made KINEMATIC and
            // flown into the player's hands on a curve. Nothing was wrong with
            // the fall -- what looked like broken physics was the collect.
            //
            // A felled tree is excluded on the same grounds and by the flag
            // beside this one; see kDebrisSoft for why a mushroom needs its own
            // rather than borrowing `felled`, which also means KIND_TREE
            // shading and a mesh whose shades are NOT resolved per voxel.
            // ...AND IT HAS TO BE WITHIN REACH, IF THIS ONE WAS GIVEN A REACH.
            //
            // (user 2026-09-15: "dont have the chipped chunk caused by the
            // arrow get absorbed by the player. only when the player is close
            // enough to absorbe the chunk".)
            //
            // THERE WAS NO DISTANCE TEST HERE AT ALL, and until the arrow
            // there was no need of one: every chip in the game was made by a
            // tool, and swinging a tool puts you beside what you hit. A shaft
            // is the first thing that can knock a piece off something across
            // the clearing, and the piece flew the whole way back.
            //
            // HORIZONTAL, so standing on a ledge above a chip does not put it
            // out of reach -- the player's eye is 1.6 m over their feet and the
            // reach is 1.6 m, which would make a chip at your toes a borderline
            // case measured in 3D. See kArrowAbsorbM.
            const float adx = d.pos.x - eye.x, adz = d.pos.z - eye.z;
            const bool inReach = d.absorbR >= kAbsorbAnywhere ||
                                 (d.absorbR >= 0.0f &&
                                  adx * adx + adz * adz <= d.absorbR * d.absorbR);
            if (!d.absorbing && inReach && !d.felled && !d.scenery && d.voxels <= kAbsorbSize &&
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
            }
            // ---- ...OR IT RUNS OUT OF TIME -------------------------------
            //
            // A MUSHROOM LASTS AS LONG AS A FELLED TREE. A chip's thirty
            // seconds is the lifetime of something you were going to pick up
            // anyway; a cap is never collected now, so at that figure it would
            // simply blink out of the wood while you stood next to it -- the one
            // complaint this engine has heard more than once.
            //
            // WHAT IT COSTS: one of kDebrisInstances' sixty-four slots for half
            // an hour, and there is no eviction -- a full band means spawnPiece
            // returns -1 and the next thing cut loose does not fall at all. That
            // is the same bargain a felled tree already makes, and a player cuts
            // mushrooms in ones rather than dozens; it is recorded here because
            // the failure would look like NOTHING FLOATS breaking rather than
            // like a full slot table.
            // ---- ...OR IT IS A CORPSE AND THE RED HAS RUN OUT ------------
            //
            // (user 2026-09-15: "when the life is killed, it should stay red
            // while it breaks into peices, then it should dissapear completely
            // as it drops the raw steak as it does currently".)
            //
            // THE PIECES GO WITH THE FLASH, which is v1's own sequence and its
            // own words: "the sparks are the death, so nothing is left behind
            // to collect ... they have been tumbling since the hit, red for as
            // long as HURT ran". Without this the corpse lay in the wood for
            // the full scenery lifetime -- half an hour of grey lumps where an
            // animal used to be, long after the red that explained them.
            //
            // hurtT0 IS THE WHOLE TEST and only a shattered animal has one --
            // see corpseFade, where the same field draws the red. An ordinary
            // chip of stone keeps its sentinel and its thirty seconds.
            // ...AND THEN IT LIES THERE FOR HALF A SECOND -----------------
            //
            // (user 2026-09-15: "have it turn red, split into peices, then rest
            // on the ground for half a second where it then dissapears".)
            //
            // THREE BEATS, NOT TWO. The first cut retired the pieces when the
            // RED ran out, which is 500 ms after the blow -- and 500 ms after
            // the blow they are still in the air, so they blinked out mid-fall
            // and read as an animal going through the floor. The red is the
            // first beat; landing is the second; the half second asked for here
            // is the third and it starts when the tumbling stops.
            //
            // REST IS MEASURED, not timed: a piece thrown off a bank falls
            // further than one dropped on the flat, and waiting a fixed span
            // would catch the first mid-air and leave the second lying. The
            // solver already knows -- see Physics::velocityOf.
            else if (d.hurtT0 > -1e8) {
                // -- A CORPSE IS PLACED ON THE GROUND, NOT DROPPED ON IT -----
                //
                // (user 2026-09-15: "it clips straight through the ground ...
                // then rest on the ground for half a second".)
                //
                // THE SOLVER CANNOT HOLD THESE UP AND THE TRACE SAYS SO. An
                // octant of an animal is frequently a ONE-VOXEL SLAB, and a
                // convex hull that thin cooks to nothing -- so the piece has no
                // collider at all. It does not land: it falls through the
                // world until the backstop in this same function catches it
                // half a metre down, puts it back, and it falls again.
                // Measured, one piece of a rabbit, every frame:
                //
                //     f0   y 37.34  under -0.04  speed 0.33
                //     f10  y 36.98  under +0.32  speed 3.66
                //     f40  y 37.39  under -0.09  speed 0.33
                //     f50  y 37.03  under +0.27  speed 3.66
                //
                // A 0.4 m sawtooth that never decays, a third of a metre of it
                // inside the hillside. That is the clipping, and no amount of
                // waiting for it to "settle" would ever have ended.
                //
                // ...AND IT STAYS A RIGID BODY (user 2026-09-15: "turn the
                // red pecies into rigid bodies. it should obey physics").
                //
                // A previous cut PARKED them: placed on the ground and made
                // kinematic the moment they reached it. That did stop the
                // clipping, and it stopped them being physics -- they landed
                // dead flat wherever they happened to arrive, which is not what
                // a corpse coming apart looks like. The clipping had one cause
                // and it was the BIRTH, not the simulation: see the lift in
                // shatterFlyer. With the pieces born above the ground the
                // solver holds them up by itself and there is nothing here to
                // correct.
                //
                // REST IS MEASURED rather than timed, because a piece thrown
                // off a bank falls further than one dropped on the flat.
                Vec3 lin{0, 0, 0}, ang{0, 0, 0};
                // A BODY THE SOLVER WILL NOT ANSWER FOR IS NOT MOVING -- PhysX
                // puts a settled body to sleep, and asleep is exactly the state
                // this is waiting for.
                const bool got = ph.velocityOf(d.phys, &lin, &ang);
                const bool still =
                    !got || lin.x * lin.x + lin.y * lin.y + lin.z * lin.z < 0.25f;
                if (still && d.restT0 < -1e8) d.restT0 = nowMs;
                if (!still) d.restT0 = -1e9;
                // ...AND THE CAP IS WHAT COVERS A PIECE THROWN CLEAR. One that
                // lands on a ledge the terrain query does not describe still
                // goes, half a second late.
                const bool rested = d.restT0 > -1e8 && nowMs - d.restT0 > 500.0;
                if (rested || nowMs - d.hurtT0 > 2500.0) {
                    retireDebris(ph, i);
                    continue;
                }
            }
            else if (nowMs - d.bornMs >
                     (d.lifeMs > 0.0 ? d.lifeMs
                                     : ((d.felled || d.scenery) ? kFelledLifeMs
                                                                : kDebrisLifeMs))) {
                retireDebris(ph, i);
                continue;
            }
            // ---- ...OR IT IS OUTSIDE THE RING -----------------------------
            //
            // (user 2026-09-17: "a tree that was felled over never despawns in
            //  the renderer. fix this. have it render within render distance
            //  like everything else.")
            //
            // A FELLED TREE LASTS HALF AN HOUR (kFelledLifeMs) and that was the
            // whole of its lifetime rule -- so a trunk you cut and walked away
            // from stayed in the TLAS, and stayed a PhysX body with a static
            // collision window around it, for the rest of the session. Every
            // other piece of the world is bounded by the chunk ring: walk far
            // enough and the chunk unloads. A debris body was the one thing
            // with no such bound, which is exactly what "never despawns" is.
            //
            // THE SAME DISC THE CHUNKS USE, so "render distance" means one
            // thing here -- see the ring in update(), which is viewChunks of
            // CHUNK_VOX voxels and is a disc rather than a square.
            //
            // HORIZONTAL, because the ring is: a body in a valley below you is
            // not further away in the sense that matters.
            //
            // RETIRED RATHER THAN HIDDEN. The slot table is sixty-four for the
            // whole world and a hidden body still holds one, still holds its
            // triangles and still costs the solver a step. Coming back to where
            // a tree fell and finding it gone is what unloading a chunk already
            // does to everything else standing there.
            {
                const float rdx = d.pos.x - eye.x, rdz = d.pos.z - eye.z;
                const float ring = float(maxi(1, viewChunks)) * float(CHUNK_VOX) * VOXEL_M;
                if (rdx * rdx + rdz * rdz > ring * ring) {
                    retireDebris(ph, i);
                    continue;
                }
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
            //
            // NOT ON A MUSHROOM EITHER. The shiver is there because a chip you
            // are about to collect reads as frozen in the half second before it
            // lifts; nothing is about to collect a cap, so the same motion is
            // just scenery that will not settle.
            float wq[4] = {d.quat[0], d.quat[1], d.quat[2], d.quat[3]};
            if (!d.absorbing && !d.felled && !d.scenery) {
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
        // PER CALL, or the `whole` path below inherits the last tree's hinge
        // and splits this one at a row that means nothing here.
        hingeCut_ = -1;
        if (whole) {
            // NOTHING IS CONNECTED TO THE GROUND, because the ground is gone --
            // see dropUndermined. The fill's answer is about the model's own
            // bottom row, which is exactly the assumption that has stopped
            // being true, so it is thrown away and all of it falls.
            sevSeen_.assign(sevSeen_.size(), 0);
        } else {
            const int cut = sevLooseCells_ - t.bornLoose;
            // -- ...OR THE HINGE IS TOO THIN TO HOLD IT UP ------------------
            //
            // See kHingeFrac. The connectivity test above is the honest answer
            // to "has anything come away", and a trunk hanging by one voxel has
            // not -- which is why it needed a second question rather than a
            // looser version of the first.
            if (t.bornHinge < 0) {
                int r0 = 0, c0 = 0;
                long a0 = 0;
                hingeRow(t.volume, t.sx, t.sy, t.sz, &r0, &c0, &a0);
                t.bornHinge = maxi(1, c0);
            }
            int hr = -1, hc = 0;
            long habove = 0;
            hingeRow(vol, t.sx, t.sy, t.sz, &hr, &hc, &habove);
            const bool hinged = hr >= 0 && habove >= kHingeAboveVox &&
                                hc <= maxi(kHingeFloorVox,
                                           int(float(t.bornHinge) * kHingeFrac));
            if (!hinged && cut < kMinLooseCells && sevLooseFrac_ < kFellFraction) return false;
            // THE SPLIT IS BY ROW WHEN IT IS THE HINGE THAT GAVE, not by
            // connectivity -- the point is that the thread is still joined, so
            // asking the flood where to cut would put the whole tree in the
            // stump. Everything above the hinge goes over; the hinge row and
            // below stay as the stump, which is what is left standing after a
            // real tree comes down.
            hingeCut_ = hinged ? hr : -1;
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
                    // THE HINGE'S OWN CUT LINE, when that is what fired -- see
                    // hingeCut_. Otherwise the flood's.
                    const bool stays = (hingeCut_ >= 0) ? (y <= hingeCut_)
                                                        : (sevSeen_[cellIx(x, y, z)] != 0);
                    if (stays)
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
            const int need = maxi(1, int(float(q * q * q) * kFellFillFrac));
            // A ROCK HAS NO CANOPY, so the second bar is the first one for
            // anything that is not a tree and the loop below is what it was.
            const int needAny =
                isTree ? maxi(1, int(float(q * q * q) * kFellCrownFrac)) : need;
            greedyBoxes(
                cx, cy, cz, Vec3{0.0f, 0.0f, 0.0f}, cell,
                [&](int i, int j, int k) {
                    // ---------------------------------------------------
                    // A TREE'S COLLIDER IS ITS WOOD.
                    //
                    // This used to be a vertical PRISM over the stump: keep
                    // every cell within the base footprint's half extents of
                    // where the model meets the ground, drop the rest as
                    // canopy. On a pine that is the trunk, because a pine's
                    // trunk is straight and central. ON A BIRCH IT IS A STUB.
                    //
                    // Measured with --fell-test in the birch band: the felled
                    // body's own shape bounds came back
                    //
                    //     lo.y 15.19   hi.y 16.00
                    //
                    // -- an EIGHTY CENTIMETRE collider under an eleven metre
                    // tree. A birch leans, so its trunk leaves the prism a
                    // metre up and everything above that was classed canopy
                    // and thrown away. The body then had no length to topple
                    // with: it dropped 0.37 m, came to rest at pitch 0.0 and
                    // sat there, while the MESH -- the whole tree -- hung off
                    // it wherever that stub happened to stop. That is the tree
                    // standing in the air, and it is why a felled birch never
                    // looked like it fell.
                    //
                    // The honest question was never "is this cell over the
                    // stump" -- it was "is this cell WOOD", and the palette
                    // has known the answer since the model was loaded, because
                    // it classified every colour as foliage or bark to decide
                    // what was translucent. So leaves simply do not count
                    // towards the fill, and the collider follows the trunk
                    // wherever the trunk actually goes, lean and all.
                    //
                    // THE CANOPY IS STILL NOT IN THE SOLVER, which is the part
                    // that must not regress: a crown collider is a fat cone
                    // that cannot lie down, and the recorded result of trying
                    // was a tree gaining spin until it tumbled out of the world
                    // at 28 m/s. Leaves are foliage by colour, so they are
                    // excluded here as completely as the prism excluded them --
                    // and the thin outer branches fail kFellFillFrac on their
                    // own, being mostly air at this cell size.
                    //
                    // A BOULDER IS UNCHANGED. It has no foliage in it, so the
                    // test is vacuous for a rock and the loop is what it was.
                    // ---------------------------------------------------
                    // TWO COUNTS, TWO BARS. Wood at kFellFillFrac is the
                    // trunk and the thick branches, exactly as before; anything
                    // solid at kFellCrownFrac is the dense inner canopy, which
                    // is what a felled conifer actually comes to rest on. A
                    // cell passes on either.
                    int wood = 0, any = 0;
                    for (int b2 = 0; b2 < q; ++b2)
                        for (int a2 = 0; a2 < q; ++a2)
                            for (int e2 = 0; e2 < q; ++e2) {
                                const int x = i * q + e2, y = j * q + b2, z = k * q + a2;
                                if (x >= t.sx || y >= t.sy || z >= t.sz) continue;
                                const uint8_t mv =
                                    fallVol_[size_t(x) + size_t(z) * size_t(t.sx) +
                                             size_t(y) * size_t(t.sx) * size_t(t.sz)];
                                if (mv == mat::AIR) continue;
                                ++any;
                                if (!(isTree && palette.isFoliage(mv))) ++wood;
                                if (wood >= need || any >= needAny) return true;
                            }
                    return false;
                },
                &winBoxes_, kFellMaxBoxes * 4);
            if (winBoxes_.size() <= kFellMaxBoxes) break;
            cell *= 1.5f;
        }
        if (winBoxes_.empty()) return false;
        std::printf("  fell     collider %zu boxes at %.2f m cells%s\n", winBoxes_.size(),
                    double(cell), isTree ? "  (trunk + dense crown)" : "");

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
        // -------------------------------------------------------------------
        // ARMED, NOT DRIVING -- and this is v1's own correction, word for word:
        //
        //     "the drive PRESCRIBES rotation, which the contact solver cannot
        //      argue with, so starting it at the cut let the trunk rotate
        //      straight through its own stump. Drop cleanly onto the cut face
        //      first; phStep starts the topple once it has actually landed."
        //
        // v2 spun the body AT THE CUT, so a trunk severed halfway up was born
        // leaning on its own stump with a quarter radian a second of turn in
        // it, wedged, and it crept over across five seconds while barely
        // descending. Reported as "a felled tree when cut at the trunk just
        // stays stationary while it falls ... instead of floating while it
        // falls".
        //
        // So the piece is STOPPED here -- no spin, no sideways drift -- and
        // falls straight down onto the cut face under gravity alone. The hinge
        // is armed and applied by updateDebris on the frame it lands.
        // -------------------------------------------------------------------
        ph.stopBody(phys);

        Debris &d = debris_[slot];
        d = Debris{};
        d.live = true;
        d.felled = true;
        // THE HINGE, HELD UNTIL IT LANDS -- see the note at the stop above. A
        // rock gets none: it was never balanced, it was resting on stone that
        // is no longer there, so gravity is the whole story.
        {
            const float dl = sqrtf(swingDir.x * swingDir.x + swingDir.z * swingDir.z);
            if (isTree && dl > 1e-4f) {
                d.tipAxis = Vec3{-swingDir.z / dl, 0.0f, swingDir.x / dl};
                d.tipArmed = true;
            }
        }
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
        // ...AND IT KEEPS THE WOOD ITSELF, which is what an axe meets from here
        // on. A COPY: fallVol_ is scratch and the next tree to come down
        // overwrites it. The body's origin is the model's corner, so these are
        // in the frame the mesh is in with no offset -- see originOff below.
        d.vox = fallVol_;
        d.vsx = t.sx;
        d.vsy = t.sy;
        d.vsz = t.sz;
        // A MUSHROOM IS THE THIRD ANSWER HERE. modelKind 3 is the mushroom
        // set (makeInstance, which sets `bouncy` off the same number), and a
        // cap big enough to reach this path rather than dropModelHangers' must
        // be cuttable by the same two tools the standing one was.
        d.takesAs = isTree                  ? uint8_t(kDebrisWood)
                    : (so.modelKind == 3)   ? uint8_t(kDebrisSoft)
                                            : uint8_t(kDebrisStone);
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
    // -----------------------------------------------------------------------
    // THE NARROWEST ROW THROUGH THE LOWER HALF, AND WHAT IS STANDING ON IT.
    //
    // See kHingeFrac. Rows only, not cells: a hinge is a horizontal section and
    // the question is how much wood crosses it, which is exactly a row count.
    //
    // THE LOWER HALF ONLY. A crown is full of gaps and some row up in the
    // foliage is always nearly empty; felling on one of those would drop a
    // tree because its branches are sparse. A cut is made at chest height, so
    // that is where this looks.
    // -----------------------------------------------------------------------
    static void hingeRow(const std::vector<uint8_t> &vol, int sx, int sy, int sz, int *row,
                         int *count, long *above) {
        int bestRow = -1, bestN = INT_MAX;
        const int top = maxi(2, sy / 2);
        std::vector<int> perRow(size_t(sy), 0);
        for (int y = 0; y < sy; ++y) {
            int n = 0;
            for (int z = 0; z < sz; ++z)
                for (int x = 0; x < sx; ++x)
                    if (vol[size_t(x) + size_t(z) * size_t(sx) +
                            size_t(y) * size_t(sx) * size_t(sz)] != mat::AIR)
                        ++n;
            perRow[size_t(y)] = n;
            // ROW ZERO IS THE FOOT and is never the hinge -- a tree standing on
            // the ground has its narrowest section there by construction on
            // anything that tapers.
            if (y >= 1 && y < top && n < bestN) {
                bestN = n;
                bestRow = y;
            }
        }
        if (bestRow < 0) {
            if (row) *row = -1;
            if (count) *count = 0;
            if (above) *above = 0;
            return;
        }
        long up = 0;
        for (int y = bestRow + 1; y < sy; ++y) up += perRow[size_t(y)];
        if (row) *row = bestRow;
        if (count) *count = bestN;
        if (above) *above = up;
    }

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
        // ...AND THE SCATTER A TILL PUT AWAY. Same trick, same reason: decor
        // is regenerated from the scatter on every adopt, so a flower over a
        // seed bed stands back up unless it is put down again here. See
        // hiddenScatter_ for how this was reported three times.
        for (auto it = hiddenScatter_.lower_bound({key, INT_MIN});
             it != hiddenScatter_.end() && it->first == key; ++it) {
            const int slot = it->second;
            if (slot >= 0 && size_t(slot) < c.decorDesc.size())
                c.decorDesc[size_t(slot)].instanceMask = 0;
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
    // WHAT THE LAST BITE CUT LOOSE FROM THE STATIC GROUND, if anything.
    //
    // Returned rather than spawned here for the reason the spoil is: this
    // class has no Physics, and threading one in for a body it does not own
    // would put the simulation inside the world's geometry. The caller has
    // both and does the spawning -- see App, beside the spoil's own spawn.
    //
    // TAKE, not peek: the flag clears, so one freed piece becomes one body
    // however many times this is asked.
    bool takeHangers(const std::vector<uint8_t> **vol, int *n, Vec3 *at, float *yaw) {
        if (!hangHave_) return false;
        hangHave_ = false;
        *vol = &hangVol_;
        *n = hangN_;
        *at = hangAt_;
        *yaw = hangYaw_;
        return true;
    }

    // -----------------------------------------------------------------------
    // TURN THE EARTH OVER -- v1'S HOE, WHOLE.
    //
    // (user 2026-09-14: "import the hoe from v1. import all of the hoes
    // mechanics. let it till dirt.")
    //
    // WHAT v1 DOES, and every line of it is here: a disc of columns round the
    // aim point; only SOIL turns, and SAND is explicitly not soil ("it is in
    // digOnlyTab because the SHOVEL moves it, which is a different question
    // from whether a hoe can make a seed bed out of a beach"); the surface
    // voxel comes away and the one under it becomes tilled earth, so the ground
    // drops by one and what you are left standing on is turned soil; the
    // strands on top go with it; a column already turned is not turned again
    // ("the hoe cannot dig itself deeper"); and after TILL_MS it all grows back
    // because nothing was ever planted in it.
    //
    // WHAT v2 GETS FOR FREE, and it is most of the fiddly half:
    //
    //   the strands   StrandColumns grows nothing on an edited column, so the
    //                 grass over a tilled square disappears without this
    //                 function touching a blade. v1 lifts them by hand into
    //                 `str` and lays them back down on the revert; here the
    //                 edit going away IS the grass coming back.
    //   the palette   mat::TILLED is a fixed id (see its note), so there is no
    //                 runtime mint and no full-table fallback that quietly
    //                 repaints iron ore.
    //   the height    the edit layer already overlays the height field for
    //                 every reader -- the walk, the swing, the probe, the
    //                 mesher -- so lowering a column is two voxel writes and
    //                 nothing has to be told.
    //
    // Returns the number of columns turned, 0 if none -- which is a swing at
    // rock, at sand, or at ground somebody has already been over.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHAT WAS STANDING ON THE GROUND THAT JUST CAME AWAY.
    //
    // (user 2026-09-14: "have flowers dissapear when the hoe destroys the grass
    // underneath it.")
    //
    // THE GRASS ALREADY WENT AND THE FLOWERS DID NOT, and the difference is
    // what each one IS. A blade is generated per column, so an edit on that
    // column stops it being generated -- StrandColumns' own rule, and the till
    // gets it free. A flower is a MODEL INSTANCE placed by the scatter, with a
    // transform of its own; nothing about editing the dirt under it reaches it,
    // so it went on standing in the air over turned soil.
    //
    // v1 does this by hand and says so: "the strands on top go with it -- grass
    // tufts, flowers, twigs and cones stand ON the surface voxel that just came
    // away and carry no support of their own."
    //
    // NOT fellSlots_, WHICH IS FOREVER. That set is for a tree you cut down and
    // it survives eviction on purpose. A till grows back in forty-five seconds
    // and the flowers have to come back with it, so the hiding is recorded on
    // the Till itself and undone by the revert.
    //
    // MUSHROOMS AND PINECONES TOO. Everything the scatter stands on the floor
    // is in the same position; a cap left hanging over a seed bed is the same
    // picture as a flower. Trees and rocks are NOT -- kinds 0 and 1 are things
    // with a footprint that a hoe has no business moving.
    // -----------------------------------------------------------------------
    // TAKE THE FRUIT YOU ARE LOOKING AT.
    //
    // (user 2026-09-17: "import the apple/oranges pick up mechanic. so they
    //  need to be a handheld now.")
    //
    // A RAY, WALKED, NOT A COLLIDER. A fruit is `walkThrough` on purpose -- it
    // hangs several metres up in a crown and a 40 cm collider there is a thing
    // to snag on -- so it is on no Solid list and `swingRay` cannot see it.
    // Giving 1,848 of them colliders to make one reachable would be paying for
    // the feature everywhere to use it in one place.
    //
    // So this samples along the aim and asks the decor directly. Cheap for the
    // same reason hideScatterOn is: decorAt is a flat array per chunk with the
    // kind in it, and only the handful of chunks the reach crosses are looked
    // at.
    //
    // NEAREST ALONG THE RAY WINS, not nearest to the eye -- two apples on one
    // branch should be taken front to back, which is what stepping outward and
    // stopping at the first hit gives for free.
    //
    // REMEMBERED OFF THE CHUNK. `adoptMany` rebuilds every decor instance from
    // the scatter with its mask back on, so a fruit that is only masked here
    // grows back on the next re-mesh -- which is the bug hideScatterOn's own
    // note says was reported three times. hiddenScatter_ is replayed by
    // reapplyDamage and is what makes a picked apple stay picked.
    //
    // Returns which fruit it was (the index into the fruit templates: 0 apple,
    // 1 orange) or -1 for nothing in reach.
    // -----------------------------------------------------------------------
    // THE LOWEST-HANGING FRUIT WITHIN A HORIZONTAL RADIUS, which is exactly
    // the phrase's literal meaning: the one a player could actually walk up to
    // and take. For --food-test, which otherwise picks an arbitrary fruit and
    // fails on the ones eight metres up a giant oak -- a true report about the
    // wrong fruit.
    bool lowestFruitNear(const Vec3 &p, float maxR, Vec3 *atOut) const {
        const float r2 = maxR * maxR;
        bool any = false;
        Vec3 best{0.0f, 0.0f, 0.0f};
        const float span = maxR + 8.0f;
        const int x0 = floorDiv(int(std::floor((p.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(std::floor((p.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(std::floor((p.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(std::floor((p.z + span) / VOXEL_M)), CHUNK_VOX);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                auto it = chunks_.find(chunkKey(cx, cz));
                if (it == chunks_.end()) continue;
                const Chunk &c = it->second;
                for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
                    const DecorAt &q = c.decorAt[i];
                    if (q.kind != 6 || !c.decorDesc[i].instanceMask) continue;
                    const float dx = q.midX() - p.x, dz = q.midZ() - p.z;
                    if (dx * dx + dz * dz > r2) continue;
                    if (any && q.y >= best.y) continue;
                    best = Vec3{q.midX(), q.y, q.midZ()};
                    any = true;
                }
            }
        if (any && atOut) *atOut = best;
        return any;
    }

    int takeFruitAlong(const Vec3 &eye, const Vec3 &dir, float reachM, Vec3 *atOut = nullptr,
                       float *missedByM = nullptr) {
        if (missedByM) *missedByM = -1.0f;
        if (fruit_.empty()) return -1;
        // -- NEAREST TO THE AIM LINE, NOT FIRST INSIDE A TUBE --------------
        //
        // (user 2026-09-17, twice: "picking fruit still is not working.")
        //
        // THE FIRST VERSION MARCHED THE RAY in 10 cm steps and took the first
        // fruit within 35 cm of a sample point. That is a 35 cm tube out to
        // eight metres, which is about two and a half degrees of aim -- and it
        // is asked at the impact frame of a swing, by which time the view has
        // moved with the animation. It also cost eighty passes over the decor
        // of four chunks to answer.
        //
        // ONE PASS, AND THE RIGHT QUESTION. Every fruit in range is projected
        // onto the aim line: how far ALONG it (must be in front of the eye and
        // inside the reach) and how far OFF it (the perpendicular miss). The
        // smallest miss wins. That is what "the one I am looking at" means,
        // and it cannot be defeated by a fruit that happens to sit between two
        // sample points.
        //
        // kFruitAimM IS GENEROUS ON PURPOSE. A fruit is 40 cm and hangs among
        // leaves that hide its edges; asking for the centre to within a fruit's
        // own width is asking for marksmanship, not for picking. It reports the
        // miss it rejected, so a caller can say whether the aim was close.
        const float reach = maxf(0.5f, reachM);
        float bestMiss = 1e9f;
        long long bestKey = -1;
        int bestIdx = -1, bestKind = -1;
        Vec3 bestAt{0.0f, 0.0f, 0.0f};
        float nearestMiss = 1e9f;
        const float span = reach + 4.0f;
        const int x0 = floorDiv(int(std::floor((eye.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(std::floor((eye.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(std::floor((eye.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(std::floor((eye.z + span) / VOXEL_M)), CHUNK_VOX);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                auto it = chunks_.find(chunkKey(cx, cz));
                if (it == chunks_.end()) continue;
                Chunk &c = it->second;
                for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
                    const DecorAt &q = c.decorAt[i];
                    if (q.kind != 6) continue;
                    if (!c.decorDesc[i].instanceMask) continue;   // already taken
                    const ModelTemplate &mt = templateFor(6, int(q.index));
                    const Vec3 m{q.midX(), q.y + 0.5f * float(mt.sy) * VOXEL_M, q.midZ()};
                    const Vec3 d{m.x - eye.x, m.y - eye.y, m.z - eye.z};
                    const float along = d.x * dir.x + d.y * dir.y + d.z * dir.z;
                    if (along < 0.0f || along > reach) continue;   // behind, or out of reach
                    const float off2 = maxf(0.0f, (d.x * d.x + d.y * d.y + d.z * d.z) -
                                                      along * along);
                    const float off = sqrtf(off2);
                    if (off < nearestMiss) nearestMiss = off;
                    if (off > kFruitAimM || off >= bestMiss) continue;
                    bestMiss = off;
                    bestKey = chunkKey(cx, cz);
                    bestIdx = int(i);
                    bestKind = int(q.index);
                    bestAt = m;
                }
            }
        if (missedByM && nearestMiss < 1e8f) *missedByM = nearestMiss;
        if (bestIdx < 0) return -1;

        auto it = chunks_.find(bestKey);
        if (it == chunks_.end()) return -1;
        it->second.decorDesc[size_t(bestIdx)].instanceMask = 0;
        // REMEMBERED OFF THE CHUNK, or adoptMany puts it straight back on the
        // next re-mesh -- see hideScatterOn, where that was reported three
        // times against the flowers.
        hiddenScatter_.insert({bestKey, bestIdx});
        rebuildTlas();   // the TLAS carries the mask
        if (atOut) *atOut = bestAt;
        return bestKind;
    }

    void hideScatterOn(const Vec3 &p, float radiusM, std::vector<std::pair<long long, int>> *out) {
        const float r2 = radiusM * radiusM;
        const float span = radiusM + 8.0f;
        const int x0 = floorDiv(int(std::floor((p.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(std::floor((p.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(std::floor((p.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(std::floor((p.z + span) / VOXEL_M)), CHUNK_VOX);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                auto it = chunks_.find(chunkKey(cx, cz));
                if (it == chunks_.end()) continue;
                Chunk &c = it->second;
                for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
                    const DecorAt &q = c.decorAt[i];
                    if (q.kind != 2 && q.kind != 3 && q.kind != 4) continue;   // flower, cap, cone
                    if (!c.decorDesc[i].instanceMask) continue;                // already down
                    // WHERE IT STANDS, not where its model's corner is.
                    const float dx = q.midX() - p.x, dz = q.midZ() - p.z;
                    if (dx * dx + dz * dz > r2) continue;
                    c.decorDesc[i].instanceMask = 0;
                    out->push_back({chunkKey(cx, cz), int(i)});
                    // ...AND REMEMBERED OFF THE CHUNK, or the re-mesh this
                    // till has already asked for puts it straight back.
                    hiddenScatter_.insert({chunkKey(cx, cz), int(i)});
                }
            }
        // THE TLAS CARRIES THE MASK, so changing one is a rebuild -- the same
        // line dropHangers' caller runs after it clears a hive.
        rebuildTlas();
    }

    // HOW MANY OF THE FLOOR SCATTER IN THIS DISC ARE STILL DRAWN.
    //
    // For --hoe-test, and it exists because "the flowers are still not
    // dissapering" was reported TWICE -- the first fix compared the till centre
    // against the model's CORNER and missed most of them, and nothing in the
    // engine could have said so. Counting what is still masked on is the only
    // statement about this that is not somebody looking at a screen.
    int scatterShownNear(const Vec3 &p, float radiusM) const {
        const float r2 = radiusM * radiusM;
        const float span = radiusM + 8.0f;
        const int x0 = floorDiv(int(std::floor((p.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(std::floor((p.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(std::floor((p.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(std::floor((p.z + span) / VOXEL_M)), CHUNK_VOX);
        int n = 0;
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                const auto it = chunks_.find(chunkKey(cx, cz));
                if (it == chunks_.end()) continue;
                const Chunk &c = it->second;
                for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
                    const DecorAt &q = c.decorAt[i];
                    if (q.kind != 2 && q.kind != 3 && q.kind != 4) continue;
                    if (!c.decorDesc[i].instanceMask) continue;
                    const float dx = q.midX() - p.x, dz = q.midZ() - p.z;
                    if (dx * dx + dz * dz <= r2) {
                        ++n;
                        if (undermineLog)
                            std::printf("  [shown] kind %d in chunk (%d,%d) at mid (%.2f, %.2f) "
                                        "y %.2f, %.2f m from the hole\n",
                                        int(q.kind), cx, cz, double(q.midX()), double(q.midZ()),
                                        double(q.y), double(std::sqrt(dx * dx + dz * dz)));
                    }
                }
            }
        return n;
    }

    // ...and put them back, which is the half a permanent set would not need.
    void showScatter(const std::vector<std::pair<long long, int>> &slots) {
        for (const auto &s : slots) {
            // OUT OF THE REPLAY SET FIRST, and unconditionally -- a chunk that
            // is not resident still has to come back with its flowers up.
            hiddenScatter_.erase(s);
            auto it = chunks_.find(s.first);
            if (it == chunks_.end()) continue;   // evicted; the scatter rebuilds it standing
            Chunk &c = it->second;
            if (s.second < 0 || size_t(s.second) >= c.decorDesc.size()) continue;
            // A FELLED SLOT STAYS FELLED. The two sets can name the same slot --
            // a flower under a tree that came down while the ground was turned
            // -- and the permanent one wins.
            if (fellSlots_.count({s.first, s.second})) continue;
            c.decorDesc[size_t(s.second)].instanceMask = kMaskWorld;
        }
        rebuildTlas();
    }

    size_t till(const Vec3 &p, float radiusM, double nowSec) {
        const int r = maxi(1, int(std::ceil(radiusM / VOXEL_M)));
        const int ci = int(std::floor(p.x / VOXEL_M));
        const int cj = int(std::floor(p.z / VOXEL_M));
        TerrainProbe probe(&terrain, &mesher_.edits);
        TerrainMemo memo;
        std::vector<std::pair<std::array<int, 3>, uint8_t>> cells;
        const int r2 = r * r;
        size_t columns = 0;
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dz * dz > r2) continue;
                const int i = ci + dx, j = cj + dz;
                // THE TOP AS IT IS NOW, edits included -- so a pit dug beside
                // the patch is tilled at its own floor rather than at the
                // height the generator still thinks this column has.
                int h = terrain.heightVox(i, j, memo);
                while (h > 1 && probe.material(i, j, h) == mat::AIR) --h;
                if (h < 2) continue;
                const uint8_t top = probe.material(i, j, h);
                // ONE LAYER ONLY. v1 keeps a `tillSet` of the voxels that ARE
                // tilled earth and tests THAT rather than the colour -- here
                // the material is the record, because the edit layer is the
                // only thing that can have put it there.
                // ...OR A SEED LYING ON ONE. Both materials are this
                // feature's own: a column wearing either has already been
                // turned, and turning it again would bury the seed and start
                // a second record for ground that is already in one.
                if (top == mat::TILLED || isSeed(top)) continue;
                if (!isSoilMat(top) || top == mat::SAND || isSand(top)) continue;
                // ...and there has to be something under it to turn into.
                const uint8_t below = probe.material(i, j, h - 1);
                if (below == mat::AIR) continue;
                cells.push_back({{i, j, h}, mat::AIR});
                cells.push_back({{i, j, h - 1}, mat::TILLED});
                tilled_.push_back({i, j, h, top, below, float(nowSec)});
                ++columns;
            }
        if (cells.empty()) return 0;
        remesh(mesher_.edits.writeCells(cells));
        // The bite, not the wood -- see markGroundDirty. One voxel of margin
        // because the patch's own cells straddle the edge of it.
        markGroundDirty(ci - r - 1, cj - r - 1, ci + r + 1, cj + r + 1);
        // ...AND THE SCATTER THAT WAS STANDING ON IT. Recorded against the
        // FIRST column of this bite, which is the one the revert will reach
        // first -- see tillRevert, which shows them again when it does.
        if (columns && !tilled_.empty())
            hideScatterOn(p, radiusM, &tilled_[tilled_.size() - columns].hidden);
        return columns;
    }

    // -----------------------------------------------------------------------
    // ...AND IT GROWS BACK OVER, since nothing was ever planted in it.
    //
    // v1's own reason, in its own words. Called every frame; the list is in
    // time order because nothing is ever inserted out of order, so this walks
    // from the front and stops at the first one that is not ready.
    //
    // A COLUMN SOMEBODY HAS SINCE DUG IS LEFT ALONE -- v1's "something was
    // built or dug here since". The test is that the two voxels still say what
    // the till left them saying; if they do not, the record is dropped without
    // touching anything, because putting the old dirt back would be undoing
    // somebody else's hole.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // SOMETHING HAS BEEN PLANTED IN THIS BED.
    //
    // Which is not a crop and not a voxel: it is the REVERT being cancelled.
    // Turned earth grows back over after kTillSec "since nothing was ever
    // planted in it", so planting is that clock stopping. See App::plantSeed.
    //
    //  FALSE STILL COUNTS AS A PLANTING and deliberately changes nothing:
    // a bed out of reach of water reverts on its own clock, which is what the
    // ask says it should. The return is how many columns were found either way,
    // so the caller can tell a planting from a click at the sky.
    //
    // MATCHED BY COLUMN, not by the click point: one till is eighty-one columns
    // and a seed goes in the BED, not in the voxel under the crosshair.
    // -----------------------------------------------------------------------
    size_t plantAt(const Vec3 &p, float radiusM, bool keep) {
        const int r = maxi(1, int(std::ceil(radiusM / VOXEL_M)));
        const int ci = int(std::floor(p.x / VOXEL_M));
        const int cj = int(std::floor(p.z / VOXEL_M));
        const int r2 = r * r;
        std::vector<std::pair<std::array<int, 3>, uint8_t>> cells;
        // FOUND FIRST, WRITTEN AFTER. The bed is refused whole or sown whole;
        // marking columns as the sweep meets them and then meeting a sown one
        // would leave half a bed planted and no seed on it.
        std::vector<Till *> bed;
        for (Till &t : tilled_) {
            const int dx = t.i - ci, dz = t.j - cj;
            if (dx * dx + dz * dz > r2) continue;
            // ALREADY SOWN IS NOT SOWN AGAIN. Without this a held stack drains
            // into one square metre of dirt a seed at a time, and the bed ends
            // up wearing as many seeds as you were carrying.
            if (t.seeded || t.planted) return 0;
            bed.push_back(&t);
        }
        const size_t n = bed.size();
        if (keep)
            for (Till *t : bed) t->planted = true;
        // -- THREE SEEDS, LYING ON THE BED --------------------------------
        //
        // (user 2026-09-14: "planting grass turns the tilled dirt to regular
        // dirt. this is wrong. I should see 3 seeds in the form of 3 voxels on
        // the tilled dirt when planting".)
        //
        // THE FIRST CUT WROTE THE SEED OVER THE TILLED VOXEL, which is why it
        // read as the bed un-tilling itself: a sown bed was a DIFFERENT BROWN
        // where a tilled one had been, so the turned earth you had just made
        // went away the moment you planted in it.
        //
        // SO A SEED GOES ON TOP, at t.y -- the voxel the till emptied. Lowering
        // a column is exactly "the old surface becomes air", so every tilled
        // column already has a free voxel over it and a seed needs no room made
        // for it.
        //
        // SPREAD RATHER THAN PILED: three at one spot is one seed drawn three
        // times. Taken at a quarter, a half and three quarters of the way
        // through the bed's own column list -- a disc walked in scan order --
        // so they land apart without a second idea of where the middle is.
        //
        // AND THE FIRST OF THEM CARRIES THE SEED THAT IS OWED BACK: a bed that
        // grows back over hands ONE seed to the player, however many voxels of
        // it you can see. See Till::seeded and the pop-out in tillRevert.
        // ...AND EACH ONE WEARS A DIFFERENT SHADE OF THE HELD MODEL, which
        // is what makes three voxels read as three seeds rather than as one
        // seed drawn three times. See mat::SEED_0.
        for (int k = 1; k <= 3 && !bed.empty(); ++k) {
            Till *t = bed[(bed.size() * size_t(k)) / 4u % bed.size()];
            cells.push_back({{t->i, t->j, t->y}, uint8_t(mat::SEED_0 + (k - 1))});
            if (k == 1) t->seeded = true;
        }
        if (!cells.empty()) {
            remesh(mesher_.edits.writeCells(cells));
            // A seed is one voxel and there are three of them, but they sit in
            // a bed this call already walked -- so the bed's own box is what
            // moved, and cells carries exactly it.
            int pi0 = cells.front().first[0], pj0 = cells.front().first[1];
            int pi1 = pi0, pj1 = pj0;
            for (const auto &cm : cells) {
                pi0 = mini(pi0, cm.first[0]);
                pj0 = mini(pj0, cm.first[1]);
                pi1 = maxi(pi1, cm.first[0]);
                pj1 = maxi(pj1, cm.first[1]);
            }
            markGroundDirty(pi0 - 1, pj0 - 1, pi1 + 1, pj1 + 1);
        }
        return n;
    }

    // `seedsBack` collects the spots where a seed has to pop out of the ground
    // -- see Till::seeded. The caller owns the drop pool, so it does the
    // spilling; this only says where.
    void tillRevert(double nowSec, std::vector<Vec3> *seedsBack = nullptr) {
        if (tilled_.empty()) return;
        TerrainProbe probe(&terrain, &mesher_.edits);
        std::vector<std::array<int, 3>> back;
        size_t n = 0;
        while (n < tilled_.size() && double(tilled_[n].at) + kTillSec <= nowSec) {
            const Till &q = tilled_[n];
            ++n;
            // A PLANTED BED DOES NOT GROW BACK. The record is dropped from the
            // list all the same -- it has nothing left to do, and leaving it
            // would walk it again on every frame for the rest of the session.
            if (q.planted) continue;
            if (!q.hidden.empty()) showScatter(q.hidden);
            // THE SEED COMES BACK OUT. Before the two material tests below,
            // because it is owed whatever state the ground is in -- a bed
            // somebody dug through still had a seed in it.
            if (q.seeded && seedsBack)
                seedsBack->push_back(Vec3((float(q.i) + 0.5f) * VOXEL_M,
                                          (float(q.y) + 0.5f) * VOXEL_M,
                                          (float(q.j) + 0.5f) * VOXEL_M));
            // AIR, OR THIS TILL'S OWN SEED LYING IN IT. The seed sits in the
            // voxel the till emptied -- see plantAt -- and refusing the column
            // because of it would leave a seed hovering over a crater, which is
            // worse than either outcome on its own. Anything ELSE up there is
            // somebody's build and the column is left alone.
            {
                const uint8_t above = probe.material(q.i, q.j, q.y);
                if (above != mat::AIR && !isSeed(above)) continue;
            }
            {
                const uint8_t below = probe.material(q.i, q.j, q.y - 1);
                if (below != mat::TILLED) continue;
            }
            // ERASED, NOT OVERWRITTEN. The generator's own answer is what was
            // there before -- putting `prevTop` back as an EDIT would leave the
            // column marked for ever, and a marked column grows no grass. See
            // EditStore::eraseCells.
            back.push_back({q.i, q.j, q.y});
            back.push_back({q.i, q.j, q.y - 1});
        }
        if (n) tilled_.erase(tilled_.begin(), tilled_.begin() + long(n));
        if (back.empty()) return;
        remesh(mesher_.edits.eraseCells(back));
        int ri0 = back.front()[0], rj0 = back.front()[1], ri1 = ri0, rj1 = rj0;
        for (const std::array<int, 3> &c : back) {
            ri0 = mini(ri0, c[0]);
            rj0 = mini(rj0, c[1]);
            ri1 = maxi(ri1, c[0]);
            rj1 = maxi(rj1, c[1]);
        }
        markGroundDirty(ri0 - 1, rj0 - 1, ri1 + 1, rj1 + 1);
    }

    size_t tilledCount() const { return tilled_.size(); }

    // -----------------------------------------------------------------------
    // CUT THE BLADES HERE, AND ONLY THE BLADES.
    //
    // (user 2026-09-14: "when the player left clicks the wheat, the wheat
    // breaks, and the seeds and wheat drop.")
    //
    // WHY THIS IS NOT dig(). A bite is a sphere of whatever it encloses, and a
    // tuft of wheat stands ON ground you are not trying to move -- swinging at
    // waist-high straw and leaving a crater in the soil is not breaking the
    // wheat, it is digging a hole that happens to remove some. So this walks
    // the COLUMNS in reach and takes each one's blade band, which is the span
    // between the soil and the tip. Nothing below the surface is touched and
    // no spoil is produced: a blade is not a chunk of anything.
    //
    // THE EDIT IS THE WHOLE MECHANISM, and it is one the mesher already had.
    // "A COLUMN ANYBODY HAS EDITED GROWS NOTHING" is StrandColumns' own rule in
    // voxel/columns.h -- a blade is placed from a hash on the column rather
    // than stored anywhere, so the way to remove one is to mark the column, not
    // to find the voxel and erase it. Writing AIR over the band does both: it
    // marks the column AND leaves the probe agreeing with the screen (see
    // TerrainProbe::bladeCut).
    //
    // THE RADIUS IS IN METRES AND THE CALLER SETS IT FROM THE PLANT, not from
    // the tool. A fixed bite is what made one patch pay out over and over:
    // 0.5 m is smaller than a tuft, so there was always more of the same plant
    // left to swing at. See App::breakWheat, which measures the tuft and hands
    // its whole radius over -- after which a second swing finds nothing,
    // because there IS nothing, which is a better rule than any tally.
    //
    // Returns how many blade voxels were taken, which is zero when you swing at
    // a column with nothing growing on it -- the caller uses that to decide
    // whether anything actually broke.
    // -----------------------------------------------------------------------
    size_t mow(const Vec3 &p, float radiusM) {
        // -- ONE VOXEL OF SLACK, AND IT IS NOT A FUDGE (user 2026-09-14:
        //    "sometimes if a strand of wheat is too big, it doesnt dissapsear
        //    in one hit ... one strand of wheat was left") ------------------
        //
        // THE TWO TESTS MEASURE FROM DIFFERENT POINTS. VoxelTerrain::tuftAt
        // decides a blade is tall by the distance from the tuft's SITE, which
        // is a float anywhere inside its cell; this walks VOXELS, from the
        // voxel the site falls in. Those differ by up to a voxel in each axis,
        // so a stalk right on the rim passes the first test and fails the
        // second -- and one stalk left standing in a cut patch is exactly what
        // that looks like.
        //
        // The slack is a whole voxel rather than a half because the offset is
        // in x AND z: sqrt(2) voxels at worst. Over-cutting by a voxel takes
        // nothing that was not going to be cut anyway -- outside the tuft the
        // blades are short grass, and the swing went through them.
        const int radiusVox = maxi(1, int(std::ceil(radiusM / VOXEL_M)) + 1);
        const int ci = int(std::floor(p.x / VOXEL_M));
        const int cj = int(std::floor(p.z / VOXEL_M));
        TerrainProbe probe(&terrain, &mesher_.edits);
        TerrainMemo memo;
        std::vector<std::array<int, 3>> cells;
        const int r2 = radiusVox * radiusVox;
        for (int dz = -radiusVox; dz <= radiusVox; ++dz)
            for (int dx = -radiusVox; dx <= radiusVox; ++dx) {
                if (dx * dx + dz * dz > r2) continue;
                const int i = ci + dx, j = cj + dz;
                const int h = terrain.heightVox(i, j, memo);
                const uint8_t top = terrain.topMaterial(i, j, h, memo);
                const int sr = terrain.strandRows(i, j, top);
                if (sr <= 0) continue;
                int lo = 0, hi = 0;
                VoxelTerrain::bladeSpan(h, sr, &lo, &hi);
                for (int y = lo; y <= hi; ++y) {
                    // ALREADY CUT is not cut again. The probe reads the edit
                    // layer, so a second swing at the same tuft finds air and
                    // this returns zero -- which is what stops one plant paying
                    // out for ever.
                    if (!isBlade(probe.material(i, j, y))) continue;
                    cells.push_back({i, j, y});
                }
            }
        if (cells.empty()) return 0;
        remesh(mesher_.edits.carveCells(cells));
        return cells.size();
    }

    size_t dig(const Vec3 &p, int radiusVox, std::vector<uint8_t> *spoil = nullptr,
               int *spoilN = nullptr, Vec3 *spoilAt = nullptr) {
        // ASK AGAIN ABOUT THIS PLACE, LATER AND WIDER. dropTerrainHangers below
        // answers for the 2.7 m around the bite in this frame; the watch
        // answers for 9.6 m of it a fraction of a second from now, which is the
        // only way a piece bigger than the box is ever seen. See the block over
        // kFloatRegionVox -- and note this is INSIDE dig rather than at its
        // call sites, for the reason carveLevelToBody was fused: a rule that
        // depends on every caller remembering a second line is a rule that has
        // already been deleted once.
        markFloatDirty(p);
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
        // The bite, not the wood -- see markGroundDirty. The dig is a sphere of
        // radiusVox about (ci, cj), so its footprint is that square.
        markGroundDirty(ci - radiusVox - 1, cj - radiusVox - 1, ci + radiusVox + 1,
                        cj + radiusVox + 1);   // the floor the solver stands on has moved
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
        // A NEW ANSWER EVERY BITE. Cleared here rather than after the spawn so
        // a blow that frees nothing cannot hand the caller the last one's.
        hangHave_ = false;
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

        // The cube's centre in world metres, for the body the caller spawns --
        // the same point dig reports for the spoil, because it is the same box.
        hangAt_ = Vec3{(float(ci) + 0.5f) * VOXEL_M, (float(cy) + 0.5f) * VOXEL_M,
                       (float(cj) + 0.5f) * VOXEL_M};
        hangN_ = n;
        hangYaw_ = 0.0f;   // terrain stands in the world's own frame

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
                    // ---------------------------------------------------
                    // IT FALLS. IT DOES NOT SIMPLY STOP EXISTING.
                    //
                    // This used to carve the voxel and nothing else, so ground
                    // a bite cut loose VANISHED -- unless it happened to land
                    // inside the spoil cube above, which is only the handful
                    // of voxels within the bite's own radius. Everything
                    // further out was deleted in place. A tree does not do
                    // this: a severed trunk becomes a real body, and that is
                    // the behaviour the terrain was missing.
                    //
                    // The material comes from the generator, sampled BEFORE
                    // the carve below, exactly as the spoil is -- after it
                    // they are all air.
                    //
                    // Same cube layout spawnDebris wants, so the caller can
                    // hand it straight over. See takeHangers.
                    {
                        if (hangVol_.size() != hangSolid_.size())
                            hangVol_.assign(hangSolid_.size(), mat::AIR);
                        else if (!hangHave_)
                            std::fill(hangVol_.begin(), hangVol_.end(), mat::AIR);
                        const int hh = terrain.heightVox(wi, wj, memo);
                        const uint8_t tp = terrain.topMaterial(wi, wj, hh);
                        const uint8_t mm = terrain.materialAt(wi, wj, wy, hh, tp);
                        if (mm != mat::AIR) {
                            // meshVolume's layout is x + z*n + y*n*n, which is
                            // what ix() already gives.
                            hangVol_[q] = mm;
                            hangHave_ = true;
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
        hangHave_ = false;
        hangN_ = n;
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
                    // ---------------------------------------------------
                    // IT FALLS. IT DOES NOT SIMPLY STOP EXISTING.
                    //
                    // The same bug the terrain had, in the model's own frame:
                    // a chip this blow cut loose only became anything if it
                    // happened to land inside the spoil cube -- the bite's own
                    // radius -- and everything further out was set to AIR and
                    // gone. THIS is the path a visible piece breaking off a
                    // boulder takes; the terrain one frees a voxel or two per
                    // blow and was never what was being reported.
                    //
                    // Collected in MODEL space, which is what spawnDebris
                    // wants: the caller hands it the model's quarter turn and
                    // the body is born wearing it, exactly as the spoil is.
                    if (v != mat::AIR) {
                        if (hangVol_.size() != hangSolid_.size())
                            hangVol_.assign(hangSolid_.size(), mat::AIR);
                        else if (!hangHave_)
                            std::fill(hangVol_.begin(), hangVol_.end(), mat::AIR);
                        hangVol_[q] = v;
                        hangHave_ = true;
                    }
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
        // The cut-loose piece is centred on the same voxel and stands in the
        // same frame, so it takes the same place and the same turn.
        if (hangHave_) {
            const float pmx = (float(mx) + 0.5f) * VOXEL_M;
            const float pmz = (float(mz) + 0.5f) * VOXEL_M;
            float hwx = 0.0f, hwz = 0.0f;
            solidWorldSpace(so, pmx, pmz, &hwx, &hwz);
            hangAt_ = Vec3{hwx, so.baseY + (float(my) + 0.5f) * VOXEL_M, hwz};
            hangYaw_ = float(so.yaw & 3) * 1.57079633f;
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
    // Where the first fruit of the run landed, or a zero vector if the
    // orchard pass placed none. Read once, by the load report.
    Vec3 firstFruit() const { return fruitSeen_.empty() ? Vec3(0, 0, 0) : fruitSeen_[0]; }
    // What the last level hanger sweep freed, for --fire-test. -1 means no
    // body: voxels removed with no slot is exactly the failure the RULE over
    // kMinBodyVoxels exists to make loud.
    // -----------------------------------------------------------------------
    // WHAT IN THIS MAP IS ALREADY STANDING ON NOTHING.
    //
    // (user 2026-09-17: "audit for floating voxels and make them subject to
    //  physics.")
    //
    // ONE FLOOD FROM THE FOUNDATION, and everything it does not reach is
    // floating by definition. The voxeliser lays solid ground under the whole
    // footprint, so y == 0 is a complete and exact seed -- there is no
    // sampling and no tolerance in this answer.
    //
    // A DIAGNOSTIC, NOT A SWEEP, and deliberately so. dropLevelHangers deals
    // with what the PLAYER cuts loose, which is the case that was reported.
    // What this finds is different in kind: geometry the .fbx was authored
    // with that never touched the ground in the first place -- a sign on a
    // bracket, an awning, a wire. Turning all of that into rigid bodies at
    // load would rain the map's own props on the floor the moment [O] is
    // pressed, which is not what anybody means by nothing floats.
    //
    // So this REPORTS, and a person decides. --float-audit.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE MAP DID NOT REACH ITS OWN GROUND.
    //
    // (user 2026-09-17: "nothing in the world should be floating ... audit for
    //  floating voxels and make them subject to physics.")
    //
    // AND THE AUDIT FOUND SOMETHING PHYSICS MUST NOT BE THE ANSWER TO.
    // --float-audit reported 1,108,956 voxels -- 13.04% of the map -- in 610
    // pieces that no flood from the foundation can reach, the largest of them
    // 182,155 voxels and 18.4 m tall. That is a BUILDING. Turning it into a
    // rigid body because it is unsupported would not stop it floating, it
    // would drop the map.
    //
    // THE ONE NUMBER THAT SAID WHOSE FAULT IT WAS: that building sits at row
    // 14 with solid TWO ROWS beneath it. Twenty centimetres. It is not
    // suspended in the air, it is standing just short of the floor -- the .fbx
    // models the houses sitting on a lawn slab that is half a voxel thick, the
    // foundation is laid to the MEASURED lawn row, and between the two there
    // is a gap nothing fills. Every house in the map is hovering.
    //
    // SO THE FIX IS TO CLOSE THE GAP, NOT TO SIMULATE IT. Walk each column up
    // from the foundation, and where the next solid above it is within
    // kLevelSeatVox, fill the air between with what is already underneath. The
    // geometry added is under the buildings and is never seen; what changes is
    // that they are attached to the world, which is what nothing floats means
    // for a static map.
    //
    // BOUNDED, AND THAT IS WHAT KEEPS IT HONEST. Four rows is 40 cm -- enough
    // for a slab that was rounded away, not enough to grow a pillar under a
    // sign hanging two metres up. Anything still floating after this is
    // floating on purpose, and --float-audit will say so.
    // -----------------------------------------------------------------------
    void seatLevelOnItsFoundation() {
        if (levelVol_.empty()) return;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        auto idx = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        };
        long long filled = 0;
        int columns = 0;
        for (int z = 0; z < SZ; ++z)
            for (int x = 0; x < SX; ++x) {
                if (levelVol_[idx(x, 0, z)] == mat::AIR) continue;   // no foundation here
                // The top of the solid run that starts at the foundation.
                int top = 0;
                while (top + 1 < SY && levelVol_[idx(x, top + 1, z)] != mat::AIR) ++top;
                // ...and the next solid above it, if it is close enough to be a
                // gap rather than a gap on purpose.
                int next = -1;
                for (int d = 2; d <= kLevelSeatVox + 1 && top + d < SY; ++d)
                    if (levelVol_[idx(x, top + d, z)] != mat::AIR) {
                        next = top + d;
                        break;
                    }
                if (next < 0) continue;
                // FILLED WITH WHAT IS ALREADY UNDER IT, so the seam wears the
                // ground's own colour rather than inventing one. It is buried
                // either way, but a material nothing else in the map uses is
                // how an invisible fill becomes a visible stripe the first
                // time somebody digs near it.
                const uint8_t m = levelVol_[idx(x, top, z)];
                for (int y = top + 1; y < next; ++y) {
                    levelVol_[idx(x, y, z)] = m;
                    ++filled;
                }
                ++columns;
            }
        if (filled)
            std::printf("  level    seated on its foundation: %lld voxels filled under %d "
                        "columns (gaps up to %.2f m)\n",
                        filled, columns, double(kLevelSeatVox) * VOXEL_M);
    }

    // -----------------------------------------------------------------------
    // WHAT THE MAP HELD UP BEFORE ANYBODY SHOT AT IT.
    //
    // (user 2026-09-17: "its breaking things vertically, it should only leave
    //  the initial shot mark ... things are glitching when they become rigid
    //  bodies.")
    //
    // dropLevelHangers asks CAN THIS REACH THE GROUND, and on its own that is
    // the wrong question for a map that was never fully grounded to begin
    // with. --float-audit measures 489 pieces and 4.74% of the voxels that no
    // flood from the foundation reaches even before a round is fired -- roofs
    // and upper storeys with real gaps in the source model. Shoot anywhere
    // near one of those and the sweep correctly observes that it cannot reach
    // the ground, and cuts the whole thing loose. THAT is the vertical
    // breaking: a post or a panel that was always floating, turned into a
    // rigid body by a shot that happened to pass it.
    //
    // SO THE QUESTION BECOMES: DID THIS SHOT DISCONNECT IT. This is the
    // before picture -- every voxel the foundation held up at load -- and a
    // component is severed only if it USED to be in here. Something that was
    // already floating stays exactly where the map author left it.
    //
    // A BIT PER CELL. 136 M cells is 17 MB resident, which is the price of the
    // sweep being able to tell a thing it broke from a thing it found broken.
    // -----------------------------------------------------------------------
    void markLevelGrounded() {
        levelGrounded_.assign(levelVol_.size(), false);
        if (levelVol_.empty()) return;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        auto idx = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        };
        auto solid = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
            return levelVol_[idx(x, y, z)] != mat::AIR;
        };
        static const int kN[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        std::vector<size_t> stack;
        long long n = 0;
        for (int z = 0; z < SZ; ++z)
            for (int x = 0; x < SX; ++x) {
                if (!solid(x, 0, z) || levelGrounded_[idx(x, 0, z)]) continue;
                stack.clear();
                stack.push_back(idx(x, 0, z));
                levelGrounded_[idx(x, 0, z)] = true;
                while (!stack.empty()) {
                    const size_t k = stack.back();
                    stack.pop_back();
                    ++n;
                    const int yy = int(k / (size_t(SX) * size_t(SZ)));
                    const int rem = int(k % (size_t(SX) * size_t(SZ)));
                    const int zz = rem / SX, xx = rem % SX;
                    for (const auto &nb : kN) {
                        const int ax = xx + nb[0], ay = yy + nb[1], az = zz + nb[2];
                        if (!solid(ax, ay, az)) continue;
                        const size_t nk = idx(ax, ay, az);
                        if (levelGrounded_[nk]) continue;
                        levelGrounded_[nk] = true;
                        stack.push_back(nk);
                    }
                }
            }
        std::printf("  level    %lld voxels stand on the foundation -- only these can be "
                    "cut loose\n", n);
    }

    struct FloatAudit {
        long long solid = 0;     // solid voxels in the map
        long long grounded = 0;  // ...reachable from the foundation
        long long floating = 0;  // ...not
        int pieces = 0;          // how many separate floating components
        int largest = 0;         // voxels in the biggest one
        int tallest = 0;         // and how many voxels tall it stands
        // -- AND THE ONE NUMBER THAT SAYS WHOSE FAULT IT IS ---------------
        //
        // How far the biggest floating piece's underside is above the nearest
        // solid beneath it. ONE means it is resting on something and the flood
        // simply could not step across -- a gap the voxeliser left, and a data
        // problem. A large number means it is genuinely suspended in air.
        // Without this the audit cannot tell a broken map from a broken test.
        int largestBaseY = 0;    // the lowest row the biggest piece occupies
        int largestGapY = -1;    // ...and the drop from it to the next solid
    };

    // -----------------------------------------------------------------------
    // WHERE THE FREE-STANDING POSTS ARE -- the light poles, the signs, the
    // fence uprights. Used by --pole-test, which is the acceptance test for
    // "break a light pole and it falls".
    //
    // A POST IS A RUN THAT STANDS CLEAR. It starts above the column's own
    // ground run, it is at least minH tall, and the 9x9 of map around its
    // middle is nearly all air -- which is what separates a lamp post from a
    // wall, and is measured rather than assumed because a wall column passes
    // every other description of a post.
    //
    // Returns the world position of each post's FOOT.
    void findLevelPosts(int minH, std::vector<Vec3> *out) const {
        if (!out) return;
        out->clear();
        if (levelVol_.empty()) return;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        auto idx = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        };
        auto solid = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
            return levelVol_[idx(x, y, z)] != mat::AIR;
        };
        std::vector<uint8_t> claimed(levelVol_.size(), 0);
        for (int z = 1; z < SZ - 1; ++z)
            for (int x = 1; x < SX - 1; ++x) {
                if (!solid(x, 0, z)) continue;
                int y = 0;
                while (y + 1 < SY && solid(x, y + 1, z)) ++y;
                int s = -1;
                for (int yy = y + 1; yy < SY; ++yy)
                    if (solid(x, yy, z)) { s = yy; break; }
                if (s < 0) continue;
                int e = s;
                while (e + 1 < SY && solid(x, e + 1, z)) ++e;
                if (e - s + 1 < minH) continue;
                if (claimed[idx(x, s, z)]) continue;
                const int my = (s + e) / 2;
                int girth = 0;
                for (int dz = -4; dz <= 4; ++dz)
                    for (int dx = -4; dx <= 4; ++dx)
                        if (solid(x + dx, my, z + dz)) ++girth;
                if (girth > 25) continue;   // a wall or a roof, not a post
                // CLAIM THE WHOLE POST, or every column of a 5x5 shaft is
                // reported as a post of its own and the test fires twenty-five
                // rounds into the same lamp.
                for (int yy = s; yy <= e; ++yy)
                    for (int dz = -4; dz <= 4; ++dz)
                        for (int dx = -4; dx <= 4; ++dx)
                            if (solid(x + dx, yy, z + dz)) claimed[idx(x + dx, yy, z + dz)] = 1;
                out->push_back(Vec3{kLevelAtX + float(x) * VOXEL_M,
                                    kLevelAtY + float(s) * VOXEL_M,
                                    kLevelAtZ + float(z) * VOXEL_M});
            }
    }

    // -----------------------------------------------------------------------
    // IS ANYTHING DIRECTLY ABOVE THIS POINT NOW STANDING ON AIR?
    //
    // The verdict --pole-test needs, and the only one that cannot be gamed.
    // "Did the sweep drop something" is the wrong question: most columns in
    // this map are walls, a cut does not sever them, and dropping nothing is
    // the RIGHT answer there. The question that matches the rule is whether the
    // cut left anything hanging -- so this floods the piece above the cut and
    // reports its size if it cannot reach the foundation, and 0 if it can.
    //
    // If the sweep did its job the piece is already gone from the grid and this
    // finds nothing, which is also 0. Both good outcomes answer 0; only a piece
    // still present AND disconnected answers non-zero.
    //
    // Bounded by maxVox, and a flood that exhausts it is reported as 0 -- that
    // is a piece of building, not a hanger, and the caps in dropLevelHangers
    // make the same call for the same reason.
    int levelHangingAbove(const Vec3 &at, int maxVox) const {
        if (levelVol_.empty()) return 0;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        auto idx = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        };
        auto solid = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
            return levelVol_[idx(x, y, z)] != mat::AIR;
        };
        const int cx = int((at.x - kLevelAtX) / VOXEL_M);
        const int cy = int((at.y - kLevelAtY) / VOXEL_M);
        const int cz = int((at.z - kLevelAtZ) / VOXEL_M);
        if (cx < 0 || cz < 0 || cx >= SX || cz >= SZ) return 0;
        int sy = -1;
        for (int y = maxi(0, cy); y < SY; ++y)
            if (solid(cx, y, cz)) { sy = y; break; }
        if (sy < 0) return 0;
        static const int kNb[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        std::unordered_set<uint32_t> seen;
        std::vector<int> stack;
        stack.push_back(int(idx(cx, sy, cz)));
        seen.insert(uint32_t(idx(cx, sy, cz)));
        int n = 0;
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            ++n;
            if (n > maxVox) return 0;   // structure, not a hanger
            const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
            const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
            const int z = rem / SX, x = rem % SX;
            if (y == 0) return 0;       // it reaches the foundation
            for (const auto &nb : kNb) {
                const int ax = x + nb[0], ay = y + nb[1], az = z + nb[2];
                if (!solid(ax, ay, az)) continue;
                const uint32_t nk = uint32_t(idx(ax, ay, az));
                if (seen.count(nk)) continue;
                seen.insert(nk);
                stack.push_back(int(nk));
            }
        }
        return n;
    }

    FloatAudit auditLevelFloaters() const {
        FloatAudit a;
        if (levelVol_.empty()) return a;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        auto idx = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        };
        auto solid = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
            return levelVol_[idx(x, y, z)] != mat::AIR ||
                   levelDisplay_.a[idx(x, y, z)] != mat::AIR;
        };
        for (int y = 0; y < SY; ++y)
            for (int z = 0; z < SZ; ++z)
                for (int x = 0; x < SX; ++x)
                    if (solid(x, y, z)) ++a.solid;

        // A BIT PER CELL. The map is 136 M cells, so this is 17 MB and a
        // vector<bool> is the right container for once -- a byte each would be
        // 136 MB to answer a question and throw the answer away.
        std::vector<bool> seen(size_t(SX) * size_t(SY) * size_t(SZ), false);
        std::vector<size_t> stack;
        static const int kN[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        auto flood = [&](int sx, int sy, int sz, int *count, int *loY, int *hiY) {
            stack.clear();
            stack.push_back(idx(sx, sy, sz));
            seen[idx(sx, sy, sz)] = true;
            while (!stack.empty()) {
                const size_t k = stack.back();
                stack.pop_back();
                const int y = int(k / (size_t(SX) * size_t(SZ)));
                const int rem = int(k % (size_t(SX) * size_t(SZ)));
                const int z = rem / SX, x = rem % SX;
                ++(*count);
                if (loY && y < *loY) *loY = y;
                if (hiY && y > *hiY) *hiY = y;
                for (const auto &nb : kN) {
                    const int nx = x + nb[0], ny = y + nb[1], nz = z + nb[2];
                    if (!solid(nx, ny, nz)) continue;
                    const size_t nk = idx(nx, ny, nz);
                    if (seen[nk]) continue;
                    seen[nk] = true;
                    stack.push_back(nk);
                }
            }
        };

        // ---- everything the foundation holds up --------------------------
        for (int z = 0; z < SZ; ++z)
            for (int x = 0; x < SX; ++x)
                if (solid(x, 0, z) && !seen[idx(x, 0, z)]) {
                    int n = 0;
                    flood(x, 0, z, &n, nullptr, nullptr);
                    a.grounded += n;
                }

        // ---- and everything it does not ----------------------------------
        for (int y = 1; y < SY; ++y)
            for (int z = 0; z < SZ; ++z)
                for (int x = 0; x < SX; ++x) {
                    if (!solid(x, y, z) || seen[idx(x, y, z)]) continue;
                    int n = 0, loY = y, hiY = y;
                    flood(x, y, z, &n, &loY, &hiY);
                    a.floating += n;
                    ++a.pieces;
                    if (hiY - loY + 1 > a.tallest) a.tallest = hiY - loY + 1;
                    if (n > a.largest) {
                        a.largest = n;
                        a.largestBaseY = loY;
                        // Straight down from this seed until something solid
                        // turns up. The seed is on the piece's own lowest row
                        // by construction -- the scan walks y upward -- so it
                        // is a fair place to measure the gap from.
                        a.largestGapY = -1;
                        for (int d = 1; d <= y; ++d)
                            if (solid(x, y - d, z)) {
                                a.largestGapY = d;
                                break;
                            }
                    }
                }
        return a;
    }

    int lastHangVox() const { return lastHangVox_; }
    int lastHangSlot() const { return lastHangSlot_; }
    void clearHangReport() {
        lastHangVox_ = 0;
        lastHangWorst_ = 0;
        lastHangPieces_ = 0;
        lastHangDim_[0] = lastHangDim_[1] = lastHangDim_[2] = 0;
        lastHangSlot_ = -1;
    }
    int lastHangWorst() const { return lastHangWorst_; }
    int lastHangPieces() const { return lastHangPieces_; }
    // THE SHAPE OF THE BIGGEST ONE, because a voxel count cannot tell a chip
    // from a rip. "its breaking things vertically" is a statement about
    // DIMENSIONS -- 2 x 2 x 25 is the report, 5 x 4 x 5 is a chip -- and the
    // report had no way to say which had happened.
    void lastHangDims(int *w, int *h, int *d) const {
        if (w) *w = lastHangDim_[0];
        if (h) *h = lastHangDim_[1];
        if (d) *d = lastHangDim_[2];
    }

    size_t decorCount(int kind) const {
        size_t n = 0;
        for (const auto &kv : chunks_) n += size_t(kv.second.decorKind[kind]);
        return n;
    }
    size_t residentTris() const { return residentTris_; }
    size_t pendingChunks() { return mesher_.inFlight(); }

    // -----------------------------------------------------------------------
    // HOW MANY CHUNKS THE RING HAS ASKED FOR AND NOT YET GOT.
    //
    // wanted_ is the disc rering() last drew and chunks_ is what is actually
    // resident, so the difference is exactly "the world is still coming in".
    // chunkAt() answers the same question for ONE column, which is all the
    // spawn check needs; the door out of the pause room needs the whole ring,
    // because the player is about to be put down in the middle of it and look
    // around.
    // -----------------------------------------------------------------------
    size_t missingChunks() const {
        size_t n = 0;
        for (long long k : wanted_)
            if (chunks_.find(k) == chunks_.end()) ++n;
        return n;
    }

    // Where the deck sits in the world. Far from the wood so nothing streams
    // into view behind it, and at a round height so a model's own numbers are
    // easy to read off it.
    static Vec3 stageOrigin() { return Vec3(kStageAtX, kStageAtY, kStageAtZ); }
    static Vec3 stageCentre() {
        return Vec3(kStageAtX + float(kStageVox) * VOXEL_M * 0.5f, kStageAtY + VOXEL_M,
                    kStageAtZ + float(kStageVox) * VOXEL_M * 0.5f);
    }

    // -- THE DECK, FOR THE PLAYER TO STAND ON -------------------------------
    //
    // The same numbers the room needs, for the same reason and after the same
    // fault: the walk collider reads the TERRAIN, and what the terrain says is
    // under you here is six hundred and forty metres of nothing. See
    // App::clampToStage -- and the pause room had the same six planes until it
    // stopped being a place you travelled to.
    //
    // THE TOP OF THE DECK, NOT ITS ORIGIN. It is one voxel thick and placed at
    // kStageAtY, so the surface is one voxel up -- which is the same height
    // stageCentre() reports, because the subject is standing on it too.
    static float stageFloorY() { return kStageAtY + VOXEL_M; }
    static float stageMinX() { return kStageAtX; }
    static float stageMaxX() { return kStageAtX + float(kStageVox) * VOXEL_M; }
    static float stageMinZ() { return kStageAtZ; }
    static float stageMaxZ() { return kStageAtZ + float(kStageVox) * VOXEL_M; }

    // WHERE A BUTTON IS is the app's to say now -- see App::panelButtonAt. What
    // stays here is what a button IS: how big it is, what it is called, what
    // colour it wears and how far it travels when it is pressed.
    static float buttonRadiusM() { return float(kBtnR) * VOXEL_M; }
    // ...and how far apart the three of them stand. It was the room's own
    // kBtnX spacing, kept as a metre so a panel in the open world does not have
    // to know about a wall's voxel lattice.
    static constexpr float kBtnSpacingM = 1.30f;

    // -- WHAT EACH BUTTON IS CALLED, AND WHERE THE WORD HANGS ---------------
    //
    // The label is a plane of light standing in the air in front of the wall --
    // see V2Holo in Shared.slang -- and everything about where it is comes from
    // the button it names, for the reason buttonAt exists at all: two
    // descriptions of one object drift.
    //
    // A COLOUR IS A BUTTON'S PROPERTY, NOT A MODEL'S. kBtnRgb used to be a local
    // table inside loadRoomButtons, which was fine while the only thing that
    // wanted it was the palette it tinted the model with. The word over the
    // button wants the same red, so it moved out here rather than being typed a
    // second time -- a green "quit" is the exact bug this prevents.
    static constexpr uint8_t kBtnRgb[3][3] = {
        {214, 58, 58}, {64, 196, 92}, {122, 96, 232}};

    static const char *buttonLabel(int b) {
        static const char *kWords[3] = {"quit", "back", "discord"};
        return kWords[b];
    }

    // ONE GLYPH PIXEL, 2.1 cm (user 2026-09-13: a quarter smaller than the 2.8
    // it landed at). The CEILING on it is geometry rather than taste and is
    // worth writing down, because it is the number a future change will walk
    // into: "discord" is seven glyphs at six cells of advance, so 41 cells wide,
    // and "back" beside it is 23 -- half of each is 32 cells of the gap between
    // their two buttons. At the ten-voxel spacing those buttons started on that
    // capped the cell at 3.1 cm; at thirteen it is 4.0 cm. 2.1 leaves 63 cm of
    // clear wall between the two words.
    static constexpr float kBtnLabelCellM = 0.021f;
    // How far above the button's CENTRE the middle of the word sits. The model
    // is five voxels tall, so its top is 25 cm up; this leaves about a hand's
    // width of dark wall between the two, which is what makes the label read as
    // belonging to the button rather than sitting on it.
    static constexpr float kBtnLabelRiseM = 0.52f;
    // WHERE THE WORD HANGS is the app's too, for the reason the button's own
    // place is: both come off the panel's pose, and deriving one of them here
    // and one of them there is the drift this note has warned about twice.
    // App::panelLabelMid is the reader of kBtnLabelRiseM.
    // WHICH WAY THE WORD FACES is the panel's to say -- App::setRoomLabel reads
    // panelRight_ and panelUp_. It used to be these two constants, and it could
    // be, while the wall the words hung on never moved.

    // -----------------------------------------------------------------------
    // WHERE THE THREE BUTTONS ARE THIS FRAME.
    //
    // press[b] is 0 at rest and 1 fully depressed; it moves the button INTO the
    // wall along -Z by kBtnTravelM and nothing else -- no squash, no rotation.
    // A button is a rigid thing on a spring, and the only honest animation for
    // one is a translation.
    //
    // CALLED EVERY FRAME, room or not. The band is a fixed set of slots and a
    // slot nobody writes keeps whatever was in it, so "switched off" has to be
    // published too -- this is the same rule the butterflies' empty slots obey.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE THREE BUTTONS, WHEREVER THE CALLER PUT THEM.
    //
    // They used to be nailed to a wall four kilometres up -- buttonAt() read
    // the room's own corner and this wrote the instance straight out of it.
    // There is no room any more (user 2026-09-14: "remove the esc room from the
    // sky. instead put the 3 balls in front of the player IN GAME"), so the
    // panel is a POSE the app hands in: three centres and the basis they stand
    // on, worked out from where the player was looking when they pressed the
    // key.
    //
    // `into` is the direction a pressed button travels, which is away from the
    // player -- the press is 2 cm of real movement along it, not a scale or a
    // flash, and it has to follow the panel or a button pressed from the south
    // would sink sideways.
    // -----------------------------------------------------------------------
    void publishButtons(bool show, const float *press, const Vec3 *centre, const Vec3 &right,
                        const Vec3 &up, const Vec3 &into) {
        if (btnModel_[0] < 0) return;
        // THE TRANSLATION IS THE MODEL'S CORNER, NOT ITS CENTRE. place() writes
        // tx/ty/tz straight into the transform and uses the half-box only to
        // work out where the middle ended up for the motion vector -- so a
        // caller that hands it a centre gets a model displaced by half its own
        // size, which is exactly what the first render showed: three buttons
        // sitting up and to the right of the bezels they belong in. The
        // butterflies' own note says the same thing from the other side.
        // COLUMNS ARE WHERE THE MODEL'S OWN AXES GO, because place() reads the
        // matrix by ROWS -- the same note birds.h and birdflock.h carry.
        const float m[9] = {right.x, up.x, into.x,
                            right.y, up.y, into.y,
                            right.z, up.z, into.z};
        const float hx = 0.5f * float(btnSx_) * VOXEL_M;
        const float hy = 0.5f * float(btnSy_) * VOXEL_M;
        const float hz = 0.5f * float(btnSz_) * VOXEL_M;
        const float ox = m[0] * hx + m[1] * hy + m[2] * hz;
        const float oy = m[3] * hx + m[4] * hy + m[5] * hz;
        const float oz = m[6] * hx + m[7] * hy + m[8] * hz;
        for (int b = 0; b < 3; ++b) {
            const float d = press ? press[b] : 0.0f;
            const Vec3 c = centre[b] + into * (d * kBtnTravelM);
            // THE ANIMAL, NOT ITS BOX -- the button's own centre, so a press is
            // 2 cm of motion vector and the rotated half-box is not in it.
            const float anchor[3] = {c.x, c.y, c.z};
            setFlyerInstance(kButtonSlot0 + b, btnModel_[b], m, c.x - ox, c.y - oy, c.z - oz,
                             nullptr, show && btnModel_[b] >= 0, nullptr, anchor);
        }
        flushFlyerInstances();
    }

    // -----------------------------------------------------------------------
    // SWITCH A RUN OF THE FLYER BAND OFF.
    //
    // A slot nobody writes keeps whatever was last put in it, so a population
    // that simply stops ticking does not go away -- it stands still wherever it
    // was. That is fine four kilometres away inside a sealed box, and it is
    // still one traversed instance per animal for a room that has none. This is
    // the one call that says "nothing of yours is here": mask 0, which the
    // structure never visits.
    // -----------------------------------------------------------------------
    void hideFlyerBand(int slot0, int count) {
        for (int i = 0; i < count; ++i)
            setFlyerInstance(slot0 + i, -1, nullptr, 0.0f, 0.0f, 0.0f, nullptr, false);
        flushFlyerInstances();
    }

    // -----------------------------------------------------------------------
    // LOAD THE THREE BUTTONS NOW, BEFORE ANYTHING IS PRESSED -- AND THE
    // PALETTE IS WHY.
    //
    // loadRoomButtons asks Palette::forModelColor for three entries it cannot
    // do without, one per button colour. forModelColor returns AIR when the
    // table is full, and the table holds 255.
    //
    // Built lazily on the first ESC, the pause menu was therefore the LAST
    // thing in the program to ask for colours -- after every pine, rock,
    // flower, fish and tool, and after the asset editor's models, which alone
    // overrun the table by fourteen. Measured on a plain start: 250 of 255 used
    // with six wanted; open the editor once first and it is 255 of 255 with
    // none left. What the user saw was exactly that arithmetic -- "the esc room
    // is gone, 2 of 3 buttons are gone", only the one button whose colour
    // fitted in the last free slot surviving.
    //
    // So they are loaded at START-UP instead, before the held tools, where
    // three colours are cheap and certain. The pause menu is a fixture of the
    // interface and not scenery: it must not be served out of whatever the
    // world left over.
    //
    // IT WAS SIX COLOURS AND A 66,032-TRIANGLE BLAS when the menu was a room --
    // the shell white, the bulb's glass and its flex went with the room. The
    // three models are the whole of it now, and they ride the flyer band.
    // -----------------------------------------------------------------------
    void prewarmRoom() { loadRoomButtons(); }

    // In the editor, or in the wood.
    void setStage(bool on) {
        if (on == stage_) return;
        if (on) buildStage();
        stage_ = on;
        rebuildTlas();
    }
    bool staged() const { return stage_; }

    // -- THE BUILDING LEVEL, THE SAME DOOR THE DECK HAS ----------------------
    bool setLevel(bool on) {
        if (on == level_) return level_;
        if (on) {
            buildLevelBlas();
            if (levelBlocks_.empty()) return false;   // nothing to travel to
        }
        level_ = on;
        // -- A PER-PLACE MATERIAL TABLE WAS TRIED HERE AND DOES NOT WORK YET --
        //
        // (user 2026-09-17: "surely we can have multiple color paletes for
        // multiple worlds?" -- and the idea is right. The wood is not in the
        // acceleration structure while the level is open, so the two can never
        // disagree on screen about what entry 243 means.)
        //
        // WHAT WAS BUILT: Palette::beginLevelTable / forLevelColor, a second
        // 255-entry table allocated top-down over the entries the level does
        // not share, with the fixed mat:: band, the held kit and the wood's
        // flower models reserved out of it. World::buildLevelPalette still
        // builds it -- that half works and the numbers were right: 13 colours
        // into the level's own table with 121 free, and the WOOD dropped from
        // 255 of 255 (the wheat losing two of its colours) to 245.
        //
        // WHAT DOES NOT WORK is getting the device to read it. Swapping
        // materials_ here -- by in-place update AND by rebuilding the buffer --
        // changes nothing on screen. MEASURED, so nobody has to measure it
        // again:
        //
        //   * the level's mesh really does carry the level's ids: id 243 is
        //     552,327 voxels of it (histogram in buildLevelBlas).
        //   * the table really is uploaded: entry 243 was forced to pure
        //     MAGENTA and uploadMaterials printed (1.000,0.000,1.000) going in.
        //   * nothing on screen was magenta, and the map kept rendering in the
        //     WOOD's colours at those ids.
        //
        // AND THE STRONGEST CLUE CAME AFTERWARDS, from a different bug: the
        // level's entries were ALSO invisible on the shared table until
        // buildLevelPalette was made to call uploadMaterials itself (see the
        // note there). So an upload from buildLevelPalette LANDS and an upload
        // from here, one call later in the same start-up, does NOT. That is
        // where to start -- not in the palette, but in what is different about
        // this call site. gMaterials is bound per frame from materialBuffer()
        // at three sites in tracer.h.
        //
        // UNTIL THEN THE LEVEL ALLOCATES OUT OF THE WOOD'S TABLE, as it always
        // did -- see buildLevelPalette, which is one call away from switching
        // back the moment the above is understood.
        rebuildTlas();
        return level_;
    }
    bool levelOn() const { return level_; }
    bool levelReady() const { return levelLoaded_; }
    static Vec3 levelOrigin() { return Vec3(kLevelAtX, kLevelAtY, kLevelAtZ); }

    // IS THERE ANYTHING AT THIS WORLD POINT IN THE MAP? For --rip-test, which
    // fires on a lattice and must not spend its budget on open air. Asks both
    // grids, the same pair carveLevel cuts.
    bool levelSolidAtM(const Vec3 &at) const {
        if (levelVol_.empty() || levelDisplay_.a.empty()) return false;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        const int x = int((at.x - kLevelAtX) / VOXEL_M);
        const int y = int((at.y - kLevelAtY) / VOXEL_M);
        const int z = int((at.z - kLevelAtZ) / VOXEL_M);
        if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
        const size_t k = size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        return levelVol_[k] != mat::AIR || levelDisplay_.a[k] != mat::AIR;
    }
    float levelMinX() const { return kLevelAtX; }
    float levelMaxX() const { return kLevelAtX + float(levelAsset_.sx) * VOXEL_M; }
    float levelMinZ() const { return kLevelAtZ; }
    float levelMaxZ() const { return kLevelAtZ + float(levelAsset_.sz) * VOXEL_M; }

    // -- WHERE YOU ARRIVE: A FRONT YARD, LOOKING DOWN THE MAP ----------------
    //
    // MEASURED, NOT WRITTEN DOWN. The model's own origin is nothing in
    // particular, so a spawn in level metres would have to be re-derived by
    // hand every time the asset moved. Everything here comes off the asset's
    // own extent and its own heightfield, so a re-voxelised map takes the
    // arrival with it.
    //
    // -- WHY THIS IS NOT THE BUILDING'S CORNER ANY MORE ----------------------
    //
    // It used to be "two metres in from the far corner", and that was right for
    // what it was: a building is a thing you photograph, the far corner is the
    // longest sightline a 33 m pad has, and the whole of it including the mast
    // fits in the frame from there. A MAP is not a thing you photograph. You
    // stand in one, and the corner of Nuketown is the strip of foundation
    // outside the fence, facing across the short axis at the back of a house.
    //
    // So the spawn is down the LONG axis instead -- the map is 16.4 x 32.7 m
    // and that axis is yard, house, street, house, yard -- standing in a front
    // yard at one end, looking the full 32.7 m down it. That is the view the
    // map is built around.
    //
    // AND IT IS SEARCHED RATHER THAN NAMED. The ideal point is the middle of
    // the width, four metres in from the end; what is actually AT that point
    // depends on the asset, and on this one it can be a hedge or a porch. So
    // the ideal is a starting point for a ring search over the asset's own
    // heightfield, and the first open column wins. "Open" is a column with
    // less than kOpenM of anything in it -- the foundation and its painted
    // slab and nothing else. A fence, a car, a wall or a roof all fail it.
    //
    // THE FALLBACK IS THE IDEAL POINT, unsearched. It is reached only if the
    // asset has no open ground anywhere near the end of it, which would mean
    // the level is not what this function was written for -- and a spawn that
    // is merely wrong is better than one that returns nothing.
    Vec3 levelSpawn() const {
        const float spanX = float(levelAsset_.sx) * VOXEL_M;
        const float spanZ = float(levelAsset_.sz) * VOXEL_M;
        const float idealX = kLevelAtX + spanX * 0.5f;
        const float idealZ = kLevelAtZ + spanZ - 4.0f;
        if (levelColTop_.empty())
            return Vec3(idealX, kLevelAtY, idealZ);

        // -- WHAT COUNTS AS OPEN GROUND IS MEASURED, NOT NAMED ---------------
        //
        // (user 2026-09-17: "when the player spawns into nuketown, he spawns in
        // the ground".)
        //
        // THIS WAS AN ABSOLUTE HEIGHT AND IT STOPPED BEING TRUE. It read "a
        // column with more than one metre in it is something you would be
        // standing inside" -- ten voxels -- which was right when the map sat
        // on a 7-row foundation. The 3x rebuild put the ground at row 11 and
        // every open column on the map now answers TWELVE, so the test rejected
        // all of them, the ring search ran to its radius and fell out of the
        // bottom, and the fallback below returned kLevelAtY: the BASE of the
        // grid, which is half a metre under the concrete. The player spawned
        // inside the foundation.
        //
        // So the ground is read off the asset the way the voxelizer reads it --
        // the most common column top is the floor of the place, because a map
        // is mostly floor -- and "open" is that plus a metre of clutter. It is
        // right at any scale because it is not a height.
        int ground = 0;
        {
            std::vector<int> hist;
            for (int16_t v : levelColTop_) {
                if (v <= 0) continue;
                if (size_t(v) >= hist.size()) hist.resize(size_t(v) + 1, 0);
                ++hist[size_t(v)];
            }
            int best = 0;
            for (size_t i = 1; i < hist.size(); ++i)
                if (hist[i] > best) {
                    best = hist[i];
                    ground = int(i);
                }
        }
        constexpr float kOpenM = 1.0f;
        const int top = ground + int(kOpenM / VOXEL_M);
        const int cx = int((idealX - kLevelAtX) / VOXEL_M);
        const int cz = int((idealZ - kLevelAtZ) / VOXEL_M);
        auto colAt = [&](int x, int z) {
            return int(levelColTop_[size_t(x) + size_t(z) * size_t(levelAsset_.sx)]);
        };
        // Rings outward from the ideal, so the nearest open ground to it wins
        // and the search is over as soon as it starts on an ordinary map. The
        // radius is half the map: past that there is nowhere left to look.
        const int maxR = std::max(levelAsset_.sx, levelAsset_.sz) / 2;
        for (int r = 0; r <= maxR; ++r)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    // The ring, not the disc -- the inside of it was tested on
                    // an earlier pass and testing it again would make this
                    // quadratic in the radius for no new answers.
                    if (r > 0 && std::abs(dx) != r && std::abs(dz) != r) continue;
                    const int x = cx + dx, z = cz + dz;
                    // TWO VOXELS IN FROM THE EDGE, so App::clampToLevel's fence
                    // is not already pressing on the body when it lands.
                    if (x < 2 || z < 2 || x >= levelAsset_.sx - 2 || z >= levelAsset_.sz - 2)
                        continue;
                    const int t = colAt(x, z);
                    // BELOW the ground is a pit or a lake bed and is not
                    // somewhere to arrive either; above it by more than a metre
                    // is standing on something.
                    if (t < ground || t > top) continue;
                    return Vec3(kLevelAtX + (float(x) + 0.5f) * VOXEL_M,
                                kLevelAtY + float(t) * VOXEL_M,
                                kLevelAtZ + (float(z) + 0.5f) * VOXEL_M);
                }
        // ...AND EVEN THE FALLBACK STANDS ON THE FLOOR. It used to return
        // kLevelAtY, which is the bottom of the GRID and not the bottom of the
        // map -- see above for what that did.
        return Vec3(idealX, kLevelAtY + float(maxi(ground, 1)) * VOXEL_M, idealZ);
    }
    // ...AND FACING THE MIDDLE OF IT, worked out FROM the spawn rather than
    // written down beside it: two descriptions of one aim drift apart, and
    // this one would drift silently into a wall.
    //
    // YAW IS atan2(aim.x, -aim.z) IN DEGREES. That is v2's convention rather
    // than a guess -- App::teleportToLife is the one place that inverts it and
    // this is that line read forwards. Worth stating because the sign is not
    // obvious and the first version of this spawn faced the empty sky.
    float levelSpawnYaw() const {
        const Vec3 s = levelSpawn();
        const float cx = kLevelAtX + float(levelAsset_.sx) * VOXEL_M * 0.5f;
        const float cz = kLevelAtZ + float(levelAsset_.sz) * VOXEL_M * 0.5f;
        return atan2f(cx - s.x, -(cz - s.z)) * 180.0f / PI;
    }

    // -- THE LAMPS -----------------------------------------------------------
    //
    // WHICH MATERIAL IS THE GLASS -- also the tracer's on switch, so a level
    // with no bulb art returns mat::AIR here and the caller leaves the light
    // off rather than lighting a material that nothing wears.
    uint8_t levelBulbMtl() const { return levelGlassMtl_; }

    // -- THE LEVEL'S FLOOR, FOR ANYTHING FALLING IN IT ---------------------
    //
    // updateDebris clamps every loose piece above "the one floor nothing may
    // ever be under", and in the wood that is the TERRAIN. The level has no
    // terrain -- it is four kilometres away and 640 m below -- so a chip cut
    // out of a wall here had nothing under it and fell out of the world. This
    // is the heightfield the walk already stands on, and carveLevel keeps it up
    // to date so a hole shot in a floor is a hole a chip can fall through.
    // -- ...AND WHY THAT IS THE WRONG QUESTION FOR A FALLING BODY ---------
    //
    // (user 2026-09-17: "the chunking mechanic caused by the bullet is not
    //  obeying physics correctly. the chunks teleport to the top of the
    //  building where they proceed to endlessly fall forever.")
    //
    // levelGroundM below answers WHAT IS THE TOP OF THIS COLUMN, which is the
    // right question for a walk -- you stand on top of things -- and a
    // catastrophic one for the debris backstop, because indoors there is
    // geometry ABOVE you. A chip cut out of a wall on the ground floor asked
    // it and got the ROOF, four metres over its head; the clamp in
    // updateDebris then fired (the piece is far below that floor) and
    // teleported the body onto the roof. It fell off, dropped below the roof
    // line again, and was clamped straight back up -- once per frame, for
    // ever. That is the report, exactly: teleport to the top of the building,
    // then fall endlessly.
    //
    // In the wood the same code is harmless only because a heightfield IS the
    // ceiling there: nothing is ever above the terrain, so "the top of this
    // column" and "the floor under this body" are the same number. A building
    // is the case that distinction was always waiting for.
    //
    // SO THIS ASKS DOWNWARD FROM THE BODY. The first solid voxel at or below
    // the body's own height, and its TOP face -- which is the floor it would
    // land on. Starting the scan at the body's centre rather than at its top
    // is deliberate and is the safe end to be wrong at: a body whose centre
    // has sunk INTO a slab starts on a solid voxel and is lifted back onto
    // that slab, while starting from its top could find a CEILING just above
    // it and clamp the body up through it -- the same class of bug in the
    // other direction.
    //
    // Falls through to kLevelAtY, the underside of the foundation, which is
    // the one height in this map that genuinely has nothing below it.
    float levelFloorBelowM(float wx, float wy, float wz) const {
        if (levelVol_.empty()) return kLevelAtY;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        const int x = int((wx - kLevelAtX) / VOXEL_M);
        const int z = int((wz - kLevelAtZ) / VOXEL_M);
        if (x < 0 || z < 0 || x >= SX || z >= SZ) return kLevelAtY;
        int y = int((wy - kLevelAtY) / VOXEL_M);
        if (y >= SY) y = SY - 1;
        for (; y >= 0; --y) {
            const size_t k = size_t(x) + size_t(z) * size_t(SX) +
                             size_t(y) * size_t(SX) * size_t(SZ);
            if (levelVol_[k] != mat::AIR) return kLevelAtY + float(y + 1) * VOXEL_M;
        }
        return kLevelAtY;
    }

    float levelGroundM(float wx, float wz) const {
        if (levelColTop_.empty()) return kLevelAtY;
        const int x = int((wx - kLevelAtX) / VOXEL_M);
        const int z = int((wz - kLevelAtZ) / VOXEL_M);
        if (x < 0 || z < 0 || x >= levelAsset_.sx || z >= levelAsset_.sz) return kLevelAtY;
        return kLevelAtY +
               float(levelColTop_[size_t(x) + size_t(z) * size_t(levelAsset_.sx)]) * VOXEL_M;
    }
    // ...AND WHICH ONE TO LIGHT FROM.
    //
    // THE SHADER HAS EXACTLY ONE POINT LIGHT. bulbPos is a single position and
    // sampleBulbSplit traces one shadow ray at it, so a map with six pendants
    // in it cannot have six lights without a renderer-wide change (see the
    // note in Shared.slang, which weighed that and said no for one room).
    //
    // What it CAN have is six glowing bulbs and one of them lighting the room
    // you are standing in, because the material test is per-voxel and free:
    // every bulb renders at bulbRadiance whatever this returns. So the light
    // follows the eye to the nearest glass, and the others are lamps you can
    // see but that do not cast. You are in one room at a time; the seam is
    // outdoors, between rooms, where the sun is doing the work anyway.
    // ...AND WHERE EVERY ONE OF THEM IS.
    //
    // ALL OF THEM, not the nearest. An earlier cut here answered "which bulb
    // should light the world this frame" and that question has no good answer:
    // whichever it picked, the rooms it did not pick went dark, which is a
    // light that follows the player around. The shader chooses per SHADING
    // POINT out of this list instead -- see kBulbSlots in Shared.slang.
    const std::vector<Vec3> &levelBulbs() const { return levelBulbs_; }

    // -----------------------------------------------------------------------
    // PLACING AND PULLING DOWN LAMPS BY HAND.
    //
    // (user 2026-09-17: "left click to remove the bulb and right click to place
    // a bulb".)
    //
    // EACH EDIT REBUILDS THE LEVEL, and that is affordable exactly because it
    // is an edit: meshing this map measures ~380 ms (printed by buildLevelBlas)
    // and a click is a deliberate act a third of a second apart at worst. The
    // same cost per BULLET is what stops the rounds carving this place -- see
    // the note over carveLevel.
    //
    // THE CEILING IS FOUND FROM THE HIT, not from the aim: a lamp goes on the
    // slab above wherever you pointed, so pointing at the floor of a room still
    // hangs it from that room's ceiling.
    bool placeLevelBulb(const Vec3 &at) {
        if (!levelPaletteReady_ || levelGlassMtl_ == mat::AIR) return false;
        if (int(levelBulbs_.size()) >= kLevelBulbMax) return false;
        const int bx = int((at.x - kLevelAtX) / VOXEL_M);
        const int bz = int((at.z - kLevelAtZ) / VOXEL_M);
        int y = int((at.y - kLevelAtY) / VOXEL_M);
        if (bx < 2 || bz < 2 || bx >= levelAsset_.sx - 2 || bz >= levelAsset_.sz - 2) return false;
        if (y < 0) y = 0;
        if (y >= levelAsset_.sy) y = levelAsset_.sy - 1;
        auto solidAt = [&](int py) {
            return levelVol_[size_t(bx) + size_t(bz) * size_t(levelAsset_.sx) +
                             size_t(py) * size_t(levelAsset_.sx) * size_t(levelAsset_.sz)] !=
                   mat::AIR;
        };
        // UP TO THE FIRST THING OVERHEAD. If the aim landed inside solid, climb
        // out of it first -- pointing at a wall should hang the lamp off the
        // ceiling above that wall rather than refusing.
        while (y < levelAsset_.sy - 1 && solidAt(y)) ++y;
        int roof = y;
        while (roof + 1 < levelAsset_.sy && !solidAt(roof + 1)) ++roof;
        noteBulb(bx, bz, roof, levelAsset_.sx, levelAsset_.sz, levelAsset_.sy);
        rebuildLevelBlas();
        return true;
    }

    // ...AND THE NEAREST ONE WITHIN `reach` COMES DOWN. Nearest to a POINT
    // rather than the first along a ray: a bulb is five voxels of glass on a
    // ceiling and asking a ray to hit it exactly is asking the player to be
    // precise about something they cannot quite see.
    // -----------------------------------------------------------------------
    // A BULB COMES OUT, AND IT DOES NOT COST THE WHOLE MAP.
    //
    // (user 2026-09-17: "when shooting a bulb it just dissapears. make the
    //  lightbulb break into peices and fall due to gravity. also it spikes bad
    //  when shooting the bulb. optimize it.")
    //
    // THE SPIKE AND THE DISAPPEARANCE WERE ONE LINE. This used to erase the
    // bulb from the light list and call rebuildLevelBlas() -- which releases
    // every block's triangles, CLEARS levelDisplay_ ENTIRELY, and meshes the
    // whole map again. That is the 621 ms the boot report prints for building
    // the level, paid on a trigger pull. And because the removal was a rebuild
    // rather than a carve, there were never any voxels to hand to physics: the
    // bulb was simply absent from the next mesh.
    //
    // SO IT IS A CARVE NOW, and the same carve the rounds already use: clear
    // the voxels, re-mesh only the blocks they were in, hand the voxels to
    // spawnDebris. One block instead of a hundred and twenty-eight.
    //
    // ONLY THE GLASS IS IDENTIFIED BY MATERIAL, and that is not fussiness --
    // levelCordMtl_ IS mat::ROCK (see loadLevelBulb), so scanning for the cord
    // would match every stone in the box. The flex stays on the ceiling, which
    // is also what a shot-out bulb leaves behind.
    //
    // THE BULB IS IN THE DISPLAY GRID AND NOT THE COLLIDER -- dressLevel
    // stamps into the copy buildLevelBlas meshes, never into levelVol_ -- so
    // there is no collider to keep in step and nothing to fix in levelColTop_.
    //
    // `shatter` is the difference between shooting one and unscrewing one. A
    // round breaks it into pieces that fall; the lamp in hand takes it whole.
    // -----------------------------------------------------------------------
    bool takeLevelBulbNear(Physics *ph, const Vec3 &at, float reach, double nowMs,
                           bool shatter) {
        int best = -1;
        float bd = reach * reach;
        for (size_t i = 0; i < levelBulbs_.size(); ++i) {
            const Vec3 d = levelBulbs_[i] - at;
            const float q = lengthSq(d);
            if (q < bd) {
                bd = q;
                best = int(i);
            }
        }
        if (best < 0) return false;
        const Vec3 bulb = levelBulbs_[size_t(best)];
        levelBulbs_.erase(levelBulbs_.begin() + best);

        // ---- the glass, out of the display grid --------------------------
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        if (levelDisplay_.a.empty() || levelGlassMtl_ == mat::AIR) {
            rebuildTlas();
            return true;
        }
        const int cx = int((bulb.x - kLevelAtX) / VOXEL_M);
        const int cy = int((bulb.y - kLevelAtY) / VOXEL_M);
        const int cz = int((bulb.z - kLevelAtZ) / VOXEL_M);
        // The model's own size plus a voxel, which is the honest bound on how
        // far its glass can reach from the position that was recorded for it.
        const int r = maxi(2, maxi(levelBulbAsset_.sx, maxi(levelBulbAsset_.sy,
                                                            levelBulbAsset_.sz)));
        const int n = r * 2 + 1;
        std::vector<uint8_t> vol(size_t(n) * size_t(n) * size_t(n), mat::AIR);
        int got = 0;
        int b0x = levelBX_, b1x = -1, b0z = levelBZ_, b1z = -1;
        for (int y = maxi(0, cy - r); y <= mini(SY - 1, cy + r); ++y)
            for (int z = maxi(0, cz - r); z <= mini(SZ - 1, cz + r); ++z)
                for (int x = maxi(0, cx - r); x <= mini(SX - 1, cx + r); ++x) {
                    const size_t k = size_t(x) + size_t(z) * size_t(SX) +
                                     size_t(y) * size_t(SX) * size_t(SZ);
                    if (levelDisplay_.a[k] != levelGlassMtl_) continue;
                    vol[size_t(x - cx + r) + size_t(z - cz + r) * size_t(n) +
                        size_t(y - cy + r) * size_t(n) * size_t(n)] = levelDisplay_.a[k];
                    levelDisplay_.a[k] = mat::AIR;
                    ++got;
                    const int bx = x / kLevelBlockVox, bz = z / kLevelBlockVox;
                    b0x = mini(b0x, bx);
                    b1x = maxi(b1x, bx);
                    b0z = mini(b0z, bz);
                    b1z = maxi(b1z, bz);
                }
        if (got) {
            const auto t0 = std::chrono::steady_clock::now();
            int blocks = 0;
            for (int bz = maxi(0, b0z); bz <= mini(levelBZ_ - 1, b1z); ++bz)
                for (int bx = maxi(0, b0x); bx <= mini(levelBX_ - 1, b1x); ++bx) {
                    meshLevelBlock(bz * levelBX_ + bx);
                    ++blocks;
                }
            size_t total = 0;
            for (size_t t : levelBlockTris_) total += t;
            levelTris_ = total;
            if (carveLog) {
                std::printf("v2: bulb out -- %d glass voxels, %d block(s) re-meshed in "
                            "%.1f ms\n", got, blocks,
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
                std::fflush(stdout);
            }
        }

        // ---- ...AND IT FALLS, IN PIECES ----------------------------------
        //
        // SPLIT INTO OCTANTS about the glass's own middle, which is v1's
        // phShatter and the same split shatterFlyer uses on a corpse -- each
        // piece is a contiguous corner of the thing rather than confetti. A
        // bulb is small, so most octants are a voxel or two and kMinBodyVoxels
        // being 1 is what lets them all be real.
        //
        // A LITTLE OUTWARD, NOT A LAUNCH. Glass that drops straight down in
        // eight touching pieces reads as one object falling; a metre a second
        // apart is enough to read as breaking, and small enough that nothing
        // is thrown across the room.
        if (shatter && ph && got > 0) {
            for (int oct = 0; oct < 8; ++oct) {
                std::vector<uint8_t> part(vol.size(), mat::AIR);
                int pn = 0;
                for (int y = 0; y < n; ++y)
                    for (int z = 0; z < n; ++z)
                        for (int x = 0; x < n; ++x) {
                            const size_t k = size_t(x) + size_t(z) * size_t(n) +
                                             size_t(y) * size_t(n) * size_t(n);
                            if (vol[k] == mat::AIR) continue;
                            const int o = ((x >= r) ? 1 : 0) | ((y >= r) ? 2 : 0) |
                                          ((z >= r) ? 4 : 0);
                            if (o != oct) continue;
                            part[k] = vol[k];
                            ++pn;
                        }
                if (!pn) continue;
                const Vec3 centre{kLevelAtX + (float(cx) + 0.5f) * VOXEL_M,
                                  kLevelAtY + (float(cy) + 0.5f) * VOXEL_M,
                                  kLevelAtZ + (float(cz) + 0.5f) * VOXEL_M};
                const Vec3 away{((oct & 1) ? 1.0f : -1.0f) * kBulbBurstMs,
                                ((oct & 2) ? 0.6f : -0.2f) * kBulbBurstMs,
                                ((oct & 4) ? 1.0f : -1.0f) * kBulbBurstMs};
                const Vec3 spin{0.0f, 0.0f, 0.0f};
                markLeftLying(spawnDebris(*ph, part, n, centre, away, spin, nowMs, 0.0f,
                                          nullptr, uint8_t(kDebrisStone), kArrowAbsorbM),
                              kLevelChipLifeMs);   // see carveLevelToBody
            }
        }
        rebuildTlas();
        return true;
    }

    // The lamp in hand takes one whole -- see takeLevelBulbNear.
    bool removeLevelBulbNear(const Vec3 &at, float reach) {
        return takeLevelBulbNear(nullptr, at, reach, 0.0, /*shatter=*/false);
    }

    // Throw the level's geometry away and mesh it again. The only way to see an
    // edit: the map is ONE structure, so there is no smaller unit to rebuild.
    void rebuildLevelBlas() {
        for (size_t i = 0; i < levelBlocks_.size(); ++i) {
            if (levelBlockTri_[i] == TriPool::kInvalid) continue;
            pool_.release(levelBlockTri_[i], levelBlockTris_[i]);
            levelBlockTri_[i] = TriPool::kInvalid;
        }
        levelBlocks_.clear();
        levelBlockTri_.clear();
        levelBlockTris_.clear();
        levelDisplay_.a.clear();
        levelDisplay_.a.shrink_to_fit();
        levelTris_ = 0;
        buildLevelBlas();
        rebuildTlas();
    }

    // Give the level its own material table, once the kit is loaded. Called
    // from App's start-up; see buildLevelPalette for why the order is fixed.
    bool prepareLevelPalette() { return buildLevelPalette(); }

    // Take a chip out of the map -- see the definition for why the level needs
    // its own carve and cannot use dig() or carveModel().
    bool carveLevelAt(const Vec3 &at, int radiusVox, std::vector<uint8_t> *spoil = nullptr,
                      int *spoilN = nullptr, Vec3 *spoilAt = nullptr) {
        return carveLevel(at, radiusVox, spoil, spoilN, spoilAt);
    }

    // -- ...AND THE SAME CUT WITH THE RULE BUILT IN ----------------------
    //
    // CARVE AND BODY IN ONE CALL. See the RULE block over kMinBodyVoxels: the
    // level is the path that has actually regressed -- its spawn line was
    // deleted by a concurrent edit and the map went back to holes that nothing
    // comes out of -- and the reason it could regress is that the carve and
    // the spawn were two statements. They are one now, so the only way to cut
    // the map without producing a body is to stop calling this, which is not a
    // thing an edit does by accident.
    //
    // Returns the debris slot, or -1 if nothing was cut. `nVox` reports what
    // came out so a caller (and --fire-frame) can tell "the round missed" from
    // "the round cut and the voxels vanished" -- two failures that look
    // identical from in front of the wall.
    int carveLevelToBody(Physics &ph, const Vec3 &at, int radiusVox, double nowMs,
                         float absorbR, int *nVox = nullptr, Vec3 *atOut = nullptr) {
        if (nVox) *nVox = 0;
        // NOT clearHangReport() -- see lastHangVox_. Clearing per carve is
        // how the first shot's 2 x 2 x 25 rod hid behind the second shot's
        // four specks for three builds. The caller clears it when it wants
        // a fresh count; a carve only adds to it.
        std::vector<uint8_t> vol;
        int n = 0;
        Vec3 spoilAt{0.0f, 0.0f, 0.0f};
        // -- THE CHIP WANTS A SLOT TOO --------------------------------------
        //
        // (user 2026-09-18: "the pole just deleted itself".)
        //
        // Same reason the hanger sweep reserves one -- see the block in
        // dropLevelHangers. A full pool is the ordinary state of this map after
        // a magazine and a half, and every round after that was cutting a hole
        // that nothing came out of. Retiring the oldest settled chip to make
        // room is what the player would expect anyway: the litter of a firefight
        // is a rolling window, not the first sixty-four holes for ever.
        //
        // IT STILL CARVES IF THERE IS NO ROOM, which is the one place this
        // differs from the hanger. A hanger refused a slot is left standing and
        // the map is unchanged; a round refused one has to leave its mark or
        // the weapon stops marking walls -- reported three times -- and the
        // seven voxels it costs are the bite itself rather than a piece of
        // architecture.
        if (!carveLevel(at, radiusVox, &vol, &n, &spoilAt) || n <= 0) return -1;
        int solid = 0;
        for (uint8_t v : vol)
            if (v != mat::AIR) ++solid;
        // ASKED AFTER THE CARVE BECAUSE THE SIZE IS THE ARGUMENT. What a piece
        // may evict is anything no bigger than itself (see makeRoomForBodies),
        // and how big this chip is is not known until the bite has been taken.
        // Unlike the hanger, the carve above is not conditional on finding a
        // slot -- see the note at the top of this function.
        if (debrisFree() < 1) makeRoomForBodies(ph, 1, solid);
        if (nVox) *nVox = solid;
        if (atOut) *atOut = spoilAt;
        // Dropped where it came from with no throw and no spin, stopped by the
        // map it was cut out of, and collected by walking up to it.
        const Vec3 kNoVel{0.0f, 0.0f, 0.0f};
        const int slot = spawnDebris(ph, vol, n, spoilAt, kNoVel, kNoVel, nowMs, 0.0f, nullptr,
                                     uint8_t(kDebrisStone), absorbR);
        // -- NOBODY PICKS ANYTHING UP IN HERE ----------------------------
        //
        // (user 2026-09-17: "in the fps map, dont have the player absorb the
        //  chunks caused by the bullet. just leave them there. have them be
        //  removed after 100 seconds.")
        //
        // APPLIED AT THE SPAWN, NOT AT THE CALL SITE, and for the reason the
        // carve and the spawn were fused in the first place: a rule that
        // depends on every caller remembering a second line is a rule that has
        // already been deleted once here. Every body the LEVEL makes gets this
        // -- the chips, the hangers and the bulb glass -- because the level has
        // no gathering mechanic at all: the kit is stowed on the way in and
        // only the rifle is in the hand, so a chunk flying into the player was
        // never collecting anything, it was just disappearing.
        //
        // `absorbR` stays on the signature because the argument is what the
        // WOOD's callers use, and this function is shared.
        markLeftLying(slot, kLevelChipLifeMs);
        // -- ...AND WHATEVER THE CUT LEFT STANDING ON AIR -------------------
        //
        // THE OTHER HALF OF THE RULE. kMinBodyVoxels' note says voxels that
        // leave the static world become a body; this is the case where the
        // voxels that have to become a body are the ones the carve did NOT
        // touch. Cut a light pole at the ankle and every voxel above the cut
        // is still in the grid, still drawn, and no longer attached to
        // anything -- which is the NOTHING FLOATS rule, broken by the one
        // static thing that had no sweep to catch it.
        //
        // INSIDE carveLevelToBody, so it cannot be forgotten by a caller for
        // the same reason the spawn above cannot be: they are one act.
        dropLevelHangers(ph, at, radiusVox, nowMs);
        return slot;
    }

    // The level AS A COLLIDER. One Solid over the whole asset, borrowing the
    // voxels rather than copying them -- the same contract a ModelTemplate has
    // with the placements that share it, and safe for the same reason: these
    // arrays are filled once at load and never touched again.
    //
    // yaw 0 and the translation at the model's own corner, so solidModelSpace
    // is the identity minus an offset; there is one of these and it has no
    // reason to be turned.
    bool levelSolid(Solid *out) const {
        if (!levelLoaded_ || levelVol_.empty()) return false;
        Solid s;
        s.interior = true;
        s.standable = true;
        s.cx = kLevelAtX + float(levelAsset_.sx) * VOXEL_M * 0.5f;
        s.cz = kLevelAtZ + float(levelAsset_.sz) * VOXEL_M * 0.5f;
        s.hx = float(levelAsset_.sx) * VOXEL_M * 0.5f;
        s.hz = float(levelAsset_.sz) * VOXEL_M * 0.5f;
        s.top = kLevelAtY + float(levelAsset_.sy) * VOXEL_M;
        s.col = levelColTop_.data();
        s.msx = int16_t(levelAsset_.sx);
        s.msz = int16_t(levelAsset_.sz);
        s.vol = levelVol_.data();
        s.vsy = int16_t(levelAsset_.sy);
        s.yaw = 0;
        s.tx = kLevelAtX;
        s.tz = kLevelAtZ;
        s.baseY = kLevelAtY;
        *out = s;
        return true;
    }

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
    // What of it the resident ring actually occupies. Printed beside the
    // capacity at load, because a pool sized off a formula is only right
    // for as long as nobody changes what a chunk meshes to.
    size_t poolUsedBytes() const { return pool_.usedUnits() * sizeof(uint16_t); }

    // -----------------------------------------------------------------------
    bool build(const ref<Device> &device, RenderContext *ctx) {
        device_ = device;
        ctx_ = ctx;

        // The pool has to hold the resident ring plus the models. A chunk over
        // this terrain meshes to roughly 200k triangles; kChunkUnits is that
        // with a margin, and the pool grows if the ground turns out to be
        // rougher than that. Models are a rounding error beside it.
        //
        // THE DISC'S AREA, NOT THE SQUARE'S -- and until 2026-09-18 this said
        // so in a comment while the code below multiplied (2R+1)^2. It was
        // over-allocating by 21%, which is 81 MB at the default and 250 MB at
        // --view 24, none of which could ever be reached because rering will
        // not make those chunks resident.
        //
        // THE OVER-ALLOCATION WAS ALSO THE ONLY SLACK, so replacing it with an
        // exact count needs the slack put back explicitly. kPoolSlack is that,
        // and it is a real margin rather than an artefact of a wrong formula:
        // the pool grows by copying itself device-side, so the size is chosen
        // to make that rare, not impossible.
        const size_t chunks = discChunks(viewChunks);
        pool_.init(device_, size_t(double(chunks * kChunkUnits) * kPoolSlack) + (8u << 20));
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
        loadFruit();
        palette.deriveGroundFromTrees();
        // AFTER deriveGroundFromTrees, which reads the TREES' greens and browns
        // off the table and would otherwise have a building's stucco in the
        // sample -- the same ordering hazard HeldItem::prewarmColors is placed
        // around. Before uploadMaterials, because these are entries.
        loadLevel();
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
        for (const ModelTemplate &t : fruit_)
            mesher_.fruitFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ, t.col.baseCX, t.col.baseCZ});
        for (const ModelTemplate &t : pines_) mesher_.pinePerch.push_back(t.perches);
        for (const ModelTemplate &t : pines_) mesher_.pineHivePerch.push_back(t.hivePerches);
        // THE CONES' ANCHORS, RE-SORTED BY ANGLE -- see fruitAnchors for why
        // a crop wants an order the cones do not, and why it gets its own
        // list instead of permuting theirs. Built for every tree so the
        // index stays positional into one array; only the oaks ever read it.
        for (const ModelTemplate &t : pines_)
            mesher_.oakFruitPerch.push_back(fruitAnchors(t.perches, t.sx, t.sz));
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
        // ONE GENERATION, so a backlog is spread over frames rather than
        // spent on whichever one the streamer happens to go idle on. See
        // drainCompactions.
        if (drainCompactions(false, kDrainPerFrame)) changed = true;
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
        // -- AND THE ADOPT BUDGET STAYS PUT. MEASURED, TWICE ----------------
        //
        // This lifted the budget by the number of outstanding edit chunks, on
        // the theory that a till's disc was arriving a slab at a time. Both
        // halves of that were wrong and the measurements say so:
        //
        //   * a hoe bite re-meshes ONE chunk, sometimes two (--swing-log prints
        //     it now) -- which already fits in a budget of two, so there was
        //     never a queue here to jump;
        //   * lifting it adopted several chunks on one frame, and every adopt
        //     feeds the BLAS compaction drain. That drain is deliberately
        //     budgeted (see kDrainPerFrame and the note in walking-hitches)
        //     because it was 61 ms unbounded -- and it went straight back
        //     there: 28.9, 40.2 and 60.7 ms drain spikes appeared on the worst
        //     frames the moment this shipped.
        //
        // The queue jump in remesh() is the half that was worth keeping: it
        // costs nothing and puts an edit in front of STREAMING, which is a real
        // queue with real depth. `urgent_` is still tracked for that.
        while (int(batch.size()) < buildBudget && mesher_.take(&b)) {
            requested_.erase(chunkKey(b.cx, b.cz));
            urgent_.erase(chunkKey(b.cx, b.cz));
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
    // -----------------------------------------------------------------------
    // `ri0..rj1` LIMITS THE RE-SAMPLE TO A WORLD-VOXEL RECT.
    //
    // (user 2026-09-17: "keep pursuing the hoe glitch".)
    //
    // A hoe bite is 28 voxels across -- seven of these cells by seven, out of
    // 25,600 -- and the patch was re-sampled WHOLE for it because the dirty
    // flag could not say where. Handed the box (see markGroundDirty), the loop
    // walks only the rows and columns that overlap it and leaves the rest of
    // `out` exactly as the caller had it.
    //
    // THE CALLER MUST HAVE THE PREVIOUS PATCH, which is the one precondition:
    // this writes a subset now, so a partial call is only valid when `out`
    // already holds the last full sample AT THE SAME ORIGIN. App::
    // rebuildGroundPatch checks the origin and asks for everything when it has
    // moved. Defaulted to the whole world, so every other caller is unchanged.
    void groundPatch(int16_t *out, int n, int i0, int j0, int step, int ri0 = INT_MIN,
                     int rj0 = INT_MIN, int ri1 = INT_MAX, int rj1 = INT_MAX) const {
        TerrainMemo memo;
        long long haveKey = 0;
        bool have = false;
        std::shared_ptr<const ChunkEdits> ce;
        // THE TOP OF ONE COLUMN, WITH ITS OWN CHUNK'S EDITS.
        //
        // ITS OWN, WHICH IT WAS NOT. The cell scan below walks up to `step`
        // voxels past the sampled column and read the SAMPLE's edit map the
        // whole way, so every column of the cell that crossed a chunk boundary
        // was asked the wrong map: ce->voxel came back false, the scan stopped
        // at once, and the generated height -- the ground as it was before
        // anybody dug -- was reported for a column that may have a shaft in it.
        // Within `step` voxels of a chunk seam, that is a pit the solver cannot
        // see, which is the phantom lid this whole function exists to remove.
        //
        // The one-entry cache is what made the old code want a single fetch:
        // EditStore::get takes a lock. A cell is walked in row-major order, so
        // a cache of one turns the 4 x 4 into one or two fetches, and the
        // common case -- a world nobody has dug in -- is a null every time.
        // WHICH CHUNK'S EDITS COVER THIS COLUMN, through the one-entry cache.
        // Split out of topAt so the question "has anything here been dug at
        // all" can be asked WITHOUT evaluating the terrain -- see the skip in
        // the loop below, which is the whole of this function's cost.
        auto editsFor = [&](int qi, int qj) -> const std::shared_ptr<const ChunkEdits> & {
            const int qcx = floorDiv(qi, CHUNK_VOX), qcz = floorDiv(qj, CHUNK_VOX);
            const long long k = chunkKey(qcx, qcz);
            if (!have || k != haveKey) {
                ce = mesher_.edits.get(qcx, qcz);
                haveKey = k;
                have = true;
            }
            return ce;
        };
        // ...AND THE TOP OF A COLUMN WHOSE GENERATED HEIGHT IS ALREADY KNOWN.
        // It was computed inside here before, which meant every caller that
        // also needed the generated height -- and both of them do -- paid for
        // several octaves of noise twice over the same column.
        auto topWith = [&](int qi, int qj, int gh) {
            const std::shared_ptr<const ChunkEdits> &e = editsFor(qi, qj);
            if (!e) return gh;
            int y = gh;
            for (int d = 0; d < kUndermineDepthVox; ++d, --y) {
                uint8_t m = mat::AIR;
                if (!e->voxel(qi, qj, y, &m) || m != mat::AIR) break;
            }
            return y;
        };
        // The cell range that overlaps the rect. A cell `i` covers world voxels
        // [i0 + i*step, i0 + i*step + step - 1], so it is in if its far edge
        // reaches ri0 and its near edge does not pass ri1. Clamped, and empty
        // when the rect misses the patch entirely.
        auto lo = [&](int r, int base) {
            if (r == INT_MIN) return 0;
            const long long v = (long long)(r) - base - step + 1;
            const long long q = v >= 0 ? v / step : -((-v + step - 1) / step);
            return int(q < 0 ? 0 : (q > n - 1 ? n : q));
        };
        auto hi = [&](int r, int base) {
            if (r == INT_MAX) return n - 1;
            const long long v = (long long)(r) - base;
            const long long q = v >= 0 ? v / step : -((-v + step - 1) / step);
            return int(q < 0 ? -1 : (q > n - 1 ? n - 1 : q));
        };
        const int ci0 = lo(ri0, i0), ci1 = hi(ri1, i0);
        const int cj0 = lo(rj0, j0), cj1 = hi(rj1, j0);
        for (int j = cj0; j <= cj1; ++j)
            for (int i = ci0; i <= ci1; ++i) {
                const int wi = i0 + i * step, wj = j0 + j * step;
                const int h = terrain.heightVox(wi, wj, memo);
                // -- THE LOWEST GROUND IN THE CELL, NOT THE GROUND AT ONE
                //    CORNER OF IT (user 2026-09-14: "digging up dirt seems to
                //    teleport the chunks to the surface") ------------------
                //
                // THE PATCH IS SAMPLED EVERY `step` VOXELS AND A DIG IS THREE
                // ACROSS. At step 4 a pit is narrower than the spacing, so the
                // sampled columns walk straight past it and the solver keeps a
                // flat lid over a hole that is plainly there -- which is the
                // exact fault the note above this function says was FIXED. It
                // was fixed for the column that happens to be sampled and for
                // no other.
                //
                // It only started throwing things when the dirt got five
                // voxels deeper: a soil pit used to bottom out on stone within
                // half a metre, which is inside updateDebris' own 0.5 m of
                // slack, and now it is over a metre. A chip born a metre under
                // a phantom floor is ejected through it.
                //
                // SO THE CELL'S MINIMUM, and only where an edit actually is.
                // The scan is step*step columns of an edit map that is empty
                // almost everywhere -- `ce` is null for every chunk nobody has
                // dug in, which is nearly all of them -- so the common case
                // costs one null test more than it did.
                //
                // MINIMUM RATHER THAN THE SAMPLE, because the two ways of being
                // wrong are not equal: a floor that is too LOW lets a body fall
                // into a pit it could have rested beside, and a floor that is
                // too HIGH throws it out of a pit it is standing in. Only one
                // of those looks like teleporting.
                int y = topWith(wi, wj, h);
                // -- THE CELL'S MINIMUM, BUT ONLY WHERE SOMETHING WAS DUG ----
                //
                // (user 2026-09-15: "when the life dies and turns red, it clips
                // straight through the ground".)
                //
                // THE MINIMUM WAS TAKEN OVER EVERY CELL AND THAT IS WHY BODIES
                // SANK. This scan exists for one thing: a pick bite is 3 voxels
                // and the samples are 4 apart, so a pit can fall between them
                // and the solver keeps a lid over a hole that is plainly there.
                // Natural ground has no such problem -- the samples describe it
                // exactly -- but taking the minimum anyway hands back the
                // LOWEST corner of every cell, which on a slope is the bottom
                // of a 0.4 m step. MEASURED: a corpse resting on a hillside sat
                // 0.42 m inside it, which is most of the animal.
                //
                // So a neighbour only counts if it has actually been DUG --
                // its edited top is below what the generator says. That keeps
                // the pit, which is the whole point, and leaves every slope in
                // the world alone.
                // -- AND IT IS SKIPPED WHERE NOTHING HAS BEEN DUG ----------
                //
                // (user 2026-09-15: "can you investigate hitching in the game
                // ... just as im walking around the environment its hitching".)
                //
                // THIS SCAN WAS THE HITCH, AND FOR UNDUG GROUND IT WAS PROVABLY
                // DEAD WORK. A neighbour only counts if its top is BELOW what
                // the generator says -- the test two lines down -- and where
                // there are no edits, topWith returns exactly the generated
                // height. So the branch could never be taken; it simply
                // evaluated the terrain first and then threw the answer away.
                //
                // Fifteen neighbours on each of 25,600 samples is 384,000
                // octave-noise evaluations per rebuild, all of it for the rare
                // cell with a pit in it. MEASURED on the frame the player
                // crosses the patch boundary, which is the whole of the fault:
                //
                //     sampling the patch   65.8 ms      the PhysX cook   0.25 ms
                //
                // -- that is four dropped frames at 60 fps, every time, and the
                // old note above ("costs one null test more than it did") was
                // describing the null test it does AFTER the noise rather than
                // instead of it.
                //
                // THE FOUR CORNERS, because a cell may straddle a chunk seam
                // and it is the CHUNK that owns an edit map. Clean chunks --
                // which is nearly every chunk in the world, for ever -- answer
                // this in four integer divisions and a cached pointer test.
                // ...AND AGAINST THE EDITED BOX, NOT MERELY THE CHUNK. Asking
                // "has this chunk any edits" turned the sweep back on for every
                // cell of a chunk the moment one hoe bite landed in it -- see
                // ChunkEdits::touchesRect, which records what that measured.
                // A cell is 4x4 voxels and a chunk is hundreds of them, so the
                // box is the difference between sweeping the bite and sweeping
                // the wood around it.
                const int wi1 = wi + step - 1, wj1 = wj + step - 1;
                auto dugNear = [&](int qi, int qj) {
                    const std::shared_ptr<const ChunkEdits> &e = editsFor(qi, qj);
                    return e && e->touchesRect(wi, wj, wi1, wj1);
                };
                const bool anyDug = dugNear(wi, wj) || dugNear(wi1, wj) || dugNear(wi, wj1) ||
                                    dugNear(wi1, wj1);
                if (anyDug)
                    for (int dj = 0; dj < step; ++dj)
                        for (int di = 0; di < step; ++di) {
                            if (!di && !dj) continue;
                            const int qi = wi + di, qj = wj + dj;
                            // -- THE CHEAP HALF FIRST, AND IT IS WHY TILLING
                            //    HITCHED (user 2026-09-17: "when tilling the
                            //    land, the terrain glitches out").
                            //
                            // `anyDug` above turns this scan on for a whole
                            // CHUNK, and a till makes a chunk dirty for as long
                            // as the bed lasts -- so every ground-patch rebuild
                            // near tilled earth paid the full fifteen-neighbour
                            // sweep again, and the hoe sets groundDirty_ on
                            // every swing. MEASURED, walking while tilling:
                            // worst frame 68.5 ms with 25-42 ms of it in the
                            // physics rebuild, against 26-33 ms for the same
                            // walk without the hoe.
                            //
                            // AND IT WAS DEAD WORK, for the same reason the
                            // skip above was. This loop can only lower `y` when
                            // a column's EDITED top is under its generated one,
                            // which needs an edit entry in that column -- and a
                            // column with no edit span has none by definition.
                            // So the span is asked first: a hash lookup against
                            // a map that is empty for all but the eighty-one
                            // columns a bite actually turned, while heightVox
                            // under it is several octaves of noise.
                            //
                            // A TILL CANNOT FALL BETWEEN THE SAMPLES ANYWAY --
                            // it is twenty-eight voxels across and they are four
                            // apart -- so the sweep never had anything to find
                            // there. The narrow pick bite this scan exists for
                            // still works: its columns carry spans.
                            const std::shared_ptr<const ChunkEdits> &qe = editsFor(qi, qj);
                            if (!qe) continue;
                            if (qe->col.find(ChunkEdits::ckey(qi, qj)) == qe->col.end()) continue;
                            // ONCE, and used for both questions below. It was
                            // evaluated twice: once inside topAt and again for
                            // the dug test.
                            const int qh = terrain.heightVox(qi, qj, memo);
                            const int qy = topWith(qi, qj, qh);
                            if (qy >= y) continue;
                            // Dug, or merely lower ground? The generator answers
                            // for nothing anybody has touched.
                            if (qy >= qh) continue;
                            y = qy;
                        }
                // Higher ground inside the cell is not what this is looking
                // for: the minimum is the floor, and a rise within one cell is
                // the patch's own coarseness and was always there.
                //
                // `h` used to be dead here -- computed, then `(void)h` -- while
                // topAt evaluated the very same column again a line later. It
                // is the sample's generated height and it now feeds both the
                // column's own top and nothing else needs asking twice.
                out[size_t(i) + size_t(j) * size_t(n)] = int16_t(y);
            }

        // -- WIDENING THE PIT WAS TRIED, MEASURED, AND TAKEN OUT AGAIN ------
        //
        // The minimum above is what this function says it is: the lowest ground
        // in the cell. A pit narrower than the sample spacing is therefore a
        // DIMPLE -- one sample down, its neighbours still at the surface -- and
        // the fix that suggests itself is to spread the depression onto those
        // neighbours so the hole has a flat bottom a body can rest on.
        //
        // IT WAS WRITTEN AND IT MADE THINGS WORSE. Dilating by one sample makes
        // the smallest possible hole 1.2 m wide in the solver, and everything
        // that digs at the SURFACE leaves its spoil lying inside that radius:
        // measured with --soil-test, a chip set down 0.4 m from a pit sank 2.19
        // m into ground that was plainly still there, and one 1.6 m away sank a
        // third of a metre. That is what "digging dirt voxels is very glitchy"
        // looks like, and a pick working underground never shows it.
        //
        // AND IT WAS NOT BUYING ANYTHING. The shaft ejection it was written for
        // is fixed by the BACKSTOP alone -- see the footprint probe in
        // updateDebris, which is what actually stopped a chip being teleported
        // to the rim. With the dilation removed --shaft-test still leaves the
        // spoil 4.3 m down, and --soil-test's chips rest on the surface at
        // every distance. Two measurements, one line of code, and the line is
        // the one that had to go.
    }

    // Has anything been dug since the patch was last built? Read-and-clear:
    // the floor the solver stands on has to be rebuilt when the ground under
    // it changes, and a dig is the only thing that changes it.
    // A PEEK, because the decision to rebuild and the act of clearing the flag
    // are not the same moment any more -- see App::maybeRebuildGroundPatch.
    bool groundDirty() const { return groundDirty_; }

    bool takeGroundDirty() {
        const bool d = groundDirty_;
        groundDirty_ = false;
        return d;
    }

    // -----------------------------------------------------------------------
    // ...AND *WHERE* IT GOT DIRTY, IN WORLD VOXELS.
    //
    // (user 2026-09-17, still on the hoe: "keep pursuing the hoe glitch".)
    //
    // THE FLAG ALONE COSTS A WHOLE PATCH. rebuildGroundPatch re-samples 25,600
    // cells because the flag cannot say which of them moved, and a hoe bite is
    // twenty-eight voxels across -- seven cells by seven. So every swing paid
    // for the wood when it had changed a doormat. With the sweep fixed that is
    // down to ~4.4 ms a swing, which is no longer a stall but is still the
    // largest thing a till does.
    //
    // UNIONED, NOT REPLACED, because several edits can land between two
    // rebuilds -- a till, the revert of an older bed, and a dig are all
    // possible in one frame and all of them must be re-sampled.
    void markGroundDirty(int i0, int j0, int i1, int j1) {
        if (!groundDirty_) {
            gdI0_ = i0;
            gdJ0_ = j0;
            gdI1_ = i1;
            gdJ1_ = j1;
        } else {
            gdI0_ = mini(gdI0_, i0);
            gdJ0_ = mini(gdJ0_, j0);
            gdI1_ = maxi(gdI1_, i1);
            gdJ1_ = maxi(gdJ1_, j1);
        }
        groundDirty_ = true;
    }
    // The union since the last take, or false if nothing is pending. Read
    // BEFORE takeGroundDirty, which is what clears the flag.
    bool groundDirtyBox(int *i0, int *j0, int *i1, int *j1) const {
        if (!groundDirty_) return false;
        *i0 = gdI0_;
        *j0 = gdJ0_;
        *i1 = gdI1_;
        *j1 = gdJ1_;
        return true;
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

    // THE ANSWER MOVED to TerrainProbe, the same way terrainSolidAt's did and
    // for the same reason: the player walks on this now and render/player.h
    // cannot reach into this class. One implementation, so the floor the body
    // stands on and the floor the mesher draws cannot disagree about a hole.
    int terrainTopAt(int i, int j, TerrainMemo &memo) const {
        TerrainProbe probe(&terrain, &mesher_.edits, memo);
        return probe.topVox(i, j);
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

    // -----------------------------------------------------------------------
    // ...AND THE SCATTER THE BITE LEFT IN THE AIR.
    //
    // (user 2026-09-14: "flowers are still floating sometimes. investigate and
    // fix this.")
    //
    // WHY dropUndermined NEVER SAW THEM. That function walks `c.solids`, and a
    // flower is not in it -- adoptMany pushes a Solid only for placements that
    // are NOT walkThrough, which is kinds 0, 1 and 3. Flowers (2), cones (4)
    // and hives (5) are drawn and walked through, so they have no collider, so
    // they are invisible to every query built on colliders. A mushroom falls
    // when you dig under it and the flower beside it does not, and the reason
    // is a list it was never on.
    //
    // KINDS 2 AND 4 ONLY. The mushroom is a solid and dropUndermined already
    // has it; running both over the same placement would fell it twice. A hive
    // hangs in a crown and is dropped with its tree by dropHangers.
    //
    // IT FALLS RATHER THAN VANISHING, which is the house rule: anything
    // severable from the static grid falls. The body is built from the
    // template's own voxels -- the same route a killed animal's corpse takes --
    // so what lands is the flower, in its own colours.
    //
    // PERMANENT, via fellSlots_. Chunk decor is regenerated from the scatter on
    // every adopt (see reapplyDamage), so a flower that is merely masked off
    // stands straight back up the next time the chunk comes back.
    // -----------------------------------------------------------------------
    // Set by --soil-test while it is asking why a flower did not fall.
    bool undermineLog = false;
    int dropScatterUndermined(Physics &ph, const Vec3 &at, float radiusM, double nowMs) {
        if (!ph.available()) {
            if (undermineLog) std::printf("  [under] SKIPPED -- no physics\n");
            return 0;
        }
        const float span = radiusM + 2.0f;
        if (undermineLog)
            std::printf("  [under] called at (%.2f, %.2f, %.2f) span %.2f\n", double(at.x),
                        double(at.y), double(at.z), double(span));
        const int x0 = floorDiv(int(std::floor((at.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(std::floor((at.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(std::floor((at.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(std::floor((at.z + span) / VOXEL_M)), CHUNK_VOX);
        TerrainProbe probe(&terrain, &mesher_.edits);
        const float r2 = span * span;
        int dropped = 0;
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                const long long key = chunkKey(cx, cz);
                auto it = chunks_.find(key);
                if (it == chunks_.end()) continue;
                Chunk &c = it->second;
                for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
                    const DecorAt &q = c.decorAt[i];
                    // -- THE CAP TOO, WHICH IT NEVER WAS ------------------
                    //
                    // (user 2026-09-16: "the terrain glitches out" -- this is
                    // the half of that report which is not the hoe.)
                    //
                    // FLOWER, CAP AND CONE ARE ONE SET EVERYWHERE ELSE. Both
                    // scatterShownNear and hideScatterOn test
                    // `kind != 2 && kind != 3 && kind != 4`, and this one line
                    // left the CAP out -- so a mushroom whose ground was dug
                    // away stayed hanging in the air, and NOTHING FLOATS is the
                    // rule this engine has been told twice.
                    //
                    // IT HID BEHIND THE MISMATCH. --soil-test counts what is
                    // still drawn with scatterShownNear, which DOES include the
                    // cap, so the failure read as "a flower did not fall" and
                    // the flower code looked correct because it was. What gave
                    // it away was the diagnostic staying silent: the item was
                    // being skipped before the support test ever ran.
                    if (q.kind != 2 && q.kind != 3 && q.kind != 4) continue;
                    if (!c.decorDesc[i].instanceMask) continue;   // already down or hidden
                    const float dx = q.midX() - at.x, dz = q.midZ() - at.z;
                    if (dx * dx + dz * dz > r2) continue;
                    // IS THERE ANYTHING UNDER IT. The placement's y is the
                    // model's own bottom, so the ground is the voxel below --
                    // and three voxels of slack, because a stamp sits in the
                    // first air cell above a surface that a slope can put half
                    // a voxel either way.
                    const int qi = int(std::floor(q.midX() / VOXEL_M));
                    const int qj = int(std::floor(q.midZ() / VOXEL_M));
                    const int qy = int(std::floor(q.y / VOXEL_M));
                    bool held9 = false;
                    int heldBy = 0, heldAt = 0;
                    for (int d = 1; d <= 3 && !held9; ++d)
                        if (probe.material(qi, qj, qy - d) != mat::AIR) {
                            held9 = true;
                            heldBy = int(probe.material(qi, qj, qy - d));
                            heldAt = d;
                        }
                    if (held9) {
                        if (undermineLog)
                            std::printf("  [under]   HELD kind %d at vox (%d,%d,%d) by material "
                                        "%d, %d below\n",
                                        int(q.kind), qi, qj, qy, heldBy, heldAt);
                        continue;
                    }
                    // ---- it is standing on nothing: down it goes ----------
                    const ModelTemplate &t = templateFor(q.kind, int(q.index));
                    c.decorDesc[i].instanceMask = 0;
                    fellSlots_.insert({key, int(i)});
                    ++dropped;
                    if (undermineLog)
                        std::printf("  [under]   DROPPED kind %d slot %d, mask now %u\n",
                                    int(q.kind), int(i),
                                    unsigned(c.decorDesc[i].instanceMask));
                    if (t.volume.empty() || t.sx <= 0) continue;   // hidden, at least
                    const int n = maxi(maxi(t.sx, t.sy), t.sz);
                    std::vector<uint8_t> cube(size_t(n) * size_t(n) * size_t(n), mat::AIR);
                    const int ox = (n - t.sx) / 2, oy = (n - t.sy) / 2, oz = (n - t.sz) / 2;
                    for (int y = 0; y < t.sy; ++y)
                        for (int z = 0; z < t.sz; ++z)
                            for (int x = 0; x < t.sx; ++x) {
                                const uint8_t v = t.volume[size_t(x) + size_t(z) * size_t(t.sx) +
                                                           size_t(y) * size_t(t.sx) * size_t(t.sz)];
                                if (v == mat::AIR) continue;
                                cube[size_t(x + ox) + size_t(z + oz) * size_t(n) +
                                     size_t(y + oy) * size_t(n) * size_t(n)] = v;
                            }
                    // AT ITS OWN MIDDLE, and with no throw on it: this is
                    // something losing its footing, not something being hit.
                    const Vec3 mid(q.midX(), q.y + 0.5f * float(t.sy) * VOXEL_M, q.midZ());
                    const Vec3 still{0.0f, 0.0f, 0.0f};
                    const Vec3 spin{jitter(1.4f), jitter(1.4f), jitter(1.4f)};
                    spawnDebris(ph, cube, n, mid, still, spin, nowMs, 0.0f, nullptr, kDebrisSoft);
                }
            }
        if (dropped) rebuildTlas();
        return dropped;
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
                for (const Solid &s : it->second.solids) {
                    // -- ON THE MODEL, NOT ON THE TRUNK ---------------------
                    //
                    // This tested s.cx/s.hx, which for a pine is a 60 cm trunk
                    // and for the tree is nothing like the truth. Every caller
                    // that goes on to ask solidAtWorld is asking about the
                    // VOXELS -- crowns, branches, a boulder's overhang -- so a
                    // gather that measured the collider handed them a list
                    // with the answer already left out of it. See
                    // solidWorldBox.
                    //
                    // IT COSTS ALMOST NOTHING WHERE IT MATTERS LEAST. The
                    // wide gathers (115 m for the birds and the bunnies) are
                    // dominated by `reach`, so their lists barely change; the
                    // 2.8 m gather a butterfly makes round itself is the one
                    // that grows, and that one was wrong.
                    float bx = 0.0f, bz = 0.0f, bhx = 0.0f, bhz = 0.0f;
                    solidWorldBox(s, VOXEL_M, &bx, &bz, &bhx, &bhz);
                    if (fabsf(bx - p.x) < reach + bhx && fabsf(bz - p.z) < reach + bhz)
                        out->push_back(s);
                }
            }
    }

    // -----------------------------------------------------------------------
    // WHERE THE DECOR OF ONE KIND IS, near a point.
    //
    // collidersNear's sibling, and it exists because a bee has to be able to
    // find a beehive and a flower. Both are PLACEMENTS rather than colliders --
    // a hive hangs several metres up in a crown and a flower is walked through,
    // so neither is in `solids` -- but every placement is recorded in decorAt
    // with its kind and where it stands, which is exactly the question.
    //
    // A HIDDEN SLOT IS NOT THERE. dropHangers clears the instance mask of a
    // hive whose tree has been felled, and fellSlots_ keeps it clear across a
    // re-adopt; asking decorAt alone would send bees to a hive that is on the
    // ground in pieces. The mask is the one place that knows.
    //
    // THE POSITION IS THE TRANSFORM'S, WHICH IS A CORNER. See World::place and
    // the three times that has bitten in this project -- a caller that wants
    // the middle of the thing has to say so itself, because decorAt does not
    // carry the model and cannot know how big it is.
    // -----------------------------------------------------------------------
    void decorNear(int kind, Vec3 p, float reach, std::vector<Vec3> *out) const {
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
                const Chunk &c = it->second;
                for (size_t i = 0; i < c.decorAt.size() && i < c.decorDesc.size(); ++i) {
                    const DecorAt &q = c.decorAt[i];
                    if (int(q.kind) != kind) continue;
                    if (!c.decorDesc[i].instanceMask) continue;
                    if (fabsf(q.x - p.x) > reach || fabsf(q.z - p.z) > reach) continue;
                    out->push_back(Vec3(q.x, q.y, q.z));
                }
            }
    }

    // Block until the ring around the camera is fully resident. Used once at
    // startup so the first frame is not a hole in the ground.
    // -----------------------------------------------------------------------
    // THE WOOD, AS IT WAS GENERATED. Everything the player did to it, undone.
    //
    // (user 2026-09-17: "let me press q to refresh the game" -- bound to G,
    //  because Q already throws the held item out of your hand.)
    //
    // THREE THINGS, AND ALL THREE OR NONE. The edits are what make this world
    // different from the one the generator describes; the meshed chunks have
    // those edits baked into their geometry; and the ring is what decides a
    // chunk is already present and need not be built. Clear the edits alone and
    // every pit stays on screen until something happens to re-mesh it. Drop the
    // chunks alone and they come straight back with the holes still in them.
    //
    // THE CHUNKS ARE RELEASED, NOT JUST FORGOTTEN. Each one holds a run of the
    // triangle pool, and erasing the map without handing that back leaks it --
    // rering's own eviction path is the shape this follows, because it is the
    // same operation done to all of them at once rather than to the ones that
    // fell outside the disc.
    //
    // WHAT THIS DOES NOT DO is re-read anything from disk. The terrain is
    // compiled in, so a reload cannot pick up a changed constant -- that still
    // wants a build. What it gives back is the world without the digging.
    void reloadWorld() {
        mesher_.edits.clear();
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            pool_.release(it->second.triOffset, it->second.tris);
            it = chunks_.erase(it);
        }
        wanted_.clear();
        requested_.clear();
        urgent_.clear();
        // ...and the ring has to be re-drawn rather than assumed unchanged: it
        // short-circuits on the player not having moved, and on a reload he has
        // not.
        primed_ = false;
        groundDirty_ = true;
    }

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
        // HOW MANY OF ITS ITEMS ARE ALREADY COMPACTED. A generation is drained
        // a few items at a time now rather than whole -- see the budget in
        // drainCompactions -- so it has to remember where it got to.
        size_t next = 0;
    };

    // How many builds share a pool, and how many epochs an unfilled one may
    // stay open before it is sealed and becomes eligible to be read.
    static constexpr size_t kGroupSize = 16;
    // ...and how many of them one ordinary frame will compact. See
    // drainCompactions for why this is counted in builds.
    // TWO, not four: four still left 12 to 14 ms on a frame, because each
    // build costs a blocking size readback. Two halves that again and still
    // drains fifteen builds a second against a mesher that delivers two.
    static constexpr int kDrainPerFrame = 2;
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
    // The apple and the orange -- see loadFruit and the fruit pass in
    // scene/chunks.h, which hangs a crop in one bearing oak in seven.
    std::vector<ModelTemplate> fruit_;
    // The first few fruit the ring adopted -- see the note at the push.
    std::vector<Vec3> fruitSeen_;
    // What the last level hanger sweep freed -- see lastHangVox().
    // The before picture -- see markLevelGrounded. One bit per cell.
    std::vector<bool> levelGrounded_;
    // Which row the hinge gave at, or -1 when the flood decided. Set inside
    // fellTree and read by the split a few lines later; see kHingeFrac.
    int hingeCut_ = -1;
    int lastHangVox_ = 0;
    int lastHangWorst_ = 0, lastHangPieces_ = 0;
    int lastHangDim_[3] = {0, 0, 0};
    int lastHangSlot_ = -1;
    Blas waterBlas_;
    // The editor's floor -- see buildStage.
    // -----------------------------------------------------------------------
    // THE PAUSE MENU'S THREE BUTTONS.
    //
    // WHAT WAS HERE: a white box 6.2 m square at (-4096, 2048, -4096), a second
    // stage on the editor deck's model -- somewhere the world was REPLACED
    // rather than hidden -- with the three buttons set into its -Z wall and a
    // pendant bulb lighting it, because a sealed box in a path tracer with two
    // outside lights renders as an empty frame.
    //
    // It is gone (user 2026-09-14: "remove the esc room from the sky. instead,
    // put the 3 balls in front of the player IN GAME"). The buttons stand in
    // the wood in front of the player now, on the flyer band, lit by the same
    // sun as everything else -- so the room's shell, its bulb, its BLAS, its
    // six palette entries, its floor-and-walls collider and the teleport at
    // each end of it are all deleted, and what is left below is what a BUTTON
    // is rather than where the wall put it. App::panelButtonAt has the where.
    // -----------------------------------------------------------------------    // -- THE BUTTONS ARE BALLS NOW, NOT DISCS -----------------------------
    //
    // "make the buttons 3x3 spheres, but move the faces to outwards. so its
    // really a 5x5 sphere but its smooth." That is a recipe for a voxel sphere
    // rather than a size: a cube with its faces pushed out is what a rounded
    // radius test draws, and dx^2+dy^2+dz^2 <= R^2 + R is exactly that test --
    // the half-voxel of slack is what fills the six flat patches a bare R^2
    // leaves, which are the thing that makes a small voxel ball read as a die.
    //
    // kBtnR IS THE SAME 6 IT WAS, because the disc's size was never what was
    // wrong with it. What changed is that it is a solid ball rather than a
    // patch painted on a wall -- and there is no wall left to paint.
    // kBtnR is the PICK radius -- what the crosshair has to be inside -- and it
    // is the button's own half-width plus a little grace, not a shape that
    // exists in the geometry at all.
    static constexpr int kBtnR = 4;                   // 0.4 m: the pick radius
    static constexpr float kBtnTravelM = 0.12f;       // how far in a press goes


    // -- A SMALL PLATFORM, AND IT IS SMALL ON PURPOSE ---------------------
    //
    // 8 m square, down from 16. The deck is a place to stand a model up and
    // walk round it; at sixteen metres most of it was empty floor, and empty
    // floor is the one thing on an asset stage that is never the subject. The
    // gridlines are still a metre apart, so it is eight of them a side and you
    // can read a model's size straight off the deck.
    static constexpr int kStageVox = 80;           // 8 m square
    static constexpr float kStageAtX = 4096.0f;    // well clear of the wood
    // -- ...SUSPENDED IN THE SKY, UNDER THE CLOUDS ------------------------
    //
    // 640 m. Clouds.slang puts the deck of cloud at CLOUD_LO = 760 and the tops
    // at 1080, so this is a hundred and twenty metres of clear air below the
    // nearest of them -- high enough that there is nothing but sky in any
    // direction, close enough that the cloud base is plainly overhead rather
    // than an abstraction. The terrain never reaches it: the generator's
    // ceiling is far below, which is what makes this a place rather than a
    // platform over a landscape.
    static constexpr float kStageAtY = 640.0f;
    static constexpr float kStageAtZ = 4096.0f;
    static constexpr const char *kButtonVox = "C:/voxelbit/game/assets/decoration/button.vox";
    int btnModel_[3] = {-1, -1, -1};
    int btnSx_ = 0, btnSy_ = 0, btnSz_ = 0;
    Blas stageBlas_;
    uint32_t stageTri_ = TriPool::kInvalid;
    bool stage_ = false;
    uint32_t waterTriOffset_ = TriPool::kInvalid;

    // -----------------------------------------------------------------------
    // -- THE LEVEL: NUKETOWN ------------------------------------------------
    //
    // A place, in the sense the asset deck above is one and for the same
    // reason (user: "this level is in a seperate world from the main world").
    // The wood is not in the acceleration structure while you are here -- no
    // chunks, no water, no decor -- so a ray that misses the map finds the
    // sky, and there is nothing of the forest to walk back into by accident.
    //
    // IT IS ITS OWN CORNER OF THE COORDINATE SPACE, not a second space. This
    // engine has one, and "a separate world" is built out of distance plus a
    // structure that only holds one of them at a time: the deck is at +4096 and
    // this is at -4096, eight kilometres apart, both at 640 m where the
    // terrain generator's ceiling cannot reach. The pause panel used to be a
    // third at (-4096, 2048, -4096) before it came down into the wood; nothing
    // is there now, and this takes the same quarter at the stage's height.
    //
    // -- IT WAS THE BUILDING UNTIL 2026-09-17 -------------------------------
    //
    // (user: "take nuket.fbx, voxelize it to the 10cm voxel framework. I want
    // you to replace the current building on the o keybind with this new
    // nuketown map.")
    //
    // building.vox and tools/voxelize_building.py are both still here and both
    // still work -- the swap is this one line, and the tools are siblings
    // rather than one having replaced the other. What the change cost the rest
    // of the engine is worth writing down, because it was NOTHING: the level is
    // an asset behind a path, so a map that is 16 x 33 m where the building was
    // 33 x 36 m needed no edit to the collider, the fence, the streamer or the
    // BLAS. levelSpawn is the one thing that moved, and it moved because a
    // CORNER is the right place to photograph a building from and the wrong
    // place to stand in a map. See its own note.
    //
    // THE ASSET IS THE WHOLE .fbx voxelised at the engine's own 10 cm by
    // tools/voxelize_nuketown.py -- 164 x 99 x 327 voxels, about 519 k
    // triangles once meshed, seventeen colours. That tool's header carries the
    // two decisions worth knowing about: the map is 1,374 solid boxes rather
    // than a shell, so the cavity rule is separating a BOX's inside from a room
    // rather than a wall seam from one; and the model has no ground outside its
    // painted slabs, so the tool lays a foundation under the whole footprint.
    // Without that last one this is a level you fall out of.
    static constexpr const char *kLevelVox = "C:/voxelbit/game/assets/level/nuketown.vox";
    static constexpr float kLevelAtX = -4096.0f;
    static constexpr float kLevelAtY = 640.0f;
    static constexpr float kLevelAtZ = -4096.0f;
    // THE LEVEL'S SIZE, AND AFTER loadLevel NOTHING ELSE. Its `a` is cleared
    // the moment levelVol_ has been built out of it -- see the note there --
    // so this is sx/sy/sz and an empty vector. Read it for dimensions; the
    // voxels are levelVol_.
    VoxAsset levelAsset_;
    std::vector<uint8_t> levelVol_;     // global material ids, VoxAsset layout
    std::vector<int16_t> levelColTop_;
    // -- THE PENDANT IN THE DARK ROOMS -- see loadLevelBulb and dressLevel ---
    //
    // The pause room's own lamp, at the path its buildRoom used.
    static constexpr const char *kLevelBulbVox =
        "C:/voxelbit/source/wip/technology/lightbulb.vox";
    // The ceiling on how many a place may have. The shader lights the nearest
    // kBulbSlots of them (see Shared.slang) and every one past that is geometry
    // that glows but does not cast -- still worth having, but not without a
    // limit, because each one is also triangles and a rebuild.
    static constexpr int kLevelBulbMax = 64;

    // -----------------------------------------------------------------------
    // -- WHERE THE LAMPS ARE, BAKED -----------------------------------------
    //
    // PASTE HERE. Place them in the game with the lightbulb in hand -- right
    // click hangs one, left click takes the nearest one down -- then press
    // CTRL+C and paste what lands on the clipboard between the braces below.
    // That is the whole round trip, and it is the same one the asset editor's
    // [C] and the pose panel's "copy pose" use: this engine bakes by pasting,
    // because a tuned number that only exists in a running process is a number
    // that dies with it.
    //
    // WORLD METRES, absolute. They carry kLevelAtX/Y/Z in them, so they are
    // only meaningful for THIS level at THIS origin -- which is also why they
    // can be pasted back without being converted.
    //
    // EMPTY ON PURPOSE (user 2026-09-17: "I want you to also remove every
    // lightbulb from the world, I will place them manually"). The survey that
    // used to fill this is gone; see the note where it stood in dressLevel.
    // A FUNCTION AND NOT AN ARRAY, for one dull reason: C++ has no zero-length
    // array, and this list starts empty and is MEANT to. An initializer list
    // may be empty, so the braces below take a paste of nought entries or forty
    // without the shape of the declaration changing.
    static std::vector<Vec3> levelBulbSeed() {
        return {
            // PLACED BY HAND 2026-09-17 and pasted back in from CTRL+C. Two
            // lamps a room, on two floors, in each of the two houses.
            { -4071.75f, 645.05f, -4059.85f },
            { -4073.05f, 645.05f, -4067.65f },
            { -4073.45f, 649.85f, -4069.15f },
            { -4073.05f, 649.85f, -4060.05f },
            { -4078.25f, 645.15f, -4035.75f },
            { -4077.85f, 650.05f, -4028.85f },
            { -4077.55f, 650.05f, -4035.95f },
            { -4078.05f, 645.15f, -4028.85f },
        };
    }
    // v1's FLWPATCH: how far one flower species runs before the next takes
    // over. 96 voxels, 9.6 m. A LENGTH, so it does not scale with the map.
    static constexpr int kFlowerPatchVox = 96;
    // The bulb's art, already resolved to GLOBAL material ids -- so stamping it
    // is a copy and the palette is not touched at [O] time.
    VoxAsset levelBulbAsset_;
    uint8_t levelGlassMtl_ = mat::AIR;   // ...and the tracer's bulbMtl
    uint8_t levelCordMtl_ = mat::AIR;
    // The file's own palette, kept between loadLevel and buildLevelPalette --
    // see the note on why those are two functions. VoxModel::pal's own type,
    // so this is a copy and not a conversion.
    std::array<std::array<uint8_t, 4>, 255> levelPal_{};
    bool levelPaletteReady_ = false;
    // EVERY ENTRY A HELD MODEL WEARS. Filled by addHeldVox as the kit loads and
    // read once by buildLevelPalette; the kit is the one thing that is drawn in
    // both places, so these are the ids the level's table may not reuse.
    std::array<uint8_t, 256> heldMtl_{};
    // Where every bulb ended up, world metres. The tracer lights ONE point, so
    // App points it at whichever of these is nearest the eye -- see the note
    // there. Filled by dressLevel, so empty until the first [O].
    std::vector<Vec3> levelBulbs_;
    // Whether the baked seed has been taken. After that the list above is the
    // player's -- see the note at the bulb pass in dressLevel.
    bool levelBulbsSeeded_ = false;
    // -- THE LEVEL IN BLOCKS -- see buildLevelBlas for why it is not one ----
    //
    // 64 voxels square, full height: 6.4 m columns, 128 of them over this map.
    // Small enough that re-meshing one after a bullet is a few milliseconds and
    // large enough that the map is not a thousand structures.
    static constexpr int kLevelBlockVox = 64;

  public:
    // One line per chip, under --swing-log. Off by default: a burst is five a
    // second and each would print.
    bool carveLog = false;

  private:
    // The identity material map the block mesher reads -- mat::COUNT long, built
    // on first use. See meshLevelBlock for what a 256-long one did to [O].
    decltype(VoxAsset::a) levelIdent_;
    std::vector<Blas> levelBlocks_;
    std::vector<uint32_t> levelBlockTri_;
    std::vector<size_t> levelBlockTris_;
    int levelBX_ = 0, levelBZ_ = 0;
    // The DRESSED grid -- the map plus its grass, flowers and lamps. Resident
    // while the level is open because a block rebuild has to re-read it.
    VoxAsset levelDisplay_;
    size_t levelTris_ = 0;
    bool level_ = false;
    bool levelLoaded_ = false;
    // Whether the device is holding the LEVEL's material table rather than the
    // wood's. Only setLevel writes it; uploadMaterials reads it.
    bool levelTableActive_ = false;

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
        // WHEN THIS PIECE WAS CUT OFF A LIVING THING, or the sentinel for
        // every ordinary chip of stone. See corpseFade.
        double hurtT0 = -1e9;
        // ...and when it stopped moving, which is when its half second on the
        // ground starts. Sentinel means "still going".
        double restT0 = -1e9;
        bool live = false;
        bool absorbing = false;
        // How near the player has to be before this one will come to them. See
        // kArrowAbsorbM; kAbsorbAnywhere is the default and is what a tool's
        // chip has always had.
        float absorbR = kAbsorbAnywhere;
        // HOW LONG THIS ONE LASTS, or the sentinel for "whatever its kind
        // lasts". Per body rather than per kind because the level's chips want
        // their own number and are otherwise ordinary debris -- see
        // kLevelChipLifeMs.
        double lifeMs = -1.0;
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
        // Too long for a chip's fixed window and clamp -- see kLongBodyM.
        // Not the same thing as `felled`: a light pole is neither a tree
        // nor a chip, and before this there was no third answer.
        bool longBody = false;
        // -------------------------------------------------------------------
        // IT IS SCENERY: IT IS NOT COMING TO YOU.
        //
        // Everything small enough to carry lifts off after kAbsorbWaitMs and
        // curves into the player's hands (see the absorb in updateDebris), and
        // "small" is the only question asked. That is right for a chip and
        // wrong for anything that is small and still a THING -- a mushroom cut
        // off its stem is a couple of hundred voxels, so it fell correctly for
        // half a second and then flew at the player. Reported as "when cutting
        // the mushroom from the static terrain it just flies".
        //
        // NOT `felled` ABOVE, WHICH WOULD HAVE BEEN THE EASY REUSE. That flag
        // also means KIND_TREE shading, a mesh whose shades are NOT resolved
        // per voxel, a per-frame static window, and no floor backstop -- four
        // things a cap does not want. This is the one bit that was actually
        // meant.
        //
        // NOT A PROPERTY OF THE MATERIAL EITHER -- see kDebrisSoft. Two bodies
        // of the same mushroom differ: the bite is loot, the cap is scenery.
        // It is set where the body is BORN, because that is the only place that
        // knows which of the two this one is.
        // -------------------------------------------------------------------
        bool scenery = false;
        // THE FELLING HINGE, HELD UNTIL THE PIECE LANDS. v1's tipArm/tipAx:
        // the topple is ARMED at the cut and applied on the frame the trunk
        // has actually come down on its own stump, so it drops square instead
        // of rotating through the wood underneath it. See fellTree.
        bool tipArmed = false;
        Vec3 tipAxis{1, 0, 0};
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
        // -------------------------------------------------------------------
        // ...AND ITS OWN VOXELS, WHICH ARE WHAT A TOOL MEETS.
        //
        // The boxes are the SOLVER's shape -- forty centimetre cells, no
        // canopy, a fill fraction. Swinging at that would be swinging at a
        // fattened cartoon of the log: you would hit air a third of a metre off
        // the bark and miss a branch entirely. These are the real thing, in the
        // body's own axes with `originOff` as their corner, and they are what
        // the swing marches (debrisRay) and what a bite removes (carveDebris).
        //
        // Held rather than borrowed back off the model because the model is no
        // longer the truth about this object: the instance it came from is a
        // stump now, and by the second blow this volume has holes the template
        // never had.
        // -------------------------------------------------------------------
        std::vector<uint8_t> vox;
        int vsx = 0, vsy = 0, vsz = 0;
        // Stone, wood or soil -- see DebrisTakes. What a Solid answers with
        // `standable` and a body cannot, because a felled tree is lying down.
        uint8_t takesAs = kDebrisStone;
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
    // The world-voxel box of what moved since the last patch rebuild -- see
    // markGroundDirty. Only meaningful while groundDirty_ is set.
    int gdI0_ = 0, gdJ0_ = 0, gdI1_ = 0, gdJ1_ = 0;
    // Scratch for dropTerrainHangers, kept so a blow allocates nothing.
    std::vector<uint8_t> hangSolid_, hangSeen_;
    std::vector<int> hangStack_;
    // THE GROUND A BITE CUT LOOSE, AS VOXELS WAITING TO BECOME A BODY.
    // Filled by dropTerrainHangers in the same (2r+1) cube layout meshVolume
    // and spawnDebris read, and taken by the caller -- see takeHangers.
    std::vector<uint8_t> hangVol_;
    int hangN_ = 0;
    Vec3 hangAt_{0.0f, 0.0f, 0.0f};
    // A MODEL'S CUBE IS IN THE MODEL'S OWN FRAME, so the body has to be born
    // wearing the model's quarter turn -- the same yaw the spoil is given.
    // Zero for terrain, which has no frame of its own.
    float hangYaw_ = 0.0f;
    bool hangHave_ = false;
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
    // ...AND THE SCATTER A TILL PUT AWAY, WHICH HAS TO SURVIVE THE REBUILD.
    //
    // (user 2026-09-14, reported THREE times: "the flowers are still not
    // dissapering when being tilled under".)
    //
    // THE FIX WAS RIGHT AND IT WAS UNDONE ONE FRAME LATER. hideScatterOn
    // clears the instance mask, but a till EDITS THE GROUND, and editing the
    // ground re-meshes the chunk -- so a second or so afterwards adoptMany
    // rebuilds decorDesc from the deterministic scatter, every mask back on,
    // and the flowers stand up again over the turned earth. Nothing was wrong
    // with the hiding; it was being thrown away by the thing that asked for it.
    //
    // WHICH IS WHY --hoe-test PASSED TWICE ON A BROKEN ENGINE: it counted the
    // masks in the same breath as the till, before the mesher had answered.
    //
    // SO IT IS KEPT HERE, off the chunk, and replayed by reapplyDamage exactly
    // as fellSlots_ is. Unlike fellSlots_ it is NOT forever -- showScatter
    // takes slots back out of it when the bed grows back.
    // -----------------------------------------------------------------------
    std::set<std::pair<long long, int>> hiddenScatter_;
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
    // -- A SET, AND A FLAT ONE (user 2026-09-15: "its hitching") ------------
    //
    // This was a std::map -- a red-black tree -- holding nothing but keys, and
    // rering() CLEARS AND REFILLS IT on every chunk boundary the player crosses.
    // At the default ring radius that is a disc of about 450 chunks, so every
    // crossing paid 450 node allocations and 450 rebalances to rebuild a set
    // that is only ever asked "is this key in you".
    //
    // Measured on a walk, rering ran 3 to 9.4 ms and it is one of the few costs
    // here that lands EXACTLY when the player is moving -- crossing boundaries
    // is what walking IS. Nothing needs the ordering: the three readers are two
    // count() tests and one iteration that does not care what order it gets.
    std::unordered_set<long long> wanted_;
    std::set<long long> requested_;
    // The subset of `requested_` that an EDIT asked for, which update() adopts
    // without waiting for the streaming budget. See remesh().
    std::set<long long> urgent_;

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
        // -- AND THE VOXELS THEMSELVES, FOR THE ONES THAT CAN DIE ----------
        //
        // (user 2026-09-14: "when killing life, the life breaks apart into
        // multiple pieces".)
        //
        // A model is uploaded as TRIANGLES and that is all anything needed
        // until something had to come apart: a corpse is built from the
        // animal's own voxels, in the pose it was last drawn, exactly as v1
        // rebuilds a trace-injected creature's ragdoll from its item model.
        // There is no route back from a BLAS to a grid, so the grid is kept.
        //
        // KEPT ONLY WHERE IT IS ASKED FOR. addFlyerModel takes a flag: the
        // twenty-odd animals want it (a few KB each -- the biggest is a duck at
        // 20 x 14 x 12) and the held tools, the kit and the pause room's
        // buttons do not.
        //
        // WORLD LAYOUT, x + z*sx + y*sx*sz, which is what VoxAsset is and what
        // spawnDebris wants -- see shatterFlyer.
        std::vector<uint8_t> vol;
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
    double lastDebrisMs_ = 0.0;   // see corpseFade
    int flyerBase_ = -1;        // first instance of the band, -1 while unbuilt
    // What each slot of the band is drawing, and how big it is -- see
    // setFlyerInstance, where both are written, and flyerAt, which is why.
    std::vector<int16_t> flyerModel_ = std::vector<int16_t>(size_t(kFlyerInstances), -1);
    std::vector<float> flyerR_ = std::vector<float>(size_t(kFlyerInstances), 0.0f);
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

    // -- HOW MANY GENERATIONS ONE CALL MAY COMPACT ---------------------------
    //
    // (user 2026-09-15: "just as im walking around the environment its
    // hitching".)
    //
    // THE DRAIN WAS UNBOUNDED and that is the hitch. Compaction is
    // opportunistic -- it runs when the streamer goes idle, on the reasoning
    // that idle time is free -- but a group only becomes eligible once the
    // DEVICE has finished it, so while you walk they pile up unread, and the
    // moment the streamer draws breath every one of them lands on the same
    // frame. Each carries up to kGroupSize builds, and each build costs a
    // blocking size readback plus a buffer creation.
    //
    // MEASURED on a walk, on frames that adopted NO chunks at all:
    //
    //     61.5 ms      51.9 ms      43.1 ms      31.3 ms      29.5 ms
    //
    // A FEW BUILDS A FRAME is the whole fix. The work is identical and the
    // total is identical; it is simply not allowed to arrive all at once.
    //
    // COUNTED IN BUILDS RATHER THAN GENERATIONS, because a generation is up to
    // kGroupSize of them and each one costs a blocking size readback plus a
    // buffer creation: capping at one generation still left 23 ms frames.
    // Sixteen builds is what a group holds and four a frame drains one in four
    // frames -- fifteen a second, against a mesher that delivers two.
    //
    // -1 IS UNLIMITED and is what every non-frame caller keeps: the prime and
    // buildBlas both need the queue genuinely empty when they return, and both
    // are already synchronous by design.
    bool drainCompactions(bool force, int maxItems = -1) {
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
        int done = 0;
        while (!groups_.empty()) {
            CompactGroup &grp = groups_.front();
            if (!grp.sealed || grp.epoch > deviceDone_) break;
            // AS MANY AS THE BUDGET ALLOWS, FROM WHERE THIS GROUP GOT TO. The
            // generation is only retired -- and its query pool only recycled --
            // once every one of its builds has been compacted, so a half-drained
            // group is left at the front for the next call.
            while (grp.next < grp.items.size()) {
                if (maxItems >= 0 && done >= maxItems) return any;
                PendingCompact &p = grp.items[grp.next];
                finishCompact(p, grp.pool.get());
                recyclePending(p.staged);
                ++grp.next;
                ++done;
                any = true;
            }
            freePools_.push_back(grp.pool);
            groups_.pop_front();
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
                      std::vector<std::array<uint8_t, 4>> *colourSink = nullptr,
                      // NON-ZERO REPLACES THIS MODEL'S GREENS with that
                      // material instead of minting palette entries for them.
                      // A flower's stem is grass, and saying so here means it
                      // wears the grass RAMP -- the same six shades, picked per
                      // voxel on the device -- rather than one authored green
                      // that matches nothing it is standing in.
                      uint8_t stemId = mat::AIR,
                      // ...AND THIS ONE IS LAST FOR A REASON. Inserting it into
                      // the middle of this list renumbered every positional
                      // argument after it, and the rock loaders pass colourSink
                      // and stemId positionally -- which the compiler caught,
                      // but only because the types happened to disagree.
                      bool solidify = false,
                      // ...AND SO IS THIS ONE, for the same reason. See above.
                      //
                      // HOW HARD THIS SET FOLDS ITS COLOURS ONTO THE TABLE.
                      // Zero is what every set shipped with: each colour gets
                      // its own entry, subject only to the quantised bucket
                      // forModelColor already dedupes on. Non-zero lets a colour
                      // SHARE an entry already in the table within that
                      // distance.
                      //
                      // ONLY THE OAK PASSES ONE, and only because the table has
                      // no room -- see the note at its call. Folding is not free
                      // and it is not a default: a shared entry carries the
                      // MATERIAL of whatever minted it, so a set that folds is
                      // a set that has agreed to wear another's surface.
                      int matchTol = Palette::kModelMatch) {
        // -- TOLERANCE REUSE IS THE DEFAULT NOW --------------------------
        //
        // (user 2026-09-17, on adding the cherry forest and the desert:
        //  "ok go ahead and do tolerance reuse".)
        //
        // THE WORLD USED TO MINT EVERY SHADE IT ASKED FOR. This defaulted to 0
        // -- "zero for the world, which mints what it asks for" -- while the
        // LIFE band had been folding at kModelMatch for weeks. So the trees,
        // the rocks and the decor each took an entry per distinct authored
        // colour, and the 255-entry table filled up: it read 255 of 255 on the
        // build this landed in, with nothing left for another biome.
        //
        // v1 runs the SAME 256 ceiling with five biomes in it, and this is how:
        // its PAL_TOL reuses an entry that is already close enough rather than
        // minting a neighbour of it. MEASURED over v2's own .vox files, what
        // the world costs and what the cherry forest and the desert would add:
        //
        //     tol  0   world 205   + 80   -- over the ceiling
        //     tol 16   world 103   + 18   <- here
        //     tol 20   world  87   + 13
        //
        // kModelMatch RATHER THAN A SECOND NUMBER, because 16 is already the
        // engine's answer to "how close is the same colour" -- the flyer band
        // has used it since the life was imported. One constant is one thing to
        // tune.
        //
        // WHAT STILL MINTS EXACTLY: anything registered with `exact` -- the
        // held kit and the flyer band's own frames -- because that path takes
        // the exact key and never reaches the tolerance branch. And the private
        // materials (the firefly's glow, the sparks, the smoke) ask
        // resolveModelColor and PRINT how many entries share them, so if this
        // ever folds one of them onto the world the load line says so.
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
                // NOT HOLLOW. See vox.h::fillCavities for what that means and
                // what it deliberately does NOT mean -- the gaps between a
                // pine's branches are not cavities and are left alone.
                if (solidify) solidVox += fillCavities(&a);
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
                // THE BIRCHES BEGIN HERE. loadPines() fills one vector with
                // both species -- pines below birchBase, birches at and above
                // it -- so this is the only moment the palette can be told
                // which greens belong to which wood. See markBirchStart.
                if (&pines_ == out && int(out->size()) == mesher_.birchBase)
                    palette.markBirchStart();
                for (int e = 1; e <= 255; ++e)
                    if (used[e]) {
                        const std::array<uint8_t, 4> &pc = mo.pal[e - 1];
                        // Green-dominant is the stem and its leaves; the bloom
                        // is not, and keeps its authored colour.
                        // Green-dominant is the stem and its leaves; the bloom
                        // is not, and keeps its authored colour.
                        if (stemId != mat::AIR && pc[1] > pc[0] && pc[1] > pc[2])
                            idOfEntry[e] = stemId;
                        else
                            idOfEntry[e] =
                                palette.forModelColor(pc, /*conifer=*/true, /*exact=*/false,
                                                      matchTol);
                    }

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
    int solidVox = 0;   // how many sealed-in voxels fillCavities filled, for the report

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
        // -- AND THE OAKS (user 2026-09-16: "import the oak forest from v1") --
        //
        // SEVEN, AND THEY ARE A DIFFERENT SHAPE OF TREE. Measured off the
        // files: 2.1 m to 17.1 m tall, and 3.4 m to 17.0 m ACROSS -- the
        // biggest is as wide as it is high, where a birch is 30 m of near
        // vertical trunk and a pine is a spire. oak_1 is a bush at 2.1 m and is
        // kept in the set deliberately: v1 has it, and a wood whose smallest
        // member is waist-high is what makes the rest read as old.
        //
        // AFTER THE BIRCHES, so the three ranges are contiguous and positional:
        // pine [0, birchBase), birch [birchBase, oakBase), oak [oakBase, size).
        // templateFor, the footprints, the colliders and the perch lists are
        // all positional into this one array and none of them needs to know a
        // third species exists.
        // /*solidify=*/true -- the trees are the set this was asked for.
        loadModelSet(paths, &pines_, false, false, 0u, /*perches=*/true, /*upscale=*/0,
                     /*colourSink=*/nullptr, /*stemId=*/mat::AIR, /*solidify=*/true);

        // -- AND THE OAKS, AS THEIR OWN SET AND FOLDED ---------------------
        //
        // A SECOND CALL RATHER THAN MORE PATHS IN THE FIRST, because this set
        // and only this set is asked to share entries. loadModelSet appends, so
        // the ranges stay contiguous exactly as the rocks' two calls do.
        //
        // THE FOLD IS NOT A PREFERENCE, IT IS THE TABLE BEING FULL. The oak
        // brings seven colours -- three barks and four greens, measured across
        // all seven files -- and the palette stood at 252 of 255 before it
        // arrived. Registered plainly they took the last three and then the
        // HELD WHEAT starved, losing two of its own and drawing as a single
        // voxel. That is the fourth time this table has taken something away
        // silently; see the note over Palette::forModelColor.
        //
        // 30 IS CHOSEN OFF THE OAK'S OWN SEVEN COLOURS, measured rather than
        // picked. They are three barks and four greens:
        //
        //     bark   (87,77,54) (99,89,66) (109,100,82)   each ~21 apart
        //     green  (82,115,47) (105,143,51) (107,141,77) (134,167,89)
        //
        // At 22 the three barks collapse to one and not a single green moves --
        // the nearest pair of those is 36 apart -- which recovered two entries
        // and left the wheat one short. At 30 the two middle greens join as
        // well, and the oak keeps a DARK, a MID and a LIGHT canopy shade, which
        // is all the variation a crown reads at.
        //
        // AND A LEAF SHARING A LEAF'S ENTRY IS SHARING THE FOLIAGE MATERIAL --
        // the translucency and the waxy roughness -- which is the right surface
        // for it and is what conifer=true hands any green-dominant colour here
        // anyway. This is the one case where sharing is not a compromise.
        // -- ...AND THE OAK WEARS THE PINE'S BARK ON PURPOSE ---------------
        //
        // (user 2026-09-17: "the oak tree color looks terrible. match the
        //  pine trees wood color for the oak trees.")
        //
        // MEASURED, all three of the oak's authored barks fold onto entries
        // the PINES minted, and they are the nearest thing in the table by a
        // long way:
        //
        //     oak (101, 89, 67) -> pine (112, 86, 63)
        //     oak (113,104, 88) -> pine (129, 99, 72)
        //     oak ( 84, 76, 51) -> pine ( 94, 73, 53)
        //
        // SO THE FOLD IS THE FEATURE HERE, NOT A COMPROMISE. It was briefly
        // treated as one: the oak was given its own three entries, lifted
        // x1.25 to answer "use a lighter brown on the oak trees wood", and
        // the answer was rejected on sight. The lift is gone and so are the
        // three entries -- one wood colour for every trunk in the world, which
        // is what was asked for and is also what the table would prefer.
        //
        // The LEAVES fold too and always did, onto the birches' greens, which
        // the note above calls "the one case where sharing is not a
        // compromise". Both halves of this set now share, for two different
        // reasons that happen to agree.
        mesher_.oakBase = int(pines_.size());
        if (!terrain.forced || terrain.biome == Biome::Oak) {
            std::vector<std::string> oaks;
            for (int i = 1; i <= 7; ++i)
                oaks.push_back(oakDir + "/oak_" + std::to_string(i) + ".vox");
            loadModelSet(oaks, &pines_, false, false, 0u, /*perches=*/true, /*upscale=*/0,
                         /*colourSink=*/nullptr, /*stemId=*/mat::AIR, /*solidify=*/true,
                         /*matchTol=*/30);
        }
        if (pines_.empty()) {
            std::fprintf(stderr, "v2: no pine models loaded from %s -- pass --pines\n",
                         pineDir.c_str());
            return false;
        }
        loadedPines = int(pines_.size());
        if (solidVox > 0)
            std::printf("  trees    %d sealed-in voxels filled with their own walls\n",
                        solidVox);
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

    // ONE FILE, TWO SETS -- the pine wood's flowers and the birch wood's, the
    // same models with their stems painted out of the two grass ramps. The
    // scatter picks the half that matches the blade it is standing in; see
    // mesher_.flowerBirch0, and loadMushrooms for the same shape.
    void loadFlowers() {
        loadModelSet({decorDir + "/flowers.vox"}, &flowers_, true, false, 0u, false, 0, nullptr,
                     mat::GRASS_0);
        mesher_.flowerBirch0 = int(flowers_.size());
        loadModelSet({decorDir + "/flowers.vox"}, &flowers_, true, false, 0u, false, 0, nullptr,
                     mat::BGRASS_0);
        loadedFlowers = int(flowers_.size());
    }

    // TWO SETS FROM ONE FILE: the models as authored, then the same models
    // revoxelised at 2x. The small ones come first and mushroomBig0 is the
    // boundary, so the scatter can weight the draw between them.
    void loadMushrooms() {
        // NOT HOLLOW EITHER (user 2026-09-14: "fill in the hollow mushroom with
        // the same red pixels that make up the surface"). The trees got this on
        // the same ask and the mushrooms were left out of it; they are the one
        // decoration a tool can now cut into, so the inside is visible.
        //
        // /*solidify=*/true is vox.h::fillCavities, and the "same red" falls out
        // of it rather than being named here: a sealed voxel takes the commonest
        // palette entry among its solid neighbours, so the cap's air fills with
        // cap and the stem's with stem. MEASURED on the shipped asset -- the big
        // cap is 1053 sealed voxels against 1615 solid, which is to say it was
        // very nearly all shell.
        //
        // BEFORE THE UPSCALE, which is the order loadModelSet already runs in:
        // filling the authored 23^3 model is 12k voxels of flood, filling the 2x
        // one would be 93k, and upscale2x of a solid interior is solid anyway.
        //
        // IT COSTS NO TRIANGLES -- it SAVES them. A cavity wall is solid meeting
        // air, so the mesher was emitting the inside of every cap; sealing it
        // deletes those faces.
        loadModelSet({decorDir + "/mushroom.vox"}, &mushrooms_, true, false, 0u, false, 0,
                     nullptr, mat::AIR, /*solidify=*/true);
        mushroomBig0 = int(mushrooms_.size());
        loadModelSet({decorDir + "/mushroom.vox"}, &mushrooms_, true, false, 0u, false,
                     /*upscale=*/1, nullptr, mat::AIR, /*solidify=*/true);
        loadedMushrooms = int(mushrooms_.size());
    }

    // The beehive. Birch only -- see the hive pass in scene/chunks.h, which
    // hangs one in a hundredth of the trees.
    void loadHives() {
        if (!terrain.birch()) return;
        loadModelSet({decorDir + "/beehive.vox"}, &hives_, false, true);
    }

    // -- THE TREE FRUIT: AN APPLE AND AN ORANGE ---------------------------
    //
    // (user 2026-09-17: "add apples and oranges to some of the trees in the
    //  oak forest. import the v1 mechanics of this.")
    //
    // OAK ONLY, and the gate is the same one loadHives uses: no model set
    // means no pass, with no second flag to keep in step.
    //
    // THE LEAF WEARS THE OAK'S OWN CANOPY, WHICH IS WHAT stemId IS FOR.
    // Each fruit is 19 voxels of flesh and 3 to 5 of stem and leaf, and the
    // leaf is authored (171,178,100) -- a green that belongs to the bake
    // rather than to this wood. Handed to stemId it is replaced by the
    // crown's own entry instead of minting a fourth green, which is exactly
    // what the browser engine does with `near(fj.pal[fj.nbody], OAKLEAF)`,
    // and exactly what a flower's stem already does with the grass ramp.
    //
    // SO THE FRUIT COSTS TWO PALETTE ENTRIES, one red and one orange, and
    // those two are the whole point of it: they are minted at tolerance 0
    // rather than folded, because a fruit that shares an entry with
    // something near it is a fruit you cannot see in a crown.
    void loadFruit() {
        if (!terrain.oak()) return;
        // THE NEAREST CANOPY entry to the bake's leaf green -- not the
        // nearest entry, which is a different question and gave a wrong
        // answer here. See Palette::nearestFoliage for the measurement.
        const uint8_t leaf = palette.nearestFoliage({171, 178, 100, 255});
        loadModelSet({decorDir + "/fruit_apple.vox", decorDir + "/fruit_orange.vox"},
                     &fruit_, false, true, 0u, /*perches=*/false, /*upscale=*/0,
                     /*colourSink=*/nullptr, /*stemId=*/leaf ? leaf : mat::AIR,
                     /*solidify=*/false, /*matchTol=*/0);
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
    // -----------------------------------------------------------------------
    // A HOLLOW WHITE BOX WITH THREE COLOURED DISCS ON ONE WALL.
    //
    // Built in code rather than authored, for the reason the stage deck is:
    // it is a fixture of the interface and not a model anybody will re-author,
    // so there is nothing for a .vox file to be the source of truth about.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE BUTTON, THREE TIMES, IN THREE COLOURS.
    //
    // ONE FILE AND THREE MODELS. button.vox is 5 x 5 x 5 and every one of its
    // 54 voxels is the same firebrick red, which makes recolouring it a
    // one-entry edit of the palette it carries -- addFlyerModel reads
    // mo.pal[e-1] and nothing else, so three copies with three palettes are
    // three coloured buttons and no special case anywhere downstream.
    // -----------------------------------------------------------------------
    void loadRoomButtons() {
        if (btnModel_[0] >= 0) return;
        VoxModel mo;
        std::string err;
        if (!voxLoad(kButtonVox, &mo, &err)) {
            std::fprintf(stderr, "v2: room button %s: %s -- the wall keeps its discs%s",
                         kButtonVox, err.c_str(), "\n");
            return;
        }
        // The colours the room has always used, and in the order the wall reads
        // left to right: quit, back, Discord. They live beside buttonAt now --
        // the label hanging over each button is tinted with the same three.
        const auto &rgb = kBtnRgb;
        for (int b = 0; b < 3; ++b) {
            VoxModel tint = mo;
            for (int e = 0; e < 255; ++e) {
                if (!tint.pal[size_t(e)][3]) continue;
                tint.pal[size_t(e)] = {rgb[b][0], rgb[b][1], rgb[b][2], 255};
            }
            int sx = 0, sy = 0, sz = 0;
            btnModel_[b] = addFlyerModel(tint, "room button", &sx, &sy, &sz);
            if (b == 0) { btnSx_ = sx; btnSy_ = sy; btnSz_ = sz; }
        }
        if (btnModel_[0] >= 0)
            std::printf("  room     button.vox %d x %d x %d, three of them\n", btnSx_,
                        btnSy_, btnSz_);
    }

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

    // -----------------------------------------------------------------------
    // THE LEVEL'S VOXELS AND ITS COLOURS -- at start-up, with everything else.
    //
    // WHY THIS IS NOT LAZY, when the deck it sits beside is. Palette entries are
    // served FIRST COME and there are 255 of them for the whole world; whatever
    // registers last is what gets handed mat::AIR and silently stops being
    // drawn. A level built on the keypress that opens it would be asking after
    // the trees, the rocks, the flowers, the ground ramps and the held kit --
    // last in the queue, for an asset that wants twenty-odd entries at once.
    // That is the exact shape of the bug that took the stone tools and then the
    // pause room; HeldItem::prewarmColors is the fix both times and this is the
    // same move. See the palette notes in scene/voxelworld.h.
    //
    // THE STRUCTURE IS STILL LAZY, and that is the half worth deferring: half a
    // million voxels is half a million triangles and a BLAS to match, and most
    // runs never press the key. buildLevelBlas does that on arrival.
    //
    // A MISSING FILE IS A WARNING, NOT A FAILURE. The .fbx it is voxelised from
    // is gitignored, so a fresh clone has no map until tools/voxelize_nuketown.py
    // is run, and refusing to start the wood over that would be absurd.
    // -----------------------------------------------------------------------
    bool loadLevel() {
        if (levelLoaded_) return true;
        VoxModel mo;
        std::string err;
        if (!voxLoad(kLevelVox, &mo, &err)) {
            std::fprintf(stderr, "v2: level %s: %s -- [O] will have nowhere to go\n", kLevelVox,
                         err.c_str());
            return false;
        }
        levelAsset_ = toWorldWhole(mo);
        if (levelAsset_.sx <= 0 || levelAsset_.sy <= 0 || levelAsset_.sz <= 0) {
            std::fprintf(stderr, "v2: level %s composed to nothing\n", kLevelVox);
            return false;
        }

        // -- THE COLOURS ARE NOT ALLOCATED HERE ANY MORE ---------------------
        //
        // They are allocated in buildLevelPalette, which runs after the held
        // kit has loaded, and the ORDER is the whole reason it was split out:
        // the level has a palette table of its own now, and the one thing that
        // table may not touch is an entry the KIT wears -- because the kit
        // walks through the door with you. The kit's ids are not known until it
        // has loaded. See Palette::beginLevelTable.
        //
        // WHAT IT COSTS is levelAsset_.a staying alive a few hundred
        // milliseconds longer than it used to: the file's own palette entries,
        // one byte a voxel, freed at the end of buildLevelPalette exactly as
        // they were freed at the end of this function before.
        levelPal_ = mo.pal;
        levelLoaded_ = true;
        // -- AND ITS COLOURS ARE TAKEN NOW, NOT AFTER THE KIT ----------------
        //
        // This was deferred to App start-up while the level had a palette TABLE
        // of its own: that table may not reuse an entry the held kit wears, and
        // the kit's ids are not known until it has loaded. The table is gone
        // (see World::setLevel) and the reason went with it.
        //
        // MOVING IT BACK MATTERS, it is not tidying. Deferred, the level was the
        // LAST thing in the program to ask for colours -- after the trees, the
        // animals, the flyer band and the kit -- and colours are served
        // first-come. The moment the rifle went byte-exact and took three more,
        // the level overran the table: 204,021 voxels came back AIR, and the
        // bulb's glass came back AIR with them, which silently skipped every
        // lamp in the map (`levelGlassMtl_ != mat::AIR` guards the whole pass).
        // One overflow, two features gone, and the log only mentioned one.
        buildLevelPalette();
        std::printf("  level    %d x %d x %d voxels (%.1f x %.1f x %.1f m), colours deferred\n",
                    levelAsset_.sx, levelAsset_.sy, levelAsset_.sz,
                    float(levelAsset_.sx) * VOXEL_M, float(levelAsset_.sy) * VOXEL_M,
                    float(levelAsset_.sz) * VOXEL_M);
        return true;
    }

    // -----------------------------------------------------------------------
    // THE LEVEL'S OWN PALETTE -- after the kit, and that is not negotiable.
    //
    // (user 2026-09-17: "surely we can have multiple color paletes for multiple
    // worlds?")
    //
    // Palette::beginLevelTable says what a second table is and why the level is
    // allowed one. This is the caller that decides WHAT IS STILL SHARED, which
    // is the only judgement in the whole thing:
    //
    //   * the fixed mat:: band, 0 .. TREE_BASE. The level borrows BGRASS_0 for
    //     its lawns and ROCK for the bulb's flex, and every ramp the device
    //     spreads in groundShade() lives in there.
    //   * every entry the HELD KIT wears (heldMtl_). You carry the axe and the
    //     rifle through [O] and they must not change colour on the way.
    //   * every entry the WOOD'S FLOWERS wear, because dressLevel stamps those
    //     models' own voxels into the map -- wood art standing in a level.
    //
    // Everything else the wood owns -- every tree, rock, animal, butterfly and
    // bird -- is invisible from inside the level, so its entries are free. That
    // is roughly 130 of them against the dozen the map wants, which is the
    // difference between a table that is full and one that is not.
    // -----------------------------------------------------------------------
    bool buildLevelPalette() {
        if (!levelLoaded_ || levelPaletteReady_) return levelPaletteReady_;
        // Only the entries some voxel actually wears -- the trap every .vox
        // walks into otherwise is that the file ships all 255 palette slots
        // whether the artist used them or not, and allocating per SLOT would
        // hand this one asset the whole table.
        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        std::vector<bool> used(256, false);
        for (uint8_t v : levelAsset_.a) used[v] = true;
        int asked = 0;
        int greens = 0;
        const int before = palette.used();
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)]) {
                // -- THE MAP'S GREENS ARE THE OAK WOOD'S GRASS --------------
                //
                // (user 2026-09-17: "on the grass terrain in nuketown, I want
                // you to make the ground have the same colors as the oak forest
                // grass, multiple random colors".)
                //
                // ONE ID, SIX SHADES, AND THE DEVICE PICKS. mat::BGRASS_0 is
                // the broadleaf ramp the oak floor already wears -- six greens
                // sampled off the oaks' own foliage -- and groundShade() spreads
                // it per voxel out of a hash of the voxel's position. So this
                // single substitution IS the "multiple random colors": nothing
                // has to be scattered, and two neighbouring lawn voxels come out
                // different shades for free. See the ramp note over GRASS_0.
                //
                // AND IT PAYS FOR THE RIFLE. The level's two greens were two
                // entries of a table that had three left in it; borrowing a ramp
                // that already exists hands them back, which is where the gun's
                // eleven exact colours came from. See HeldItem::mergeTolFor.
                //
                // BY SATURATION, NOT BY INDEX, so a re-voxelised map still
                // lands the same way -- the same test the pause-room bulb used
                // to tell its glass from its cap. The lawns are (21,121,39) and
                // the hedges (6,107,0); the tree canopies are CYAN in this model
                // (79,213,231) and correctly fail it, because blue beats green
                // there.
                const std::array<uint8_t, 4> &c = levelPal_[size_t(e) - 1];
                if (int(c[1]) > int(c[0]) + 24 && int(c[1]) > int(c[2]) + 24) {
                    idOfEntry[size_t(e)] = mat::BGRASS_0;
                    ++greens;
                    continue;
                }
                idOfEntry[size_t(e)] = palette.forModelColor(c, false);
                ++asked;
            }

        levelVol_.assign(levelAsset_.a.size(), mat::AIR);
        int lost = 0;
        for (size_t k = 0; k < levelAsset_.a.size(); ++k) {
            const uint8_t id = idOfEntry[size_t(levelAsset_.a[k])];
            levelVol_[k] = id;
            if (levelAsset_.a[k] && id == mat::AIR) ++lost;
        }
        seatLevelOnItsFoundation();
        markLevelGrounded();
        levelColTop_ = columnTops(levelAsset_, idOfEntry);
        loadLevelBulb();
        // ...AND THE PALETTE-INDEX COPY IS DONE WITH. levelVol_ now holds the
        // same grid as GLOBAL material ids, which is what the mesher, the
        // collider and the renderer all want; keeping the file's own entries
        // as well is 27 MB spent saying the same 27 million voxels twice. What
        // survives of levelAsset_ is its DIMENSIONS -- see the member.
        levelAsset_.a.clear();
        levelAsset_.a.shrink_to_fit();
        levelPaletteReady_ = true;
        // -- AND THE DEVICE HAS TO BE TOLD, WHICH IT USED NOT TO NEED --------
        //
        // While the level's colours were minted inside World::build this was
        // free: a dozen later loaders each call uploadMaterials, and the last
        // of them carried the level's entries along with their own. This
        // function runs AFTER all of that -- it has to, so that the held kit's
        // ids are known -- which makes it the last thing in the program to
        // mint, and nothing was ever going to push it.
        //
        // WHAT IT LOOKED LIKE, because the symptom does not point here: every
        // surface in the map rendered the same cool grey, measured at sRGB
        // (170,177,184). That is MaterialLook's DEFAULT albedo of 0.4 with the
        // sky on it -- the entries existed, the geometry pointed at them, and
        // the device's copy of the table had simply never been told what they
        // were. Nothing in the log was wrong, which is what cost the time.
        uploadMaterials();
        // MINTED, NOT ASKED, and the difference is the point: the level
        // registers with exact=false, so a colour within kQuantStep of one the
        // wood already owns costs NOTHING. "asked" made the level look like it
        // was spending fifteen entries out of the scarcest thing in this
        // engine, and it is not.
        std::printf("  level    %d colours asked, %d minted, %d greens on the grass ramp\n", asked,
                    palette.used() - before, greens);
        if (lost)
            std::fprintf(stderr,
                         "v2: PALETTE FULL -- %d level voxels came back AIR and will not draw\n",
                         lost);
        return true;
    }

    // -----------------------------------------------------------------------
    // THE PENDANT'S ART AND ITS TWO COLOURS -- at start-up, with the level.
    //
    // (user 2026-09-17: "use the same lightbulb from the original ui white
    // box".) That box is the pause room, which was deleted -- but the ART
    // survives on disk and the tracer's hook for it survives in the shader, so
    // this is the room's own bulb put somewhere new rather than a new one. The
    // glass colour, the saturation split, the cap-down flip and the flex are
    // all buildRoom's, recovered from the commit that removed the room.
    //
    // THE COLOURS ARE MINTED HERE AND THE GEOMETRY IS STAMPED LATER, which is
    // the same split the level itself runs on and for the same reason: palette
    // entries are served first-come, dressLevel does not run until the first
    // [O], and a colour asked for at that point is a colour handed mat::AIR.
    // The glass going AIR would be a room with an invisible light in it.
    //
    // AND THE GLASS IS ALSO THE SWITCH. V6Params::bulbMtl is one field for
    // "which material is the bulb" and "is the bulb on", so there is no state
    // where one is set and the other is not -- see the note in Shared.slang.
    void loadLevelBulb() {
        // A WARM WHITE, and its exact value is the room's. The tracer returns
        // bulbRadiance off any voxel wearing this, so what is registered here
        // mostly decides what the glass looks like when the lamp is OFF.
        // OUT OF THE LEVEL'S OWN TABLE, which is where it belongs -- there is
        // no lamp in the wood and this entry would be dead weight in the wood's
        // 255. loadLevelBulb is called from buildLevelPalette, after
        // beginLevelTable, so forLevelColor is live by the time this runs.
        levelGlassMtl_ = palette.forModelColor({255, 246, 214, 255}, false);
        // -- THE CAP AND THE FLEX COST NOTHING, AND THAT IS THE POINT --------
        //
        // mat::ROCK rather than a colour of their own. The table was FULL when
        // this went in -- three colours refused -- and a screw cap and a metre
        // of flex are the cheapest thing in the room to stop minting for: the
        // cap is a dozen voxels behind the glass and the flex is one voxel
        // wide. The stone ramp is six dark greys the device already spreads by
        // hash (see groundShade), which is a better cord than one flat grey
        // would have been anyway.
        //
        // NOT THE GLASS, obviously -- that has to be its own id because it IS
        // the emitter test.
        levelCordMtl_ = mat::ROCK;
        VoxModel mo;
        std::string err;
        if (!voxLoad(kLevelBulbVox, &mo, &err)) {
            std::fprintf(stderr, "v2: level bulb %s: %s -- using a plain sphere\n", kLevelBulbVox,
                         err.c_str());
            return;
        }
        const VoxAsset raw = toWorldWhole(mo);
        levelBulbAsset_.sx = raw.sx;
        levelBulbAsset_.sy = raw.sy;
        levelBulbAsset_.sz = raw.sz;
        levelBulbAsset_.a.assign(raw.a.size(), mat::AIR);
        for (int y = 0; y < raw.sy; ++y)
            for (int z = 0; z < raw.sz; ++z)
                for (int x = 0; x < raw.sx; ++x) {
                    const uint8_t e = raw.at(x, y, z);
                    if (!e) continue;
                    const std::array<uint8_t, 4> &c = mo.pal[size_t(e) - 1];
                    const int mx = maxi(c[0], maxi(c[1], c[2]));
                    const int mn = mini(c[0], mini(c[1], c[2]));
                    levelBulbAsset_.a[size_t(x) + size_t(z) * size_t(raw.sx) +
                                      size_t(y) * size_t(raw.sx) * size_t(raw.sz)] =
                        (mx - mn > 24) ? levelGlassMtl_ : levelCordMtl_;
                }
    }

    // -----------------------------------------------------------------------
    // WHAT GROWS ON THE MAP, AND WHAT HANGS IN IT -- stamped into a COPY.
    //
    // (user 2026-09-17: "I also want to have grass strands with flowers on the
    // grass as well in nuketown" / "put a lightbulb in the dark rooms".)
    //
    // -- THE COPY IS THE WHOLE DESIGN, NOT AN IMPLEMENTATION DETAIL ---------
    //
    // This runs on `a`, the display asset buildLevelBlas is about to mesh, and
    // NEVER on levelVol_. levelVol_ is the COLLIDER (see levelSolid, which
    // hands the walk both it and levelColTop_), so a blade of grass written
    // there is a blade of grass you climb: `blocked()` tests the body's box
    // against those voxels and `solidColumnTopBelow` would stand you on top of
    // the sward. The wood does not have this problem because its blades are
    // terrain geometry and its collider is a heightfield; here one array is
    // both, so the only place to put scenery is a copy of it.
    //
    // levelColTop_ is therefore also LEFT ALONE, and is read here rather than
    // recomputed -- it already says where the ground is, and the ground is the
    // one thing none of this may move.
    //
    // ONCE, LAZILY, on the first [O]. Everything it spends is already paid for:
    // the grass is mat::BGRASS_0 (the level's own greens map onto it at load),
    // the flowers are the wood's flower models with their palettes long since
    // registered, and the bulb's two colours were minted at start-up for the
    // reason written over levelGlassMtl_.
    // -----------------------------------------------------------------------
    void dressLevel(VoxAsset *a) {
        const int SX = a->sx, SY = a->sy, SZ = a->sz;
        if (SX <= 0 || SY <= 0 || SZ <= 0 || levelColTop_.empty()) return;
        auto at = [&](int x, int y, int z) -> uint8_t & {
            return a->a[size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ)];
        };
        auto solidAt = [&](int x, int y, int z) {
            return at(x, y, z) != mat::AIR;
        };

        // -- 1. GRASS, AND FLOWERS STANDING IN IT ---------------------------
        //
        // The oak wood's density, and for the oak wood's reason: a lawn is
        // sward from edge to edge, not a pine floor with glades in it. What is
        // NOT borrowed is the wood's patch field -- there is no noise here at
        // all, because a mown yard has no thickets and inventing some would be
        // the one thing that stops it reading as a lawn.
        //
        // HEIGHT IS THE VARIATION INSTEAD. Two to six voxels, 20 to 60 cm,
        // hashed per column. See v2-tall-grass: height never density.
        //
        // The blades take mat::BGRASS_0 like the ground under them, so
        // groundShade() hands every one of them its own shade out of the six --
        // which is also why no strand code is set here. A blade in the wood is
        // shaded by its ROW off the strand field the chunk mesher packs;
        // meshAsset packs none, so these fall through to the position hash and
        // come out as a mix rather than as a gradient. On a 3-voxel blade that
        // is the same picture.
        int blades = 0, flowers = 0;
        const int fl0 = mesher_.flowerBirch0;
        const int flN = int(flowers_.size()) - fl0;
        for (int z = 0; z < SZ; ++z)
            for (int x = 0; x < SX; ++x) {
                const int top = int(levelColTop_[size_t(x) + size_t(z) * size_t(SX)]) - 1;
                if (top < 0 || top >= SY - 4) continue;
                if (!isBGrass(at(x, top, z))) continue;
                const uint32_t h = hashU32(uint32_t(x) * 2654435761u, uint32_t(z) * 40503u);
                // A FLOWER FIRST, and it takes the column out of the running
                // for a blade: v1's own note is that a flower grows OUT of the
                // grass, but two things cannot stand in one voxel column and a
                // stem is greener than a blade is anyway.
                // -- AND THE SPECIES COMES FROM A PATCH, NOT FROM THE COLUMN --
                //
                // (user 2026-09-17: "make sure the flowers group together like
                // in the sandbox world. and not random.")
                //
                // THE PLACEMENT STAYS UNIFORM AND THAT IS THE POINT. This is
                // v1's flowerAt and the wood's own scatter, and the obvious
                // reading of "grouped" is the one both of them rejected: there
                // is no colony field gating PRESENCE. Every lawn column may
                // carry a flower at one flat rate, and what reads as a group is
                // A DRIFT OF ONE COLOUR INTO ANOTHER -- the species is constant
                // over a patch, so a dozen scattered plants of one kind read as
                // a bed without any of them being clumped. v1's own words:
                // "nothing draws the boundary, ~11 scattered plants do". The
                // wood tried the other structure three times and never got
                // there; see the note over Chunks::flowerDensity.
                //
                // kFlowerPatchVox is v1's FLWPATCH. It is a LENGTH, so it does
                // NOT scale with the map -- a 9.6 m bed is a 9.6 m bed whether
                // the houses round it are authored small or tripled.
                //
                // HALVED TWICE (user, both on 2026-09-17). 6/1024 was one
                // flower per ~170 lawn columns, 3/1024 one per ~340, and
                // 3/2048 is one per ~680. The mask is widened rather than the
                // numerator cut, because 3 -> 1.5 is not a thing a mask can say.
                if (flN > 0 && (h & 2047u) < 3u) {
                    const uint32_t cell = hashU32(uint32_t(x / kFlowerPatchVox) * 0x9E3779B9u,
                                                  uint32_t(z / kFlowerPatchVox) * 0x85EBCA6Bu);
                    const ModelTemplate &f = flowers_[size_t(fl0 + int(cell % uint32_t(flN)))];
                    if (f.sx > 0 && f.sy > 0 && f.sz > 0 && !f.volume.empty()) {
                        const int ox = x - f.sx / 2, oz = z - f.sz / 2;
                        for (int fy = 0; fy < f.sy; ++fy)
                            for (int fz = 0; fz < f.sz; ++fz)
                                for (int fx = 0; fx < f.sx; ++fx) {
                                    const uint8_t v =
                                        f.volume[size_t(fx) + size_t(fz) * size_t(f.sx) +
                                                 size_t(fy) * size_t(f.sx) * size_t(f.sz)];
                                    if (!v) continue;
                                    const int wx2 = ox + fx, wy = top + 1 + fy, wz = oz + fz;
                                    if (wx2 < 0 || wz < 0 || wx2 >= SX || wz >= SZ || wy >= SY)
                                        continue;
                                    if (solidAt(wx2, wy, wz)) continue;
                                    at(wx2, wy, wz) = v;
                                }
                        ++flowers;
                        continue;
                    }
                }
                // HALVED (user 2026-09-17: "reduce the grass frequency in
                // half"). 107/256 was the oak wood's 0.42; 53/256 is 0.21.
                if ((h >> 12 & 255u) >= 53u) continue;
                // ...AND TWO TO SIX VOXELS TALL, 20 to 60 cm. It shipped at
                // 1..3, went to 1..8 on the first ask and settled here on the
                // second: the single-voxel blades read as litter rather than
                // grass and the eight-voxel ones hid the map. Height is the
                // only thing that varies -- see the note above on why a mown
                // yard gets no patch field.
                const int hgt = 2 + int((h >> 20) % 5u);
                for (int k = 1; k <= hgt; ++k) {
                    if (top + k >= SY || solidAt(x, top + k, z)) break;
                    at(x, top + k, z) = mat::BGRASS_0;
                }
                ++blades;
            }

        // -- 2. THE LAMPS, AND EVERY ONE OF THEM IS PLACED BY HAND ----------
        //
        // (user 2026-09-17: "I want you to also remove every lightbulb from the
        // world, I will place them manually".)
        //
        // THERE WAS A SURVEY HERE AND IT IS GONE. It walked every column for
        // air gaps with standing room, flooded them into rooms by adjacency and
        // a shared ceiling, and hung a lattice of lamps in each -- and it was
        // deleted rather than tuned, because no amount of tuning answers the
        // real objection. "Too many lightbulbs on the field" is not a threshold
        // being wrong; it is a machine guessing at something the person can
        // simply say. A carport, an awning, the underside of a balcony and the
        // gap beside a parked bus are all a ceiling over a floor, and none of
        // them wants a pendant.
        //
        // WHAT REPLACES IT IS A LIST, filled two ways and drawn one way:
        //
        //   * kLevelBulbSeed below -- the baked positions, pasted in from the
        //     game. Empty until somebody places some.
        //   * placeLevelBulb / removeLevelBulbNear -- right click and left
        //     click, with the lamp in hand. See the level's kit block.
        //
        // The loop under this comment draws whatever is in the list, whichever
        // way it got there.
        //
        // THE SEED IS TAKEN ONCE. After that the list is the player's -- every
        // later dress runs because they placed or removed one, and re-seeding
        // would put the baked set back and throw the edit away.
        if (levelGlassMtl_ != mat::AIR && !levelBulbsSeeded_) {
            levelBulbsSeeded_ = true;
            for (const Vec3 &baked : levelBulbSeed()) levelBulbs_.push_back(baked);
        }
        for (const Vec3 &bulb : levelBulbs_) stampBulbWorld(a, bulb);
        std::printf("  level    dressed: %d grass blades, %d flowers, %zu bulbs\n", blades,
                    flowers, levelBulbs_.size());
        std::fflush(stdout);
    }

    // The pause room's pendant, rebuilt where the room's own buildRoom put it.
    //
    // THE MODEL IS AUTHORED CAP-DOWN and is read bottom-up here for it -- the
    // room's note is worth keeping because the symptom was a glowing ball with
    // its screw cap hanging underneath and the flex growing out of the glass.
    //
    // GLASS AND CAP ARE TOLD APART BY SATURATION, not by palette index, so a
    // re-authored bulb still splits the same way. Same test the level's greens
    // use, and the butterflies' yellow before either of them.
    // -- WHERE A LAMP GOES, AND IT IS AGAINST THE CEILING ----------------
    //
    // (user 2026-09-17: "have the lightbulbs attach to the cieling ... try to
    // put the lightbulbs in the center of the room at the top of the cieling".)
    //
    // The pause room hung its pendant 40 cm down on a flex, which is right for
    // a 6 m room with nothing else in it and wrong for a house: the bulbs stood
    // in mid-air with a string over them. So the model's TOP voxel sits on the
    // ceiling row and there is no flex to speak of -- the fitting is the thing
    // touching the slab.
    //
    // NOTE, DO NOT STAMP. This only records a position; stampBulbWorld draws
    // whatever is in the list. The two are separate so the player's edits and
    // the survey's findings go through the same door -- see dressLevel.
    void noteBulb(int bx, int bz, int ceilY, int SX, int SZ, int SY) {
        if (bx < 2 || bz < 2 || bx >= SX - 2 || bz >= SZ - 2) return;
        if (ceilY < 3 || ceilY >= SY - 1) return;
        if (int(levelBulbs_.size()) >= kLevelBulbMax) return;
        const int h = levelBulbAsset_.sx > 0 ? levelBulbAsset_.sy : 5;
        const int by = ceilY - h / 2;
        if (by < 2) return;
        levelBulbs_.push_back(Vec3(kLevelAtX + (float(bx) + 0.5f) * VOXEL_M,
                                   kLevelAtY + (float(by) + 0.5f) * VOXEL_M,
                                   kLevelAtZ + (float(bz) + 0.5f) * VOXEL_M));
    }

    // ...AND DRAW ONE THE LIST ALREADY HOLDS. The ceiling is found again here
    // rather than stored beside the position, so a lamp the player put under a
    // different slab still gets its fitting drawn against the right one.
    void stampBulbWorld(VoxAsset *a, const Vec3 &w) {
        const int SX = a->sx, SY = a->sy, SZ = a->sz;
        const int bx = int((w.x - kLevelAtX) / VOXEL_M);
        const int by = int((w.y - kLevelAtY) / VOXEL_M);
        const int bz = int((w.z - kLevelAtZ) / VOXEL_M);
        if (bx < 0 || by < 0 || bz < 0 || bx >= SX || by >= SY || bz >= SZ) return;
        auto solid = [&](int y) {
            return a->a[size_t(bx) + size_t(bz) * size_t(SX) +
                        size_t(y) * size_t(SX) * size_t(SZ)] != mat::AIR;
        };
        int roof = by;
        while (roof + 1 < SY && !solid(roof + 1)) ++roof;
        stampBulb(a, bx, by, bz, roof);
    }

    void stampBulb(VoxAsset *a, int bx, int by, int bz, int roof) {
        const int SX = a->sx, SY = a->sy, SZ = a->sz;
        auto at = [&](int x, int y, int z) -> uint8_t & {
            return a->a[size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ)];
        };
        auto put = [&](int x, int y, int z, uint8_t m) {
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return;
            if (at(x, y, z) != mat::AIR) return;
            at(x, y, z) = m;
        };
        int hiY = by;
        if (levelBulbAsset_.sx > 0) {
            const VoxAsset &b = levelBulbAsset_;
            const int x0 = bx - b.sx / 2, z0 = bz - b.sz / 2, y0 = by - b.sy / 2;
            hiY = y0 + b.sy - 1;
            for (int y = 0; y < b.sy; ++y)
                for (int z = 0; z < b.sz; ++z)
                    for (int x = 0; x < b.sx; ++x) {
                        const uint8_t m = b.at(x, b.sy - 1 - y, z);
                        if (!m) continue;
                        put(x0 + x, y0 + y, z0 + z, m);
                    }
        } else {
            // No art: a plain sphere, exactly as the room fell back to.
            for (int dy = -2; dy <= 2; ++dy)
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (dx * dx + dy * dy + dz * dz > 6) continue;
                        put(bx + dx, by + dy, bz + dz, levelGlassMtl_);
                    }
            hiY = by + 2;
        }
        // ...AND WHATEVER IS LEFT BETWEEN IT AND THE SLAB, which is now at
        // most a voxel or two of fitting rather than a flex -- see noteBulb.
        // Still drawn, because a bulb whose top lands one short of the ceiling
        // reads as floating.
        for (int y = hiY + 1; y <= roof; ++y) put(bx, y, bz, levelCordMtl_);
    }

    // -----------------------------------------------------------------------
    // THE LEVEL'S GEOMETRY, IN BLOCKS.
    //
    // (user 2026-09-17, three times: "the bullets are not making impact on the
    // terrain ... the bullets should be taking small chunks out of the
    // environment like the bow does".)
    //
    // IT WAS ONE BLAS AND THAT IS WHY IT COULD NOT BE CARVED. A chip has to
    // re-mesh whatever it damaged, and whatever it damaged was 136 M cells --
    // MEASURED at 383 ms, which is not a hitch, it is a stall. So a round hit
    // the map, `arrowChip` ran, `carveModel` refused it (the level's collider
    // has no chunk and no decor slot) and nothing happened at all. Three
    // reports, one cause, and the cause was the shape of the asset.
    //
    // CUT INTO 64-VOXEL COLUMNS -- full height, 6.4 m square -- the map is 128
    // blocks and one of them is about 1.2 M cells, which meshes in a few
    // milliseconds. That is a cost a weapon can pay.
    //
    // THE SEAMS COST NOTHING, and that is the one thing that had to be got
    // right: meshAssetBlock loops over the block and tests NEIGHBOURS against
    // the whole grid, so a face is emitted where solid meets air and never
    // where solid meets the next block. See its note.
    //
    // levelDisplay_ STAYS RESIDENT, which the single-BLAS version did not need:
    // a block rebuild has to re-read the dressed grid -- the grass, the flowers
    // and the lamps -- and re-dressing the map to carve a hole in a wall would
    // be the 383 ms back again. 130 MB for a level that is open, freed with the
    // rest of it when it is not.
    void buildLevelBlas() {
        if (!levelBlocks_.empty() || !levelLoaded_) return;
        // levelVol_ already holds GLOBAL material ids, so the mesher's table is
        // the identity -- the translation happened once, at load.
        levelDisplay_.sx = levelAsset_.sx;
        levelDisplay_.sy = levelAsset_.sy;
        levelDisplay_.sz = levelAsset_.sz;
        // A COPY, AND dressLevel IS WHY -- the grass, the flowers and the bulbs
        // go into this and never into levelVol_, which is what the body walks
        // into. See the long note over dressLevel.
        levelDisplay_.a = levelVol_;
        dressLevel(&levelDisplay_);

        levelBX_ = (levelAsset_.sx + kLevelBlockVox - 1) / kLevelBlockVox;
        levelBZ_ = (levelAsset_.sz + kLevelBlockVox - 1) / kLevelBlockVox;
        levelBlocks_.assign(size_t(levelBX_) * size_t(levelBZ_), Blas{});
        levelBlockTri_.assign(levelBlocks_.size(), TriPool::kInvalid);
        levelBlockTris_.assign(levelBlocks_.size(), 0);

        const auto tMesh = std::chrono::steady_clock::now();
        size_t total = 0;
        for (int bi = 0; bi < int(levelBlocks_.size()); ++bi) total += meshLevelBlock(bi);
        const double meshMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tMesh)
                .count();
        levelTris_ = total;
        if (total == 0) {
            std::fprintf(stderr, "v2: the level meshed to nothing\n");
            return;
        }
        std::printf("v2: level built, %zu triangles in %d x %d blocks (meshed in %.0f ms)\n", total,
                    levelBX_, levelBZ_, meshMs);
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // NOTHING IN THE LEVEL FLOATS EITHER.
    //
    // (user 2026-09-17: "im breaking a light pole from the static terrain and
    //  its floating. nothing in the world should be floating. everything that
    //  gets disconnected from the static terrain becomes a rigid body.")
    //
    // THE LEVEL WAS THE ONE STATIC THING WITH NO HANGER SWEEP. The terrain has
    // dropTerrainHangers and a model has dropModelHangers; the map had nothing
    // at all, so carveLevel cut voxels and never asked what the cut had left
    // standing on air. Shoot through a light pole and the whole pole above the
    // hole simply stayed where it was.
    //
    // WHY A LOCAL BOX CANNOT DO THIS, which is what makes it a different
    // problem from the terrain's. dropTerrainHangers works inside a 27-voxel
    // cube and calls anything touching the boundary still attached. A light
    // pole is four metres of geometry: cut its base and the severed piece runs
    // straight out of any such box, so that test would call it attached every
    // time -- which is exactly the bug being reported.
    //
    // SO THE QUESTION IS ASKED PROPERLY: CAN THIS REACH THE GROUND. The
    // voxeliser lays a solid foundation under the entire footprint (see
    // tools/voxelize_nuketown.py, BASE_M, and its note about there being no
    // ground outside the painted slabs), so grounded has an exact meaning here
    // that it does not have in the terrain: the flood reaches y == 0. Anything
    // that cannot is severed, whatever shape it is.
    //
    // BOUNDED BY A CAP, NOT BY A BOX. A flood out of a cut wall reaches the
    // whole building and would cost the map; it stops at kLevelHangCap and the
    // piece is then treated as ATTACHED. The two ways of being wrong are not
    // equal -- leaving a big piece standing is what the map does today, while
    // cutting a house loose because a flood ran out of budget is a building
    // falling out of the world.
    //
    // Returns how many pieces came away.
    // -----------------------------------------------------------------------
    int dropLevelHangers(Physics &ph, const Vec3 &at, int radiusVox, double nowMs) {
        if (levelVol_.empty() || levelDisplay_.a.empty()) return 0;
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        const int cx = int((at.x - kLevelAtX) / VOXEL_M);
        const int cy = int((at.y - kLevelAtY) / VOXEL_M);
        const int cz = int((at.z - kLevelAtZ) / VOXEL_M);
        const int r = maxi(1, radiusVox) + 2;
        auto idx = [&](int x, int y, int z) {
            return size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ);
        };
        auto solid = [&](int x, int y, int z) {
            if (x < 0 || y < 0 || z < 0 || x >= SX || y >= SY || z >= SZ) return false;
            return levelVol_[idx(x, y, z)] != mat::AIR ||
                   levelDisplay_.a[idx(x, y, z)] != mat::AIR;
        };

        // -- THE VISITED SET IS PER FLOOD, AND SHARING IT WAS THE RIP -------
        //
        // (user 2026-09-17, THREE times: "the bullet impact chunks are still
        //  causing a vertical rip".)
        //
        // IT USED TO BE ONE SET ACROSS EVERY FLOOD THIS SWEEP RUNS, on the
        // reasonable-sounding grounds that "a component reached from two
        // different seeds is walked once". That is true only for floods that
        // RUN TO COMPLETION, and most of these do not: a seed in a wall hits
        // kLevelHangSpan within a metre and breaks out CAPPED, having already
        // marked several hundred voxels as visited on the way.
        //
        // The next seed is the next solid voxel the scan reaches that is not
        // in that set -- which is to say, a voxel right beside the poisoned
        // blob. Its flood cannot enter the blob, so it runs along whatever
        // corridor is left around it; it cannot reach y == 0 through the
        // blockage; and being thin it never trips the span cap either. So it
        // is declared floating and dropped.
        //
        // A CORRIDOR AROUND A BLOB IN A WALL IS A LONG THIN SLIVER. That is
        // the vertical rip, exactly: not a piece the shot freed, a piece the
        // PREVIOUS FLOOD'S BOOKKEEPING invented. Neither kLevelHangSpan nor
        // kEmbeddedFrac could ever have fixed it, which is why tightening both
        // did not -- they are tests on a component, and the component being
        // handed to them was not real.
        //
        // So `seen` is cleared per seed, and what carries between floods is a
        // separate record of what has been DECIDED.
        std::unordered_set<uint32_t> seen;
        // -- ...AND WHAT IS ALREADY KNOWN TO BE HELD UP --------------------
        //
        // Every voxel of a component this sweep has already resolved as
        // attached, whether it was proved (it reached y == 0) or assumed (it
        // hit the cap). Two jobs, and it is important they are both done by
        // one set:
        //
        //   * a seed inside it is skipped, which is the cost saving the shared
        //     `seen` was reaching for and is all it was ever entitled to;
        //   * a flood that REACHES it is grounded on the spot, which is the
        //     part that was missing. The mass is attached, so anything joined
        //     to it is attached, and there is no need to walk it again to say
        //     so.
        //
        // Assumed-attached propagating as if it were proved is deliberate and
        // is the same bargain the cap already makes: leaving a big piece
        // standing is the status quo, cutting a house loose because a flood ran
        // out of budget is a building falling out of the world.
        std::unordered_set<uint32_t> attached;
        std::vector<int> stack, comp;
        int pieces = 0;

        for (int sy = maxi(0, cy - r); sy <= mini(SY - 1, cy + r) && pieces < kLevelHangMax; ++sy)
        for (int sz = maxi(0, cz - r); sz <= mini(SZ - 1, cz + r) && pieces < kLevelHangMax; ++sz)
        for (int sx = maxi(0, cx - r); sx <= mini(SX - 1, cx + r) && pieces < kLevelHangMax; ++sx) {
            if (!solid(sx, sy, sz)) continue;
            const uint32_t s0 = uint32_t(idx(sx, sy, sz));
            if (attached.count(s0)) continue;   // already decided -- see above

            // ---- one component, flooded until it grounds or runs out -------
            comp.clear();
            stack.clear();
            seen.clear();
            stack.push_back(int(s0));
            seen.insert(s0);
            bool grounded = false, capped = false;
            int lo[3] = {sx, sy, sz}, hi[3] = {sx, sy, sz};
            while (!stack.empty()) {
                const int k = stack.back();
                stack.pop_back();
                const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
                const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
                const int z = rem / SX, x = rem % SX;
                comp.push_back(k);
                if (y == 0) { grounded = true; break; }
                if (x < lo[0]) lo[0] = x;
                if (x > hi[0]) hi[0] = x;
                if (y < lo[1]) lo[1] = y;
                if (y > hi[1]) hi[1] = y;
                if (z < lo[2]) lo[2] = z;
                if (z > hi[2]) hi[2] = z;
                // THE BUDGET, NOT THE VERDICT -- see kLevelIslandSpan. This
                // used to be kLevelHangSpan, which is 1.2 m, so a 3.4 m light
                // pole could not survive the flood that was meant to judge it.
                if (int(comp.size()) >= kLevelHangCap || hi[0] - lo[0] >= kLevelIslandSpan ||
                    hi[1] - lo[1] >= kLevelIslandSpan || hi[2] - lo[2] >= kLevelIslandSpan) {
                    capped = true;
                    break;
                }
                // SIX-CONNECTED, not twenty-six. A piece joined to the map
                // only through a diagonal is joined by nothing anybody would
                // call a join, and treating that as attached is how a pole
                // ends up hanging by one corner.
                static const int kN[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                             {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
                bool met = false;
                for (const auto &nb : kN) {
                    const int nx = x + nb[0], ny = y + nb[1], nz = z + nb[2];
                    if (!solid(nx, ny, nz)) continue;
                    const uint32_t nk = uint32_t(idx(nx, ny, nz));
                    // TOUCHING SOMETHING ALREADY KNOWN TO BE HELD UP IS BEING
                    // HELD UP. See `attached` -- this is the memo that makes
                    // a per-flood visited set affordable.
                    if (attached.count(nk)) {
                        met = true;
                        break;
                    }
                    if (seen.count(nk)) continue;
                    seen.insert(nk);
                    stack.push_back(int(nk));
                }
                if (met) {
                    grounded = true;
                    break;
                }
            }
            if (grounded || capped) {
                // DECIDED, so neither this sweep's later seeds nor their floods
                // need to walk it again -- and, crucially, a later flood that
                // touches it is grounded rather than blocked. See `attached`.
                for (int k : comp) attached.insert(uint32_t(k));
                continue;
            }
            if (comp.empty()) continue;
            // -- ...AND IT HAS TO BE SOMETHING THIS SHOT BROKE ------------
            //
            // See markLevelGrounded. A component that could not reach the
            // ground BEFORE the carve either is one the map shipped floating;
            // freeing it is not enforcing nothing-floats, it is demolishing
            // the author's geometry because a round went past.
            bool wasHeld = false;
            if (!levelGrounded_.empty())
                for (int k : comp)
                    if (levelGrounded_[size_t(k)]) {
                        wasHeld = true;
                        break;
                    }
            if (!wasHeld) continue;
            // -- ...AND IT HAS TO BE A THING, NOT PART OF A WALL -----------
            //
            // (user 2026-09-17, twice: "its breaking things vertically, it
            //  should only leave the initial shot mark.")
            //
            // MEASURED, with the penetration audit: the piece a shot was
            // freeing is 2 x 2 x 25 -- a two-and-a-half metre rod, 75 voxels,
            // coming to rest with a third of itself inside the map. That is
            // the long vertical scar in the report: a trim strip or a pipe run
            // that sits in a shallow recess, touching the wall down its whole
            // length, cut free at the bottom by a round and dropped.
            //
            // AND IT IS NOT WRONG BY THE RULE, which is what makes it awkward:
            // that rod really was held up from below and really is disconnected
            // now. The light pole this sweep was written for is the SAME
            // event. The difference is not connectivity, it is what the thing
            // IS -- and there is a physical test for that.
            //
            // A FREE-STANDING THING TOUCHES THE WORLD AT ITS FEET. A pole in
            // the open has air on every side and one contact under it; a strip
            // let into a wall touches that wall along its entire length. So
            // count how many of the piece's voxels still have solid map beside
            // them, as a fraction of the piece. Few contacts is an object that
            // was standing there; many is masonry, and masonry stays.
            //
            // This is also the honest reading of "only leave the initial shot
            // mark": a hole in a wall should be a hole in a wall.
            {
                static const int kNb[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                              {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
                int touching = 0;
                for (int k : comp) {
                    const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
                    const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
                    const int z = rem / SX, x = rem % SX;
                    bool met = false;
                    for (const auto &nb : kNb) {
                        const int ax = x + nb[0], ay = y + nb[1], az = z + nb[2];
                        if (!solid(ax, ay, az)) continue;
                        // Its OWN voxels are not contacts -- seen holds the
                        // whole component by the time this runs.
                        if (seen.count(uint32_t(idx(ax, ay, az)))) continue;
                        met = true;
                        break;
                    }
                    if (met) ++touching;
                }
                // -- AN ISLAND FALLS, WHATEVER SIZE IT IS -----------------
                //
                // (user 2026-09-18: "the light pole is not being subject to
                //  gravity when cut from the static terrain".)
                //
                // ZERO contacts is not a small number of contacts, it is a
                // different fact: nothing anywhere in the map is beside this
                // piece, so it is not "probably detached", it is hanging in
                // air. That is the NOTHING FLOATS rule with no judgement left
                // in it, and it needs no size test -- a 34-voxel pole and a
                // 5-voxel finial are the same case.
                //
                // MEASURED: all six light poles in the map sever with exactly
                // 0 contacts. See kLevelHangSpan's note for the walls.
                if (touching > 0) {
                    // Still resting against the map somewhere, so "is this a
                    // severed thing or part of the building" is a real
                    // question rather than a settled one -- and the answer
                    // stays the conservative one it has been since the
                    // vertical scar: only a shot mark comes away.
                    const int spanX = hi[0] - lo[0] + 1, spanY = hi[1] - lo[1] + 1,
                              spanZ = hi[2] - lo[2] + 1;
                    if (maxi(spanX, maxi(spanY, spanZ)) > kLevelHangSpan) {
                        // DECIDED, and recorded as such. A component refused
                        // here is refused for this whole sweep, so a later
                        // seed inside it does not walk it again and a flood
                        // that REACHES it is grounded by it -- the same
                        // bargain `attached` already makes for a capped one.
                        for (int k : comp) attached.insert(uint32_t(k));
                        continue;
                    }
                    const float held = float(touching) / maxf(1.0f, float(comp.size()));
                    if (held > kEmbeddedFrac) {   // it is part of the building
                        for (int k : comp) attached.insert(uint32_t(k));
                        continue;
                    }
                }
            }

            // ---- it is standing on nothing, so it becomes a body -----------
            //
            // WHAT SHAPE IT IS, AND THEREFORE HOW MANY BODIES IT NEEDS -- and
            // all of it asked BEFORE a single voxel is removed. That ordering
            // is the fix for "the pole just deleted itself" and it is the only
            // ordering that can be right: the two statements below used to be
            // "take the piece out of the grid" and then "ask physics for a
            // slot", and the second one is allowed to say no.
            //
            // -- AND WHAT THE HULL WOULD LIE ABOUT HAS TO BE SPLIT ----------
            //
            // (user 2026-09-17: "things are glitching when they become rigid
            //  bodies.")
            //
            // EVERY BODY IN THIS ENGINE IS A CONVEX HULL. Physics::
            // addChunkBody builds a PxConvexMesh from the piece's voxel
            // corners, which is right for a CHIP -- a bite is a digital sphere
            // and a hull of one is the same sphere. It is wrong for whatever a
            // sever happens to cut loose: an L, a rail with a notch, a bracket.
            // The hull FILLS THE CONCAVITY, and the volume it fills is map
            // that was never removed -- so the body is born penetrating, and a
            // solver answers that with an impulse proportional to the depth.
            //
            // MEASURED by --fire-frame's penetration audit, which is what this
            // was written from: a 75-voxel piece with 25 of its voxels inside
            // the map. A third of it buried, and it is thrown.
            //
            // SO A PIECE THE HULL WOULD LIE ABOUT IS SPLIT. Fill ratio is the
            // test -- voxels over the bounding box that holds them -- and
            // octants are the split, the same 2x2x2 of the piece's own box
            // that shatterFlyer uses on a corpse and takeLevelBulbNear uses on
            // glass. Each octant is a contiguous corner, so each is far nearer
            // convex than the whole, and the sum of them is still the piece.
            //
            // A TIDY PIECE STAYS ONE BODY. Splitting everything would spend
            // eight of sixty-four debris slots on every post in the map for no
            // gain: a solid fragment's hull IS the fragment.
            const int bw = hi[0] - lo[0] + 1, bh2 = hi[1] - lo[1] + 1, bd = hi[2] - lo[2] + 1;
            const float fill =
                float(comp.size()) / maxf(1.0f, float(bw) * float(bh2) * float(bd));
            // -- ...BUT THE FILL RATIO ONLY ESTIMATES WHAT CAN BE ASKED -----
            //
            // (user 2026-09-18, the light pole.)
            //
            // The concern above is precise: the hull fills the piece's
            // concavities, and if map lives in one of them the body is born
            // penetrating. The fill ratio is a PROXY for that, and it is a bad
            // one for anything long -- a 3.4 m pole with a lamp on top scores
            // 0.32 to 0.39 against its own box purely because the lamp makes
            // the box wide, and would be shattered into octants on the way
            // down. A pole that bursts into eight pieces is subject to gravity
            // and still reads as wrong.
            //
            // THE EXACT QUESTION IS CHEAP HERE, so ask it instead: is there any
            // solid map left inside the piece's own bounding box that is not
            // the piece? The box is a superset of the hull, so a clear box
            // means the hull cannot be enclosing anything.
            //
            // ASKED AGAINST `seen` RATHER THAN AGAINST THE HOLE. This used to
            // run after the loop that empties the piece out of the grid, on the
            // grounds that anything still standing there was by definition not
            // part of it -- true, and it welded the shape question to the
            // removal. `seen` holds exactly the component by the time the flood
            // is done (see the contact test above, which already relies on it),
            // so the same answer comes out with nothing yet removed.
            //
            // MEASURED: all six light poles have a completely clear box, so all
            // six fall as poles. Nothing that fails this test changes.
            bool boxClear = true;
            for (int y = lo[1]; y <= hi[1] && boxClear; ++y)
                for (int z = lo[2]; z <= hi[2] && boxClear; ++z)
                    for (int x = lo[0]; x <= hi[0]; ++x)
                        if (solid(x, y, z) && !seen.count(uint32_t(idx(x, y, z)))) {
                            boxClear = false;
                            break;
                        }
            const bool whole = (fill >= kHullFillOk || boxClear);
            // -- HOW MANY SLOTS THIS PIECE WILL WANT ------------------------
            //
            // One for a piece that stays whole, or one per non-empty octant --
            // counted here with the same test the spawn loop below uses, off
            // the component's own coordinates rather than off the cube, so the
            // two cannot disagree about how many bodies there are going to be.
            const int hx = (bw + 1) / 2, hy2 = (bh2 + 1) / 2, hz = (bd + 1) / 2;
            int need = 1;
            if (!whole) {
                bool hasOct[8] = {false, false, false, false, false, false, false, false};
                need = 0;
                for (int k : comp) {
                    const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
                    const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
                    const int z = rem / SX, x = rem % SX;
                    const int o = ((x - lo[0] >= hx) ? 1 : 0) | ((y - lo[1] >= hy2) ? 2 : 0) |
                                  ((z - lo[2] >= hz) ? 4 : 0);
                    if (!hasOct[o]) {
                        hasOct[o] = true;
                        ++need;
                    }
                }
                if (need < 1) need = 1;
            }
            // -- ALL OF A PIECE OR NONE OF IT -------------------------------
            //
            // (user 2026-09-18: "on nuketown, the pole just deleted itself,
            //  instead of being subject to physics.")
            //
            // THE POOL IS SIXTY-FOUR BODIES AND A FIREFIGHT FILLS IT. Every
            // round leaves a chip that nobody collects and that lives
            // kLevelChipLifeMs -- a hundred seconds -- so sixty-four rounds,
            // which is thirteen seconds of trigger at kBulletIntervalMs, is a
            // full pool. Cut a light pole in that state and every guard above
            // passed it honestly, the voxels left the grid, spawnDebris was
            // asked for a slot, and there was not one. The pole was carved out
            // of the world and nothing was created. That is the vanishing bug
            // in [[v2-severed-terrain-becomes-a-body]] arriving through the one
            // door the RULE block over kMinBodyVoxels does not cover: the rule
            // makes the carve and the spawn ONE CALL, and a single call can
            // still half fail.
            //
            // IT COULD NOT BE SEEN FROM ANY TEST because every test drains the
            // pool between shots -- runRipTest and runPoleTest both say so in
            // as many words, for the good reason that otherwise they measure
            // kDebrisInstances. A firefight does not drain it.
            //
            // SO THE SLOTS ARE COUNTED FIRST, exactly as the float watch counts
            // them in stageFloatPiece -- "half a piece leaving the world is the
            // vanishing bug this whole rule exists to answer". Litter is
            // retired to make the room (see makeRoomForBodies); if even that
            // cannot find enough, the piece is left standing, which is the same
            // bargain every other refusal in this function makes.
            // THE PIECE ITSELF IS WHAT IT MAY EVICT -- see makeRoomForBodies.
            // A post takes the slot of a chip or a wall fragment and never the
            // slot of something bigger than it.
            if (debrisFree() < need) makeRoomForBodies(ph, need, int(comp.size()));
            if (debrisFree() < need) {
                if (carveLog) {
                    // WHY THERE WAS NO ROOM, not just that there was none. The
                    // eviction has four ways to pass over a body and they look
                    // identical from outside: this says which one it was.
                    int live = 0, litter = 0, big = 0, kept = 0, fell = 0, going = 0;
                    int minVox = 1 << 30, maxVox = 0;
                    for (int i = 0; i < kDebrisInstances; ++i) {
                        const Debris &d = debris_[i];
                        if (!d.live) continue;
                        ++live;
                        minVox = mini(minVox, d.voxels);
                        maxVox = maxi(maxVox, d.voxels);
                        if (d.felled || d.longBody) ++fell;
                        else if (d.absorbing) ++going;
                        else if (d.absorbR != kAbsorbNever || d.lifeMs <= 0.0) ++kept;
                        else if (d.voxels > int(comp.size())) ++big;
                        else ++litter;
                    }
                    std::fprintf(stderr,
                                 "v2: level hanger of %d voxels left standing -- %d wanted, %d "
                                 "free; of %d live: %d litter, %d too big, %d collectable, %d "
                                 "felled, %d absorbing; voxels %d..%d\n",
                                 int(comp.size()), need, debrisFree(), live, litter, big, kept,
                                 fell, going, minVox, maxVox);
                }
                for (int k : comp) attached.insert(uint32_t(k));
                continue;
            }

            // THE SAME CUBE CONTRACT dig() AND carveLevel USE -- an n-cube
            // indexed x + z*n + y*n*n holding the material of every voxel in
            // it. spawnDebris trims it to the piece's own bounds and puts the
            // body at its centre of mass, so a four-metre pole in a
            // four-metre cube is still a four-metre pole.
            const int n = maxi(hi[0] - lo[0], maxi(hi[1] - lo[1], hi[2] - lo[2])) + 1;
            std::vector<uint8_t> vol(size_t(n) * size_t(n) * size_t(n), mat::AIR);
            int b0x = levelBX_, b1x = -1, b0z = levelBZ_, b1z = -1;
            for (int k : comp) {
                const int y = int(size_t(k) / (size_t(SX) * size_t(SZ)));
                const int rem = int(size_t(k) % (size_t(SX) * size_t(SZ)));
                const int z = rem / SX, x = rem % SX;
                // THE DISPLAY GRID IS WHAT THE PIECE IS MADE OF -- the collider
                // grid carries no dressing. Same choice carveLevel makes for
                // its spoil, and the same reason.
                uint8_t m = levelDisplay_.a[size_t(k)];
                if (m == mat::AIR) m = levelVol_[size_t(k)];
                vol[size_t(x - lo[0]) + size_t(z - lo[2]) * size_t(n) +
                    size_t(y - lo[1]) * size_t(n) * size_t(n)] = m;
                levelDisplay_.a[size_t(k)] = mat::AIR;
                levelVol_[size_t(k)] = mat::AIR;
                if (!levelGrounded_.empty()) levelGrounded_[size_t(k)] = false;
                const int bx = x / kLevelBlockVox, bz = z / kLevelBlockVox;
                b0x = mini(b0x, bx);
                b1x = maxi(b1x, bx);
                b0z = mini(b0z, bz);
                b1z = maxi(b1z, bz);
            }
            const Vec3 centre{kLevelAtX + (float(lo[0]) + 0.5f * float(n)) * VOXEL_M,
                              kLevelAtY + (float(lo[1]) + 0.5f * float(n)) * VOXEL_M,
                              kLevelAtZ + (float(lo[2]) + 0.5f * float(n)) * VOXEL_M};
            const Vec3 still{0.0f, 0.0f, 0.0f};
            int slot = -1;
            if (whole) {
                slot = spawnDebris(ph, vol, n, centre, still, still, nowMs, 0.0f, nullptr,
                                   uint8_t(kDebrisStone), kArrowAbsorbM);
                markLeftLying(slot, kLevelChipLifeMs);   // see carveLevelToBody
            } else {
                // hx / hy2 / hz are the ones the slot count above was taken
                // with -- one definition, so the number of bodies reserved and
                // the number spawned cannot drift apart.
                for (int oct = 0; oct < 8; ++oct) {
                    std::vector<uint8_t> part(vol.size(), mat::AIR);
                    int pn = 0;
                    for (int y = 0; y < n; ++y)
                        for (int z = 0; z < n; ++z)
                            for (int x = 0; x < n; ++x) {
                                const size_t kk = size_t(x) + size_t(z) * size_t(n) +
                                                  size_t(y) * size_t(n) * size_t(n);
                                if (vol[kk] == mat::AIR) continue;
                                const int o = ((x >= hx) ? 1 : 0) | ((y >= hy2) ? 2 : 0) |
                                              ((z >= hz) ? 4 : 0);
                                if (o != oct) continue;
                                part[kk] = vol[kk];
                                ++pn;
                            }
                    if (!pn) continue;
                    const int sl = spawnDebris(ph, part, n, centre, still, still, nowMs, 0.0f,
                                               nullptr, uint8_t(kDebrisStone), kArrowAbsorbM);
                    markLeftLying(sl, kLevelChipLifeMs);   // see carveLevelToBody
                    if (sl >= 0) slot = sl;
                }
            }
            // THE MAP IS RE-MESHED WHETHER OR NOT A BODY TOOK, because the
            // voxels have left the grid either way -- and geometry that is
            // still drawn where nothing is any more is worse than a piece that
            // failed to fall.
            for (int bz = maxi(0, b0z); bz <= mini(levelBZ_ - 1, b1z); ++bz)
                for (int bx = maxi(0, b0x); bx <= mini(levelBX_ - 1, b1x); ++bx)
                    meshLevelBlock(bz * levelBX_ + bx);
            // ...AND THE COLUMN TOPS IT STOOD IN. The walk stands on these; a
            // pole that has fallen must stop being something to step onto.
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int x = lo[0]; x <= hi[0]; ++x) {
                    int top = 0;
                    for (int y = SY - 1; y >= 0; --y)
                        if (levelVol_[idx(x, y, z)] != mat::AIR) {
                            top = y + 1;
                            break;
                        }
                    levelColTop_[size_t(x) + size_t(z) * size_t(SX)] = int16_t(top);
                }
            ++pieces;
            // CUMULATIVE, AND THAT MATTERS. This was per-piece with the last
            // one winning, so a 75-voxel rod freed by one shot was hidden
            // behind a 1-voxel speck freed by the next -- and the report read
            // "1 voxels cut loose" while something 2.5 m long was falling out
            // of the map. A total and a worst say what actually happened.
            lastHangVox_ += int(comp.size());
            if (int(comp.size()) > lastHangWorst_) {
                lastHangWorst_ = int(comp.size());
                lastHangDim_[0] = hi[0] - lo[0] + 1;
                lastHangDim_[1] = hi[1] - lo[1] + 1;
                lastHangDim_[2] = hi[2] - lo[2] + 1;
            }
            ++lastHangPieces_;
            lastHangSlot_ = slot;
            if (carveLog) {
                std::printf("v2: level hanger -- %d voxels cut loose, body slot %d\n",
                            int(comp.size()), slot);
                std::fflush(stdout);
            }
        }
        if (pieces) rebuildTlas();
        return pieces;
    }

    // One block, from the dressed grid. Returns how many triangles it holds.
    // Releasing first is what makes this callable again on a block that already
    // has geometry -- which is the whole point of it.
    size_t meshLevelBlock(int bi) {
        if (bi < 0 || bi >= int(levelBlocks_.size())) return 0;
        if (levelBlockTri_[size_t(bi)] != TriPool::kInvalid) {
            pool_.release(levelBlockTri_[size_t(bi)], levelBlockTris_[size_t(bi)]);
            levelBlockTri_[size_t(bi)] = TriPool::kInvalid;
        }
        levelBlocks_[size_t(bi)] = Blas{};
        levelBlockTris_[size_t(bi)] = 0;
        // -- THE IDENTITY MAP COVERS EVERY MATERIAL, NOT THE FIRST 256 ------
        //
        // THIS CRASHED [O] (user 2026-09-17: "typing o crashed the game"). It
        // was `ident(256)` filled 1..255 -- a literal, not a bound. A 16-bit
        // material id was briefly in the tree at the time, and meshAssetBlock's
        // `idOfEntry[v]` indexed that 256-element vector with a five-figure id
        // and read off the end. The widening was reverted the same day, so a
        // byte id fits again and 256 would work TODAY -- which is exactly why
        // this is written against mat::COUNT instead. The literal was only ever
        // right by coincidence, and the coincidence has already broken once.
        //
        // NOT A "WIDEN EVERY 256" BUG. Every OTHER idOfEntry(256) in this file
        // is still 256 and still CORRECT, because those are indexed by a .vox
        // palette ENTRY, which is 1..255 by the file format. This one is
        // indexed by an ENGINE id, and the two look identical at the call site.
        //
        // BUILT ONCE -- mat::COUNT entries is 128 KB, nothing to hold and
        // something to rebuild 128 times at [O] and again on every bullet.
        if (levelIdent_.empty()) {
            levelIdent_.assign(size_t(mat::COUNT) + 1, mat::AIR);
            for (size_t i = 1; i < levelIdent_.size(); ++i)
                levelIdent_[i] = decltype(levelIdent_)::value_type(i);
        }
        const int bx = bi % levelBX_, bz = bi / levelBX_;
        const VoxMesh mesh =
            meshAssetBlock(levelDisplay_, levelIdent_, VOXEL_M, bx * kLevelBlockVox, 0,
                           bz * kLevelBlockVox, kLevelBlockVox, levelAsset_.sy, kLevelBlockVox);
        if (mesh.triCount() == 0) return 0;   // an empty block is a legal block
        levelBlockTri_[size_t(bi)] = pool_.upload(ctx_, mesh.tri);
        levelBlocks_[size_t(bi)] = buildBlas(mesh);
        levelBlockTris_[size_t(bi)] = mesh.triCount();
        return mesh.triCount();
    }

    // -----------------------------------------------------------------------
    // TAKE A CHIP OUT OF THE MAP.
    //
    // BOTH GRIDS, and that is not a duplicate: levelVol_ is what the BODY walks
    // into and levelDisplay_ is what the EYE sees, and a hole that is only in
    // one of them is either a wall you can see through and not walk through or
    // the other way round. dressLevel's note explains why they are two arrays.
    //
    // A SPHERE, like every other carve in this engine -- see World::dig, whose
    // radius this shares so a bullet hole and an arrow's chip are the same size.
    //
    // ONLY THE BLOCKS IT TOUCHED are rebuilt, which is normally one and at a
    // corner is four.
    bool carveLevel(const Vec3 &at, int radiusVox, std::vector<uint8_t> *spoil = nullptr,
                    int *spoilN = nullptr, Vec3 *spoilAt = nullptr) {
        if (levelBlocks_.empty() || levelDisplay_.a.empty()) return false;
        const int cx = int((at.x - kLevelAtX) / VOXEL_M);
        const int cy = int((at.y - kLevelAtY) / VOXEL_M);
        const int cz = int((at.z - kLevelAtZ) / VOXEL_M);
        const int r = maxi(1, radiusVox);
        const int SX = levelAsset_.sx, SY = levelAsset_.sy, SZ = levelAsset_.sz;
        // -- WHAT CAME OUT, AS VOXELS -- dig()'s own contract, word for word --
        //
        // A (2r+1) CUBE indexed x + z*n + y*n*n, holding the material of every
        // voxel actually removed and AIR everywhere else. That is the layout
        // meshVolume reads and spawnDebris expects, so the piece that falls out
        // of a wall here is built by the same code that builds the one an arrow
        // knocks off a boulder -- which is the ask.
        //
        // SAMPLED AS IT CARVES. dig() samples first and then carves; this
        // cannot, because the level has no generator to ask afterwards -- the
        // grid IS the record.
        if (spoil && spoilN) {
            const int sn = r * 2 + 1;
            *spoilN = sn;
            spoil->assign(size_t(sn) * size_t(sn) * size_t(sn), mat::AIR);
        }
        if (spoilAt)
            *spoilAt = Vec3{kLevelAtX + (float(cx) + 0.5f) * VOXEL_M,
                            kLevelAtY + (float(cy) + 0.5f) * VOXEL_M,
                            kLevelAtZ + (float(cz) + 0.5f) * VOXEL_M};
        bool any = false;
        int b0x = levelBX_, b1x = -1, b0z = levelBZ_, b1z = -1;
        for (int y = cy - r; y <= cy + r; ++y) {
            if (y < 0 || y >= SY) continue;
            for (int z = cz - r; z <= cz + r; ++z) {
                if (z < 0 || z >= SZ) continue;
                for (int x = cx - r; x <= cx + r; ++x) {
                    if (x < 0 || x >= SX) continue;
                    const int dx = x - cx, dy = y - cy, dz = z - cz;
                    if (dx * dx + dy * dy + dz * dz > r * r) continue;
                    const size_t k = size_t(x) + size_t(z) * size_t(SX) +
                                     size_t(y) * size_t(SX) * size_t(SZ);
                    if (levelDisplay_.a[k] == mat::AIR && levelVol_[k] == mat::AIR) continue;
                    // THE DISPLAY GRID IS WHAT THE PIECE IS MADE OF, not the
                    // collider: a chip off a wall should carry the wall's own
                    // colour, and only that grid has the dressing in it.
                    if (spoil && spoilN && levelDisplay_.a[k] != mat::AIR) {
                        const size_t sn = size_t(*spoilN);
                        (*spoil)[size_t(dx + r) + size_t(dz + r) * sn + size_t(dy + r) * sn * sn] =
                            levelDisplay_.a[k];
                    }
                    levelDisplay_.a[k] = mat::AIR;
                    levelVol_[k] = mat::AIR;
                    any = true;
                    const int bx = x / kLevelBlockVox, bz = z / kLevelBlockVox;
                    b0x = mini(b0x, bx);
                    b1x = maxi(b1x, bx);
                    b0z = mini(b0z, bz);
                    b1z = maxi(b1z, bz);
                }
            }
        }
        if (!any) return false;
        // THE NEIGHBOUR OF A HOLE ON A SEAM ALSO CHANGES, because a voxel
        // removed at a block's edge uncovers a face that belongs to the block
        // NEXT to it. So the range is grown by ONE VOXEL and then mapped to
        // blocks -- which reaches into the neighbour only when the hole is
        // actually against the boundary.
        //
        // IT WAS GROWN BY A WHOLE BLOCK and that was nine blocks every time:
        // 90 ms a round, measured, for a hole that touches one. A bullet in the
        // middle of a wall now costs one block and about 10.
        b0x = maxi(0, mini(b0x, (maxi(0, cx - r - 1)) / kLevelBlockVox));
        b0z = maxi(0, mini(b0z, (maxi(0, cz - r - 1)) / kLevelBlockVox));
        b1x = mini(levelBX_ - 1, maxi(b1x, mini(SX - 1, cx + r + 1) / kLevelBlockVox));
        b1z = mini(levelBZ_ - 1, maxi(b1z, mini(SZ - 1, cz + r + 1) / kLevelBlockVox));
        size_t total = 0;
        const auto tRe = std::chrono::steady_clock::now();
        int rebuilt = 0;
        for (int bz = b0z; bz <= b1z; ++bz)
            for (int bx = b0x; bx <= b1x; ++bx) {
                meshLevelBlock(bz * levelBX_ + bx);
                ++rebuilt;
            }
        for (size_t t : levelBlockTris_) total += t;
        levelTris_ = total;
        // WHAT A CHIP ACTUALLY COST, because the whole reason the level is cut
        // into blocks is that this number used to be 383 ms for one hole.
        if (carveLog) {
            std::printf("v2: carve -- %d blocks re-meshed in %.1f ms, %zu triangles now\n",
                        rebuilt,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - tRe)
                            .count(),
                        total);
            std::fflush(stdout);
        }
        // THE COLUMN TOPS MOVED, so the walk has to be told: a hole shot in a
        // floor is a hole you can fall through, and levelColTop_ is what the
        // collider stands on. Only the columns that changed.
        for (int z = maxi(0, cz - r); z <= mini(SZ - 1, cz + r); ++z)
            for (int x = maxi(0, cx - r); x <= mini(SX - 1, cx + r); ++x) {
                int top = 0;
                for (int y = SY - 1; y >= 0; --y)
                    if (levelVol_[size_t(x) + size_t(z) * size_t(SX) +
                                  size_t(y) * size_t(SX) * size_t(SZ)] != mat::AIR) {
                        top = y + 1;
                        break;
                    }
                levelColTop_[size_t(x) + size_t(z) * size_t(SX)] = int16_t(top);
            }
        rebuildTlas();
        return true;
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
    //
    // A DISC, NOT A SQUARE (user 2026-09-13: "make the terrain renderer into
    // that of a circle instead of a square").
    //
    // It was called a ring everywhere in this file and was never one: the two
    // loops below walked [-R, R] squared, so the world reached R chunks ahead
    // along an axis and R*sqrt(2) along a diagonal. Standing still and turning
    // on the spot, the far edge of the world therefore MOVED IN AND OUT by 41%
    // -- which is what you actually see, because the edge is where the fog
    // meets nothing.
    //
    // The test is on the chunk INDEX, which makes the boundary a staircase of
    // whole chunks rather than a true circle; at 25.6 m a chunk that is exactly
    // right, because a chunk is the unit that exists or does not.
    //
    // WHAT IT COSTS, AND IT IS NEGATIVE. A disc of radius R holds pi/4 of the
    // square -- at R = 12, 441 chunks against 625 -- so this is 29% FEWER
    // chunks to mesh, upload and keep in the TLAS for the same view distance
    // straight ahead. The corners it drops were the furthest, haziest and most
    // expensive part of the frame.
    bool rering(int cx, int cz) {
        const int R = maxi(1, viewChunks);
        const int R2 = R * R;
        wanted_.clear();
        // The disc's size is known and does not change, so the table is sized
        // once and never rehashes mid-fill. The SAME count the pool is sized
        // from -- see discChunks.
        wanted_.reserve(discChunks(R));
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) {
                if (i * i + j * j > R2) continue;
                wanted_.insert(chunkKey(cx + i, cz + j));
            }

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
                // The same disc as above, and it HAS to be the same test: a
                // chunk requested here but not wanted there is meshed, adopted
                // and evicted on the next rering, forever.
                const int d2 = i * i + j * j;
                if (d2 > R2) continue;
                const long long k = chunkKey(cx + i, cz + j);
                if (chunks_.count(k) || requested_.count(k)) continue;
                order.push_back({d2, {cx + i, cz + j}});
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
                if (p.kind >= 0 && p.kind < 7) ++c.decorKind[p.kind];
                // A HIVE IS NOT WALKED INTO EITHER. It hangs several metres up
                // in a crown, so a ground collider for it would be an invisible
                // wall under the tree.
                // ...AND A FRUIT (kind 6), for the reason a cone is: it hangs in
                // a crown out of reach, and a 40 cm collider up a tree is a
                // thing to snag on rather than a thing to stand on.
                const bool walkThrough =
                    (p.kind == 2 || p.kind == 4 || p.kind == 5 || p.kind == 6);
                // The slot this instance is about to take, recorded on its
                // Solid so a blow can find its way back here. solids is a
                // FILTERED subset of decorDesc, so its own index is no use.
                s.ownerChunk = key;
                s.decorSlot = int32_t(c.decorDesc.size());
                c.decorDesc.push_back(makeInstance(p, &info, walkThrough ? nullptr : &s));
                c.decorInfo.push_back(info);
                {
                    // The same halfOf makeInstance subtracted -- read back off
                    // the model it placed rather than recomputed, so the two
                    // cannot drift. See DecorAt.
                    const ModelTemplate &mt = templateFor(p.kind, p.index);
                    DecorAt da{uint8_t(p.kind), uint16_t(p.index),
                               c.decorDesc.back().transform[0][3],
                               c.decorDesc.back().transform[1][3],
                               c.decorDesc.back().transform[2][3], 0.0f, 0.0f};
                    // The same two lines makeInstance turns the model by.
                    da.hx = halfOf((p.yaw & 1) ? mt.sz : mt.sx);
                    da.hz = halfOf((p.yaw & 1) ? mt.sx : mt.sz);
                    c.decorAt.push_back(da);
                    // WHERE THE ORCHARD IS, so the load report can say where
                    // to look. A crop hangs several metres up inside a crown
                    // and is 40 cm across, which makes "is the fruit there"
                    // a question a screenshot answers badly and a coordinate
                    // answers exactly. Capped, so this costs one compare per
                    // placement once the first few are in.
                    if (p.kind == 6 && fruitSeen_.size() < 4)
                        fruitSeen_.push_back({da.x, da.y, da.z});
                }
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
            // -- ...AND IT TRIES NOT TO STALL BEFORE IT STALLS --------------
            //
            // (user 2026-09-15: "just as im walking around the environment its
            // hitching".)
            //
            // THIS WENT STRAIGHT TO THE BLOCKING FORM, and `force` means
            // syncPoint, which means ctx_->submit(true) -- the main thread
            // waiting for the GPU to finish everything it has been given.
            // MEASURED on a walk: one frame at 75.6 ms, of which 70.3 was this.
            //
            // The note over kUncompactedBudget says the trade is meant to be
            // paid "in memory instead of in frames", and the valve was paying
            // in both: the budget is what bounds the memory, and then closing
            // it bought a full pipeline stall as well.
            //
            // A PLAIN DRAIN NEEDS NO STALL AT ALL. It compacts every group the
            // device has ALREADY finished -- the epoch test in the loop -- and
            // by the time a group matters the device is normally a frame or two
            // past it, so the memory comes back for nothing. Only if that frees
            // so little that we are still over budget has the valve genuinely
            // closed, and only then is a stall the honest answer.
            //
            // THE SECOND TEST IS NOT REDUNDANT. Without it this is the same
            // stall one line later; with it, the stall becomes the rare case it
            // was always described as.
            if (uncompactedBytes() >= kUncompactedBudget) {
                drainCompactions(false);
                if (uncompactedBytes() >= kUncompactedBudget) drainCompactions(true);
            }
        }
    }

    const ModelTemplate &templateFor(int kind, int index) const {
        const std::vector<ModelTemplate> &v =
            (kind == 0)   ? pines_
            : (kind == 1) ? rocks_
            : (kind == 2) ? flowers_
            : (kind == 3) ? mushrooms_
            : (kind == 5) ? hives_
            : (kind == 6) ? fruit_
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
    // -- WHAT THE MOTION VECTOR IS MEASURED FROM, AND WHY IT IS A PARAMETER -
    //
    // prevOffset is "how far did this instance travel since the last frame",
    // and the point it is measured at is the centre of the model's BOUNDING
    // BOX. That is right for anything whose box is a property of the object --
    // which is everything that keeps one mesh.
    //
    // IT IS WRONG FOR ANYTHING ANIMATED BY SWAPPING MESHES, and the songbirds
    // are the worst case in the engine. Their eleven frames are a quarter turn,
    // so the FOOTPRINT turns with them -- measured on all three species, frame
    // 00 is 3 x 7 x 6 voxels and frame 05 is 6 x 7 x 3. publish() compensates
    // the translation so the BODY stays centred on its perch, and then this
    // adds half of a box that just changed size back on: the box centre moves
    // up to 0.15 m in x and 0.15 m in z at every pose step, fifteen times a
    // second, about a bird that is sitting perfectly still.
    //
    // A tenth of a metre of motion vector on a bird twenty metres away is
    // several pixels of it, pointing at the sky behind it. DLSS then resolves
    // the bird against whatever is there, and the report is exactly what that
    // looks like: "the perched song birds are flickering ... when I put DLSS on
    // with no upscale the flickering goes away" -- at DLAA every output pixel
    // has a sample of its own this frame and the bad history is outvoted; at
    // Balanced it is 58% of them and the history is most of the answer.
    //
    // So a caller that knows a point FIXED TO THE ANIMAL passes it: the perch
    // for a bird, the body centre for a fish. The pose may then change shape
    // however it likes and the instance still reports the motion the animal
    // actually had. The turn inside the art is described separately, by the
    // spin channel -- see setFlyerInstance and V6Instance::flapPad.
    void place(size_t idx, const float *m, float tx, float ty, float tz, uint32_t mask, bool show,
               float hx, float hy, float hz, bool track = true, const float *anchor = nullptr) {
        RtInstanceDesc &inst = instanceDescs_[idx];
        writeTransform(inst, m, tx, ty, tz);
        inst.instanceMask = show ? mask : 0;
        inst.instanceID = uint32_t(idx);

        const float3 at = anchor ? float3(anchor[0], anchor[1], anchor[2])
                                 : float3(tx + m[0] * hx + m[1] * hy + m[2] * hz,
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
        // THE PAUSE ROOM USED TO REPLACE IT AS WELL, on the argument that a
        // pause menu you can see the wood through is a HUD and not a room. That
        // was overruled (user 2026-09-14: "remove the esc room from the sky.
        // instead, put the 3 balls in front of the player IN GAME") -- the three
        // buttons stand in the wood now, on the flyer band, and there is no
        // second world to swap to. buildRoom and its shell are gone with it.
        if (level_) {
            // THE LEVEL REPLACES THE WORLD TOO, for the reason above the stage
            // branch below gives: "this level is in a seperate world from the
            // main world" is a statement about what a RAY can find, and the
            // only way to mean it is for the wood not to be in the structure.
            // One instance -- the building and its slab are one asset and one
            // BLAS, because they are one place.
            // ONE INSTANCE PER BLOCK, not one for the place. The level is cut
            // into 64-voxel columns so a bullet can re-mesh what it hit without
            // re-meshing the map -- see buildLevelBlas. Each block carries its
            // own corner in the transform; an empty one has no structure and is
            // skipped, which is most of the sky above this map.
            for (size_t bi = 0; bi < levelBlocks_.size(); ++bi) {
                if (!levelBlocks_[bi].valid()) continue;
                const int bx = int(bi) % levelBX_, bz = int(bi) / levelBX_;
                RtInstanceDesc lv = {};
                writeTransform(lv, kI, kLevelAtX + float(bx * kLevelBlockVox) * VOXEL_M, kLevelAtY,
                               kLevelAtZ + float(bz * kLevelBlockVox) * VOXEL_M);
                lv.instanceMask = kMaskWorld;
                lv.accelerationStructure = levelBlocks_[bi].as->getGpuAddress();
                V6Instance info{};
                info.triOffset = levelBlockTri_[bi];
                info.kind = KIND_TERRAIN;
                info.tint = float3(1.0f, 1.0f, 1.0f);
                push(lv, info);
            }
        } else if (stage_) {
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
