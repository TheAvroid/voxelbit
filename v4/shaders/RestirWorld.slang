// ---------------------------------------------------------------------------
// RestirWorld.slang -- reservoirs that live on the WORLD instead of on the
//                      screen, so turning around does not throw them away.
//
// WHAT THIS FIXES. RestirTemporal.cs.slang reuses last frame's reservoir for a
// pixel by reprojecting it, and rejects the reprojection when the depth or the
// normal says the pixel is now looking at something else. That rejection is
// correct and it is also the technique's largest remaining cost: everything the
// pixel had learnt is discarded, and the pixel falls back to a one-sample path
// trace for as long as it takes to build a new history. Walk out from behind a
// trunk and the strip of world it uncovers is the noisiest thing in the frame.
//
// The history was not wrong. It was filed in the wrong place. A reservoir
// belongs to the SURFACE the pixel was looking at, not to the pixel, and a
// pixel is simply the worst possible place to keep one -- it is an index into a
// grid that slides around underneath the world every time the camera moves.
//
// WHY A VOXEL WORLD CAN DO THIS AND A TRIANGLE SCENE CANNOT. Keeping reservoirs
// in world space needs somewhere to put them: a finite, stable set of surface
// locations that both the writer and the reader can name without having to
// agree on a parameterisation. Triangle renderers have no such thing, which is
// why world-space ReSTIR is an open problem rather than a feature. Here it is
// just the voxel face -- see VoxelKey.slang. The same key the radiance cache
// files under.
//
// ---------------------------------------------------------------------------
// USED ONLY WHERE THE SCREEN HISTORY FAILED, AND THAT RESTRICTION IS WHAT KEEPS
// IT UNBIASED.
//
// restir.h has the long version of why the spatial output is never fed back:
// restirMerge weights a reservoir by its M, which is only unbiased if the two
// reservoirs are INDEPENDENT, and a shared structure is exactly how independence
// is lost. Two pixels on one face reading and writing one reservoir would build
// the same widening web of paths that cost the engine 2.4x the light it should
// have had.
//
// So the world map is NOT a second history that every pixel merges every frame.
// It is consulted only when the reprojection was rejected -- when this pixel has
// no history of its own at all, and therefore nothing for a shared sample to be
// correlated WITH. The frame after that the pixel has its own screen history
// again and stops reading the map. The loop never closes.
//
// The M it is allowed to contribute is capped harder than the screen history
// for the same reason: a face that has been accumulating for a thousand frames
// would otherwise arrive with enough confidence to freeze the pixel it lands
// on, which is the runaway restirCapM exists to prevent.
//
// ---------------------------------------------------------------------------
// THE MAP IS OPEN-ADDRESSED WITH A LOCK WORD, WHICH IS NOT THE OBVIOUS CHOICE.
//
// The obvious one is a 64-bit compare-exchange on the key, and it is what SHaRC
// would do -- but only when SHARC_ENABLE_64_BIT_ATOMICS is on, and this engine
// runs it off. Sharc.slang has the reason: 64-bit atomics need shader model 6.6
// on D3D12 and a device feature on Vulkan that slang-gfx does not enable,
// because slang-gfx creates the device and v2 does not. So this map uses the
// same fallback the SDK does -- one 32-bit lock word per slot, taken with
// InterlockedExchange, with a bounded fuse so a contended slot can never hang
// the device.
//
// A LOST RACE IS A DROPPED SAMPLE, NOT A WRONG ONE. Every operation here fails
// closed: an insert that cannot take the lock gives up, and a find that cannot
// take it reports a miss. Both leave the pixel doing what it would have done
// without the map at all, which is the only failure mode a noise-reduction
// structure is allowed to have.
// ---------------------------------------------------------------------------
#pragma once

#include "Restir.slang"
#include "VoxelKey.slang"

// Same bucket width as RTXGI's hash map. A key hashes to a base slot and the
// probe walks this many from it before giving up; wider costs a longer walk on
// every miss, narrower drops entries while the map is still mostly empty.
static const uint kRestirWorldBucket = 16u;

// A slot is free, or holds a key. The lock word is separate so the key can be
// read and written as an ordinary 64-bit value while the lock is held.
RWStructuredBuffer<uint64_t> gWorldKeys;
RWStructuredBuffer<uint> gWorldLock;
RWStructuredBuffer<Reservoir> gWorldRes;

cbuffer RestirWorldCB {
    uint gWorldCapacity;
    uint gWorldMaxM;      // how much confidence a world reservoir may carry in
    uint gWorldEnabled;
    uint gWorldPad;
};

// Take a slot's lock, or fail. The fuse is the SDK's: a bounded number of
// attempts, because an unbounded spin on a GPU is a hang rather than a wait --
// the threads holding the lock may be in the same wave as the ones waiting for
// it and can never be scheduled to release it.
bool worldTryLock(uint slot)
{
    const uint cLock = 0xAAAAAAAAu;
    uint state;
    InterlockedExchange(gWorldLock[slot], cLock, state);
    return state != cLock;
}

void worldUnlock(uint slot)
{
    uint ignored;
    InterlockedExchange(gWorldLock[slot], 0u, ignored);
}

// Find the slot holding this key, if the map has one. Read-only and lock-free:
// a torn read here can only produce a reservoir belonging to another face,
// which restirMerge will weight by its own target function and which the
// caller's rejection tests have already decided is plausible. It is a sample,
// not a decision.
bool worldFind(uint64_t key, out uint slot)
{
    const uint base = voxelKeyHash(key) % gWorldCapacity;
    for (uint i = 0; i < kRestirWorldBucket; ++i) {
        const uint s = (base + i) % gWorldCapacity;
        if (gWorldKeys[s] == key) {
            slot = s;
            return true;
        }
    }
    slot = 0u;
    return false;
}

// Claim the slot for this key, inserting it if the map does not have it yet.
// Fails when the bucket is full or the lock could not be taken.
bool worldClaim(uint64_t key, out uint slot)
{
    const uint base = voxelKeyHash(key) % gWorldCapacity;
    for (uint i = 0; i < kRestirWorldBucket; ++i) {
        const uint s = (base + i) % gWorldCapacity;
        // The common case, and it needs no lock: the slot already holds this
        // key and only its reservoir is being updated.
        if (gWorldKeys[s] == key) {
            slot = s;
            return true;
        }
        if (gWorldKeys[s] == kVoxKeyInvalid) {
            if (!worldTryLock(s))
                continue; // someone else is claiming it; try the next slot
            // Re-read under the lock. Between the test above and the lock being
            // taken another thread may have claimed this slot for a different
            // key, and writing ours over it would lose theirs.
            const uint64_t held = gWorldKeys[s];
            if (held == kVoxKeyInvalid) {
                gWorldKeys[s] = key;
                gWorldRes[s] = restirEmpty();
                worldUnlock(s);
                slot = s;
                return true;
            }
            worldUnlock(s);
            if (held == key) {
                slot = s;
                return true;
            }
        }
    }
    slot = 0u;
    return false;
}

// Read the reservoir filed against a face, capped to the confidence a shared
// sample is allowed to bring. Returns an empty reservoir on any miss.
Reservoir worldRead(float3 p, float3 n)
{
    if (gWorldEnabled == 0)
        return restirEmpty();
    uint slot;
    if (!worldFind(voxelFaceKey(p, n), slot))
        return restirEmpty();
    Reservoir r = gWorldRes[slot];
    restirCapM(r, gWorldMaxM);
    return r;
}

// File a pixel's reservoir against the face it was looking at.
//
// ONE WRITER PER FACE PER FRAME, CHOSEN BY THE LOCK. Several pixels land on one
// face and all of them offer their reservoir; the first to take the lock stores
// its own and the rest give up. That is deliberately not a merge: merging the
// pixels that share a face would average samples drawn from the same few
// bounces and hand the result back out as though it were independent evidence,
// which is the correlation this whole file is written to avoid. One pixel's
// honest reservoir is worth more than an average of several correlated ones.
void worldWrite(float3 p, float3 n, Reservoir r)
{
    if (gWorldEnabled == 0 || r.M == 0u || r.W <= 0.0f)
        return;
    uint slot;
    if (!worldClaim(voxelFaceKey(p, n), slot))
        return;
    if (!worldTryLock(slot))
        return;
    gWorldRes[slot] = r;
    worldUnlock(slot);
}
