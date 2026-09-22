// ---------------------------------------------------------------------------
// Vitals.ps.slang -- the two bars, drawn as the screen itself.
//
// (user 2026-09-19: "create a pixelated effect when taking damage/hunger. red
//  for damage, and yellow for hunger. look to v1 on instruction.")
//
// A port of v1's hurt block out of render/wgsl/blit.js, arithmetic for
// arithmetic. There is deliberately no hearts-and-drumsticks UI in either
// engine -- "remove the hunger/hearts ui. keep the mechanics" -- so THIS IS THE
// BAR. The pixelation is not a cue about the health; it is the health.
//
// ---- HOW IT READS -----------------------------------------------------------
//
// The red CREEPS INWARD one step per point lost. Distance from the centre is a
// PROBABILITY rather than a threshold, so the blocks gather at the rim and
// scatter inward, and the whole curve scales by the level: one point down
// speckles the very edge, four points fills most of the frame. Coverage is what
// carries the severity -- the colour never changes, and neither does a block's
// opacity. v1's note is emphatic about that: if the opacity climbed too, the
// effect would read as one red sheet thickening rather than as more pixels.
//
// SEEDED PER HIT, NEVER PER FRAME. A per-frame hash re-rolls every block sixty
// times a second and sizzles; v1's original was a DOM canvas painted once at
// the hit whose opacity alone moved, and the seed is what preserves that.
//
// ---- AND THE GOLD IS A COPY, NOT A SHARED HELPER ----------------------------
//
// v1 says why and it is worth keeping: every number in the red was tuned by eye
// over two days of the user's own notes, and folding the two into one function
// would mean a later tweak to one bar silently retuning the other. They are two
// readouts that happen to look alike, not one readout.
//
// THREE THINGS DIFFER, each for a reason:
//   * THE SEED IS OFFSET. Both bars dither over the same 64x36 grid, so a
//     shared seed would have them competing for exactly the same cells --
//     whichever drew second would win and the first would vanish where they
//     overlapped. Offset, they interleave and both stay readable at once.
//   * THERE IS NO PER-HIT SPIKE. hurtT is an event (you were just struck);
//     going hungry is a slow state, so the level alone drives it.
//   * IT PAINTS UNDER THE RED, because hunger is the less urgent bar and a blow
//     landing while starving must still read as a blow.
// ---------------------------------------------------------------------------

cbuffer gVitCB
{
    float2 gSize;      // framebuffer pixels
    float gRedLevel;   // 0 at full health .. 4 one point from death
    float gGoldLevel;  // 0 at a full stomach .. 4 empty, and starving
    float gHurtT;      // 1 on the blow, riding down over ~0.55 s
    float gSeed;       // per HIT, not per frame -- see the note above
};

// v1's ih3, bit for bit. The blocks have to land in the same places or the
// grain is a different grain.
float ih3(int x, int y, int z)
{
    uint h = uint(x) * 374761393u + uint(y) * 3266489917u + uint(z) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return float(h ^ (h >> 16u)) / 4294967296.0f;
}

// The block grid: the resolution v1's DOM canvas was painted at, kept so the
// blocks are the same size they have always been.
static const float2 kGrid = float2(64.0f, 36.0f);

float4 main(float2 texC: TEXCOORD, float4 posH: SV_POSITION) : SV_TARGET
{
    if (gRedLevel <= 0.0f && gGoldLevel <= 0.0f) discard;

    const float2 uv = posH.xy / gSize;
    const float2 cell = floor(uv * kGrid);
    // Normalised to [-1, 1] over the grid, then a distance with x weighted
    // down: a 16:9 window is wider than it is tall and an unweighted radius
    // would reach the top and bottom long before the sides.
    const float2 n = (cell / (kGrid - float2(1.0f, 1.0f))) * 2.0f - 1.0f;
    const float d = sqrt(n.x * n.x * 0.85f + n.y * n.y);

    float3 col = float3(0.0f, 0.0f, 0.0f);
    float acc = 0.0f;   // how much of this pixel is painted, for the blend

    // ---- HUNGER FIRST, so the red lands on top of it ----------------------
    if (gGoldLevel > 0.0f)
    {
        const float a = (d - (0.86f - gGoldLevel * 0.10f)) / 0.62f;
        if (a > 0.0f)
        {
            // A FIXED seed: hunger has no hit to seed from.
            const int s = 4409;
            if (ih3(int(cell.x), int(cell.y), s) <=
                min(1.0f, a * a * 1.1f * (0.34f + gGoldLevel * 0.42f)))
            {
                const float v = ih3(int(cell.x), int(cell.y), s + 7717);
                // rgb(205..249, 150..189, 20) -- the hotbar badge's gold, kept
                // warm enough that it never reads as the red's lighter end.
                col = float3((205.0f + floor(v * 45.0f)) / 255.0f,
                             (150.0f + floor(v * 40.0f)) / 255.0f, 20.0f / 255.0f);
                acc = min(0.55f, 0.16f + a * 0.34f);
            }
        }
    }

    // ---- ...AND THE RED OVER IT -------------------------------------------
    if (gRedLevel > 0.0f)
    {
        const float a = (d - (0.86f - gRedLevel * 0.10f)) / 0.62f;
        if (a > 0.0f)
        {
            const int s = int(gSeed);
            if (ih3(int(cell.x), int(cell.y), s) <=
                min(1.0f, a * a * 1.1f * (0.34f + gRedLevel * 0.42f)))
            {
                const float v = ih3(int(cell.x), int(cell.y), s + 7717);
                // rgb(150..209, 10, 14) -- the old canvas's own reds. ONE red:
                // damage is carried entirely by how many pixels are red, never
                // by what colour they are.
                col = float3((150.0f + floor(v * 60.0f)) / 255.0f, 10.0f / 255.0f,
                             14.0f / 255.0f);
                // ...and the per-hit spike, which the gold has no equivalent of.
                acc = min(0.55f, 0.16f + a * 0.34f + 0.22f * gHurtT);
            }
        }
    }

    if (acc <= 0.0f) discard;
    return float4(col, acc);
}
