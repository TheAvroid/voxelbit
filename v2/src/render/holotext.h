// ---------------------------------------------------------------------------
// holotext.h -- the face the pause room's labels are written in.
//
// A 5 x 6 bitmap alphabet and the one function that turns a word into the glyph
// masks V2Holo carries. It is the whole of the text pipeline: there is no atlas,
// no texture, no shaper and no measurement pass, because a label here is a
// handful of letters standing on a wall and anything more is a font engine
// nobody asked for.
//
// WHY A BITMAP AND NOT THE GAME'S OWN .otf. v2 already loads 3x3-pixel.otf for
// the interface (see App::bakePx3), and it is the right face for text that ImGui
// draws. It is the wrong one for text the TRACER draws: to use it here the
// outlines would have to be rasterised on the host, packed into an atlas,
// uploaded as a texture and then sampled per ray -- a texture binding and a
// filter decision, for three words that never change. The face below is that
// font's own metric read off its description (five cells to a capital, four to
// an x-height, six to an advance) and drawn by hand at that size, so the labels
// come out on the same grid as the rest of the interface.
//
// THE ROWS, AND THE ONE THING THAT HAD TO BE GOT RIGHT. Row 4 is the baseline.
// An x-height letter is rows 1..4, an ascender (b d f h k l t) reaches up to row
// 0, and a descender (g j p q y) reaches down to row 5. A figure is rows 0..4,
// cap height, like every face sets them.
//
// THE FIRST CUT PUT EVERY LETTER ON ROWS 0..4, on the argument that at five
// cells nobody can see the difference between a lowercase b and a capital B.
// That is true about b and completely wrong about the word it is in: "back"
// came out as bACK and "discord" as diSCORd, because a c, an o, an r and a k
// drawn five rows tall are not small letters at all, they are capitals. Height
// is not decoration here -- an alphabet where every letter is the same height IS
// an uppercase alphabet, whatever shapes it uses. One row of x-height is the
// whole of the fix.
//
// See V2Holo in Shared.slang for what the packed form means, and v1's GLYPH5
// (the x{n} beside the held tool) for where the idea comes from.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include "../../shaders/Shared.slang"

namespace v2 {

// ---------------------------------------------------------------------------
// THE FACE.
//
// One entry per printable character: six strings of five columns, '#' for a lit
// cell. Written out rather than packed because a hex table is a face nobody can
// edit, and the point of this shape is that adding a letter is drawing it.
// ---------------------------------------------------------------------------
struct HoloGlyph {
    char ch;
    const char *row[kHoloRows];
};

static const HoloGlyph kHoloFont[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", "....."}},
    // -- the x-height, rows 1..4 --------------------------------------------
    {'a', {".....", ".###.", "#..#.", "#..#.", ".####", "....."}},
    {'c', {".....", ".###.", "#....", "#....", ".###.", "....."}},
    {'e', {".....", ".###.", "#####", "#....", ".###.", "....."}},
    {'i', {"..#..", ".....", "..#..", "..#..", "..#..", "....."}},
    {'m', {".....", "##.##", "#.#.#", "#.#.#", "#.#.#", "....."}},
    {'n', {".....", "####.", "#...#", "#...#", "#...#", "....."}},
    {'o', {".....", ".###.", "#...#", "#...#", ".###.", "....."}},
    {'r', {".....", "#.##.", "##...", "#....", "#....", "....."}},
    {'s', {".....", ".####", "##...", "...##", "####.", "....."}},
    {'u', {".....", "#...#", "#...#", "#...#", ".####", "....."}},
    {'v', {".....", "#...#", "#...#", ".#.#.", "..#..", "....."}},
    {'w', {".....", "#...#", "#.#.#", "#.#.#", ".#.#.", "....."}},
    {'x', {".....", "#..#.", ".##..", ".##..", "#..#.", "....."}},
    {'z', {".....", "#####", "...#.", ".#...", "#####", "....."}},
    // -- the ascenders, rows 0..4 -------------------------------------------
    {'b', {"#....", "#....", "####.", "#...#", "####.", "....."}},
    {'d', {"....#", "....#", ".####", "#...#", ".####", "....."}},
    {'f', {"..##.", ".#...", "####.", ".#...", ".#...", "....."}},
    {'h', {"#....", "#....", "####.", "#...#", "#...#", "....."}},
    {'k', {"#....", "#..#.", "#.#..", "##...", "#.##.", "....."}},
    {'l', {".##..", "..#..", "..#..", "..#..", ".###.", "....."}},
    {'t', {".#...", "####.", ".#...", ".#...", "..##.", "....."}},
    // -- and the descenders, which reach row 5 ------------------------------
    {'g', {".....", ".####", "#...#", ".####", "....#", "####."}},
    {'j', {"...#.", ".....", "...#.", "...#.", "#..#.", ".##.."}},
    {'p', {".....", "####.", "#...#", "####.", "#....", "#...."}},
    {'q', {".....", ".####", "#...#", ".####", "....#", "....#"}},
    {'y', {".....", "#...#", "#...#", ".####", "....#", "####."}},
    // -- FIGURES ARE CAP HEIGHT, rows 0..4, and that is not an inconsistency:
    // it is what every face does. A row of digits next to a lowercase word is
    // a number and should look like one.
    {'0', {".###.", "#..##", "#.#.#", "##..#", ".###.", "....."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", ".###.", "....."}},
    {'2', {".###.", "#...#", "..##.", ".#...", "#####", "....."}},
    {'3', {"####.", "....#", ".###.", "....#", "####.", "....."}},
    {'4', {"#..#.", "#..#.", "#####", "...#.", "...#.", "....."}},
    {'5', {"#####", "#....", "####.", "....#", "####.", "....."}},
    {'6', {".###.", "#....", "####.", "#...#", ".###.", "....."}},
    {'7', {"#####", "....#", "...#.", "..#..", "..#..", "....."}},
    {'8', {".###.", "#...#", ".###.", "#...#", ".###.", "....."}},
    {'9', {".###.", "#...#", ".####", "....#", ".###.", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", "....."}},
    {'.', {".....", ".....", ".....", ".....", "..#..", "....."}},
};

// One glyph, packed the way V2Holo::bits wants it: bit (row * kHoloCols + col),
// row 0 at the top. An unknown character comes back blank rather than as a
// missing-glyph box, because a label is a literal in this engine's own source
// and a character with no drawing is a typo to fix, not a state to render.
inline uint32_t holoPackGlyph(char ch) {
    if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    for (const HoloGlyph &g : kHoloFont) {
        if (g.ch != ch) continue;
        uint32_t bits = 0;
        for (int r = 0; r < kHoloRows; ++r)
            for (int c = 0; c < kHoloCols; ++c)
                if (g.row[r][c] == '#')
                    bits |= 1u << uint32_t(r * kHoloCols + c);
        return bits;
    }
    return 0;
}

// How many glyphs of a word actually fit in one slot.
inline int holoGlyphCount(const std::string &word) {
    const size_t cap = size_t(kHoloMaxGlyphs);
    return int(word.size() > cap ? cap : word.size());
}

// How wide a word is, in glyph CELLS -- glyphs times the advance, less the gap
// nobody needs after the last one. The host centres a label with this, and it is
// the only measurement in the file.
inline int holoWidthCells(const std::string &word) {
    const int n = holoGlyphCount(word);
    return n <= 0 ? 0 : n * kHoloAdvance - (kHoloAdvance - kHoloCols);
}

// ---------------------------------------------------------------------------
// LAY A WORD OUT ON A PLANE, CENTRED ON `mid`.
//
// `mid` is where the middle of the word goes -- the centre of its box, which is
// what a caller wanting a label over a button actually has -- and what comes out
// is the TOP-LEFT corner V2Holo::org is defined as. Doing the conversion here is
// the lesson publishButtons learned from place(): the one function that knows
// what a field means should be the one that fills it in, or every caller gets
// the half-a-box offset wrong in its own way.
//
// `right` and `up` must be unit and perpendicular. The shader takes their cross
// product as the plane normal and divides by `cell` with no normalisation of its
// own, so a scaled axis here is a stretched word there.
// ---------------------------------------------------------------------------
inline void holoSetWord(V2Holo &h, const std::string &word, const float3 &mid,
                        const float3 &right, const float3 &up, float cellM,
                        const float3 &tint) {
    h = V2Holo{};
    const int n = holoGlyphCount(word);
    if (n <= 0 || cellM <= 0.0f) return;   // cell == 0 is the off switch
    h.count = uint32_t(n);
    h.cell = cellM;
    h.right = right;
    h.up = up;
    h.tint = tint;
    h.org = mid - right * (0.5f * float(holoWidthCells(word)) * cellM) +
            up * (0.5f * float(kHoloRows) * cellM);
    for (int i = 0; i < n; ++i)
        h.bits[i >> 2][i & 3] = holoPackGlyph(word[size_t(i)]);
}

}  // namespace v2
