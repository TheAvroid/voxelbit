  // @module - the positional constraint solver shared by ragdolls and dropped items
  // @exports physStep, physRetire
  // ── SOLVER ── sequential impulse / PGS, fixed 60 Hz, PH.iters iterations, no pair state between steps.
  const phStep = (b, h) => {
    if (b.sleeping || b.absorbing) return;             // an absorbing chunk is driven by its flight curve, not by physics
    if (b.n >= PH.crashThrough && !b.tipping && b.tipAx !== undefined) {
      // ── HUNG UP ── still airborne and not moving: something solid is holding it. Re-arm the drive.
      const mv = Math.abs(b.pos[1] - (b.stuckY === undefined ? 1e9 : b.stuckY));
      const tN = performance.now();
      if (b.stuckT === undefined || mv > 1.5) { b.stuckT = tN; b.stuckY = b.pos[1]; }
      else if (tN - b.stuckT > PH.stuckMs) {
        b.stuckT = tN; b.stuckY = b.pos[1];
        b.tipping = 1; b.tipT0 = tN;                   // another go at going over…
        b.vel[1] -= PH.stuckNudge;                     // …plus a shove, so it settles rather than hovering
        b.sleeping = false; b.sleepT = 0;
        PH.stats.unstuck++;
      }
    }
    if (b.tipArm) {                                    // waiting to land on the base before it starts to go over
      const tA = performance.now();
      if (!b.tipSeatT && (b.contacts | 0) > 0 && Math.abs(b.vel[1]) < 12) b.tipSeatT = tA;   // the moment it settled onto the cut face
      if (b.tipSeatT) { b.vel[0] = 0; b.vel[2] = 0; b.omega[0] = b.omega[1] = b.omega[2] = 0; }   // SEATED: no sliding off the base, no early lean
      if ((b.tipSeatT && tA - b.tipSeatT > PH.tipHoldMs) || tA - b.tipArmT > PH.tipArmMs) {
        b.tipArm = 0; b.tipping = 1; b.tipT0 = tA; PH.stats.topples++;   // seated long enough (or nothing to land on) — now tip
      }
    }
    if (b.n >= PH.crashThrough && b.tipAx !== undefined) {   // the ceiling outlives the drive: past tipDone the fall is plain physics, and that is where it used to run away
      const wm = Math.hypot(b.omega[0], b.omega[1], b.omega[2]);
      if (wm > PH.tipMax) { const kc = PH.tipMax / wm; b.omega[0] *= kc; b.omega[1] *= kc; b.omega[2] *= kc; }
    }
    if (b.tipping) {                                   // ── TOPPLE DRIVE ── see the block comment on PH.tipDone
      // b.ay is the body's own up-axis in world space, refreshed at the end of every step, so its Y
      // component IS cos(tilt) — 1 upright, 0 flat on its side.
      const up = b.ay[1];
      if (up <= PH.tipDone || performance.now() - b.tipT0 > PH.tipMaxMs) { b.tipping = 0; b.fellDown = 1; }   // committed (or stuck) — hand it back to plain physics, and RECORD that it went over (see the break-on-impact block below)
      else {
        // A FLOOR on the topple-axis component left the contact solver free to inject rotation about the
        // OTHER two axes — impulses off the stump, the crown clipping ground — and that cross-axis spin is
        // what made a falling trunk shudder and spin out (user: "glitches out and spases out"). A tree
        // pivoting on its stump turns about ONE axis; anything else is solver noise, so it is damped hard
        // for as long as the drive is running. The topple component itself is still only a floor, so a
        // genuine impulse can speed the fall up — it just cannot wrench the trunk sideways.
        // sN: 0 the instant it is severed, 1 at the release angle. The quadratic term is the momentum —
        // early on the trunk creeps, and by the time it is halfway over it is moving several times faster.
        const sN = Math.min(1, Math.max(0, (1 - up) / Math.max(1e-3, 1 - PH.tipDone)));
        const want = PH.tipSeed + PH.tipRate * sN + PH.tipAccel * sN * sN;
        // Prescribed rotation overrides the solver, so while the body is still deeply penetrating
        // something, DON'T add to it — let the contact push it out first. Without this the drive simply
        // carries the trunk through whatever it is resting on (user: it fell through the stump).
        const wNow = b.omega[0] * b.tipAx + b.omega[2] * b.tipAz;
        const wAxis = Math.min(PH.tipMax, (b.deepest || 0) > PH.tipBlockDepth ? wNow : Math.max(wNow, want));
        const perpX = b.omega[0] - wAxis * b.tipAx, perpZ = b.omega[2] - wAxis * b.tipAz;
        const k = PH.tipDamp;
        b.omega[0] = wAxis * b.tipAx + perpX * k;
        b.omega[1] *= k;
        b.omega[2] = wAxis * b.tipAz + perpZ * k;
        b.sleepT = 0;                                  // a half-fallen trunk must not doze off mid-tilt
      }
    }
    // ── THE TREE BREAKS WHEN IT HITS THE GROUND (user 2026-08-23: "the tree is supposed to break up in chunks
    // when it hits the ground") ── armed at the fell (sim/chop.js sets fellWhole beside noAbsorb) and fired by
    // the motion test in the body sweep below. The FIRST version was a landing test that had to know the trunk
    // was down, which needed the topple drive to have run — and that drive is what made every tree tilt the
    // same way; it was replaced by a plain 10 s clock so the fall could be raw physics. Both were wrong, and
    // measuring the fall says why: a felled body ARRESTS hard when it lands and then creeps and rolls for
    // another 10-15 s at 2-3.5 vox/s. A clock cannot see the landing at all, and waiting for the creep to stop
    // broke the tree long after the player watched it come down. The arrest is the landing, it needs to know
    // nothing about trunks, and the fall stays raw physics. See PH.fellHitVy for the numbers.
    // RESTING LATCH: a body that was in contact last step and is barely moving does not re-accumulate
    // gravity. Without this, every contact-free step re-added 3.3 vox/s of fall which the next step's
    // impulse cancelled — a limit cycle that kept a visibly stationary body above the sleep threshold
    // forever. Self-correcting: if the support goes away, contacts hit 0 and gravity resumes next step.
    const resting = !(b.noRest !== undefined && performance.now() < b.noRest) && (b.contacts | 0) > 0 && Math.hypot(b.vel[0], b.vel[1], b.vel[2]) < PH.sleepLin;   // noRest: a freshly shattered piece is in contact with its siblings and would otherwise hold itself up — see PH.fellSettleMs   // NB: this stays ON during a topple. Exempting tipping bodies reopened the gravity/impulse limit cycle the latch exists to stop, and a driven topple does not need gravity to turn anyway.
    if (!resting) b.vel[1] -= PH.gravity * (b.slowFall === undefined ? 1 : b.slowFall) * h;   // a felled trunk falls in a slowed time base — see PH.fallSlow
    const ld = Math.exp(-PH.linDamp * h), ad = Math.exp(-PH.angDamp * h);
    b.vel[0] *= ld; b.vel[1] *= ld; b.vel[2] *= ld;
    b.omega[0] *= ad; b.omega[1] *= ad; b.omega[2] *= ad;
    b.pos[0] += b.vel[0] * h; b.pos[1] += b.vel[1] * h; b.pos[2] += b.vel[2] * h;
    const w = b.omega, q = b.q;                      // q += 0.5 * omega(quat) * q
    const dx = 0.5 * h * (w[0] * q[3] + w[1] * q[2] - w[2] * q[1]);
    const dy = 0.5 * h * (w[1] * q[3] + w[2] * q[0] - w[0] * q[2]);
    const dz = 0.5 * h * (w[2] * q[3] + w[0] * q[1] - w[1] * q[0]);
    const dw = -0.5 * h * (w[0] * q[0] + w[1] * q[1] + w[2] * q[2]);
    q[0] += dx; q[1] += dy; q[2] += dz; q[3] += dw; phQNorm(q);
    // ── CONTACT GENERATION (once per step) ── probe voxels are SPHERES of radius 0.5, like Teardown
    let nc = 0, deepest = 0;
    for (let i = 0; i < b.probes.length && nc < PH.maxContacts; i++) {
      const pi = b.probes[i];
      phTmp[0] = b.lx[pi] + 0.5 - b.com[0]; phTmp[1] = b.ly[pi] + 0.5 - b.com[1]; phTmp[2] = b.lz[pi] + 0.5 - b.com[2];
      phQRot(b.q, phTmp, phTmp2);
      const wx = b.pos[0] + phTmp2[0], wy = b.pos[1] + phTmp2[1], wz = b.pos[2] + phTmp2[2];
      const gx = Math.floor(wx), gy2 = Math.floor(wy), gz = Math.floor(wz);
      if (!phSolidAt(gx, gy2, gz)) continue;
      phNormalAt(gx, gy2, gz, phNrm);
      // sphere-vs-voxel depth along the contact normal, measured from the solid cell's open face
      const fx = wx - (gx + 0.5), fy = wy - (gy2 + 0.5), fz = wz - (gz + 0.5);
      const along = fx * phNrm[0] + fy * phNrm[1] + fz * phNrm[2];
      const depth = Math.max(0.01, 1.0 - along);     // 0.5 (cell half) + 0.5 (voxel sphere) - along
      if (depth > deepest) deepest = depth;
      cR[nc * 3] = phTmp2[0]; cR[nc * 3 + 1] = phTmp2[1]; cR[nc * 3 + 2] = phTmp2[2];
      cN[nc * 3] = phNrm[0]; cN[nc * 3 + 1] = phNrm[1]; cN[nc * 3 + 2] = phNrm[2];
      cD[nc] = depth; nc++;
    }
    b.contacts = nc; b.deepest = deepest;
    PH.stats.contacts += nc;
    if (nc) {
      const invM = 1 / b.mass;
      const Ii = [1 / Math.max(b.I[0], 1e-3), 1 / Math.max(b.I[1], 1e-3), 1 / Math.max(b.I[2], 1e-3)];
      for (let it = 0; it < PH.iters; it++) {        // PGS: relax every contact, repeatedly
        for (let c = 0; c < nc; c++) {
          const rx = cR[c * 3], ry = cR[c * 3 + 1], rz = cR[c * 3 + 2];
          const nx = cN[c * 3], ny = cN[c * 3 + 1], nz = cN[c * 3 + 2];
          const vpx = b.vel[0] + (b.omega[1] * rz - b.omega[2] * ry);
          const vpy = b.vel[1] + (b.omega[2] * rx - b.omega[0] * rz);
          const vpz = b.vel[2] + (b.omega[0] * ry - b.omega[1] * rx);
          const vn = vpx * nx + vpy * ny + vpz * nz;
          if (vn >= 0) continue;
          const cx = ry * nz - rz * ny, cy = rz * nx - rx * nz, cz = rx * ny - ry * nx;   // r x n
          const denom = invM + cx * cx * Ii[0] + cy * cy * Ii[1] + cz * cz * Ii[2];
          const j = -(1 + PH.restitution) * vn / Math.max(denom, 1e-6);
          b.vel[0] += j * nx * invM; b.vel[1] += j * ny * invM; b.vel[2] += j * nz * invM;
          b.omega[0] += cx * j * Ii[0]; b.omega[1] += cy * j * Ii[1]; b.omega[2] += cz * j * Ii[2];
          const tvx = vpx - vn * nx, tvy = vpy - vn * ny, tvz = vpz - vn * nz;   // Coulomb friction
          const tl = Math.hypot(tvx, tvy, tvz);
          if (tl > 1e-4) {
            const jt = Math.min(PH.friction * j, b.mass * tl / nc);
            b.vel[0] -= (tvx / tl) * jt * invM; b.vel[1] -= (tvy / tl) * jt * invM; b.vel[2] -= (tvz / tl) * jt * invM;
          }
        }
      }
      // Baumgarte positional correction: a FRACTION of the penetration, hard-capped. Applying the full
      // depth every substep pumps small chunks up out of the ground.
      let px = 0, py = 0, pz = 0;
      for (let c = 0; c < nc; c++) { const k = Math.min(cD[c] * 0.25, 0.06);
        px += cN[c * 3] * k; py += cN[c * 3 + 1] * k; pz += cN[c * 3 + 2] * k; }
      b.pos[0] += px / nc; b.pos[1] += py / nc; b.pos[2] += pz / nc;
      const rb = Math.exp(-6 * h);                   // rest bias — sheds the residual the solver cannot cancel
      b.vel[0] *= rb; b.vel[2] *= rb;
      b.omega[0] *= rb; b.omega[1] *= rb; b.omega[2] *= rb;
    }
    const lin = Math.hypot(b.vel[0], b.vel[1], b.vel[2]), ang = Math.hypot(b.omega[0], b.omega[1], b.omega[2]);
    // A chip of a few voxels has inertia ~0.33, so any impulse spins it past a sensible angular
    // threshold — yet its orientation is meaningless at that size. Judge those on linear rest alone.
    // ── …AND "A FEW VOXELS" MEANS MORE THAN SIX (2026-08-08) ── this reused retireMax (6) as the cutoff for
    // judging rest on linear motion alone, which left a PINECONE (13 voxels) and a needle tuft needing the
    // full angular test. MEASURED, in a calm forest with nothing chopped: PH.bodies pinned at 16/16, and the
    // occupants were 7-15 voxel scraps sitting at a FIXED position with sleep:false — linearly at rest and
    // still spinning on solver jitter, so they never slept, never retired, and held slots for the whole
    // chunkLifeMs. A saturated budget is what forces the eviction paths to run at all, and it is why a chunk
    // the player was carrying could be thrown away to make room for a cone nobody will ever see.
    // The original rationale is unchanged, only its threshold: at this size the orientation is meaningless,
    // and a body that has stopped MOVING has stopped. Linear rest is still required either way.
    const angOK = b.n <= PH.sleepAngFree ? true : ang < PH.sleepAng;
    // Sleep on LOW MOTION alone, not on having a contact this step. Contacts flicker in and out as a
    // resting body micro-settles, and requiring one meant the countdown decayed faster than it grew —
    // a body that had visibly stopped stayed awake forever. Free fall cannot false-trigger it: one step
    // of gravity is already 3.3 vox/s, well over sleepLin.
    if ((b.contacts | 0) > 0) b.cT = performance.now();   // when it last actually touched the world
    // ── AND IT MAY NOT FALL ASLEEP IN OPEN AIR (user 2026-08-22, screenshot: small clusters hanging in the
    // sky after the chunks fall) ── sleep was judged on low motion ALONE, deliberately, because contacts
    // flicker in and out as a body micro-settles. But a body whose support GOES AWAY — the chunk it was
    // resting on absorbed, the branch under it carved — keeps its near-zero velocity for the 40 frames the
    // counter needs and drops off to sleep in mid-air. A sleeping body skips the step entirely, so it can
    // never fall again: MEASURED after a fell, 11 bodies unsupported in air, one with contacts 0, sleeping
    // true, nine voxels up with nothing solid beneath it. Half a second of grace keeps the flicker tolerance
    // the original wanted while making "has not touched anything for a while" disqualifying.
    const heldRecently = (performance.now() - (b.cT === undefined ? 0 : b.cT)) < 500;
    if (lin < PH.sleepLin && angOK && heldRecently) {
      if (!b.tipping && ++b.sleepT >= PH.sleepFrames) { b.sleeping = true; b.vel[0] = b.vel[1] = b.vel[2] = 0; b.omega[0] = b.omega[1] = b.omega[2] = 0;
        // ── A PIECE OF A FELLED TREE IS NEVER BAKED BACK IN (user 2026-08-22: "theres also still voxels from
        // the tree after it had been felled", alongside "dont let chunks dissapear, unless absorbed by the
        // player") ── those two asks pull opposite ways for every OTHER chunk, and retiring is the compromise:
        // the voxels survive, as world geometry. For a tree the player just felled it is the wrong trade — the
        // whole point of the pieces is that they are COLLECTABLE, and a retired one is terrain you cannot pick
        // up, sitting where the tree came down. So fellLoot keeps its slot and leaves only by being absorbed or
        // by expiring on fellLifeMs (5 min). MEASURED before this: 4 pieces of one oak baked into the world.
        if (b.n <= PH.retireMax && !b.fellLoot) b.retire = true;
        // ── AND SETTLED SCRAP FAR FROM THE PLAYER GOES BACK IN THE GRID TOO ── MEASURED: PH.bodies sits
        // pinned at 16/16 in ordinary play with nothing being chopped, because the support resolver sheds a
        // needle or a cone here and there and each one holds a slot for the full chunkLifeMs. A saturated
        // budget is what forces the eviction paths to run at all, and every live body is a box the tracer
        // steps and the broad phase tests. Baking is not destruction — physRetire writes the voxels back into
        // W at their resting pose, where they cost nothing per query and look identical.
        // Gated on DISTANCE so it can never take a chunk out from under someone about to collect it: inside
        // this radius a settled chunk still vacuums up by walking to it, exactly as before. noAbsorb (a felled
        // trunk) is scenery the player may still want to chop, so it is never baked out from under them.
        // ── AND IT USED TO EXCLUDE EXACTLY THE BODIES IT WAS WRITTEN FOR ── a `!b.nearR` term sat here, but
        // supDrop sets nearR = PH.absorbR unconditionally on every body the support resolver sheds, so 100% of
        // resolver scrap — the cones and needle tufts named above as the reason this path exists — failed the
        // guard and was never baked back. PH.bodies then sat pinned at 16/16 in ordinary play and the eviction
        // paths ran on chunks the player was walking toward. The distance gate alone already covers the concern:
        // retireFarR is 48 and the largest nearR anything sets is 16 (absorbR, and the arrow's is the same), so
        // a body eligible here is three times further away than any reach that could collect it.
        else if (!b.noAbsorb && !b.fellLoot && b.n <= PH.retireFar) {   // …and the distance path is exempt for the same reason
          const rdx = b.pos[0] - P.x, rdy = b.pos[1] - smoothEye, rdz = b.pos[2] - P.z;
          if (rdx * rdx + rdy * rdy + rdz * rdz > PH.retireFarR * PH.retireFarR) b.retire = true;
        } }
    } else b.sleepT = Math.max(0, b.sleepT - 1);     // DECAY, don't reset: one jittery step must not restart the countdown
    phQRot(b.q, PHX, b.ax); phQRot(b.q, PHY, b.ay); phQRot(b.q, PHZ, b.az);   // keep the collision axes in step with the pose
  };
  // A settled CHIP is written back into the static grid at its resting pose and its slot released. Only
  // tiny fragments do this — a few voxels have no orientation worth preserving, so grid-snapping them is
  // invisible, and it keeps the body budget for things that need continuous transforms.
  // …but ONLY when it settled on the TERRAIN. A shed leaf that came to rest on a BRANCH used to be baked
  // in right there, and when the tree was later felled the crown left W while the baked needle stayed —
  // hanging in mid-air. (Measured: 10 of the 15 voxels a felled pine left floating.) A fragment that
  // stopped anywhere else is simply dropped instead: it is a speck, and losing it is invisible next to
  // leaving it stuck in the sky.
  const phOnGround = (b) => {
    for (let i = 0; i < b.n; i++) {
      phTmp[0] = b.lx[i] + 0.5 - b.com[0]; phTmp[1] = b.ly[i] + 0.5 - b.com[1]; phTmp[2] = b.lz[i] + 0.5 - b.com[2];
      phQRot(b.q, phTmp, phTmp2);
      const x = Math.floor(b.pos[0] + phTmp2[0]), y = Math.floor(b.pos[1] + phTmp2[1]), z = Math.floor(b.pos[2] + phTmp2[2]);
      if (y <= H(x, z) + 2) return true;               // on the terrain surface (+2 covers snow and ground decor) — safe to bake in
    }
    return false;
  };
  const physRetire = (b) => {
    if (!phOnGround(b)) { PH.stats.dropped++; return false; }   // came to rest up a tree — do NOT bake it into the grid (see phOnGround)
    const cells = [];
    for (let i = 0; i < b.n; i++) {
      phTmp[0] = b.lx[i] + 0.5 - b.com[0]; phTmp[1] = b.ly[i] + 0.5 - b.com[1]; phTmp[2] = b.lz[i] + 0.5 - b.com[2];
      phQRot(b.q, phTmp, phTmp2);
      const x = Math.floor(b.pos[0] + phTmp2[0]), y = Math.floor(b.pos[1] + phTmp2[1]), z = Math.floor(b.pos[2] + phTmp2[2]);
      if (y < 1 || y >= WY) continue;
      const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
      if (W[ii]) continue;                           // only into empty air — never overwrite terrain
      W[ii] = b.id[i]; cells.push(ii);
    }
    // ── AND THE RESOLVER HAS TO BE TOLD (user 2026-08-22, screenshot: leaf clusters hanging in mid-air) ──
    // sim/support-rules.js opens with "every mutation of W funnels through here", and this was the one that
    // did not: physRetire writes voxels straight into the grid and patched the GPU, and NOTHING ever asked
    // whether they were held up. A body is retired when it goes to SLEEP, and sleep is judged on low motion
    // alone — a small piece can doze off resting on ANOTHER BODY, or simply stop between contacts, and then be
    // baked into the world in mid-air, permanently, because no later pass revisits a cell nobody queued.
    // Rare before; my fell-shatter made it common by producing up to 24 pieces per tree, most of them under
    // retireMax (6) and so all candidates for exactly this path.
    // Queue them instead of trying to judge support here: the flood is the authority on what holds what, and
    // an anchored bake costs one dedupe hit while a floating one is dropped the way any other orphan is.
    if (cells.length) { gpuPatch(cells, false); for (let k = 0; k < cells.length; k++) supPush(cells[k]); }
    PH.stats.retired++;
    return true;
  };
  const physStep = (dtR) => {                        // fixed-rate accumulator, capped so a stall cannot spiral
    if (!PH.on || !PH.bodies.length) { PH.acc = 0; return; }
    const t0 = performance.now();
    PH.acc = Math.min(PH.acc + dtR, 0.25);
    let k = 0;
    while (PH.acc >= PH.dt && k < 8) {
      for (const b of PH.bodies) {
        if (b.sleeping) continue;
        // ── CCD-LITE ── contacts are point samples with no swept test, so a fast body tunnels: a toppling
        // 93-voxel pine has a tip 50 from the COM, and at 3 rad/s that tip moves 2.5 voxels per 60 Hz step
        // — straight through the stump it should hit. Subdivide THIS body's step so no point on it can
        // travel more than half a voxel, which is finer than the grid it is colliding against.
        // ── JOLT OWNS THE MOTION; THIS LOOP KEEPS THE BOOKKEEPING (user 2026-09-01) ── and the split
        // belongs HERE, at the integrator, not at physStep's caller. The first wiring skipped the whole
        // of physStep under ?jolt, which quietly took the rest of this function with it: RETIREMENT (a
        // slept body baking back into W), the absorb rules, the lifetime expiry and the support queue
        // all live in this same loop. A felled tree would have fallen correctly under Jolt and then
        // never become world again - it would sit as a rigid body for ever, holding a body slot and a
        // GPU page. So the loop still runs for every body; only the integrate-and-collide step is
        // skipped, because Jolt has already written b.pos, b.q, b.vel and b.sleeping this frame.
        if (!JOLT.on) {
          const tip = Math.hypot(b.vel[0], b.vel[1], b.vel[2]) + Math.hypot(b.omega[0], b.omega[1], b.omega[2]) * b.rMax;
          const sub2 = Math.max(1, Math.min(PH.maxCCD, Math.ceil(tip * PH.dt / 0.5)));
          const hh = PH.dt / sub2;
          for (let q3 = 0; q3 < sub2; q3++) phStep(b, hh);
          PH.stats.ccd = Math.max(PH.stats.ccd, sub2);
        }
      }
      PH.acc -= PH.dt; k++;
    }
    // How much CATCH-UP this frame actually cost: k is the number of fixed steps run, and a frame that runs
    // more than one is repaying a previous slow frame. __vb.phStat().stepK / accMs / stepKMax.
    PH.stats.stepK = k;
    PH.stats.stepKMax = Math.max(PH.stats.stepKMax | 0, k);
    PH.stats.accMs = +(PH.acc * 1000).toFixed(1);
    if (k > 1) PH.stats.catchUpFrames = (PH.stats.catchUpFrames | 0) + 1;
    PH.stats.stepFrames = (PH.stats.stepFrames | 0) + 1;
    // ── ABSORB ── a chunk the axe knocked loose is drawn INTO the player a moment after it breaks away.
    // NO ITEM is granted: this used to call startGrab(3, …), the twig pickup, which stuffed the hotbar
    // with sticks every swing (user 2026-08-02). Chopping debris is scrap, not loot.
    // The flight is a real transition rather than a pop: once it starts the chunk leaves the simulation
    // (no gravity, no contacts, no hitbox), eases toward the player's chest on a curve that accelerates
    // into the grab, and shrinks to nothing as it arrives.
    const tNow = performance.now();
    for (let i = PH.bodies.length - 1; i >= 0; i--) {
      const b = PH.bodies[i];
      // ── THE FELL'S BREAK CLOCK LIVES HERE, NOT IN THE PER-BODY STEP ── phStep returns immediately for a
      // SLEEPING body, and a severed trunk resting on its own stump is asleep inside a second, so a break
      // tested in there could never fire on the one case it exists for (measured: 20 s, still one 8,016-voxel
      // body). This sweep visits every body whatever its state, and it already owns tNow.
      if (b.fellWhole && !b.absorbing) {                // ── SETTLED, OR THE BACKSTOP EXPIRED ── see PH.fellCalmLin for the measurement this threshold comes from
        const lin9 = Math.hypot(b.vel[0], b.vel[1], b.vel[2]), ang9 = Math.hypot(b.omega[0], b.omega[1], b.omega[2]);
        if (b.calmT === undefined || lin9 > PH.fellCalmLin || ang9 > PH.fellCalmAng) b.calmT = tNow;   // still moving — the dwell restarts, so one quiet FRAME mid-topple can never fire it
        // ── NOT WHILE IT IS STILL GOING OVER (user 2026-08-23: "you dont give it a chance to tilt over and
        // land on the terrain") ── a severed trunk does not fall straight down. It DROPS ONTO ITS OWN CUT FACE
        // first (sim/chop.js arms tipArm, the block at the top of this file seats it), and only then does the
        // topple drive run it over. That seat is an arrest like any other — fall speed collapses, contacts go
        // up — so the impact test fired there and burst the tree while it was still standing on its stump.
        // While tipArm or tipping is set the tree has not landed on anything yet, so nothing is armed and the
        // peak is reset: the fall that counts is the one AFTER it has committed, which is the one the player
        // watches come down. Bodies that never had a topple (a crown separated by the support pass) have
        // neither flag and are unaffected.
        // …but ONLY while it is still upright: a tree that has leaned past PH.fellTiltUp is off its stump, so
        // the next thing to stop it is the terrain — and MEASURED, that first ground contact often happens
        // while the drive is STILL RUNNING (up = 0.41, contacts 2, bounced 3 voxels back up, fellDown only
        // 140 ms later). Blocking on the drive alone threw that impact away and broke the tree 850 ms late.
        // A body with no topple at all — a crown the support pass separated — has neither flag and is exempt.
        // ── …AND THE COVER EXPIRES (user 2026-08-27: "I knocked down a birch tree and it did not break") ──
        // this block had no deadline, and b.tipping does NOT clear when the trunk is over: it clears when the
        // trunk gets over OR when the drive's hard stop runs out, and that stop is PH.tipMaxMs — 15 SECONDS.
        // A trunk the drive cannot roll (wedged on a neighbour, blocked by terrain, or paused by
        // tipBlockDepth) therefore sat at up ~1.0 with tipping set for the full 15 s, and every frame of it
        // reset BOTH the impact test and the calm dwell. Nothing could break it but the fellBreakMs backstop
        // at 25 s, which is exactly the report: knocked down, and it does not come apart.
        // The BIRCH forest is where this shows up because BKCELL is 44 and the crowns are 100+ voxels wide —
        // the densest stand in the game, so a severed birch leaning onto its neighbour is ordinary, not rare.
        // What the block is FOR is the trunk's seat on its own CUT FACE, and that is over inside ~2 s
        // (tipArmMs 1600 + tipHoldMs 380). Past fellStuckMs a trunk still within ~26 deg of vertical is not
        // going over, it is STUCK, and whatever is holding it up is the thing it landed on — which is the
        // definition of the arrest this suppression was hiding. So the cover runs off b.tipArmT (stamped at
        // the cut, never restamped — a drive RE-ARMED by the hung-up path is therefore not re-covered) and
        // expires. MEASURED over 47 felled birches: every one broke on impact in 1.8-4.8 s and none reached
        // this deadline, so the healthy path is untouched; a stuck trunk now breaks at ~3.1 s instead of 15+.
        if ((b.tipArm || b.tipping) && (b.ay ? b.ay[1] : 0) > PH.fellTiltUp
            && (b.tipArmT === undefined || tNow - b.tipArmT < PH.fellStuckMs)) { b.fellPkVy = 0; b.hitT = undefined; b.calmT = tNow; }
        const down9 = b.vel[1] < 0 ? -b.vel[1] : 0;    // ── IMPACT = THE FALL BEING ARRESTED ── see PH.fellHitVy for the two fells this is measured from
        if (down9 > (b.fellPkVy || 0)) b.fellPkVy = down9;   // the body's OWN peak fall speed, so the test scales with how far it had to drop
        // TOUCHED RECENTLY, not touching THIS FRAME: contacts flicker 0<->2 between frames on a body lying on
        // uneven ground (the sleep rule below ignores them for the same reason). Requiring them every frame
        // reset the dwell over and over — MEASURED on a landed pine, contacts dropped to 0 every few frames and
        // the break came 1.6 s after it was already flat. b.cT is the same last-touched stamp the sleep test uses.
        const touched9 = tNow - (b.cT === undefined ? 0 : b.cT) < PH.fellHitHoldMs;
        if (!((b.fellPkVy || 0) > PH.fellHitVy && down9 < b.fellPkVy * PH.fellHitFrac && touched9)) b.hitT = undefined;   // airborne, or never really fell — a trunk sitting on its stump never arms this
        else if (b.hitT === undefined) b.hitT = tNow;
        const age9 = tNow - b.born;
        // ── AND ONCE IT HAS BEEN REFUSED, IT RETRIES EVERY TICK (user 2026-08-26: "sometimes the birch tree
        // doesnt break into chunks when it falls") ── phShatterTree's no-room arm returns 0 under a comment
        // promising "this runs again next tick, so the room arrives over a handful of frames", but the call
        // only happened while one of the three triggers below still held. So a trunk refused on its landing
        // frame relied on a trigger still being true later to be asked again, rather than on the next tick.
        // HONESTLY: this is a safety net, not a measured fix. Every tree broke in 2.0-3.9 s across 24 fells,
        // including a scene saturated to 256 bodies (13 refusals, still broke) and a pool squeezed to 18
        // (1 refusal, broke in 2.2 s) — the retry has never been the thing that rescued one. It is here
        // because the no-room arm's own contract says next tick and this is what makes that true, and it is
        // what lets the coarse fallback's counter climb at all. b.shatTry is set the moment it is refused.
        // (An earlier note here claimed a 25 s stall. That was my own broken build — a `//` appended inside
        //  PH's one-line literal had eaten fellHitVy..fellCalmAng, so the impact test could never pass and
        //  every tree sat until the fellBreakMs backstop. Fixed; the number was an artefact, not behaviour.)
        // WHICH TRIGGER BROKE IT, recorded so an audit can tell a tree that broke on IMPACT (the intended
        // path) from one that sat there until the fellBreakMs backstop — the two look identical from outside
        // and only the second is the "it didn't break" report. __vb.phStat().breakWhy / breakAge.
        const bHit9 = b.hitT !== undefined && tNow - b.hitT > PH.fellHitMs;
        const bCalm9 = age9 > PH.fellMinMs && tNow - b.calmT > PH.fellCalmMs;
        const bBack9 = age9 > PH.fellBreakMs;
        if ((b.shatTry | 0) > 0 || bHit9 || bCalm9 || bBack9) {
          const why9 = bHit9 ? 'hit' : bCalm9 ? 'calm' : bBack9 ? 'backstop' : 'retry';
          if (phShatterTree(b)) { b.fellWhole = 0;
            PH.stats.breakWhy = why9; PH.stats.breakAge = Math.round(age9);
            PH.stats.breakN = (PH.stats.breakN | 0) + 1;
            PH.stats['break_' + why9] = (PH.stats['break_' + why9] | 0) + 1;
            continue; } }   // a 0 means it could not break YET (no room for uniform pieces) — keep the flag and try again next tick
      }
      // ── A FELLED TREE'S PIECES BAKE ONCE YOU HAVE WALKED AWAY FROM THEM ── the two retire arms in the
      // settle block above both exclude fellLoot, so every chunk of every tree holds a body slot for the full
      // fellLifeMs (5 min), and that is the cost that ACCUMULATES while you clear a stand: MEASURED felling
      // six birches from one spot, PH.bodies 0 -> 256 and the TRACE pass 3.29 -> 5.91 ms, 249 fps -> 147,
      // because every ray walks every rigid body. Distance is what decides it — 104 chunks left behind cost
      // nothing measurable, 256 underfoot cost +80%.
      // IT HAS TO LIVE HERE AND NOT UP THERE. Those arms run on the single frame a body falls asleep, and a
      // chunk settles at the foot of the tree you are standing at, so the distance test could never pass and
      // then the body sleeps and is never re-asked. MEASURED: the arm fired zero times at 260 voxels and zero
      // at 120 before it moved. This pass visits every body every tick, sleeping ones included.
      // Baking is not deletion — physRetire writes the voxels into W at their resting pose, so the pile looks
      // identical and is still there to chop; it stops being a box the tracer steps. Gated far beyond any
      // reach that could collect one (fellBakeR against an absorbR of 16) plus fellBakeMs of settled age.
      if (b.fellLoot && b.sleeping && !b.absorbing && !b.retire && tNow - b.born > PH.fellBakeMs && (frame & 15) === (b.n & 15)) {
        const fdx = b.pos[0] - P.x, fdy = b.pos[1] - smoothEye, fdz = b.pos[2] - P.z;
        if (fdx * fdx + fdy * fdy + fdz * fdz > PH.fellBakeR * PH.fellBakeR) { b.retire = true; PH.stats.fellBaked = (PH.stats.fellBaked | 0) + 1; }
      }
      if (!b.absorbing && tNow - b.born > (b.noAbsorb ? PH.treeLifeMs : (b.fellLoot ? PH.fellLifeMs : PH.chunkLifeMs))) {   // fellLoot = a piece of a tree the player felled (sim/chop-tree.js): 5 minutes, not 10
        PH.bodies.splice(i, 1); PH.stats.expired = (PH.stats.expired | 0) + 1;   // ── EXPIRED (user) ── see treeLifeMs / chunkLifeMs
        continue;                                      // one already flying into the player finishes its flight — it is about to be gone anyway
      }
      // ── A BODY OUTSIDE THE GENERATED RECT CAN NEVER LAND ── and until it times out it holds a slot.
      // MEASURED, in a calm forest with nothing being chopped: PH.bodies pinned at 16/16, and 15 of the 16
      // were 600-1100 voxels away with sleep:false. They are cones and needle tufts the support resolver
      // sheds out at the edge of the loaded window, where W is empty — so phSolidAt finds no ground, they
      // fall forever, never settle, never retire, and sit on the budget for the full chunkLifeMs. That
      // saturation is what forces the eviction paths to run at all, and it is why a chunk the player was
      // actually holding could be evicted to make room for debris nobody will ever see.
      // Nothing here is observable: it is outside the built world, in free fall, at 60-110 m.
      if (!b.absorbing && !b.noAbsorb && b.pos[1] < 1) {   // fell out of the world entirely
        PH.bodies.splice(i, 1); PH.stats.voidFall = (PH.stats.voidFall | 0) + 1; continue;
      }
      if (!b.absorbing && !b.noAbsorb && !b.sleeping && tNow - b.born > 2000 &&
          (b.pos[0] < rect.xlo || b.pos[0] > rect.xhi || b.pos[2] < rect.zlo || b.pos[2] > rect.zhi)) {
        PH.bodies.splice(i, 1); PH.stats.offRect = (PH.stats.offRect | 0) + 1; continue;   // 2 s of grace so a chunk thrown toward the edge still gets its arc
      }
      if (b.absorbing) {
        const k = Math.min(1, (tNow - b.absorbT0) / PH.absorbFly);
        const e = k * k * (3 - 2 * k);                 // smoothstep — leaves the ground gently, arrives fast
        // ── AND A BIG CHUNK AIMS LOWER THAN A CHIP (user 2026-08-22: "the chunks from the tree appear too high
        // being absorbed into the player ... the chunks absorbed from the tools are fine") ── absorbY places the
        // body's CENTRE 12 voxels under the eye, which is chest height for the 30-voxel chip that number was
        // tuned on. A felled-tree chunk is 350 voxels — several voxels tall — so its centre at chest puts its
        // TOP across the view. Dropping the target by the body's own half-height makes the arrival read the
        // same whatever the size, and leaves the chip exactly where it already was (half of ~3 is ~1.5).
        const half9 = b.gpu ? 0.5 * Math.max(b.gpu.bw | 0, b.gpu.bh | 0, b.gpu.bd | 0) : 0;
        const tx = P.x, ty = smoothEye + PH.absorbY - half9, tz = P.z;   // tracked live so the chunk follows a moving player
        b.pos[0] = b.absorbP[0] + (tx - b.absorbP[0]) * e;
        b.pos[1] = b.absorbP[1] + (ty - b.absorbP[1]) * e + Math.sin(e * Math.PI) * 3;   // slight arc so it lifts rather than slides
        b.pos[2] = b.absorbP[2] + (tz - b.absorbP[2]) * e;
        b.scale = 1;                                   // FULL SIZE all the way in (user): it used to shrink to a tenth on the way to the hand, which read as the chunk evaporating rather than being picked up
        b.omega[0] = 6; b.omega[2] = 4;                // keeps tumbling on the way in
        const w = b.omega, q = b.q, h2 = dtR;
        const dx = 0.5 * h2 * (w[0] * q[3] + w[1] * q[2] - w[2] * q[1]);
        const dy = 0.5 * h2 * (w[1] * q[3] + w[2] * q[0] - w[0] * q[2]);
        const dz = 0.5 * h2 * (w[2] * q[3] + w[0] * q[1] - w[1] * q[0]);
        const dw = -0.5 * h2 * (w[0] * q[0] + w[1] * q[1] + w[2] * q[2]);
        q[0] += dx; q[1] += dy; q[2] += dz; q[3] += dw; phQNorm(q);
        phQRot(q, PHX, b.ax); phQRot(q, PHY, b.ay); phQRot(q, PHZ, b.az);
        if (k >= 1) { PH.bodies.splice(i, 1); PH.stats.absorbed++; }
        continue;
      }
      if (b.n > PH.absorbSize && !b.fellLoot) { b.tooBig = true; continue; }   // fellLoot: a piece of a tree the player felled (sim/chop-tree.js) is exempt — see the note there   // ── TOO BIG TO CARRY (user) ── it stays where it is until the player breaks it down. No on-screen message (user): the chunk simply not coming to you is the feedback. tooBig is kept purely so __vb.phys() can report which chunks were refused.
      if (!b.absorbAt || tNow < b.absorbAt) {          // no break scheduled it (or not yet) — but a LOOSE one at rest comes in when you walk up to it (user)
        // …only once it has settled: a chunk mid-tumble keeps its arc. But a small chunk on uneven ground
        // can jitter forever without ever formally sleeping — which left ARROW chunks uncollectable no
        // matter how close you stood (user). Near-stillness counts as settled for those.
        // ── AND THAT RULE IS FOR EVERY CHUNK NOW, NOT JUST ARROW ONES (user 2026-08-19: "have the player be
        // able to pick up ALL loose voxels. so long as they are small enough") ── the note above describes the
        // exact failure and then fixed it for `nearR` chunks alone: a small piece resting on uneven ground can
        // jitter against two contacts forever without the solver ever formally sleeping it, and until it does,
        // this line refuses it no matter how long you stand over it. Nothing about that is specific to arrows —
        // it is the generic "came to rest but never went quiet" case, and it is exactly the loose debris the
        // user is walking up to and failing to collect.
        // A REAL STILLNESS TEST, not just the clock. The arrow rule leans on `nearR` to mean "this one is a
        // scrap, let it in", and generalising the clock alone would sweep up chunks still mid-arc — a piece
        // thrown past you at 1.6 s would swerve into your chest. 2 vox/s is well under a bounce and well over
        // solver jitter, so a genuinely moving chunk keeps its arc and a stuck one is collectable.
        // SIZE IS STILL THE ONLY OTHER GATE, deliberately: PH.absorbSize is the user's own "too big to carry,"
        // break it down first" rule and this does not touch it.
        const vJ = b.vel[0] * b.vel[0] + b.vel[1] * b.vel[1] + b.vel[2] * b.vel[2];
        // ── A FELLED PIECE MUST ACTUALLY LAND FIRST (user 2026-08-31: "the top half of the tree is either
        // dissapearing or floating") ── fellLoot used to sit in this OR beside nearR, which made a tree chunk
        // "settled" on AGE ALONE at 1500 ms. A pine takes about 5.7 s to go over, so every piece was legal to
        // absorb while it was still in the air: stand at the trunk and the upper tree is collected out of its
        // own arc before it can reach the ground. That is the whole report - the top half never lands because
        // it is picked up mid-fall.
        // It now has to be still (vJ < 4) or asleep, like everything else, with a LONG age backstop so the
        // original reason for the exemption still holds: a chunk wedged against two others can jitter forever
        // without the solver formally sleeping it, and it must not become uncollectable. 8 s clears a fall
        // with room to spare.
        if (!b.sleeping && !(tNow - b.born > 1500 && (b.nearR || vJ < 4))
                        && !(tNow - b.born > 8000 && b.fellLoot)) continue;   // …and a felled-tree piece counts as settled on age alone: a chunk still nudging its siblings was refused, which is most of what "not picking up every chunk" was   // …a second and a half is well past the bounce, and does not depend on a chunk ever going quiet
        const dxA = b.pos[0] - P.x, dyA = b.pos[1] - (smoothEye + PH.absorbY), dzA = b.pos[2] - P.z;
        const rA = b.nearR || PH.absorbR;               // an ARROW's chunk keeps its own, much shorter reach (user)
        if (dxA * dxA + dyA * dyA + dzA * dzA > rA * rA) continue;
      }
      b.absorbing = true; b.absorbT0 = tNow;
      PH.stats.flights = (PH.stats.flights | 0) + 1;   // ── THE INVARIANT ── every flight STARTED must finish: flights === absorbed + in-flight. Any shortfall is a chunk deleted out of the air on its way to the player, which is invisible on screen except as the thing you dug never arriving.
      b.absorbP = [b.pos[0], b.pos[1], b.pos[2]];      // where the flight starts
      b.sleeping = false;                              // a settled chunk must still be able to fly in
    }
    // ── A CHUNK IN FLIGHT IS NOT SCENERY ── the absorb block above documents the invariant ("every flight
    // STARTED must finish: flights === absorbed + in-flight") and this loop was the thing breaking it: retire
    // bakes a settled body back into the static grid, and nothing here asked whether that body was already on
    // its way to the player. MEASURED on a 4-voxel chip: flights +1, absorbed +0, retired +1 in the SAME frame —
    // the chunk was collected and welded into the ground instead of arriving. It matters more now than it did,
    // because the absorb gate no longer waits for a formal sleep (user 2026-08-19), so far more small chips
    // start a flight while still inside retireMax; but the guard is right either way and costs one test.
    for (let i = PH.bodies.length - 1; i >= 0; i--) { const rb = PH.bodies[i]; if (rb.absorbing || !rb.retire) continue;
      // A speck that settled up a tree is still dropped — losing it is invisible and leaving it welded into
      // the canopy is not (see phOnGround). Anything BIGGER that could not be baked keeps its slot instead:
      // the distant-scrap path below hands this loop 7-64 voxel pieces, and silently destroying one of those
      // would be exactly the "hit it and it disappeared" class of bug.
      if (physRetire(rb) || rb.n <= PH.retireMax) PH.bodies.splice(i, 1); else rb.retire = false; }
    phAabbAll();                                     // …and refresh the broad-phase boxes LAST, after integration, the absorb flight and any retire have all moved or removed bodies (see phAabb)
    if (!PH.bodies.length) bodyTop = 0;              // no body references the shape buffer — reclaim it whole
    PH.stats.substeps += k;
    PH.stats.stepMs = +(performance.now() - t0).toFixed(3);
  };

