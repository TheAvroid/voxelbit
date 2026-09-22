// app_room.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: pause room, stage, level, panel and buttons
// -----------------------------------------------------------------------------
    static const std::vector<StackBake> &stackBakes() {
        static const std::vector<StackBake> t = {
            //          cell     across      up      tilt
            {"seeds", {0.0110f, 0.100f, 0.205f, 0.000f}},
            {"wheat", {0.0110f, -0.145f, 0.313f, 0.000f}},
            {"steak", {0.0110f, 0.072f, 0.349f, -0.053f}},   // re-baked 2026-09-19
            // THE ARROW CARRIES A COUNT TOO -- baked 2026-09-20 off the [K]
            // card, the same way every other row here was.
            {"arrow", {0.0110f, 0.211f, 0.400f, 0.000f}},
            // THE TWO FRUIT, OFF THE [K] CARD, and one line serves both for the
            // same reason one HeldPose does: they are the same shape at the
            // same scale, so a badge placed against one is placed against the
            // other. See the apple/orange registration in app_load.inl.
            {"apple", {0.0110f, 0.111f, 0.258f, 0.000f}},
            {"orange", {0.0110f, 0.111f, 0.258f, 0.000f}},
            // -- THE AMMO COUNT, WHICH IS THIS TABLE'S FIRST NON-STACK ------
            //
            // (user 2026-09-18: "make sure to let me adjust the position of the
            // ammo text.")
            //
            // AND THE ANSWER TO THAT ASK IS THIS ROW PLUS THE [K] CARD that was
            // already there: the gun's badge is drawn by setStackBadge like
            // every other, so the four sliders on that card are its four
            // numbers, and "bake" copies the line to paste back in here. There
            // was nothing to build.
            //
            // IT CANNOT TAKE THE DEFAULTS. Everything else in this table is
            // held near the middle of the view; the rifle's anchor is 72 cm to
            // the RIGHT of the eye and 91 cm in front of it, which at this
            // engine's field of view is most of the way to the edge of the
            // frame already. StackCfg's own `across` of +0.100 puts the number
            // past it -- a badge nobody can see, which reads as a badge that is
            // not being drawn.
            //
            // So it goes back the other way -- LEFT, along the view, and only
            // a little up. Bigger than a stack count too: this one is read
            // while something is shooting back.
            //
            // THE TWO WRONG ANSWERS, BOTH RENDERED, because the trade here is
            // not obvious and the second one looks like the fix for the first:
            //
            //   across -0.450, up 0.250 -- reads perfectly at rest and the
            //     RECEIVER EATS IT under automatic fire. The gun is thrown 9 cm
            //     up and 22 cm back a round (kRecoilUpVox, kRecoilBackVox), and
            //     coming 22 cm nearer the eye is what does it: the badge is at
            //     the gun MID-DEPTH, so the model swells around it.
            //   across -0.500, up 0.400 -- clears the gun in every pose, by
            //     lifting the number off the grass and ONTO THE SKY, where
            //     white ink at kStackNits is a ghost. The room labels already
            //     carry this measurement: "the same ink that beat a wall lost
            //     to a cloud". A badge you cannot read is not a badge.
            //
            // LEFT rather than UP is what satisfies both. At -0.650 the number
            // is left of the gun s far end -- which is its leftmost edge on
            // screen, and the only part of it that reaches this far in -- and
            // stays over the grass, where the ink wins.
            // THE USER'S BAKE (2026-09-18), off the [K] card. It is back on the
            // RIGHT of the hand -- across is positive again -- and higher than
            // either of the two cuts above; the sky note still applies to
            // anything much taller than this.
            {"assault rifle", {0.0150f, 0.145f, 0.338f, 0.000f}},
            // THE PISTOL'S IS THE USER'S BAKE (2026-09-18), off the [K] card,
            // and it sits closer in and lower than anything derived would have
            // put it. What was derived first, and why it is worth keeping the
            // reasoning even though the numbers moved:
            //
            //   across  is an offset from the model's OWN half width (0.5 * sx *
            //           VOXEL_M), and the pistol's shared box is 5 voxels wide
            //           against the rifle's 3, so equal numbers are not equal
            //           places. 0.045 was tried and left the badge 4.5 cm off
            //           the slide -- at 0.9 m the model ate the left half of the
            //           glyph and it rendered as three white bars.
            //   up      has to clear the gun's own BOX, not match the rifle's
            //           height: 0.238 put it a centimetre UNDER the top of the
            //           pistol's box and the near block cut the corner off it.
            //
            // If a number here ever renders as part of a glyph, it is one of
            // those two collisions and not a font bug.
            {"pistol", {0.0150f, 0.028f, 0.272f, 0.000f}},
        };
        return t;
    }

    // Applied once, after the kit is built -- the only moment every tool has a
    // name AND a slot. A name in the table that is not in the kit is ignored
    // rather than an error: the table outlives any one build of the kit.
    void applyStackBakes() {
        stackCfg_.assign(size_t(held_.count()), StackCfg{});
        for (const StackBake &b : stackBakes())
            for (int i = 0; i < held_.count(); ++i)
                if (std::string(held_.tool(i).name) == b.name) stackCfg_[size_t(i)] = b.cfg;
    }

    // The row for a tool, minted on first use. Never called from the render
    // loop with an id the kit does not have -- see stackOf below, which is.
    StackCfg &stackFor(int tool) {
        if (stackCfg_.size() < size_t(held_.count())) stackCfg_.resize(size_t(held_.count()));
        return stackCfg_[size_t(tool)];
    }
    // ...and the read-only side, which IS called from the render loop.
    const StackCfg &stackOf(int tool) const {
        static const StackCfg kBase;
        return (tool >= 0 && size_t(tool) < stackCfg_.size()) ? stackCfg_[size_t(tool)] : kBase;
    }
    // NOT a slider: how bright a label is was settled for the room's own
    // words and there is no reason for this one to differ. See kLabelNits.
    static constexpr float kStackNits = 3.0f;
    // How long the badge's pop runs and how far it swells at the top of it.
    // A quarter of a second is about as long as a flourish can be before it
    // starts reading as the number being wrong; 45% is enough to catch the eye
    // at the edge of vision, which is where the hand is.
    static constexpr float kStackPopSec = 0.25f;
    static constexpr float kStackPopSwell = 0.45f;

    void setStackBadge() {
        V2Holo &h = tracer_.holo[kHoloHand];
        h = V2Holo{};
        // NOTHING IN THE ROOM. The panel owns the screen there and a number
        // hanging in front of it is the hand intruding on a menu.
        if (pauseOpen_ || menuOpen_ || !held_.ready() || !held_.shown) return;
        // -- THE GUN ON SCREEN, NOT THE ONE ON ORDER --------------------
        //
        // (user 2026-09-19: "when switching weapons in the fps mode, the ammo
        //  count number doesnt transition correctly over to the next gun".)
        //
        // A SWAP IS TWO PHASES and `selected()` is already the SECOND gun for
        // the whole of the first -- see HeldItem::drawnTool, which is what the
        // model and the transform have used since the swap animation landed.
        // This line did not move with them, so for the 130 ms the rifle spends
        // being lowered the number beside it was the PISTOL's six, hanging off
        // the rifle at the pistol's badge offset (stackOf(sel) below is keyed
        // on the same index).
        //
        // Reading the drawn tool makes the count a property of the thing you
        // can see: twenty rides the rifle down, six comes up with the pistol.
        const int sel = held_.drawnTool();
        if (sel < 0) return;
        const Tool &t = held_.tool(sel);
        // -- THE GUN COUNTS ROUNDS, NOT COPIES OF ITSELF --------------------
        //
        // (user 2026-09-18: "put a number next to the assault rifle just like
        // the stacked number on hand held items. this is to count the guns
        // ammo.")
        //
        // THE SAME BADGE, A DIFFERENT NUMBER -- and "the same badge" is the
        // whole of the ask. It is drawn by the same call, in the same glyphs,
        // out of the same per-tool row of four numbers, so the [K] card that
        // places a stack count places this too and there is nothing new to
        // tune, bake or keep in step. What differs is three lines: where the
        // number comes from, that it has no "x" in front of it, and that it is
        // shown at one and at zero.
        //
        // AN AMMO COUNT IS NOT A STACK COUNT and the two rules it does not
        // share say why. A stack hides below two, because "x1" beside an axe is
        // noise -- but a gun with ONE round left is the most worth saying. And
        // a stack turns gold at its cap; a full magazine is the ordinary state
        // of a gun and colouring it would make twenty the thing that catches
        // the eye rather than two.
        // EITHER GUN (user 2026-09-18, the pistol). isGun and gunAmmoOf are the
        // same two lookups the trigger uses, so the number beside the hand and
        // the number the trigger spends can never be different numbers.
        const bool gun = isGun(sel);
        // FORCED WHILE THE PANEL IS OPEN, so there is something to aim the
        // sliders at -- almost everything in the kit sits at one. A gun needs
        // no such help: its number is always up.
        const int n = gun ? gunAmmoOf(sel)
                          : ((stackPanelOpen_ && stackPanelForce_) ? maxi(2, t.stack) : t.stack);
        // -- THE POP IS ARMED ABOVE THE BADGE'S OWN CUT-OFF -----------------
        //
        // (user 2026-09-15: "have the stack 'pop up' as well. a smooth pop up
        // animation".)
        //
        // A BADGE ONLY EXISTS FROM TWO UP, and that is exactly why this cannot
        // live below the return. Tracked there, the count 1 is never seen --
        // so picking up the SECOND of something, which is the moment the number
        // first appears and the most worth announcing, would arrive as "a tool
        // I have no previous count for" and pass in silence.
        //
        // NOT ON THE FIRST SIGHT OF A TOOL, though: selecting a stack you were
        // already carrying is not a change to it, and popping there made every
        // scroll of the kit twitch. So the tool has to match as well as the
        // count having moved.
        // -- ...AND THE GUN ONLY POPS ON THE WAY UP -------------------------
        //
        // A stack pops on any change, because every change to one is an event:
        // you picked something up, or ate it. A magazine changes five times a
        // second while the trigger is down, and a badge that swells on every
        // round is a badge that is never still -- the flourish stops reading as
        // "look at this" and starts reading as a rendering fault.
        //
        // SO IT POPS WHEN THE ROUNDS COME BACK, which is the one moment worth
        // announcing and the moment the player is waiting on: 0 -> 20 at the
        // end of the reload, and nothing on the way down.
        if (n != stackPopN_ || sel != stackPopTool_) {
            if (n != stackPopN_ && sel == stackPopTool_ && (!gun || n > stackPopN_))
                stackPopT0_ = simMs_;
            stackPopN_ = n;
            stackPopTool_ = sel;
        }
        if (!t.carried) return;
        // SHOWN AT ONE AND AT ZERO FOR A GUN -- see the block above. An empty
        // magazine reading "0" for the length of the reload is the animation's
        // own caption, and it is how a player knows the gun is busy rather than
        // broken.
        if (!gun && n < 2) return;

        char word[8];
        // TWO CALLS RATHER THAN ONE WITH A CHOSEN FORMAT: a format string that
        // is not a literal is a /W4 warning in this build, and this tree
        // compiles warnings as errors.
        if (gun)
            std::snprintf(word, sizeof(word), "%d", n);
        else
            std::snprintf(word, sizeof(word), "x%d", n);
        // THE HAND'S OWN WORLD POINT -- the same three terms the Q drop uses.
        const Vec3 rt = camRight(), up = camUp(), fw = forward();
        const Vec3 hand =
            pos_ + rt * lastHeld_.cam.x + up * lastHeld_.cam.y + fw * lastHeld_.cam.z;
        // Half the model's own width, so a long item does not wear its number
        // over itself. The box is in the tool's voxels; VOXEL_M turns it into
        // metres, and the pose scale is already in lastHeld_.
        const StackCfg &cfg = stackOf(sel);
        // -- ...AND IT POPS WHEN IT CHANGES ---------------------------------
        //
        // (user 2026-09-15: "have the stack 'pop up' as well. a smooth pop up
        // animation".)
        //
        // A NUMBER THAT CHANGES SILENTLY IS A NUMBER YOU MISS. Picking up the
        // sixth of something rewrites one glyph in the corner of the hand and
        // nothing about the frame says it happened -- the same complaint the
        // gold at ten answered, one step earlier.
        //
        // ARMED ON THE CHANGE, NOT ON THE PICKUP, so every route to the count
        // is covered by construction: a walk-over absorb, a break that pays
        // out, a craft, a bite taken out of a stack of meat. There is no list
        // of ways to gain an item to keep in step with, because this watches
        // the number itself.
        //
        // sin(pi*u) IS THE CURVE: it leaves 1.0, reaches the full swell at the
        // halfway point and comes back to 1.0, so the badge never steps -- and
        // its slope at u=0 is steep, which is what separates a POP from a
        // throb. Scaling the CELL is the whole of it: holoSetWord derives the
        // word's origin from the cell, so the number grows about its own middle
        // rather than sliding off the hand.
        float cellM = cfg.cell;
        {
            const float u = float((simMs_ - stackPopT0_) * 0.001) / kStackPopSec;
            if (u >= 0.0f && u < 1.0f) cellM *= 1.0f + kStackPopSwell * sinf(PI * u);
        }
        const float halfW = 0.5f * float(t.sx) * VOXEL_M + cfg.across;
        const Vec3 at = hand + rt * halfW + up * cfg.up;
        // THE TILT IS A ROLL OF THE PAGE, not of the glyphs: holoSetWord takes
        // the two axes the word is laid out on, so turning both of them in
        // their own plane turns the whole number. v1's `tilt` is the same
        // quantity -- its default is -0.26, a slight lean, and this starts at
        // zero because a world-space label has the model's own perspective on
        // it already.
        const float c = cosf(cfg.tilt), sn = sinf(cfg.tilt);
        const Vec3 rt2 = rt * c + up * sn;
        const Vec3 up2 = up * c - rt * sn;
        // -- GOLD AT THE CAP (user 2026-09-15: "have 10x be the max stacked
        //    item number just like in v1. make the text turn gold when its
        //    maxed at 10") ------------------------------------------------
        //
        // The cap was already v1's ten -- HeldItem::kStackMax, and HeldItem::take
        // clamps to it -- so the only thing missing was being TOLD. A number
        // that stops climbing and says nothing reads as a bug in the counter.
        //
        // The same warm gold the sparks settled on, at the label's own
        // brightness: this is a colour, not a second light.
        // A FULL MAGAZINE IS NOT A CAPPED STACK. The gold says "this will not
        // go any higher", which is news about a stack and is the normal state
        // of a gun -- see the block above.
        const bool full = !gun && n >= HeldItem::kStackMax;
        // -- AND THE GUN S NUMBER IS BRIGHTER --------------------------------
        //
        // kStackNits is 3.0, which was settled for a number that hangs beside
        // the hand over the ground. An ammo count follows the gun wherever it
        // is pointed -- up a wall, into a doorway, at the SKY -- and the room
        // labels have already measured what happens to white ink up there:
        // "the same ink that beat a wall lost to a cloud". kLabelNits is 4.0
        // for exactly that reason and is the right lamp for this one too.
        const float nits = gun ? kLabelNits : kStackNits;
        const float3 tint = full ? float3(nits * 1.00f, nits * 0.78f, nits * 0.22f)
                                 : float3(nits, nits, nits);
        holoSetWord(h, word, float3(at.x, at.y, at.z), float3(rt2.x, rt2.y, rt2.z),
                    float3(up2.x, up2.y, up2.z), cellM, tint);
    }

    void setRoomLabels() {
        for (int b = 0; b < kButtonSlots; ++b) setRoomLabel(b, World::buttonLabel(b));
    }

    // ONE WORD OVER ONE BUTTON, in that button's own colour. Split out of
    // setRoomLabels so the green one can say something other than "back" while
    // it waits -- see leaveRoom.
    void setRoomLabel(int b, const char *word) {
        if (b < 0 || b >= kButtonSlots) return;
        // Normalised on its brightest channel and then scaled: the three
        // colours were chosen as surfaces and have quite different lumas, so
        // scaling them as they stand would make "back" half the brightness of
        // "quit" for no reason anybody could see.
        float lin[3], m = 0.0f;
        for (int k = 0; k < 3; ++k) {
            lin[k] = srgbToLinearF(float(World::kBtnRgb[b][k]) / 255.0f);
            m = maxf(m, lin[k]);
        }
        const float s = (m > 0.0f) ? (kLabelNits / m) : 0.0f;
        const float3 tint(lin[0] * s, lin[1] * s, lin[2] * s);
        // ALL THREE OFF THE PANEL, so a word cannot end up facing a way its
        // button is not. The page is read along the panel and stands up it.
        const Vec3 mid = panelLabelMid(b);
        const Vec3 rt = panelRight_;
        const Vec3 up = panelUp_;
        holoSetWord(tracer_.holo[b], word, float3(mid.x, mid.y, mid.z),
                    float3(rt.x, rt.y, rt.z), float3(up.x, up.y, up.z),
                    World::kBtnLabelCellM, tint);
    }

    // Back to the wood from the asset deck: the position, heading and flight
    // saved on the way in. Extracted because the pause room has to be able to
    // do it too -- see the note at the editor's key.
    // -----------------------------------------------------------------------
    // WHAT STANDS ON THE DECK.
    //
    // THE PORCUPINE HAS THE MIDDLE (user 2026-09-14: "put the porcupine on the
    // asset editor in the middle and remove the bunny from it"), and it is not
    // standing there to be looked at -- the editor's tools operate on it.
    // WHICH animal that is lives in one table, kEditSubjects in
    // render/assetedit.h; this function only clears the deck and lets the
    // editor publish onto it.
    //
    // THE BUNNY IS GONE FROM IT, and the despawn below is most of how: the
    // wild rabbits were already cleared here, and with the editor no longer
    // publishing one either, there is no rabbit left on the deck at all. Its
    // STRIPS are still compiled and still played -- the rabbits in the wood hop
    // out of them -- they are simply not a subject this deck offers now.
    //
    // THE CARDINAL STEPPED ASIDE RATHER THAN OUT when the deck first grew a
    // subject; it has since been removed outright (2026-09-13). See the body.
    //
    // AND EVERY ANIMAL LETS GO OF ITS SLOTS. The editor draws through
    // kBunnySlot0, so the ten rabbits that were following you round the pines
    // have to be cleared or nine keep standing wherever they were -- four
    // kilometres away and, on this deck's coordinates, under the floor.
    //
    // THE MARCHERS NOW MATTER TWICE OVER. Their band was already cleared for
    // that reason; it also has to be, because the porcupine the editor stands
    // in the middle is drawn from the SAME STRIP the wild ones walk on. Leave
    // them live and the deck holds two porcupines out of one animation, only
    // one of which is posed by the bake you are editing.
    // update() stops ticking them while the stage is up, so nothing refills.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHERE YOU ARRIVE ON THE DECK, AND WHY IT IS NOT STRAIGHT ON.
    //
    // It used to be three metres down +Z with yaw 0 -- square to the subject,
    // for the sound reason that yaw 0 looks down -Z here (Camera::direction) so
    // standing on the other side put it behind the camera. That reason is
    // untouched; what changed is that there are HANDLES on the deck now, and a
    // square-on camera is the one angle from which a three-axis gizmo does not
    // work.
    //
    // A TRANSLATE ARROW IS DRAGGED ALONG ITS SCREEN DIRECTION, so an axis
    // pointing straight at the eye has no screen direction and cannot be
    // pulled at all. At yaw 0 that is the Z arrow -- and Z is the axis a bound
    // travels along, so the handle you reach for first was the dead one. The
    // pitch ring was edge-on for the same reason, a violet bar rather than a
    // ring. Both were photographed before this changed.
    //
    // A THREE-QUARTER VIEW IS THE STANDARD ANSWER and every 3D tool opens on
    // one. Off the corner, all three axes have a screen direction and both
    // rings read as rings.
    //
    // THE AIM IS COMPUTED, NOT TYPED. The eye is two metres over the body it is
    // carried by and the subject is a rabbit on the floor, so a hand-picked
    // pitch is a guess that has to be re-guessed the moment anything moves. Ask
    // for the angles that point at the subject and they are right by
    // construction -- which is also what makes the eye height below the only
    // number here worth choosing by eye.
    // -----------------------------------------------------------------------
    void standOnDeck() {
        const Vec3 c = World::stageCentre();
        // A COUPLE OF METRES OFF EACH OF THE TWO NEAR CORNERS, which is what
        // gives the three-quarter view a gizmo needs: at yaw 0 the Z arrow
        // points straight at the eye and cannot be dragged at all.
        const float back = 2.35f;
        // -- ON ITS FEET, NOT HOVERING --------------------------------------
        //
        // "let the player stand on the asset editor platform." It used to
        // arrive FLYING, and not as a choice: the walk collider reads the
        // terrain, the terrain under the deck is 640 m down, and a walking
        // body would simply have fallen off the world. Flight was the only
        // thing holding anybody up. See clampToStage, which is the floor --
        // and the fence -- that makes standing possible.
        //
        // THE EYE HEIGHT IS NO LONGER A COMPOSITION. It was 1.30 m, measured
        // off a render because the deck's own horizon ran through the rabbit at
        // 0.95; it is now the player's own 2.00 m, because a person standing on
        // a platform is exactly what was asked for. The subject sits lower in
        // the frame than it did, which is what looking down at something on the
        // floor looks like -- and the aim below still points at the middle of
        // it rather than at the deck.
        player_.pos = Vec3(c.x - back, c.y, c.z + back);
        player_.fly = false;
        player_.onGround = true;
        player_.vy = 0.0f;
        pos_ = player_.eyePosition();
        // The middle of the rabbit rather than its feet, so it sits on the
        // crosshair instead of under it.
        lookAt(Vec3(c.x, c.y + 0.25f, c.z));
    }

    void stageSubject() {
        const Vec3 c = World::stageCentre();
        bunnies_.despawnAll();
        bunnies_.publish(world_, kBunnySlot0);        // every slot, written empty
        // ...AND THE MARCHERS AND THE BEES TOO, or the deck keeps whatever the
        // band was holding when you pressed the key. despawnAll clears the
        // POPULATION; only a publish clears the BAND, and the live path stops
        // writing these runs the moment the stage is up.
        bunnies_.publishSkunks(world_, kMarchSlot0);
        bees_.despawnAll();
        bees_.publish(world_, kBeeSlot0);
        critters_.despawnAll();
        critters_.publish(world_, kCritterSlot0);
        edit_.enter(world_, c);
        edit_.publish(world_, kBunnySlot0);
        // -- AND NOTHING ELSE STANDS ON IT ------------------------------
        //
        // The cardinal is gone (user 2026-09-13: "remove the cardinal from the
        // asset editor"). assetedit.h's own note already explains why it was
        // the odd one out: it "was a cardinal that you could look at and
        // nothing else. That is a VIEWER" -- and once the deck grew real tools
        // the bird was a second subject competing with whatever you actually
        // came here to look at.
        //
        // unstage() still runs on the way out, so a cardinal placed by an older
        // build cannot be left standing on an empty deck.
    }

    void leaveStage() {
        edit_.leave();
        birds_.unstage();
        world_.setStage(false);
        player_.pos = woodPos_;
        player_.fly = woodFly_;
        yaw_ = woodYaw_;
        pitch_ = woodPitch_;
        player_.vy = 0.0f;
        pos_ = player_.eyePosition();
    }

    // -----------------------------------------------------------------------
    // ARRIVING AT NUKETOWN -- in a front yard, on foot, looking down the map.
    //
    // standOnDeck's sibling, and the differences are the interesting part. The
    // deck has to be given its floor height as a constant because it IS a
    // constant; this level's ground is its own voxels, so the spawn asks the
    // asset's heightfield where the top of that column is (World::levelSpawn)
    // and a re-voxelised map moves the player with it rather than leaving them
    // buried in a foundation that got thicker.
    //
    // ON FOOT rather than flying, which the deck cannot manage -- see the note
    // on the [O] handler, and Solid::interior for what makes the walk work in
    // a place the terrain function has never heard of.
    //
    // ...AND WITH THE RIFLE IN HAND (user 2026-09-17). The gun is loaded at
    // start-up and stowed -- see the kit block -- so this is the only place it
    // enters the wheel, and leaveLevel is the only place it goes back out.
    // -----------------------------------------------------------------------
    void standInLevel() {
        player_.pos = world_.levelSpawn();
        player_.fly = false;
        player_.onGround = true;
        player_.vy = 0.0f;
        yaw_ = world_.levelSpawnYaw();
        // The wood's own default rather than dead level -- Options::pitch is 7
        // and a body standing still looks very slightly up, not at its feet.
        pitch_ = 7.0f;
        pos_ = player_.eyePosition();
        // -- THE GUN ------------------------------------------------------
        //
        // give() then select(), and both are needed: give() only moves the
        // hand when the hand is EMPTY (its note says why -- walking over a
        // pick while swinging an axe must not swap the axe out), and arriving
        // at a shooting map holding a stone axe is not what was asked for.
        // select() is what makes it the thing you are carrying.
        // -- ...AND NOTHING ELSE (user 2026-09-17: "remove all the tools from
        // the hand on the nuketown level. only the gun should be in the hand").
        //
        // THE WHEEL IS SAVED, NOT REBUILT. See HeldItem::snapshotKit: stowing
        // and re-giving would reset every stack to one, so nine stalks of wheat
        // would come home as one. The snapshot is the pair (carried, stack) per
        // slot, and it is what leaveLevel puts back -- which also puts the
        // rifle back to "not carried" for free, because it never was in the
        // wood.
        woodKit_ = held_.snapshotKit();
        for (int i = 0; i < held_.count(); ++i) held_.stow(i);
        if (rifleTool_ >= 0) {
            held_.give(rifleTool_);
            held_.select(rifleTool_);
            // -- AND IT ARRIVES LOADED -------------------------------------
            //
            // (user 2026-09-18: "have it start with 20 bullets".)
            //
            // AT THE DOOR, NOT AT START-UP. A magazine set once when the kit is
            // built is a magazine that is however empty you left it the last
            // time you were here -- walk out of nuketown on your last round and
            // walk back in with one round. The gun is handed over here and
            // nowhere else, so this is the moment it is a fresh gun.
            rifleAmmo_ = kRifleMag;
            held_.cancelReload();
        }

        // -- ...AND THE PISTOL IS IN THE WHEEL BESIDE IT -------------------
        //
        // (user 2026-09-18: "put it in the inventory, when the player scrolls
        // up it selects it".)
        //
        // GIVEN, NOT SELECTED. The rifle is what the map hands you and the
        // pistol is what scrolling up finds -- which is the whole of the ask,
        // and it is `give` without `select` that says it. See the kit block for
        // why the slot order is what makes "scroll up" mean this one.
        if (pistolTool_ >= 0) {
            held_.give(pistolTool_);
            // ...AND IT IS LOADED TOO, for the reason the rifle's magazine is
            // filled here and not at start-up: this is the moment it becomes a
            // fresh gun.
            pistolAmmo_ = kPistolMag;
        }

        // -- AND THE LAMP IS NOT (user 2026-09-17: "remove the lightbulb from
        // the hand on the fps map").
        //
        // It used to be given here so it sat one scroll up from the rifle, on
        // the reasoning that the map is where the pendants are and so the map
        // is where you would want to place one. In the hand it is a second
        // thing to scroll past on a map whose whole kit is meant to be the gun
        // -- the same instruction that emptied the wheel in the first place.
        //
        // NOTHING ELSE CHANGES. The bulb is still a tool and still works in the
        // wood, and shooting a pendant out still runs through
        // takeLevelBulbNear, which has never cared what is in the hand.
    }

    // -----------------------------------------------------------------------
    // THE DOOR, OPENED AT A NAMED MAP -- /locate <map name>.
    //
    // (user 2026-09-18: "I want to be able to type /locate (map name) for
    // example.")
    //
    // EVERYTHING [O] DOES, AND THEN THE ARRIVAL. It is written here rather than
    // in the console because [O]'s handler and this one have to agree about
    // FOUR things or the trip is broken in a way that surfaces much later:
    // where you were in the wood (or leaving by [O] drops you at the origin,
    // underground -- app_input.inl's note), that no fall carries across the
    // doorway, that the film is reset, and that the fog is invalidated.
    //
    // ALREADY IN THE ARCADE IS NOT A SPECIAL CASE, it is the common one:
    // /locate canyon from inside nuketown must NOT re-save woodPos_, or the
    // way home becomes the middle of the map you just left.
    bool enterLevelAt(const World::LevelMap &m) {
        if (!world_.levelOn()) {
            woodPos_ = player_.pos;
            woodYaw_ = yaw_;
            woodPitch_ = pitch_;
            woodFly_ = player_.fly;
            if (!world_.setLevel(true)) return false;
            // The kit swap is standInLevel's, and it is the whole of what
            // arriving means beyond a position -- the rifle, the stowed wood
            // tools, the badge. Called for its side effects; the position it
            // picks is overwritten immediately below.
            standInLevel();
        }
        player_.pos = world_.levelMapSpawn(m);
        yaw_ = world_.levelMapYaw(m);
        pitch_ = 7.0f;               // the wood's default -- see standInLevel
        player_.fly = false;
        player_.onGround = true;
        player_.vy = 0.0f;
        pos_ = player_.eyePosition();
        tracer_.resetAccumulation();
        volfog_.invalidate();
        return true;
    }

    void leaveLevel() {
        world_.setLevel(false);
        player_.pos = woodPos_;
        player_.fly = woodFly_;
        yaw_ = woodYaw_;
        pitch_ = woodPitch_;
        player_.vy = 0.0f;
        player_.onGround = false;   // the wood's ground decides, not this
        pos_ = player_.eyePosition();
        // ...AND THE GUN STAYS AT THE DOOR ("dont let the player have the gun
        // in the regular sandbox yet").
        //
        // stow() RATHER THAN dropSelected(), and the difference matters: drop
        // leaves the thing lying in the world as a pickup, which would put a
        // rifle in the wood the moment you walked home with one. stow just
        // takes the slot out of the wheel. It does not move the selection, so
        // the hand is pointed back at something that is actually carried here.
        if (rifleTool_ >= 0) held_.stow(rifleTool_);
        // ...AND THE PISTOL GOES BACK THROUGH THE SAME DOOR.
        if (pistolTool_ >= 0) held_.stow(pistolTool_);
        if (bulbTool_ >= 0) held_.stow(bulbTool_);
        // A CYCLE HALF PLAYED DOES NOT WALK HOME WITH YOU. stow() takes the
        // gun out of the wheel but not out of the clock -- leaving through
        // the door mid-reload would finish it in the wood, on whatever is in
        // your hand there. cycle() already does this when you scroll off the
        // gun; the door is the other way out. See HeldItem::cancelReload.
        held_.cancelReload();
        // ...AND THE WOOD'S OWN KIT COMES BACK, stacks and all. restoreKit
        // steps the hand off the rifle on its own if that is what was selected,
        // so there is no cycle() to get wrong here.
        if (!woodKit_.empty()) {
            held_.restoreKit(woodKit_);
            woodKit_.clear();
        }
    }

    // -----------------------------------------------------------------------
    // [G] IN THE LEVEL -- THE MAP AGAIN, AND YOU BACK AT ITS SPAWN.
    //
    // (user 2026-09-21: "when pressing g on the nuketown/fps mode. have it
    //  reset the map/level".)
    //
    // THE KEY ALREADY MEANT THIS IN THE WOOD. refreshWorld drops the edits,
    // re-scatters the life and respawns you, so [G] has one meaning in both
    // places -- "put it back the way you found it" -- and the level was the
    // half with no implementation. It cannot borrow the wood's: refreshWorld
    // rebuilds terrain chunks out of an edit layer and then calls chooseSpawn,
    // and a map is neither a chunk nor anywhere chooseSpawn can send you.
    //
    // IT DOES NOT CALL standInLevel(), AND THAT IS THE TRAP IN HERE. Arriving
    // snapshots the wood's wheel into woodKit_ so leaveLevel can hand it back.
    // Run that a second time from INSIDE the level and it snapshots the
    // LEVEL's wheel instead -- so the rifle becomes what you own, and every
    // stack you were carrying in the wood is gone the moment you walk home.
    // enterLevelAt guards the same call for the same reason. So the arrival is
    // written out here: a position, a facing and two full magazines, and
    // nothing at all that touches the kit.
    //
    // BACK TO THE MAP YOU ARE STANDING ON, not to the arcade's front door.
    // levelMapAt is the question clampToLevel already asks every frame, and
    // resetting nuketown should not walk you out to the canyon.
    //
    // THE DROPS ARE LEFT WHERE THEY ARE. Drops::clearAll is all-or-nothing and
    // the list is shared with the wood, so calling it here would delete
    // pickups lying in a forest that was not reset. Nothing SPILLS in the
    // level anyway -- there is no fruit here, and leaveLevel stows the guns
    // rather than dropping them precisely so a rifle never becomes one -- so
    // the only drops in a map are ones the player put there on purpose, and
    // deleting those is not resetting a map, it is taking their things.
    // -----------------------------------------------------------------------
    void resetLevel() {
        if (!world_.levelOn()) return;
        const auto t0 = std::chrono::steady_clock::now();
        if (!world_.resetLevel(physics_)) return;
        const World::LevelMap *m = world_.levelMapAt(player_.pos.x, player_.pos.z);
        player_.pos = m ? world_.levelMapSpawn(*m) : world_.levelSpawn();
        yaw_ = m ? world_.levelMapYaw(*m) : world_.levelSpawnYaw();
        pitch_ = 7.0f;   // the wood's default -- see standInLevel
        player_.fly = false;
        player_.onGround = true;
        player_.vy = 0.0f;
        pos_ = player_.eyePosition();
        // FRESH GUNS, for the reason standInLevel fills them at the door and
        // not at start-up: this is the moment the map becomes new again.
        if (rifleTool_ >= 0) {
            held_.select(rifleTool_);
            rifleAmmo_ = kRifleMag;
            held_.cancelReload();
        }
        if (pistolTool_ >= 0) pistolAmmo_ = kPistolMag;
        // The film and the fog both describe a map with holes in it. The same
        // pair refreshWorld resets, for the same reason -- and resetHistory
        // rather than just accumulation, because this IS the screen-wide case
        // it is for: every pixel changed at once.
        tracer_.resetHistory();
        tracer_.resetAccumulation();
        volfog_.invalidate();
        std::printf("v2: level reset in %.0f ms -- damage undone, lamps back, %s\n",
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count(),
                    m ? m->name.c_str() : "the arcade");
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // THE FENCE ROUND THE LEVEL -- clampToStage's sibling, and it is here for
    // exactly the reason that one is.
    //
    // The walk asks the TERRAIN what is under the body, and what the terrain
    // says is under this slab is six hundred and forty metres of nothing. On
    // the platform the level's own voxels answer first and the terrain never
    // wins; step OFF the platform and they stop answering, and the body falls
    // the whole way into a wood it was supposed to have left -- streaming the
    // forest in behind it, which is the one thing a separate world is for
    // avoiding.
    //
    // So the slab's footprint is the edge of the world while you are here, and
    // clamping to the concrete rather than to a second description of where it
    // is has always been the rule.
    //
    // -- ...AND THE CONCRETE IS PER-MAP NOW ---------------------------------
    //
    // (user 2026-09-18: "the grey platform that shares both maps is still
    //  there. remove it.")
    //
    // The slab used to run the whole length of the grid, so the grid's bounds
    // and the concrete's were the same rectangle and this could use either.
    // tools/voxelize_arcade.py cuts the foundation to each map's own footprint
    // now -- the maps are ISLANDS with open air between them -- and the two
    // stopped being the same thing. Clamping to the GRID would fence you a
    // hundred metres out into that air, standing on the invisible floor below,
    // which is a worse version of the platform the change was asked for.
    //
    // SO THE FENCE IS THE MAP YOU ARE IN. The same rule as before, read off
    // the same sidecar /locate uses, and it still needs no second description
    // of where anything is.
    //
    // FALLING BACK TO THE GRID IS DELIBERATE AND IS NOT A GUESS. There is no
    // map underfoot in exactly two cases: a level with no .maps sidecar beside
    // it (loadLevelMaps says a missing file is not an error, and that is the
    // OLD layout, which wants the old fence), and a player who has turned FLY
    // on and left the island. The second wants it too -- a fence that dragged
    // a flying player sideways would be the bug, not the fix. The WALK can
    // reach neither: you cannot step off an island that ends where the fence
    // does.
    // -----------------------------------------------------------------------
    void clampToLevel() {
        if (!world_.levelOn()) return;
        const float r = 0.32f;   // shoulders -- clampToStage's number
        Vec3 p = player_.pos;
        const World::LevelMap *m = world_.levelMapAt(p.x, p.z);
        const float x0 = m ? world_.levelMapMinX(*m) : world_.levelMinX();
        const float x1 = m ? world_.levelMapMaxX(*m) : world_.levelMaxX();
        const float z0 = m ? world_.levelMapMinZ(*m) : world_.levelMinZ();
        const float z1 = m ? world_.levelMapMaxZ(*m) : world_.levelMaxZ();
        p.x = clampf(p.x, x0 + r, x1 - r);
        p.z = clampf(p.z, z0 + r, z1 - r);
        // ...AND A FLOOR UNDER THE WHOLE OF IT. The slab's underside is the
        // bottom of this world; nothing below it is anywhere.
        const float floorY = World::levelOrigin().y;
        if (p.y <= floorY) {
            p.y = floorY;
            player_.vy = 0.0f;
            player_.onGround = true;
        }
        player_.pos = p;
    }

    // -----------------------------------------------------------------------
    // THE PAUSE PANEL: THREE BUTTONS, IN THE WOOD, IN FRONT OF YOU.
    //
    // "Remove the esc room from the sky. Instead, put the 3 balls in front of
    // the player IN GAME, along with the floating text above the balls just
    // like how it was in the esc room. When the user hits escape, the 3 balls
    // appear in a line horizontally. Pressing esc again removes them."
    // (user 2026-09-14.)
    //
    // WHAT WENT. A sealed white box at (-4096, 2048, -4096) that REPLACED the
    // world in the structure, a teleport in and out of it, the saved wood
    // position that trip needed, a point light invented because a closed box
    // has no sun in it, and a six-plane collider because the walk cannot see a
    // room the terrain does not contain. All of it was in service of one idea
    // -- that a pause menu you can see the wood through is a HUD rather than a
    // place -- and the answer to that is simply that this one IS in the wood.
    //
    // THE PANEL IS PINNED WHERE YOU WERE LOOKING, ONCE. Not carried on the
    // camera: a panel that follows your head is a HUD again, and the whole
    // point of these being models in the world is that you can step round them,
    // that the sun lights them, and that a tree can stand between you and the
    // discord button. Walk away and they stay where you left them until you
    // press the key again.
    //
    // THE MOUSE IS TAKEN, exactly as the room took it, and for the same reason:
    // the buttons are picked by the CROSSHAIR -- see buttonUnderCrosshair -- and
    // the outer two sit well off centre, so a player who cannot turn their head
    // can only ever press the middle one.
    // -----------------------------------------------------------------------
    void setRoomOpen(bool on) {
        // The green button (and anything else that closes the panel) puts
        // the ladder back at the bottom. See escRung_.
        if (!on) escRung_ = 0;
        if (on == pauseOpen_) return;
        pauseOpen_ = on;
        // Nothing half-pressed carries across -- see tickButtons.
        for (int b = 0; b < kButtonSlots; ++b) btnPress_[b] = 0.0f;
        btnHeld_ = -1;
        btnPend_ = -1;
        if (!on) {
            tracer_.clearHolos();
            if (!captureBeforeRoom_ && looking_) setCapture(false);
            return;
        }

        // -- WHERE IT STANDS -------------------------------------------------
        //
        // A panel's width in front of the eye, level with it, and square to the
        // way you are facing. The heading is taken FLAT: pitch is deliberately
        // dropped, so looking at your feet when you press the key does not bury
        // the buttons in the ground or hang them over your head.
        const Vec3 f = forward();
        const float fl = sqrtf(f.x * f.x + f.z * f.z);
        const Vec3 flat = (fl > 1e-3f) ? Vec3(f.x / fl, 0.0f, f.z / fl) : Vec3(0.0f, 0.0f, -1.0f);
        panelUp_ = Vec3(0.0f, 1.0f, 0.0f);
        // -- forward x up, AND THE ORDER OF THE THREE IS WHY -------------
        //
        // It was up x forward, and that is the player's LEFT: facing -Z, which
        // is where the camera points at a yaw of zero, cross((0,1,0),(0,0,-1))
        // is (-1,0,0). The row came out MIRRORED -- discord, back, quit from
        // left to right, where the room's wall had always read quit, back,
        // discord -- and it is only visible because button 0 and button 2 are
        // different colours; a symmetric panel would have hidden it.
        //
        // forward x up gives (1,0,0) for the same facing, which is +X, which is
        // the player's right. Button 0 is then on the left where it was.
        panelRight_ = normalize(cross(flat, panelUp_));
        // The face looks BACK at you, so the press travels away from you.
        panelInto_ = flat;
        panelFlat_ = flat;
        panelAt_ = player_.eyePosition() + flat * kPanelReachM;

        captureBeforeRoom_ = looking_;
        if (!looking_) setCapture(true);
        setRoomLabels();
    }

    // WHERE BUTTON b STANDS, and the one place that says so. The picker, the
    // publisher and the words above them all read this: three descriptions of
    // one object is the drift World::buttonAt was written to prevent, and the
    // panel moving into the world does not make that less true.
    Vec3 panelButtonAt(int b) const {
        // -- THE FOURTH IS NOT ON THE ROW (user 2026-09-21: "put it under the
        //    back green sphere").
        //
        //    So it is button 1's own column, dropped by one spacing along the
        //    panel's up. Written off panelButtonAt(1) rather than off panelAt_
        //    so it cannot drift from the button it is under -- the same reason
        //    this function exists at all.
        if (b == 3) return panelButtonAt(1) - panelUp_ * World::kBtnDropM;
        return panelAt_ + panelRight_ * (float(b - 1) * World::kBtnSpacingM);
    }
    // ...and where its word hangs: a hand's width above the button it names.
    Vec3 panelLabelMid(int b) const {
        return panelButtonAt(b) + panelUp_ * World::kBtnLabelRiseM;
    }


    // -----------------------------------------------------------------------
    // WHICH BUTTON THE PLAYER IS LOOKING AT, or -1.
    //
    // A ray against the buttons on a known plane, in host code -- there is no
    // reason to ask the acceleration structure about a handful of circles whose
    // centres this file already knows, and doing it here means the highlight
    // and the click cannot disagree about which one is under the cursor.
    // -----------------------------------------------------------------------
    int buttonUnderCrosshair() const {
        if (!pauseOpen_) return -1;
        const Vec3 o = pos_;
        const Vec3 d = Camera::direction(yaw_, pitch_);
        const float r = World::buttonRadiusM();
        // A RAY AGAINST THE SPHERES. It was a ray against discs on the
        // wall plane, which was right while the buttons were painted on it --
        // now they are balls standing proud of it, and a plane test would let
        // you press one by looking at the wall BESIDE it from far enough to the
        // side. The nearest root wins, so a ball in front of another cannot be
        // pressed through.
        // kButtonSlots, NOT 3 -- AND THIS IS WHY THE FOURTH DID NOTHING.
        //
        // The settings sphere published, drew, wore its label and had an action
        // wired to it, and was still dead: every OTHER loop over the buttons
        // had been widened and the PICKER had not, so nothing could ever
        // return its index. It was a ball you could look straight at and not
        // press. A count written as a literal in one of five places is the
        // whole bug, and it is the fifth place that decides.
        int best = -1;
        float bestT = 1e9f;
        for (int b = 0; b < kButtonSlots; ++b) {
            const Vec3 c = panelButtonAt(b);
            const float ox = o.x - c.x, oy = o.y - c.y, oz = o.z - c.z;
            const float half = ox * d.x + oy * d.y + oz * d.z;
            const float cq = ox * ox + oy * oy + oz * oz - r * r;
            const float disc = half * half - cq;
            if (disc < 0.0f) continue;
            const float sq = sqrtf(disc);
            float t = -half - sq;
            if (t <= 0.0f) t = -half + sq;      // standing inside it
            if (t <= 0.0f || t >= bestT) continue;
            bestT = t;
            best = b;
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // THE ROOM'S PHYSICS, WHICH IS SIX PLANES AND THREE CYLINDERS.
    //
    // The walk collider reads the TERRAIN -- see walkWorld -- and the pause
    // room is not terrain: it is its own bottom-level structure at -4096, 2048,
    // where there is no chunk and never will be. So the collider finds nothing
    // under the player and they fall, for ever, out of the bottom of a sealed
    // box. Flight hid it; walking is what was asked for.
    //
    // A BOX IS TWO CORNERS, and that is nearly the whole of it. Applied AFTER
    // the player has moved, so the ordinary walk -- momentum, the head bob, the
    // crouch, the jump -- all still happen, and this only says where they stop.
    //
    // THE BUTTONS ARE SOLID TOO. They are balls standing two thirds proud of
    // the wall at chest height, so without this you can walk into the middle of
    // one and press it from inside, looking at the back of its far side.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // IS THE COLUMN AT (x, z) UNDER WATER?
    //
    // The three calls WaterField::rebuild makes, in the same order and with the
    // same meaning -- this is the engine's single definition of "wet", and every
    // creature that needs to know asks it the same way. A second definition is
    // how a bunny and a lily pad end up disagreeing about where a lake is.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // ...AND HOW HIGH THAT WATER STANDS, for anything that has to fly OVER it.
    //
    // wetColumnAt answers "is this a lake", which is all a walker or a spawn
    // site needs. A flyer needs the number: `ground` is the top of the SOLID
    // column, which in a lake is the BED, and a creature holding a metre over
    // that is a metre above the bottom of the lake. See Critters::flyFloor and
    // the ladybug that was reported swimming.
    //
    // Deliberately NOT -infinity on a dry column: the caller takes a max
    // against the ground and a real sentinel keeps that honest without needing
    // a second flag.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // IS THE GROUND HERE A BEACH?
    //
    // The surface material, asked the way the mesher asks it -- topMaterial of
    // the column's own height -- so this cannot disagree with what is drawn.
    // Both the single sand id and the four grains the device spreads it over,
    // because either can be the surface of a shore. See Bunnies::blocked.
    // -----------------------------------------------------------------------
    bool sandAt(float x, float z) {
        // -- A DESERT IS NOT A BEACH, AND THIS FUNCTION COULD NOT TELL ------
        //
        // The note above says what this is FOR -- "is the ground here a beach"
        // -- and the body answers "is the surface material sand", which was the
        // same question right up until there was a band whose whole floor is
        // sand. Then it silently became "is this the desert or a shore", and
        // Bunnies::blocked treats a true as an obstacle for every walker in the
        // engine.
        //
        // So the desert was uninhabitable, and in the exact way that is hardest
        // to read: the gecko, the cobra and the scorpion loaded, reported their
        // frames and their slots, and every one of them printed "none placed".
        // Nothing was wrong with the species, the gate or the lattice -- every
        // site they tried was, correctly, sand.
        //
        // THE ASK THIS RULE CAME FROM IS UNCHANGED (user 2026-09-14: "avoid
        // putting landmammals on the sand banks"). A bank is a strip of shore
        // beside water. The dunes are not that, and the 0.5 here is the same
        // line woodBit, waterAt and the tree scatter all draw -- so a rim
        // column that is half sand is still a shore to this test, and the
        // desert proper is not.
        if (world_.terrain.desertMix(x) >= 0.5f) return false;
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        TerrainMemo memo;
        const int h = world_.terrain.heightVox(ci, cj, memo);
        return isSand(world_.terrain.topMaterial(ci, cj, h, memo));
    }

    float waterTopAt(float x, float z) {
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        TerrainMemo memo;
        const int line =
            world_.terrain.lakeLineAt(world_.terrain.wx(ci), world_.terrain.wx(cj), memo);
        if (line == VoxelTerrain::kNoWaterVox) return -1.0e30f;
        const int h = world_.terrain.heightVox(ci, cj, memo);
        if (!world_.terrain.wetColumn(ci, cj, h, line, memo)) return -1.0e30f;
        return float(line + 1) * VOXEL_M;
    }

    bool wetColumnAt(float x, float z) {
        const int ci = int(std::floor(x / VOXEL_M)), cj = int(std::floor(z / VOXEL_M));
        TerrainMemo memo;
        const int line =
            world_.terrain.lakeLineAt(world_.terrain.wx(ci), world_.terrain.wx(cj), memo);
        if (line == VoxelTerrain::kNoWaterVox) return false;
        const int h = world_.terrain.heightVox(ci, cj, memo);
        return world_.terrain.wetColumn(ci, cj, h, line, memo);
    }

    // -----------------------------------------------------------------------
    // A LOOSE BODY, AS A SWING.
    //
    // The one place a DebrisHit becomes something the tools, the audio and the
    // log can all read -- so a felled tree answers every question they ask in
    // exactly the words a standing one does. What it is MADE of comes out of
    // the body (World::DebrisTakes) rather than out of its material id: a
    // boulder's grey is a palette entry and not mat::ROCK, so the material
    // could never have told anybody which tool to use.
    // -----------------------------------------------------------------------
    static Swing looseSwing(const DebrisHit &h, const Vec3 &eye, const Vec3 &dir) {
        Swing s;
        s.hit = true;
        s.kind = Swing::Loose;
        s.debris = h.slot;
        s.takesAs = h.takes == kDebrisWood   ? Takes::Wood
                    : h.takes == kDebrisSoil ? Takes::Soil
                                             : Takes::Stone;
        // ...AND WHETHER IT IS A MUSHROOM, which Takes above cannot say: a cap
        // answers to the axe AND the pick, and that enum holds one tool's worth
        // of material. Swing::soft is the same flag a STANDING cap arrives with
        // (swingRayModels, off Solid::bouncy), so toolTakes needs one rule for
        // both and the fallen half of a mushroom cannot end up cuttable by a
        // different tool than the standing half. See kDebrisSoft.
        s.soft = h.takes == kDebrisSoft;
        s.dist = h.t;
        s.point = h.point;
        s.material = h.mat;
        s.eye = eye;
        s.dir = dir;
        s.reach = swingReachM(dir);
        return s;
    }

    // -----------------------------------------------------------------------
    // THE DECK IS A FLOOR AND A FENCE.
    //
    // clampToRoom, asked of the other place that is not the world. The room is
    // a box and this is a plate, so it is the same function with the walls and
    // the ceiling taken out -- and one thing put in that the room gets for
    // free.
    //
    // THE FENCE, WHICH THE ROOM GETS FROM ITS WALLS. The deck has none, and it
    // has nothing underneath it either: one step in any direction is six
    // hundred metres of clear air and no ground at the bottom of it. So the rim
    // stops you. An eight metre deck is as far as walking round a model ever
    // goes, and flight is still there and still unclamped upwards for the view
    // from above.
    //
    // IT RUNS WHILE FLYING TOO. A body that can be flown through its own floor
    // is a body that can be left under the deck when flight goes off, which is
    // the one way back into the fall this exists to prevent.
    // -----------------------------------------------------------------------
    void clampToStage() {
        if (!world_.staged()) return;
        const float r = 0.32f;   // shoulders -- clampToRoom's number
        Vec3 p = player_.pos;
        p.x = clampf(p.x, World::stageMinX() + r, World::stageMaxX() - r);
        p.z = clampf(p.z, World::stageMinZ() + r, World::stageMaxZ() - r);
        const float floorY = World::stageFloorY();
        if (p.y <= floorY) {
            p.y = floorY;
            player_.vy = 0.0f;
            player_.onGround = true;
        }
        player_.pos = p;
    }

    // -- AND NOTHING CLAMPS THE PLAYER ANY MORE ---------------------------
    //
    // clampToRoom was here: six planes and three cylinders, the pause room's
    // whole physics, written because the walk collider reads the TERRAIN and
    // the room was a separate structure four kilometres from the nearest chunk
    // -- so without it the player fell out of the bottom of a sealed box.
    //
    // The panel stands in the wood now, on ground the walk already understands,
    // so there is nothing left to clamp to. YOU CAN WALK THROUGH THE BUTTONS,
    // deliberately: they are 50 cm of light at eye height in the middle of a
    // path, and a pause menu that can shove you off a ledge or wedge you
    // against a trunk would be a worse thing than one you can step through.

    // -----------------------------------------------------------------------
    // THE BUTTONS' OWN CLOCK.
    //
    // A LINEAR RAMP, NOT AN EXPONENTIAL EASE. Everything else that moves in
    // this engine is eased, because everything else is an animal or a camera
    // and those do not start instantly. A button is a piece of plastic under a
    // finger: it goes down at the speed the finger pushes it, and -- more to
    // the point -- an exponential never actually ARRIVES, so there would be no
    // frame that is the bottom of the travel to hang the action on.
    //
    // DOWN FAST AND BACK SLOWER. 80 ms down is about as long as a real switch
    // takes and short enough that the click still feels like the cause; the
    // spring back is twice that because nobody is pushing it any more.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // THE CRT POWER-OFF, AND THEN THE DOOR.
    //
    // ON THE WALL CLOCK, NOT THE SHOT CLOCK. Everything else in this frame is
    // fed the scaled dt so a scripted camera move and a live one agree; this
    // wants the real one, because it is a thing happening to the SCREEN rather
    // than to the world, and a paused or slowed game should still take exactly
    // as long to switch off.
    //
    // THE EXIT IS ONE FRAME LATE ON PURPOSE. shutdown is called when the ramp
    // has already been at 1 for a frame, so the fully black frame is presented
    // before the window goes -- otherwise the last thing on screen is the
    // second-to-last frame of the collapse, which is a bright dot.
    // -----------------------------------------------------------------------
    // -- RETURNS TRUE ONCE THE WINDOW IS GOING, AND THE CALLER MUST STOP ----
    //
    // (user 2026-09-20: "when hitting esc three times to exit the application,
    //  the game crashes.")
    //
    // SampleApp::shutdown does not unwind anything -- it sets mShouldTerminate
    // and calls mpWindow->shutdown() there and then. The rest of the frame
    // carried on afterwards: onFrameRender calls this at app_frame.inl:94 and
    // then runs World::update a sixty lines later, which drains the BLAS
    // compaction queue and reads back a post-build query on a device whose
    // window has just been torn down. That is the reported stack, exactly:
    //
    //   DXGI_ERROR_DEVICE_REMOVED
    //     RtAccelerationStructurePostBuildInfoPool::getElement
    //     World::finishCompact / drainCompactions / update
    //     ForestApp::onFrameRender   app_frame.inl:154
    //
    // Nothing between here and the end of that frame is safe once the window is
    // gone, so the frame has to END, not continue. Returning a bool is the
    // whole fix; the caller does the rest.
    bool tickQuit(float wallDt) {
        if (!quitting_) return false;
        const bool wasDone = quitT_ >= 1.0f;
        quitT_ = minf(1.0f, quitT_ + wallDt / kQuitFadeSec);
        tracer_.crt = quitT_;
        if (!wasDone) return false;
        askShutdown(0);
        return true;
    }

    void tickButtons(float dt) {
        btnClock_ += dt;
        for (int b = 0; b < kButtonSlots; ++b) {
            const float want = (btnHeld_ == b) ? 1.0f : 0.0f;
            const float step = dt / ((btnHeld_ == b) ? kBtnDownSec : kBtnUpSec);
            btnPress_[b] += clampf(want - btnPress_[b], -step, step);
        }

        // CONTACT. The order matters: the button is put at the bottom of its
        // travel and PUBLISHED there before the action runs, so the last frame
        // the player sees -- which for the red one is the last frame there is
        // -- has the button fully down in it.
        if (btnPend_ >= 0 && btnClock_ >= btnAt_) {
            const int b = btnPend_;
            btnPend_ = -1;
            btnHeld_ = -1;              // ...and it springs back from here
            btnPress_[b] = 1.0f;
            publishPanel(true);
            pressButton(b);
            return;
        }
        publishPanel(pauseOpen_);
    }

    // The three of them, where the panel put them. One call, so the press ramp
    // and the panel's pose cannot be handed over from two different places.
    // -----------------------------------------------------------------------
    // IT WALKS WITH YOU, AND IT DOES NOT TURN WITH YOU.
    //
    // (user 2026-09-17: "have the ui menu move with the players wasd movement,
    //  not the mouse movement. dont have the ui frozen in place like it
    //  currently is.")
    //
    // THE PANEL WAS PLACED ONCE, AT THE MOMENT IT OPENED, and then left in the
    // world -- a panel's width in front of wherever you happened to be standing
    // and facing. Walk away and you leave it behind; that is the "frozen in
    // place".
    //
    // TWO THINGS THAT SOUND LIKE ONE. Following the CAMERA would nail it to
    // the middle of the screen and it would swing every time the mouse moved,
    // which is the thing being asked against. Following the PLAYER moves it
    // with the body and leaves its facing alone: it keeps the heading it was
    // opened with (panelFlat_), so it stays put as you look around, and it
    // stays the same distance in front as you walk.
    //
    // THE HEADING IS NOT RE-TAKEN, ever, which is the whole point. Re-deriving
    // it from forward() here is exactly the mouse-follow that is not wanted --
    // and it would also spin the row of buttons through their own labels.
    void repositionPanel() {
        panelAt_ = player_.eyePosition() + panelFlat_ * kPanelReachM;
    }

    void publishPanel(bool show) {
        if (show) {
            repositionPanel();
            // -- AND THE WORDS COME WITH THEM ---------------------------
            //
            // (user 2026-09-20: "when pressing esc to bring up the esc sphere
            //  menu, the words dont stay with the spheres".)
            //
            // repositionPanel moves panelAt_ to follow the player every frame,
            // and the BUTTONS are published from panelButtonAt(b) -- which
            // reads it. The labels were written once, in openPause, off the
            // panelAt_ of that instant. So the balls walked with you and their
            // words stayed where you pressed the key.
            //
            // Re-set here, beside the publish that moves them, because that is
            // the one place the two can be seen to agree. The HEADING is still
            // never re-taken: setRoomLabel reads panelRight_/panelUp_, which
            // repositionPanel deliberately leaves alone (see its note) -- so
            // this follows the position without the mouse-follow that note is
            // about.
            setRoomLabels();
        }
        const Vec3 at[kButtonSlots] = {panelButtonAt(0), panelButtonAt(1),
                                       panelButtonAt(2), panelButtonAt(3)};
        world_.publishButtons(show, btnPress_, at, panelRight_, panelUp_, panelInto_);
    }

    // 0 red = quit, 1 green = back to the wood, 2 blue = the Discord server.
    // -----------------------------------------------------------------------
    // SWITCH THE TUBE OFF, AND LET tickQuit CLOSE THE DOOR.
    //
    // Not shutdown(0). The window would be gone on the same frame the press
    // landed, and the last thing anybody saw would be a button halfway down --
    // which is the same complaint the press animation was added to fix, one
    // level further in.
    //
    // ONE FUNCTION BECAUSE THERE ARE TWO WAYS OUT AND THEY MUST NOT DIFFER:
    // the red button on the wall, and the third ESC. A quit that collapsed the
    // picture when pressed with the mouse and blinked out when pressed with the
    // keyboard would read as a bug in whichever one you found second.
    //
    // IDEMPOTENT. ESC held down, or pressed again while the picture is going,
    // must not restart the collapse from full brightness.
    // -----------------------------------------------------------------------
    void beginQuit() {
        if (quitting_) return;
        quitT_ = 0.0f;
        quitting_ = true;
    }

    void pressButton(int b) {
        if (b == 0) {
            beginQuit();
        } else if (b == 1) {
            leaveRoom();
        } else if (b == 2) {
            openDiscord();
        } else if (b == 3) {
            // -- THE GREY ONE OPENS THE SETTINGS (user 2026-09-21: "when the
            //    user clicks it, it open the current (y) settings").
            //
            //    THE SAME CALL [Y] MAKES, not a second way in: setMenuOpen is
            //    what owns whether the panel is up, and a button that set the
            //    flag itself would be a second description of one state -- the
            //    drift this file's own panelButtonAt note is about.
            //
            //    THE PAUSE PANEL STAYS. The settings are drawn over the world
            //    either way, and a player who opened them from here has not
            //    asked to leave the menu -- pressing [Y] or ESC closes them.
            setMenuOpen(!menuOpen_);
        }
    }

    // -----------------------------------------------------------------------
    // THE GREEN BUTTON PUTS THE PANEL AWAY, and that is the whole of it.
    //
    // IT USED TO BE A JOURNEY. The room stood four kilometres off the wood, so
    // "back" was a teleport, and a teleport is only safe once the chunks it
    // lands in exist. The press therefore armed a WAIT -- the button held at
    // the bottom of its travel, the word over it changed to "loading",
    // tickRoomExit letting the player out on the first frame chunkAt(woodPos_)
    // was true with nothing missing and nothing in flight, and a 15 s giveup so
    // a stalled streamer could not be a room with no door. That was written
    // for a real fault: ESC in the first seconds of a launch, or out of the
    // asset deck, used to drop the player into an empty grey hole.
    //
    // THERE IS NOWHERE TO COME BACK FROM NOW. The panel hangs in the wood the
    // player never left, on chunks they were standing in a moment ago, so the
    // wait would wait on nothing -- and worse than nothing: woodPos_ is now
    // only ever the ASSET DECK's saved spot, so a panel opened by somebody who
    // has not visited the deck would test the ORIGIN, find no chunk there, and
    // sit on "loading" for the full fifteen seconds before letting go.
    //
    // THE DECK'S DOOR IS UNCHANGED and still teleports on the frame it is
    // pressed -- leaveStage never had this wait, it borrowed the room's by
    // being reachable through it. If an empty ring is ever seen stepping off
    // the deck, the wait belongs THERE, keyed on the position leaveStage is
    // about to restore, and the shape of it is in this note.
    // -----------------------------------------------------------------------
    void leaveRoom() { setRoomOpen(false); }

    // THE ONE PLACE THE LINK LIVES. docs/discord-integration.md is where it
    // came from; the browser is the shell's business, not ours.
    void openDiscord() const {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", "https://discord.gg/AtW5fWZtSG", nullptr, nullptr,
                      SW_SHOWNORMAL);
#endif
        std::printf("  room     opening https://discord.gg/AtW5fWZtSG\n");
        std::fflush(stdout);
    }

