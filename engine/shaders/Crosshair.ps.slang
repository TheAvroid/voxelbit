// ---------------------------------------------------------------------------
// Crosshair.ps.slang -- the mark in the middle of the screen. v2's, ported.
//
// THE MARK INVERTS THE FRAME UNDER IT rather than being drawn in a colour, and
// that is the whole design. A white crosshair goes blind against a bright sky,
// a white flower or a sunlit trunk, which in this world is most of what you
// would be pointing at; `1 - backdrop` is visible against all three, and
// against itself. There is no backdrop it can match.
//
// v2 got this from glLogicOp(GL_XOR) against white on a fixed-function
// pipeline, because on eight-bit channels XOR with white IS a bitwise NOT.
// There is no logic-op state here and none is needed: a blend of
// OneMinusDstColor against a source of pure white is `1 * (1 - dst)`, which is
// the same pixels through the ordinary blender. See makeCrosshair() in app.h.
//
// WHERE IT RUNS IS LOAD-BEARING, and it runs on the WINDOW's own pixels, after
// the frame has been blitted there. Three things follow, and all three are the
// reason:
//
//   IT IS NEVER FED TO THE DENOISER. Ray Reconstruction is a temporal model; a
//   mark drawn before it would be taken for part of the scene and would ghost
//   across the screen as the camera turned.
//
//   IT IS ALWAYS 32 PIXELS. Drawn into the traced frame it would be stretched
//   with it, so dropping the Resolution slider to 50% would leave a soft,
//   oversized cross -- exactly in the setting where the picture is already
//   softer and the mark is needed most.
//
//   IT SITS UNDER THE MENU, because Falcor draws the GUI after onFrameRender.
//   Moot in practice: the mark is hidden while the menu is up, since the
//   settings panel owns the middle of the screen.
// ---------------------------------------------------------------------------

cbuffer gCrossCB
{
    // The framebuffer size in pixels. Passed in rather than derived from the
    // texture coordinate, because the centre has to land ON a pixel and a
    // divide would put it half a pixel off at odd resolutions.
    float2 gSize;
    float gArm;   // half the length of a bar: 16 px at 1080p, so 32 across
    float gHalf;  // half its thickness: 2 px at 1080p, so 4 through
};

float4 main(float2 texC: TEXCOORD, float4 posH: SV_POSITION) : SV_TARGET
{
    // Distance from the centre, folded into one quadrant: the plus is
    // symmetric, so one comparison per axis does all four arms.
    const float2 p = abs(posH.xy - gSize * 0.5f);

    // The union of the two bars. v2 had to emit the vertical as a stub above
    // and one below, because XOR applied twice to the shared centre punched a
    // hole clean through the middle of the mark. Here the test is a boolean and
    // the inversion happens once, so the overlap is free.
    const bool inside = (p.x < gArm && p.y < gHalf) || (p.y < gArm && p.x < gHalf);
    if (!inside)
        discard;

    // White. The blend state turns it into one minus what is already there.
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
