// app_console.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: the console command parser
// -----------------------------------------------------------------------------
    // Returns the reply to show. Never throws; an unknown command is a message,
    // not a failure.
    std::string runCommand(std::string line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '/')) line.erase(line.begin());
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (line.empty()) return std::string();

        std::string verb = line, arg;
        const size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            verb = line.substr(0, sp);
            arg = line.substr(sp + 1);
            while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
        }
        for (char &c : verb) c = char(tolower((unsigned char)c));
        for (char &c : arg) c = char(tolower((unsigned char)c));

        if (verb == "locate") {
            if (arg.empty()) return locateMenu("locate what?");
            // WATER IS NOT A BIOME, so it is not a row in that table -- it is
            // a feature of the landform inside one. Handled before the band
            // loop, and it works in a pinned world too, unlike the bands.
            if (arg == "water" || arg == "lake") {
                float wx = 0.0f, wz = 0.0f;
                if (!nearestWater(&wx, &wz))
                    // NOT "the birch wood has none at all" any more: that was
                    // true when birchWater was kNoWater and has not been since
                    // the lakes landed. All three woods are wet now -- 5.9% of
                    // the pine, 7.6% of the birch, 7.5% of the oak -- so a miss
                    // here means you are between basins, not in a dry wood.
                    return std::string("no water within 6 km -- lakes sit in basins, "
                                       "and you are between them; walk on or try "
                                       "/locate <wood>");
                teleportTo(wx, wz);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "the shore -- %.0f, %.0f", wx, wz);
                return std::string(buf);
            }
            for (const BiomeName &bn : biomeNames()) {
                if (arg != bn.name && arg != bn.alias) continue;
                // --birch and --pine pin the world to one wood, so there is no
                // other band to travel to. Say so rather than teleporting to a
                // place that is the same as this one.
                if (world_.terrain.forced) {
                    return std::string("the world is pinned to one wood "
                                       "(--birch / --pine / --oak) -- restart without it to "
                                       "walk between them");
                }
                const float tx = nearestBandX(bn.biome);
                teleportTo(tx, pos_.z);
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s forest -- %.0f, %.0f", bn.name, tx, pos_.z);
                return std::string(buf);
            }
            // ---- ...AND THE LIFE ------------------------------------------
            for (const LifeName &ln : lifeNames())
                if (arg == ln.name || arg == ln.alias) return locateLife(ln);
            return locateMenu("nothing called '" + arg + "'.");
        }
        if (verb == "where") {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%.0f, %.0f, %.0f -- the %s wood", pos_.x, pos_.y,
                          pos_.z,
                          world_.terrain.woodName(pos_.x));
            return std::string(buf);
        }
        if (verb == "help")
            return std::string("/locate <biome|water>   /where   "
                               "ENTER runs and closes   ESC cancels\n"
                               "/locate <animal> takes you to the nearest one:\n") +
                   lifeList("  ", 7);
        return std::string("unknown command '" + verb + "' -- try /help");
    }

    // THE SAME TRADE THE MENU MAKES: while a panel is up the mouse belongs to
    // it, and the look is handed back on close only if it was ours to begin
    // with. Kept separate from setMenuOpen so the two panels cannot fight over
    // who restores the capture.
    // -----------------------------------------------------------------------
    // INTO THE ROOM AND BACK OUT, which is the editor deck's own trip -- see
    // the U key. The wood's position, heading and flight are remembered on the
    // way in and put back on the way out, because a pause that moves you is not
    // a pause.
    //
    // THE MOUSE IS TAKEN, NOT HANDED BACK, which is the opposite of what every
    // other panel in this file does and is the right call here. The buttons are
    // picked by the CROSSHAIR -- see buttonUnderCrosshair -- and the outer two
    // sit twenty degrees off centre, so a player who cannot turn their head can
    // only ever press the green one. A settings panel is a page you point at; a
    // room is a place you look around in.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE WORDS OVER THE BUTTONS.
    //
    // "quit", "back" and "discord", standing in the air a hand's width above
    // the button each one names -- see V2Holo in Shared.slang for what they are
    // and render/holotext.h for the face they are written in.
    //
    // IN THE ROOM, NOT ON THE SCREEN. This is the whole point of them and it is
    // worth saying next to the code: an ImGui label over each button would have
    // been four lines and would have been WRONG, because the pause room is a
    // place the player is standing in and text pasted on the glass is not in it.
    // The plane these are drawn on has world coordinates, so it has parallax
    // against the wall behind it, the bulb's own glare passes in front of it,
    // and it reprojects for Ray Reconstruction like every other surface. v1 made
    // the same call about the stack badge beside the held tool -- its note reads
    // "drawn INTO the image (user: not HTML in the corner)" -- and could only
    // fake the perspective with a tilt and a shear, because a raymarcher that
    // shades one hit has nowhere to put a second surface. A path tracer does.
    //
    // THE COLOUR HAD TO BE MEASURED, AND THE FIRST TWO GUESSES WERE THE SAME
    // MISTAKE. In the room the walls were white at 226 lit to a mean of 180, so
    // a label had to be BRIGHTER THAN THE WALL to read at all -- paint of any
    // colour on a white wall under one lamp comes out a grey. Both early cuts
    // just turned the number up, and both rendered as WHITE WORDS WITH A
    // COLOURED FRINGE.
    //
    // What was wrong is not the level, it is which channels got it. kBtnRgb is
    // an sRGB colour, so the way to make a radiance out of it is to LINEARISE it
    // -- and sRGB is a steep curve down low, so (214, 58, 58) is not
    // (1.00, 0.27, 0.27) of light but (1.00, 0.06, 0.06). Scaling the encoded
    // numbers had been handing the off-channels four times too much, and up on
    // the tone map's shoulder the top channel is compressed hard while those
    // off-channels are not, so the three of them converge and the hue is
    // squeezed out. That is the whole reason the level below is applied to a
    // LINEARISED colour and normalised on its brightest channel.
    //
    // -- AND THE LEVEL ITSELF WENT UP WHEN THE ROOM CAME DOWN ---------------
    //
    // 2.0 was measured against a wall. The panel stands in the open now and the
    // thing behind a word is usually SKY, which the tone map puts far higher
    // than any interior -- so the same ink that beat a wall lost to a cloud.
    // Measured off --room --spawn 7, ink luma against the luma right beside it:
    //
    //     gain   quit          back          discord       saturation
    //     2.0    +4            +30           +63           0.23 / 0.17 / 0.26
    //     4.0    +17           +34           +85           0.18 / 0.13 / 0.19
    //     8.0    +31           +27           +65           0.12 / 0.06 / 0.17
    //     16.0   +41           +38           +104          0.07 / 0.05 / 0.09
    //
    // GREEN IS THE ONE THAT DECIDES IT. A saturated green is the highest-luma
    // hue there is -- 0.7152 of the luma weight is in that channel -- so "back"
    // was rendering at 205 against a sky at 212 and simply was not there. It is
    // also the first word to go white as the level rises, and by 8.0 it has
    // (saturation 0.06, which is a grey).
    //
    // 4.0 IS WHERE BOTH HOLD: every word clears its background, and all three
    // still read as their button's colour rather than as white. Past that the
    // contrast column barely moves and the saturation column falls off a cliff,
    // which is the same failure the paragraph above describes arriving by a
    // different road.
    static constexpr float kLabelNits = 4.0f;
    //
    // THE BUTTON'S OWN COLOUR, out of World::kBtnRgb, so a label can never name
    // the wrong button.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // HOW MANY OF THE THING IN YOUR HAND, WRITTEN BESIDE IT.
    //
    // (user 2026-09-14: "import the stacked item numbers next to the hand held
    // object. it should be setup similar to the current main menu floating
    // text.")
    //
    // THE SAME GLYPHS THE ROOM'S LABELS USE, which is what that sentence asks
    // for and is also the cheap answer: V2Holo already draws world-space text
    // through the tracer, so this is one slot and one call. v1 does it the
    // other way -- a badge blitted at the held model's projected corner, with
    // its own lane in the uniform buffer and its own glyph table in the blit
    // shader -- and none of that machinery has to exist here.
    //
    // WHERE IT GOES. The held item is a VIEWMODEL: it lives in camera space and
    // has no world position of its own. HeldXform::cam is its offset along the
    // camera's three axes, which is exactly what the Q drop already converts to
    // a world point, so the badge is that point pushed out along camRight.
    //
    // ...AND IT FACES THE CAMERA BECAUSE ITS PAGE IS THE CAMERA'S. right and up
    // are camRight/camUp, so the number is square to the eye at every angle --
    // a label on the panel is on a wall and this one is not.
    //
    // HIDDEN BELOW TWO. "x1" next to an axe is noise, and it is the state
    // almost everything in the kit is in almost always.
    // -----------------------------------------------------------------------
    // -- ...AND ALL FOUR OF THEM ARE LIVE (user 2026-09-14: "let me adjust the
    //    display number just like how we were able to in v1") ---------------
    //
    // v1 tunes this badge from a panel with exactly four sliders -- SB_K is
    // ['x', 'y', 'size', 'tilt'] -- and these are the same four in world terms:
    // across the view, up it, the glyph cell, and a roll about the view axis.
    //
    // NOT `static constexpr` ANY MORE, and that is the whole change: a number
    // you can only edit by rebuilding is not one you can judge, because the
    // thing you are judging is how it sits next to a model that is bobbing.
    // They start where they were tuned, so a session that never opens the panel
    // sees what a session before it did.
    //
    // HALF THE SIZE IT WAS (same message): 0.022 -> 0.011. v1's default `size`
    // is 1 against a badge the blit draws at a fixed pixel scale, so there is
    // no number to carry over -- this one was chosen by eye against a 0.9 m
    // tool and was chosen too big.
    // -- AND THEY ARE PER ITEM NOW (user 2026-09-14) -----------------------
    //
    // The note that stood here said this was deliberately NOT per item: "this
    // one hangs off the model's own measured half-width, so the same four
    // numbers frame every tool in the kit. If one item ever needs its own, the
    // shape of it is v1's sbCfgs table."
    //
    // TWO DID, IN THE SAME MESSAGE. The seed and the wheat came back with
    // different numbers -- the seed up 0.205 and the wheat up 0.313 and ACROSS
    // -0.145, on the other side of the model. The half-width is a box, and a
    // stalk held upright and a handful of seed are not the same shape inside
    // one; no single offset frames both.
    //
    // A ROW PER TOOL, defaulted to the base. That is v1's sbCfgs exactly, and
    // its own fallback is the same: an id with nothing of its own reads the
    // base rather than minting an entry from a render loop.
    struct StackCfg {
        float cell = 0.011f;    // glyph cell, in metres
        float across = 0.100f;  // clear of the model's right edge
        float up = 0.060f;      // ...and up from its middle
        float tilt = 0.000f;    // a roll about the view axis, radians
    };
    std::vector<StackCfg> stackCfg_;

    // -----------------------------------------------------------------------
    // THE BAKED ROWS -- what the panel's tuning looks like once it has landed.
    //
    // (user 2026-09-14, giving both directly: "seed position: ... stackUpM_ =
    // 0.205f ..." and "wheat position: ... stackAcrossM_ = -0.145f, stackUpM_ =
    // 0.313f".)
    //
    // KEYED BY NAME, NOT BY SLOT. A slot is where a thing happens to sit in the
    // wheel in this build -- the wheat moved from 4 to 5 when the hoe was added
    // -- and a bake keyed by that would silently start framing the bow. v1
    // keys sbCfgs by ITEM_NAMES for the same reason.
    //
    // ANYTHING NOT LISTED TAKES StackCfg's OWN DEFAULTS, which is every tool:
    // a tool is never held in a stack of two, so its row would never be seen.
    // Only things you can gather need one.
    struct StackBake {
        const char *name;
        StackCfg cfg;
    };
