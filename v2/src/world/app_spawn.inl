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

    // -----------------------------------------------------------------------
    // IS THERE WATER HERE, AND HOW FAR.
    //
    // ONE QUESTION, TWO WORLDS. On the measured ground the imagery knows where
    // the lakes are and `CoverField::waterDistance` rings out to find one; on
    // the invented landform there is no imagery at all and a lake is whatever
    // `lakeColumn` says it is. `usingCover()` is the only thing that tells
    // them apart, and a spawn picker that asked the cover alone would silently
    // accept anywhere at all on the `--no-dem` path -- every candidate would
    // come back "no water to fail on".
    //
    // RINGS OUTWARD AND STOPS AT THE FIRST HIT, which is waterDistance's own
    // shape: a shore costs a handful of lookups and open ground costs the
    // sweep. The step is coarse (8 m) because this is a gate, not a
    // measurement -- the exact distance only has to be good enough to rank.
    // -----------------------------------------------------------------------
    float waterWithin(float x, float z, float maxM) const {
        const VoxelTerrain &t = world_.terrain;
        if (t.usingCover()) return t.cover().waterDistance(x, z, maxM);
        TerrainMemo memo;
        auto wet = [&](float px, float pz) {
            const int vi = int(floorf(px / VOXEL_M)), vj = int(floorf(pz / VOXEL_M));
            int wy = 0;
            return t.lakeColumn(vi, vj, memo, &wy);
        };
        if (wet(x, z)) return 0.0f;
        for (float r = 8.0f; r <= maxM; r += 8.0f) {
            const int n = maxi(8, int(6.2831853f * r / 8.0f));
            for (int k = 0; k < n; ++k) {
                const float a = float(k) * (6.2831853f / float(n));
                if (wet(x + cosf(a) * r, z + sinf(a) * r)) return r;
            }
        }
        return maxM;
    }

    // -----------------------------------------------------------------------
    // STAGE ONE OF A SPAWN: A BIOME AT RANDOM, AND A SHORE INSIDE IT.
    //
    // (user 2026-09-19: "currently the player spawns in the same spot
    // everytime. instead have the player spawn at different locations that
    // have water. pick a biome at random.")
    //
    // WHY THERE ARE TWO STAGES AT ALL. The search below this was never random
    // in the way it looked: it drew 512 points from a 30..400 m ring around
    // opt_.camX/camZ and took the best, and 512 points in a 0.5 km2 disc is
    // dense enough that the best one is the SAME clearing whatever the seed.
    // Measured with tests/spawn_probe.cpp over twenty seeds: mean pairwise
    // distance 212 m, closest pair 1 m, and all twenty in the birch. That is
    // the report, exactly.
    //
    // FIRST HIT, NOT BEST. This stage samples the whole window and takes the
    // first point that qualifies. Taking the best instead would be a search
    // for one global optimum, and two seeds that both find it land in the same
    // clearing -- measured, that first cut put two of twenty launches one
    // metre apart. First-hit has no optimum to converge on: the same twenty
    // seeds came out with the closest pair 645 m away.
    //
    // WATER IS A GATE HERE, not the soft penalty it is in the scoring below.
    // That penalty only ever expressed a preference, so a seed whose disc held
    // no water still spawned -- "spawn near water" producing a spawn nowhere
    // near it is a bug this file already carries one note about.
    //
    // WHAT THE SECOND STAGE IS FOR is unchanged and is the reason this one
    // stops at "somewhere in the right wood, beside water": openness, sunlight
    // and not-on-scree are what make a spawn pleasant rather than merely
    // legal, and none of that was ever the problem.
    // -----------------------------------------------------------------------
    static constexpr float kSpawnWaterM = 60.0f;

    bool pickShoreAnchor(uint32_t seed, float *outX, float *outZ, uint8_t *outWood) const {
        const VoxelTerrain &t = world_.terrain;

        // -- WHICH WOOD ------------------------------------------------------
        // A pinned world has one and rolling over five would spend most of
        // its samples looking for bands that are not there.
        //
        // -- ALL FIVE, 2026-09-19. IT WAS THREE, AND TWO OF THEM COULD NOT BE
        //    SPAWNED IN AT ALL ------------------------------------------------
        //
        // This list was written when there were three bands and neither the
        // cherry nor the desert was added to it. Two separate faults came out
        // of the one omission, and the second is the loud one:
        //
        //   * an UNPINNED world could never roll you into the cherry or the
        //     sand -- three fifths of the map, silently unreachable at boot;
        //   * and a PINNED one was worse. The forced branch below falls
        //     through to kWoodPine for anything it does not name, so --cherry
        //     and --desert asked for a pine shore in a world where woodBit
        //     answers cherry (or desert) at every single column. Nothing ever
        //     matched, so the search ran its full budget, failed, and printed
        //
        //         spawn  no shore found in the rolled wood -- fell back to the
        //                window's own anchor
        //
        //     on EVERY launch. The anchor is a legal spot, so this looked like
        //     a world with a dull spawn rather than like a broken roll.
        //
        // Derived from the biome rather than listed twice: a sixth band adds
        // itself here, which is the whole of what went wrong the last two
        // times.
        // -- ...AND IT IS THE PINE UNLESS SOMETHING SAYS OTHERWISE --------
        //
        // (user 2026-09-19: "have the person spawn in the pine forest by
        //  default".)
        //
        // THE ROLL IS WHY THIS IS ONE LINE AND NOT A REWRITE. Rolling all five
        // was the fix for two bands being unreachable at boot, and the list
        // below is still what a pinned world narrows; what was wrong was the
        // DEFAULT -- two launches in five opened on sand or blossom, and one
        // of them is not a forest at all. A first minute in the pine is the
        // wood this engine is built around.
        //
        // The other four are still one keystroke away: [G] walks the cycle
        // (see respawnToNextBiome) and /locate <wood> goes straight there, so
        // nothing has become unreachable -- it has stopped being the OPENING.
        uint8_t woods[5] = {kWoodPine, kWoodBirch, kWoodOak, kWoodCherry, kWoodDesert};
        int nW = 1;
        if (t.forced) {
            woods[0] = (t.biome == Biome::Birch)    ? kWoodBirch
                       : (t.biome == Biome::Oak)    ? kWoodOak
                       : (t.biome == Biome::Cherry) ? kWoodCherry
                       : (t.biome == Biome::Desert) ? kWoodDesert
                                                    : kWoodPine;
            nW = 1;
        }
        const uint8_t want = woods[int(hashUnit(seed + 7u, 0u) * float(nW)) % nW];
        if (outWood) *outWood = want;

        // THE DEM WINDOW, WITH THE 200 m OF MARGIN /locate ALREADY USES. A
        // window is finite and heightM holds the border sample outside it, so
        // a spawn past the edge is a spawn on a featureless plain that looks
        // exactly like the terrain having failed to load.
        const float halfX =
            t.usingDem() ? maxf(0.0f, 0.5f * t.dem().spanX() - 200.0f) : 3000.0f;
        const float halfZ =
            t.usingDem() ? maxf(0.0f, 0.5f * t.dem().spanZ() - 200.0f) : 3000.0f;

        // -- TWO PASSES, AND THE SECOND ONE DROPS THE WATER ------------------
        //
        // GIVING UP HERE IS WORSE THAN A DRY SPAWN, and it is not obvious
        // until you follow what "false" costs. The caller falls back to
        // opt_.camX/camZ, which on the DEM path is a lakeside coordinate in
        // REAL metres divided by the shrink -- fine -- and on `--no-dem` is
        // that same pair UNDIVIDED, (-12173, 8026), fourteen kilometres out on
        // an invented landform that has no window at all. The old code never
        // showed this because its non-DEM ring was centred on the ORIGIN and
        // practically always found something to keep.
        //
        // So a window with no shore in the rolled wood gets a spawn in that
        // wood anyway. Dry and in the right forest beats beside a lake in the
        // wrong one, and both beat fourteen kilometres from anywhere.
        for (int pass = 0; pass < 2; ++pass) {
            const bool wantWater = (pass == 0);
            for (uint32_t i = 0; i < 4096; ++i) {
                // A DIFFERENT STREAM PER PASS, or the second pass re-walks the
                // first one's points in the same order and its first keeper is
                // whatever the water gate rejected first -- which is a spot
                // chosen by the gate it just stopped applying.
                const float x = -halfX + 2.0f * halfX * hashUnit(seed + 11u + uint32_t(pass) * 4u, i);
                const float z = -halfZ + 2.0f * halfZ * hashUnit(seed + 13u + uint32_t(pass) * 4u, i);
                if (!(t.woodBit(x) & want)) continue;
                // The same two hard gates the scoring loop opens with -- well
                // clear of the shore rather than merely out of the water, and
                // off the scree. Asked here so a doomed anchor is never handed
                // on.
                const float h = t.heightM(x, z);
                if (h < t.waterAt(x) + 5.0f) continue;
                const int ci = int(floorf(x / VOXEL_M)), cj = int(floorf(z / VOXEL_M));
                const int slope = maxi(absi(t.heightVox(ci + 1, cj) - t.heightVox(ci - 1, cj)),
                                       absi(t.heightVox(ci, cj + 1) - t.heightVox(ci, cj - 1)));
                if (slope >= VoxelTerrain::kTreeSlope) continue;
                const float d = waterWithin(x, z, kSpawnWaterM);
                if (d < 2.0f) continue;   // that is the lake itself, either pass
                if (wantWater && d >= kSpawnWaterM) continue;
                *outX = x;
                *outZ = z;
                return true;
            }
        }
        return false;
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

        // -- STAGE ONE: WHERE TO LOOK ----------------------------------------
        //
        // A FLAG THAT NAMES A PLACE WINS. --acadia, --ouachita, --lake, --peak
        // and --front each set camX/camZ to a spot scored over their own
        // window and each of them means take me THERE; roaming would turn all
        // five into "somewhere else" while still printing a happy spawn line.
        // See Options::camPlace.
        //
        // AND A FAILED ROLL FALLS BACK TO THE OLD ANCHOR rather than to the
        // world origin, which on measured ground is a specific spot in
        // Colorado that nobody chose. A window with no shore in the rolled
        // wood is not a crash, it is a window -- --no-cover on an invented
        // landform can be exactly that -- and the spawn is still a spawn.
        //
        // -- AND A ROLL THAT LANDS ON BARE GROUND IS RE-ROLLED ---------------
        //
        // Stage one only promises a shore in the right wood; whether there is
        // a GLADE within 400 m of it is the ring's question, and on rocky
        // high ground the answer is no. Measured with tests/spawn_probe.cpp:
        // two launches in twenty found nothing inside the openness band and
        // fell back to the anchor -- which is a legal spawn on bare scree with
        // a lake 41 m away, and reads as the picker having given up. One spot
        // like that was rendered to check, and it is: no trees, no glade, the
        // reply saying "nothing better found".
        //
        // THE ANCHOR IS THE CHEAP HALF, so re-rolling it is what to do about
        // that -- three tries takes the 10% to about one launch in a thousand.
        // Each attempt salts the seed so it draws a different anchor AND a
        // different ring; re-running the same two streams would find the same
        // nothing three times.
        float anchorX = opt_.camX, anchorZ = opt_.camZ;
        uint8_t rolled = 0;
        bool roamed = false;

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
        // THE FALLBACK IS THE ANCHOR, which stage one already proved is
        // standable, off the scree and beside water. It used to be
        // opt_.camX/camZ -- fine when the ring was centred there and a
        // coordinate from another world once it was not.
        float anyX = opt_.camX, anyZ = opt_.camZ, anyScore = 1e9f;

        for (uint32_t attempt = 0; attempt < 3 && !found; ++attempt) {
            const uint32_t sd = seed + attempt * 0x9E37u;
            if (!opt_.camPlace) {
                roamed = pickShoreAnchor(sd, &anchorX, &anchorZ, &rolled);
                // A window with no shore in the rolled wood will not grow one on
                // the next try, so there is nothing to re-roll for.
                if (!roamed) break;
            }
            // THE FIRST ATTEMPT'S ANCHOR IS THE FALLBACK. Once the ring below
            // has scored anything at all, anyScore is set and this stops
            // firing -- so a run where no attempt finds a glade keeps the
            // first anchor, which stage one already proved is standable and
            // beside water.
            if (anyScore > 1e8f) {
                anyX = anchorX;
                anyZ = anchorZ;
            }

            for (uint32_t i = 0; i < 512; ++i) {
                // A disc, sampled with a square root so the points are spread over
                // the AREA rather than piled up near the middle.
                // ------------------------------------ SEARCH AROUND THE ANCHOR
                // This ring used to be centred on the WORLD ORIGIN, which on an
                // invented landform is as good a place as any -- every direction
                // looks the same. On measured ground it is a specific spot in
                // Colorado that nobody chose, and it threw the search kilometres
                // away from the lakeside point opt_.camX/camZ names. The radius
                // shrinks with it: 3 km of wander is how you leave the shore.
                //
                // AND THE CENTRE IS STAGE ONE'S NOW, not opt_.camX/camZ. That is
                // the whole of what made every launch land in one clearing: the
                // ring was fine, it was only ever asked around ONE point. It stays
                // TIGHT even when nothing anchored it -- the 200..3000 m arm is
                // gone, because a shore anchor is a place worth staying near and
                // three kilometres of wander is how you leave it.
                const float r = 30.0f + 370.0f * sqrtf(hashUnit(sd + 1u, i));
                const float a = hashUnit(sd + 2u, i) * 6.2831853f;
                const float x = anchorX + r * cosf(a), z = anchorZ + r * sinf(a);
                // AND IT STAYS IN THE WOOD THAT WAS ROLLED. A band is 800 m wide
                // and this ring reaches 400, so an anchor near a band edge can
                // hand back a glade in the NEXT wood -- which is harmless to stand
                // in and makes "pick a biome at random" false. The anchor is
                // itself in the band, so there is always something left to pick.
                if (roamed && !(t.woodBit(x) & rolled)) continue;

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

                // WATER IS PART OF THE SCORE NOW, not a hope. openness() alone
                // picks a sunlit clearing and does not care whether it can see a
                // lake -- which is why "spawn near water" produced a spawn nowhere
                // near water even with the coordinates measured off the shore.
                // Lower is better here, so distance is a penalty; beyond kWantM it
                // stops mattering and openness decides again.
                float score = openness(x, z);
                if (t.usingCover()) {
                    // A SHORE, NOT A SWIM. Penalising distance alone drove every
                    // seed to distance ZERO -- standing in Cheesman. The depth
                    // guard above could not catch it either: it asks waterAt(),
                    // which is the PROCEDURAL waterline and is unset on the DEM
                    // path, so a lake the imagery knows about is invisible to it.
                    //
                    // Anything inside kWantM is equally good, so the openness term
                    // still chooses between shoreline sites rather than being
                    // overridden by a metre of distance.
                    const float kWantM = 40.0f;
                    const float d = t.cover().waterDistance(x, z, kWantM * 3.0f);
                    if (d < 2.0f) continue;               // that is the lake itself
                    score += 0.9f * minf(1.0f, maxf(0.0f, d - kWantM) / kWantM);
                }
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
        }   // ...and try another anchor if this one had no glade near it
        if (!found) {
            bestX = anyX;
            bestZ = anyZ;
            bestScore = anyScore;
        }

        opt_.camX = bestX;
        opt_.camZ = bestZ;
        // -- AND THE LINE SAYS WHICH WOOD IT ROLLED --------------------------
        //
        // The one thing a spawn line could never answer was "why am I here
        // again", and with a biome now being CHOSEN it is the first thing to
        // check when a run looks wrong. It prints the wood UNDERFOOT rather
        // than the bit that was rolled -- the loop keeps the two the same, so
        // a line that disagrees with the roll is a report about the loop
        // rather than a reassuring echo of the roll.
        std::printf("  spawn    %.1f, %.1f  the %s wood  openness %.2f%s  "
                    "(--spawn %u to come back here)\n",
                    bestX, bestZ, t.woodName(bestX), double(bestScore),
                    found ? "" : " -- nothing better found", unsigned(seed));
        if (!roamed && !opt_.camPlace)
            std::printf("  spawn    no shore found in the rolled wood -- fell back to the "
                        "window's own anchor\n");
        std::fflush(stdout);
        if (world_.terrain.usingCover()) {
            const float d = world_.terrain.cover().waterDistance(opt_.camX, opt_.camZ, 400.0f);
            printf("  spawn    water is %.0f world m away\n", d);
            // AND LOOK AT IT. Spawning 24 m from a lake while facing a
            // boulder is indistinguishable from not spawning near a lake at
            // all -- the request was about what you SEE. yaw = atan2(dx, -dz)
            // is this camera's convention: direction() is
            // (sin y, ., -cos y), so -z is yaw 0.
            float wx = 0.0f, wz = 0.0f;
            if (!opt_.yawGiven &&
                world_.terrain.cover().nearestWater(opt_.camX, opt_.camZ, 400.0f, &wx, &wz)) {
                opt_.yaw = atan2f(wx, -wz) * 180.0f / PI;
                printf("  spawn    facing it, yaw %.0f\n", opt_.yaw);
            }
        }
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

