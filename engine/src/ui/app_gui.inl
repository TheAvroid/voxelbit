// app_gui.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: onGuiRender: every ImGui panel
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // The readout, and the settings menu on Y.
    //
    // Every knob here is also a launch flag, and that is deliberate: the flags
    // were unusable as a way to FIND a setting, because choosing between them
    // meant knowing the answer already. Quitting, editing a command line and
    // reloading a scene of tens of millions of triangles to try one number is
    // not a way to learn what the number does. In here the change lands on the
    // next frame and the frame rate two lines above it reacts, so the trade is
    // visible while you make it.
    //
    // Rows that would need the world rebuilt -- the seed, the ring radius, the
    // densities -- are deliberately absent. A menu that silently does nothing
    // is worse than one that does not offer the control.
    // -----------------------------------------------------------------------
    // =======================================================================
    // A SETTINGS CATEGORY, IN ITS OWN BOX.
    // =======================================================================
    //
    // (user 2026-09-20: "I want you to also give each category in the settings
    //  its own box. so there should be 3 boxes that make up the settings. can
    //  you use clean ui practices" -- with a card-based dashboard for
    //  reference.)
    //
    // THE THREE WERE TABS, AND A TAB HIDES TWO THIRDS OF ITSELF. That is the
    // right trade when the categories compete for one screen and the wrong one
    // here: this panel already scrolls, so the tabs bought nothing and cost the
    // player a click to find out whether a setting even exists. Three cards
    // down the panel show all of it and read as three things rather than one
    // long list.
    //
    // STACKED, NOT IN A ROW. The reference is a dashboard of small tiles; these
    // hold sliders with labels beside them, and three columns of those at this
    // width would wrap every row. Full-width cards are the same idea applied to
    // the content that is actually here.
    //
    // WHAT MAKES IT A CARD: a rounded fill a shade off the panel, a one-pixel
    // edge in the wood's green, a title in the same green, and air inside it.
    // Nothing else -- no shadow (ImGui has none to give that is not a second
    // draw) and no second background behind the widgets.
    static constexpr float kCardPadX = 12.0f, kCardPadY = 10.0f;
    // THE GAP BETWEEN COLUMNS, IN CHARACTERS RATHER THAN PIXELS. A raw 14 px
    // is what this was, and on a 3820-wide capture it came to four tenths of
    // one percent of the panel -- so the three fills sat flush against each
    // other and read as ONE box again, which is the thing the whole layout
    // exists to avoid. Measured against the font, it holds at any size.
    static constexpr float kCardGapCh = 4.0f;
    float cardGap_ = 14.0f;
    // Each box's height as it was LAST frame -- see the placement block.
    float cardH_[3] = {0.0f, 0.0f, 0.0f};
    float cardHWas_[3] = {-1.0f, -1.0f, -1.0f};   // ...and the frame before
    // Where each box's top edge was, and how tall the heading is -- the two
    // things that let the word sit on the boxes. See the header placement.
    float cardY_[3] = {0.0f, 0.0f, 0.0f};
    float headH_ = 0.0f;

    // -- A LINE THAT READS WITH NOTHING BEHIND IT ---------------------
    //
    // The three cards carry their own fill, so everything inside them is
    // legible. The READOUT above them is not in a card -- it is the price the
    // rows below are a trade against, not a category -- and with the panel's
    // own box gone it sits directly on a sunlit wood, where grey-on-green at
    // this size is unreadable.
    //
    // AN OUTLINE RATHER THAN A FOURTH BOX. The ask was three boxes; boxing
    // the header to make it readable would answer a legibility problem with
    // the one layout the ask rules out.
    //
    // A DROP SHADOW WAS TRIED FIRST AND WAS NOT ENOUGH -- captured and looked
    // at. A shadow only helps where the text is lighter than what is behind
    // it, and this panel opens over a sunlit wood: dim grey on light green
    // stays invisible whatever is drawn under it at ONE offset. An outline
    // works in every direction at once, which is why any HUD that has to
    // survive an arbitrary background uses one.
    //
    // DRAWN BEFORE THE TEXT, because the window draw list paints in order and
    // the outline has to be under it. TextUnformatted is what advances the
    // layout, so none of this costs space.
    void shadowLine(const char *t, const ImVec4 &col) {
        if (!t || !*t) return;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 ink = IM_COL32(0, 0, 0, 215);
        const float o = 2.0f;
        dl->AddText(ImVec2(p.x - o, p.y), ink, t);
        dl->AddText(ImVec2(p.x + o, p.y), ink, t);
        dl->AddText(ImVec2(p.x, p.y - o), ink, t);
        dl->AddText(ImVec2(p.x, p.y + o), ink, t);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
    }

    // -- THREE COLUMNS, NOT THREE ROWS --------------------------------
    //
    // (user 2026-09-20: "take the boxes out of the same scroll line. I dont
    //  want to scroll. Have them be next to eachother like on v1.")
    //
    // STACKED WAS THE WRONG CALL and the reason it was made is worth keeping:
    // the reference was a dashboard of small tiles, these cards hold sliders
    // with labels beside them, and three of THOSE at the panel's old width
    // would wrap every row. The answer was not to stack them -- it was to
    // make the panel three columns WIDE. Stacking bought a narrow panel and
    // paid for it in the one currency a settings screen cannot spend: you had
    // to scroll to find out whether a setting existed.
    //
    // `width` IS THE COLUMN and the caller owns it, because only the caller
    // knows how many columns there are and what the troughs had to give up to
    // fit them.
    // -- EACH BOX IS ITS OWN WINDOW NOW -------------------------------
    //
    // (user 2026-09-20: "have the ui boxes seperate from each other still.
    //  they drag together which is wrong.")
    //
    // THREE CARDS IN ONE WINDOW CAN ONLY EVER DRAG AS ONE. Everything the
    // last two rounds built -- the channel-split fills, the per-column clip
    // rect, the arithmetic that placed each column, the card-width rule --
    // existed to make three boxes out of one window, and all of it is gone.
    // A window is already a box that drags, remembers where it was put, and
    // clips its own contents; three of them are three boxes by construction
    // and the work is in deleting rather than adding.
    //
    // THE TITLE BAR IS THE CARD'S LABEL. It is also the handle, which is the
    // reason this reads right: a box you can pick up should say where to pick
    // it up, and a heading that is also a grip is the oldest answer there is.
    //
    // AUTO-SIZED, so nothing scrolls -- the other half of the same ask. A
    // window that fits its rows has no scrollbar to find and no content
    // hidden under the fold.
    // -- NO ARROW, WHICH MEANS DRIVING Begin OURSELVES -----------------
    //
    // (user 2026-09-20: "remove the arrows from the top of the settings
    //  boxes.")
    //
    // THE ARROW IS ImGui's COLLAPSE TOGGLE and the only way to be rid of it
    // is ImGuiWindowFlags_NoCollapse -- which Falcor's Gui::WindowFlags has
    // no member for and its pushWindow therefore cannot pass. There are four
    // flags in that translation and NoCollapse is not one of them.
    //
    // SO THE WINDOW IS OURS AND THE WIDGETS ARE STILL FALCOR'S. Gui::Widgets
    // has a protected default constructor and one member, `mpGui`, and every
    // widget method is `return mpGui ? mpGui->mpWrapper->add...` -- it needs
    // the Gui and nothing about the window. A two-line subclass that fills in
    // that pointer lets Begin be called with any flags we like while every
    // row below goes on being w.slider().
    //
    // IT ALSO DROPS Falcor's OWN PushFont. pushWindow pushes mpActiveFont
    // immediately after Begin, which is what made the pixel face need pushing
    // twice; driving Begin ourselves means the face we push is the face that
    // draws, once.
    struct cardWidgets : Gui::Widgets {
        explicit cardWidgets(Gui *g) { mpGui = g; }
    };

    static ImGuiWindowFlags cardWinFlags() {
        return ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    }

    // The fill, the edge and the title strip -- pushed around the three and
    // popped after them, because styleV2 is shared with the water panel, the
    // stack and the HUD, which are each one box and want the panel colour.
    struct cardSkin {
        cardSkin() {
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::kCard());
            ImGui::PushStyleColor(ImGuiCol_TitleBg, ui::kCard());
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ui::kCard());
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, ui::kCard());
            ImGui::PushStyleColor(ImGuiCol_Border, ui::kCardEdge());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kGreen());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        }
        ~cardSkin() {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(6);
        }
    };

    // A RULE ACROSS THE CARD -- which is a plain separator again, now that
    // the card IS a window. It was drawn by hand against a column's own x for
    // two rounds because ImGui sizes a separator from the WINDOW, and the
    // window was all three columns. One box per window makes the library's
    // answer the right one.
    // -- THE HANDLE THE CURSOR IS ON GROWS ----------------------------
    //
    // (user 2026-09-20: "the circle expands when the cursor hovers.")
    //
    // IMGUI HAS NO PER-WIDGET GRAB SIZE and no hover state until AFTER the
    // widget has been drawn, which is one frame too late to size it. So the
    // answer is the same one the equal-height cards used: remember which one
    // was hovered LAST frame and draw that one bigger. A frame of lag on a
    // mouse-over is below noticing -- the cursor is still travelling -- and
    // the alternative is drawing every slider twice.
    //
    // ONE INDEX, NOT A SET, because exactly one widget can be hovered. The
    // counter is reset at the top of the panel, so a row's identity is its
    // POSITION in the draw order -- which is stable as long as the panel does
    // not reorder itself mid-frame, and it does not.
    //
    // A TEMPLATE because Gui::Window::slider is one, over the value type and
    // over five optional arguments. Forwarding is the only way to wrap it
    // without writing the overload set out again.
    // -- ...AND IT GROWS AS A CIRCLE, WHICH GrabMinSize CANNOT DO ------
    //
    // (user 2026-09-20: "make it expand outwards making a bigger circle, not
    //  an oval as it is currently.")
    //
    // GrabMinSize IS THE GRAB'S WIDTH AND ITS HEIGHT IS THE ROW'S. That is
    // the whole of the bug: the handle is a circle only while the two happen
    // to be equal, and growing the one ImGui exposes stretches it sideways
    // into exactly the oval that was reported. The height comes from
    // FramePadding, and pushing THAT for one row makes the row taller and
    // shunts every row under it down as the cursor passes -- a worse artefact
    // than the one being fixed.
    //
    // SO THE BIG ONE IS DRAWN, not styled. The grab keeps its circle at all
    // times and a larger filled circle goes over it at the same centre, which
    // is a true circle at any radius and moves no layout at all.
    //
    // THE CENTRE IS COMPUTED FROM THE CURSOR, NOT FROM GetItemRect. Falcor
    // draws the trough and then the label beside it, so the last item after
    // the call is the TEXT -- asking the item rect gives the label's box and
    // puts the circle in the wrong place entirely. The cursor before the call
    // plus CalcItemWidth is the trough, whatever is drawn afterwards.
    //
    // `label, value, lo, hi` ARE NAMED rather than swallowed by the pack,
    // because the fraction along the trough is the one thing a wrapper cannot
    // get any other way. Everything past them still forwards.
    template <typename T, typename... R>
    bool hoverSlider(Gui::Widgets &w, const char *label, T &v, T lo, T hi, R &&...r) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float tw = ImGui::CalcItemWidth();
        const float th = ImGui::GetFrameHeight();
        // -- A PERFECT CIRCLE AT REST (user 2026-09-21: "make the slider
        //    circles perfect circles").
        //
        //    THE RESTING GRAB WAS NOT A CIRCLE AND NOT EVEN AN OVAL -- it was
        //    a RECTANGLE. GrabRounding defaults to 0 and nothing here ever set
        //    it, so what ImGui drew was a 12 px bar as tall as the row. Only
        //    the hovered handle was round, because that one is drawn by hand a
        //    few lines down; the other dozen rows in the panel were oblongs.
        //
        //    TWO VARS, AND BOTH ARE NEEDED. Rounding alone on a 12 x 20 grab
        //    gives a vertical stadium, not a circle -- the shape has to be
        //    SQUARE first. So GrabMinSize is pinned to the row height and the
        //    rounding to half of it, which is a circle by construction at any
        //    font scale.
        //
        //    PUSHED HERE RATHER THAN IN cardSkin: the row height is only known
        //    once the window's font scale has been applied, and cardSkin runs
        //    before Begin. GetFrameHeight() at this point is the real one.
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, th);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, th * 0.5f);
        const bool changed = w.slider(label, v, lo, hi, std::forward<R>(r)...);
        ImGui::PopStyleVar(2);
        // -- ...AND IT GROWS OVER TIME, NOT ON THE FRAME (user 2026-09-21:
        //    "make the bigger knob effect do a smooth transition into the
        //     bigger circle knob. right now it is instant.")
        //
        //    `sliderGrow_` is 0..1 and eased in onGuiRender; SMOOTHSTEPPED
        //    here rather than there because the curve belongs to the look and
        //    the easing belongs to the clock. A linear ramp on a radius reads
        //    as a pop at both ends -- the eye sees the first and last frames
        //    of it, and those are exactly where a linear ramp has its corners.
        if (sliderN_ == sliderShown_ && sliderGrow_ > 0.001f) {
            // `th`, NOT GetStyle().GrabMinSize -- the push above has already
            // been popped by here, so the style would read back ImGui's 12 and
            // the big circle would be centred on a travel the grab never had.
            const float d = th;
            const float span = maxf(1e-6f, float(hi) - float(lo));
            const float f = clampf((float(v) - float(lo)) / span, 0.0f, 1.0f);
            const ImVec2 c(p0.x + d * 0.5f + f * maxf(0.0f, tw - d), p0.y + th * 0.5f);
            const float e = sliderGrow_ * sliderGrow_ * (3.0f - 2.0f * sliderGrow_);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                c, d * 0.5f * (1.0f + (kGrabGrow - 1.0f) * e),
                ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SliderGrabActive
                                                         : ImGuiCol_SliderGrab),
                // 0 = let ImGui tessellate for the radius. It was 24, which is
                // enough for a small knob and visibly faceted once the hover
                // has grown it by half again.
                0);
        }
        if (ImGui::IsItemHovered()) sliderHotNext_ = sliderN_;
        ++sliderN_;
        return changed;
    }
    static constexpr float kGrabGrow = 1.45f;
    // HOW LONG THE GROWTH TAKES. Short enough to feel like a response to the
    // cursor rather than an animation playing at you; long enough that the
    // eye reads it as a move rather than a cut.
    static constexpr float kGrabGrowSec = 0.11f;
    int sliderN_ = 0;         // this frame's running index
    int sliderHot_ = -1;      // which one the cursor was on LAST frame
    int sliderHotNext_ = -1;  // ...and which it is on now
    // WHICH ROW THE BIG CIRCLE IS ON, AND HOW FAR OUT IT IS (0..1).
    //
    // `sliderShown_` is NOT the same as `sliderHot_`, and that is the whole of
    // the handoff: moving the cursor straight from one slider to the next has
    // to shrink the first before it grows the second, or the circle teleports
    // -- which is the instant jump this replaced, just moved sideways.
    int sliderShown_ = -1;
    float sliderGrow_ = 0.0f;

    void cardRule() {
        ImGui::PushStyleColor(ImGuiCol_Separator, ui::kGreenDim(0.35f));
        ImGui::Separator();
        ImGui::PopStyleColor();
    }


    void onGuiRender(Gui *pGui) override {
        if (opt_.outGiven) return;
        // THE READOUT IS ONE NUMBER.
        //
        // It used to carry the resolutions, the denoiser mode, the clock, the
        // chunk and triangle counts and a line of key hints -- which is a
        // developer's console pinned over a wood you are trying to look at.
        // Everything it said is still available and better placed: the sizes
        // and the mode are rows in the settings menu that owns them, the clock
        // is in the sky, the counts are in --stats, and the keys are on F1.
        // THE READOUT STAYS TOP-LEFT while the menu is centred, and v2 gives
        // the reason: this number is read WHILE MOVING, and a number that
        // follows the crosshair around is a number in the way.
        const float fbW = float(getTargetFbo()->getWidth());
        const float fbH = float(getTargetFbo()->getHeight());

        // THE FACE FIRST, AND NOTHING ELSE ON THE FRAME THAT BAKES IT. The
        // window is what decides the size, so this cannot be done at load
        // time; see bakePx3 for why the frame is then given up.
        if (bakePx3(pGui, fbH)) return;

        // NO TITLE BAR, NO MOVE, NO RESIZE GRIP on either panel. v2 drew its
        // own title and put the panel where it belonged; ImGui's chrome on top
        // of that is a second title over the first and a drag handle for a
        // window that is not meant to be dragged.
        const Gui::WindowFlags kBare = Gui::WindowFlags::AutoResize | Gui::WindowFlags::NoResize;

        // ---- the frame rate ------------------------------------------------
        //
        // NO BOX. The veil is zero, so the window paints nothing and what is on
        // the screen is the number and the number only -- which is what a
        // readout meant to be GLANCED at should be. The panel was buying one
        // thing, contrast, and the shadow below buys it back for two pixels
        // instead of a rectangle: gold over snow or a bright sky is gold you
        // cannot read.
        //
        // TOP LEFT, which is where it began and where it is again (user
        // 2026-09-07). The corner it sits in is the only thing that has moved:
        // it is still the bare number, still gold, still with no panel behind
        // it. Left-aligned it needs no measuring to place -- the corner is the
        // corner -- but the width is still measured, because the recorder's
        // panel has to be told where the readout ends so the two do not stack
        // on top of each other. See `below`.
        // -------------------------------------------------------------------
        // THE WATER PANEL -- `--water-ui`, top right.
        //
        // NO KEY. It was [I], then [O], then [L], and none of them stuck; it is
        // a start-up flag now (user 2026-09-14: "you remove the water panel
        // from l"). See onKeyEvent for the whole of that history.
        //
        // One row per term the water actually does, each a live uniform bit
        // (kWF* in Shared.slang) rather than a rebuild. v1 keeps the same row
        // of switches in its WATER_BAKE and its note is the reason this exists:
        // a panel that cannot speak for every site "is a lie", so every one of
        // these turns off the WHOLE term wherever it is evaluated.
        //
        // TOP RIGHT, AND SET EVERY FRAME. ImGui remembers window positions in
        // an ini between runs, so asking once is not the same as asking.
        // -------------------------------------------------------------------
        if (waterPanelOpen_) {
            styleV2 style(pGui, px3_, 1.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
            Gui::Window ww(pGui, "water##v2", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            // NO KEY IN THE TITLE, because it no longer has one -- this said
            // "[I]" for a day after the panel had moved to [O], which is the
            // argument for not naming a binding in a label at all.
            ww.text("WATER");
            ww.separator();
            static const char *kWaterRows[10] = {
                "sun sparkle",      // kWFGlint -- the PIXELATED one
                "caustics",         // kWFCaustic
                "world reflection", // kWFReflect
                "absorption",       // kWFAbsorb
                "in-scatter",       // kWFScatter
                "sunlight to bed",  // kWFSunPath
                "voxel swell",      // kWFSwell
                "surface ripple",   // kWFRipple
                "shore foam",       // kWFFoam
                "sun glare",        // kWFSunGlare -- the smooth one
            };
            uint32_t wf = 0;
            for (int b = 0; b < 10; ++b) {
                ww.checkbox(kWaterRows[b], waterTerm_[b]);
                if (waterTerm_[b]) wf |= (1u << b);
            }
            tracer_.waterFlags = wf;
            ww.separator();
            // ---------------------------------------- THE WAVE HEIGHT, LIVE
            // The nine above are on/off; this one is the amount, and it is the
            // term that has actually been argued about. "Water mounds" twice
            // and "you turned off the waves" once is one number being set from
            // two rooms away, so it belongs on the panel with the rest of the
            // water and not in a command-line flag nobody has open.
            //
            // 0 is a mirror, 1.0 is the full authored table (0.256 m of
            // displacement over a 5.2 m wavelength -- a sea state), and the
            // default sits under half of that.
            ww.slider("wave height", tracer_.waterWaveGain, 0.0f, 1.0f);
            ww.separator();
            if (ww.button("all on")) {
                for (int b = 0; b < 10; ++b) waterTerm_[b] = true;
            }
            const ImVec2 wsz = ImGui::GetWindowSize();
            ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - wsz.x - 12.0f), 12.0f));
        }

        // -------------------------------------------------------------------
        // THE STACK BADGE'S FOUR NUMBERS, LIVE.
        //
        // (user 2026-09-14: "let me adjust the display number just like how we
        // were able to in v1.")
        //
        // v1 tunes this from a card with exactly four sliders -- SB_K is
        // ['x', 'y', 'size', 'tilt'] -- and these are the same four. Built on
        // the water panel rather than on the settings menu because it is the
        // same KIND of thing: a handful of numbers you move while looking at
        // what they do, not a preference you set once.
        //
        // IT SHOWS THE COUNT IT IS TUNING and forces one if the hand is empty
        // of a stack, because four sliders that move nothing you can see are
        // four sliders nobody can use. v1 does the same -- "empty hand -> tune
        // the axe, the same fallback the pose card takes".
        //
        // NOT PER ITEM, which v1's is. Its badge is a screen-space blit whose
        // right place depends on how the model happens to project; this one
        // hangs off the model's own measured half-width, so the same four
        // numbers frame every tool in the kit. If one item ever needs its own,
        // the shape of it is v1's sbCfgs table.
        // -------------------------------------------------------------------
        // -------------------------------------------------------------------
        // EVERYTHING THAT POSES WHAT IS IN YOUR HAND, ON ONE KEY.
        //
        // (user 2026-09-17: "also let me adjust the aim down sights position in
        // the settings menu. put the hand held item adjustments and the aim
        // down site adjustments all on the k keybind with the stack number
        // positionings. stack the different boxes on the right side above one
        // another.")
        //
        // THREE CARDS, ONE KEY, STACKED DOWN THE RIGHT. They were in two
        // different places and one of them did not exist: the pose rows lived
        // in the settings menu -- which you have to open, which takes the
        // mouse, and which is the wrong shape for a thing you tune while
        // looking at it -- and the stack card was already here on [K]. The
        // sighted pose had no rows at all.
        //
        // WHY THEY ARE NOT ONE WINDOW. Each is bound to something different:
        // the first two to the tool the wheel is on, the third to a badge that
        // may not be drawn at all. Three windows means each can size itself to
        // its own content and be read past -- and the stacking below is the
        // only thing that has to know they are related.
        //
        // THE CURSOR IS THE WHOLE LAYOUT. `stackY` walks down the right edge as
        // each card declares its height, so a card that grows pushes the ones
        // under it and nothing overlaps. ImGui reports a window's size only
        // AFTER its contents are submitted, which is why every card positions
        // itself at the END of its own block -- the same order the water panel
        // and the stack card already used, now with one number carried between
        // them.
        // -------------------------------------------------------------------
        if (stackPanelOpen_) {
            float stackY = 12.0f;
            const float stackGap = 8.0f;

            // ---- 1. THE HAND ITEM'S POSE ---------------------------------
            if (held_.ready()) {
                styleV2 style(pGui, px3_, 1.0f, fbH);
                ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
                Gui::Window hw(pGui, "hand##v2", {0, 0}, {0, 0}, kBare);
                px3Font face(px3_);
                ImGui::SetWindowFontScale(style.scale);
                hw.text("HAND ITEM");
                hw.separator();
                hw.text(held_.name());
                if (held_.count() > 1)
                    hw.text(fmt("  %d of %d -- the wheel changes tools", held_.selected() + 1,
                                held_.count()));
                hw.checkbox("in hand  (H)", held_.shown);
                hw.separator();
                // THE RANGES ARE IN WORLD VOXELS, like the pose itself. Thirty
                // voxels is three metres, further than a hand reaches in any
                // direction; forty forward is four, well past arm's length.
                hw.slider("  right", held_.pose().x, -30.0f, 30.0f, false, "%.3f");
                hw.slider("  up", held_.pose().y, -30.0f, 30.0f, false, "%.3f");
                hw.slider("  forward", held_.pose().z, 1.0f, 40.0f, false, "%.3f");
                hw.slider("  yaw", held_.pose().yaw, -PI, PI, false, "%.3f");
                hw.slider("  pitch", held_.pose().pitch, -PI, PI, false, "%.3f");
                hw.slider("  roll", held_.pose().roll, -PI, PI, false, "%.3f");
                // ONE IS EXACT: one model voxel per 10 cm world voxel. The
                // slider is still here because a viewmodel is judged by eye,
                // but anything other than 1.000 no longer matches the grid the
                // world is built on.
                hw.slider("  size", held_.pose().scale, 0.25f, 2.0f, false, "%.3f");
                // ---- WHERE THE ARROW SITS ON THE STRING ------------------
                //
                // IN WHOLE VOXELS -- the arrow is voxels stamped into the bow's
                // own grid and there is nowhere for half a voxel to land (see
                // ArrowOffset). THE ROWS ONLY ASK: rebuilding a structure from
                // inside the interface means a blocking submit in the middle of
                // a command list that is still being written, which took the
                // game down once. The slider writes a WANT and the frame acts
                // on it beside world_.update.
                if (held_.holdingBow()) {
                    bool moved = false;
                    moved |= hw.slider("  arrow across", arrowWant_.across, -12, 12);
                    moved |= hw.slider("  arrow along", arrowWant_.along, -12, 12);
                    moved |= hw.slider("  arrow up", arrowWant_.up, -12, 12);
                    if (moved) arrowDirty_ = true;
                }
                // COPY, NOT SAVE. The bake writes defaults.h and this pose is
                // not in it -- a viewmodel pose belongs beside the model it
                // poses rather than in a file of renderer settings. So the row
                // hands over the literal to paste into HeldPose.
                if (hw.button("copy pose")) {
                    poseCopied_ = fmt("{ %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff }",
                                      held_.pose().x, held_.pose().y, held_.pose().z,
                                      held_.pose().yaw, held_.pose().pitch, held_.pose().roll,
                                      held_.pose().scale);
                    poseCopied_ = std::string(held_.name()) + "  " + poseCopied_;
                    if (held_.holdingBow())
                        poseCopied_ += fmt("   arrow +{ %d, %d, %d } voxels on every frame",
                                           held_.arrow().across, held_.arrow().along,
                                           held_.arrow().up);
                    std::printf("v2: held pose %s\n", poseCopied_.c_str());
                    std::fflush(stdout);
                    ImGui::SetClipboardText(poseCopied_.c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted(poseCopied_.empty() ? "  tune it, then copy"
                                                           : poseCopied_.c_str());
                ImGui::PopStyleColor();
                const ImVec2 hs = ImGui::GetWindowSize();
                ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - hs.x - 12.0f), stackY));
                stackY += hs.y + stackGap;
            }

            // ---- 2. ...AND WHERE IT GOES DOWN THE SIGHTS ------------------
            //
            // ONLY FOR SOMETHING THAT HAS SIGHTS. `aimable` is set by handing
            // add() a second pose, so this card appears for the rifle and for
            // nothing else in the kit.
            if (held_.aimable()) {
                styleV2 style(pGui, px3_, 1.0f, fbH);
                ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
                Gui::Window aw2(pGui, "sights##v2", {0, 0}, {0, 0}, kBare);
                px3Font face(px3_);
                ImGui::SetWindowFontScale(style.scale);
                aw2.text("AIM DOWN SIGHTS");
                aw2.separator();
                // -- TICK THIS AND THE GUN COMES UP AND STAYS UP -------------
                //
                // The sighted pose is only on screen while the gun is up, and
                // until this existed the only way to raise it was to HOLD the
                // right button -- the button you have to let go of to drag a
                // slider. See HeldItem::adsHold: it is OR'd with the button
                // rather than being a second state, so what is tuned here is
                // exactly what the button gives and not a preview of it.
                aw2.checkbox("aim down sights while I tune", held_.adsHold);
                // ...AND WHAT IT IS ACTUALLY DOING. The ease takes 90 ms, so
                // this reads 0% for a few frames after the tick and 100% once
                // the gun has arrived; a slider dragged in between moves a pose
                // that is only part way there, which is worth being able to
                // see rather than guess at.
                aw2.text(fmt("  up the eye: %.0f%%", double(held_.adsAmount() * 100.0f)));
                aw2.separator();
                aw2.slider("  right", held_.adsPose().x, -30.0f, 30.0f, false, "%.3f");
                aw2.slider("  up", held_.adsPose().y, -30.0f, 30.0f, false, "%.3f");
                aw2.slider("  forward", held_.adsPose().z, 1.0f, 40.0f, false, "%.3f");
                aw2.slider("  yaw", held_.adsPose().yaw, -PI, PI, false, "%.3f");
                aw2.slider("  pitch", held_.adsPose().pitch, -PI, PI, false, "%.3f");
                aw2.slider("  roll", held_.adsPose().roll, -PI, PI, false, "%.3f");
                aw2.slider("  size", held_.adsPose().scale, 0.25f, 2.0f, false, "%.3f");
                if (aw2.button("copy sights pose")) {
                    adsCopied_ = fmt("{ %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff }",
                                     held_.adsPose().x, held_.adsPose().y, held_.adsPose().z,
                                     held_.adsPose().yaw, held_.adsPose().pitch,
                                     held_.adsPose().roll, held_.adsPose().scale);
                    adsCopied_ = std::string(held_.name()) + " ADS  " + adsCopied_;
                    std::printf("v2: sights pose %s\n", adsCopied_.c_str());
                    std::fflush(stdout);
                    ImGui::SetClipboardText(adsCopied_.c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted(adsCopied_.empty() ? "  paste into kRifleAds in app.h"
                                                          : adsCopied_.c_str());
                ImGui::PopStyleColor();
                const ImVec2 as = ImGui::GetWindowSize();
                ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - as.x - 12.0f), stackY));
                stackY += as.y + stackGap;
            }

            // ---- 3. THE STACK BADGE'S FOUR NUMBERS -----------------------
            {
            styleV2 style(pGui, px3_, 1.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
            Gui::Window sw(pGui, "stack##v2", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            const int sel = held_.ready() ? held_.selected() : -1;
            // THE CARD IS NAMED AFTER WHAT IT IS PLACING. The gun's badge is a
            // magazine and not a stack -- see setStackBadge -- and a card
            // headed STACK COUNT while you tune the ammo counter is the kind
            // of small lie that costs somebody ten minutes looking for the
            // other panel.
            sw.text(isGun(sel) ? "AMMO COUNT" : "STACK COUNT");
            sw.separator();
            sw.text(sel >= 0 ? held_.tool(sel).name : "nothing in hand");
            sw.checkbox("show a count while this is open", stackPanelForce_);
            sw.separator();
            // BOUND TO WHAT IS IN THE HAND, resolved every frame -- v1's own
            // rule for its card ("resolved from the hand each time the panel
            // refreshes, exactly as pkIt is"). Scroll to another tool and the
            // sliders are that tool's.
            StackCfg &cfg = stackFor(sel >= 0 ? sel : 0);
            sw.slider("size", cfg.cell, 0.003f, 0.05f, false, "%.4f m");
            sw.slider("across", cfg.across, -0.4f, 0.6f, false, "%.3f m");
            sw.slider("up", cfg.up, -0.3f, 0.4f, false, "%.3f m");
            sw.slider("tilt", cfg.tilt, -1.6f, 1.6f, false, "%.3f rad");
            sw.separator();
            if (sw.button("defaults")) cfg = StackCfg{};
            // -- THE BAKE (user 2026-09-14: "let me bake the stack count number
            //    text into here") ---------------------------------------------
            //
            // WHAT A BAKE IS HERE: the four numbers as the four SOURCE LINES
            // they live on, ready to paste over the declarations in
            // setStackBadge. Not a config file and not a save -- this engine
            // bakes by pasting, which is what the asset editor's [C] does with
            // its strip table and what the settings menu's "copy pose" does
            // with a HeldPose. A tuned number that only exists in a running
            // process is a number that dies with it.
            //
            // TO THE CONSOLE AS WELL, ALWAYS. assetedit.h's note is the reason:
            // OpenClipboard fails outright when another program is holding it,
            // and a copy that silently did nothing is worse than no copy. The
            // console is the copy that cannot fail.
            if (sw.button("bake")) {
                // THE ROW, AS THE LINE THAT DECLARES IT. The table is keyed by
                // the tool's NAME rather than by its slot, because a slot is
                // where a thing happens to sit in the wheel this build and a
                // name is what it is -- the same reason v1 keys sbCfgs by
                // ITEM_NAMES and not by index.
                stackBaked_ = fmt("        { \"%s\", { %.4ff, %.3ff, %.3ff, %.3ff } },",
                                  sel >= 0 ? held_.tool(sel).name : "?", double(cfg.cell),
                                  double(cfg.across), double(cfg.up), double(cfg.tilt));
                std::printf("v2: stack badge bake -- paste into kStackBakes:\n%s\n",
                            stackBaked_.c_str());
                std::fflush(stdout);
                ImGui::SetClipboardText(stackBaked_.c_str());
            }
            sw.text(stackBaked_.empty() ? "tune it, then bake" : "copied -- paste into app.h");
            char line[160];
            std::snprintf(line, sizeof(line), "%.4f  %.3f  %.3f  %.3f", double(cfg.cell),
                          double(cfg.across), double(cfg.up), double(cfg.tilt));
            sw.text(line);
            const ImVec2 ssz = ImGui::GetWindowSize();
            // UNDER THE OTHER TWO, not pinned to the bottom corner as it was
            // when it was the only card on this key. Clamped so that a tall
            // stack on a short window still has its last card on screen.
            ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - ssz.x - 12.0f),
                                       minf(stackY, maxf(0.0f, fbH - ssz.y - 12.0f))));
            }
        }

        // -------------------------------------------------------------------
        // THE ASSET EDITOR'S READOUT -- top right, only on the deck.
        //
        // NOT OPTIONAL AND NOT A TOGGLE, unlike every other panel in this file.
        // Which frame is selected, what it plays, how far it has been nudged
        // and which axis the arrows are pointing at are not settings you turn
        // on to check: they are the tool's ENTIRE state, and a nudge whose
        // effect you cannot read is a nudge you have to count in your head.
        // v1's #edHud is up for the same reason and says the same first line.
        //
        // The key list is under it, out of AssetEdit::help, so the panel and F1
        // cannot describe two different sets of bindings.
        // -------------------------------------------------------------------
        if (edit_.on() && !waterPanelOpen_) {
            std::vector<std::string> rows;
            edit_.hudLines(&rows);
            styleV2 style(pGui, px3_, 1.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(8.0f, 8.0f);
            Gui::Window ew(pGui, "asset editor##v2", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            ew.text("ASSET EDITOR  [I]");
            ew.separator();
            for (const std::string &r : rows) ew.text(r.c_str());
            ew.separator();
            int nh = 0;
            const char *const *hr = AssetEdit::help(&nh);
            for (int i = 0; i < nh; ++i) ew.text(hr[i]);
            const ImVec2 esz = ImGui::GetWindowSize();
            ImGui::SetWindowPos(ImVec2(maxf(0.0f, fbW - esz.x - 12.0f), 12.0f));
        }

        float below = 12.0f;
        // HOISTED out of the frame-rate block: the coordinate readout shares
        // this corner now and "the two corners agree" has to mean three.
        const float inset = 12.0f;
        {
            // Room for the shadow, and no more: a window's draw list is clipped
            // to its own rectangle, and at zero padding a two-pixel offset
            // loses its bottom-right corner.
            const float pad = 3.0f;
            styleV2 style(pGui, px3_, 0.0f, fbH);  // veil 0: no panel, no box
            ImGui::GetStyle().WindowPadding = ImVec2(pad, pad);
            Gui::Window fpsWin(pGui, "v2fps", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            // WHAT IS ON THE SCREEN, which is not what this counter naturally
            // measures. fpsFrames_ increments once per onFrameRender, so it
            // counts RENDERED frames; DLSS-G inserts its frames at the
            // swapchain, downstream of that function, and they never pass
            // through it. Reporting it raw made switching frame generation on
            // look like it COST frame rate -- the engine does render fewer real
            // frames, because DLSS-G takes GPU time and paces submission, while
            // more frames reach the screen.
            //
            // So the readout is rendered plus generated: one number, and the
            // one a person is actually looking at. With frame generation off
            // genFps_ is zero and this is exactly what it always was. The
            // rendered figure is still in the settings menu, where the two are
            // worth telling apart.
            //
            // AND IT IS ONLY THE NUMBER. The unit was worth its width while the
            // readout sat in a panel with other lines to be told apart from;
            // alone in a corner in gold there is nothing else it could be
            // counting.
            const std::string f = fmt("%.0f", fps_ + genFps_);
            const ImVec2 sz = ImGui::CalcTextSize(f.c_str());
            // Set every frame rather than on first use: ImGui remembers window
            // positions in an ini file between runs, so "where I asked for it"
            // and "where it appears" are otherwise two different things.
            ImGui::SetWindowPos(ImVec2(inset - pad, inset - pad));
            // WHERE THE NEXT THING IN THIS CORNER MAY START. The recorder's
            // panel shares the corner now, and a REC badge drawn over the frame
            // rate is two readouts and neither legible.
            below = inset + sz.y + 8.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 2.0f, at.y + 2.0f),
                                                IM_COL32(0, 0, 0, 150), f.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold());
            ImGui::TextUnformatted(f.c_str());
            ImGui::PopStyleColor();
        }

        // ---- the compass -------------------------------------------------
        //
        // (user 2026-09-21: "add a compass feature. put it in the top middle
        //  of the screen. v1 had this feature ... make the compass color
        //  gold. like the fps.")
        //
        // A STRIP, NOT A DIAL, which is what v1 drew and what a first-person
        // camera wants: a dial asks you to read a needle against a ring, and
        // the only thing you ever want off a compass while walking is "which
        // way am I pointed" -- one letter, under one mark, dead ahead.
        //
        // THE SAME DRESS AS THE FRAME RATE, because the ask says so and
        // because they are the same kind of thing: 3x3 pixel face, gold, and
        // the two-pixel black shadow that keeps gold legible over a bright
        // sky -- but NOT in a window; see the first line inside the test.
        if (opt_.compass) {
            // IT IS NOT IN A WINDOW, AND THAT IS THE WHOLE OF WHY IT WAS
            // INVISIBLE. This was first written as a bare Gui::Window with
            // nothing inside it, drawing into GetWindowDrawList() -- and a
            // window's draw list is CLIPPED TO ITS OWN RECTANGLE, which the
            // frame rate's note above already says in as many words. An
            // AutoResize window with no widgets in it is a few pixels of
            // padding wide, parked wherever ImGui last cascaded it, so every
            // glyph asked for at the middle of the screen landed outside the
            // clip and was thrown away. Nothing was ever on the screen.
            //
            // The foreground list is the viewport's own and is clipped to the
            // viewport; it is what the "press g to respawn" banner already
            // uses to put text in the middle of the screen, for exactly this
            // reason. See noticeText in app_hud.inl.
            ImDrawList *dl = ImGui::GetForegroundDrawList();

            // MEASURED AT THE SIZE IT IS DRAWN AT. CalcTextSize reports the
            // CURRENT WINDOW's font scale, and outside a window there is no
            // such thing to read -- it would measure one size while AddText
            // drew another. CalcTextSizeA takes the pixel size as an argument
            // and so cannot disagree with the AddText beside it.
            ImFont *face = px3_ ? px3_ : ImGui::GetFont();
            const float px = px3_ ? face->FontSize : maxf(1.0f, v2FontPx(fbH));
            auto wide = [&](const char *t) {
                return face->CalcTextSizeA(px, FLT_MAX, 0.0f, t);
            };

            // WHICH WAY IS NORTH. yaw_ is degrees and is built with
            // atan2(d.x, -d.z) -- see the "facing it" line in app_spawn.inl --
            // so zero looks down -Z. Calling that north makes +X east, which
            // is the convention every other heading in this engine already
            // uses and the one /locate prints.
            auto wrap180 = [](float d) {
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;
                return d;
            };
            struct Mark { float bearing; const char *label; };
            static const Mark kMarks[] = {
                {0.0f, "N"},   {45.0f, "NE"},  {90.0f, "E"},   {135.0f, "SE"},
                {180.0f, "S"}, {225.0f, "SW"}, {270.0f, "W"},  {315.0f, "NW"},
            };
            // HOW MUCH SKY THE STRIP COVERS. 120 degrees either side would fit
            // seven marks and read as a ruler; 60 shows two or three, which is
            // the span a person actually navigates by.
            const float kArcDeg = 60.0f;
            const float halfW = minf(240.0f, fbW * 0.16f) * (px / 12.0f);
            const float cx = fbW * 0.5f;
            const float top = 12.0f;

            // kGold() is an ImVec4 for the style stack; a draw list wants a
            // packed colour, and mixing the two is a compile error rather
            // than a wrong colour, which is the good outcome.
            const ImU32 gold = ImGui::GetColorU32(ui::kGold());
            const ImU32 shadow = IM_COL32(0, 0, 0, 150);
            const float drop = maxf(1.0f, px * 0.16f);

            for (const Mark &mk : kMarks) {
                const float off = wrap180(mk.bearing - yaw_);
                if (off < -kArcDeg || off > kArcDeg) continue;
                const float x = cx + (off / kArcDeg) * halfW;
                const ImVec2 tsz = wide(mk.label);
                const ImVec2 at(x - tsz.x * 0.5f, top);
                dl->AddText(face, px, ImVec2(at.x + drop, at.y + drop), shadow, mk.label);
                dl->AddText(face, px, at, gold, mk.label);
            }
            // ...AND THE MARK THAT SAYS WHICH ONE YOU ARE POINTED AT. Below
            // the letters rather than through them: a caret drawn over a glyph
            // is a smudge at this size.
            const float ty = top + wide("N").y + 1.0f;
            const float arm = maxf(3.0f, px * 0.33f);
            dl->AddTriangleFilled(ImVec2(cx + drop, ty + drop),
                                  ImVec2(cx - arm + drop, ty + arm * 1.5f + drop),
                                  ImVec2(cx + arm + drop, ty + arm * 1.5f + drop), shadow);
            dl->AddTriangleFilled(ImVec2(cx, ty), ImVec2(cx - arm, ty + arm * 1.5f),
                                  ImVec2(cx + arm, ty + arm * 1.5f), gold);
        }

        // ---- what the recorder has to say, top left ------------------------
        //
        // IT IS DRAWN INTO THE WINDOW, NOT INTO THE FRAME, so it can never end
        // up in the recording -- see the note at the capture site in
        // onFrameRender. The badge pulses because a recorder that is running is
        // the one piece of state where "I did not notice it was still on" is
        // expensive.
        //
        // THE PANEL OPENS ONLY WHEN THERE IS A LINE FOR IT, which the fps
        // readout used to guarantee and no longer does. An empty AutoResize
        // window is not nothing on the screen: it is a small rounded rectangle
        // in the corner with nothing inside it.
        //
        // So the saved notice is retired HERE, before the test, rather than
        // inside the window where it used to be -- expiring mid-draw would
        // leave exactly that empty rectangle for a frame.
        // ---- where you are, under the frame rate ---------------------------
        //
        // (user 2026-09-19: "give me a toggle for coords. x, y, and z coords.")
        //
        // THE PLAYER'S FEET, NOT THE EYE. pos_ is where the body stands, and a
        // coordinate readout is for knowing where you ARE -- somewhere to walk
        // back to, a spot to describe. The eye is 1.6 m of head above that and
        // would make every y a decimal nobody asked for.
        //
        // IN METRES, which is the unit the whole engine is written in and the
        // one /locate and every log line already print. Voxels would be ten
        // times the number and agree with nothing the player can read anywhere
        // else.
        //
        // IT SHARES THE CORNER, so it takes `below` from whatever drew above it
        // and hands it on -- the frame rate, then this, then the recorder. Two
        // readouts stacked on one another is two readouts and neither legible;
        // that is the rule the REC badge already follows.
        if (showCoords_) {
            const float pad = 3.0f;
            styleV2 style(pGui, px3_, 0.0f, fbH);
            ImGui::GetStyle().WindowPadding = ImVec2(pad, pad);
            Gui::Window cw2(pGui, "v2coords", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            ImGui::SetWindowPos(ImVec2(inset - pad, below - pad));
            const std::string line =
                fmt("%.0f  %.0f  %.0f", pos_.x, pos_.y, pos_.z);
            const ImVec2 sz = ImGui::CalcTextSize(line.c_str());
            below += sz.y + 8.0f;
            // A DROP SHADOW, like the frame rate's and for its reason: this
            // sits over open sky as often as over ground, and a pale number on
            // a pale cloud is not a readout.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 2.0f, at.y + 2.0f),
                                                IM_COL32(0, 0, 0, 150), line.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(196, 204, 214));
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }

        if (savedTake_.valid() && nowSeconds() - savedAt_ >= kSavedNotice)
            savedTake_ = vb::Take{};
        if (recHintAt_ >= 0.0 && nowSeconds() - recHintAt_ >= kSavedNotice)
            recHintAt_ = -1.0;
        if (recorder_.recording() || recorder_.busy() || savedTake_.valid() ||
            recHintAt_ >= 0.0) {
            styleV2 style(pGui, px3_, 0.60f, fbH);  // a lighter veil than the menu's
            Gui::Window hud(pGui, "v2hud", {0, 0}, {12, 12}, kBare);
            px3Font face(px3_);
            ImGui::SetWindowFontScale(style.scale);
            // UNDER THE FRAME RATE, not on top of it -- see `below`.
            ImGui::SetWindowPos(ImVec2(12.0f, below));
            if (recorder_.recording()) {
                const float pulse = 0.55f + 0.45f * std::sin(float(ImGui::GetTime()) * 4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.30f, pulse));
                ImGui::TextUnformatted(
                    fmt("REC  %s", clockLabel(recorder_.elapsed()).c_str()).c_str());
                ImGui::PopStyleColor();
                // The costs, only once they are non-zero. A held frame is the
                // recorder covering a hitch in the GAME; a dropped one is the
                // encoder falling behind the recorder. They are different
                // problems and the readout does not merge them.
                if (recorder_.heldFrames() || recorder_.droppedFrames()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(200, 160, 90));
                    ImGui::TextUnformatted(fmt("  %lld held  %lld dropped",
                                               (long long)recorder_.heldFrames(),
                                               (long long)recorder_.droppedFrames())
                                               .c_str());
                    ImGui::PopStyleColor();
                }
            } else if (recorder_.busy()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(255, 214, 120));
                ImGui::TextUnformatted("encoding...");
                ImGui::PopStyleColor();
            } else if (savedTake_.valid()) {
                // WHERE THE PERSON WHO PRESSED R IS ACTUALLY LOOKING. The take
                // is written and the recorder names it on stdout, but stdout is
                // behind the window -- and with nothing opening any more, a
                // silent stop is indistinguishable from a stop that failed.
                //
                // It FADES rather than waiting to be dismissed. An
                // acknowledgement is not a dialog: the thing wanted after a
                // take is the wood back, not another key to press.
                const double f = (kSavedNotice - (nowSeconds() - savedAt_)) / 1.2;
                const float a = float(f > 1.0 ? 1.0 : f);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(150, 220, 160, a));
                ImGui::TextUnformatted(fmt("saved  %s", savedTake_.path.c_str()).c_str());
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(150, 158, 170, a));
                ImGui::TextUnformatted(fmt("  %lld frames  %s",
                                           (long long)savedTake_.frames,
                                           clockLabel(savedTake_.seconds()).c_str())
                                           .c_str());
                ImGui::PopStyleColor();
            }
            // -- WHERE THE RECORDER IS, FOR SOMEONE HOLDING A GUN -----------
            //
            // Raised by the reload in onKeyEvent, which is the one moment the
            // player has just pressed what they believe is the record key and
            // watched a magazine go in instead. It fades on the take notice's
            // own clock rather than waiting to be dismissed -- the thing
            // wanted in a firefight is the firefight, not another key.
            if (recHintAt_ >= 0.0) {
                const double f = (kSavedNotice - (nowSeconds() - recHintAt_)) / 1.2;
                const float a = float(f > 1.0 ? 1.0 : f);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::rgb(255, 214, 120, a));
                ImGui::TextUnformatted("f9 records -- r reloads the gun");
                ImGui::PopStyleColor();
            }
        }

        // ---- the copyright, bottom centre, always --------------------------
        //
        // The JS engine's #copyr watermark, brought over as it is written
        // there: the same wording, the same gold, the same 0.64 opacity, the
        // same ten pixels off the bottom edge, centred. Its own note says why
        // it sits outside every overlay over there -- "it has to survive on the
        // loading screen, in play and on the esc menu alike" -- and the same
        // holds here, which is why it is drawn unconditionally rather than
        // beside the readout that comes and goes.
        //
        // THE (c) IS A LETTER AND A RING, and that is not a flourish: the 3x3
        // pixel face has no copyright glyph, so that engine builds the mark out
        // of a px3 "c" with a circle drawn round it. Anything else -- a Unicode
        // (c) from a fallback face, or the two characters -- is a different
        // typeface in the middle of a word. Same trick here, with the circle on
        // the window's own draw list.
        //
        // NOT IN THE RECORDING, like every other thing drawn here: the capture
        // happens in onFrameRender, upstream of the whole interface. That
        // matches the engine it comes from, whose watermark is a DOM element
        // over a canvas the recorder never sees.
        {
            // Padding, and it is NOT what keeps the ring off the window's
            // clip edge -- see the PushClipRectFullScreen below. Raising this
            // was the first fix tried and it cannot work, for a reason worth
            // writing down: ImGui insets InnerClipRect by HALF the window
            // padding but places the CURSOR at the full padding, so every
            // pixel added here moves the text one pixel further from the left
            // edge and the clip only half a pixel to meet it. The margin grows
            // at half a pixel per pixel spent, and the ring -- which starts to
            // the LEFT of the text origin -- keeps losing the race.
            const float pad = 11.0f;
            styleV2 style(pGui, px3_, 0.0f, fbH);  // no panel behind it either
            ImGui::GetStyle().WindowPadding = ImVec2(pad, pad);
            Gui::Window cop(pGui, "v2copy", {0, 0}, {0, 0}, kBare);
            px3Font face(px3_);
            // -- A QUARTER SMALLER (user 2026-09-21) -----------------------
            //
            // SCALE, NOT A FONT SIZE. px3_ is baked at one size and ImGui
            // scales the atlas, so the window scale is the only lever -- the
            // same note the settings heading carried when it wanted twice.
            //
            // EVERYTHING BELOW MEASURES ITSELF, so nothing else moves: the
            // width comes from CalcTextSize, the centring divides by it, and
            // the (c) ring is sized in em of the glyph it encircles. This is
            // the second 25% the line has taken -- it was cut from 7px to
            // 5.25px when it landed -- and it still reads because a pixel
            // face has no hinting to lose.
            ImGui::SetWindowFontScale(style.scale * 0.75f);

            // A SPACE AFTER THE c, so the ring has somewhere to be. That engine
            // gets the room from a flex box; here the gap is the space glyph.
            static const char *kMark = "c";
            static const char *kRest = " 2026 voxelbit - all rights reserved";
            const ImVec2 ms = ImGui::CalcTextSize(kMark);
            const ImVec2 rs = ImGui::CalcTextSize(kRest);
            const float wide = ms.x + rs.x;
            ImGui::SetWindowPos(ImVec2((fbW - wide) * 0.5f - pad, fbH - rs.y - 10.0f - pad));

            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            // -- OUT FROM UNDER THE WINDOW'S CLIP RECTANGLE -----------------
            //
            // Everything here is drawn by hand at absolute positions, and the
            // ring reaches further left than the cursor this window sized
            // itself around -- so an auto-resized window's rectangle is simply
            // the wrong shape to clip it by, and it was shaving the left of
            // the circle. The stroke read one pixel there against two on the
            // right, which is what a half-clipped stroke looks like and why an
            // angular PRESENCE test scored it a clean hundred per cent: enough
            // of it survived to be found, and the wrong half was measured.
            //
            // The overlay is screen furniture at a fixed corner, so it is
            // pinned to the screen and clipped by nothing else. Popped below.
            dl->PushClipRectFullScreen();
            // The shadow first, the whole line at once -- #copyr carries one
            // too, and without it gold on a bright sky is unreadable.
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 150), kMark);
            dl->AddText(ImVec2(at.x + ms.x + 1.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 150), kRest);

            const ImU32 gold = ImGui::GetColorU32(ui::kGold(0.64f));
            dl->AddText(at, gold, kMark);
            dl->AddText(ImVec2(at.x + ms.x, at.y), gold, kRest);
            // -- THE RING, ROUND THE INK AND NOT ROUND THE LINE BOX --------
            //
            // CalcTextSize answers with the ADVANCE and the LINE HEIGHT, which
            // is a box the letter merely sits somewhere inside: for a lowercase
            // c that box is most of a line tall and the ink is a short bar
            // across the middle of it. Centring on that box put the circle high
            // and sized it to the leading, which is why it read as a letter
            // next to a circle rather than as a copyright mark.
            //
            // The glyph's own extent is the right thing to ask for, and ImGui
            // will give it: FindGlyph returns the ink box in the font's base
            // units, so scaling by the ratio of the current size to that base
            // puts it in pixels. The ring is then concentric with the letter by
            // construction, at any font size and after any SetWindowFontScale.
            //
            // HALF #copyr's 0.14 em OF THICKNESS, AND A PIXEL MORE RADIUS.
            //
            // The border width that is right in CSS is not right here, and the
            // reason is the size: at this text size 0.14 em is a stroke almost
            // as wide as the hole it encloses, so the ring filled in and the
            // gold c vanished ON TOP of gold. What was left to read was the c's
            // SHADOW -- a dark notch in a solid disc, sitting down and right of
            // centre because a shadow is offset by definition. The letter was
            // centred the whole time; there was nothing to see it against.
            //
            // So the stroke is halved and the circle grows by a pixel, which
            // puts daylight back between the ink and the ring. That is what the
            // CSS is really buying at ITS size, and this is the same look
            // arrived at through this font's numbers rather than through that
            // one's.
            {
                const ImFont *fnt = ImGui::GetFont();
                const ImFontGlyph *gl = fnt ? fnt->FindGlyph((ImWchar)'c') : nullptr;
                const float em = ImGui::GetFontSize();
                float cx = at.x + ms.x * 0.5f, cy = at.y + ms.y * 0.5f, rad = ms.x * 0.62f;
                if (gl && fnt->FontSize > 0.0f) {
                    const float k = em / fnt->FontSize;
                    const float x0 = at.x + gl->X0 * k, x1 = at.x + gl->X1 * k;
                    const float y0 = at.y + gl->Y0 * k, y1 = at.y + gl->Y1 * k;
                    cx = (x0 + x1) * 0.5f;
                    // THE GLYPH BOX IS THE CENTRE. There WAS a pixel of
                    // correction here, on the strength of a measurement that
                    // said the c sat low in the ring -- and the measurement was
                    // of the wrong thing. It was taken while the stroke was
                    // 0.14 em, thick enough that the gold c disappeared into
                    // gold and the only mark left to find was the c's SHADOW,
                    // which is offset +1,+2 because that is what a shadow is.
                    // So a shadow was measured and the ring was moved down to
                    // meet it.
                    //
                    // With the stroke halved the letter is visible and can be
                    // measured directly: ink rows 1963-1974 about a ring
                    // spanning 1959-1980, which is the box centre exactly. The
                    // correction is removed rather than re-tuned -- the metrics
                    // were right the whole time.
                    cy = (y0 + y1) * 0.5f;
                    // TWO PIXELS MORE AIR (user 2026-09-07), on top of the one
                    // the thinner stroke bought back. em * 0.13 is the gap
                    // #copyr gets from its box-sizing; the constant beside it
                    // is this face's, which is small enough that a proportional
                    // term alone cannot buy a whole pixel.
                    rad = maxf(x1 - x0, y1 - y0) * 0.5f + em * 0.13f + 3.0f;
                }
                const float ring = maxf(1.0f, em * 0.07f);
                // -- THE DARK IS AN OUTLINE, NOT A DROP SHADOW ---------------
                //
                // It was a drop shadow at the text's own +1,+2, and that ate
                // the left of the circle. A GLYPH is a solid block, so an
                // offset shadow only ever peeks out from behind it; a thin RING
                // is a one-pixel stroke, and an offset of one pixel lands the
                // dark arc squarely underneath it. The anti-aliased gold then
                // blends with black and the stroke muddies to nothing.
                //
                // Measured on a 16x crop before and after: coverage round the
                // left of the ring was 47-50% against 75-96% everywhere else,
                // and only the left -- because the y offset of two clears the
                // stroke while the x offset of one is about the width of it.
                //
                // So the dark is drawn CONCENTRIC and two pixels wider instead.
                // The gold covers the middle of it and dark shows on both
                // edges, which is what an outline is: the same contrast against
                // bright ground, and no side of the circle can be eaten because
                // nothing is offset into it.
                dl->AddCircle(ImVec2(cx, cy), rad, IM_COL32(0, 0, 0, 150), 0, ring + 2.0f);
                dl->AddCircle(ImVec2(cx, cy), rad, gold, 0, ring);
            }
            dl->PopClipRect();
            // The window has to be told how much it is holding: everything
            // above went straight to the draw list, which ImGui does not
            // measure. Without this the box is empty and collapses.
            ImGui::Dummy(ImVec2(wide, rs.y));
        }

        // ---- the console --------------------------------------------------
        // Drawn before the settings panel and independently of it: T and Y are
        // separate surfaces and either may be up without the other.
        if (consoleOpen_) {
            const float cw = fbW > 0 ? float(fbW) : 1280.0f;
            const float ch = fbH > 0 ? float(fbH) : 720.0f;
            const float margin = 12.0f;  // the HUD's inset, so the two line up
            const float boxW = minf(cw - margin * 2.0f, 720.0f);
            // BOTTOM LEFT, ANCHORED BY ITS BOTTOM EDGE. The pivot is what
            // makes that work: this window is AlwaysAutoResize, so it grows
            // downward by a line the moment a command prints a reply, and
            // positioning its top-left would push the prompt off the bottom of
            // the screen. Pivot (0,1) pins the BOTTOM-left corner instead, so
            // the reply opens upward and the caret never moves.
            ImGui::SetNextWindowPos(ImVec2(margin, ch - margin), ImGuiCond_Always,
                                    ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(boxW, 0.0f), ImGuiCond_Always);
            ImGui::Begin("##v2console", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings);
        // PUSHED BY HAND HERE, because the console is the one surface that is
        // not a Gui::Window inside a styleV2 -- it is a bare ImGui window with
        // its own End() below, and the pop has to happen before that End.
        if (px3_) ImGui::PushFont(px3_);
            // The caret has to be taken on the frame the box appears, or the
            // first keystroke is eaten deciding what is focused.
            if (consoleFocus_) {
                ImGui::SetKeyboardFocusHere();
                consoleFocus_ = false;
            }
            // NOTHING IS DRAWN ABOVE THE PROMPT ANY MORE. The reply used to
            // sit here, stacked over the input the way a game console reads,
            // but Enter now shuts the box before it could be read -- so the
            // reply moved to the fading line at the foot of this branch, which
            // takes over this exact corner once the window is gone.
            ImGui::TextUnformatted(">");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            bool submitted = false;
            // ------------------------------------------ TAB COMPLETES THE WORD
            // ImGui fires CallbackCompletion on Tab, which is also the only way
            // to get Tab at all -- the key otherwise moves focus between widgets
            // and never reaches the box.
            //
            // It commits to the COMMON PREFIX of the matches, not to the first
            // one: with `l` typed and lake1/lake2/longs all matching, Tab gives
            // `l` back and the ghost shows what the first match would be. A
            // completion that guesses is worse than one that waits.
            auto completeCb = [](ImGuiInputTextCallbackData *d) -> int {
                auto *self = static_cast<ForestApp *>(d->UserData);
                const std::string line(d->Buf, d->Buf + d->BufTextLen);
                const std::vector<std::string> hits = self->completions(line);
                if (hits.empty()) return 0;
                const std::string pre = commonPrefix(hits);
                const size_t cut = line.find_last_of(' ');
                const size_t tokStart = (cut == std::string::npos) ? 0 : cut + 1;
                if (pre.size() <= line.size() - tokStart) return 0;   // nothing to add
                d->DeleteChars(int(tokStart), int(line.size() - tokStart));
                d->InsertChars(int(tokStart), pre.c_str());
                // One match and it is complete: add the space so the next token
                // can be typed straight away.
                if (hits.size() == 1) d->InsertChars(d->CursorPos, " ");
                return 0;
            };
            if (ImGui::InputText("##cmd", consoleBuf_, sizeof(consoleBuf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_CallbackCompletion,
                                 completeCb, this)) {
                consoleMsg_ = runCommand(std::string(consoleBuf_));
                consoleBuf_[0] = 0;
                // CLOSES ON ENTER. A command is meant to be one keystroke to
                // open, the line, and done -- back in the wood looking at what
                // it did rather than reading past a box. The reply is not lost
                // with the window: it is handed to the fading line below, which
                // draws in exactly this corner for a few seconds afterwards.
                consoleMsgUntil_ = nowSeconds() + kConsoleMsgHold;
                submitted = true;
            }
            // ------------------------------------------------ THE GHOST
            // The rest of the best match, drawn dimmed at exactly the caret's
            // x so it reads as the word completing itself rather than as a
            // separate hint. Drawn AFTER InputText so it lands on top, and only
            // while the box holds something -- an empty line would otherwise
            // show "/locate" as though it had been typed.
            {
                const std::string line(consoleBuf_);
                if (!line.empty()) {
                    const std::vector<std::string> hits = completions(line);
                    if (!hits.empty()) {
                        const size_t cut = line.find_last_of(' ');
                        const size_t tokStart = (cut == std::string::npos) ? 0 : cut + 1;
                        const std::string rest = hits[0].substr(
                            std::min(hits[0].size(), line.size() - tokStart));
                        if (!rest.empty()) {
                            const ImVec2 rmin = ImGui::GetItemRectMin();
                            const ImVec2 pad = ImGui::GetStyle().FramePadding;
                            const float tw = ImGui::CalcTextSize(line.c_str()).x;
                            ImGui::GetWindowDrawList()->AddText(
                                ImVec2(rmin.x + pad.x + tw, rmin.y + pad.y),
                                IM_COL32(150, 150, 150, 160), rest.c_str());
                            if (hits.size() > 1) {
                                char tb[96];
                                std::snprintf(tb, sizeof tb, "  (%d matches, Tab)",
                                              int(hits.size()));
                                ImGui::GetWindowDrawList()->AddText(
                                    ImVec2(rmin.x + pad.x + tw +
                                               ImGui::CalcTextSize(rest.c_str()).x,
                                           rmin.y + pad.y),
                                    IM_COL32(120, 120, 120, 130), tb);
                            }
                        }
                    }
                }
            }

            // ---- ESC CLOSES IT, AND IT HAS TO BE ASKED HERE ----------------
            //
            // Not in onKeyEvent, which never sees the key. Falcor dispatches
            // keyboard as
            //
            //     if (mShowUI && mpGui->onKeyboardEvent(e)) return;   // eaten
            //     ... onKeyEvent(e);                                  // skipped
            //
            // and Gui::onKeyboardEvent returns io.WantCaptureKeyboard, which is
            // TRUE for every key while a text field is active. So the whole app
            // is deaf while you are typing -- by design, or typing "f" would
            // toggle fly mode.
            //
            // ImGui's own InputText does handle Escape, but only by reverting
            // the edit and dropping focus. That left the box open and unfocused
            // and took a second Escape to actually shut, which is what this is
            // fixing.
            //
            // IsKeyPressed rather than reading io.KeysDown: it is edge
            // triggered, so holding Escape does not close this and then arm the
            // quit on the next frame. ImGuiKey_Escape resolves because Falcor
            // populates io.KeyMap (Gui.cpp), which is worth knowing -- most of
            // its KeysDown indices are Falcor's own key codes, not ImGui's.
            const bool escaped = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (px3_) ImGui::PopFont();
            ImGui::End();
            // Closed AFTER End(), because setConsoleOpen may hand the mouse
            // back and the window still has to be finished either way. Escape
            // throws the line away and takes the old reply with it; Enter has
            // already run the line and wants the reply left up.
            if (escaped) {
                consoleMsgUntil_ = 0.0;
                setConsoleOpen(false);
            } else if (submitted) {
                setConsoleOpen(false);
            }
        } else if (!consoleMsg_.empty() && nowSeconds() < consoleMsgUntil_) {
            // ---- WHAT THE COMMAND SAID, AFTER THE BOX HAS GONE -------------
            //
            // Same corner, same font, no prompt and no input, so the reply
            // reads as the tail of the line you just typed rather than as a
            // second surface. NoInputs because the mouse is back on the camera
            // the instant Enter lands -- a window sitting there taking clicks
            // would steal the look for as long as it was up.
            const float cw = fbW > 0 ? float(fbW) : 1280.0f;
            const float ch = fbH > 0 ? float(fbH) : 720.0f;
            const float margin = 12.0f;
            const float boxW = minf(cw - margin * 2.0f, 720.0f);
            // Solid first and only fading over the last kConsoleMsgFade
            // seconds, so a reply you are still reading does not dim under you.
            const float left = float(consoleMsgUntil_ - nowSeconds());
            const float alpha = left >= kConsoleMsgFade ? 1.0f : left / kConsoleMsgFade;
            ImGui::SetNextWindowPos(ImVec2(margin, ch - margin), ImGuiCond_Always,
                                    ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowSize(ImVec2(boxW, 0.0f), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::Begin("##v2consolemsg", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing);
            if (px3_) ImGui::PushFont(px3_);
            ImGui::TextUnformatted(consoleMsg_.c_str());
            if (px3_) ImGui::PopFont();
            ImGui::End();
            ImGui::PopStyleVar();
        }

        // THE NOTICE, BEFORE THE EARLY RETURN. The settings panel is the one
        // thing that suppresses it and drawNoticeGui makes that decision
        // itself -- putting it after this line would mean the banner only ever
        // showed with the menu open, which is exactly backwards.
        drawNoticeGui(pGui);

        if (!menuOpen_) return;

        styleV2 style(pGui, px3_, 1.0f, fbH);
        // ITS OWN FLAGS, and both differences from the readout matter.
        //
        // AllowMove, because without it Falcor passes ImGuiWindowFlags_NoMove
        // and the panel cannot be dragged no matter what the position code
        // does. That was half of why it sat still; the other half was the
        // re-centring below.
        //
        // NOT AutoResize, because the panel has outgrown the screen. Auto-sized
        // windows do not scroll -- they simply grow, and the rows past the
        // bottom edge become unreachable. Giving it an explicit height capped
        // to the window turns the overflow into a scrollbar.
        // -- ...AND THE PANEL ITSELF HAS NO BOX ---------------------------
        //
        // (user 2026-09-20: "remove the big box that encompasses the smaller
        //  box categories. I want to see 3 seperate boxes.")
        //
        // THREE CARDS INSIDE A FOURTH IS A NESTED BOX, and nesting is the one
        // thing a card layout must not do: the outer fill says "these belong
        // together" at the same moment the inner three say "these are
        // separate", so the eye is told both at once. The cards ARE the panel
        // now -- the window keeps its position, its drag and its scrollbar,
        // and draws nothing of its own.
        //
        // PUSHED HERE RATHER THAN IN styleV2, which is shared: the water
        // panel, the stack and the HUD are each a single box and the fill is
        // right for every one of them.
        //
        // DECLARED BEFORE THE WINDOW, and that is the whole of why it is a
        // struct. ImGui reads these at Begin and they must be popped after
        // End -- Gui::Window ends in its DESTRUCTOR, so the only way to pop
        // afterwards is to be destroyed afterwards, which means being
        // constructed first.
        // -- THE PLACEMENT, ONCE, FOR THREE WINDOWS ---------------------
        //
        // See cardFlags/cardSkin. The three boxes are separate windows, so
        // ImGui owns each one's position from the first frame onward and this
        // only has to say where they START: side by side, centred, a fifth of
        // the way down. After that they are the player's to put anywhere, and
        // ImGui remembers each independently -- which is the whole ask.
        const float cw = ImGui::CalcTextSize("0").x * style.scale;
        // The trough, the gap, and the label beside it -- the same arithmetic
        // the one-column panel used, so every row is the width it was.
        float sliderW = 3.0f * 12.0f * cw;
        float colW = sliderW + 26.0f * cw + 32.0f;
        cardGap_ = kCardGapCh * cw;
        const float room = fbW * 0.98f;
        if (colW * 3.0f + cardGap_ * 2.0f > room) {
            // The TROUGHS give, never the labels: a shorter slider is still a
            // slider and a wrapped label is a broken row.
            sliderW = maxf(8.0f * cw,
                           sliderW - (colW * 3.0f + cardGap_ * 2.0f - room) / 3.0f);
            colW = sliderW + 26.0f * cw + 32.0f;
        }
        Gui::setSliderWidth(sliderW);

        // -- CONTROLS IS AS WIDE AS THE KEY LIST -------------------------
        //
        // The keys are laid out in COLUMNS (see keyHelpText) and the column
        // width the sliders want is narrower than the widest of those lines.
        // Pinned to it, the list was simply cut off at the window's edge --
        // captured and seen. Wrapping is not an option for text that is
        // already a table, so the window takes the width its content needs
        // and the two beside it are placed past THAT rather than past a
        // column they do not occupy.
        const float keyW = ImGui::CalcTextSize(keyBriefText()).x * style.scale;
        const float ctlW = maxf(colW, keyW + 4.0f * cw);

        // The hover index is per FRAME and per panel -- see hoverSlider.
        sliderN_ = 0;
        sliderHot_ = sliderHotNext_;
        sliderHotNext_ = -1;

        // ...AND THE GROWTH, ON THE FRAME CLOCK. Shrink the row that is being
        // left before adopting the one being entered; only when it has reached
        // zero does the circle change rows, so it never jumps the gap.
        {
            const float step = maxf(0.0f, ImGui::GetIO().DeltaTime) / kGrabGrowSec;
            if (sliderShown_ != sliderHot_) {
                sliderGrow_ -= step;
                if (sliderGrow_ <= 0.0f) {
                    sliderGrow_ = 0.0f;
                    sliderShown_ = sliderHot_;
                }
            } else {
                sliderGrow_ = clampf(sliderGrow_ + (sliderHot_ >= 0 ? step : -step), 0.0f, 1.0f);
            }
        }

        // -- TWO COLUMNS, AND THE BLOCK IS CENTRED ----------------------
        //
        // (user 2026-09-20: "move general under visuals. then move everything
        //  to the center of the screen.")
        //
        // CONTROLS IS THE TALL ONE -- it holds the whole key list -- so
        // stacking the two short boxes beside it is what squares the block
        // off. Three in a row left a wide strip of nothing under the two
        // that end early.
        //
        // CENTRING NEEDS THE HEIGHTS AND THE HEIGHTS NEED A FRAME. An
        // auto-sized window does not know how tall it is until it has been
        // laid out once, so the first placement uses whatever the last frame
        // measured -- zero, the first time -- and `menuPlaced_` is withheld
        // until all three have reported. The panel therefore settles on its
        // second frame and then stops moving, which is one frame nobody sees
        // and no guessed constants.
        const bool place = !menuPlaced_;
        ImVec2 at[3];
        if (place) {
            const float rightH = cardH_[1] + cardGap_ + cardH_[2];
            const float blockH = maxf(cardH_[0], rightH);
            const float blockW = ctlW + cardGap_ + colW;
            const float x0 = floorf(maxf(0.0f, (fbW - blockW) * 0.5f));
            const float y0 = floorf(maxf(0.0f, (fbH - blockH) * 0.5f));
            at[0] = ImVec2(x0, y0);
            at[1] = ImVec2(x0 + ctlW + cardGap_, y0);
            at[2] = ImVec2(at[1].x, y0 + cardH_[1] + cardGap_);
            // -- SETTLED, NOT MERELY NON-ZERO -------------------------
            //
            // AN AUTO-SIZED WINDOW REACHES ITS SIZE OVER SEVERAL FRAMES, not
            // one: ImGui lays out what it can, measures, and grows. Latching
            // the placement the moment all three heights were non-zero took
            // VISUALS at about a fifth of its final height and put GENERAL
            // straight through the middle of it -- captured, and it read as
            // two boxes on top of each other.
            //
            // A height that has not changed since last frame is one the
            // window has finished growing into. Three or four frames, then it
            // locks and the boxes are the player's to move.
            const bool steady = cardH_[0] > 0.0f && cardH_[1] > 0.0f && cardH_[2] > 0.0f &&
                                fabsf(cardH_[0] - cardHWas_[0]) < 0.5f &&
                                fabsf(cardH_[1] - cardHWas_[1]) < 0.5f &&
                                fabsf(cardH_[2] - cardHWas_[2]) < 0.5f;
            for (int i = 0; i < 3; ++i) cardHWas_[i] = cardH_[i];
            if (steady) menuPlaced_ = true;
        }

        // -- THE WORD OVER THE THREE BOXES -----------------------------
        //
        // (user 2026-09-20: "remove v2 from the text. remove the fps from the
        //  settings. move the settings text up 10 pc. double the font size.")
        //
        // THE FRAME RATE WENT BACK WHERE IT LIVES. It is on the HUD already
        // and it is not a setting; having it here as well was the last of the
        // readout that used to sit above the tabs, and the reason the rest of
        // that readout went applies to it too.
        //
        // A TENTH OF THE SCREEN, not a tenth of anything else -- "up 10 pc"
        // over a panel whose own height changes with its contents can only
        // mean the window, and the window is what the cards are placed
        // against. Clamped at the top edge so a short window cannot push the
        // word off it.
        //
        // NoInputs matters: an invisible pane over the boxes would eat the
        // drag that is the point of them being separate.
        {
            // -- TOP CENTRE OF THE SCREEN, NOT OF THE BOXES --------------
            //
            // (user 2026-09-20: "make the settings in the middle top center of
            //  the screen. put it above the boxes. move it 50px down.")
            //
            // IT USED TO HANG OFF THE FIRST BOX -- at[0].x, and a tenth of the
            // screen above it -- which put it wherever the left column
            // happened to start and moved it every time the key list changed
            // width. The screen is the thing it should be centred on, and the
            // boxes are centred on the screen too, so the two agree without
            // either measuring the other.
            //
            // -- ...AND DOWN ONTO THE BOXES ---------------------------
            //
            // (user 2026-09-20: "move the settings text downwards to the top
            //  of the settings boxes.")
            //
            // FIFTY PIXELS FROM THE TOP OF THE SCREEN WAS A LONG WAY FROM
            // WHAT IT NAMES. The boxes sit in the middle of the window, so a
            // heading pinned to the top edge had most of the sky between it
            // and the thing it is the heading for. It rides the boxes now:
            // its bottom sits just above whichever of the three is highest,
            // so it stays a heading even after they are dragged about.
            //
            // STILL CENTRED ON THE SCREEN, not on the boxes. The boxes are
            // centred on the screen too until somebody moves one, and a word
            // that slides sideways every time a column changes width is what
            // the last arrangement was wrong about.
            //
            // ONE FRAME BEHIND, like the heights it sits beside: the boxes
            // are drawn after this, so where they are is last frame's answer.
            // They only move when dragged, and a heading a frame behind a
            // drag is not something an eye can catch.
            //
            // MEASURED, because the word has to be centred on its own width.
            // Placed every frame rather than once: this window takes no input
            // and cannot be dragged, so there is nothing for a latch to
            // protect and a resized window should re-centre it.
            // -- NO "settings" HEADING (user 2026-09-21: "remove the settings
            //    title from the settings menu").
            //
            //    The three cards name themselves and the key that opened them
            //    is the word that was written over them, so it was furniture.
            //
            //    headH_ STAYS AT 0 AND THAT IS LOAD-BEARING: the card placer
            //    above reads it to decide how far under the heading the boxes
            //    sit, and its own guard is `headH_ > 0.0f`. Leaving the member
            //    at zero is what makes that branch mean "there is no heading"
            //    rather than "the heading has not been measured yet".
        }

        cardSkin skin;
        {
            // POSITIONED FROM INSIDE, NOT BEFORE. Falcor's pushWindow calls
            // SetNextWindowPos({0,0}) itself, and SetNextWindowPos is
            // "next window" state -- the second call replaces the first,
            // whatever its condition. So anything set out here is thrown away
            // and all three boxes open in the corner on top of each other,
            // which is exactly what the first capture showed.
            //
            // SetWindowPos acts on the CURRENT window and nothing overwrites
            // it. It is what the one-panel version did, for the same reason.
            ImGui::SetNextWindowSizeConstraints(ImVec2(ctlW, 0.0f),
                                                ImVec2(ctlW, float(fbH) * 0.96f));
            {
                // ONE PUSH NOW, not two: we drive Begin ourselves, so nothing
                // pushes another font over ours afterwards -- see cardWidgets.
                px3Font face(px3_);
                ImGui::Begin("controls##v2", nullptr, cardWinFlags());
                cardWidgets w(pGui);
                // ...and how tall it came out, for next frame's centring.
                cardH_[0] = ImGui::GetWindowSize().y;
                cardY_[0] = ImGui::GetWindowPos().y;
                if (place) ImGui::SetWindowPos(at[0]);
                ImGui::SetWindowFontScale(style.scale);
                ImGui::PushItemWidth(sliderW);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kText());
                // -- THE KEYS THEMSELVES, FIRST ---------------------------
                //
                // (user 2026-09-19: "list all the keybinds in controls in the
                //  settings".)
                //
                // ONE COPY, TWO READERS -- see ForestApp::keyHelpText, which
                // is the string F1 prints. A second list typed in here is a
                // list that goes stale, and [G] changing meaning this morning
                // is the proof: it would still say REFRESH.
                //
                // -- LISTED, NOT HIDDEN ---------------------------------
                //
                // (user 2026-09-20: "under controls list the keybinds.")
                //
                // IT WAS BEHIND A DISCLOSURE AND A SCROLLING CHILD, on the
                // argument that forty lines of keys above the sliders is a
                // wall for a returning player. That argument was about a
                // ONE-COLUMN panel where the keys pushed everything else off
                // the bottom. Controls is its own window now, so the wall has
                // nothing behind it to bury -- and a key list you have to
                // click to see is a key list nobody reads.
                //
                // THE BRIEF LIST, NOT THE PROSE ONE -- see keyBriefText. F1
                // and the console still print the long form, where the
                // explanations earn their place; a panel read at a glance
                // wants "caps lock = crouch" and nothing after it.
                //
                // The monospace font is not decoration: the string is laid out
                // in columns and the proportional face closes them up.
                {
                    if (px3_) ImGui::PushFont(px3_);
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                    ImGui::TextUnformatted(keyBriefText());
                    ImGui::PopStyleColor();
                    if (px3_) ImGui::PopFont();
                    cardRule();
                }
            hoverSlider(w, "Walk speed", player_.walk, 0.2f, 200.0f);
            // No invalidate: it changes nothing that has already been traced, only
            // how far the next mouse movement will turn the view -- exactly like
            // exposure and walk speed above it.
            hoverSlider(w, "Sensitivity", opt_.sensitivity, 0.02f, 0.50f, false, "%.3f deg/px");
            // AND NOTHING ELSE ABOUT THE MOUSE. There were two more rows here --
            // acceleration and weight -- and they are gone at the user's word; the
            // note in Options says why they were removed rather than zeroed.
            // Sensitivity is now the whole of it, which is what a raw mouse means.
            if (hoverSlider(w, "Field of view", fov_, 10.0f, 100.0f)) invalidate();

            // ---- THE THING IN YOUR HAND ---------------------------------------
            //
            // SEVEN SLIDERS, AND THEY ARE THE POINT OF THIS SECTION. The pose came
            // over from the JS engine's held-item panel, and it came over because
            // that panel existed: nobody arrives at { 0.91, -0.10, 0.96, 0.04,
            // -1.42, 1.58 } by reasoning about it. A viewmodel is judged by eye and
            // adjusted by hand, and the two conversions between that engine and
            // this one (see render/helditem.h) mean the bake is a starting point
            // here rather than a finished answer.
            //
            // THERE IS NO LIGHTING ROW, and that is the feature. The tool is traced
            // at the primary vertex off the world's own materials, so how bright it
            // is has exactly one answer and it is the same answer the wood gets --
            // there is nothing here to tune, and nothing that can drift out of step
            // with the frame behind it. An earlier cut of this composited a
            // separately lit axe over the finished image and needed three rows to
            // make it agree with the picture; it never quite did.
            //
            // THESE SEVEN DO INVALIDATE, through animating(): moving the tool makes
            // every sample already in the film describe a tool that is somewhere
            // else.
            //
            // The values are the JS engine's units, voxels and radians, so a row
            // read here can be pasted straight back into that engine's PICK_DEFS
            // and vice versa. See the note on HeldPose.
            // -- THE POSE ROWS MOVED TO [K] (user 2026-09-17) -------------------
            //
            // "put the hand held item adjustments and the aim down site adjustments
            // all on the k keybind with the stack number positionings."
            //
            // They were seven sliders in the middle of a settings MENU, and a
            // viewmodel is not a setting: it is a thing you move while looking at
            // it, which means the menu must not be holding the mouse. [K] draws
            // them as a card down the right edge with the sights and the stack
            // badge under it -- see the block in drawUi. What is left here is the
            // pointer, because a row that used to be in a menu and is now nowhere
            // is worse than either.
            if (held_.ready()) {
                cardRule();
                w.checkbox(fmt("%s in hand  (H)", held_.name()).c_str(), held_.shown);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted("  K -- pose, sights and stack badge");
                ImGui::PopStyleColor();
            }
                ImGui::PopStyleColor();
                ImGui::PopItemWidth();
                ImGui::End();
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(colW, 0.0f),
                                                ImVec2(colW, float(fbH) * 0.96f));
            {
                // ONE PUSH NOW, not two: we drive Begin ourselves, so nothing
                // pushes another font over ours afterwards -- see cardWidgets.
                px3Font face(px3_);
                ImGui::Begin("visuals##v2", nullptr, cardWinFlags());
                cardWidgets w(pGui);
                // ...and how tall it came out, for next frame's centring.
                cardH_[1] = ImGui::GetWindowSize().y;
                cardY_[1] = ImGui::GetWindowPos().y;
                if (place) ImGui::SetWindowPos(at[1]);
                ImGui::SetWindowFontScale(style.scale);
                ImGui::PushItemWidth(sliderW);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kText());
            // ---- the compass ----------------------------------------------
            //
            // (user 2026-09-21: "let it be toggleable in the settings under
            //  visuals".) FIRST IN THE CARD, because it is the one row here
            // that changes what is ON the screen rather than how the screen
            // is RENDERED -- everything below it is the renderer.
            w.checkbox("Compass", opt_.compass);
            // ---- the denoiser ---------------------------------------------------
            //
            // NO ON/OFF ROW. Ray Reconstruction is always on -- at one sample a
            // pixel it is not an enhancement, it is the thing that makes the image
            // an image, and offering to switch it off is offering to break the
            // renderer. --no-dlss still exists for a reference render, which is the
            // only context where the accumulating film is the right answer.
            //
            // THREE MODES, NOT FIVE. Ultra performance and Performance are not
            // offered: below Balanced, Ray Reconstruction is upscaling from so few
            // pixels that a conifer canopy -- thin, high-frequency geometry with
            // bright sky behind it -- comes back as mush that no amount of
            // denoising recovers. The ENUM still has them and --dlss names them, so
            // a benchmark keeps a capability the in-game menu does not offer; the
            // same split --scale got.
            if (dlss_.available() && opt_.dlss) {
                Falcor::Gui::DropdownList modes = {
                    {uint32_t(DlssQuality::Balanced), "Balanced"},
                    {uint32_t(DlssQuality::Quality), "Quality"},
                    {uint32_t(DlssQuality::Dlaa), "DLAA (no upscale)"},
                };

                // AND IF WE ARE IN A MODE THE LIST DOES NOT OFFER, SHOW IT ANYWAY.
                // This is not tidiness. Falcor's addDropdown scans the list for the
                // live value and leaves its index at -1 when it is not found, then
                // unconditionally does `values[curItem]` (Gui.cpp:668) -- so a value
                // outside the list is not a blank combo, it is an out-of-bounds read
                // on a std::vector. Reachable from the command line today with
                // `--dlss performance`.
                const uint32_t live = uint32_t(opt_.dlssQuality);
                bool listed = false;
                for (const auto &e : modes) listed = listed || e.value == live;
                if (!listed)
                    modes.insert(modes.begin(), {live, dlssQualityName(opt_.dlssQuality)});

                uint32_t m = live;
                if (w.dropdown("Mode", modes, m)) {
                    opt_.dlssQuality = DlssQuality(m);
                    tracer_.setQuality(opt_.dlssQuality);
                }
            } else if (dlss_.available()) {
                // --no-dlss: the film is accumulating instead, and a reconstruction
                // mode is not a thing that has a meaning here.
                w.text("Ray Reconstruction off (--no-dlss) -- accumulating");
            } else {
                w.text(fmt("DLSS unavailable: %s", dlss_.status().c_str()));
            }

            // FRAME GENERATION IS NOT IN THIS MENU. It is on at 2x and stays there;
            // --fg 2x|3x|4x|off still works and the capability probes in
            // streamline.h still run, so a card that cannot do it still declines
            // quietly. Its diagnostics -- what DLSS-G says about itself, the
            // generated-frame counts, whether the device reached Streamline before
            // the swapchain -- are printed at start-up and under --stats, which is
            // where a diagnostic belongs.
            cardRule();

            // -- AND NOTHING ELSE IN THIS CARD (user 2026-09-21: "remove all of
            //    the visual settings except the mode and compass").
            //
            //    NINE ROWS WENT, NOT NINE FEATURES -- the same trade the
            //    volumetric fog rows made a day earlier, and for the same
            //    reason. Gone from the PANEL: the neural radiance cache and its
            //    four training knobs, exposure, both shadow lifts, the sun
            //    glare, the vignette, fog density, fog height and the spark
            //    glow. Every one of them is still a launch flag and still bakes
            //    from defaults.h, so nothing has become unreachable -- it has
            //    stopped being in a menu.
            //
            //    WHAT IS LEFT IS WHAT A PLAYER CHANGES: whether the compass is
            //    on the screen, and which DLSS mode the image is reconstructed
            //    at. The rest was tuning, and tuning is work that finishes.
                ImGui::PopStyleColor();
                ImGui::PopItemWidth();
                ImGui::End();
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(colW, 0.0f),
                                                ImVec2(colW, float(fbH) * 0.96f));
            {
                // ONE PUSH NOW, not two: we drive Begin ourselves, so nothing
                // pushes another font over ours afterwards -- see cardWidgets.
                px3Font face(px3_);
                ImGui::Begin("general##v2", nullptr, cardWinFlags());
                cardWidgets w(pGui);
                // ...and how tall it came out, for next frame's centring.
                cardH_[2] = ImGui::GetWindowSize().y;
                cardY_[2] = ImGui::GetWindowPos().y;
                if (place) ImGui::SetWindowPos(at[2]);
                ImGui::SetWindowFontScale(style.scale);
                ImGui::PushItemWidth(sliderW);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kText());
            // -- WHERE YOU ARE --------------------------------------------
            //
            // (user 2026-09-19: "give me a toggle for coords. x, y, and z
            //  coords.")
            //
            // IN GENERAL RATHER THAN VISUALS, and the line the tabs are drawn
            // on is what decides it: visuals is what the PICTURE looks like and
            // this changes nothing about the picture -- it is a readout, like
            // the frame rate and the hardware line further down.
            //
            // THE KEY IS ON THE LABEL. Every other toggle in this panel that
            // has one says so in its own text (the hand item's "(H)"), because
            // a checkbox is how you find a feature and a key is how you use it
            // afterwards.
            // -- NO SNOW ROW, AND THE SNOW IS STILL THERE ----------------
            //
            // (user 2026-09-20: "just remove the snow for now. keep the code
            //  in place" / "also remove the snow selection from the settings
            //  menu".)
            //
            // THE FEATURE IS NOT DELETED, THE DOOR IS. Everything is where it
            // was -- flakeNearest and the flake lattice in Trace.cs.slang,
            // snowTime/snowFall/snowStep in V6Params, VoxelTerrain::snowLay
            // and the accumulation steps in onFrameRender. What has gone is
            // the one checkbox that let a player switch it on, because it
            // still flickers and half-finished weather is worse than none.
            //
            // `--snow` STILL WORKS, deliberately: that is how the next round
            // of this gets looked at without unpicking anything. VoxelTerrain
            // ::snowOn defaults false, so a player never meets it.
            //
            // TO PUT IT BACK: three lines, a w.checkbox on snowOn plus the
            // opt_ mirror. Nothing else has to be rebuilt or rewired.
            w.checkbox("Show coordinates  (F3)", showCoords_);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            ImGui::TextUnformatted("  metres, under the frame rate -- x y z at your feet");
            ImGui::PopStyleColor();
            cardRule();

            // No invalidate here either, and for a stronger reason than the two
            // above: this one changes nothing the renderer can even see. Hidden
            // rather than greyed when there is no voice -- under --no-sound or on
            // a machine with no endpoint, a slider that does nothing is worse
            // than no slider.
            // CALLED VOLUME, BECAUSE THAT IS WHAT SOMEBODY LOOKS FOR. It was
            // "Ambience", which is accurate -- the bed is the only sound the engine
            // makes -- and accurate is not the same as findable.
            //
            // AND A REASON WHEN IT IS MISSING, which reverses the old rule here.
            // Hiding a dead slider is right; hiding it without explanation sends
            // somebody hunting through a menu for a row that was never going to be
            // drawn. A line of text is not a control that does nothing, it is the
            // answer to the question the missing control provokes.
            //
            // THE DEFAULT IS THE MIDDLE OF THE TRACK. This ran to 2.0, and the
            // bed is a background: everything anybody would actually choose
            // lived in the first eighth of the travel, with the whole right-hand
            // half reserved for twice the level the asset was baked at. So the
            // top is now twice the default instead of eight times it, which puts
            // the handle you start with in the centre and spends the travel on
            // the range the ear is actually being asked about.
            //
            // The command line is unchanged and still reaches the baked level:
            // --ambience 1.0, or 2.0, is a number rather than a drag.
            if (ambience_.active()) {
                float amb = ambience_.masterGain();
                // The floor is what stops a bake at zero from welding the control
                // shut: a slider whose top is its bottom can never be dragged back
                // up, and the volume is the one setting somebody is most likely to
                // take all the way down before baking.
                const float top = maxf(0.05f, defaults::kAmbience * 2.0f);
                if (hoverSlider(w, "Volume", amb, 0.0f, top, false, "%.2f"))
                    ambience_.setMasterGain(amb);
            }
            // ITS OWN SLIDER, which is the JS engine's split: the bed and the
            // things the world does are two buses there, because a wood that is too
            // loud and an axe that is too loud are different complaints.
            // NO "Tools" BUS ROW (user 2026-09-21). toolSfx_ keeps its own gain
            // and its default; there is just no slider for it in the menu.
            if (!ambience_.active()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted(opt_.background
                                           ? "Volume: no audio under --background"
                                           : "Volume: no audio (--no-sound, or no endpoint)");
                ImGui::PopStyleColor();
            }


            // ---- WHICH SKY IS NOT A QUESTION THIS MENU ASKS ---------------------
            //
            // Atmospheric scattering is ON and stays on. The Preetham fit it
            // replaced survives only as a fallback for a machine where the LUT
            // shaders will not compile, and as --no-atmosphere for anyone comparing
            // against an image taken before 2026-09-06. Neither is worth a row, and
            // the wrong answer to it silently freezes every sunset.
            //
            // THE NIGHT FLOOR IS BACK, as half of the row below rather than as a
            // row of its own. It left with the sky group and lived on --night-floor
            // alone, which put the answer to "the night is too dark" behind a
            // relaunch. It stands in for airglow and starlight: the model knows
            // about sunlight and nothing else, so at 0 a deep night is honestly --
            // and uselessly -- black.
            //
            // AND NOTE THE TURBIDITY ROW BELOW. It is a PREETHAM parameter, and the
            // scattering path carries its own fixed aerosol profile and ignores it,
            // so with the atmosphere always on that slider moves nothing anybody
            // can see. It is left alone because --no-atmosphere still reads it.
            // ONE ROW FOR THE WHOLE NIGHT. It drives the moon's key light and the
            // airglow floor together -- Options::nightBrightness says why it has to
            // be both -- so 0.5x is a night half as bright at every phase of the
            // moon, rather than only on the ones where the term it happened to move
            // was the one doing the lighting. 0 is the physically honest black; 3
            // leaves the wood readable at midnight, which is usually what a
            // screenshot at that hour actually wants.
            //
            // LEFT OF 1.0 IS DARKER, which is the direction this gets reached for,
            // and the row is still named for brightness: every other slider in this
            // menu moves right for more of what it names, and one that ran
            // backwards would be wrong more often than it was clever.
            // NO NIGHT BRIGHTNESS OR SKY TURBIDITY ROW (user 2026-09-21).
            // --night and --turbidity still drive both, and both still bake.
            // NO DEPTH-OF-FIELD ROW HERE, and this time the reason is measured
            // rather than assumed. There was a slider, and the lens it drove was
            // correct -- the result still looked wrong. Ray Reconstruction wants
            // every sample in a pixel to share an origin, and a lens is precisely
            // the thing that stops them doing so; what came out was not shallow
            // focus but a smear the denoiser could not resolve. render/camera.h
            // said as much before any of it was tried.
            //
            // --aperture and --focus still drive the same lens OFFLINE, where the
            // film accumulates and there is no denoiser in the way. That is where
            // it earns its keep, and it is the only place it ever did.
            cardRule();

            float hours = clock_.tday * 24.0f;
            if (hoverSlider(w, "Time of day", hours, 0.0f, 24.0f)) {
                clock_.tday = clampf(hours / 24.0f, 0.0f, 0.99999f);
                invalidate();
            }
            char speed[24];
            clock_.speedLabel(speed, sizeof(speed));
            w.text(fmt("Cycle speed  %s   (X + wheel, or:)", speed));
            if (w.button("slower")) clock_.nudgeSpeed(false);
            if (w.button("faster", true)) clock_.nudgeSpeed(true);
            if (w.button(clock_.paused ? "resume" : "pause", true)) clock_.paused = !clock_.paused;
            cardRule();


            // -- NO HARDWARE READOUT (user 2026-09-21: "remove cluster, neural,
            //    cuda text from settings"). Three lines that named capabilities
            //    of the device rather than anything to decide -- true, and not
            //    a setting. --stats still prints all three.


            // A bake writes SOURCE, not a config file, deliberately. A config read
            // at startup would be one more thing that can be stale, missing, or
            // disagree with the flags; a header means the defaults are visible in
            // the diff, travel with the branch, and cost nothing at runtime.
            // THE ONE RED THING IN THE PANEL. This rewrites the defaults every
            // later run starts from -- the only row here that changes anything
            // outside the session -- and red is what this game's screen does
            // when something is happening to you. Spending it here and nowhere
            // else is what keeps it meaning that. See ui::kRed.
            // GOLD, NOT RED (user 2026-09-21). It was red because it is the one
            // row that rewrites source rather than state -- but red in this
            // panel now reads as the quit ball, and gold is the colour every
            // other thing that ACTS in this engine already wears: the compass,
            // the watermark, the download button on the site. The three alphas
            // are unchanged, so it is the same button at the same weights.
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kGold(0.28f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kGold(0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kGold(0.85f));
            const bool bake = w.button("Bake as default");
            ImGui::PopStyleColor(3);
            if (bake) bakeStatus_ = bakeDefaults();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            ImGui::TextUnformatted(bakeStatus_.empty()
                                       ? "writes src/core/defaults.h; then rebuild.bat, in v2/"
                                       : bakeStatus_.c_str());
            ImGui::PopStyleColor();
                ImGui::PopStyleColor();
                ImGui::PopItemWidth();
                ImGui::End();
            }
        }
        // -- NO CLOSING LINE, AND NO RULE UNDER THE BOXES --------------
        //
        // (user 2026-09-20: "remove the line at the bottom of the boxes and
        //  the text.")
        //
        // It read "Y or ESC close   F1 controls, in the console" over a rule
        // spanning the panel. It was written when there WAS a panel for it to
        // span and a tab bar hiding two thirds of the settings from anyone who
        // did not know to look; neither is true now. Both keys still work.
    }

    #include "platform/app_input.inl"
