  // @module - the positional constraint solver shared by ragdolls and dropped items
  // @exports physStep
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
    // ── THE TREE BREAKS ON IMPACT, NOT ON THE CUT (user 2026-08-22: "can you make the trees break in pieces
    // when it falls over? not at the moment the player chomps the tree down, but the moment the tree hits the
    // terrain") ── armed at the fell (sim/chop.js sets fellWhole beside noAbsorb) and fired here, once the
    // topple drive has handed back to plain physics AND the trunk has been in contact for a few frames.
    // Debounced rather than fired on the first contact, because a trunk going over can clip its own stump or
    // a rise on the way down and neither is the landing. phShatterTree lifts noAbsorb on the pieces, so the
    // tree becomes collectable at the same moment it breaks — the two halves of the ask are one event.
    // ── AND IT HAS TO HAVE ACTUALLY GONE OVER ── gating on `!b.tipping` alone was wrong and MEASURED wrong:
    // a freshly severed trunk is ARMED (tipArm) but not yet TIPPING, so the gate passed while the tree was
    // still standing on its own stump and it burst into four pieces upright, at the cut — the exact thing the
    // user asked to move away from. fellDown is set only where the topple drive completes, so a tree that has
    // not fallen cannot break, however long it rests there.
    // ── …OR IT IS SIMPLY LYING DOWN (user 2026-08-22: "also sometimes they dont break at all") ── fellDown is
    // set only where the topple DRIVE completes, and a trunk can end up flat without that ever happening: it
    // can be knocked over by the pieces of another tree, slide off its own stump, or be armed and never seated
    // so `tipping` is never entered at all. Those trees landed and stayed whole. b.ay[1] is the body's own
    // up-axis in world space, so testing it against the same tipDone the drive uses asks the only question
    // that matters — is this thing on its side — and a standing tree reads ~1 and still cannot break.
    if (b.fellWhole && !b.tipping && (b.fellDown || b.ay[1] <= PH.tipDone)) {
      if ((b.contacts | 0) > 0) {
        if ((b.fellLandT = (b.fellLandT | 0) + 1) >= PH.fellLandFrames) { b.fellWhole = 0; phShatterTree(b); return; }
      } else b.fellLandT = 0;
    }
    // RESTING LATCH: a body that was in contact last step and is barely moving does not re-accumulate
    // gravity. Without this, every contact-free step re-added 3.3 vox/s of fall which the next step's
    // impulse cancelled — a limit cycle that kept a visibly stationary body above the sleep threshold
    // forever. Self-correcting: if the support goes away, contacts hit 0 and gravity resumes next step.
    const resting = (b.contacts | 0) > 0 && Math.hypot(b.vel[0], b.vel[1], b.vel[2]) < PH.sleepLin;   // NB: this stays ON during a topple. Exempting tipping bodies reopened the gravity/impulse limit cycle the latch exists to stop, and a driven topple does not need gravity to turn anyway.
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
    if (lin < PH.sleepLin && angOK) {
      if (!b.tipping && ++b.sleepT >= PH.sleepFrames) { b.sleeping = true; b.vel[0] = b.vel[1] = b.vel[2] = 0; b.omega[0] = b.omega[1] = b.omega[2] = 0;
        if (b.n <= PH.retireMax) b.retire = true;
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
        else if (!b.noAbsorb && b.n <= PH.retireFar) {
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
        const tip = Math.hypot(b.vel[0], b.vel[1], b.vel[2]) + Math.hypot(b.omega[0], b.omega[1], b.omega[2]) * b.rMax;
        const sub2 = Math.max(1, Math.min(PH.maxCCD, Math.ceil(tip * PH.dt / 0.5)));
        const hh = PH.dt / sub2;
        for (let q3 = 0; q3 < sub2; q3++) phStep(b, hh);
        PH.stats.ccd = Math.max(PH.stats.ccd, sub2);
      }
      PH.acc -= PH.dt; k++;
    }
    // ── ABSORB ── a chunk the axe knocked loose is drawn INTO the player a moment after it breaks away.
    // NO ITEM is granted: this used to call startGrab(3, …), the twig pickup, which stuffed the hotbar
    // with sticks every swing (user 2026-08-02). Chopping debris is scrap, not loot.
    // The flight is a real transition rather than a pop: once it starts the chunk leaves the simulation
    // (no gravity, no contacts, no hitbox), eases toward the player's chest on a curve that accelerates
    // into the grab, and shrinks to nothing as it arrives.
    const tNow = performance.now();
    for (let i = PH.bodies.length - 1; i >= 0; i--) {
      const b = PH.bodies[i];
      if (!b.absorbing && tNow - b.born > (b.noAbsorb ? PH.treeLifeMs : PH.chunkLifeMs)) {
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
        const tx = P.x, ty = smoothEye + PH.absorbY, tz = P.z;   // tracked live so the chunk follows a moving player
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
        if (!b.sleeping && !(tNow - b.born > 1500 && (b.nearR || vJ < 4))) continue;   // …a second and a half is well past the bounce, and does not depend on a chunk ever going quiet
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

