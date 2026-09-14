#pragma once
// ---------------------------------------------------------------------------
// THE ASSET EDITOR'S TOOLS, PORTED FROM v1.
//
// The deck itself is older than this file -- World::buildStage makes the white
// floor and app.h's [I] is the door -- and what stood on it was a cardinal that
// you could look at and nothing else. That is a VIEWER. v1's asset editor is a
// tool, and the difference is that its output is a block of code:
//
//   ui/editor.js:  ", / . scrub frames, E move the frame, R rotate it,
//                   <- / -> reorder"
//   ...and one button whose tooltip is the whole design:
//                  "copy the per-frame offsets so you can paste them back to
//                   be baked into the code"
//
// THE SUBJECT IS THE BUNNY (user 2026-09-13: "put the bunny in the middle of
// the asset editor"), which is v1's subject too -- its editor stages two of
// them in two lanes and its [b] swaps which one the tools operate on. The three
// strips (jump, rotate/left, rotate/right) are what this cycles through
// instead: v2's rabbit already has both turns as separate art, so one lane and
// a strip selector says the same thing with less on the deck.
//
// WHAT THE TOOLS EDIT IS A BunnyBake, NOT THE .vox FILES. See the long note
// over that struct in bunnies.h for why alignment cannot live in the art. This
// class holds a WORKING COPY of the three compiled-in tables, draws the strip
// through them, and hands the result back as C++ you paste over the rows in
// bunnies.h. Nothing here writes a file and nothing here changes what the
// rabbits in the wood are doing until you paste.
//
// AND IT DRAWS THROUGH Bunnies::pose. That is the one rule that makes the tool
// worth trusting: the deck and the wood run the same arithmetic over the same
// table, so a bake that looks right here cannot look different out there.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Utils/UI/InputTypes.h"

#include "../gpu/world.h"
#include "bunnies.h"

namespace v2 {

// Falcor's, so the key names below read the way app.h's do. A redefinition
// of the same alias in the same namespace is legal, which is what lets app.h
// keep its own copy of this line.
namespace Input = Falcor::Input;

// The strip plays at the rate everything else in this engine does; the editor
// only ever pauses it, never speeds it up. A scrub tool whose PLAY speed is
// also a setting is a tool that can show you a timing the game will not.
inline constexpr float kEditFps = 12.0f;

// ---------------------------------------------------------------------------
class AssetEdit {
  public:
    void attach(Bunnies *b) { buns_ = b; }

    bool on() const { return on_; }

    // -----------------------------------------------------------------------
    // ONTO THE DECK. `at` is World::stageCentre() -- the middle, which is what
    // was asked for and also the only place on a square deck that needs no
    // explanation.
    //
    // THE WORKING COPY IS TAKEN ON EVERY ENTRY, from the compiled tables. So
    // leaving and coming back is a RESET, deliberately: the export is the only
    // way work leaves this tool, and a session that silently survived a trip to
    // the wood would make it easy to believe a bake had been committed when it
    // had only been typed. The console line printed on the way out is the
    // safety net -- see leave().
    // -----------------------------------------------------------------------
    void enter(const Vec3 &at) {
        at_ = at;
        on_ = true;
        strip_ = kStripHop;
        sel_ = 0;
        axis_ = 2;
        playing_ = true;
        clock_ = 0.0f;
        for (int s = 0; s < kBunnyStrips; ++s) {
            const int n = buns_ ? buns_->frames(s) : 0;
            bake_[s].assign(size_t(n), BunnyBake{0, 0, 0, 0, 0, 0});
            // THE TABLE'S OWN LENGTH BOUNDS THE READ, not the strip's. They
            // agree today and the paste target is a fixed-size array -- so the
            // day somebody adds a twelfth .vox to the folder and forgets the
            // row, this leaves the extra slot at identity instead of reading
            // off the end of a constexpr array.
            const int m = mini(n, bunnyBakeCount(s));
            for (int i = 0; i < m; ++i) bake_[s][size_t(i)] = bunnyBake(s)[i];
            for (int i = m; i < n; ++i) bake_[s][size_t(i)].src = i;
        }
        std::printf("v2: asset editor -- %s, %d frames.  , . frame   <- -> reorder   "
                    "R/V turn   ^ v nudge   G axis   B strip   K play   C copy   N reset\n",
                    bunnyStripName(strip_), frameCount());
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // OFF THE DECK, AND THE BAKE GOES TO THE CONSOLE ON THE WAY OUT.
    //
    // v1 autosaves every nudge into localStorage and its note is blunt about
    // why -- alignment work is slow and there is nothing else it is written
    // down in. This has no localStorage and deliberately no file: a tool that
    // writes into the asset tree is a tool that can disagree with the source
    // you are about to paste into. Printing the table instead costs one line of
    // console and means an hour of nudging cannot be lost to a stray [I].
    // -----------------------------------------------------------------------
    void leave() {
        if (on_ && dirty()) {
            std::printf("v2: asset editor -- unexported edits, here they are:\n%s",
                        bakeSource().c_str());
            std::fflush(stdout);
        }
        on_ = false;
    }

    // -----------------------------------------------------------------------
    // THE EDITOR OWNS THE KEYBOARD WHILE IT IS UP, which is v1's rule word for
    // word ("the asset editor owns these two keys while it is up") and the only
    // rule that works: a key cannot mean two things at once in one mode. So R
    // turns a frame here rather than starting a recording, and the arrows move
    // frames rather than scrubbing the clock. Both get their meanings back the
    // moment you step off the deck.
    //
    // Returns true when the press was the editor's, so app.h can stop.
    // -----------------------------------------------------------------------
    bool key(const Falcor::KeyboardEvent &e) {
        if (!on_ || !buns_) return false;
        const bool shift = e.hasModifier(Input::Modifier::Shift);
        // A STRIP CAN BE EMPTY AND [B] HAS TO STILL WORK. loadStrip gives up on
        // the whole strip when one of its eleven files is missing, so the turns
        // can be absent while the hop is there -- and a tool that swallowed
        // every key on an empty strip would leave you standing on one with no
        // way off it, and with [R] silently starting a recording instead.
        const bool any = frameCount() > 0;
        switch (e.key) {
            case Input::Key::Comma: if (any) step(-1); return true;
            case Input::Key::Period: if (any) step(1); return true;
            case Input::Key::Left: if (any) reorder(-1); return true;
            case Input::Key::Right: if (any) reorder(1); return true;
            case Input::Key::Up: if (any) nudge(1); return true;
            case Input::Key::Down: if (any) nudge(-1); return true;
            case Input::Key::R: if (any) turn(1, shift ? -1 : 1); return true;
            case Input::Key::V: if (any) turn(0, shift ? -1 : 1); return true;
            case Input::Key::G:
                if (!any) return true;
                axis_ = (axis_ + 1) % 3;
                say("nudge axis %c", "XYZ"[axis_]);
                return true;
            case Input::Key::B:
                strip_ = (strip_ + 1) % kBunnyStrips;
                sel_ = 0;
                clock_ = 0.0f;
                say("%s -- %d frames", bunnyStripName(strip_), frameCount());
                return true;
            case Input::Key::K:
                if (!any) return true;
                playing_ = !playing_;
                say(playing_ ? "playing" : "paused on frame %d", sel_);
                return true;
            case Input::Key::C: copyOut(); return true;
            case Input::Key::N: clearStrip(); return true;
            default: return false;
        }
    }

    // The strip runs unless something paused it. Every tool pauses it, exactly
    // as v1's do ("scrubbing pauses the animation"): a frame you are aligning
    // that slides out from under you a twelfth of a second later is not a frame
    // you can align.
    void update(float dt) {
        if (!on_ || !playing_) return;
        const int n = frameCount();
        if (n <= 0) return;
        clock_ += dt * kEditFps;
        while (clock_ >= float(n)) clock_ -= float(n);
        sel_ = maxi(0, mini(n - 1, int(clock_)));
    }

    // ONE INSTANCE, through the population's own pose(). Facing 0, which in
    // this engine looks down -Z -- and the deck puts the camera on the +Z side
    // looking back, so the subject arrives face on rather than showing you its
    // tail. Fade 1: nothing is materialising here.
    void publish(World &world, int slot) const {
        if (!on_ || !buns_ || frameCount() == 0) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            world.flushFlyerInstances();
            return;
        }
        const BunnyPose p = buns_->pose(strip_, sel_, 0.0f, 1.0f, at_, bake_[strip_].data());
        if (p.model < 0) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
        } else {
            world.setFlyerInstance(slot, p.model, p.m, p.tx, p.ty, p.tz, nullptr, true);
        }
        world.flushFlyerInstances();
    }

    // -----------------------------------------------------------------------
    // THE EXPORT, AND IT IS C++ RATHER THAN v1'S JSON.
    //
    // v1 copies `{"bunny_jump":[{"frame":0,"ox":0,...}]}` and somebody turns
    // that into a BUNNY_JUMP_BAKE by hand. There is no reason for that step
    // here: the destination is a C++ table in a header this tool can see, so
    // what goes on the clipboard is the table itself, formatted the way the
    // file already formats it. Paste replaces the rows between the two markers
    // in bunnies.h and the wood has the change.
    //
    // ALL THREE STRIPS, ALWAYS -- v1 exports both its lanes at once for the
    // same reason: alignment on a turn is usually done against the hop it
    // hands over to, and an export that carried only the strip you were
    // looking at would make you do the trip twice to keep them in step.
    // -----------------------------------------------------------------------
    std::string bakeSource() const {
        static const char *kVar[kBunnyStrips] = {"kBunnyHopBake", "kBunnyTurnLBake",
                                                 "kBunnyTurnRBake"};
        static const char *kCount[kBunnyStrips] = {"kBunnyJumpFrames", "kBunnyTurnFrames",
                                                   "kBunnyTurnFrames"};
        std::string s;
        for (int st = 0; st < kBunnyStrips; ++st) {
            char head[200];
            std::snprintf(head, sizeof(head), "inline constexpr BunnyBake %s[%s] = {\n", kVar[st],
                          kCount[st]);
            s += head;
            // A STRIP THAT DID NOT LOAD EXPORTS THE TABLE IT ALREADY HAS, not
            // an empty brace list. The paste target is a fixed-size array, so
            // `= {};` is legal C++ that zero-fills it -- which would silently
            // throw away a committed bake because one .vox happened to be
            // missing from a folder the day somebody pressed [C].
            const std::vector<BunnyBake> committed(bunnyBake(st),
                                                   bunnyBake(st) + bunnyBakeCount(st));
            const std::vector<BunnyBake> &b = bake_[st].empty() ? committed : bake_[st];
            for (size_t i = 0; i < b.size(); ++i) {
                char row[160];
                // Four rows a line, which is what the file already does, so a
                // paste over the identity table produces a diff of the numbers
                // that changed rather than of the whole block.
                std::snprintf(row, sizeof(row), "%s{%d, %d, %d, %d, %d, %d},%s",
                              (i % 4 == 0) ? "    " : "  ", b[i].src, b[i].ox, b[i].oy, b[i].oz,
                              b[i].yaw, b[i].pitch, (i % 4 == 3 || i + 1 == b.size()) ? "\n" : "");
                s += row;
            }
            s += "};\n";
        }
        return s;
    }

    // Has anything been said that the compiled tables do not already say?
    bool dirty() const {
        for (int st = 0; st < kBunnyStrips; ++st) {
            const std::vector<BunnyBake> &b = bake_[st];
            const size_t lim = mini(int(b.size()), bunnyBakeCount(st));
            for (size_t i = 0; i < lim; ++i) {
                const BunnyBake &c = bunnyBake(st)[i];
                if (b[i].src != c.src || b[i].ox != c.ox || b[i].oy != c.oy || b[i].oz != c.oz ||
                    b[i].yaw != c.yaw || b[i].pitch != c.pitch)
                    return true;
            }
        }
        return false;
    }

    // -- WHAT THE PANEL SAYS ------------------------------------------------
    // Built here rather than in app.h's onGuiRender because every number on it
    // is this class's state, and a readout assembled next to the widget is a
    // readout that drifts from what the tool is actually holding.
    void hudLines(std::vector<std::string> *out) const {
        out->clear();
        if (!on_ || !buns_) return;
        const int n = frameCount();
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s   frame %d/%d%s", bunnyStripName(strip_),
                      n ? sel_ + 1 : 0, n, playing_ ? "" : "   PAUSED");
        out->push_back(buf);
        if (n <= 0) {
            out->push_back("no frames loaded");
            return;
        }
        const BunnyBake &b = bake_[strip_][size_t(sel_)];
        std::snprintf(buf, sizeof(buf), "plays %02d.vox   %+d %+d %+d vox   yaw %d  pitch %d",
                      b.src, b.ox, b.oy, b.oz, ((b.yaw % 4) + 4) % 4, ((b.pitch % 4) + 4) % 4);
        out->push_back(buf);
        std::snprintf(buf, sizeof(buf), "nudge axis %c%s", "XYZ"[axis_],
                      dirty() ? "   * unexported" : "");
        out->push_back(buf);
        if (!msg_.empty()) out->push_back(msg_);
    }

    // The key list, for F1 and for the panel. One source for both, so the help
    // cannot go on describing a binding after it has moved.
    static const char *const *help(int *n) {
        static const char *const kRows[] = {
            "  ,  .                  previous / next frame (pauses)",
            "  left / right          reorder: this frame earlier / later",
            "  up / down             nudge it one voxel along the current axis",
            "  G                     ...and which axis that is: X, Y, Z",
            "  R / SHIFT+R           yaw the pose a quarter turn",
            "  V / SHIFT+V           pitch it a quarter turn",
            "  B                     next strip: jump, turn left, turn right",
            "  K                     play / pause",
            "  C                     COPY THE BAKE to the clipboard, as C++",
            "  N                     throw this strip's edits away",
        };
        *n = int(sizeof(kRows) / sizeof(kRows[0]));
        return kRows;
    }

  private:
    int frameCount() const { return buns_ ? buns_->frames(strip_) : 0; }

    void pause() { playing_ = false; }

    void step(int d) {
        const int n = frameCount();
        if (n <= 0) return;
        pause();
        sel_ = ((sel_ + d) % n + n) % n;
        clock_ = float(sel_);
        msg_.clear();
    }

    // -- REORDER: SWAP TWO SLOTS' src, NOT TWO FRAMES ----------------------
    //
    // v1 swaps the frame OBJECTS in its array. It can: its frames are parsed
    // voxels living in the editor. Here the strip is eleven BLASes on the
    // device that the wood is also using, so what moves is the row that says
    // WHICH of them plays WHEN -- which is the same edit, expressed where it
    // can actually be baked. The selection follows the frame, as v1's does.
    void reorder(int d) {
        const int n = frameCount();
        if (n < 2) return;
        pause();
        const int j = sel_ + d;
        if (j < 0 || j >= n) return;   // no wrap: v1's edMoveStep refuses it too
        std::vector<BunnyBake> &b = bake_[strip_];
        const BunnyBake t = b[size_t(sel_)];
        b[size_t(sel_)] = b[size_t(j)];
        b[size_t(j)] = t;
        sel_ = j;
        clock_ = float(sel_);
        say("frame %d now plays %02d.vox", sel_ + 1, b[size_t(sel_)].src);
    }

    // -- NUDGE, AND THE TAIL OF THE STRIP COMES WITH IT --------------------
    //
    // This is v1's ED_FOLLOW = 1 and it is the setting that makes offsetting a
    // strip tractable. A bound is a body travelling: frame 5 is not two voxels
    // forward of the ORIGIN, it is two voxels forward of frame 4, and every
    // frame after it inherits that. Moving one frame alone means re-typing the
    // same delta into six more rows and re-checking all of them.
    //
    // ONLY THE FRAMES AFTER IT. v1 says why in one line -- "everything before
    // it is work you have already signed off".
    //
    // PER AXIS, WHICH IS THE ONE PLACE THIS DEPARTS FROM v1. Its follow assigns
    // the whole offset (`g.ox = f.ox; g.oy = f.oy; g.oz = f.oz`), so a nudge in
    // Y also flattens X and Z down the tail -- and a bound is built by laying
    // travel into oz frame by frame and THEN adding the rise, which that would
    // undo every time. Carrying only the axis you moved leaves the other two
    // where you put them.
    void nudge(int d) {
        const int n = frameCount();
        if (n <= 0) return;
        pause();
        std::vector<BunnyBake> &b = bake_[strip_];
        int *f = comp(&b[size_t(sel_)], axis_);
        *f += d;
        for (int i = sel_ + 1; i < n; ++i) {
            *comp(&b[size_t(i)], axis_) = *f;
        }
        const BunnyBake &s = b[size_t(sel_)];
        say("%+d %+d %+d vox (and the %d after it)", s.ox, s.oy, s.oz, n - 1 - sel_);
    }

    // A quarter turn of THIS frame only. v1 is explicit that rotation is not
    // carried down the strip the way an offset is -- "[r] is per-frame by
    // nature, and a sequence whose frames are rotated to a common heading is
    // exactly what the bake tables are for".
    void turn(int axis, int d) {
        if (frameCount() <= 0) return;
        pause();
        BunnyBake &b = bake_[strip_][size_t(sel_)];
        int &q = axis ? b.yaw : b.pitch;
        q = ((q + d) % 4 + 4) % 4;
        say("%s %d quarter%s", axis ? "yaw" : "pitch", q, q == 1 ? "" : "s");
    }

    void clearStrip() {
        const int n = mini(frameCount(), bunnyBakeCount(strip_));
        for (int i = 0; i < n; ++i) bake_[strip_][size_t(i)] = bunnyBake(strip_)[i];
        say("%s back to the committed bake", bunnyStripName(strip_));
    }

    // -- THE CLIPBOARD, WHICH IS THE POINT OF THE WHOLE TOOL ---------------
    //
    // ...AND THE CONSOLE AS WELL, ALWAYS. OpenClipboard fails outright when
    // another process is holding it, and it does so by returning false rather
    // than by anything the user would see -- v1 hit the same class of problem
    // from the other side (navigator.clipboard REJECTS on an unfocused page,
    // "a bare try/catch would have reported success on the one path most likely
    // to fail"). Printing it too means the copy can fail and the work still be
    // in front of you.
    void copyOut() {
        const std::string src = bakeSource();
        const bool ok = toClipboard(src);
        std::printf("v2: asset editor -- bake %s:\n%s", ok ? "copied" : "NOT copied (clipboard busy)",
                    src.c_str());
        std::fflush(stdout);
        say(ok ? "copied -- paste it into bunnies.h" : "clipboard busy -- it is in the console");
    }

    static bool toClipboard(const std::string &s) {
        if (!OpenClipboard(nullptr)) return false;
        bool ok = false;
        if (EmptyClipboard()) {
            // +1 for the terminator: CF_TEXT is a NUL-terminated block, and a
            // handle sized to strlen() hands every reader whatever follows it.
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
            if (h) {
                if (void *p = GlobalLock(h)) {
                    std::memcpy(p, s.c_str(), s.size() + 1);
                    GlobalUnlock(h);
                    // OWNERSHIP PASSES TO THE CLIPBOARD on success, so the
                    // handle is freed here only when it does not.
                    ok = SetClipboardData(CF_TEXT, h) != nullptr;
                }
                if (!ok) GlobalFree(h);
            }
        }
        CloseClipboard();
        return ok;
    }

    static int *comp(BunnyBake *b, int axis) {
        return axis == 0 ? &b->ox : axis == 1 ? &b->oy : &b->oz;
    }

    template <typename... A>
    void say(const char *f, A... a) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), f, a...);
        msg_ = buf;
    }
    void say(const char *f) { msg_ = f; }

    Bunnies *buns_ = nullptr;
    bool on_ = false;
    Vec3 at_ = Vec3(0.0f, 0.0f, 0.0f);
    int strip_ = kStripHop;
    int sel_ = 0;
    int axis_ = 2;            // Z, which is the one a bound travels along
    bool playing_ = true;
    float clock_ = 0.0f;
    std::string msg_;
    std::vector<BunnyBake> bake_[kBunnyStrips];
};

}  // namespace v2
