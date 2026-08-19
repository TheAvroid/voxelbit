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
  const BIRD_SLOTS = BIRD_N - 1;                       // every flying bird that is not bird 0 gets a slot, always. Bird 0 owns the dedicated drop slot 4
  // 9 divides by 3, which is the species count today (cardinal / blue_bird / robin) so the round-robin split
  // below still comes out exact — three of each. It was 12 to divide by 1/2/3/4; if a FOURTH species is ever
  // added, take this back to 12 and give the traced creatures their slots back some other way, because an
  // uneven split shows up as one species being rarer than the others rather than as anything subtle.
  let birdKills = 0;                                   // songbirds brought down by a shaft — the flock refills itself, so a headcount alone proves nothing
  const birds = Array.from({ length: BIRD_N }, () => ({ th: 0, om: 0, omT: 0, altO: 0, altT: 0, tRe: 0, g: 0, pyPrev: 0, vyS: 0, glid: false, flapT0: -1, init: false, x: 0, y: 0, z: 0, mode: 0, swoopT0: 0, swoopA: 0, swO: 0, edge: false, fi: 0, sp: 0 }));   // the rest ride the compacted creature slots. mode: 0 wander / 1 thermal soar / 2 swoop
  const bird = birds[0];                               // the original singleton name, kept for the editor path + the primary hitbox
  const birdStep = (b, bi, tb, dt) => {                // one bird's flight for this frame → its world pose. Identical maths for every bird; only the seed state differs.
        // ── PROCEDURAL POPULATION ── the flyers follow the SAME ring rule as every other creature: a bird that falls
        // more than LIFE_KEEP behind is recycled into the LIFE_IN..LIFE_OUT band around wherever the player is now.
        // Without this the six spawned once at world start and the sky went empty the moment you walked away.
        // The ground-creature band (0.78-0.94 of the keep radius) puts a flyer on the horizon where it is a single
        // pixel — fine for a worm you walk up to, useless for a bird meant to be seen. Birds ride a TIGHTER ring:
        // they arrive well inside the view and cross it. Nothing pops, because at 400+ voxels and 14 m up a bird is
        // a speck against open sky either way.
        const bKeep = Math.min(renderDist + 64, 1040), bOut = bKeep * 0.50, bIn = bKeep * 0.24;
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
        const BIRD_OUT = 0.85, BIRD_IN = 0.35;
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
        if (b.init && (desertM(b.x, b.z) > BIRD_OUT || (pinkMe ? !chOut(b.x, b.z) : chOut(b.x, b.z)))) b.init = false;
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
          for (let q = 0; q < 8 && !ok; q++) {
            a0 = (bi / BIRD_N) * Math.PI * 2 + Math.random() * 1.4 + q * 0.7854;
            r0 = bIn + Math.random() * Math.max(1, bOut - bIn);
            const bx9 = P.x + Math.cos(a0) * r0, bz9 = P.z + Math.sin(a0) * r0;
            // ── AND THE BLOSSOM SWAPS THE FLOCK RATHER THAN EMPTYING IT ── the pink bird is admitted ONLY
            // inside the band and the three ordinary songbirds ONLY outside it, so the two skies are disjoint
            // and a bird is never drawn over the wrong forest. the species is decided per CANDIDATE POINT, not per
            // slot, because a slot recycles and the player walks: the same slot legitimately carries a robin
            // over the oak wood and a pink bird ten minutes later inside the blossom.
            const inCh = chOut(bx9, bz9);
            ok = desertM(bx9, bz9) <= BIRD_IN && (!inCh || BIRD_PINK >= 0);   // blossom is legal only when there IS a pink bird to put in it
            if (ok) pinkHere = inCh;
          }
          b.off = !ok;
          if (!ok) return null;                       // no forest within the ring — the sky over the desert stays empty
          b.x = P.x + Math.cos(a0) * r0; b.z = P.z + Math.sin(a0) * r0;
          b.x = Math.max(rect.xlo + 60, Math.min(rect.xhi - 60, b.x));   // never outside the generated rect — the hmap is stale there and the terrain-follow flies blind
          b.z = Math.max(rect.zlo + 60, Math.min(rect.zhi - 60, b.z));
          b.g = hmap[gwrap(Math.floor(b.x), WX) + gwrap(Math.floor(b.z), WZ) * WX] || P.y;
          b.th = Math.random() * Math.PI * 2; b.pyPrev = b.g + BIRD_ALT; b.init = true;
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
          const want = Math.atan2((rect.xlo + rect.xhi) * 0.5 - b.x, (rect.zlo + rect.zhi) * 0.5 - b.z);
          let dth = want - b.th;
          while (dth > Math.PI) dth -= 2 * Math.PI; while (dth < -Math.PI) dth += 2 * Math.PI;
          b.omT = Math.max(-1.0, Math.min(1.0, dth * 1.2)); b.mode = 0; b.edge = true;   // the steer-home urge overrides whatever whim it was on
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
