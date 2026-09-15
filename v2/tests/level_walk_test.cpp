// level_walk_test -- walking inside a model that has rooms in it.
//
// Solid::interior is the whole subject. Every other solid in this engine is a
// FLOOR or a WALL and never both: blocked() skips standable solids outright,
// because walking into a rock is meant to put you on top of it and moveAxis's
// step-up is what does that. A building cannot take that deal, and the two
// answers it needs instead are what this pins.
//
//   1. THE FLOOR UNDER YOU IS THE ONE UNDER YOU. Standing in the lobby, the
//      ground is the ground floor -- not the roof eleven metres up, which is
//      what the column heightfield says and what put the old query on top of
//      the building the instant the body moved.
//   2. ...AND IT FOLLOWS YOU UP. On the upper storey the answer is the upper
//      storey, from the same query and with no per-floor knowledge.
//   3. A WALL STOPS YOU. Interior solids are not exempt from blocked() the way
//      standable ones are, or you would walk through the masonry.
//   4. A DOORWAY DOES NOT. The same wall with a hole in it is passable, which
//      is the only thing that makes 3 a wall rather than a bounding box.
//   5. A STEP DOES NOT EITHER -- anything under the step-up is walked onto.
//   6. NOTHING IN THE WOOD MOVES. The same body, the same ground, over a solid
//      that is standable and NOT interior, answers exactly what it used to.
//
// The model here is built in code rather than loaded: building.vox is 4 MB and
// gitignored upstream of its .glb, and a test that needs an asset to run is a
// test that stops running. The shape is the part that matters and it is the
// shape a building has -- a slab, a room over it, a ceiling, a room over that.
#include <cmath>
#include <cstdio>
#include <vector>

#include "render/player.h"
#include "scene/collide.h"
#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;
static void chk(bool ok, const char *what, const char *detail) {
    if (!ok) ++fails;
    std::printf("  %-4s %-46s %s\n", ok ? "ok" : "FAIL", what, detail);
}

// ---------------------------------------------------------------------------
// A two-storey box on a slab, 6.0 x 6.4 x 5.0 m. In voxels, 10 cm each:
//
//     y 0..1    the slab, solid right across          (top at 0.2 m)
//     y 2..31   storey one, walls only                (30 voxels -> 3.0 m)
//     y 32      the floor of storey two, right across (top at 3.3 m)
//     y 33..62  storey two, walls only
//     y 63      the roof, right across                (top at 6.4 m)
//
// The wall runs along z = 2.5 m with a 1.2 m doorway punched through it, so the
// same wall answers 3 and 4 depending only on where you walk at it. THE
// DOORWAY HAS TO BE WIDER THAN THE BODY, which is 0.52 m (halfWidth 0.26) --
// the first cut of this was 0.4 m and the body could not have fitted through
// it if the code had been perfect. A 40 cm step sits at the far end for 5.
// ---------------------------------------------------------------------------
static const int SX = 60, SY = 64, SZ = 50;
static const int WALL_Z = 25;                       // 2.5 m
static const int DOOR_X0 = 25, DOOR_X1 = 36;        // 2.5 .. 3.7 m
static const int STEP_Z = 40;                       // 4.0 m and beyond

static VoxAsset buildBlock() {
    VoxAsset a;
    a.sx = SX;
    a.sy = SY;
    a.sz = SZ;
    a.a.assign(size_t(SX) * size_t(SY) * size_t(SZ), 0);
    auto set = [&](int x, int y, int z) {
        a.a[size_t(x) + size_t(z) * size_t(SX) + size_t(y) * size_t(SX) * size_t(SZ)] = 1;
    };
    for (int z = 0; z < SZ; ++z)
        for (int x = 0; x < SX; ++x) {
            set(x, 0, z);
            set(x, 1, z);          // the slab
            set(x, 32, z);         // the floor of storey two
            set(x, 63, z);         // the roof
        }
    // The internal wall, both storeys, with a doorway through the lower one.
    for (int y = 2; y < 63; ++y)
        for (int x = 0; x < SX; ++x) {
            if (y < 32 && x >= DOOR_X0 && x <= DOOR_X1) continue;   // the doorway
            set(x, y, WALL_Z);
        }
    // A four-voxel step (40 cm, inside stepUp) against the far end.
    for (int z = STEP_Z; z < SZ; ++z)
        for (int x = 0; x < SX; ++x)
            for (int y = 2; y < 6; ++y) set(x, y, z);
    return a;
}

int main() {
    const VoxAsset asset = buildBlock();
    std::vector<uint8_t> ident(256, mat::AIR);
    for (int i = 1; i < 256; ++i) ident[size_t(i)] = uint8_t(i);
    const std::vector<int16_t> colTop = columnTops(asset, ident);

    // Where it stands. 640 m up, like the real level, so the terrain under it
    // is hundreds of metres of nothing and cannot accidentally be the answer.
    const float BASE = 640.0f;
    Solid s;
    s.interior = true;
    s.standable = true;
    s.cx = float(SX) * VOXEL_M * 0.5f;
    s.cz = float(SZ) * VOXEL_M * 0.5f;
    s.hx = float(SX) * VOXEL_M * 0.5f;
    s.hz = float(SZ) * VOXEL_M * 0.5f;
    s.top = BASE + float(SY) * VOXEL_M;
    s.col = colTop.data();
    s.msx = int16_t(SX);
    s.msz = int16_t(SZ);
    s.vol = asset.a.data();
    s.vsy = int16_t(SY);
    s.yaw = 0;
    s.tx = 0.0f;
    s.tz = 0.0f;
    s.baseY = BASE;

    VoxelTerrain terrain;
    WalkWorld w;
    w.terrain = &terrain;
    w.solids = &s;
    w.solidCount = 1;

    Player p;
    const float slabTop = BASE + 0.2f;      // y 0..1 filled -> top of voxel 1
    const float floor2 = BASE + 3.3f;       // y 32 filled   -> top of voxel 32

    std::printf("a two-storey block at y %.0f: slab top %.2f, upper floor %.2f\n\n", BASE, slabTop,
                floor2);

    // ---- 1. THE GROUND FLOOR, NOT THE ROOF -------------------------------
    //
    // The column at (0.8, 3.0) is slab, air, floor, air, roof. columnTops says
    // 64 -- the roof, 6.4 m up -- and that is the answer this replaced.
    // (1.0, 1.0) is in the near room: clear of the wall at z 2.5 and of the
    // step at z 4.0, so the column there is slab / air / floor / air / roof.
    p.pos = Vec3(1.0f, slabTop, 1.0f);
    p.onGround = true;
    float g = p.surfaceAt(w, 1.0f, 1.0f);
    char buf[160];
    std::snprintf(buf, sizeof buf, "got %.2f, wanted %.2f (roof would be %.2f)", g, slabTop,
                  BASE + 6.4f);
    chk(std::fabs(g - slabTop) < 1e-3f, "standing in the lobby, the floor is the slab", buf);

    // ---- 2. ...AND THE UPPER STOREY WHEN YOU ARE ON IT --------------------
    p.pos = Vec3(1.0f, floor2, 1.0f);
    g = p.surfaceAt(w, 1.0f, 1.0f);
    std::snprintf(buf, sizeof buf, "got %.2f, wanted %.2f", g, floor2);
    chk(std::fabs(g - floor2) < 1e-3f, "upstairs, the floor is the upper storey", buf);

    // ---- 3. A WALL STOPS YOU ----------------------------------------------
    //
    // Walking at a solid stretch of the wall: x 1.0 is well clear of the
    // doorway, and the body's box at z 2.45 straddles the wall voxel at 2.5.
    p.pos = Vec3(1.0f, slabTop, 2.2f);
    p.onGround = true;
    const bool atWall = p.blocked(w, 1.0f, 2.45f);
    chk(atWall, "a wall blocks", atWall ? "blocked, as it must" : "walked into the masonry");

    // ---- 4. ...AND A DOORWAY DOES NOT -------------------------------------
    //
    // The SAME wall at the SAME height, a metre along: x 3.1 is the middle of
    // the doorway, and a 0.52 m body clears its 1.2 m opening.
    p.pos = Vec3(3.1f, slabTop, 2.2f);
    const bool atDoor = p.blocked(w, 3.1f, 2.45f);
    chk(!atDoor, "a doorway in it does not",
        atDoor ? "the doorway is a wall -- the box is the shape, not the voxels" : "walked through");

    // ---- 5. A STEP IS WALKED ONTO, NOT INTO -------------------------------
    //
    // The 40 cm step at z >= 4.0 m is under stepUp, so it must not read as a
    // wall, and the ground on top of it must be the top of the step.
    p.pos = Vec3(1.0f, slabTop, 3.8f);
    const bool atStep = p.blocked(w, 1.0f, 4.1f);
    chk(!atStep, "a 40 cm step is not a wall", atStep ? "blocked by a kerb" : "stepped up");
    g = p.surfaceAt(w, 1.0f, 4.1f);
    std::snprintf(buf, sizeof buf, "got %.2f, wanted %.2f", g, BASE + 0.6f);
    chk(std::fabs(g - (BASE + 0.6f)) < 1e-3f, "...and its top is the ground there", buf);

    // ---- 6. NOTHING IN THE WOOD MOVES -------------------------------------
    //
    // The same model, the same body, with interior cleared: a standable solid
    // must answer the TOP OF THE COLUMN exactly as it always did, and must go
    // on being exempt from blocked(). This is the regression that would take
    // the boulders with it.
    Solid rock = s;
    rock.interior = false;
    w.solids = &rock;
    p.pos = Vec3(0.8f, slabTop, 3.0f);
    g = p.surfaceAt(w, 0.8f, 3.0f);
    std::snprintf(buf, sizeof buf, "got %.2f, wanted the column top %.2f", g, BASE + 6.4f);
    chk(std::fabs(g - (BASE + 6.4f)) < 1e-3f, "a standable model still answers its column top", buf);
    const bool rockBlocks = p.blocked(w, 0.3f, 0.85f);
    chk(!rockBlocks, "...and is still exempt from blocked()",
        rockBlocks ? "a rock became a wall" : "unchanged");

    std::printf("\n%s -- %d failure%s\n", fails ? "FAILED" : "all good", fails,
                fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
