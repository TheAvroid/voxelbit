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
            {"steak", {0.0110f, 0.056f, 0.260f, 0.000f}},
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
        const int sel = held_.selected();
        if (sel < 0) return;
        const Tool &t = held_.tool(sel);
        // FORCED WHILE THE PANEL IS OPEN, so there is something to aim the
        // sliders at -- almost everything in the kit sits at one.
        const int n = (stackPanelOpen_ && stackPanelForce_) ? maxi(2, t.stack) : t.stack;
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
        if (n != stackPopN_ || sel != stackPopTool_) {
            if (n != stackPopN_ && sel == stackPopTool_) stackPopT0_ = simMs_;
            stackPopN_ = n;
            stackPopTool_ = sel;
        }
        if (!t.carried || n < 2) return;

        char word[8];
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
        const bool full = n >= HeldItem::kStackMax;
        const float3 tint = full ? float3(kStackNits * 1.00f, kStackNits * 0.78f,
                                          kStackNits * 0.22f)
                                 : float3(kStackNits, kStackNits, kStackNits);
        holoSetWord(h, word, float3(at.x, at.y, at.z), float3(rt2.x, rt2.y, rt2.z),
                    float3(up2.x, up2.y, up2.z), cellM, tint);
    }

    void setRoomLabels() {
        for (int b = 0; b < 3; ++b) setRoomLabel(b, World::buttonLabel(b));
    }

    // ONE WORD OVER ONE BUTTON, in that button's own colour. Split out of
    // setRoomLabels so the green one can say something other than "back" while
    // it waits -- see leaveRoom.
    void setRoomLabel(int b, const char *word) {
        if (b < 0 || b >= 3) return;
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
        if (bulbTool_ >= 0) held_.stow(bulbTool_);
        // ...AND THE WOOD'S OWN KIT COMES BACK, stacks and all. restoreKit
        // steps the hand off the rifle on its own if that is what was selected,
        // so there is no cycle() to get wrong here.
        if (!woodKit_.empty()) {
            held_.restoreKit(woodKit_);
            woodKit_.clear();
        }
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
    // So the slab's footprint is the edge of the world while you are here. The
    // asset IS the platform -- the .glb's biggest mesh by far is the slab, and
    // every wall stands inside it -- so clamping to the asset's own bounds is
    // clamping to the concrete, with no second description of where it is.
    // -----------------------------------------------------------------------
    void clampToLevel() {
        if (!world_.levelOn()) return;
        const float r = 0.32f;   // shoulders -- clampToStage's number
        Vec3 p = player_.pos;
        p.x = clampf(p.x, world_.levelMinX() + r, world_.levelMaxX() - r);
        p.z = clampf(p.z, world_.levelMinZ() + r, world_.levelMaxZ() - r);
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
        if (on == pauseOpen_) return;
        pauseOpen_ = on;
        // Nothing half-pressed carries across -- see tickButtons.
        btnPress_[0] = btnPress_[1] = btnPress_[2] = 0.0f;
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
        return panelAt_ + panelRight_ * (float(b - 1) * World::kBtnSpacingM);
    }
    // ...and where its word hangs: a hand's width above the button it names.
    Vec3 panelLabelMid(int b) const {
        return panelButtonAt(b) + panelUp_ * World::kBtnLabelRiseM;
    }


    // -----------------------------------------------------------------------
    // WHICH BUTTON THE PLAYER IS LOOKING AT, or -1.
    //
    // A ray against three discs on a known plane, in host code -- there is no
    // reason to ask the acceleration structure about three circles whose
    // centres this file already knows, and doing it here means the highlight
    // and the click cannot disagree about which one is under the cursor.
    // -----------------------------------------------------------------------
    int buttonUnderCrosshair() const {
        if (!pauseOpen_) return -1;
        const Vec3 o = pos_;
        const Vec3 d = Camera::direction(yaw_, pitch_);
        const float r = World::buttonRadiusM();
        // A RAY AGAINST THREE SPHERES. It was a ray against three discs on the
        // wall plane, which was right while the buttons were painted on it --
        // now they are balls standing proud of it, and a plane test would let
        // you press one by looking at the wall BESIDE it from far enough to the
        // side. The nearest root wins, so a ball in front of another cannot be
        // pressed through.
        int best = -1;
        float bestT = 1e9f;
        for (int b = 0; b < 3; ++b) {
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
    void tickQuit(float wallDt) {
        if (!quitting_) return;
        const bool wasDone = quitT_ >= 1.0f;
        quitT_ = minf(1.0f, quitT_ + wallDt / kQuitFadeSec);
        tracer_.crt = quitT_;
        if (wasDone) shutdown(0);
    }

    void tickButtons(float dt) {
        btnClock_ += dt;
        for (int b = 0; b < 3; ++b) {
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
        if (show) repositionPanel();
        const Vec3 at[3] = {panelButtonAt(0), panelButtonAt(1), panelButtonAt(2)};
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

