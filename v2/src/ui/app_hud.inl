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
            st.FrameRounding = r * 0.4f;
            st.GrabRounding = r * 0.4f;
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
            st.GrabMinSize = 16.0f;
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
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_CheckMark, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_Button, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::kHot());
            ImGui::PushStyleColor(ImGuiCol_Header, ui::kBar());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::rgb(52, 70, 94));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::rgb(52, 70, 94));
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
    void makeCrosshair() {
        crosshair_ = Falcor::FullScreenPass::create(getDevice(), "v2/shaders/Crosshair.ps.slang");
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
        if (!crosshair_ || menuOpen_ || quitting_) return;

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
        crosshair_->execute(ctx, target);
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
        std::printf("  %7s %7s %7s %7s %7s %7s %7s %7s %7s %6s\n", "total", "stream", "blas",
                    "tlas", "rering", "drain", "phys", "life", "pub", "chunk");
        for (size_t i = 0; i < 10 && i < worst.size(); ++i) {
            const HitchFrame *h = worst[i];
            std::printf("  %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %6d\n",
                        double(h->total), double(h->stream), double(h->blas), double(h->tlas),
                        double(h->rering), double(h->drain), double(h->phys), double(h->life),
                        double(h->pub), h->adopted);
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
        if (Falcor::Profiler *prof = getDevice()->getProfiler()) {
            std::string line;
            for (Falcor::Profiler::Event *e : prof->getEvents()) {
                if (!e) continue;
                const std::string path = e->getName();
                // Falcor names nested events by their path; the leaf is enough.
                const size_t slash = path.find_last_of('/');
                const std::string leaf =
                    slash == std::string::npos ? path : path.substr(slash + 1);
                // "probes" and "fog" were being COLLECTED and then dropped here, so
                // every profile this engine has ever printed was silent about the
                // DDGI probe trace and about the fog volume.
                if (leaf != "probes" && leaf != "trace" && leaf != "fog" &&
                    leaf != "reconstruct" && leaf != "tonemap")
                    continue;
                line += fmt("   %s %.2f ms", leaf.c_str(), e->getGpuTimeAverage());
            }
            if (!line.empty()) std::printf("  gpu      %s\n", line.c_str());
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
        std::printf("  compact   %.0f MB built -> %.0f MB kept (%.0f%%), %zu still pending\n"
                    "  pools     %.0f MB in %zu recycled buffers\n",
                    w.uncompactedMb, w.compactedMb,
                    w.uncompactedMb > 0.0 ? 100.0 * w.compactedMb / w.uncompactedMb : 0.0,
                    w.pendingCompactions, w.poolMb, w.poolBuffers);
        std::fflush(stdout);
    }

    void printHelp() const {
        std::printf(
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
            "  G                     REFRESH -- the wood as it was generated: every\n"
            "                        pit filled in, every tilled bed turned back,\n"
            "                        the life re-scattered. Not a rebuild -- a\n"
            "                        changed constant still wants one of those\n"
            "  R                     RECORD -- press again to stop and save. With the\n"
            "                        ASSAULT RIFLE in hand it RELOADS instead;\n"
            "                        ctrl+R is always the recorder\n"
            "  - / =                 exposure down / up\n"
            "  [ / ]                 bounces down / up\n"
            "  P                     screenshot            F1   this help\n"
            // The water panel had this line and no longer has a key at all --
            // it is `--water-ui` now. See onKeyEvent, where L used to be.
            "  O                     NUKETOWN -- a level in its own sky, and the\n"
            "                        only place the assault rifle exists;\n"
            "                        press again to come back to the wood\n"
            "  ESC                   free the mouse -- again for the PAUSE BUTTONS,\n"
            "                        a third time to quit\n"
            "                        red quits, green returns, purple is Discord\n\n");
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
            "constexpr float kTimeOfDay = %.4ff;  // %s\n"
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
            // THE LIVE GAIN WHERE THERE IS ONE, and the option otherwise. A bake
            // under --no-sound or --background never opened the bed, so masterGain()
            // is the 1.0 the object was constructed with rather than anything anybody
            // chose -- and baking that would turn the wood up fourfold for having
            // tuned the picture with the sound off.
            ambience_.active() ? ambience_.masterGain() : opt_.ambience,
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
