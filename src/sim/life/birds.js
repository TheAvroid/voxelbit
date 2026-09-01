  // ── flying cardinal ── free WANDERING flight (no home, no leash — it goes where it goes; it does NOT follow the player): the turn rate
  // drifts to a new random target every few seconds (varied, believable arcs — never a fixed circle) and it terrain-follows via hmap so it
  // always clears the canopy. Pose is written into drop slot 4 each frame (see the drops uniform section) and DDA-raytraced off-grid —
  // smooth rotation, depth-tested, no world-grid writes.
  // ── THE FLOCK IS SIZED TO ITS SLOTS (user 2026-08-05: "flying birds are still dissapearing over the lake") ──
  // capping the draw at the nearest 5 of 12 freed six slots for the creatures that were being starved, but it
  // traded one disappearing act for another: a flock is always in motion, so which five are nearest changes
  // constantly and birds swapped in and out of existence in front of the player — worst over open water, where
  // there is nothing for them to be hidden behind. A hard cap on a moving set can only ever look like that.
  // So the flock is sized to the slots instead: BIRD_N - 1 birds share the drop slots (bird 0 has its own,
  // slot 4), and BIRD_SLOTS covers every one of them. Nothing is selected, so nothing can be deselected.
  // 12 -> 9 birds costs three of the flock and still leaves the traced creatures 31 slots, up from the 28 they
  // had before any of this.
  const BIRD_N = 9;
  const BIRD_RING_N = 12;                              // ring samples per placement, 30 degrees apart — every legal one is a candidate (see the placement below)
  const BIRD_SLOTS = BIRD_N - 1;                       // every flying bird that is not bird 0 gets a slot, always. Bird 0 owns the dedicated drop slot 4
  // 9 divides by 3, which is the species count today (cardinal / blue_bird / robin) so the round-robin split
  // below still comes out exact — three of each. It was 12 to divide by 1/2/3/4; if a FOURTH species is ever
  // added, take this back to 12 and give the traced creatures their slots back some other way, because an
  // uneven split shows up as one species being rarer than the others rather than as anything subtle.
  let birdKills = 0;                                   // songbirds brought down by a shaft — the flock refills itself, so a headcount alone proves nothing
  const birds = Array.from({ length: BIRD_N }, () => ({ th: 0, turnBias: 0, om: 0, omT: 0, altO: 0, altT: 0, tRe: 0, g: 0, pyPrev: 0, vyS: 0, glid: false, flapT0: -1, init: false, x: 0, y: 0, z: 0, mode: 0, swoopT0: 0, swoopA: 0, swO: 0, edge: false, fi: 0, sp: 0 }));   // the rest ride the compacted creature slots. mode: 0 wander / 1 thermal soar / 2 swoop
  const bird = birds[0];                               // the original singleton name, kept for the editor path + the primary hitbox
  const birdStep = (b, bi, tb, dt) => {                // one bird's flight for this frame → its world pose. Identical maths for every bird; only the seed state differs.
        // ── PROCEDURAL POPULATION ── the flyers follow the SAME ring rule as every other creature: a bird that falls
        // more than LIFE_KEEP behind is recycled into the LIFE_IN..LIFE_OUT band around wherever the player is now.
        // Without this the six spawned once at world start and the sky went empty the moment you walked away.
        // The ground-creature band (0.78-0.94 of the keep radius) puts a flyer on the horizon where it is a single
        // pixel — fine for a worm you walk up to, useless for a bird meant to be seen. Birds ride a TIGHTER ring:
        // they arrive well inside the view and cross it. Nothing pops, because at 400+ voxels and 14 m up a bird is
        // a speck against open sky either way.
        // ── AND THEY ARRIVE FROM THE FOG, NOT OUT OF CLEAR SKY (user 2026-08-20: "the song birds seem to just
        // appear out of the sky … do they not have the same render distance as everything else?") ── they did
        // not, and it was deliberate: this band was 0.24-0.50 of the keep radius against the ground life's
        // 0.78-0.94, on the reasoning that a bird placed on the horizon is a single pixel and one meant to be
        // SEEN should arrive well inside the view. The note even claimed nothing pops "because at 400+ voxels
        // and 14 m up a bird is a speck against open sky". That is the flaw: a speck against OPEN SKY is
        // exactly where a new one is easiest to catch, because there is nothing else up there to look at.
        // A bird recycles at bKeep, so 0.24-0.50 also meant it appeared less than halfway out and then had to
        // cross the whole ring again — the shortest possible time between an appearance and the next one.
        // Same band as every other creature now: it fades in at the fog line and flies toward you, which is
        // what "the same render distance as everything else" means.
        const bKeep = Math.min(renderDist + 64, 1040), bOut = bKeep * 0.94, bIn = bKeep * 0.78;
        if (b.init) { const ddx = b.x - P.x, ddz = b.z - P.z;
          if (ddx * ddx + ddz * ddz > bKeep * bKeep) b.init = false; }   // left the ring → respawn it ahead of you instead
        // ── FORESTS ONLY, NEVER THE DESERT (user 2026-08-17: "the birds should be oak and pine forests only.
        // I only want them disabled in the desert") ── the flock's FIRST biome test; birds.js had none, which
        // is why restoring the species list put birds over the sand as well. TWO different thresholds on
        // purpose, and the pair is the whole design:
        //   * BIRD_OUT 0.85 RECYCLES a bird that has flown deep over the dunes. Not 0.5, because a bird is
        //     the one creature that SHOULD be able to cross a treeline — clipping it off at the halfway line
        //     would read as an invisible wall in open sky, which is exactly what the ground creatures' own
        //     gate comment warns about. It drifts out over the sand and is recycled once it is properly out.
        //   * BIRD_IN 0.35 refuses a SPAWN. Tighter than the recycle line so a fresh bird never appears
        //     already half-way to being culled, which would flicker the flock along the border.
        // The two are the same shape as the ground life's BIO_DESERT/BIO_FOREST pair and for the same reason.
        // BIRD_TURN is where the flock BANKS BACK, and it is the line that actually contains them now — see the
        // turn-away in the flight section below. BIRD_GRAD is how far apart the mask is sampled to find which way
        // out is; BIRD_POP is how far a bird must be before the backstop recycle is allowed to fire at all.
        const BIRD_OUT = 0.85, BIRD_IN = 0.35, BIRD_TURN = 0.45, BIRD_GRAD = 64, BIRD_POP2 = 420 * 420;
        // ── THE ONE KEEP-OUT MASK THE FLOCK READS ── sand and ice are the same question to a songbird ("is
        // this sky mine"), so the wall, the recycle and the spawn ring all ask it through here and cannot
        // drift apart. The three were already computing max(desertM, arcticM) inline in two of the three.
        const birdKeep = (x, z) => Math.max(desertM(x, z), arcticM(x, z));
        // ── AND THE BLOSSOM IS THE OTHER PLACE THE FLOCK DOES NOT GO (user 2026-08-18: "dont spawn any
        // songbirds") ── this file gated on desertM ALONE, so the sky over every forest was the same sky; a
        // cherry forest is "not desert" and the cardinals, blue birds and robins would have kept crossing it.
        // The cheap-out is the same one the ground gates use: cherryM is a sub-region of the oak mask, so a
        // bird over the pine forest or the desert never pays for the second sample.
        const chOut = (x9, z9) => chNear(x9) && cherryM(x9, z9) > 0.5;   // 0.5 — the number the TREE uses (world/terrain.js `blos`), not BIRD_IN, which is a DESERT constant that happened to be in scope. At 0.35 every flying bird in the 0.35..0.5 strip was pink while the oaks under it were still green. chNear, not `oakM > 0`: oakM is true across the whole infinite oak forest, so it was never a cheap-out at all
        // ── AND THE RECYCLE IS SPECIES-AWARE, BOTH WAYS ── an ordinary songbird is retired when it drifts INTO
        // the blossom, and a pink bird when it drifts OUT of it. Only the first half was written at first, and
        // the flock is free-flying: a pink bird simply flew across the border and kept going, measured at
        // oakM 0.2 with no blossom under it at all. A one-directional containment on a moving body is not
        // containment, which is the same lesson BIO_CHLINE records for the walkers.
        const pinkMe = BIRD_PINK >= 0 && b.sp === BIRD_PINK;
        // ── AND THEY ROOST AT NIGHT (user 2026-08-20: "have the song birds stop flying at night") ── the sun's
        // own elevation, derived from tday exactly as main/tick-camera.js derives it, so this cannot drift out
        // of step with the sky: ang -> el -> sin(el) IS the y of the sun vector that lights the world.
        // TWO THRESHOLDS, not one, and they are the same shape as the desert's BIRD_OUT/BIRD_IN pair above and
        // for the same reason: a single line at dusk would put a bird one flap either side of it into a spawn/
        // retire loop along the boundary. They stop being PLACED while the sun is under 0.06 and the ones
        // already up are retired at 0.02, so the flock thins out through the last of the light and the sky is
        // empty by dark rather than emptying with a pop.
        // Retired, not landed: a songbird has no perched pose in this system — the birds that sit in trees are
        // a separate, grid-stamped population (see the perched-bird stamp) and they are already there all
        // night. This is the FLYING flock going to roost, which is what an empty night sky means.
        const sunEl = Math.sin(Math.sin(tday * Math.PI * 2 - Math.PI / 2) * 1.05);
        // ── THE FLOCK GOES TO ROOST A BIRD AT A TIME (user 2026-08-20: "the song birds in the sky seem to be
        // dissapeared too") ── the pair of thresholds this replaces did stop the boundary flicker they were
        // written for, but they still retired all nine birds on the SAME frame, so the sky emptied in one
        // blink. Measured across dusk: sky 9 -> 0 between two samples. Same shape as the ground's dusk ramp in
        // main/tick-life.js, and the identical window (+0.10 down to -0.06, ending where moonMode takes the
        // sky), so the birds land while the meadow is thinning rather than after it: the flock SIZE follows
        // the light, and a slot beyond it goes home. Monotonic through dusk and dawn, so there is nothing for
        // a bird on the boundary to oscillate against — which is what the two thresholds were guarding.
        const roostT = Math.max(0, Math.min(1, (sunEl + 0.06) / 0.16));
        const roostK = roostT * roostT * (3 - 2 * roostT);
        const flockUp = Math.round(BIRD_N * roostK);   // how many of the nine still have light to fly in
        if (b.init && bi >= flockUp) b.init = false;
        // ── AND THE DESERT RECYCLE MAY NOT FIRE IN PLAIN VIEW (user 2026-08-26: "when a bird seems to fly over
        // the desert, it just instantly dissapears") ── this is a hard `init = false` and the block right below
        // re-places the slot out past the fog in the SAME call, so a bird crossing the line where you can see it
        // does not fly away, it BLINKS. The threshold pair was picked on the assumption that deep desert is also
        // far away, but desertM is sampled AT THE BIRD, not at the player: stand at the dune edge and a bird a
        // hundred voxels out is already past 0.85. Two changes, and the first is the one that matters:
        // the flock now banks back at BIRD_TURN (below), so a bird should never reach BIRD_OUT while you are
        // watching; and if one still does, the recycle waits until it is BIRD_POP away, where a 12-voxel bird
        // against the sky is a couple of pixels. Distance-gating the DESERT arm only — the blossom containment is
        // a different report with a different shape and is left exactly as it was.
        const dpv2 = (b.x - P.x) * (b.x - P.x) + (b.z - P.z) * (b.z - P.z);
        // ── AND THE ARCTIC (user 2026-08-29) ── the third sky the flock stays out of, on the desert's own pair of
        // thresholds: BIRD_OUT recycles one that has drifted deep over the ice, BIRD_IN refuses the spawn.
        // These are SONGBIRDS — cardinals, blue birds and robins over a glacier is the same category of wrong as
        // them over open sand, and the flock is not part of LIFE_WANT so the population damping never saw it.
        if (b.init && ((birdKeep(b.x, b.z) > BIRD_OUT && dpv2 > BIRD_POP2) || (pinkMe ? !chOut(b.x, b.z) : chOut(b.x, b.z)))) b.init = false;
        if (!b.init) {                                // placed out past the fog, never in plain view, and staggered so they never read as a formation
          // ── AND IF THERE IS NOWHERE LEGAL, DO NOT PLACE IT AT ALL ── stand deep in the desert and every
          // candidate on the ring is sand, so the flock has to be able to answer "none". Eight tries around
          // the ring (the ring is a circle; eight covers it) and then `b.off`, which the two draw paths skip
          // exactly the way they already skip a ragdolling bird. Without this a bird with no legal spot would
          // keep its x/z of 0 and be drawn at the world origin.
          // Half the flock looks for blossom, half for ordinary forest, split by slot so the two populations are
          // stable rather than racing: stand in the cherry forest and the pink half fills the sky while the
          // other half simply reports `off`, and the reverse in the oak wood. With no pink bird loaded this is
          // always false and every bird takes the old path unchanged.
          // ── DECIDED PER CANDIDATE POINT, NOT PER SLOT (audit 2026-08-18) ── this was `(bi & 1) === 0`, a pure
          // function of the slot number, and the comment below already claimed it was per point. The consequence
          // was that deep in the oak forest every EVEN slot failed all eight ring tries and set b.off forever,
          // and deep in the blossom every odd slot did — half the flock permanently unplaceable, re-testing
          // eight ring points a frame to keep failing. The species is now read OFF the point that was accepted,
          // so any slot can take any legal spot and the two skies stay disjoint just the same.
          let pinkHere = false;
          let a0 = 0, r0 = 0, ok = false;
          // ── EVERY LEGAL ANGLE, THEN PICK ONE ── this loop used to stop at the FIRST legal spot while turning
          // a fixed +45 degrees per try, and that is a boundary attractor as surely as the rect-centre one the
          // flight code above replaced. Walk in from the desert and the legal arc is whatever lies forest-side:
          // every bird whose own base angle points at the sand fails, rotates the SAME WAY, and lands just past
          // the same edge of that arc — nine birds converging on one bearing, which is the knot at the treeline
          // (user 2026-08-26: "still cluster a bunch of song birds into one area … right at the edge of the pine
          // forest"). MEASURED standing there before this: up to 4 of the 9 inside 220 voxels of one another.
          // Collecting the legal angles and choosing UNIFORMLY among them spreads the flock across the whole arc
          // instead of stacking it against the boundary, for the same handful of mask samples the loop already
          // spent. The per-bird base angle and its jitter stay — they are what keeps two birds off the same slot
          // when the ring is entirely legal, which is the deep-forest case this must not disturb.
            // ── AND THE BLOSSOM SWAPS THE FLOCK RATHER THAN EMPTYING IT ── the pink bird is admitted ONLY
            // inside the band and the three ordinary songbirds ONLY outside it, so the two skies are disjoint
            // and a bird is never drawn over the wrong forest. the species is decided per CANDIDATE POINT, not per
            // slot, because a slot recycles and the player walks: the same slot legitimately carries a robin
            // over the oak wood and a pink bird ten minutes later inside the blossom.
          const ringA = [], ringR = [], ringC = [];
          for (let q = 0; q < BIRD_RING_N; q++) {
            const aq = (bi / BIRD_N) * Math.PI * 2 + Math.random() * 1.4 + q * (Math.PI * 2 / BIRD_RING_N);
            const rq = bIn + Math.random() * Math.max(1, bOut - bIn);
            const bx9 = P.x + Math.cos(aq) * rq, bz9 = P.z + Math.sin(aq) * rq;
            const inCh = chOut(bx9, bz9);
            if (bi < flockUp && birdKeep(bx9, bz9) <= BIRD_IN && (!inCh || BIRD_PINK >= 0)) { ringA.push(aq); ringR.push(rq); ringC.push(inCh); }
          }
          if (ringA.length) { const p9 = (Math.random() * ringA.length) | 0;
            a0 = ringA[p9]; r0 = ringR[p9]; pinkHere = ringC[p9]; ok = true; }
          b.off = !ok;
          if (!ok) return null;                       // no forest within the ring — the sky over the desert stays empty
          b.x = P.x + Math.cos(a0) * r0; b.z = P.z + Math.sin(a0) * r0;
          b.x = Math.max(rect.xlo + 60, Math.min(rect.xhi - 60, b.x));   // never outside the generated rect — the hmap is stale there and the terrain-follow flies blind
          b.z = Math.max(rect.zlo + 60, Math.min(rect.zhi - 60, b.z));
          b.g = hmap[gwrap(Math.floor(b.x), WX) + gwrap(Math.floor(b.z), WZ) * WX] || P.y;
          b.th = Math.random() * Math.PI * 2; b.pyPrev = b.g + BIRD_ALT; b.init = true;
          b.turnBias = (Math.random() - 0.5) * 1.3;   // ±0.65 rad — what stops the whole flock taking the SAME escape heading off the same wall (see the edge block below)
          b.dying = false; b.rag = false; b.ragBody = null; b.ragParts = null; b.ragIt = 0;   // a recycled slot must not inherit the last bird's death: `rag` would keep it from ever being drawn, and a stale ragIt would build the next corpse from the wrong pose
          b.altO = Math.random() * 30; b.tRe = tb + Math.random() * 3;   // staggered whim timers → independent behaviour
          b.flapT0 = tb - Math.random() * 0.5;         // desynced wingbeats
          b.om = 0; b.vyS = 0; b.swO = 0; b.mode = 0; b.glid = false; b.off = false;
          // ── SPECIES ── fixed by SLOT, never rolled: a coin flip drifts, this holds an exact even split no
          // matter how many recycle. A bird that found blossom takes the pink strip; everything else keeps the
          // old round-robin, and it is taken over the ORDINARY species only (FLYERS minus the derived pink one)
          // so adding the fourth did not quietly put pink birds over the oak forest.
          const nOrd = Math.max(1, FLYERS.length - (BIRD_PINK >= 0 ? 1 : 0));
          b.sp = pinkHere ? BIRD_PINK : (FLYERS.length ? (bi % nOrd) : 0);
        }
        if (tb > b.tRe) {                             // pick the next BEHAVIOUR, not just a turn rate — that is what reads as intent instead of drift
          const r = Math.random();
          if (r < 0.18) {                                // THERMAL SOAR: settle into a steady banked circle, wings locked out, drifting a little higher
            b.mode = 1;
            b.omT = (Math.random() < 0.5 ? 1 : -1) * (0.45 + Math.random() * 0.35);
            b.altT = 18 + Math.random() * 26;
            b.tRe = tb + 6 + Math.random() * 7;
          } else if (r < 0.34) {                         // SWOOP: fold in and dive, trade the height for speed, then bleed it back into the climb-out
            b.mode = 2; b.swoopT0 = tb;
            b.swoopA = 12 + Math.random() * Math.min(26, 6 + b.altO);   // never dives deeper than the height band it actually has in hand
            b.omT = (Math.random() - 0.5) * 0.4;
            b.tRe = tb + 3.2;
          } else {                                       // WANDER: a fresh turn-rate + altitude urge, as before
            b.mode = 0;
            b.omT = (Math.random() - 0.5) * 1.0;      // rad/s: anything from a lazy drift to a tightish arc, either direction
            b.altT = Math.random() * 34;              // cruise 0–34 vox above the minimum safe height
            b.tRe = tb + 1.5 + Math.random() * 3.0;
          }
        }
        // WORLD-EDGE AWARENESS: past the generated rect the hmap is stale and the terrain-follow flies blind — bank
        // back toward the middle well before the edge. Not a player leash (the bird still never follows you); it just
        // refuses to fly out of the world.
        if (b.x < rect.xlo + 90 || b.x > rect.xhi - 90 || b.z < rect.zlo + 90 || b.z > rect.zhi - 90) {
          // ── STEER OFF THE WALL, NOT AT A SHARED POINT (user 2026-08-20: "a very large amount of birds will
          // fly in one location") ── this used to aim every edge bird at the rect CENTRE, which is a single
          // world point and therefore a point ATTRACTOR: every bird that touches the boundary flies the same
          // course to the same spot, and the ones that get there first are still circling it when the next
          // arrive. That is a flock knot, and it is worse the longer birds spend in the edge band.
          // WHY IT SHOWS UP WHEN YOU WALK INTO THE PINES and not standing still: the band is measured off the
          // GENERATED rect, and the rect only tracks you as fast as generation can build it. Pine forest is
          // the heaviest terrain in the world to generate, so walking into it is exactly when the rect lags
          // furthest behind the player — the interior shrinks, birds ahead of you fall into the edge band in
          // numbers, and they all set off for the same coordinate. On a fast machine the rect keeps up (a
          // harness walk measured a 33-voxel lag and never more than 2 birds in the band at once), which is
          // why this reproduces on a real session and not in a test.
          // The fix keeps the purpose — never fly out of the world, where the hmap is stale and the
          // terrain-follow flies blind — and drops the shared destination: turn away from the wall (or walls)
          // actually being approached, along the INWARD NORMAL, offset by a per-bird bias so the flock fans
          // out along the boundary instead of collapsing onto one heading.
          let ix = 0, iz = 0;
          if (b.x < rect.xlo + 90) ix = 1; else if (b.x > rect.xhi - 90) ix = -1;
          if (b.z < rect.zlo + 90) iz = 1; else if (b.z > rect.zhi - 90) iz = -1;
          const want = Math.atan2(ix, iz) + b.turnBias;   // a corner gives the diagonal, which is still the way out
          let dth = want - b.th;
          while (dth > Math.PI) dth -= 2 * Math.PI; while (dth < -Math.PI) dth += 2 * Math.PI;
          b.omT = Math.max(-1.0, Math.min(1.0, dth * 1.2)); b.mode = 0; b.edge = true;   // the steer-home urge overrides whatever whim it was on
        // ── AND THE SAND IS A WALL TOO ── same steer, same per-bird bias, one threshold earlier than the recycle.
        // The inward normal is the falling direction of the mask itself, sampled at ±BIRD_GRAD on each axis, so it
        // works on both faces of the band (the sand is a BAND with pine on both sides — see world/window.js) with
        // no notion of which side the bird is on. Deep enough in and the mask saturates flat, which reads as a zero
        // gradient; that case just turns round. Four mask samples, and only for a bird actually in the band.
        // The world edge above still wins: flying out of the generated rect is the worse failure of the two.
        // ── AND THE ICE IS THE SAME WALL (user 2026-08-30: "dont let flying birds enter the arctic. have them
        // turn around back into their respective enviornment") ── the arctic joins the sand in THIS block rather
        // than getting one of its own: birdKeep is max(sand, ice), so one threshold, one gradient and one steer
        // serve both, and a bird between the two banks away from whichever is nearer. The recycle above keeps the
        // same pair and stays one threshold further out — the backstop for a bird that crosses anyway, not the
        // mechanism. Before this the arctic had ONLY that recycle, which is exactly why songbirds vanished over
        // the ice instead of turning back from it.
        } else if (birdKeep(b.x, b.z) > BIRD_TURN) {
          const gx = birdKeep(b.x + BIRD_GRAD, b.z) - birdKeep(b.x - BIRD_GRAD, b.z);
          const gz = birdKeep(b.x, b.z + BIRD_GRAD) - birdKeep(b.x, b.z - BIRD_GRAD);
          const want = (gx || gz) ? Math.atan2(-gx, -gz) + b.turnBias : b.th + Math.PI;
          let dth = want - b.th;
          while (dth > Math.PI) dth -= 2 * Math.PI; while (dth < -Math.PI) dth += 2 * Math.PI;
          b.omT = Math.max(-1.0, Math.min(1.0, dth * 1.2)); b.mode = 0; b.edge = true;
        } else b.edge = false;
        b.om += (b.omT - b.om) * (1 - Math.exp(-2.5 * dt));   // turn rate eases toward target — smooth, believable arcs, no snap turns
        b.om = Math.max(-1.1, Math.min(1.1, b.om));
        b.th += b.om * dt;
        const Hx = Math.sin(b.th), Hz = Math.cos(b.th);   // horizontal heading; the beak (model −depth) points along it
        const spd = BIRD_SPD * (1 + Math.max(-0.28, Math.min(0.5, -b.vyS * 0.045)));   // ENERGY EXCHANGE: a dive buys speed (up to +50%), a climb costs it — the swoop reads as physics, not animation
        b.x += Hx * spd * dt; b.z += Hz * spd * dt;
        const gHere = hmap[gwrap(Math.floor(b.x), WX) + gwrap(Math.floor(b.z), WZ) * WX];   // terrain-follow, ASYMMETRIC: climb fast ahead of rising ground, sink only gently —
        const gNext = hmap[gwrap(Math.floor(b.x + Hx * 45), WX) + gwrap(Math.floor(b.z + Hz * 45), WZ) * WX];   // — so crossing a gorge is a mild swoop, not a plunge below the rims
        const gT = Math.max(Math.max(gHere, gNext), WL);   // never track below sea level (also guards hmap zeros beyond the generated rect once it wanders far)
        b.g += (gT - b.g) * (1 - Math.exp(-(gT > b.g ? 3.0 : 0.35) * dt));
        b.altO += (b.altT - b.altO) * (1 - Math.exp(-0.4 * dt));
        const swT = b.mode === 2 ? -b.swoopA * Math.sin(Math.PI * Math.min(1, (tb - b.swoopT0) / 3.2)) : 0;   // half-sine dive-and-recover over the swoop's 3.2 s
        b.swO += (swT - b.swO) * (1 - Math.exp(-4 * dt));   // eased, so an interrupted swoop recovers smoothly instead of popping
        const py3 = Math.max(b.g + 122, b.g + BIRD_ALT + b.altO + b.swO + Math.sin(tb * 1.2) * 3.0);   // safe height + wander band + swoop + flap bob; the floor keeps every dive above the tallest pine (116)
        const px3 = b.x, pz3 = b.z;
        const vy = (py3 - b.pyPrev) / Math.max(dt, 1e-4); b.pyPrev = py3;   // raw vertical velocity — NOISY (integer hmap samples step as voxel boundaries cross)
        b.vyS += (vy - b.vyS) * (1 - Math.exp(-5.0 * dt));   // low-passed — attitude and the flap↔glide switch both read THIS, or the pitch snaps and the modes flicker
        b.y = py3;
        const Fy = Math.max(-0.6, Math.min(0.6, b.vyS / BIRD_SPD * 3.0));   // climb/dive attitude (×3 so the gentle bob reads at a glance)
        const fl2 = Math.hypot(1, Fy);
        const F = [Hx / fl2, Fy / fl2, Hz / fl2];        // 3D flight direction (pitch included)
        const xl = Math.hypot(F[0], F[2]) || 1;
        const Xw0 = [F[2] / xl, 0, -F[0] / xl];          // wingspan = up × F, normalised — for om > 0 this side faces the turn centre
        const Zw0 = [F[1] * Xw0[2], F[2] * Xw0[0] - F[0] * Xw0[2], -F[1] * Xw0[0]];   // body-up = F × Xw0 (unit: F ⊥ Xw0)
        const bank = Math.max(-0.5, Math.min(0.5, spd * b.om / GRAVITY * 1.8));   // v·ω/g at the LIVE speed (signed) — rolls INTO whichever way it's turning, harder when it is moving faster
        const cb = Math.cos(bank), sb = Math.sin(bank);
        const Xw = [Xw0[0] * cb - Zw0[0] * sb, Xw0[1] * cb - Zw0[1] * sb, Xw0[2] * cb - Zw0[2] * sb];   // roll about F: the wing on the turn-centre side dips
        const Zw = [Zw0[0] * cb + Xw0[0] * sb, Zw0[1] * cb + Xw0[1] * sb, Zw0[2] * cb + Xw0[2] * sb];
        const Yw = [-F[0], -F[1], -F[2]];                // tail — right-handed with the above (Yw×Zw = Xw)
        b.glid = false;                            // COASTING REMOVED (user) — the bird never holds the spread-wing glide frame; it cycles its flap strip continuously
        if (b.flapT0 < 0) b.flapT0 = tb;           // keep the wing cycle anchored at frame 00 (never reset to -1 now that glide is gone)
        const STR = FLYERS[b.sp] || FLYERS[0];         // which strip this individual flies on — identical rig either way (F is already the flight vector above)
        const sBase = STR.item0, sN = STR.n, sGl = STR.glide;
        let fi = Math.floor((tb - b.flapT0) * BIRD_FLAP) % sN;   // always cycle through the flap frames — no glide hold
        if (tb % 3.4 < 0.16) fi += sN;                   // BLINK — every ~3.4 s the eye flashes plumage-red for a beat (the +N..+2N-1 blink variants)
        b.fi = fi; b.gliding = b.glid;               // debug taps (__vb.bird)
    return { x: px3, y: py3, z: pz3, Xw, Yw, Zw, fi, item: sBase + fi, glideItem: sBase - 1 + sGl };
  };
