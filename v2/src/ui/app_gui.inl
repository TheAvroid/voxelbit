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
            sw.text(sel >= 0 && sel == rifleTool_ ? "AMMO COUNT" : "STACK COUNT");
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
        {
            const float inset = 12.0f;  // the HUD's, so the two corners agree
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
        if (savedTake_.valid() && nowSeconds() - savedAt_ >= kSavedNotice)
            savedTake_ = vb::Take{};
        if (recorder_.recording() || recorder_.busy() || savedTake_.valid()) {
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
            } else {
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
            ImGui::SetWindowFontScale(style.scale);

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
        const Gui::WindowFlags kPanel = Gui::WindowFlags::AllowMove | Gui::WindowFlags::NoResize;
        Gui::Window w(pGui, "settings##v2", menuOpen_, {0, 0}, {0, 0}, kPanel);
        px3Font face(px3_);
        ImGui::SetWindowFontScale(style.scale);

        // WIDTH IN CHARACTERS, HEIGHT FROM THE CONTENT -- v2's rule exactly.
        // Seventy columns is the longest line the panel can hold, and fixing
        // the width is not only tidiness: right-aligning anything inside a
        // window that is auto-sizing to its own content is a feedback loop, and
        // the window grows a little wider every frame.
        const float cw = ImGui::CalcTextSize("0").x;
        // THE PANEL GREW WITH THE SLIDERS. Seventy columns was the width of the
        // longest LINE OF TEXT, which was the right rule while the sliders were
        // short. Three times the slider needs somewhere to put it, so the panel
        // is sized from the widget instead: the trough, the gap, and the label
        // beside it.
        const float sliderW = 3.0f * 12.0f * cw;  // was about twelve columns
        const float panelW = sliderW + 26.0f * cw + 32.0f;
        // Eighty-five percent of the window, so there is always visibly
        // something behind the panel -- a settings screen that covers the thing
        // being adjusted is a settings screen you have to close to judge.
        const float panelH = floorf(fbH * 0.85f);
        ImGui::SetWindowSize(ImVec2(panelW, panelH));

        // CENTRED ON OPEN, THEN YOURS. v2 centred it every frame, and its
        // reasoning for the position was right -- the middle of the screen is
        // where the eyes already are, unlike the top-left corner it used to sit
        // in. But setting the position EVERY frame is also what made the panel
        // immovable: ImGui moves a dragged window and the next frame put it
        // straight back, so it did not so much refuse to move as twitch.
        //
        // So the placement happens once, when the menu opens, and after that
        // the window is ImGui's to drag.
        //
        // AND WHERE IT OPENS IS WHERE IT WAS LEFT. It used to re-centre on
        // every opening, on the argument that a panel dragged somewhere
        // awkward is never lost. That is true and it is still the wrong
        // default: someone who moves a panel out of the way of the thing they
        // are tuning means it, and having it jump back to the middle of the
        // screen every time undoes the move as fast as they can make it.
        // Centring is now only what happens the FIRST time, before there is a
        // remembered place to prefer.
        //
        // Clamped to the framebuffer on the way back out, because the window
        // can be resized between one opening and the next -- a panel remembered
        // at x = 3000 on a wide monitor must not be off the edge of a narrow
        // one, which would leave it genuinely lost with no way to drag it back.
        if (!menuPlaced_) {
            if (menuPos_.x < 0.0f) {  // never opened: the middle of the screen
                menuPos_ = ImVec2(floorf(maxf(0.0f, (fbW - panelW) * 0.5f)),
                                  floorf(maxf(0.0f, (fbH - panelH) * 0.5f)));
            }
            const float mx = maxf(0.0f, fbW - panelW);
            const float my = maxf(0.0f, fbH - 40.0f);  // keep the title grabbable
            ImGui::SetWindowPos(ImVec2(clampf(menuPos_.x, 0.0f, mx),
                                       clampf(menuPos_.y, 0.0f, my)));
            menuPlaced_ = true;
        } else {
            // Read back every frame rather than on close: ImGui does not tell
            // us when a drag ends, and the menu can be shut by ESC, by Y or by
            // its own close box, so there is no one place a "save on close"
            // hook could live without one of the three missing it.
            menuPos_ = ImGui::GetWindowPos();
        }

        // EVERY WIDGET IN THE PANEL, one width. ImGui's default is a fraction of
        // the window, which would have made the sliders grow with the panel and
        // the panel grow with the sliders -- so it is stated once, explicitly,
        // and popped at the end of the function.
        // Falcor draws slider troughs at a width of its own, so asking ImGui is
        // not enough -- see Gui::setSliderWidth, added to v2's fork for this.
        Gui::setSliderWidth(sliderW);
        ImGui::PushItemWidth(sliderW);

        // The title line: the name on the left, and on the right the number
        // that everything below is a trade against. One line, both ends.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTitle());
        ImGui::TextUnformatted("v2  settings");
        {
            const std::string f = fmt("%.0f fps", fps_ + genFps_);
            ImGui::SameLine();
            ImGui::SetCursorPosX(panelW - ImGui::CalcTextSize(f.c_str()).x -
                                 ImGui::GetStyle().WindowPadding.x);
            ImGui::TextUnformatted(f.c_str());
        }
        ImGui::PopStyleColor();

        // What the frame is actually made of, in the colour of things you can
        // only read. v2 put the same block here for the same reason: the rows
        // below are a trade, and a trade needs a price on screen.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted(
            fmt("%d bounces   film %s", liveDepth_,
                liveMaxAccum_ ? fmt("%u samples", liveMaxAccum_).c_str() : "unlimited")
                .c_str());
        ImGui::TextUnformatted(fmt("%d x %d traced -> %d x %d shown in %d x %d",
                                   tracer_.width(), tracer_.height(), tracer_.outWidth(),
                                   tracer_.outHeight(), int(getTargetFbo()->getWidth()),
                                   int(getTargetFbo()->getHeight()))
                                   .c_str());
        ImGui::TextUnformatted(fmt("%zu chunks   %.1f M tris   %zu instances",
                                   world_.chunkCount(), world_.residentTris() / 1e6,
                                   world_.instanceCount())
                                   .c_str());
        if (flock_.ready())
            ImGui::TextUnformatted(fmt("%d butterflies   %d colours", flock_.flying(),
                                       flock_.colourCount())
                                       .c_str());
        ImGui::PopStyleColor();
        w.separator();

        // ── NO PRESETS AND NO RESOLUTION ROW ──────────────────────────────
        //
        // Removed at the user's asking, along with the five preset buttons
        // (Max FPS / Fast / Balanced / Sharp / Native) that set the same two
        // numbers between them. The engine renders at 100 % of the window and
        // that is now simply what it does -- defaults::kScale is 1.00 and
        // nothing in the menu moves it.
        //
        // --scale still exists and still works, because an offline render or
        // a benchmark has a real reason to ask for a different size and no
        // menu to ask it through. What is gone is the in-game control, not
        // the capability behind it.

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
        w.separator();

        // ---- the neural radiance cache --------------------------------------
        //
        // The batch count is the honest indicator and it is why it is on screen:
        // "on" tells you what was asked for, and the number climbing tells you
        // the network is actually being fed. A cache that is enabled but whose
        // count is stuck is a cache that is doing nothing, and without this row
        // that looks exactly like one that is working.
        if (nrc_.available()) {
            if (w.checkbox("Neural radiance cache", opt_.nrc)) {
                nrc_.enabled = opt_.nrc;
                invalidate();
            }
            if (opt_.nrc) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
                ImGui::TextUnformatted(
                    fmt("   %s   %u batches", nrc_.warm() ? "predicting" : "warming up",
                        nrc_.batches())
                        .c_str());
                ImGui::PopStyleColor();
                w.checkbox("  keep learning", nrc_.training);
                if (w.slider("  from bounce", nrc_.queryDepth, 1, 6)) invalidate();
                w.slider("  train 1 px in", nrc_.trainEvery, 8, 512);
                w.slider("  learning rate", nrc_.learningRate, 0.0005f, 0.05f, false, "%.4f");
                // fp16 training does diverge, and when it does every query is a
                // NaN that the film will happily accumulate. There is no
                // recovering a poisoned network, so the only cure is on offer.
                if (w.button("  retrain from scratch")) nrc_.reset();
            }
        } else if (neural_.available()) {
            w.text(fmt("Neural radiance cache: %s", nrc_.status().c_str()));
        }
        w.separator();

        // ---- what the hardware is doing under all of this --------------------
        //
        // Read-only where there is nothing to decide. Clusters and cooperative
        // vectors are capabilities of this device and this backend, not
        // preferences, and a checkbox for something the driver has already
        // refused is a checkbox that lies.
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted(fmt("clusters   %s", clusters_.available() ? "available"
                                                                          : "not on this backend")
                                   .c_str());
        ImGui::TextUnformatted(
            fmt("neural     %s", neural_.available() ? "cooperative vectors" : "unavailable")
                .c_str());
        ImGui::TextUnformatted(fmt("cuda       %s", cuda_.available() ? "shared with the renderer"
                                                                     : "unavailable")
                                   .c_str());
        ImGui::PopStyleColor();
        w.separator();

        // Exposure changes no sample already drawn, so it deliberately does NOT
        // throw the accumulation away. Nor does the toe -- both are the curve
        // between the film and the screen, not the film.
        w.slider("Exposure", opt_.r.exposure, 0.05f, 40.0f);
        // THE KNOB FOR "SHADOWS ARE TOO DARK", and the range matters. This is
        // the ACES toe: the curve's slope near black is b/0.14, so the 0.14
        // default is a slope of exactly 1.0 -- linear, no crush. Below that the
        // curve is eating shadow detail; above it is deliberately lifting.
        //
        // It used to stop at 0.20 and that was too low to be the answer to
        // anything. 0.35 reaches a slope of 2.5, and the shoulder does not move
        // with it: b appears only in the linear term, so highlights roll off
        // identically at every setting on this slider.
        w.slider("Shadow lift", opt_.r.shadowLift, 0.03f, 0.35f, false, "%.3f toe");

        // THE ONE THAT ONLY TOUCHES THE DARK. The toe above is a parameter of
        // the tone curve and so acts on the whole frame; this falls to exactly
        // zero at the reach below it, which is why the midtones do not move.
        // Reach for this one when the undersides of the canopy are too dark and
        // the rest of the frame is right, which is the usual case in a wood.
        w.slider("Deep shadow lift", opt_.r.deepLift, 0.0f, 0.15f, false, "%.3f");
        // How far the corners fall off. 0 is off, which is where it starts --
        // the tone map runs it last, after the flare, so it darkens the
        // finished image rather than having the ghosts scatter back over it.
        // THE SUN GLARE HAD NO CONTROL AT ALL, which is how "I can see the sun
        // through the tree" ended up with no way to answer it from inside the
        // game. It is a look, and every other look in this menu is a slider.
        // 0 removes the glare and the ghosts entirely and costs nothing else --
        // the sun disc itself is drawn by the sky, not by this.
        // RANGE RAISED WITH THE DEFAULT. 2.0 was the ceiling and is now where
        // the slider starts, which would have made it a knob that only turns
        // down. 4.0 keeps as much headroom above the default as there is below.
        w.slider("Sun glare", tracer_.flare, 0.0f, 4.0f, false, "%.2f");
        w.slider("Vignette", tracer_.vignette, 0.0f, 1.0f, false, "%.2f");

        // AUTO-EXPOSURE, BLOOM AND THE DEEP-LIFT'S REACH ARE NOT IN THIS MENU,
        // deliberately. All three are reachable from the command line
        // (--auto-exposure, --exposure-key, --bloom, --bloom-threshold,
        // --deep-range) and the first three bake, so nothing about them is
        // gone -- they are simply not worth the rows they cost here.
        w.slider("Walk speed", player_.walk, 0.2f, 200.0f);
        // No invalidate: it changes nothing that has already been traced, only
        // how far the next mouse movement will turn the view -- exactly like
        // exposure and walk speed above it.
        w.slider("Sensitivity", opt_.sensitivity, 0.02f, 0.50f, false, "%.3f deg/px");
        // AND NOTHING ELSE ABOUT THE MOUSE. There were two more rows here --
        // acceleration and weight -- and they are gone at the user's word; the
        // note in Options says why they were removed rather than zeroed.
        // Sensitivity is now the whole of it, which is what a raw mouse means.
        if (w.slider("Field of view", fov_, 10.0f, 100.0f)) invalidate();
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
            if (w.slider("Volume", amb, 0.0f, top, false, "%.2f"))
                ambience_.setMasterGain(amb);
        }
        // ITS OWN SLIDER, which is the JS engine's split: the bed and the
        // things the world does are two buses there, because a wood that is too
        // loud and an axe that is too loud are different complaints.
        if (toolSfx_.ready()) {
            float s = toolSfx_.gain();
            if (w.slider("Tools", s, 0.0f, 2.0f, false, "%.2f")) toolSfx_.setGain(s);
        }
        if (!ambience_.active()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            ImGui::TextUnformatted(opt_.background
                                       ? "Volume: no audio under --background"
                                       : "Volume: no audio (--no-sound, or no endpoint)");
            ImGui::PopStyleColor();
        }

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
            w.separator();
            w.checkbox(fmt("%s in hand  (H)", held_.name()).c_str(), held_.shown);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
            ImGui::TextUnformatted("  K -- pose, sights and stack badge");
            ImGui::PopStyleColor();
        }
        w.separator();

        // ---- the air, and the lens ------------------------------------------
        //
        // THE CAP IS THE TOP OF THE USEFUL BAND, NOT THE TOP OF WHAT THE PASS
        // WILL DRAW. The froxel grid goes on working far past this -- the cap
        // used to be 0.10, five times what the analytic fog could offer before it
        // washed out to grey -- but nothing up there is a look anyone reaches for,
        // and a trough that wide is not adjustable. The default is 0.0022, so 0.10
        // spent its first 2% on every value worth having and the rest on soup.
        // 0.0040 puts the default a little past halfway and makes the whole travel
        // mean something.
        //
        // Five decimals rather than four for the same reason: %.4f reads out in
        // steps of 0.0001, which over this range is forty of them end to end.
        if (w.slider("Fog density", opt_.r.fogDensity, 0.0f, 0.0040f, false, "%.5f /m"))
            invalidate();
        if (w.slider("Fog height", opt_.r.fogHeight, 1.0f, 200.0f, false, "%.0f m"))
            invalidate();

        // -- THE GLOW ROUND A SPARK, AND ROUND A LAMP ----------------------
        //
        // (user 2026-09-17: "can you give me a slider to adjust the intensity
        //  of the voluemtric light coming from the spark voxel? put it on the
        //  y settings toggle.")
        //
        // IN THE FOG BLOCK RATHER THAN A NEW ONE, because that is what it is:
        // light scattered by air. It is deliberately NOT under the volumetric
        // fog checkbox below -- pointLightGlow is its own medium and its own
        // integral precisely so that it still works where the fog volume does
        // not (indoors, and in air too clear to march), so hiding it there
        // would put the control behind a switch that does not govern it.
        //
        // 0 TO 4, with 1 where it shipped. Past about 3 a pendant starts to
        // haze the room it is in rather than ring itself, which is a look and
        // not a fault -- the top of the range is there to make it reachable.
        if (w.slider("Spark / lamp glow", opt_.r.sparkGlow, 0.0f, 4.0f, false, "%.2fx"))
            invalidate();

        if (volfog_.available()) {
            // Unchecking this leaves NO fog at all, not the old analytic
            // model -- that one is gone. It is here to measure what the two
            // passes cost, and to see the wood without any air in it.
            if (w.checkbox("Volumetric fog (all fog)", volfog_.enabled)) {
                volfog_.invalidate();
                invalidate();
            }
            if (volfog_.enabled) {
                // Forward scattering. This is the knob that decides whether the
                // air near the sun GLOWS or whether the whole volume simply
                // lifts: at 0 the phase is a sphere and the fog is milk, and by
                // 0.9 nearly all the scattered light goes on in the direction it
                // was already travelling, which is what makes a beam a beam.
                if (w.slider("  forward scatter", volfog_.anisotropy, 0.0f, 0.95f, false,
                             "%.2f g")) {
                    volfog_.invalidate();
                    invalidate();
                }

                // The one honestly fudged number in the system -- the sky dome
                // is not traced per froxel, see the note in VolFogInject. Drop
                // it to zero and shadowed air goes black, which is a stronger
                // effect than it sounds and worth seeing once.
                if (w.slider("  sky fill", volfog_.ambient, 0.0f, 1.0f, false, "%.2f")) {
                    volfog_.invalidate();
                    invalidate();
                }

                // How much sky light reaches air the up-ray found under
                // canopy. 1.00 is the old unshadowed behaviour and brings the
                // sun blur back with it; 0 puts black holes under the trees.
                if (w.slider("  sky under canopy", volfog_.skyShadow, 0.0f, 1.0f, false,
                             "%.2f")) {
                    volfog_.invalidate();
                    invalidate();
                }

                // How far the 64 slices are stretched. Short and the haze stops
                // dead at a visible wall; long and every slice is spent on air
                // too distant to resolve, so the beams in the first ten metres
                // coarsen. 400 m is about where the wood stops being legible.
                if (w.slider("  march reaches", volfog_.farD, 50.0f, 1200.0f, false, "%.0f m")) {
                    volfog_.invalidate();
                    invalidate();
                }
                w.slider("  settle (still)", volfog_.settleStill, 0.02f, 1.0f, false, "%.2f");
                w.slider("  settle (moving)", volfog_.settleMoving, 0.05f, 1.0f, false, "%.2f");
                w.text("  160x90x64 froxels, one shadow ray each");
            }
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
        if (w.slider("Night brightness", nightLevel_, 0.0f, 3.0f, false, "%.2fx"))
            applyNightLevel();
        if (w.slider("Sky turbidity", opt_.turbidity, 1.8f, 8.0f)) {
            applySun(true);
            invalidate();
        }
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
        w.separator();

        float hours = clock_.tday * 24.0f;
        if (w.slider("Time of day", hours, 0.0f, 24.0f)) {
            clock_.tday = clampf(hours / 24.0f, 0.0f, 0.99999f);
            invalidate();
        }
        char speed[24];
        clock_.speedLabel(speed, sizeof(speed));
        w.text(fmt("Cycle speed  %s   (X + wheel, or:)", speed));
        if (w.button("slower")) clock_.nudgeSpeed(false);
        if (w.button("faster", true)) clock_.nudgeSpeed(true);
        if (w.button(clock_.paused ? "resume" : "pause", true)) clock_.paused = !clock_.paused;
        w.separator();

        // A bake writes SOURCE, not a config file, deliberately. A config read
        // at startup would be one more thing that can be stale, missing, or
        // disagree with the flags; a header means the defaults are visible in
        // the diff, travel with the branch, and cost nothing at runtime.
        if (w.button("Bake as default")) bakeStatus_ = bakeDefaults();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kNote());
        ImGui::TextUnformatted(bakeStatus_.empty()
                                   ? "writes src/core/defaults.h; then rebuild.bat, in v2/"
                                   : bakeStatus_.c_str());
        w.separator();
        // v2 closed with the keys, because a panel that has to be discovered
        // twice is a panel nobody finds the second thing in.
        ImGui::TextUnformatted("Y or ESC  close        F1  controls, in the console");
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();
    }

    #include "platform/app_input.inl"
