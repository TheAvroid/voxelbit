// app_spawn.inl
//
// Lifted out of app.h. This file is #included INSIDE the body of ForestApp, at
// exactly the point the code used to sit, so the preprocessor sees the same
// text in the same order -- member declaration order, layout and init order are
// unchanged. It is not a standalone header and has no include guard.
//
// Contents: choosing a spawn and nudging out of solids
// -----------------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // HOW MUCH OF A WOOD THE LISTENER IS STANDING IN.  0 in the open, 1 under
    // a closed canopy, and it drives nothing but the ambience volume.
    //
    // IT IS THE PLANTING RULE, NOT A SECOND OPINION.  scene/chunks.h decides
    // whether a cell grows a tree with
    //
    //     saturate((dens - 0.30) / 0.32) * 0.92 + 0.05
    //
    // and the canopy-closure ramp inside it is reused here verbatim. A
    // separate "am I in a forest" field would be a second definition of the
    // wood, and two definitions drift apart the first time either is tuned --
    // the audible symptom being birds in a clearing.
    //
    // The 0.05 floor is deliberately NOT carried over. That floor is the
    // handful of stragglers a real clearing still has standing in it, and a
    // clearing you can still hear the wood from is not a clearing.
    //
    // BOTH BANDS COUNT: pine and birch are both woods, so birchMix does not
    // appear here at all.
    //
    // AND THERE IS NO SLOPE TEST, though the planter has one. kTreeSlope stops
    // a tree standing on scree; it does not stop scree being in the middle of
    // a forest, and cutting the birds because you stepped onto a boulder field
    // would put a hard edge in a signal that is otherwise smooth everywhere.
    //
    // THE WATER FADE IS KEPT, AND RAMPED. topMaterial paints sand up to 3.4 m
    // and the planter refuses that band outright -- a beach and a lake are the
    // one part of this world with no wood in them. Ramped over the four metres
    // above it rather than switched at it, which also lands the full level at
    // the same 7.6 m chooseSpawn calls "well clear of the shore". A step test
    // on the height field is exactly the pop the smoothing in
    // Ambience::update should not be asked to hide.
    // -----------------------------------------------------------------------
    float forestGain() const {
        const VoxelTerrain &t = world_.terrain;
        const float x = pos_.x, z = pos_.z;

        // The band's line. A dry band answers kNoWater, so `above` comes out
        // enormous and the gain saturates -- which is what "nowhere near water"
        // should mean here.
        const float above = t.heightM(x, z) - (t.waterAt(x) + 0.8f);
        if (!(above > 0.0f)) return 0.0f;
        const float wet = above < 4.0f ? above * 0.25f : 1.0f;

        const float closure = (t.standDensity(x, z) - 0.30f) / 0.32f;
        return wet * (closure < 0.0f ? 0.0f : (closure > 1.0f ? 1.0f : closure));
    }

    void chooseSpawn() {
        uint32_t seed = opt_.spawnSeed;
        if (seed == 0) {
            const uint64_t t =
                uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
            // Mixed rather than truncated: the low bits of a steady clock move
            // in lockstep with its resolution, and two launches a millisecond
            // apart should not land next to each other.
            seed = uint32_t(t * 0x9E3779B97F4A7C15ull >> 32) | 1u;
        }

        const VoxelTerrain &t = world_.terrain;
        float bestX = opt_.camX, bestZ = opt_.camZ;
        bool found = false;

        // -- HOW OPEN IS IT, AND IS THE OPENING FACING THE SUN --------------
        //
        // The stand-density field is what plants the trees -- the gate in
        // scene/chunks.h keeps 5% of the base density at 0.30 and 97% at 0.62 --
        // so asking it is asking how many trunks are here, without a chunk
        // having to exist.
        //
        // A DISC, NOT A POINT. One cell of low density is a hole between two
        // trees, not a glade; a ring at twelve metres is what tells the two
        // apart.
        //
        // AND ALONG THE SUN'S OWN LINE, which is the half that actually
        // delivers sunlight. A clearing is not the same thing as a sunlit
        // clearing: at the default elevation of 24 degrees a 22 m pine throws
        // about fifty metres of shadow, so a twenty-metre glade with a wall of
        // trees on its sunward side is in shade for the whole morning. The
        // sunward samples run out to sixty metres and carry half the score.
        //
        // The azimuth is the one the options carry rather than the clock's,
        // because the clock is not running yet when this is called -- and the
        // base is where the day starts, which is when a spawn happens.
        FbmMemo dm;
        const float sunA = opt_.sunAz * PI / 180.0f;
        const float sunX = cosf(sunA), sunZ = sinf(sunA);
        auto openness = [&](float px, float pz) {
            float local = t.standDensity(px, pz, dm);
            for (int k = 0; k < 8; ++k) {
                const float a2 = float(k) * (TWO_PI / 8.0f);
                local += t.standDensity(px + cosf(a2) * 12.0f, pz + sinf(a2) * 12.0f, dm);
            }
            local *= (1.0f / 9.0f);
            float sunward = 0.0f;
            for (int k = 1; k <= 6; ++k) {
                const float d2 = float(k) * 10.0f;
                sunward += t.standDensity(px + sunX * d2, pz + sunZ * d2, dm);
            }
            sunward *= (1.0f / 6.0f);
            return 0.5f * local + 0.5f * sunward;
        };

        // -- AND CAN THE GROUND ITSELF SEE THE SUN --------------------------
        //
        // Openness is about TRUNKS. This is about the hill, and at this sun it
        // matters just as much: 24 degrees of elevation means a rise of one
        // metre shadows two and a half metres of ground behind it, so a glade
        // on the wrong side of a ridge is a glade in shade all morning. The
        // first cut of this found beautifully sparse spots that were dark.
        //
        // One march along the sun's own bearing, out to ninety-odd metres,
        // asking whether the height field ever climbs above the line the sun
        // comes in on. It is the honest terrain-shadow test and it is cheap
        // enough BECAUSE it runs last -- only for a candidate that has already
        // beaten everything before it, which is a handful of times, not 512.
        //
        // The elevation is the option's rather than the clock's, for the reason
        // the azimuth is: the clock has not started when this is called.
        const float tanEl = tanf(maxf(2.0f, opt_.sunEl) * PI / 180.0f);
        auto sunlit = [&](float px, float pz, float ph) {
            for (int k = 1; k <= 16; ++k) {
                const float d2 = float(k) * 6.0f;
                if (t.heightM(px + sunX * d2, pz + sunZ * d2) > ph + d2 * tanEl) return false;
            }
            return true;
        };

        // THE BEST OF ALL OF THEM, not the first that passes. The old rule took
        // whatever candidate cleared a band and stopped, which is why it opened
        // in a wood as often as not: a band admits the thick end of itself just
        // as readily as the thin end.
        float bestScore = 1e9f;
        float anyX = opt_.camX, anyZ = opt_.camZ, anyScore = 1e9f;

        for (uint32_t i = 0; i < 512; ++i) {
            // A disc, sampled with a square root so the points are spread over
            // the AREA rather than piled up near the middle.
            const float r = 200.0f + 2800.0f * sqrtf(hashUnit(seed + 1u, i));
            const float a = hashUnit(seed + 2u, i) * 6.2831853f;
            const float x = r * cosf(a), z = r * sinf(a);

            const float h = t.heightM(x, z);
            // WELL CLEAR OF THE SHORE, not merely out of the water. topMaterial
            // paints a sand band for the first 3.4 m above the waterline, so a
            // spawn a metre up is a spawn on a beach -- which is the one part
            // of this world with no trees in it and the last place to open a
            // forest in.
            // Asked per candidate rather than hoisted: the waterline is per
            // band now, and a spawn search ranges far enough to cross one.
            if (h < t.waterAt(x) + 5.0f) continue;

            const int ci = int(floorf(x / VOXEL_M)), cj = int(floorf(z / VOXEL_M));
            const int slope = maxi(absi(t.heightVox(ci + 1, cj) - t.heightVox(ci - 1, cj)),
                                   absi(t.heightVox(ci, cj + 1) - t.heightVox(ci, cj - 1)));
            if (slope >= VoxelTerrain::kTreeSlope) continue;  // scree, not ground

            const float score = openness(x, z);
            // Kept whatever happens, so a seed that finds nothing ideal still
            // spawns somewhere sensible rather than at the world origin.
            if (score < anyScore) {
                anyScore = score;
                anyX = x;
                anyZ = z;
            }
            // TOO THICK is a wall of trunks to wake up in. TOO OPEN is a bald
            // patch, which is not the wood this engine is for -- the point is
            // to open in sunlight AMONG trees, not away from them.
            if (score > 0.45f || score < 0.20f) continue;
            if (score >= bestScore) continue;
            // LAST, because it is the expensive one -- see the note over it.
            if (!sunlit(x, z, h)) continue;
            bestScore = score;
            bestX = x;
            bestZ = z;
            found = true;
        }
        if (!found) {
            bestX = anyX;
            bestZ = anyZ;
            bestScore = anyScore;
        }

        opt_.camX = bestX;
        opt_.camZ = bestZ;
        std::printf("  spawn    %.1f, %.1f  openness %.2f%s  (--spawn %u to come back here)\n",
                    bestX, bestZ, double(bestScore), found ? "" : " -- nothing better found",
                    unsigned(seed));
        std::fflush(stdout);
    }

    // A spawn inside a trunk is a spawn you cannot walk out of: a tree is a
    // wall at every height, so the collision resolver has nowhere to push you.
    // The terrain test above cannot see trees -- they are placed per chunk and
    // no chunk existed yet -- so this runs once the ring is resident and steps
    // outward until the body fits.
    // How far a solid may rise above the ground before standing where it is
    // counts as being INSIDE it rather than on it. A voxel is 10 cm and the
    // player steps up rather more than that, so this is a low kerb: anything
    // taller is something you would be buried in.
    static constexpr float kSpawnStepM = 0.45f;

    void nudgeOutOfSolids() {
        // NOT named 'near'. windows.h, which this file includes for the mouse
        // capture, still defines near and far as empty macros from the segmented
        // memory era, and the error it produces names neither of them.
        std::vector<Solid> nearby;
        world_.collidersNear(player_.pos, 12.0f, &nearby);
        // -- INSIDE IS THE TEST, NOT UNSTANDABLE (user 2026-09-07) ----------
        //
        // "dont spawn me into rocks, or anything for that matter." This asked
        // `!s.standable`, which excludes precisely the thing that was being
        // complained about: a rock IS standable -- that is what lets you climb
        // a small one -- so every rock in the wood was skipped by the test
        // meant to keep you out of them, and a spawn inside a boulder was not a
        // near miss but a case the check declined to look at.
        //
        // Standable is the wrong question anyway. Standing ON a rock is fine
        // and being INSIDE one is not, and those differ by HEIGHT, not by kind.
        // So the test is vertical now: a solid blocks the spot if its footprint
        // holds you AND its top stands more than a step above the ground you
        // would be placed on -- which is the definition of being embedded in
        // it. A pebble whose top is within a step is something you walk onto,
        // and it still does not block.
        //
        // Trunks keep working unchanged: a trunk's top is a canopy twenty
        // metres up, so it fails the height test by a mile, exactly as it
        // failed the standable test before.
        //
        // AND THE FOOTPRINT IS THE MODEL'S VOXELS. `touches` is the collider
        // ellipse -- a circle round the widest part of a model's bottom two
        // metres -- so it covered several metres of open air beside a big
        // boulder and pushed a spawn out of ground that was perfectly clear,
        // while a body under a leaning crown was inside voxels the ellipse did
        // not reach. Asking the voxels answers both, and answers them about the
        // body's own height rather than about a shadow on the ground.
        TerrainMemo nm;
        auto blocked = [&](float x, float z) {
            const float g = world_.terrain.heightM(x, z, nm);
            for (const Solid &s : nearby) {
                if (s.hx <= 0.0f || s.hz <= 0.0f) continue;
                if (s.vol) {
                    if (solidBoxOverlap(s, x, g, z, g + kBodyHeightM, player_.halfWidth, VOXEL_M))
                        return true;
                    continue;
                }
                if (!touches(s, x, z, player_.halfWidth)) continue;
                if (s.top > g + kSpawnStepM) return true;
            }
            return false;
        };
        if (!blocked(player_.pos.x, player_.pos.z)) return;

        // Outward in rings rather than in a random walk, so the nudge is the
        // SHORTEST one that works and the spot stays the spot that was chosen.
        for (int ring = 1; ring <= 16; ++ring) {
            const float rad = 0.5f * float(ring);
            for (int step = 0; step < 16; ++step) {
                const float a = 6.2831853f * float(step) / 16.0f;
                const float x = player_.pos.x + rad * cosf(a);
                const float z = player_.pos.z + rad * sinf(a);
                if (blocked(x, z)) continue;
                opt_.camX = x;
                opt_.camZ = z;
                player_.placeOnGround(walkWorld(), x, z);
                pos_ = player_.eyePosition();
                std::printf("  spawn    stepped %.1f m clear of solid ground cover\n", rad);
                std::fflush(stdout);
                return;
            }
        }
    }

    #include "ui/app_hud.inl"
};

}  // namespace v2

