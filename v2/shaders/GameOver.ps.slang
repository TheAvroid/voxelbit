// ---------------------------------------------------------------------------
// GameOver.ps.slang -- the screen when the bar runs out.
//
// (user 2026-09-19: "create a game over screen on death.")
//
// v1 hands death to `VIT.onDeath(why)` and a DOM overlay. There is no DOM here,
// so this is the same idea through the one mechanism v2 has for screen-space
// marks: a full-screen pass over the window's own pixels, after the blit, on
// the same path the crosshair takes. See Crosshair.ps.slang for why that
// position matters -- nothing drawn here is ever fed to Ray Reconstruction, and
// it stays at its authored pixel size whatever the Resolution slider says.
//
// THE ALPHABET IS THE WORLD'S OWN. ui/holotext.h carries a 5 x 6 bitmap font
// that the signs in the wood are drawn with, and holoPackGlyph packs one glyph
// into a uint exactly the way this reads it -- bit (row * 5 + col), row 0 at the
// top. Reusing it means the death screen is lettered in the same hand as
// everything else in the game rather than in whatever a second font would be,
// and there is no second table to keep in step.
//
// IT FADES IN RATHER THAN CUTTING. `gFade` rides 0 -> 1 over about a second:
// dying mid-swing and being slammed straight to black reads as a crash, and the
// fade is also what lets the last frame of the world stay legible underneath
// long enough to see what killed you.
// ---------------------------------------------------------------------------

// The font's own shape -- see kHoloCols / kHoloRows in ui/holotext.h.
static const int kCols = 5;
static const int kRows = 6;
static const int kAdvance = 6;   // one cell of gap between glyphs

cbuffer gOverCB
{
    float2 gSize;
    float gFade;        // 0 the instant you die, 1 a second later
    int gCount0;        // glyphs on the big line
    int gCount1;        // ...and on the small one under it, the cause
    int3 gPad;
    // Two lines of at most sixteen glyphs, packed by holoPackGlyph. A flat
    // array rather than two, because a cbuffer array of uint is one binding
    // and the split is just an index.
    uint gGlyph[32];
};

// Is (col, row) inside glyph `g`'s bitmap?
bool glyphBit(uint g, int col, int row)
{
    if (col < 0 || col >= kCols || row < 0 || row >= kRows) return false;
    return (g & (1u << uint(row * kCols + col))) != 0u;
}

// One line of text, as coverage at this pixel. `cell` is the size of a font
// cell in pixels; the line is centred on gSize.x and sits with its top at
// `topY`.
float lineCoverage(float2 p, int first, int count, float cell, float topY)
{
    if (count <= 0) return 0.0f;
    const float wide = float(count * kAdvance - (kAdvance - kCols)) * cell;
    const float x0 = (gSize.x - wide) * 0.5f;
    const float gx = (p.x - x0) / cell;
    const float gy = (p.y - topY) / cell;
    if (gy < 0.0f || gy >= float(kRows)) return 0.0f;
    if (gx < 0.0f) return 0.0f;
    const int slot = int(gx) / kAdvance;
    if (slot < 0 || slot >= count) return 0.0f;
    const int col = int(gx) - slot * kAdvance;
    return glyphBit(gGlyph[first + slot], col, int(gy)) ? 1.0f : 0.0f;
}

float4 main(float2 texC: TEXCOORD, float4 posH: SV_POSITION) : SV_TARGET
{
    if (gFade <= 0.0f) discard;

    const float2 p = posH.xy;
    // WHOLE PIXELS, like the crosshair: the cell doubles at 2160p rather than
    // growing continuously, so the letters stay hard-edged instead of landing
    // half on a pixel and going grey.
    const float scale = max(1.0f, floor(gSize.y / 1080.0f));
    const float bigCell = 14.0f * scale;    // "GAME OVER" at ~84 px tall
    const float smallCell = 5.0f * scale;   // the cause, under it

    const float bigTop = gSize.y * 0.5f - float(kRows) * bigCell * 0.5f - 40.0f * scale;
    const float smallTop = bigTop + float(kRows) * bigCell + 34.0f * scale;

    const float big = lineCoverage(p, 0, gCount0, bigCell, bigTop);
    const float small = lineCoverage(p, 16, gCount1, smallCell, smallTop);

    // THE CURTAIN: a deep red-black rather than a flat black, so the screen
    // reads as continuous with the red the hurt pass has been painting on the
    // way down rather than as a different effect arriving.
    const float3 curtain = float3(0.055f, 0.006f, 0.010f);
    float3 col = curtain;
    float a = gFade * 0.90f;

    if (big > 0.0f)
    {
        // Letters sit ABOVE the curtain, and at full opacity, so they are
        // readable from the first frame of the fade.
        col = float3(0.94f, 0.13f, 0.15f);
        a = gFade;
    }
    else if (small > 0.0f)
    {
        col = float3(0.78f, 0.72f, 0.70f);
        a = gFade;
    }

    return float4(col, a);
}
