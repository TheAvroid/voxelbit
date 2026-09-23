// app_hud.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: fonts, crosshair, hitch/profile/help printing
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // v2's look, pushed for the life of one panel and popped after it.
    //
    // A guard object rather than a pair of calls, because ImGui's style stack
    // is strictly balanced and an early return between a push and its pop
    // corrupts every panel drawn afterwards -- silently, and not necessarily
    // this frame.
    // -----------------------------------------------------------------------
    // v2's rule for how big the text is: proportional to the window, and
    // stopped at both ends -- below twelve pixels Consolas stops being legible
    // over a moving render, and above twenty-four a settings panel starts
    // reading as a poster.
    static float v2FontPx(float fbH) { return clampf(fbH / 64.0f, 12.0f, 24.0f); }

    // -----------------------------------------------------------------------
    // THE PIXEL FONT, and the one number that decides whether it looks like one
    // -----------------------------------------------------------------------
    //
    // 3x3-pixel.otf is drawn on a 128-unit grid inside a 640-unit em: five
    // cells to a capital, four to an x-height, six to an advance, and every
    // outline coordinate in the file a multiple of 128. So there is exactly
    // one thing that can go wrong with it, and it is the thing that goes wrong
    // with every pixel font -- a cell that does not land on a whole number of
    // screen pixels is a cell rendered as a grey smear, and a face made
    // entirely of squares has nothing else to look at.
    //
    // ImGui rasterises at SizePixels / (ascent - descent), and this face's
    // ascent and descent are 1024 and -256, so the divisor is 1280 and one
    // 128-unit cell lands at SizePixels/10 screen pixels. ONLY MULTIPLES OF
    // TEN ARE WHOLE CELLS. This quantises to them, and nothing downstream is
    // allowed to scale the result -- see the note on scale in styleV2.
    static float px3Px(float fbH) {
        // Matched to the face it replaces rather than to the line box, which
        // this font leaves half empty: a capital here is five cells, or half
        // the size, where Consolas at v2FontPx gives about six tenths of it.
        const float want = 1.2f * v2FontPx(fbH);
        // v2FontPx stops at 24, so in practice this is twenty up to about
        // 1200 lines and thirty above it -- two screen pixels to a cell, or
        // three.
        return clampf(roundf(want / 10.0f) * 10.0f, 20.0f, 30.0f);
    }

    ImFont *px3_ = nullptr;    // the face at px3Size_, or null for Consolas
    float px3Size_ = 0.0f;
    bool px3Failed_ = false;

    // Bakes the pixel font for this framebuffer and says whether it just did.
    //
    // WHY THIS RUNS INSIDE A FRAME, which is the one thing ImGui asks you not
    // to do to a font atlas: Falcor keeps its Gui private to SampleApp and
    // hands it out nowhere but onGuiRender, so this is the only place that can
    // reach addFont at all. The atlas is marked Locked between NewFrame and
    // Render to catch exactly this, so the lock comes off for the length of
    // the call and goes back on -- and the caller draws NOTHING on a frame
    // that baked, because rebuilding the atlas moves the white-pixel texel
    // that NewFrame had already cached for every filled rectangle. A frame
    // without a readout is invisible; a frame of panels filled with a piece of
    // a letter is not.
    //
    // Falcor rebuilds and re-uploads the atlas texture inside addFont and
    // nowhere else, which is why that call is here: it IS the upload. The copy
    // it loads for itself is at a hardcoded 14 px -- an eighth of a cell, and
    // unusable -- and is never drawn with. It is registered under the name
    // anyway so that a stray setActiveFont("px3") finds something, Falcor's
    // own dereferencing its iterator whether or not the lookup succeeded.
    bool bakePx3(Gui *gui, float fbH) {
        if (px3Failed_) return false;
        if (opt_.font.empty() || opt_.font == "off") {
            px3_ = nullptr;
            return false;
        }
        const float want = px3Px(fbH);
        if (px3_ && want == px3Size_) return false;

        // Asked before the atlas is touched rather than after: addFont throws
        // on a file it cannot read, and a throw halfway through would leave
        // ImGui holding a font the uploaded texture does not have -- which is
        // not a missing font, it is every glyph in the interface reading from
        // the wrong place in the atlas.
        if (FILE *fp = std::fopen(opt_.font.c_str(), "rb")) {
            std::fclose(fp);
        } else {
            std::fprintf(stderr, "v2: cannot open font %s -- drawing in Consolas\n",
                         opt_.font.c_str());
            px3Failed_ = true;
            return false;
        }

        ImGuiIO &io = ImGui::GetIO();
        const bool locked = io.Fonts->Locked;
        io.Fonts->Locked = false;

        ImFontConfig cfg;
        // NO OVERSAMPLING, WHICH IS NOT THE DEFAULT. stb's is a horizontal
        // prefilter: it rasterises at three times the width and blurs back
        // down so a glyph still reads at a fractional position. That is the
        // right answer for an outline face and the exact wrong one for a grid
        // of squares -- it is a blur, and here the squares are the whole
        // picture. PixelSnapH is the other half: it keeps the text origin
        // whole, so the cells cannot drift off the grid they were baked onto.
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        ImFont *face = io.Fonts->AddFontFromFileTTF(opt_.font.c_str(), want, &cfg);
        bool ok = face != nullptr;
        if (ok) {
            try {
                gui->addFont("px3", opt_.font);
            } catch (const std::exception &e) {
                std::fprintf(stderr, "v2: font atlas upload failed (%s)\n", e.what());
                ok = false;
            }
        }
        io.Fonts->Locked = locked;
        if (!ok) {
            px3Failed_ = true;
            px3_ = nullptr;
            return false;
        }
        // ---- NO CAPITALS, AND IT IS DONE IN THE FACE ---------------------
        //
        // Asked for as a rule about the interface rather than about any one
        // label, so it is kept somewhere no label can get past it: A to Z are
        // pointed at the glyphs for a to z, in this font and no other.
        //
        // NOT BY LOWERCASING THE STRINGS, which is the obvious way and is a
        // trap. ImGui hashes a widget's LABEL into its ID: rewriting the
        // literals renames every control in the engine, the positions and
        // sizes ImGui remembers between runs are filed under those names, and
        // any two labels differing only in case would collapse onto one id and
        // become one widget. Remapping the face leaves every string exactly as
        // it was written -- and catches the text v2 does not own as well, the
        // framework's own included.
        //
        // What is typed is also untouched: a capital in the console still
        // reaches runCommand as a capital, it is only drawn as a lowercase.
        //
        // TWO TABLES, because ImGui reads case-sensitively from both.
        // FindGlyph goes through IndexLookup and CalcTextSize takes a fast
        // path through IndexAdvanceX; remap only the first and the capitals
        // draw as lowercase but are still laid out at their old, wider
        // advance -- a line of gaps.
        if (face->IndexLookup.Size > 'z' && face->IndexAdvanceX.Size > 'z') {
            for (int up = 'A'; up <= 'Z'; ++up) {
                const int lo = up - 'A' + 'a';
                face->IndexLookup[up] = face->IndexLookup[lo];
                face->IndexAdvanceX[up] = face->IndexAdvanceX[lo];
            }
        }

        px3_ = face;
        px3Size_ = want;
        const bool lower = face->FindGlyph('A') == face->FindGlyph('a');
        std::printf("v2: text in %s at %.0f px (%.0f-pixel cells), %s\n", opt_.font.c_str(),
                    want, want / 10.0f,
                    lower ? "lowercase only" : "MIXED CASE -- the remap did not take");
        return true;
    }

    // The pixel font for the length of a window's contents.
    //
    // DECLARED AFTER THE WINDOW IT APPLIES TO, always: Falcor pushes its own
    // active font INSIDE Gui::Window -- pushWindow does it after Begin -- so a
    // push made before the window is the one that loses. Being destroyed
    // before the window is what then keeps the two pushes balanced.
    struct px3Font {
        bool on;
        px3Font(ImFont *f) : on(f != nullptr) {
            if (on) ImGui::PushFont(f);
        }
        ~px3Font() {
            if (on) ImGui::PopFont();
        }
    };

    struct styleV2 {
        Gui *gui;
        // What the windows have to pass to SetWindowFontScale to land on
        // v2FontPx. Falcor loads its fonts at fourteen points times whatever
        // the display scaling is, so the number is not knowable up front --
        // unless the pixel font is on, in which case it is one and the reason
        // is below.
        float scale = 1.0f;

        styleV2(Gui *g, ImFont *px, float veil, float fbH) : gui(g) {
            const float target = v2FontPx(fbH);
            if (px) {
                // ONE, AND IT IS THE WHOLE POINT. The pixel font is baked at
                // the size it is drawn at (px3Px), so there is nothing left to
                // scale -- and scaling is precisely what would undo it, since
                // ImGui resamples the atlas bilinearly and even an exact
                // doubling lands every destination pixel between two texels
                // and hands back a grey edge on every square. The window still
                // has to be TOLD one: SetWindowFontScale is remembered per
                // window, and these windows outlive a change of face.
                scale = 1.0f;
            } else {
                // The same fixed-pitch face v2 asked GDI for. Falcor registers
                // it at startup; it only has to be switched on.
                gui->setActiveFont("monospace");
                scale = target / maxf(1.0f, ImGui::GetFontSize());
                font_ = true;
            }

            ImGuiStyle &st = ImGui::GetStyle();
            saved_ = st;
            // In proportion to the text, clamped at both ends -- see the note
            // above the palette.
            const float r = clampf(target, 10.0f, 22.0f);
            st.WindowRounding = r;
            // -- PILLS AND CIRCLES ------------------------------------
            //
            // (user 2026-09-20: "make the slider boxes a pill shape. make the
            //  slider handles a circle. the circle expands when the cursor
            //  hovers.")
            //
            // A BIG NUMBER, NOT A COMPUTED ONE. ImGui clamps a rounding to
            // half the shorter side of the rect it is drawing, so anything
            // past that IS the stadium -- and the trough's height is the font
            // plus the frame padding, which this struct is in the middle of
            // deciding. Asking for more than can be used is exact and needs no
            // number this code does not have yet.
            st.FrameRounding = 64.0f;
            st.GrabRounding = 64.0f;
            // SIXTEEN, UP FROM EIGHT. The original was deliberately thin, and
            // the reasoning still holds -- ImGui centres a slider's value in its
            // trough and draws the grab wherever the value sits, so a wide grab
            // spends part of its travel parked on the number it is there to set.
            //
            // What changed is the trough. The sliders are three times longer
            // now, so the grab covers a third of the fraction of the row it used
            // to, and the number is legible past it again. A thicker handle on a
            // longer bar is easier to catch with the mouse and no harder to read
            // around, which was the only thing the thin one was buying.
            // ...AND A CIRCLE IS A SQUARE THAT HAS BEEN ROUNDED TO DEATH.
            // The grab is as tall as the trough and GrabMinSize is its WIDTH,
            // so the width has to equal the height or the rounding gives a
            // stadium lying on its side. The trough is one line of text plus
            // the frame padding top and bottom, which is what this is.
            //
            // Sixteen was the old value and the note it replaces still holds
            // for why a grab should not be WIDER than it needs to be: ImGui
            // centres the value in the trough and draws the grab wherever the
            // value sits, so a wide grab parks on the number it is there to
            // set. A circle is the smallest shape that reads as a handle.
            st.GrabMinSize = ImGui::GetTextLineHeight() + st.FramePadding.y * 2.0f;
            st.WindowPadding = ImVec2(16.0f, 14.0f);
            st.ItemSpacing = ImVec2(8.0f, 6.0f);
            st.WindowBorderSize = 0.0f;

            ImVec4 panel = ui::kPanel();
            panel.w *= veil;
            ImGui::PushStyleColor(ImGuiCol_WindowBg, panel);
            ImGui::PushStyleColor(ImGuiCol_TitleBg, panel);
            ImGui::PushStyleColor(ImGuiCol_TitleBgActive, panel);
            ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, panel);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kText());
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui::kDim());
            // The selection bar, which in v2 was the highlight behind the row
            // the cursor was on. Here it is the trough and fill of the sliders,
            // which are the rows.
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui::rgb(52, 70, 94));
            // -- THE GAME'S COLOURS, NOT A DEVELOPER'S ------------------
            //
            // (user 2026-09-20: "remember that the games colors are light green
            //  and red. with gold accents".)
            //
            // These were CYAN -- ui::kHot, which is a fine colour for a live
            // value and belongs to nothing in this wood. The thing you grab and
            // the tick you set are the wood's green now, and the ACTIVE state
            // is the gold: green says "this is a control", gold says "you are
            // touching it". That is the accent doing a job rather than being a
            // third colour sprinkled about.
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ui::kGreen());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ui::kGold());
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ui::kGreen());
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::kGreenDim(0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kGold());
            ImGui::PushStyleColor(ImGuiCol_Header, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::kGreenDim(0.45f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::kGreenDim(0.65f));
            ImGui::PushStyleColor(ImGuiCol_Separator, ui::rgb(58, 70, 88));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::kPanel());
            colors_ = 19;
        }
        ~styleV2() {
            ImGui::PopStyleColor(colors_);
            ImGui::GetStyle() = saved_;
            if (font_) gui->setActiveFont("");
        }
        ImGuiStyle saved_;
        int colors_ = 0;
        bool font_ = false;  // whether the ctor moved Falcor's active font
    };

    // -----------------------------------------------------------------------
    // The crosshair.
    //
    // WHITE THROUGH A BLEND OF OneMinusDstColor, which is `1 * (1 - dst)` --
    // the frame under the mark, inverted. v2 reached the same pixels through
    // glLogicOp(GL_XOR) against white, because on eight-bit channels XOR with
    // white is a bitwise NOT; the fixed-function pipeline had no difference
    // blend and glBlendEquation needed an extension loader it did not have.
    // Here the ordinary blender does it, and the shader never has to read the
    // target it is writing to. See shaders/Crosshair.ps.slang.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE TWO BARS, AND THE END OF THE RUN.
    //
    // Both are FullScreenPasses over the window, made the way makeCrosshair
    // makes its own -- but with an ORDINARY alpha blend rather than the
    // crosshair's invert. The crosshair inverts because it must be visible
    // against any backdrop; these two are colours that mean something (v1's
    // reds and its badge gold) and inverting them would turn a red screen
    // green over a green wood.
    // -----------------------------------------------------------------------
    // The blend those two and the notice all want, in one place rather than
    // three copies of nine lines.
    static Falcor::ref<Falcor::BlendState> alphaBlendState() {
        Falcor::BlendState::Desc bd;
        bd.setRtBlend(0, true).setRtParams(0, Falcor::BlendState::BlendOp::Add,
                                           Falcor::BlendState::BlendOp::Add,
                                           Falcor::BlendState::BlendFunc::SrcAlpha,
                                           Falcor::BlendState::BlendFunc::OneMinusSrcAlpha,
                                           // The swapchain's alpha is not ours.
                                           Falcor::BlendState::BlendFunc::Zero,
                                           Falcor::BlendState::BlendFunc::One);
        return Falcor::BlendState::create(bd);
    }

    void makeVitalsPasses() {
        const auto blend = alphaBlendState();
        vitalsPass_ = Falcor::FullScreenPass::create(getDevice(), "v1/shaders/Vitals.ps.slang");
        vitalsPass_->getState()->setBlendState(blend);
        gameOver_ = Falcor::FullScreenPass::create(getDevice(), "v1/shaders/GameOver.ps.slang");
        gameOver_->getState()->setBlendState(blend);
    }

    void drawVitals(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) {
        if (!vitalsPass_ || quitting_) return;
        // NOT WHILE THE MENU IS UP. The panel is read, not fought through, and
        // a red rim over it is only in the way.
        if (menuOpen_) return;
        const int red = vitals_.redLevel(), gold = vitals_.goldLevel();
        if (red <= 0 && gold <= 0) return;
        auto var = vitalsPass_->getRootVar();
        var["gVitCB"]["gSize"] = float2(float(target->getWidth()), float(target->getHeight()));
        var["gVitCB"]["gRedLevel"] = float(red);
        var["gVitCB"]["gGoldLevel"] = float(gold);
        var["gVitCB"]["gHurtT"] = vitals_.hurtT;
        var["gVitCB"]["gSeed"] = float(vitSeed_);
        vitalsPass_->execute(ctx, target);
    }

    void drawGameOver(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) {
        if (!gameOver_ || deathAtMs_ < 0.0) return;
        const double age = simMs_ - deathAtMs_;
        const float fade = float(age <= 0.0 ? 0.0
                                 : age >= kGameOverFadeMs ? 1.0
                                                          : age / kGameOverFadeMs);
        auto var = gameOver_->getRootVar();
        var["gOverCB"]["gSize"] = float2(float(target->getWidth()), float(target->getHeight()));
        var["gOverCB"]["gFade"] = fade;
        // TWO LINES, AND THE FONT IS THE WOOD'S OWN -- holoPackGlyph is what
        // the signs in the world are lettered with, so the death screen is in
        // the same hand as everything else. See shaders/GameOver.ps.slang.
        // -- THE WORDS ARE DRAWN IN THE GUI PASS NOW -----------------------
        //
        // See drawNoticeGui: this shader's alphabet is holoPackGlyph, which is
        // the world's SIGN font and not the pixel font every other word on the
        // screen is set in. The curtain and the vignette stay here -- they are
        // full-screen and belong in the render path -- and the glyph counts go
        // to zero, so the shader itself is untouched and putting the lettering
        // back is a matter of restoring these two lines.
        const std::string l0 = "";
        const std::string l1 = "";
        const int n0 = 0;
        const int n1 = 0;
        var["gOverCB"]["gCount0"] = n0;
        var["gOverCB"]["gCount1"] = n1;
        for (int i = 0; i < 16; ++i)
            var["gOverCB"]["gGlyph"][i] = i < n0 ? holoPackGlyph(l0[size_t(i)]) : 0u;
        for (int i = 0; i < 16; ++i)
            var["gOverCB"]["gGlyph"][16 + i] = i < n1 ? holoPackGlyph(l1[size_t(i)]) : 0u;
        gameOver_->execute(ctx, target);
    }

    // =======================================================================
    // THE NOTICE: a hint that teaches one key, and v1's discovery banner.
    // =======================================================================
    //
    // (user 2026-09-20: "after 10 seconds of initially spawning in, display on
    //  their screen in the middle of the screen. press g to respawn ... after
    //  the text blinking for 5 seconds, it shuts off forever. this only happens
    //  on the first spawn in." -- and "can you import the achievment get from
    //  v1 when firing the bow. it should say discovery then projectile while
    //  playing the discovery 8bit jingle.")
    //
    // ONE PASS, EXECUTED TWICE. Both are lines of text over the frame and both
    // are shaders/Notice.ps.slang, but the banner wants two lines and the hint
    // wants one, so rather than rank them they each get their own execute with
    // their own constants. The pass carries no state and discards every pixel
    // that is not a lit cell, so a second run over a screen with nothing on it
    // costs a full-screen discard and nothing else -- which buys the case where
    // a discovery is earned during the hint's five seconds and both should
    // stand. Ranking them would have silently eaten one, and the hint has
    // exactly one chance to be seen.
    // -----------------------------------------------------------------------

    static constexpr double kHintDelayMs = 10000.0;   // after the FIRST spawn
    static constexpr double kHintBlinkMs = 750.0;     // one blink: 500 on, 250 off
    // -- COUNTED IN BLINKS, NOT IN SECONDS ------------------------------
    //
    // (user 2026-09-21: "have the press g to respawn text only blink 5
    //  times. not 7".)
    //
    // THE ORIGINAL ASK WAS FIVE SECONDS and this held 5000 ms, which at a
    // 750 ms blink is six and two thirds -- so what anybody actually
    // COUNTED was seven, the last one cut short. Seconds and blinks are not
    // the same unit and the screen only shows one of them.
    //
    // So the blink count is the constant now and the duration falls out of
    // it. A change to kHintBlinkMs keeps the count it was set to rather than
    // silently re-dividing into a fixed five seconds.
    static constexpr int kHintBlinks = 5;
    static constexpr double kHintShowMs = kHintBlinks * kHintBlinkMs;
    static constexpr double kHintBlinkOnMs = 500.0;
    static constexpr double kHintRampMs = 80.0;       // the edges of a blink
    static constexpr double kHintTailMs = 300.0;      // and how it stops
    // v1's own two numbers: achHideT is now + 5200, over a 0.9 s CSS transition
    // at both ends (#achv { transition: opacity 0.9s ease }).
    static constexpr double kAchHoldMs = 5200.0;
    static constexpr double kAchFadeMs = 900.0;
    // How far the banner sits above where it used to -- see its use. TWENTY
    // AND THEN TWENTY MORE (user 2026-09-20: "move the discovery banner up
    // another 20 pixels"), which is why this is one constant and not a number
    // written into the line: the next nudge is one edit in one place.
    static constexpr float kNoticeRaiseP = 40.0f;
    // How big the discovery banner is against the size it was authored at.
    // See its use for why one factor rather than four edited numbers.
    static constexpr float kBannerScale = 0.75f;

    // #ffd76a. v1 uses this one gold for the banner's "discovery", the menu
    // button hover and the watermark, and its own note says why there is only
    // one: "two golds a few hex apart read as a mistake rather than a choice".
    static float3 badgeGold() { return float3(1.0f, 0.843f, 0.416f); }
    // ...and the same colour for the interface, which speaks ImVec4.
    static inline const ImVec4 kBadgeGold{1.0f, 0.843f, 0.416f, 1.0f};


    // =======================================================================
    // THE NOTICE, IN THE GAME'S OWN HAND.
    // =======================================================================
    //
    // (user 2026-09-20: "you seem to be using a different pixel text for the
    //  discovery text. make sure the discovery text matches the rest of the
    //  games text. the 3x3 font text file.")
    //
    // IT WAS A SHADER, AND THAT WAS THE MISTAKE. The banner and the hint were
    // lettered with ui/holotext.h -- the 5 x 6 bitmap the SIGNS IN THE WORLD
    // are drawn with -- because they are drawn over the traced frame and that
    // font needs no atlas. It is the right face for text the tracer draws and
    // the wrong one for text the INTERFACE draws, and the discovery banner is
    // interface: the console, the settings panel, the coords readout and the
    // watermark are all 3x3-pixel.otf, so the banner was the one thing on
    // screen in a different alphabet.
    //
    // So it is an ImGui overlay now, with px3_ pushed, exactly like the console
    // reply a few lines up -- same face, same baking, same scaling rule. The
    // timing, the blink, the fades and the gold key are unchanged; only the
    // thing that draws the glyphs moved.
    //
    // STILL OUT OF THE RECORDING. The recorder reads the tracer's display
    // texture, and everything ImGui draws lands on the window afterwards --
    // the same reason the console and the crosshair never appear in a take.
    // -----------------------------------------------------------------------
    void drawNoticeGui(Gui *pGui) {
        // THE WINDOW, ASKED HERE. onGuiRender takes these off the target fbo
        // and this is called from inside it, so the same question has the same
        // answer -- but they are its locals, not members.
        const float fbW = float(getTargetFbo()->getWidth());
        const float fbH = float(getTargetFbo()->getHeight());
        if (!px3_ && !bakePx3(pGui, fbH)) {
            // No face yet on the first frame or two; nothing is drawn rather
            // than falling back to Consolas, which would be the same mismatch
            // in the other direction.
        }
        // WHERE "SPAWNING IN" IS DECIDED -- see the note this replaces. Armed
        // before the gates so a player who opens the menu on arrival does not
        // restart the clock.
        if (spawnAtMs_ < 0.0) spawnAtMs_ = simMs_;
        // -- THE DEATH SCREEN'S WORDS, IN THE HAND EVERYTHING ELSE USES ----
        //
        // (user 2026-09-21: "the game over screen doesnt seem to be using the
        //  right pixel font. double check this and fix.")
        //
        // IT WAS NOT, AND IT WAS NOT A BUG SO MUCH AS A DIFFERENT ALPHABET.
        // drawGameOver lettered both lines with holoPackGlyph -- the font the
        // SIGNS IN THE WORLD are cut from, a same-height uppercase alphabet
        // built for geometry in the traced scene (see V2Holo). Every other
        // word on the screen is 3x3-pixel.otf through ImGui. The two are not
        // close, and the death screen is the one place a player stops and
        // reads.
        //
        // SO THE LETTERS MOVE TO THIS PASS AND THE CURTAIN STAYS IN THAT ONE.
        // GameOver.ps.slang still draws the fade and the vignette -- it is
        // full-screen and belongs in the render path -- and its glyph count is
        // simply zero now, so the shader and its constant buffer are untouched
        // and the change is one line to undo.
        //
        // NOT gated on hintSpent_ below: it is drawn before that early-out,
        // because a dead player has certainly spent the hint.
        if (deathAtMs_ >= 0.0) {
            const double dage = simMs_ - deathAtMs_;
            const float dfade = float(dage <= 0.0             ? 0.0
                                      : dage >= kGameOverFadeMs ? 1.0
                                                                : dage / kGameOverFadeMs);
            if (dfade > 0.0f) {
                const float dw = ImGui::GetIO().DisplaySize.x;
                const float dh = ImGui::GetIO().DisplaySize.y;
                std::string why = deathWhy_;
                for (char &c : why) c = char(std::tolower((unsigned char)c));
                // THE SAME SCALE PAIR THE DISCOVERY BANNER USES, so the two
                // pieces of screen type in this engine are set the same way --
                // a small tracked-out label over a larger line.
                noticeText(pGui, "game over", dw * 0.5f, dh * 0.42f, 2.6f * kBannerScale,
                           dfade, ImVec4(0.94f, 0.30f, 0.28f, 1.0f),
                           /*tracking=*/8.0f * kBannerScale);
                if (!why.empty())
                    // +20 px UNDER THE RED (user 2026-09-21: "move the text
                    // down 20 pixels. its too close"). Scaled by noticeUi_ with
                    // the rest of the block, so the gap holds at every
                    // framebuffer size rather than closing up on a small one.
                    noticeText(pGui, why.c_str(), dw * 0.5f,
                               dh * 0.42f + (56.0f * kBannerScale + 20.0f) * noticeUi_,
                               1.3f * kBannerScale, dfade,
                               ImVec4(0.92f, 0.92f, 0.90f, 1.0f),
                               /*tracking=*/4.0f * kBannerScale);
            }
        }

        // ...AND THE DEATH SCREEN IS DRAWN BEFORE THAT RETURN, because the
        // return's own condition includes being dead. Everything below it is
        // a notice for a LIVING player -- the hint, the discovery banner --
        // and the curtain is the one piece of screen type that belongs on
        // top of a death. Putting the block after it was a silent no-op: the
        // curtain came up with no words on it at all.
        if (menuOpen_ || quitting_ || deathAtMs_ >= 0.0) return;

        const float cw = fbW > 0.0f ? fbW : 1280.0f;
        const float ch = fbH > 0.0f ? fbH : 720.0f;
        noticeUi_ = maxf(1.0f, ch / 1080.0f);

        // -- v1's BANNER ----------------------------------------------------
        float achA = 0.0f;
        if (achAtMs_ >= 0.0) {
            const double age = simMs_ - achAtMs_;
            if (age >= kAchHoldMs + kAchFadeMs)
                achAtMs_ = -1.0;
            else if (age < kAchFadeMs)
                achA = float(age / kAchFadeMs);
            else if (age < kAchHoldMs)
                achA = 1.0f;
            else
                achA = float((kAchHoldMs + kAchFadeMs - age) / kAchFadeMs);
        }
        if (achA > 0.0f) {
            // #achv sits at calc(50% - 350px), which on a 1080-line window is a
            // fifth of the way down. Proportional rather than a fixed 350 so it
            // does not walk into the middle of a short window.
            // TWENTY PIXELS UP, BOTH LINES (user 2026-09-20: "move the
            // discovery text along with the projectile text up 20 pixels on
            // the screen"). A RAW twenty, not scaled by noticeUi_: the ask is
            // in screen pixels on the display it was read on, and multiplying
            // it by the interface scale would make it thirty-seven there.
            const float top = ch * 0.20f - kNoticeRaiseP;
            // -- A QUARTER SMALLER, AND EVERY NUMBER OF IT ---------------
            //
            // (user 2026-09-20: "decrease the discovery banner text by 25%".)
            //
            // ONE FACTOR OVER THE WHOLE BANNER rather than two smaller type
            // sizes typed in. The tracking and the gap between the two lines
            // are part of how a label is set -- shrink the glyphs and leave
            // the letter-spacing where it was and it stops reading as one
            // word -- so the scale multiplies all four.
            noticeText(pGui, "discovery", cw * 0.5f, top, 1.0f * kBannerScale, achA,
                       kBadgeGold, /*tracking=*/6.0f * kBannerScale);
            noticeText(pGui, achLabel_.c_str(), cw * 0.5f,
                       top + 26.0f * kBannerScale * noticeUi_, 1.9f * kBannerScale, achA,
                       ImVec4(1.0f, 1.0f, 1.0f, 1.0f), /*tracking=*/4.0f * kBannerScale);
        }

        // -- THE ONE-OFF HINT ------------------------------------------------
        if (hintSpent_) return;
        const double age = simMs_ - spawnAtMs_ - kHintDelayMs;
        if (age >= kHintShowMs) {
            hintSpent_ = true;
            return;
        }
        if (age < 0.0) return;
        if (respawnUsed_) {
            hintSpent_ = true;
            return;
        }
        const double ph = std::fmod(age, kHintBlinkMs);
        double b = 0.0;
        if (ph < kHintRampMs)
            b = ph / kHintRampMs;
        else if (ph < kHintBlinkOnMs - kHintRampMs)
            b = 1.0;
        else if (ph < kHintBlinkOnMs)
            b = (kHintBlinkOnMs - ph) / kHintRampMs;
        const double left = kHintShowMs - age;
        if (left < kHintTailMs) b *= left / kHintTailMs;
        if (b <= 0.0) return;
        if (!hintSaid_) {
            hintSaid_ = true;
            std::printf("v2: hint -- press g to respawn (%.1f s in)\n",
                        (simMs_ - spawnAtMs_) / 1000.0);
            std::fflush(stdout);
        }
        // A BIT BELOW THE MARK, not on it: the crosshair's arm reaches 16 px
        // from the centre at 1080p, so this leaves the same again clear.
        // THE KEY IS GOLD, the banner's own -- so the one thing the player has
        // to press is the one thing coloured. Drawn as three runs rather than
        // one string, which is the only way ImGui colours part of a line.
        noticeKeyLine(pGui, cw * 0.5f, ch * 0.5f + 34.0f * noticeUi_, float(b));
    }

    // The interface's own scale, so the notice grows with the rest of it.
    // PASSED rather than asked for: the window size is a local of
    // onGuiRender, and a second way of getting it is a second thing to keep
    // in step.
    float noticeUi_ = 1.0f;

    // ONE CENTRED LINE, with the drop shadow v1 gives this banner
    // (`text-shadow: 0 1px 3px #000`). Without it gold at this size over a lit
    // cloud is unreadable, which the first capture of the shader version showed
    // plainly -- so the shadow survived the move.
    void noticeText(Gui *pGui, const char *text, float cx, float topY, float scale, float alpha,
                    const ImVec4 &col, float tracking) {
        (void)pGui;
        if (!text || !*text) return;
        if (px3_) ImGui::PushFont(px3_);
        const float s = scale * noticeUi_;
        // Tracked out by drawing a glyph at a time: v1 sets letter-spacing on
        // this banner and that spacing is most of why it reads as a label
        // rather than as a word. ImGui has no such style, so the advance is
        // stepped by hand.
        const float sp = tracking * noticeUi_;
        float wide = 0.0f;
        for (const char *p = text; *p; ++p) {
            char one[2] = {*p, 0};
            wide += ImGui::CalcTextSize(one).x * s + sp;
        }
        wide -= sp;
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        ImFont *face = px3_ ? px3_ : ImGui::GetFont();
        const float px = ImGui::GetFontSize() * s;
        const float shadow = maxf(1.0f, 2.0f * noticeUi_);
        float x = cx - wide * 0.5f;
        for (const char *p = text; *p; ++p) {
            char one[2] = {*p, 0};
            dl->AddText(face, px, ImVec2(x + shadow, topY + shadow),
                        IM_COL32(0, 0, 0, int(alpha * 184.0f)), one);
            dl->AddText(face, px, ImVec2(x, topY),
                        IM_COL32(int(col.x * 255.0f), int(col.y * 255.0f), int(col.z * 255.0f),
                                 int(alpha * 255.0f)),
                        one);
            x += ImGui::CalcTextSize(one).x * s + sp;
        }
        if (px3_) ImGui::PopFont();
    }

    // "press g to respawn", with the g in the banner's gold.
    void noticeKeyLine(Gui *pGui, float cx, float topY, float alpha) {
        (void)pGui;
        if (px3_) ImGui::PushFont(px3_);
        // 1.25 ONCE, AND A QUARTER OFF IT (user 2026-09-20: "shrink the press
        // g to respawn by 25%"). Written as the product rather than as 0.9375
        // so the number that was tuned and the cut that was asked for stay
        // legible as two separate decisions.
        const float s = 1.25f * 0.75f * noticeUi_;
        const char *kText = "press g to respawn";
        // WHICH LETTER IS THE KEY, found rather than counted -- the accent
        // cannot drift onto the wrong letter if the wording is ever reworded.
        const std::string t(kText);
        const size_t g = t.find(" g ");
        const size_t gi = (g == std::string::npos) ? t.size() : g + 1;
        float wide = 0.0f;
        for (size_t i = 0; i < t.size(); ++i) {
            char one[2] = {t[i], 0};
            wide += ImGui::CalcTextSize(one).x * s;
        }
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        ImFont *face = px3_ ? px3_ : ImGui::GetFont();
        const float px = ImGui::GetFontSize() * s;
        const float shadow = maxf(1.0f, 2.0f * noticeUi_);
        float x = cx - wide * 0.5f;
        for (size_t i = 0; i < t.size(); ++i) {
            char one[2] = {t[i], 0};
            const ImVec4 c = (i == gi) ? kBadgeGold : ImVec4(0.94f, 0.94f, 0.92f, 1.0f);
            dl->AddText(face, px, ImVec2(x + shadow, topY + shadow),
                        IM_COL32(0, 0, 0, int(alpha * 184.0f)), one);
            dl->AddText(face, px, ImVec2(x, topY),
                        IM_COL32(int(c.x * 255.0f), int(c.y * 255.0f), int(c.z * 255.0f),
                                 int(alpha * 255.0f)),
                        one);
            x += ImGui::CalcTextSize(one).x * s;
        }
        if (px3_) ImGui::PopFont();
    }

    // -----------------------------------------------------------------------
    // ONE DISCOVERY, THEN ANY NUMBER OF THEM -- v1's unlockAch, with its split.
    //
    // `key` is what makes it fire exactly once and `label` is what the player
    // reads, and both are session-only: v1 moved these off localStorage on
    // purpose ("user asked for per-session"), so a relaunch brings the
    // discoveries -- and the hint above -- back.
    //
    // EARNING ONE WHILE ANOTHER IS UP RETITLES THE BANNER and restarts its five
    // seconds, which is v1's rule and its reasoning: "a proper queue would hold
    // the second one back past the moment that earned it, which is the one
    // thing a discovery must not do."
    // -----------------------------------------------------------------------
    void unlockAch(const char *key, const char *label) {
        for (const std::string &k : achFired_)
            if (k == key) return;
        achFired_.emplace_back(key);
        achLabel_ = label;
        achAtMs_ = simMs_;
        toolSfx_.discovered();
        std::printf("v2: discovery -- %s\n", label);
        std::fflush(stdout);
    }

    // THE FIRST SHAFT EVER SENT DOWNRANGE. Armed where the arrow is actually
    // away, for v1's reason: shootArrow "can bail before it (no item, dead,
    // editor open), and a discovery that fires on a swallowed shot is a
    // discovery for pressing a button."
    void unlockProjectile() { unlockAch("projectile", "projectile"); }

    double spawnAtMs_ = -1.0;    // the first frame the player was shown
    bool hintSpent_ = false;     // ...and the hint runs once per launch
    bool hintSaid_ = false;      // printed once, so a headless run can time it
    bool respawnUsed_ = false;   // they found [G] without being told
    std::vector<std::string> achFired_;   // v1's vbAch, session-only
    std::string achLabel_;
    double achAtMs_ = -1.0;

    void makeCrosshair() {
        crosshair_ = Falcor::FullScreenPass::create(getDevice(), "v1/shaders/Crosshair.ps.slang");
        Falcor::BlendState::Desc bd;
        bd.setRtBlend(0, true).setRtParams(0, Falcor::BlendState::BlendOp::Add,
                                           Falcor::BlendState::BlendOp::Add,
                                           Falcor::BlendState::BlendFunc::OneMinusDstColor,
                                           Falcor::BlendState::BlendFunc::Zero,
                                           // Alpha is left exactly as it was: the
                                           // swapchain's is not ours to invert.
                                           Falcor::BlendState::BlendFunc::Zero,
                                           Falcor::BlendState::BlendFunc::One);
        crosshair_->getState()->setBlendState(Falcor::BlendState::create(bd));
    }

    void drawCrosshair(Falcor::RenderContext *ctx, const Falcor::ref<Fbo> &target) {
        // The settings panel owns the middle of the screen, and while it is up
        // the mouse is a cursor -- a mark that does not follow it is in the way.
        // ...AND NOT OVER A SCREEN THAT IS SWITCHING OFF. The crosshair is
        // drawn after the tone map, so it would sit there in full brightness
        // over a collapsing picture and then float alone on the black.
        // ...AND NOT IF IT HAS BEEN SWITCHED OFF (user 2026-09-22). The row
        // is under visuals, beside the compass -- the two things in that card
        // that are ON the screen rather than part of how it is rendered.
        if (!crosshair_ || !opt_.crosshair || menuOpen_ || quitting_) return;

        const float w = float(target->getWidth()), h = float(target->getHeight());
        // v2's sizes, and v2's whole-number scaling rule with them: 32 px across
        // and 4 px thick at 1080p, DOUBLING at 2160p rather than growing
        // continuously, so the bars stay hard-edged instead of landing half on a
        // pixel and going grey.
        const float scale = maxf(1.0f, floorf(h / 1080.0f));

        auto var = crosshair_->getRootVar();
        var["gCrossCB"]["gSize"] = float2(w, h);
        var["gCrossCB"]["gArm"] = 16.0f * scale;
        var["gCrossCB"]["gHalf"] = 2.0f * scale;
        // -- ONLY THE CROSS'S OWN BOX IS SHADED (2026-09-23) -----------------
        //
        // A full-screen pass shaded all 7.6 million pixels of a 3820x1990 frame
        // to draw a 64-pixel cross, discarding every one of them but that. The
        // viewport and the scissor are cut to the box the arms can reach: the
        // shader reads SV_Position, which stays in whole-target pixels however
        // small the viewport is, so every pixel it does shade comes out exactly
        // as before, and the ones it no longer shades it was discarding anyway.
        const float arm = 16.0f * scale + 2.0f;
        const float cx = w * 0.5f, cy = h * 0.5f;
        const float x0 = maxf(0.0f, floorf(cx - arm)), y0 = maxf(0.0f, floorf(cy - arm));
        const float x1 = minf(w, ceilf(cx + arm)), y1 = minf(h, ceilf(cy + arm));
        auto st = crosshair_->getState();
        st->setFbo(target, false);
        st->setViewport(0, Falcor::GraphicsState::Viewport(x0, y0, x1 - x0, y1 - y0, 0.0f, 1.0f),
                        true);
        crosshair_->execute(ctx, target, /*autoSetVpSc=*/false);
    }

    // -----------------------------------------------------------------------
    // The profile report.
    //
    // PERCENTILES, NOT AN AVERAGE. What a person feels walking through this
    // wood is the worst frame in the last second, not the mean of the last
    // hundred -- and the two move independently, because the streamer's cost
    // arrives in bursts while the renderer's is flat. So the tail is reported
    // in full, and every frame that cost more than twice the median is counted
    // as a hitch and attributed: streaming, or the renderer.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE DISTRIBUTION, AND WHAT THE BAD END OF IT WAS DOING.
    //
    // PERCENTILES RATHER THAN A MEAN, because the mean is the number that hid
    // this. What a player feels is the worst one per cent: at 60 fps a frame
    // has 16.7 ms, and every frame over that is a visible stutter however good
    // the average is.
    // -----------------------------------------------------------------------
    void printHitch() {
        if (hitch_.size() < 30) {
            std::printf("\n=== HITCH: only %zu frames, not enough to judge ===\n",
                        hitch_.size());
            return;
        }
        // THE FIRST SECOND IS THE PRIME, not the walk. Every chunk in the
        // starting disc arrives at once there; including it would report the
        // load as though it were something you feel while strolling.
        const size_t skip = std::min<size_t>(hitch_.size() / 4, 120);
        std::vector<HitchFrame> f(hitch_.begin() + long(skip), hitch_.end());
        std::vector<float> t;
        t.reserve(f.size());
        for (const HitchFrame &h : f) t.push_back(h.total);
        std::sort(t.begin(), t.end());
        auto pc = [&](double q) { return t[size_t(q * double(t.size() - 1))]; };
        const double budget = 16.7;   // one frame at 60 fps
        size_t over = 0, over2 = 0;
        double sum = 0.0;
        for (float v : t) {
            sum += v;
            if (v > budget) ++over;
            if (v > budget * 2.0) ++over2;
        }
        std::printf("\n=== HITCH: %zu frames walking (%zu of prime skipped) ===\n", f.size(),
                    skip);
        std::printf("  mean %.2f ms   median %.2f   p90 %.2f   p99 %.2f   worst %.2f\n",
                    sum / double(t.size()), pc(0.50), pc(0.90), pc(0.99), t.back());
        std::printf("  over 16.7 ms: %zu frames (%.1f%%)   over 33.3: %zu (%.1f%%)\n", over,
                    100.0 * double(over) / double(t.size()), over2,
                    100.0 * double(over2) / double(t.size()));

        // ...AND THE TEN WORST, with what streaming was doing on each. A hitch
        // that is all `stream` is the world arriving; one that is none of it is
        // somewhere else entirely, and that difference is the whole point of
        // printing both.
        std::vector<const HitchFrame *> worst;
        for (const HitchFrame &h : f) worst.push_back(&h);
        std::sort(worst.begin(), worst.end(),
                  [](const HitchFrame *a, const HitchFrame *b) { return a->total > b->total; });
        std::printf("\n  the ten worst frames\n");
        std::printf("  %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s %6s\n", "total", "stream", "blas",
                    "tlas", "rering", "drain", "read", "phys", "life", "pub", "chunk");
        for (size_t i = 0; i < 10 && i < worst.size(); ++i) {
            const HitchFrame *h = worst[i];
            std::printf("  %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %6d\n",
                        double(h->total), double(h->stream), double(h->blas), double(h->tlas),
                        double(h->rering), double(h->drain), double(h->cread), double(h->phys),
                        double(h->life), double(h->pub), h->adopted);
            // ...and a structure build big enough to matter, split -- see
            // World::Profile::bAcq.
            if (h->blas > 1.0f)
                std::printf("          blas = acquire %.2f  upload %.2f  prebuild %.2f  "
                            "result %.2f  create %.2f\n",
                            double(h->bAcq), double(h->bUpd), double(h->bPre), double(h->bRes),
                            double(h->bCre));
        }
        // WHERE THE TIME OVER BUDGET ACTUALLY WENT, summed over the bad frames
        // only. This is the number that says what to fix: the total bill from
        // --stats counts the cheap frames too, and they are not the problem.
        double bt = 0.0, bs = 0.0, bb = 0.0, bl = 0.0, bp = 0.0, br = 0.0;
        for (const HitchFrame &h : f) {
            if (h.total <= budget) continue;
            bt += h.total;
            bs += h.stream;
            bb += h.blas;
            bl += h.tlas;
            bp += h.pool;
            br += h.rering;
        }
        if (over) {
            std::printf("\n  across the %zu frames over budget, %.0f ms total:\n", over, bt);
            double bd = 0.0;
            for (const HitchFrame &h : f)
                if (h.total > budget) bd += h.drain;
            std::printf("    streaming %.0f ms (%.0f%%)  -- of which blas %.0f, tlas %.0f, "
                        "pool %.0f, rering %.0f, drain %.0f\n",
                        bs, 100.0 * bs / bt, bb, bl, bp, br, bd);
            double bph = 0.0, bl2 = 0.0, bpb = 0.0;
            for (const HitchFrame &h : f) {
                if (h.total <= budget) continue;
                bph += h.phys;
                bl2 += h.life;
                bpb += h.pub;
            }
            std::printf("    physics     %.0f ms (%.0f%%)\n", bph, 100.0 * bph / bt);
            std::printf("    life        %.0f ms (%.0f%%)\n", bl2, 100.0 * bl2 / bt);
            std::printf("    publish     %.0f ms (%.0f%%)\n", bpb, 100.0 * bpb / bt);
            // WHAT IS LEFT IS THE RENDER DISPATCH AND ANYTHING NOT YET
            // TIMED. A big number here is not an answer, it is the next
            // place to put a clock.
            std::printf("    unaccounted %.0f ms (%.0f%%)\n", bt - bs - bph - bl2 - bpb,
                        100.0 * (bt - bs - bph - bl2 - bpb) / bt);
        }
        std::fflush(stdout);
    }

    void printProfile() {
        if (frameMs_.empty()) return;
        // The first frames of a run are the prime finishing and the first
        // structure builds landing, and they are not what the engine does when
        // it is running. Dropped so a steady-state number is a steady-state
        // number.
        const size_t warm = mini(size_t(30), frameMs_.size() / 4);
        std::vector<float> f(frameMs_.begin() + warm, frameMs_.end());
        std::vector<float> u(streamMs_.begin() + warm, streamMs_.end());
        const size_t n = f.size();
        if (n == 0) return;

        double sum = 0.0, streamSum = 0.0;
        for (size_t i = 0; i < n; ++i) { sum += f[i]; streamSum += u[i]; }

        std::vector<float> sorted = f;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double q) { return sorted[mini(n - 1, size_t(q * double(n)))]; };
        const float med = pct(0.50);

        size_t hitches = 0, streamHitches = 0;
        float worstStream = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (f[i] > 2.0f * med) {
                ++hitches;
                // Attributed to the streamer when the streamer accounts for
                // most of the overshoot -- which is a claim the numbers can
                // support, unlike "it felt like loading".
                if (u[i] > 0.5f * (f[i] - med)) ++streamHitches;
            }
            worstStream = maxf(worstStream, u[i]);
        }

        // The device-side breakdown, if the profiler collected one.
        //
        // EVERY EVENT, NOT A LIST OF NAMES (2026-09-23). This used to keep five
        // leaves it knew about and drop the rest, which is how "probes" and
        // "fog" were silently missing for weeks and how "atmosphere" and
        // "cloudfill" still were -- a profile that filters by name can only
        // ever confirm what its author already suspected. Printed as the tree
        // Falcor records, indented by depth, CPU beside GPU: the CPU column is
        // the cost of RECORDING the scope, which is what the frame loop pays.
        if (Falcor::Profiler *prof = getDevice()->getProfiler()) {
            std::printf("\n  %-44s %9s %9s\n", "gpu scope", "gpu ms", "cpu ms");
            for (Falcor::Profiler::Event *e : prof->getEvents()) {
                if (!e) continue;
                const std::string path = e->getName();
                int depth = 0;
                for (char ch : path) depth += ch == '/' ? 1 : 0;
                const size_t slash = path.find_last_of('/');
                const std::string leaf =
                    slash == std::string::npos ? path : path.substr(slash + 1);
                const std::string label =
                    std::string(size_t(maxi(0, depth - 1) * 2), ' ') + leaf;
                std::printf("  %-44s %9.3f %9.3f\n", label.c_str(),
                            double(e->getGpuTimeAverage()), double(e->getCpuTimeAverage()));
            }
        }

        // -- ...AND WHAT EACH FEATURE COSTS THE TRACE -- see kAblFirst ---------
        if (ablateOn()) {
            std::printf("\n  %-26s %11s %9s %10s %7s\n", "ablation (standing)", "camera ms",
                        "fog ms", "frame gpu", "frames");
            const double base = ablN_[kAblBase] ? ablSum_[kAblBase] / ablN_[kAblBase] : 0.0;
            for (int k = 0; k < kAblCount; ++k) {
                if (!ablN_[k]) continue;
                const double cam = ablSum_[k] / ablN_[k];
                std::printf("  %-26s %8.3f %+5.0f%% %9.3f %10.3f %7d\n", kAblName[k], cam,
                            base > 0.0 ? 100.0 * (cam - base) / base : 0.0,
                            ablFog_[k] / ablN_[k], ablAll_[k] / ablN_[k], ablN_[k]);
            }
        }

        // -- ...AND THE MAIN THREAD, CUT AT ITS OWN BLOCKS -- see CpuSeg --------
        //
        // Over EVERY frame since launch, prime included, so it is read against
        // the period line under it as shares rather than as absolute steady-state
        // costs. The row that matters is the one that is big for no reason.
        if (segFrames_ > 0) {
            double inside = 0.0;
            std::printf("\n  %-34s %9s %9s\n", "cpu segment", "mean ms", "worst ms");
            for (int k = 0; k < kSegCount; ++k) {
                const double mean = segSum_[k] / double(segFrames_);
                inside += mean;
                std::printf("  %-34s %9.3f %9.2f\n", kSegName[k], mean, segWorst_[k]);
            }
            std::printf("  %-34s %9.3f\n", "= onFrameRender", inside);
            for (int k = 0; k < kLapCount; ++k)
                std::printf("    %-30s %9.3f %9.2f\n", kLapName[k],
                            lapSum_[k] / double(segFrames_), lapWorst_[k]);
            std::printf("  %-34s %9.3f   (the frame period minus the above: present, Falcor)\n",
                        "outside it", sum / double(n) - inside);
        }

        const World::Profile w = world_.profile();
        std::printf(
            "\nv2 profile -- %zu frames at %dx%d -> %dx%d, %s\n"
            "  frame     mean %.2f ms (%.0f fps)   median %.2f   p95 %.2f   p99 %.2f   max %.2f\n"
            "  hitches   %zu over 2x median (%.2f%%), %zu of them streaming\n"
            "  stream    %.3f ms/frame average, worst frame %.2f ms\n",
            n, tracer_.width(), tracer_.height(), tracer_.outWidth(), tracer_.outHeight(),
            tracer_.denoising() ? dlssQualityName(opt_.dlssQuality) : "accumulate",
            sum / double(n), 1000.0 * double(n) / maxf(1e-6f, float(sum)), med, pct(0.95),
            pct(0.99), sorted[n - 1], hitches, 100.0 * double(hitches) / double(n), streamHitches,
            streamSum / double(n), worstStream);
        std::printf(
            "  chunks    %zu meshed on workers, %zu adopted on the main thread\n"
            "  mesh      %.1f ms/chunk (off-thread)\n"
            "  blas      %.2f ms/batch over %zu batches   %.1f ms total (MAIN THREAD)\n"
            "  pool      %.2f ms/chunk   %.1f ms total    (MAIN THREAD)\n"
            "  tlas      %.2f ms/rebuild over %zu rebuilds  %.1f ms total (MAIN THREAD)\n",
            w.meshed, w.adopted, w.meshed ? w.meshMs / double(w.meshed) : 0.0,
            w.blasCalls ? w.blasMs / double(w.blasCalls) : 0.0, w.blasCalls, w.blasMs,
            w.adopted ? w.poolMs / double(w.adopted) : 0.0, w.poolMs,
            w.tlasCalls ? w.tlasMs / double(w.tlasCalls) : 0.0, w.tlasCalls, w.tlasMs);
        if (opt_.sharcStats && sharc_.available()) {
            const Sharc::Stats st = sharc_.readStats();
            const double cap = double(sharc_.capacity());
            std::printf(
                "  cache     %u of %u entries live (%.1f%% occupancy)\n"
                "  cache     %u queries, %u hit (%.1f%%), %u inserts dropped (bucket full)\n",
                st.live, sharc_.capacity(), 100.0 * double(st.live) / cap,
                st.queries, st.hits,
                st.queries ? 100.0 * double(st.hits) / double(st.queries) : 0.0,
                st.insertFails);
        }
        std::printf("  drain     %.1f ms over %zu drains (%zu forced)   take %.1f   rering %.1f\n",
                    w.drainMs, w.drains, w.forcedDrains, w.takeMs, w.reringMs);
        std::printf("  read      %.1f ms over %zu compacted-size reads, worst %.2f ms\n",
                    w.compactReadMs, w.compactReads, w.compactReadWorst);
        std::printf("  rering    worst evict %.2f ms (%zu chunks evicted in all), worst ask %.2f ms\n",
                    w.evictWorst, w.evicted, w.askWorst);
        // Nonzero on a walk would mean a frame recorded far more than a frame
        // does -- see World::endDeviceFrameIfDue.
        std::printf("  devframe  %zu ended by the streamer itself: %zu loading, %zu in frames\n",
                    world_.deviceFramesForced(), world_.deviceFramesAtLoad(),
                    world_.deviceFramesForced() - world_.deviceFramesAtLoad());
        std::printf("  compact   %.0f MB built -> %.0f MB kept (%.0f%%), %zu still pending\n"
                    "  pools     %.0f MB in %zu recycled buffers\n",
                    w.uncompactedMb, w.compactedMb,
                    w.uncompactedMb > 0.0 ? 100.0 * w.compactedMb / w.uncompactedMb : 0.0,
                    w.pendingCompactions, w.poolMb, w.poolBuffers);
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // THE KEYS, AS ONE STRING, SO THE MENU AND THE CONSOLE CANNOT DISAGREE.
    //
    // (user 2026-09-19: "list all the keybinds in controls in the settings".)
    //
    // The settings panel had sliders and no keys, so the only place the
    // bindings existed was F1 -- a console the player has to alt-tab to. The
    // obvious fix is to type them into the panel as well, and the obvious fix
    // is how a list of forty keys goes stale: [G] changed meaning earlier
    // today and a second copy would still be calling it REFRESH.
    //
    // So there is one copy and two readers. printHelp prints it; the controls
    // tab draws it verbatim in the same monospace the console uses, which is
    // also why the columns line up there.
    //
    // NOT a table of {key, text} pairs: half these entries are three lines of
    // prose about what the key does and why, and a two-column table would
    // throw exactly the part worth reading.
    // -----------------------------------------------------------------------
    // THE SAME KEYS, WITH NOTHING SAID ABOUT THEM.
    //
    // (user 2026-09-20: "dont explain what the keybind do. it should be like
    //  caps lock = crouch.")
    //
    // A SECOND STRING, NOT A REPLACEMENT. keyHelpText below is what F1 and
    // the console print, and there the prose is the point: somebody reading
    // it has asked for help and wants to know that caps lock HELD goes prone,
    // that ctrl+G is not G, that R reloads with the rifle up. The settings
    // panel is not that -- it is a reference card, read at a glance by
    // somebody who already knows what crouching is.
    //
    // The two lists are the same keys in the same order, which is the only
    // thing about them that has to be kept in step by hand.
    // -----------------------------------------------------------------------
    static const char *keyBriefText() {
        return
            "left click   = look\n"
            "right-drag   = look, free\n"
            "w a s d      = walk\n"
            "shift        = sprint\n"
            "space        = jump\n"
            "caps lock    = crouch\n"
            "f            = fly\n"
            "x + wheel    = day speed\n"
            "y            = settings\n"
            "k            = stack count\n"
            "t            = console\n"
            "i            = asset editor\n"
            "g            = respawn\n"
            "ctrl+g       = refresh\n"
            "r            = record\n"
            "f1           = help\n"
            "f3           = coordinates\n"
            "o            = arcade\n"
            "esc          = free mouse\n";
    }

    static const char *keyHelpText() {
        return
            "\ncontrols:\n"
            "  click the window first  -- it needs focus, the console steals it on launch\n"
            "\n"
            "  left click            capture the mouse and look freely\n"
            "  right-drag            look around without capturing\n"
            "  W A S D               walk (hold shift to sprint)\n"
            "  space                 jump\n"
            "  caps lock             crouch -- and descend, in fly mode\n"
            "                        KEEP HOLDING IT to go PRONE; release and you rise\n"
            "                        back through the crouch to standing in one go\n"
            "  F                     toggle fly mode\n"
            "  arrow keys            scrub time (up/down = fast)\n"
            "  X + scroll wheel      day/night speed -- scroll down past 0.25x to REWIND\n"
            "  Y                     SETTINGS MENU\n"
            // The console had never been listed here at all, which is most of
            // why /locate needed telling about twice.
            "  K                     the stack count beside the hand -- size, place, tilt\n"
            "                        (it is the AMMO count with the rifle up)\n"
            "  T                     CONSOLE -- /locate <animal|biome|water> takes\n"
            "                        you to the nearest one, /where says where you\n"
            "                        are, /help lists them. ENTER runs, ESC cancels\n"
            "  I  or  U             ASSET EDITOR -- a deck in the sky, with the\n"
            "                        porcupine standing in the middle of it\n"
            "  G                     RESPAWN -- drops you in the next biome, and\n"
            "                        again for the one after that: pine, birch, oak,\n"
            "                        cherry, sand, round again\n"
            "  ctrl+G                REFRESH -- the wood as it was generated: every\n"
            "                        pit filled in, every tilled bed turned back,\n"
            "                        the life re-scattered. Not a rebuild -- a\n"
            "                        changed constant still wants one of those\n"
            "  F9                    RECORD -- press again to stop and save. Always\n"
            "                        the recorder, in every mode and with anything\n"
            "                        in your hands\n"
            "  R                     the same recorder -- but with a GUN in hand it\n"
            "                        RELOADS, so in the arcade record on F9 (ctrl+R\n"
            "                        works there too)\n"
            "  - / =                 exposure down / up\n"
            "  [ / ]                 bounces down / up\n"
            "  F1                    this help\n"
            "  F3                    coordinates -- x y z at your feet\n"
            // The water panel had this line and no longer has a key at all --
            // it is `--water-ui` now. See onKeyEvent, where L used to be.
            "  O                     NUKETOWN -- a level in its own sky, and the\n"
            "                        only place the assault rifle exists;\n"
            "                        press again to come back to the wood\n"
            "  ESC                   free the mouse -- again for the PAUSE BUTTONS,\n"
            "                        a third time to quit\n"
            "                        red quits, green returns, purple is Discord\n\n";
    }

    void printHelp() const {
        std::printf("%s", keyHelpText());
        // THE EDITOR'S OWN, out of the class that binds them, so this list
        // cannot go on describing a key after it has moved.
        std::printf("  ...and on the asset editor\'s deck:\n");
        int nh = 0;
        const char *const *hr = AssetEdit::help(&nh);
        for (int i = 0; i < nh; ++i) std::printf("%s\n", hr[i]);
        std::printf("\n");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // Write the live settings back out as src/core/defaults.h.
    //
    // The path comes from V2_SOURCE_DIR, baked in by the build, rather than
    // being derived from the working directory -- the launcher runs the exe
    // from C:\voxelbit, so anything relative would land in the wrong tree and
    // report success while writing nothing anyone would ever compile.
    // -----------------------------------------------------------------------
    std::string bakeDefaults() {
#ifndef V2_SOURCE_DIR
        return "bake unavailable: built without V2_SOURCE_DIR";
#else
        // V2_SOURCE_DIR is "<engine>/src", so the engine root -- and the
        // rebuild script the user is about to be told to run -- is one level up.
        // Derived rather than hardcoded so a copy of this tree elsewhere still
        // reports its own path.
        const std::string srcDir = V2_SOURCE_DIR;
        const std::string root =
            srcDir.size() > 4 ? srcDir.substr(0, srcDir.size() - 4) : srcDir;
        const std::string path = srcDir + "/core/defaults.h";
        char clockText[16];
        clock_.clock(clockText, sizeof(clockText));
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return "could not write " + path;

        std::fprintf(f,
            "// ---------------------------------------------------------------------------\n"
            "// defaults.h -- the settings v2 starts with.\n"
            "//\n"
            "// GENERATED FILE. Everything below is rewritten wholesale by \"Bake as\n"
            "// default\" in the in-viewer settings menu (Y), so hand edits survive only\n"
            "// until the next bake -- but hand edits are perfectly fine, the format is just\n"
            "// constants and the file is checked in.\n"
            "//\n"
            "// The point of it is that the settings menu and the command line stop being\n"
            "// separate universes: fly around, tune the picture until it looks right, bake,\n"
            "// rebuild, and the thing you tuned is what v2 opens with.\n"
            "// ---------------------------------------------------------------------------\n"
            "#pragma once\n"
            "\n"
            "namespace v2 {\n"
            "namespace defaults {\n"
            "\n"
            "constexpr float kScale = %.2ff;\n"
            "constexpr int kDepth = %d;\n"
            "constexpr int kMovingDepth = %d;\n"
            "constexpr float kExposure = %.2ff;\n"
            "constexpr float kShadowLift = %.3ff;\n"
            "constexpr float kSpeed = %.1ff;\n"
            "constexpr float kSensitivity = %.3ff;\n"
            "constexpr float kEye = %.2ff;\n"
            "constexpr float kFov = %.1ff;\n"
            "constexpr float kSunAz = %.1ff;\n"
            "constexpr float kSunEl = %.1ff;\n"
            "constexpr int kWidth = %d;\n"
            "constexpr int kHeight = %d;\n"
            "constexpr int kTrees = %d;\n"
            "constexpr float kTimeOfDay = %.6ff;  // %s\n"
            "constexpr float kCycleSpeed = %.2ff;\n"
            "constexpr bool kAtmosphere = %s;\n"
            "constexpr float kNightBrightness = %.2ff;\n"
            "constexpr bool kBlueNoise = %s;\n"
            "constexpr bool kAutoExposure = %s;\n"
            "constexpr float kBloom = %.2ff;\n"
            "\n"
            "// HOW LOUD THE WOOD IS, as a master gain over the ambience bed -- what\n"
            "// actually reaches the voice is this times the canopy closure at your feet.\n"
            "// 1.00 is the bed at the level it was baked; a quarter of that is a\n"
            "// background rather than a foreground, and it is where the Volume slider\n"
            "// sits at its MIDPOINT. Menu row \"Volume\", or --ambience.\n"
            "constexpr float kAmbience = %.2ff;\n"
            "\n"
            "// HOW MUCH THE THING IN YOUR HAND MOVES as you walk -- a gain over the\n"
            "// stride and the breath in render/helditem.h, not a speed and not a shape.\n"
            "// 1.00 is the look those constants describe, so 2.00 is twice it; 0 nails\n"
            "// the tool to its pose for a reference screenshot. No menu row -- this one\n"
            "// is --hand-sway and a bake, as kBloom and kAutoExposure are.\n"
            "constexpr float kHandSway = %.2ff;\n"
            "\n"
            "}  // namespace defaults\n"
            "}  // namespace v2\n",
            opt_.scale, opt_.r.maxDepth, opt_.movingDepth, opt_.r.exposure, opt_.r.shadowLift,
            player_.walk,
            opt_.sensitivity, player_.eye, fov_,
            // THE BASE, NOT THE DERIVED AZIMUTH. kSunAz is loaded straight
            // into clock_.azimuthBase, but this used to bake sunAz_, which is
            // what azimuthDeg() computed for the CURRENT hour -- so every bake
            // folded that hour's offset into the base and the sun walked east
            // a little further each time. Baking at 23:50 on 2026-09-06 moved
            // it from 6.9 to 95.7 in one go.
            clock_.azimuthBase, sunEl_,
            int(getTargetFbo()->getWidth()),
            int(getTargetFbo()->getHeight()), defaults::kTrees, clock_.tday, clockText,
            clock_.cycleSpeed, atmo_.enabled ? "true" : "false", nightLevel_,
            // BAKED FROM THE LIVE OBJECTS, not from opt_. The menu writes
            // straight to tracer_ and post(), so opt_ still holds whatever the
            // command line said at start-up -- baking that would quietly
            // discard the thing just tuned, which is the one job this has.
            tracer_.blueNoise ? "true" : "false",
            tracer_.post().autoExposure ? "true" : "false", tracer_.post().bloom,
            // opt_.ambience, WHICHEVER WAY THE SOUND WENT. This used to prefer
            // ambience_.masterGain() and fall back to the option, because the
            // slider wrote the voice and never the option -- and a bake under
            // --no-sound or --background would otherwise have written back the
            // 1.0 the object was constructed with, turning the wood up fourfold
            // for having tuned the picture in silence.
            //
            // THAT INVERTED WHEN THE MASTER FADER LANDED. masterGain() now holds
            // opt_.ambience TIMES opt_.volume (see applyVolumes), so reading it
            // would bake the room's own volume into the wood's level: turn the
            // game down to type, bake, and the bed is permanently quieter. The
            // option is the number the player actually set, in both states, so
            // there is no longer a fallback to choose between.
            opt_.ambience,
            // Straight off the live object, like the two above it: the menu
            // row writes into held_ and never into opt_, so opt_ still holds
            // whatever the command line said at start-up.
            held_.sway);
        std::fclose(f);
        // The FULL PATH, not just the file name. "run rebuild.bat" is only
        // useful if you already know which of the engine trees it lives in,
        // and the exe is launched from the repo root rather than from beside
        // its own source -- so the obvious place to look is the wrong one.
        return "baked. now run " + root + "/rebuild.bat";
#endif
    }
