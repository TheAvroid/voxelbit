  window.__vb = { P, tp(x, y, z, yaw, pitch) { P.x = x; P.y = y; P.z = z; if (yaw !== undefined) P.yaw = yaw; if (pitch !== undefined) P.pitch = pitch;
      maybeRecenter();                                 // recenter FIRST — the pop-out below must read fresh terrain, not stale wrapped data
      while (P.y < WY - 20 && !boxFree(P.x, P.y, P.z, HEIGHT) && !waterAt(Math.floor(P.x), Math.floor(P.y + 8), Math.floor(P.z))) P.y += 1;
      P.vy = 0; smoothEye = P.y + EYE; resetHist = 1; }, fly() { P.fly = true; }, tod(t) { if (t === undefined) return tday; tday = t; resetHist = 1; return tday; },   // GETTER when called bare: it used to assign `undefined` and NaN the clock, and tday feeds NIGHT_K -> every life count -> NaN, i.e. one stray `__vb.tod()` silently emptied the world give() { addItem(2); }, giveIt(id) { const k = addItem(id | 0); if (k >= 0) selSlot = k; return { held: heldIt(), knifeId: KNIFE_IT }; },   // …and SELECT it: addItem only fills a slot, and a knife sitting unselected in the hotbar still swings the axe   // put a specific item in hand (tests: the knife's two-hit kill needs the knife actually held)
    palLen() { return { len: palette.length, over: palette.length > 256 }; },
    palMints(lo) { return palMintLog.filter((m) => m[0] >= (lo || 0)); },
    palTrace() { const out = []; for (let i = 0; i < palTrace.length; i++) out.push({ stage: palTrace[i][0], len: palTrace[i][1], spent: (i ? palTrace[i][1] - palTrace[i - 1][1] : palTrace[i][1]) }); return out; },   // slots each load stage cost
    palAudit() {                                     // WHICH ids are exact-colour duplicates, and is each one safe to reclaim. addCol() pushes unconditionally, so a loader that calls it directly (the .json decor palettes do) mints a fresh id for a colour the table already holds — but a duplicate is only REDUNDANT if nothing tells the two ids apart, and an id carries material flags and pickup-set membership as well as a colour.
      const flags = (i) => [solidTab[i], foliaTab[i], woodTab[i], mushTab[i], rockTopTab[i], decorTab[i], axeOnlyTab[i], pickOnlyTab[i], digOnlyTab[i], sandTab[i], coneTab[i], SUP.CLASS[i] | 0].join(',');
      const picks = (i) => [PICK_ROCK.has(i), PICK_STICK.has(i), PICK_BOULDER.has(i), PICK_CONE.has(i)].map((b) => (b ? 1 : 0)).join(',');
      const by = new Map();
      for (let i = 1; i < palette.length; i++) { const c = palette[i]; if (!c) continue;
        const k = (c[0] << 16) | (c[1] << 8) | c[2]; if (!by.has(k)) by.set(k, []); by.get(k).push(i); }
      const dup = [], safe = [];
      for (const [k, ids] of by) { if (ids.length < 2) continue;
        const rec = { col: [(k >> 16) & 255, (k >> 8) & 255, k & 255], ids,
          own: ids.map((i) => palOwn.has(i)), flags: ids.map(flags), picks: ids.map(picks) };
        // Redundant means: same colour, same material flags, same pickup membership, and not
        // RESERVED — palOwn ids exist precisely so a set built from them identifies one model.
        rec.redundant = rec.flags.every((f) => f === rec.flags[0]) && rec.picks.every((q) => q === rec.picks[0]) && !rec.own.some(Boolean);
        dup.push(rec); if (rec.redundant) safe.push(...ids.slice(1)); }
      // EXACT duplicates turn out to be all deliberate, so the only slack left is
      // NEAR-duplicates: two ids a player cannot tell apart. Merging those is a real
      // colour change, so it is reported per-threshold (max channel delta out of 255)
      // rather than acted on — and only inside a bucket that already agrees on every
      // material flag and every pickup set, because those are what make an id mean
      // something beyond its colour.
      const bucket = new Map();
      for (let i = 1; i < palette.length; i++) { if (!palette[i] || palOwn.has(i)) continue;
        const k = flags(i) + '|' + picks(i); if (!bucket.has(k)) bucket.set(k, []); bucket.get(k).push(i); }
      const near = {};
      for (const t of [1, 2, 3, 4, 6, 8, 12, 16]) { let gone = 0; const ex = [];
        for (const ids of bucket.values()) { const dead = new Set();
          for (let a2 = 0; a2 < ids.length; a2++) { if (dead.has(ids[a2])) continue;
            for (let b2 = a2 + 1; b2 < ids.length; b2++) { if (dead.has(ids[b2])) continue;
              const c1 = palette[ids[a2]], c2 = palette[ids[b2]];
              const d = Math.max(Math.abs(c1[0] - c2[0]), Math.abs(c1[1] - c2[1]), Math.abs(c1[2] - c2[2]));
              if (d <= t) { dead.add(ids[b2]); gone++; if (ex.length < 8) ex.push([ids[a2], ids[b2], d, c1, c2]); } } } }
        near[t] = { reclaimable: gone, examples: ex }; }
      return { len: palette.length, free: 256 - palette.length, over: palOver, tol: PAL_TOL, tolHits: palTolHits, tolErr: palTolErr, snaps: palSnaps, edSubs: edSnapCount(), edSubErr: edSnapErrs(), groups: dup.length,
        wasted: dup.reduce((n, d) => n + d.ids.length - 1, 0), reclaimable: safe.length,
        buckets: bucket.size, near, safe, dup }; },
    // ── FLOWER SCATTER, WITHOUT WALKING THE WORLD ── asks flowerAt directly over an NxN block of CELLS, so a
    // density or variety question is answered from the generator itself rather than by scanning voxels the
    // stream may not have built yet. perCell x FLWCELL^2 is the per-column rate the old single-voxel flowers
    // stated as a literal, which is what makes "half as many" checkable rather than asserted.
    flowerDbg(cx0, cz0, n) { const N = n || 100; let hit = 0; const ks = {};
      for (let a = 0; a < N; a++) for (let b = 0; b < N; b++) { const f = flowerAt((cx0 | 0) + a, (cz0 | 0) + b); if (f) { hit++; ks[f.k] = (ks[f.k] || 0) + 1; } }
      return { cells: N * N, flowers: hit, perCell: +(hit / (N * N)).toFixed(4),
               perColumn: +(hit / (N * N) / (FLWCELL * FLWCELL)).toFixed(5), variety: ks,
               variants: FLOWERV.length, ids: FLOWERIDS.length, petalIds: FLOWERHEAD.length,
               // WHICH FLOWER IDS STILL HAVE A HITBOX. floatTab says "surface scatter", but solidTab is what
               // the player collides with, and it is set by a BLANKET `i < DECOR_MIN` sweep in palette.js — so
               // any flower colour that palShare resolved to an id below that line is solid whatever floatTab
               // says. Listing them is the difference between "the flowers feel wrong" and a number.
               solidIds: FLOWERIDS.filter((i) => solidTab[i]),
               solidCols: FLOWERIDS.filter((i) => solidTab[i]).map((i) => palette[i]) }; },
    uniInfo() { return { on: LIFE_UNI, visW: VIS_W, sec: UF[1530], blue: BLUEB_ITEM0, robin: ROBIN_ITEM0, birdsDrawn: uniBirdN, birdsWant: uniBirdWant, cursor: UF[1103] }; },
    itemInfo() { return { n: itemsRef ? itemsRef.length : 0, cells: itemMapF32.length >> 2, card: CARD_ITEM0, bunny: BUNNY_ITEM0, arm: ARMADILLO_ITEM0, armN: ARMADILLO_NFRAMES, skunk: SKUNK_ITEM0, skunkN: SKUNK_NFRAMES, porc: PORCUPINE_ITEM0, porcN: PORCUPINE_NFRAMES, worm: WORM_ITEM0 }; },   // item-table census for the unification tests
   // the 8-bit palette ceiling — breaching it corrupts voxel SOLIDITY, not just colour
    // ── THE VIEW-MODEL'S PUBLISHED POSE ── the right hand's anchor/axes/item (pickA/pickX at 48/52) beside the
    // craft preview's own (UF_PICK3). Both live in tick-camera's closure and both are written every frame, so
    // a test of the craft glide — does the flying tool actually ARRIVE where the hand will draw it? — has no
    // other way to compare the two. `red` is the unaffordable lane (pick3Y.w).
    viewPose() { return {
      hand:  { p: [+UF[48].toFixed(4), +UF[49].toFixed(4), +UF[50].toFixed(4)], s: +UF[51].toFixed(4), ax: [+UF[52].toFixed(4), +UF[53].toFixed(4), +UF[54].toFixed(4)], it: UF[55] | 0 },
      off:   { p: [+UF[1092].toFixed(4), +UF[1093].toFixed(4), +UF[1094].toFixed(4)], s: +UF[1095].toFixed(4), it: UF[1099] | 0 },
      craft: { p: [+UF[UF_PICK3].toFixed(4), +UF[UF_PICK3 + 1].toFixed(4), +UF[UF_PICK3 + 2].toFixed(4)], s: +UF[UF_PICK3 + 3].toFixed(4), ax: [+UF[UF_PICK3 + 4].toFixed(4), +UF[UF_PICK3 + 5].toFixed(4), +UF[UF_PICK3 + 6].toFixed(4)], it: UF[UF_PICK3 + 7] | 0, red: UF[UF_PICK3 + 11] } }; },
    dualSplit(v) { if (v !== undefined) dualOn = !!v; return { on: dualOn, it: dualHeldIt() }; },   // drive/read the E split headlessly
    // ── PICKUP TABLE (user 2026-08-20: "audit all objects in the terrain. make sure everything can be picked
    // up accurately") ── the STATIC half of the audit; pickAudit(x,y,z) further down is the per-voxel half that
    // says whether a pickup leaves part of an object standing. Every DECORATION MODEL the world stamps, listed with the palette ids it actually
    // wears and which right-click set claims them. Built from the models rather than from a hand-written list,
    // so a decoration added later shows up here as unclaimed instead of being silently forgotten.
    //   claimed  — ids the pick ray will trigger on
    //   passthru — ids the ray sees straight through (grass/fern/water): never a trigger, and fine for a
    //              stem or a blade, wrong for a whole object
    //   blind    — ids that are neither: the ray STOPS on them and then nothing matches, so the right-click
    //              is swallowed. This is the column that says "cannot pick this up".
    //   maxVox   — the biggest single model in the group, against the flood CAP the pickup uses: a cap below
    //              this leaves part of the object standing.
    pickTable() {
      const idsOf = (ms) => { const o = new Set(); for (const m of (ms || [])) if (m && m.vox) for (const q of m.vox) o.add(q >>> 24); return [...o]; };
      const sizeOf = (ms) => (ms || []).reduce((a, m) => Math.max(a, m && m.vox ? m.vox.length : 0), 0);
      const one = (m) => (m ? [m] : []);
      const G = [
        ['rock.vox (field stone)', one(typeof ROCKV !== 'undefined' && ROCKV), 40],
        ['rocks26 boulders', typeof ROCK26 !== 'undefined' ? ROCK26 : [], 300],
        ['desert rocks', typeof DROCK !== 'undefined' ? DROCK : [], 300],
        ['stick_1/stick_2', typeof STICKV !== 'undefined' ? STICKV : [], 24],
        ['blossom twig', typeof STICKB !== 'undefined' ? STICKB : [], 24],
        ['pinecone', one(typeof CONEV !== 'undefined' && CONEV), 24],
        ['pinecone (large)', one(typeof CONEVL !== 'undefined' && CONEVL), 24],
        ['flowers', (typeof FLOWERV !== 'undefined' ? FLOWERV : []).concat(typeof FLOWERV_CH !== 'undefined' ? FLOWERV_CH : []), 64],
        ['mushrooms', typeof MUSHV !== 'undefined' ? MUSHV : [], 64],
        ['fern2', typeof FERN2V !== 'undefined' ? FERN2V : [], 0],
        ['lily pad', typeof LILYV !== 'undefined' ? LILYV : [], 0],
        ['giant lily pad', typeof LILYPAD_GIGV !== 'undefined' ? LILYPAD_GIGV : [], 0],
        ['fallen log', one(typeof LOGV !== 'undefined' && LOGV), 0],
        ['cacti', typeof CACTI !== 'undefined' ? CACTI : [], 0],
        ['desert shrubs', typeof SHRUBV !== 'undefined' ? SHRUBV : [], 0],
        ['fruit', typeof FRUITV !== 'undefined' ? FRUITV : [], 0],
        ['beehive', one(typeof HIVEV !== 'undefined' && HIVEV), 0],
      ];
      const claim = (i) => (PICK_ROCK.has(i) ? 'rock' : PICK_BOULDER.has(i) ? 'boulder' : PICK_CONE.has(i) ? 'cone'
        : PICK_STICK.has(i) ? 'stick' : FRUIT_IDS.has(i) ? 'fruit'
        : (typeof PICK_FLOWER !== 'undefined' && PICK_FLOWER.has(i)) ? 'flower' : null);
      return G.filter((g) => g[1] && g[1].length).map(([name, ms, cap]) => {
        const ids = idsOf(ms), cl = {}, pt = [], bl = [];
        for (const i of ids) { const c = claim(i); if (c) (cl[c] = cl[c] || []).push(i); else if (PASSTHRU.has(i)) pt.push(i); else bl.push(i); }
        const mx = sizeOf(ms);
        return { name, models: ms.length, ids: ids.length, claimed: cl, passthru: pt, blind: bl,
                 maxVox: mx, cap, capShort: cap > 0 && mx > cap };
      });
    },
    // ── THE PUT-DOWN LEDGER ── how many objects the player has placed, and what the PICKUP RAY would make of
    // whatever the crosshair is on: the first non-passthru voxel it stops at, and whether the ledger claims it.
    placed() {
      const cp2 = Math.cos(P.pitch), sp2 = Math.sin(P.pitch);
      const d = [Math.sin(P.yaw) * cp2, sp2, Math.cos(P.yaw) * cp2];
      let hitI = -1, hitV = 0, hx = 0, hy = 0, hz = 0;
      for (let t = 0.6; t < 45; t += 0.3) {
        const x = Math.floor(P.x + d[0] * t), y = Math.floor(smoothEye + d[1] * t), z = Math.floor(P.z + d[2] * t);
        if (y < 1 || y >= WY) break;
        const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
        const v = W[ii]; if (!v || PASSTHRU.has(v)) continue;
        hitI = ii; hitV = v; hx = x; hy = y; hz = z; break;
      }
      const e = hitI >= 0 ? placedAt(hitI) : null;
      return { n: PLACED.length, hit: hitI >= 0 ? { x: hx, y: hy, z: hz, id: hitV } : null,
               claimed: !!e, it: e ? e.it : 0, last: PLACED.length ? { it: PLACED[PLACED.length - 1].it, y: PLACED[PLACED.length - 1].y, cells: PLACED[PLACED.length - 1].cells.length } : null };
    },
    hurtInfo() { return { flash: +UF[UF_HURTB + 3].toFixed(3), world: [UF[UF_HURTB] + winOX, UF[UF_HURTB + 1], UF[UF_HURTB + 2] + winOZ], half: [UF[UF_HURTH], UF[UF_HURTH + 1], UF[UF_HURTH + 2]], dyn: UF[UF_HURTH + 3], slot: HURT.slot }; },   // dyn = the dynamic-life slot the wounded animal is DRAWN in (0 = grid-stamped, matched by bounds instead)   // the knife hit-flash box the tracer is tinting inside right now
    swing(offMs) { swingStart = performance.now() - (offMs || 0); }, drop() { dropHeld(); },
    sel(i) { selSlot = Math.max(0, Math.min(slots.length - 1, i | 0)); },
    hand() { const h = slots[selSlot]; return h ? { it: h.it, n: h.n | 0, name: ITEM_NAMES[h.it] || null, slot: selSlot } : null; },   // WHAT IS IN THE HAND, and how many of it — the hotbar is inside the game's closure, so a test that watches a stack go down (eating through one, crafting out of one) has no other way to read it
    clash() { if (dualRocks() && (slots[selSlot].n === 2 || canAdd(KNIFE_IT)) && performance.now() - clashT0 > 700) { clashT0 = performance.now(); clashSparked = false; return true; } return false; },   // same guard as shift+click — headless knife-craft testing
    KNIFE_IT,
    snow(v2) { snowOn = v2 === undefined ? !snowOn : !!v2; if (snowOn) snowEndT = performance.now() + 60000; snowBtnSync(); },
    // ── PIN THE WEATHER ── a benchmark cannot race a 60 s storm that re-arms on a 5-minute cadence: every
    // sweep long enough to be worth trusting outlives it, and the storm then lands in the middle of the run
    // and reads as the thing being measured. Holds either state for an hour.
    snowHold(v) { const t = performance.now();
      if (v === undefined) return { snowOn, endIn: Math.round(snowEndT - t), nextIn: Math.round(snowNextT - t) };
      if (v) { snowOn = true; snowEndT = t + 3.6e6; snowFreezeAt = Math.min(snowFreezeAt, t + 1000); }
      else { snowOn = false; snowEndT = 0; }
      snowNextT = t + 3.6e6; snowBtnSync();
      return { snowOn, endIn: Math.round(snowEndT - t), nextIn: Math.round(snowNextT - t) }; }, wormPos() { const o = []; for (let j = WORM_0; j < WORM_END; j++) { const B = wbf[j];
      if (B && B.init) o.push({ j, x: +B.x.toFixed(2), z: +B.z.toFixed(2), trap: +(B.trap || 0).toFixed(2) }); } return o; },   // worm stuck-test tap
    flyers() { const o = []; for (let j = FLY_0; j < FLY_END; j++) { const B = wbf[j];
      if (B && B.init) o.push({ j, kind: B.kind | 0, x: +B.x.toFixed(2), z: +B.z.toFixed(2), hx: B.hx, hz: B.hz, hcx: B.hcx, hcz: B.hcz }); } return o; },   // flyer test tap
    modelIds(kind) { const m = { drock: DROCK, cactus: CACTI, shrub: SHRUBV, rock26: ROCK26 }[kind] || []; const o = new Set(); for (const q of m) for (const p of q.vox) o.add(p >>> 24); return [...o].sort((a, b) => a - b); },   // the palette ids a model set actually uses — colour heuristics cannot tell desert rock from desert sand. NOT `ids`: this object already has an `ids` key of a different shape further down, and a duplicate silently loses (the later one wins). 276 keys in one literal makes that easy to do — check before adding a name.
    win() { return { xlo: winOX, xhi: winOX + WX, zlo: winOZ, zhi: winOZ + WZ }; },
    // ── PAGED-STORAGE FEASIBILITY CENSUS (dev) ── classify every 8³ brick as empty / uniform / mixed.
    // Storage today is DENSE: WX*WY*WZ bytes, once on the CPU and once on the GPU, and a cube of empty
    // sky costs exactly what a cube of forest costs. View distance is HALF the window width, and bytes
    // grow with width², so seeing twice as far costs 4x the memory — that is the ceiling this measures.
    // Under a brick-descriptor scheme only MIXED bricks need their 512-byte payload; empty and uniform
    // bricks collapse into their own 4-byte descriptor. The occupancy bit already answers "empty", so
    // only occupied bricks are scanned — which is why this is seconds and not minutes.
    brickCensus() {
      const t0 = performance.now();
      const nB = BX * BY * BZ;
      let empty = 0, uniS = 0, solid = 0, surf = 0;   // all-air / one material / no air but many materials / air+solid mix
      const uniOf = new Map();
      const NB = 8, band = Math.ceil(BY / NB), bands = [];
      for (let i = 0; i < NB; i++) bands.push([0, 0, 0, 0]);
      for (let bz = 0; bz < BZ; bz++) for (let by = 0; by < BY; by++) {
        const bd = bands[Math.min(NB - 1, (by / band) | 0)];
        for (let bx = 0; bx < BX; bx++) {
          const b = bx + by * BX + bz * BX * BY;
          if (!((bricks[b >> 5] >>> (b & 31)) & 1)) { empty++; bd[0]++; continue; }
          const base = by * 8 * WX + bz * 8 * WX * WY + bx * 8;
          const first = W32[base >> 2], v0 = first & 255;
          let same = v0 !== 0 && first === ((v0 * 0x01010101) >>> 0), hasAir = false;
          scan: for (let z = 0; z < 8; z++) for (let y = 0; y < 8; y++) {
            const rw = ((by * 8 + y) * WX + (bz * 8 + z) * WX * WY + bx * 8) >> 2;
            const a = W32[rw], c = W32[rw + 1];
            if (a !== first || c !== first) same = false;
            // any zero BYTE in either word = an air voxel — the standard SWAR test, one per 4 voxels
            if ((a - 0x01010101) & ~a & 0x80808080) { hasAir = true; break scan; }
            if ((c - 0x01010101) & ~c & 0x80808080) { hasAir = true; break scan; }
          }
          if (hasAir) { surf++; bd[3]++; }
          else if (same) { uniS++; bd[1]++; uniOf.set(v0, (uniOf.get(v0) || 0) + 1); }
          else { solid++; bd[2]++; }
        }
      }
      const cur = WX * WY * WZ, desc = nB * 4;
      const pc = (n) => +(100 * n / nB).toFixed(1);
      // Three candidate schemes, all keeping a 4-byte descriptor per brick:
      const mb = (n) => +(n / 1048576).toFixed(1);
      const sA = desc + (uniS === 0 ? 0 : 0) + (solid + surf) * 512;        // A: collapse empty + uniform only
      const sB = desc + surf * 512;                                          // B: also treat every AIR-FREE brick as payload-free (regenerate/procedural)
      return { window: [WX, WY, WZ], nBricks: nB,
        empty, uniformSolid: uniS, solidManyMaterials: solid, surface: surf,
        pct: { empty: pc(empty), uniformSolid: pc(uniS), solidManyMaterials: pc(solid), surface: pc(surf) },
        uniTop: [...uniOf.entries()].sort((a, b) => b[1] - a[1]).slice(0, 6).map((e) => e[0] + ':' + e[1]),
        bandsLowToHigh: bands.map((q) => q.join('/')),
        curMB: mb(cur), descMB: mb(desc),
        schemeA: { mb: mb(sA), saving: +(cur / sA).toFixed(2), widthGain: +Math.sqrt(cur / sA).toFixed(2) },
        schemeB: { mb: mb(sB), saving: +(cur / sB).toFixed(2), widthGain: +Math.sqrt(cur / sB).toFixed(2) },
        ms: Math.round(performance.now() - t0) };
    },
    // ── HOW DEEP IS ANYTHING EVER CARVED? ── the census says the bottom bands are 100% air-free rock.
    // If nothing ever cuts into them they are invisible AND untouchable, and the window is simply taller
    // than the world needs. Per column: the LOWEST y holding an air voxel. Also the enclosed-rock count —
    // an air-free brick whose six neighbours are all air-free can never be entered by any ray at all.
    depthProbe() {
      const t0 = performance.now();
      let minAir = WY, cols = 0;
      const hist = new Array(1 + (WY >> 4)).fill(0);           // lowest-air-y in 16-voxel buckets
      for (let z = 0; z < WZ; z += 2) for (let x = 0; x < WX; x += 2) {
        const c = x + z * WX * WY;
        let lo = -1;
        for (let y = 0; y < WY; y++) if (W[c + y * WX] === 0) { lo = y; break; }
        if (lo < 0) continue;
        cols++; if (lo < minAir) minAir = lo;
        hist[Math.min(hist.length - 1, lo >> 4)]++;
      }
      // enclosed rock: air-free brick with six air-free neighbours (ray-unreachable by construction)
      const airFree = new Uint8Array(BX * BY * BZ);
      for (let bz = 0; bz < BZ; bz++) for (let by = 0; by < BY; by++) for (let bx = 0; bx < BX; bx++) {
        const b = bx + by * BX + bz * BX * BY;
        if (!((bricks[b >> 5] >>> (b & 31)) & 1)) continue;     // empty brick is all air, not air-free
        let ok = 1;
        scan: for (let z = 0; z < 8; z++) for (let y = 0; y < 8; y++) {
          const rw = ((by * 8 + y) * WX + (bz * 8 + z) * WX * WY + bx * 8) >> 2;
          const a = W32[rw], c2 = W32[rw + 1];
          if (((a - 0x01010101) & ~a & 0x80808080) || ((c2 - 0x01010101) & ~c2 & 0x80808080)) { ok = 0; break scan; }
        }
        airFree[b] = ok;
      }
      let enclosed = 0;
      for (let bz = 1; bz < BZ - 1; bz++) for (let by = 1; by < BY - 1; by++) for (let bx = 1; bx < BX - 1; bx++) {
        const b = bx + by * BX + bz * BX * BY;
        if (!airFree[b]) continue;
        if (airFree[b - 1] && airFree[b + 1] && airFree[b - BX] && airFree[b + BX] &&
            airFree[b - BX * BY] && airFree[b + BX * BY]) enclosed++;
      }
      const nB = BX * BY * BZ;
      return { WY, minAirY: minAir, colsSampled: cols,
        lowestAirHist16: hist.map((n, i) => (i * 16) + ':' + n).filter((e) => !e.endsWith(':0')),
        enclosedRockBricks: enclosed, pctEnclosed: +(100 * enclosed / nB).toFixed(1),
        ms: Math.round(performance.now() - t0) };
    },   // the loaded window in WORLD coords. Sampling voxels outside it reads a ring-buffer slot holding a different world location entirely — which reads as forest terrain appearing in the desert
    desBand() { const o = []; for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j];
      o.push({ j, sp: ((j - MAM_END) / DES_PER) | 0, idx: (j - MAM_END) % DES_PER, init: !!(B && B.init), kind: B ? (B.kind | 0) : -1 }); }
      return { species: DESERTS.map((d) => d.name + '@' + d.item0 + 'x' + d.n), nDesert: (typeof nDesert === 'undefined' ? 'UNDEFINED' : nDesert), perSpecies: (typeof nDesertOf === 'undefined' ? 'OUT-OF-SCOPE (tick-local)' : DESERTS.map((d, i) => d.name + ':' + nDesertOf(i))),
        home: DESERTS.map((d) => d.name + ':' + (DES_OAKONLY[d.name] ? 'oak x' + DES_OAKONLY[d.name] : 'sand')),   // the band is two biomes now — an oak-only species must read 0 in perSpecies and its whole count here, and a sand species must read the reverse
        DES_N, DES_PER, MAM_END, DES_END, pool: DES_END, slots: o }; },   // why a desert-band slot is or is not live
    dm(x, z) { return +desertM(x, z).toFixed(3); },   // biome weight at a world point: 0 = pine forest, 1 = open desert. The gates all key on this, so a test that wants to say "in the desert" has to be able to ask
    om(x, z) { return +oakM(x, z).toFixed(3); },
    cm(x, z) { return +cherryM(x, z).toFixed(3); },
    // Read-only twin of the twig branch of tryPickup: what WOULD come away at this voxel, without removing it.
    // Exists because the pickup itself is driven by the view ray and a twig is an 8x5x3 object on a forest floor —
    // aiming a test at one is far harder than asking this. `leaf` is the number the 2026-08-18 bug got wrong.
    twigProbe(x, y, z) { const v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      if (!PICK_TWIG.has(v)) return null;
      const sc = floodScan(x, y, z, PICK_TWIG, 24); if (!sc.cells.length) return null;
      let cone = true; for (const q of sc.kinds) if (!PICK_CONE.has(q)) { cone = false; break; }
      return { kind: cone ? 'pinecone' : 'twig', body: sc.cells.length, leaf: twigLeafCells(sc.cells).length, cm: +cherryM(x, z).toFixed(2) }; },
    // ══ THE ORPHAN AUDIT (user 2026-08-18: "this issue keeps happening across other things as well") ══
    // Answers, for one voxel, the question the whole bug class turns on: the pickup that claims this voxel
    // removes SOME set of cells — is that the whole object, or does it leave part of it behind in the air?
    // `body` is what the pickup's own id set floods. `full` is the true connected component, flooded over every
    // non-terrain id there is. Anything in full and not in body is what gets orphaned, and `leftIds` names it.
    // Deliberately read-only and deliberately generic: it is cheaper to sweep this over a forest than to reason
    // about each PICK_* set by hand, and it will catch the next object somebody gives a second material to.
    pickAudit(x, y, z) {
      const at = (gx, gy, gz) => (gy < 1 || gy >= WY) ? 0 : W[gwrap(gx, WX) + gy * WX + gwrap(gz, WZ) * WX * WY];
      const v = at(x, y, z); if (!v) return null;
      const SETS = { twig: PICK_TWIG, rock: PICK_ROCK, boulder: PICK_BOULDER, apple: PICK_APPLE, orange: PICK_ORANGE };
      let name = null, set = null;
      for (const k in SETS) if (SETS[k] && SETS[k].has(v)) { name = k; set = SETS[k]; break; }
      if (!set) return null;
      const body = floodScan(x, y, z, set, 400);
      // the TRUE component: everything a stamped object can be made of — decor and foliage, but never the
      // terrain it rests on, or the flood would swallow the hillside and report the whole world as orphaned.
      const loose = new Set(); for (let i = 1; i < 256; i++) if (decorTab[i] || foliaTab[i] || floatTab[i]) loose.add(i);
      const full = floodScan(x, y, z, loose, 4000);
      const bodySet = new Set(body.cells), left = full.cells.filter((ii) => !bodySet.has(ii));
      const leftIds = [...new Set(left.map((ii) => W[ii]))];
      return { pick: name, seedId: v, body: body.cells.length, full: full.cells.length,
               orphaned: left.length, leftIds, leftCols: leftIds.map((i) => palette[i]) };
    },
    // ── WHICH IDS STING, AND WHO ELSE IS WEARING THEM ── cactusTab is keyed by palette ID, not by biome or by
    // model, so any voxel that ends up on a cactus id hurts the player wherever it stands. That matters because
    // edCol SUBSTITUTES a colour it cannot mint on a full table (palNearest), and a substituted pink lands on the
    // nearest pink in the table — which is the cactus flower's. This tap is how to tell.
    stingIds() { const out = [];
      for (let i = 1; i < 256; i++) if (cactusTab[i]) out.push({ id: i, col: palette[i], crea: !!CREA_FLAG[i] });
      return out; },
    errLog() { return (window.__vbErrLog || []).slice(); },   // the last 32 errors with uptime stamps — read this FIRST after any freeze or crash; a GPU device loss lands here too, and it is the one failure that shows no console error at all
    lastPick() { return { ...LAST_PICK }; },           // the last twig pickup: `body` browns + `leaf` blossom voxels. leaf 0 on a cherry twig means the leaf was left hanging (the 2026-08-18 bug); it should be 4 on stick_1 and 7 on stick_2
    blosDbg() {                                      // BLOSMAP/BLOSMAPW are gone — one rank table now, and the ramp is an argument (assets/bow.js blosRemap)
      const shades = (V) => { const u = new Set(); for (const m of V) for (const q of m.vox) u.add(q >>> 24); return u.size; };
      return { oakv: OAKV.length, blosv: OAKBLOSV.length, whitv: OAKWHITV.length,
        leaf: OAKLEAF.slice(), rank: OAKLEAF.map((i) => BLOSRANK[i]),
        pinkRamp: BLOSLEAF.length, whiteRamp: BLOSWHITE.length,
        // …and what the crowns ACTUALLY wear, which is the number the dither exists to raise: a per-id remap
        // could never exceed OAKLEAF.length however long the ramp was. Bark is in these counts too, hence +bark.
        pinkShadesUsed: shades(OAKBLOSV), whiteShadesUsed: shades(OAKWHITV) }; },
    // …the cherry forest's counterpart to oakIds(). `oakv` and `blosv` MUST be equal: the pink set is derived
    // from OAKV and berryBush splices OAKV after it is loaded, so a mismatch here means OAKBLOSV[k] is a
    // different tree from OAKV[k] and the gen pool and the main thread are stamping different geometry. That is
    // not hypothetical — it is what this tap was written to find, and __vb.gtest reported 21,640 diffs from it.   // …and the THIRD border. cherryM is a sub-region of oakM, not a disjoint band, so a cherry column reads om 1 AND cm 1 — "which biome is this" is cm first, then om, then dm, and never om alone     // …and the OTHER border, the same way: 1 = oak forest, 0 = pine forest. Between them the two answer "which of the three biomes is this column", and every oak gate keys on this one
    lifeAll() { const o = []; for (let j = 0; j < DES_END; j++) { const B = wbf[j];   // DES_END, not a literal: this read 380 and silently hid six of the seven desert species behind its own bound   // EVERY live creature body with a position, across all the slot bands (flyers, worms, fish, mammals) — the biome spawn gates need one census, not four taps
      if (B && B.init && typeof B.x === 'number') o.push({ j, kind: B.kind | 0, x: +B.x.toFixed(1), y: +(B.y || 0).toFixed(1), z: +B.z.toFixed(1), dm: +desertM(B.x, B.z).toFixed(2),
        om: +oakM(B.x, B.z).toFixed(2), hom: (B.hx !== undefined && B.hz !== undefined) ? +oakM(B.hx, B.hz).toFixed(2) : null,   // …and the OAK mask, for the same reason dm is here: since 2026-08-17 the band holds oak-only species and 'is it home' cannot be answered off desertM alone
        hdm: (B.hx !== undefined && B.hz !== undefined) ? +desertM(B.hx, B.hz).toFixed(2) : null }); } return o; },   // dm = where it IS, hdm = where its HOME is. A spawn gate can only be judged by the home: a creature that was placed legally in the forest and has since walked or swum across a BLENDED border is not a spawn leak.
    snowEdges() { return { lead: UF[1269], trail: UF[1270], vis: snowVis }; }, snowStats() { return { on: snowOn, live: (snowQN - snowHead) + (snowWN - snowWHead), liveWater: snowWN - snowWHead, freezeK: freezeK, nextT: snowNextT, endT: snowEndT, freezeAt: snowFreezeAt }; },
    worldDump(wx0, wz0, n) { const out = new Uint8Array(n * n * WY); let k = 0;   // raw voxel block dump (base64) — lets a test DIFF two builds voxel-by-voxel
      for (let z = wz0; z < wz0 + n; z++) for (let x = wx0; x < wx0 + n; x++) { const b9 = gwrap(z, WZ) * WX * WY + gwrap(x, WX);
        for (let y = 0; y < WY; y++) out[k++] = W[b9 + y * WX]; }
      let s9 = ''; for (let i = 0; i < out.length; i += 32768) s9 += String.fromCharCode.apply(null, out.subarray(i, Math.min(i + 32768, out.length)));
      return btoa(s9); },
    worldHash(wx0, wz0, n) { let h9 = 0x811c9dc5;      // FNV-ish hash of an n×WY×n world block (world coords) — proves a gen change is bit-identical
      for (let z = wz0; z < wz0 + n; z++) for (let y = 0; y < WY; y++) { const b9 = gwrap(z, WZ) * WX * WY + y * WX;
        for (let x = wx0; x < wx0 + n; x++) { h9 = (Math.imul(h9 ^ W[b9 + gwrap(x, WX)], 16777619)) >>> 0; } }
      return h9; },
    gen() { const t = rectTarget(); return { bands: genBands, ms: Math.round(genMs), spinMs: Math.round(genSpinMs), fps: Math.round(fpsEma),
      deficit: { xlo: rect.xlo - t.xlo, xhi: t.xhi - rect.xhi, zlo: rect.zlo - t.zlo, zhi: t.zhi - rect.zhi },   // how far the built rect lags its target on each side (0 = fully caught up)
      pool: poolOk, q: poolQueue.length, busy: genPool ? genPool.filter((w) => w.busyId).length : 0 }; },   // streaming-gen telemetry tap
    fish() { const o = []; const bodyClip = (B) => { const hx = Math.sin(B.th), hz = Math.cos(B.th), ay = Math.floor(B.y), hf = B.fhalf || 5; for (const a of [hf, hf * 0.5, 0, -hf * 0.5, -hf]) { const bx = Math.floor(B.x + hx * a), bz = Math.floor(B.z + hz * a); for (let yy = ay - 1; yy <= ay + 2; yy++) if (solid(bx, yy, bz)) return true; } return false; };   // per-species body length — the old fixed ±5 reported every short minnow as clipping
      for (let j = FISH_0; j < FISH_END; j++) { const B = wbf[j]; if (B && B.init) o.push({ j, sp: (FISHES[B.fsp || 0] || {}).name, sch: B.school, x: +B.x.toFixed(1), y: +B.y.toFixed(1), z: +B.z.toFixed(1), spd: +(B.spd || 0).toFixed(1), trap: +(B.trap || 0).toFixed(1), inSolid: bodyClip(B), d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)), air: B.jumpV !== undefined, arm: B.jumpArm !== undefined, flee: ((B.fleeT || 0) * 1000 > performance.now()) || spooked(B), vyS: +(B.vyS || 0).toFixed(1) }); }
      return { species: FISHES.map((f) => f.name + '(' + f.n + 'f@' + f.item0 + ')'), live: o }; },   // fish test tap
    vox(x, y, z) { return W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY]; }, rect, bird, wbf, H, riverAt, lilyGigAt, tryPickup, ids: { PEBBLE, STICK_S, GRASS, MOSS, BROCK, SNOW, SAND, DIRT, NEEDLE }, slots, LILY_SZ, WL, fishCfg: FISH_CFG,   // lilyGigAt: test tap — the GIANT pads no longer stamp, so a candidate site must come back with an empty footprint; fishCfg: LIVE fish tunables (speed/flee/jump/pitch)
    dbgEye() { return smoothEye; }, get WORM_ITEM0() { return WORM_ITEM0; }, get grabActive() { return !!grabAnim; },
    dof(o) { if (o) { if (o.on !== undefined) { dofStr = o.on ? Math.max(dofStr, 0.3) : 0; dofCocK = DOF_COC * dofStr; dofStrSync(); }   // there is no on/off state any more — the slider IS the switch, so {on:false} parks it at 0 and {on:true} restores at least the default 30%
                      if (o.focus !== undefined) dofLock = o.focus;
                      if (o.strength !== undefined) { dofStr = o.strength; dofCocK = DOF_COC * dofStr; dofStrSync(); }   // the settings slider's own lane, panel and all
                      if (o.taps !== undefined) dofTapK = o.taps;                                                       // taps per pixel of radius — 40 reproduces the flat 32 the effect shipped with
                      if (o.coc !== undefined) dofCocK = o.coc; }                                                       // …and the raw aperture underneath it, which deliberately does NOT move the slider: it is for A/B-ing a radius the slider cannot reach
             return { on: dofStr > 0, strength: dofStr, focus: +UF[UF_DOF].toFixed(2), maxCocPx: +UF[UF_DOF + 1].toFixed(2),
                      taps: dofTapK, tapsAtMax: Math.max(8, Math.min(32, Math.ceil(UF[UF_DOF + 1] * dofTapK))), lockedFocus: dofLock, cocFrac: dofCocK }; },   // tapsAtMax mirrors the shader's own clamp, FLOOR included, or the report undercounts every small disc   // depth-of-field tap: read the live focal plane, or PIN focus/aperture to A/B the effect at a fixed strength (__vb.dof({focus: 20, coc: 0.02}), {focus: 0} hands it back to autofocus)
    snd() { const eff = [], amb = [], mus = [];        // AUDIO BUS census: every registered sound's LIVE element volume, split by bus. The elements are `new Audio()` and never enter the DOM, so a test has no other way to see that the sfx slider actually reached them.
            for (const s9 of sndReg) (s9.bus === BUS_SFX ? eff : s9.bus === BUS_MUS ? mus : amb).push(+s9.a.volume.toFixed(4));
            return { vol: sndVol, sfx: sfxVol, mus: musVol, gain: SND_GAIN, n: sndReg.length,
                     effects: { n: eff.length, min: eff.length ? Math.min.apply(null, eff) : null, max: eff.length ? Math.max.apply(null, eff) : null },
                     music: mus, ambience: amb }; },
    anthem(sec) { if (sec !== undefined) { playSecs = sec; if (anthemIdx < anthemSnds.length) anthemDone = false; }   // jump the PLAY CLOCK (__vb.anthem(59.5)) and let the next tick fire it — waiting out a minute of real gameplay is not a test
                  const cur = anthemSnds.findIndex((a9) => !a9.paused);
                  return { at: ANTHEM_AT, gap: ANTHEM_GAP, played: +playSecs.toFixed(2), nextAt: anthemNextAt, nextIdx: anthemIdx,
                           // ── anthemOrder, NOT ANTHEM_SET ── anthemSnds is built from the SHUFFLED copy, so
                           // indexing it with ANTHEM_SET's order paired every sound with the wrong name: this
                           // reported the declared order on every run and named the wrong track as playing,
                           // which is exactly the appearance of a shuffle that is not happening. The shuffle
                           // was fine; the instrument was reading the reference list (ui/audio.js).
                           setDone: anthemDone, playing: cur >= 0 ? anthemOrder[cur][0] : null,
                           t: cur >= 0 ? +anthemSnds[cur].currentTime.toFixed(2) : 0,
                           order: anthemOrder.map((t9) => t9[0]),
                           tracks: anthemOrder.map(([n9, g9], i9) => ({ name: n9, gain: g9, vol: +anthemSnds[i9].volume.toFixed(4), playing: !anthemSnds[i9].paused })),
                           bus: 'mus' }; },
    vig(v) { if (v !== undefined) { cineBlurK = v; blurLock = true; } return { uniform: UF[1268], on: vigOn, cineWeight: cineBlurK }; },   // cinematic vignette tap: read the live weight, or PIN it (blurLock) to A/B the effect at a fixed strength
    vigFree() { blurLock = false; }, get swingStart() { return swingStart; }, get locked() { return locked; }, edFrameBox() { const n = ED.frames.length; if (!n) return null; const f = ED.frames[((ED.sel % n) + n) % n]; return { bx: f.bx, bz: f.bz, sx: f.sx, sy: f.sy, sz: f.sz, y: ED.y }; },   // test taps
    solidAt(x, y, z) { const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY]; return !!(id && solidTab[id]); },
    solidAt2(x, y, z) { return solid(x, y, z); },      // what the PLAYER actually collides with (cones exempted) — solidAt reports the raw table
    boxFreeAt(x, y, z) { return boxFree(x, y, z, HEIGHT); },   // can the player's whole box sit here?
    lifeUidAt(s) { return lifeUid[s | 0]; },           // which pool slot is DRAWN in drop slot s (2000+slot), or -1
    // ── WHY IS THERE NO LIFE RIGHT NOW? (user 2026-08-20, after ~8 rounds of "the life is disappearing") ──
    // ONE call that answers it, because every mechanism that can empty this world is silent and they all look
    // identical from the canvas: the picture is perfect and the creatures are simply not there. The four are
    // DEATH (`dead` retires every band — measured 522 -> 49 in full daylight the instant the player died), the
    // EDITOR (`ED.on`, same gate, same effect), NIGHT (sun elevation under -0.06; the perched songbirds stay,
    // which is the tell), and a TICK THROW (the loop is caught in main/tick.js, so a fault leaves a rendered
    // frame and no error anywhere the player can see). Guessing between them from a screenshot is impossible,
    // and each has a completely different fix — hence a tap that just says which.
    lifeWhy() {
      const t = tday, sun = Math.sin(Math.sin(t * Math.PI * 2 - Math.PI / 2) * 1.05);
      const nk = Math.max(0, Math.min(1, (sun + 0.06) / 0.16));
      const lb = this.lifeBands(), bands = {};
      let alive = 0, want = 0;
      for (const k in lb.bands) { const b = lb.bands[k], w = lb.want[k === 'duckMom' ? 'duck' : k];
        alive += b.alive; if (typeof w === 'number') want += w;
        if (b.alive || w) bands[k] = b.alive + '/' + (w === undefined ? '-' : w); }
      const errs = this.errLog();
      const why = [];
      if (typeof dead !== 'undefined' && dead) why.push('PLAYER IS DEAD — every band is retired until you respawn');
      if (ED.on) why.push('ASSET EDITOR IS OPEN (ED.on) — the world is frozen and all life retired');
      if (sun < -0.06) why.push('NIGHT — day species are gone by design; perched songbirds stay');
      else if (nk < 1) why.push('DUSK/DAWN — populations are ramping (NIGHT_K ' + nk.toFixed(2) + ')');
      if (errs.length) why.push('TICK ERRORS LOGGED (' + errs.length + ') — see errLog(); a throw aborts the frame before the creatures are emitted');
      if (!why.length && alive < want * 0.5) why.push('NO KNOWN CAUSE — placement is failing; report this line');
      return { verdict: why.length ? why : ['healthy'], alive, want,
        clock: { tday: +t.toFixed(3), sunEl: +sun.toFixed(3), night: sun < -0.06, nightK: +nk.toFixed(2) },
        player: { dead: typeof dead === 'undefined' ? '?' : !!dead, hp: VIT.hp, hpMax: VIT_HP_MAX, editor: !!ED.on },
        draw: this.lifeBudget(), bands, errs: errs.slice(0, 2).map((e) => e.msg.slice(0, 160)) };
    },
    lifeBudget() { return { base: 25, total: DROP_SLOTS, drawn: lifeUid.filter((u) => u >= 2000).length }; },
    // ── THE SLOT LADDER, AS THE GAME ACTUALLY BUILT IT ── every band boundary used to be a hard-coded integer
    // repeated across eleven fragments, and a missed one does not throw: it silently mis-classifies (a bunny
    // counted as a fish, a porcupine that never bleeds). They all derive from sim/life/slots.js now, and this
    // prints the derived ladder beside the LIVE population each band is being asked for, so "is the pool the
    // size I think it is" and "is anything landing in the wrong band" are one call rather than an audit.
    // `alive` counts B.init in the band; `want` is what tick-life asked for this frame. lifeCensus() is the
    // one to cross-check it against — it classifies by slot too, so the two disagreeing means a missed literal.
    lifeBands() {
      const B9 = [['flyer', FLY_0, FLY_END], ['duckMom', DUCK_0, DUCK_END], ['duckling', BABY_0, BABY_END],
        ['worm', WORM_0, WORM_END], ['perched', CARD_0, CARD_END], ['fish', FISH_0, FISH_END],
        ['bunny', BUNNY_0, BUNNY_END], ['armadillo', ARM_0, ARM_END], ['skunk', SKUNK_0, SKUNK_END],
        ['porcupine', PORC_0, FLAM_0], ['flamingo', FLAM_0, FLAM_END], ['desert', MAM_END, DES_END]];
      const o = {};
      for (const [nm, lo, hi] of B9) { let n = 0;
        for (let j = lo; j < hi; j++) { const q = wbf[j]; if (q && q.init && !q.slain) n++; }
        o[nm] = { lo, hi, slots: hi - lo, alive: n }; }
      return { keep: CARD_KEEP, keepWas: CARD_KEEP_V0, densK: +LIFE_DENS_K.toFixed(4), pool: DES_END,
        want: Object.assign({}, LIFE_WANT),
        bands: o }; },
    // ── WHO IS ALIVE, AND WHO IS ACTUALLY ON SCREEN ── per kind. The gap between `active` and `drawn` is the
    // slot famine: the trace-injected creatures share whatever is left of the 64 drop slots after the flying
    // songbirds, and the surplus is dropped by DISTANCE ALONE, so a dense near kind can starve a far one out
    // of the frame entirely. dHiddenMin is the closest creature of that kind that did NOT get drawn — the
    // number that tells you a duck 60 voxels away is invisible. Grid-stamped kinds live in W, not in a slot,
    // so they report drawn: -1 rather than a misleading 0.
    lifeCensus() {
      const STAMPED = LIFE_UNI ? { cardinal: 1 } : { cardinal: 1, bunny: 1, armadillo: 1, skunk: 1, porcupine: 1 };   // under ?uni none of the five is stamped any more - they are in the drop-slot ledger like every other creature, so report their real drawn/hidden counts instead of the -1 placeholder
      // …and the LAST arm is the desert species by name, not an open-ended 'porcupine': this chain used to end
      // at the porcupine, so every one of the 56 desert slots was counted and reported as a porcupine.
      const bandOf = (wk) => wk < FLY_END ? 'flyer' : wk < DUCK_END ? 'duckMom' : wk < BABY_END ? 'duckling' : wk < WORM_END ? 'worm'
        : wk < CARD_END ? 'cardinal' : wk < FISH_END ? 'fish' : wk < BUNNY_END ? 'bunny' : wk < ARM_END ? 'armadillo' : wk < SKUNK_END ? 'skunk'
        : wk < FLAM_0 ? 'porcupine' : wk < FLAM_END ? 'flamingo' : ((DESERTS[((wk - MAM_END) / DES_PER) | 0] || {}).name || 'desert');
      const shown = new Set();
      for (let s = 0; s < DROP_SLOTS; s++) { const u = lifeUid[s]; if (u >= 2000) shown.add(u - 2000); }
      const acc = {};
      for (let wk = 0; wk < DES_END; wk++) { const B = wbf[wk];
        if (!B || !B.init || B.slain) continue;
        let k = bandOf(wk); if (k === 'flyer' && B.dfly) k = 'dragonfly';
        const a = acc[k] || (acc[k] = { active: 0, drawn: 0, grid: 0, dDrawnMax: 0, dGridMin: 1e9, dHiddenMin: 1e9, dHiddenMax: 0 });
        const d = Math.hypot(B.x - P.x, B.z - P.z);
        a.active++;
        if (shown.has(wk)) { a.drawn++; if (d > a.dDrawnMax) a.dDrawnMax = d; }
        else if (B.sN > 0) { a.grid++; if (d < a.dGridMin) a.dGridMin = d; }   // GRID-STAMPED, and therefore fully on screen - it lives in W rather than in a drop slot. Under the ?uni songbird hybrid this is the far half of one kind, and counting it as 'hidden' would invent a regression that is not in the image.
        else if (!STAMPED[k]) { if (d < a.dHiddenMin) a.dHiddenMin = d; if (d > a.dHiddenMax) a.dHiddenMax = d; }
      }
      for (const k in acc) { const a = acc[k];
        a.dDrawnMax = Math.round(a.dDrawnMax);
        a.dGridMin = a.dGridMin > 1e8 ? -1 : Math.round(a.dGridMin);   // the NEAREST grid-stamped one: under the hybrid this should sit just outside UNI_BIRD_R, and if it ever drops well inside it the fork is mis-firing
        a.dHiddenMin = a.dHiddenMin > 1e8 ? -1 : Math.round(a.dHiddenMin);
        a.dHiddenMax = Math.round(a.dHiddenMax);
        if (STAMPED[k]) { a.drawn = -1; a.dDrawnMax = -1; } }
      // firstCreature is COMPUTED, not the literal 25 it used to be. That literal was written when the particle
      // pool held 20 slots; the pool has since grown twice (death smoke, then pollen, then the cherry petals) and
      // the emit's real cursor is `5 + sparks3d.length`, which is 45. So this reported 95 slots free for traced
      // creatures when the true figure was 75 — a fifth of the budget, invented. It is the same failure as the
      // harness printing "(none)" over a game loop that was throwing: an instrument that cannot be wrong about
      // the number it exists to report. Read off the array, like the emit and the drop-slot reserve both do.
      const first9 = lifeSlotBase;   // 0-7 item drops, 8 cardinal, 9.. the particle band, then creatures
      return { slots: { firstCreature: first9, flock: BIRD_N - 1, flockDrawn: BIRD_SLOTS, firstFree: first9 + BIRD_SLOTS, total: DROP_SLOTS, forTraced: DROP_SLOTS - (first9 + BIRD_SLOTS) }, kinds: acc };
    },
    ed(v) { ((v === undefined) ? !ED.on : !!v) ? edEnter() : edExit(); }, edSel: edSelStep, edMove: edMoveStep, edRotate, edImport: edImportBufs, edExport: edExportSeq,
    edGiz(v) { ED.paused = true; ED.giz = v === undefined ? !ED.giz : !!v; if (ED.giz) ED.rgiz = false; edEnsureGizCols(); edLayout(); return { giz: ED.giz, boxes: ED.gizBoxes.map((g) => g.axis) }; }, edOff: edOffset, edCopy2: edCopyOffsets,   // move-gizmo test taps
    edRgiz(v) { ED.paused = true; ED.rgiz = v === undefined ? !ED.rgiz : !!v; if (ED.rgiz) ED.giz = false; edEnsureRgizCols(); edLayout(); return { rgiz: ED.rgiz, rings: ED.rgizBoxes.map((g) => g.kind) }; },   // rotate-gizmo test tap
    edRot(kind, dir) { edApplyRot(kind, dir); const n = ED.frames.length, f = n ? ED.frames[((ED.sel % n) + n) % n] : null; return f ? { sx: f.sx, sy: f.sy, sz: f.sz, nvox: f.vox.length } : null; },   // rotate + report dims (test)
    edFrameOff() { const n = ED.frames.length; if (!n) return null; const f = ED.frames[((ED.sel % n) + n) % n]; return { ox: f.ox || 0, oy: f.oy || 0, oz: f.oz || 0, name: f.name }; },
    cardDbg() { return { sel: ED.sel, blink: ED.blink, spin: ED.spin, name1: ED.name1, arm: !!ED.arm, frames: ED.frames.map((f, i) => ({ i, name: f.name, oy: f.oy || 0, hasBlink: !!f.voxBlink })) }; },   // editor-cardinal test tap
    pal(i) { return i === undefined ? palette.length : palette[i]; },
    treeAt, TCELL, isCone(id) { return PICK_CONE.has(id); },
    treeDims() { return { MSX, MSY, MSZ, rot: MROT.map((R) => ({ sx: R.sx, sz: R.sz })), foliage: foliageIds.slice() }; },
    dropAxis(i) { const o = dropOff(i | 0); return UF[o + 7] > 0.5 ? [+UF[o + 4].toFixed(3), +UF[o + 5].toFixed(3), +UF[o + 6].toFixed(3)] : null; },   // a dropped item's X axis in camera space — a spinning drop's changes every frame
    dropUp(i) { const o = dropOff(i | 0); return UF[o + 7] > 0.5 ? [+UF[o + 12].toFixed(3), +UF[o + 13].toFixed(3), +UF[o + 14].toFixed(3)] : null; },   // a dropped item's own +z in camera space — which way up it is hovering
    uf(i) { return UF[i | 0]; },                       // raw uniform slot — lets a test read a drop's whole orientation basis
    arrowChunks() { return PH.bodies.filter((b) => b.nearR).map((b) => ({ vox: b.n, reach: b.nearR, normalReach: PH.absorbR, timed: !!b.absorbAt, sleeping: !!b.sleeping, absorbing: !!b.absorbing, d: Math.round(Math.hypot(b.pos[0] - P.x, b.pos[1] - smoothEye, b.pos[2] - P.z)) })); },   // chunks an ARROW knocked loose: they wait to be walked up to. d = how far the player is from each RIGHT NOW — the number the collection radius is actually tested against
    bodiesNear() { return PH.bodies.map((b) => ({ vox: b.n, arrow: !!b.nearR, timed: !!b.absorbAt, absorbing: !!b.absorbing, src: b.src,
      y: +b.pos[1].toFixed(1), vy: +b.vel[1].toFixed(2), sleeping: !!b.sleeping, rest: b.restT | 0,
      d: Math.round(Math.hypot(b.pos[0] - P.x, b.pos[1] - smoothEye, b.pos[2] - P.z)) })); },   // …and the same for EVERY rigid body, so a test can catch one collecting itself from across the map whatever made it — or one that stopped falling in mid-air
    bodyMix() { return PH.bodies.map((b, i) => { let wood = 0, fol = 0, other = 0;   // WHAT each rigid body is made of — the tap that shows an axe swing taking needles when it was aimed at bark (user 2026-08-07)
      for (let k = 0; k < b.n; k++) { const q = b.id[k]; if (woodTab[q]) wood++; else if (foliaTab[q]) fol++; else other++; }
      return { i, vox: b.n, wood, fol, other, src: b.src, born: Math.round(performance.now() - b.born),
        d: Math.round(Math.hypot(b.pos[0] - P.x, b.pos[1] - smoothEye, b.pos[2] - P.z)) }; }); },
    fishState() { const o = [];                      // per-fish navigation health: is the body legally placed, is the wall-in-face backstop tripped, how far does its chosen lane run
      for (let j = FISH_0; j < FISH_END; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 6) continue;
        o.push({ j, x: +B.x.toFixed(1), y: +B.y.toFixed(1), z: +B.z.toFixed(1), th: +(B.th || 0).toFixed(2),
          spd: +(B.spd || 0).toFixed(1), bk: !!B.bkOn, tight: !!B.bkTight, flee: !!B.fleeing,
          reach: +(B.navReach === undefined ? -1 : B.navReach).toFixed(1),
          rep: +Math.hypot(B.repX || 0, B.repZ || 0).toFixed(1),
          trap: +(B.trap || 0).toFixed(2), air: B.jumpV !== undefined,
          d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)) }); }
      return o; },
    mamSeatCmp() { const o = [];                     // the OLD stamped seat (one heightmap column) vs the ONE seat both paths use now — the gap is the correction
      for (let j = MAM_0; j < MAM_END; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const fit = j >= FLAM_0 ? MAMFIT.flam : j >= PORC_0 ? MAMFIT.porc : (j >= SKUNK_0 ? MAMFIT.skunk : (j >= ARM_0 ? MAMFIT.arm : MAMFIT.bunny));
        o.push({ j, kind: j >= FLAM_0 ? 'flam' : j >= PORC_0 ? 'porc' : (j >= SKUNK_0 ? 'skunk' : (j >= ARM_0 ? 'arm' : 'bunny')),
          oldG: Math.round(__vb.bfSurf(B.x, B.z)), newG: +mamSeatG(B, fit).toFixed(2),
          navCtr: +navWalkStand(B.x, B.z).toFixed(2), surfCtr: +__vb.bfSurf(B.x, B.z).toFixed(2) }); }
      return o; },
    desSeat() { const o = [];                        // WHICH TERM LIFTS A DESERT CREATURE. Its seat is max(mamSeatG, gFwd) and the two read DIFFERENT planes: mamSeatG goes through navWalkStand (which refuses a surface more than one step-up above the column) while the forward probe reads navGroundAt raw, so a cactus or a rock wall 3 voxels ahead is a legal answer to "how high is my floor". Reported side by side, against H = the PRISTINE generated sand column, so a lift can be attributed rather than guessed at.
      for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 2 || typeof B.x !== 'number') continue;
        const sp = ((j - MAM_END) / DES_PER) | 0, nm = DESERTS[sp] ? DESERTS[sp].name : '?', fit = MAMFIT[nm]; if (!fit) continue;
        const hx = Math.sin(B.th || 0), hz = Math.cos(B.th || 0);
        o.push({ j, sp: nm, x: +B.x.toFixed(1), z: +B.z.toFixed(1), y: +B.y.toFixed(2), seat: fit.seat,
          foot: +(B.y - fit.seat).toFixed(2), seatG: +mamSeatG(B, fit).toFixed(2),
          gFwd: +navGroundAt(B.x + hx * 3, B.z + hz * 3).toFixed(2), fwdStand: +navWalkStand(B.x + hx * 3, B.z + hz * 3).toFixed(2),
          stand: +navWalkStand(B.x, B.z).toFixed(2), ground: +navGroundAt(B.x, B.z).toFixed(2), H: H(Math.round(B.x), Math.round(B.z)),
          seatSand: +mamSeatG(B, fit, 1).toFixed(2), sand: navSand(B.x, B.z), sandFwd: navSand(B.x + hx * 3, B.z + hz * 3) }); }
      return o; },
    sndRouting() {                                   // which registered sounds are routed through an effect, and whether the RECORDER could tap them
      const out = sndReg.map((s) => ({ src: (s.a.src || '').split('/').slice(-2).join('/'),
        fx: !!s.a._sfxOut, veTapped: !!s.a._veTap, vol: +s.a.volume.toFixed(3) }));
      return { sounds: out.filter((o) => o.fx || o.src.indexOf('hit.mp4') >= 0), sharedCtx: !!sfxAC, veCtxIsShared: VE.ac === null || VE.ac === sfxAC }; },
    veTapAll() { const d = veAudioDest(); for (const s of sndReg) veTapEl(s.a);   // force the recorder's tap exactly as starting a capture does
      return { dest: !!d, tapped: sndReg.filter((s) => s.a._veTap).length, total: sndReg.length,
        fxTapped: sndReg.filter((s) => s.a._sfxOut && s.a._veTap).length, fxTotal: sndReg.filter((s) => s.a._sfxOut).length }; },
    mamSeatProbe() { const o = [];                   // PAIRED density comparison on one pose: how much ground does each resolution MISS versus a fine grid?
      for (let j = MAM_0; j < MAM_END; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const fit = j >= FLAM_0 ? MAMFIT.flam : j >= PORC_0 ? MAMFIT.porc : (j >= SKUNK_0 ? MAMFIT.skunk : (j >= ARM_0 ? MAMFIT.arm : MAMFIT.bunny));
        const st = mamSeatSteps(fit);
        o.push({ kind: j >= FLAM_0 ? 'flam' : j >= PORC_0 ? 'porc' : (j >= SKUNK_0 ? 'skunk' : (j >= ARM_0 ? 'arm' : 'bunny')),
          steps: st, s3: mamSeatN(B, fit, 1, 1), sD: mamSeatN(B, fit, st[0], st[1]), sT: mamSeatN(B, fit, 6, 6) }); }
      return o; },
    desSeat() { const o = [];                       // DESERT ground-seat decomposition: every term the y servo feeds on, so a float can be blamed on the term that actually produced it
      for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const spD = DESERTS[((j - MAM_END) / DES_PER) | 0]; if (!spD) continue;
        const fit = MAMFIT[spD.name]; if (!fit) continue;
        const th = B.th || 0, hx = Math.sin(th), hz = Math.cos(th);
        let gF = -1e9, gN = 1e9, gS = -1e9;           // FINE scan (13x13) of the WHOLE footprint (gF/gN) and of the WEIGHT-BEARING middle MAM_SPAN of it (gS) — the clip/float ground truth the coarse seat is an approximation of
        for (let u = -6; u <= 6; u++) for (let v = -6; v <= 6; v++) {
          const sa = fit.hd * u / 6, sc = fit.hw * v / 6;
          const q = navWalkStand(B.x + hx * sa + hz * sc, B.z + hz * sa - hx * sc);
          if (q > gF) gF = q; if (q < gN) gN = q;
          if (Math.abs(u) <= 6 * MAM_SPAN && q > gS) gS = q; }
        const seatG = mamSeatG(B, fit), fwd = navGroundAt(B.x + hx * 3, B.z + hz * 3);
        const bot = B.y - fit.seat;                  // world y of the model's LOWEST occupied layer (see mamFitOf: seat = h/2 - z0)
        let vinS = 0, vinF = 0;                      // model voxels vs terrain voxels, the honest clip test — the model's lowest 3 layers against the real column
        for (let u = -2; u <= 2; u++) for (let v = -1; v <= 1; v++) {
          const wb = Math.abs(u) <= 1;               // |u| <= 1 maps to the weight-bearing middle; |u| = 2 is the overhang the seat deliberately lets graze
          const sa = fit.hd * (wb ? MAM_SPAN * u : u / 2), sc = fit.hw * (wb ? MAM_SPAN * v : v);
          const px = B.x + hx * sa + hz * sc, pz = B.z + hz * sa - hx * sc;
          const bx = gwrap(Math.floor(px), WX), bz = gwrap(Math.floor(pz), WZ) * WX * WY;
          let hit = 0;
          for (let y = Math.round(bot); y <= Math.round(bot) + 2 && y < WY; y++) { const w = W[bx + y * WX + bz]; if (w && solidTab[w] === 1) { hit = 1; break; } }
          if (hit) { vinF++; if (wb) vinS++; } }
        let vg = -1e9;                               // …and the SAME question asked of the VOXELS: navWalkStand deliberately refuses a surface more than one step above the heightmap, so a body standing on a boulder top reads as floating to any nav-based metric. This one cannot be fooled that way.
        for (let u = -2; u <= 2; u++) for (let v = -1; v <= 1; v++) {
          const px = B.x + hx * (fit.hd * u / 2) + hz * (fit.hw * v), pz = B.z + hz * (fit.hd * u / 2) - hx * (fit.hw * v);
          const bx = gwrap(Math.floor(px), WX), bz = gwrap(Math.floor(pz), WZ) * WX * WY;
          for (let y = Math.min(WY - 2, Math.round(bot) + 2); y >= 1; y--) {
            const w = W[bx + y * WX + bz]; if (!w || solidTab[w] !== 1) continue;
            if (y + 1 > vg) vg = y + 1; break; } }
        o.push({ j, sp: spD.name, x: +B.x.toFixed(2), y: +B.y.toFixed(2), z: +B.z.toFixed(2), th: +th.toFixed(2),
          bot: +bot.toFixed(2), seatG: +seatG.toFixed(2), fwd: +fwd.toFixed(2), tgt: +Math.max(seatG, fwd).toFixed(2),
          lag: +(bot - Math.max(seatG, fwd)).toFixed(2),
          fwdLift: +(Math.max(seatG, fwd) - seatG).toFixed(2),
          floatF: +(bot - gF).toFixed(2),
          floatN: +(bot - gN).toFixed(2),
          floatS: +(bot - gS).toFixed(2),          // …over the weight-bearing span only: negative here is real sinking, not the deliberate nose/tail graze
          sl: +((navWalkStand(B.x + hx * 4, B.z + hz * 4) - navWalkStand(B.x - hx * 4, B.z - hz * 4)) / 8).toFixed(3),
          vg: vg, vfloat: +(bot - vg).toFixed(2),   // vfloat = the VOXEL gap under the model's lowest layer: > 0 is real air
          vinS: vinS, vinF: vinF,                  // …and the CLIP question asked directly and without saturation: how many footprint columns have a SOLID voxel inside the model's own lowest three layers. vinS counts only the weight-bearing middle, because the nose/tail overhang is deliberately allowed to graze (see MAM_SPAN) and counting it would score the design as a bug
          gF: +gF.toFixed(2), gN: +gN.toFixed(2), gS: +gS.toFixed(2), steps: mamSeatSteps(fit), hd: fit.hd, hw: fit.hw, seat: fit.seat,
          dp: +Math.hypot(B.x - P.x, B.z - P.z).toFixed(1) }); }
      return o; },
    mushAt, MUCELL, boulderAt, BCELL,                // …and the mushroom and boulder placers, for the same reason: a floater test has to be able to stand in front of one
    logAt, LGCELL,                             // the generator's own fallen-log placement, so a test can drive straight to one instead of hunting the forest for a 14%-per-96m candidate
    logIds() { if (!LOGV) return null;              // which palette ids the ground log is built from, and whether each is one the PINE TRUNK uses
      const ids = [...new Set(LOGV.vox.map((p) => p >>> 24))];
      return ids.map((i) => ({ id: i, col: palette[i], isTrunkBark: woodIds.indexOf(i) >= 0, wood: !!woodTab[i] })); },
    barkIds() { return woodIds.map((i) => ({ id: i, col: palette[i] })); },
    // The OAK palette and what every material table says about each id. The oaks are the first asset in this
    // game whose two halves are DIFFERENT MATERIALS out of one .json, so this is the read that proves the
    // split survived the loader: bark must come back solid + wood + axe, leaf must come back folia and NOT
    // solid. `own` is the other half of the guarantee, and it is expected to be TRUE FOR THE LEAVES ONLY:
    // those four ids are minted and reserved in palOwn so no later model can tolerance-share onto them and
    // inherit "is canopy" by accident. The three bark ids come back own:false ON PURPOSE — they ARE the
    // pine's own bark ids, borrowed rather than minted (assets/bow.js), which is where their material
    // identity comes from. bark reporting own:true would mean the borrow had silently stopped happening.
    oakIds() { return OAKBARK.map((i) => [i, 'bark']).concat(OAKLEAF.map((i) => [i, 'leaf']))
      .map(([i, kind]) => ({ id: i, kind, col: palette[i], own: palOwn.has(i), solid: !!solidTab[i],
        folia: !!foliaTab[i], wood: !!woodTab[i], decor: !!decorTab[i], axe: !!axeOnlyTab[i],
        dig: !!digOnlyTab[i], pick: !!pickOnlyTab[i], sup: SUP.CLASS[i], orphanOK: !!ORPHAN_OK[i] })); },
    oaks() { return OAKV.map((m, i) => ({ i, sx: m.sx, sy: m.sy, sz: m.sz, vox: m.vox.length })); },   // the baked models, in the voxelizer's height order — oakAt picks its size tiers by index off this
    shrubIds() { return SHRUBC.concat(SHRUBF).map((i) => ({ id: i, col: palette[i], solid: !!solidTab[i], decor: !!decorTab[i], wood: !!woodTab[i], axe: !!axeOnlyTab[i], pick: !!pickOnlyTab[i], dig: !!digOnlyTab[i], cactus: !!cactusTab[i], isTrunkBark: woodIds.indexOf(i) >= 0, models: SHRUBV.length })); },   // the SHRUB pair and what every material table says about each. These are LOW ids (the first two RECLAIMED, the rest minted beside them), inside the blanket solidity sweep's range, so `solid:false` is the assertion that the sweep really was undone. The first four are the GREEN ramp and the last six the FLOWER ramp, and both quote CACTUS colours on the shrubs' own ids: diff col against __vb.modelIds('cactus') mapped through the palette to check that, and `cactus:false` is the assertion that quoting those colours did not also hand a bush a saguaro's sting. Compare against __vb.modelIds('shrub') — anything the models wear that is not listed here is a colour that escaped the loader's map
    shrubCount(x, z, r) { let n = 0, lo = 1e9, hi = -1e9; const R = r || 512;   // how many shrub CANDIDATES survive in a world-space square around (x,z) — the honest density read, independent of what has actually streamed in. It also reports the desertM range it sampled, because a count of 0 means nothing unless you know you were standing in the desert
      for (let cz = Math.floor((z - R) / SHCELL); cz <= Math.floor((z + R) / SHCELL); cz++)
        for (let cx = Math.floor((x - R) / SHCELL); cx <= Math.floor((x + R) / SHCELL); cx++) if (shrubAt(cx, cz)) n++;
      for (let dz = -R; dz <= R; dz += R >> 2) for (let dx = -R; dx <= R; dx += R >> 2) { const d = desertM(x + dx, z + dz); if (d < lo) lo = d; if (d > hi) hi = d; }
      const side = R * 2 / 10;
      return { n, sideM: side, perHa: +(n / (side * side / 10000)).toFixed(1), everyM: n ? +Math.sqrt(side * side / n).toFixed(1) : null, dmMin: +lo.toFixed(2), dmMax: +hi.toFixed(2), models: SHRUBV.length }; },
    lilyIds() { return LILYIDS.map((i) => ({ id: i, col: palette[i], decor: !!decorTab[i], solid: !!solidTab[i], folia: !!foliaTab[i], wood: !!woodTab[i], pick: !!pickOnlyTab[i], dig: !!digOnlyTab[i], sup: SUP.CLASS[i] })); },   // the LILY PAD palette ids and what every material table says about each — the pads are parsed in SHARE mode, so this is the one honest read that marking them choppable did not also mark something else
    isWoodId(id) { return !!woodTab[id | 0]; },
    isRockId(id) { return isRockSurf(id | 0); },     // is THIS id stone (strata, boulder or ore)? — lets a test find rock buried under a snow blanket, which isRockTop cannot (it stops at the snow)
    isSnowId(id) { return !!snowTab[id | 0]; },
    mamFit() { return MAMFIT; },                     // the per-model footprint extents the spawn rule and the ground servo both use
    isRockTop(x, z, yFrom) {                         // is the SURFACE at this column stone (strata, boulder or ore)? Scans down like the spawn test does — hmap is blind to stamped boulders
      const bx = gwrap(Math.floor(x), WX), bz = gwrap(Math.floor(z), WZ) * WX * WY;
      for (let y = Math.min(WY - 1, Math.round(yFrom) + 8); y >= 1; y--) {
        const v = W[bx + y * WX + bz]; if (!v || solidTab[v] !== 1) continue;
        return isRockSurf(v); }
      return false; },
    woodAtW(x, y, z) { const yy = Math.round(y); if (yy < 1 || yy >= WY) return 0;   // is there WOOD at this world point — lets a test measure how thick a trunk is along the line of sight
      const v = W[gwrap(Math.round(x), WX) + yy * WX + gwrap(Math.round(z), WZ) * WX * WY];
      return v && woodTab[v] ? v : 0; },
    aimVox(maxT) { const cp = Math.cos(P.pitch), vx = Math.sin(P.yaw) * cp, vy = Math.sin(P.pitch), vz = Math.cos(P.yaw) * cp;   // what the crosshair is actually resting on, W or off-grid body — the ground truth a chop test compares against
      let out = null;
      voxRay(P.x, smoothEye, P.z, vx, vy, vz, maxT === undefined ? 107 : maxT, (x, y, z, t) => {
        if (y < 1 || y >= WY) return 0;
        let v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY]; let inB = 0;
        if (!v) { v = phBodyIdAt(x, y, z); inB = v ? 1 : 0; }
        if (!v) return 0;
        // …and it walks through exactly what chopSwing's aim pre-pass walks through, or a test built on this tap
        // measures a DIFFERENT crosshair than the swing uses: surface scatter, open water and a snow blanket.
        if (!inB && (floatTab[v] || (isWater(v) && !solidTab[v]) || snowTab[v])) return 0;
        out = { x, y, z, id: v, body: inB, t: +t.toFixed(2), wood: !!woodTab[v], fol: !!foliaTab[v] };
        return 1;
      });
      return out; },
    phys(v) {                                          // ── VOXEL DESTRUCTION ── enable/inspect
      if (v !== undefined) { PH.on = !!v; if (!PH.on) { PH.bodies.length = 0; PH.acc = 0; bodyTop = 0; } }
      return { on: PH.on, dt: PH.dt, iters: PH.iters, bodies: PH.bodies.length, awake: PH.bodies.filter((b) => !b.sleeping).length,
        absorbSize: PH.absorbSize, tooBig: PH.bodies.filter((b) => b.n > PH.absorbSize).map((b) => b.n),   // the carry limit + every chunk currently over it
        oversize: PH.bodies.filter((b) => b.n > PH.absorbSize).map((b) => ({ vox: b.n, noAbsorb: !!b.noAbsorb, refused: !!b.tooBig })),   // every chunk over the carry limit, whether it came off a tree or not
        stats: { ...PH.stats },
        body: PH.bodies.map((b) => ({ vox: b.n, surf: b.surfN, probes: b.probes.length, mass: b.mass,
          com: b.com.map((q) => +q.toFixed(2)), pos: b.pos.map((q) => +q.toFixed(2)),
          vel: b.vel.map((q) => +q.toFixed(2)), omega: b.omega.map((q) => +q.toFixed(3)),
          quat: b.q.map((q) => +q.toFixed(3)), I: b.I.map((q) => Math.round(q)),
          gpu: b.gpu ? { off: b.gpu.off, dims: [b.gpu.bw, b.gpu.bh, b.gpu.bd] } : null,
          sleeping: b.sleeping, sleepT: b.sleepT, contacts: b.contacts | 0, deepest: +(b.deepest || 0).toFixed(2),
          ageMs: Math.round(performance.now() - b.born) })) };
    },
    physChop(x, y, z, r) { return physChopAt(x, y, z, r); },
    // ── THE AXE'S WOOD SWING, BY COORDINATE ── the same two calls sim/tools.js makes for a wood voxel now that
    // wood goes through the PICK's carve: phChopDecor confined to woodTab, then phTreeSettle on whatever tree
    // owns the column. Exists because aiming a headless crosshair at a bole is close to impossible — from any
    // distance where the ray reaches the trunk the crown is dense enough to stop it first, and teleporting
    // inside the crown gets the player ejected. Returns what the carve took and what the settle decided.
    woodChop(x, y, z, rad, bite) {
      const before = PH.stats.voxRemoved | 0;
      const ok = phChopDecor(x | 0, y | 0, z | 0, rad === undefined ? 10 : rad, bite === undefined ? 30 : bite, (v) => !!woodTab[v]);
      if (!ok) return { hit: false, took: 0 };
      const S = treeShapeAt(x | 0, z | 0);
      const st = S ? phTreeSettle(S) : null;
      return { hit: true, took: (PH.stats.voxRemoved | 0) - before, tree: !!S,
               orphans: st ? st.f.orphans : 0, detached: st ? st.bodies.length : 0, bodies: PH.bodies.length };
    },
    // the full chop signature, so a test can vary ONE parameter at a time — the material filter in particular,
    // which decides whether the flood is asked about a tree that still has all its needles or one it does not.
    physChopFull(x, y, z, rad, minBite, bite, woodOnly) {
      const S = treeShapeAt(x | 0, z | 0); if (!S) return { hit: false, why: 'no tree here' };
      return physChopAt(x | 0, y | 0, z | 0, rad, S, minBite, bite, woodOnly ? ((v) => !!woodTab[v]) : undefined);
    },
    // ── WHICH TREE OWNS THIS COLUMN, AND WHAT SHAPE DID IT GET ── the one read that separates "the axe found
    // no tree" from "it found one and the carve refused". `oak` says which arm of treeShapeAt answered, `root`
    // is the anchored/uncuttable course band (a pine's buried sink, an oak's first courses clear of ground),
    // and `vox` is the model's own voxel count, so a fell can be priced before it is attempted.
    treeShape(x, z) {
      const S = treeShapeAt(x | 0, z | 0); if (!S) return null;
      return { oak: !!S.oak, k: S.tr.k === undefined ? null : S.tr.k, rot: S.tr.rot, sink: S.tr.sink, root: S.root,
               bx: S.bx, gy: S.gy, bz: S.bz, sx: S.R.sx, sz: S.R.sz, h: S.R.h === undefined ? MSZ : S.R.h,
               vox: S.cells ? S.cells.length : null, glued: S.g ? 1 : 0, shapes: PH.stats.oakShapes | 0 };
    },
    // The root flood WITHOUT carving anything: is this tree still whole, and what does asking cost? On a
    // standing tree orphans must be 0 — anything else is the crown coming apart on its own, which is the
    // failure mode a 26-connected asset in a 6-connected flood produces. ms is the per-swing price.
    treeFlood(x, z) {
      const S = treeShapeAt(x | 0, z | 0); if (!S) return null;
      const f = phFlood(S);
      return { oak: !!S.oak, total: f.total, reached: f.reached, orphans: f.orphans, ms: PH.stats.lastFloodMs };
    },
    physSwing() { const r = chopSwing(); if (r) playToolHit(); else if (aimHitId()) playBlocked(); return r; },   // the tap drives exactly what a click does, sound included
    physChopDecor(x, y, z, r) { return phChopDecor(x, y, z, r === undefined ? 5 : r); },   // carve decor (mushrooms/ferns) at a point — test tap for the orphan/settle path
    arrowChopAt(x, y, z) { return arrowChop(x | 0, y | 0, z | 0); },   // the ARROW's carve, at a chosen voxel. Driving it through a real shot is unmeasurable: the shaft picks its own impact, and a creature in the way cancels the chop outright.
    // Stake an arrow into a chosen creature exactly as a landed shot does, so the "it goes when the animal
    // goes" sweep can be tested without having to hit a butterfly with a real shaft — which is not a test so
    // much as a marksmanship contest, and the teleport that would line one up recycles the target anyway.
    maxBodies(v) { if (v !== undefined) PH.maxBodies = Math.max(1, v | 0); return PH.maxBodies; },   // squeeze the rigid-body budget so a test can force the RARE path where phSeparate cannot seat a severed tree and hands it to the support resolver instead
    pickUp() { return { vol: +pickUpSnd.volume.toFixed(5), plays: pickUpPlays, playing: !pickUpSnd.paused,
                        t: +pickUpSnd.currentTime.toFixed(2), flight: !!grabAnim,
                        drops: drops.map((d) => ({ it: d.it, lev: dropLevitating(d), age: +((performance.now() - d.born) / 1000).toFixed(1) })) }; },   // levitating-pickup tap: which drops are still in the air, and whether the sound has fired
    dropPush(o) { drops.push(o); return drops.length; },   // push a raw drop so a test can drive the REAL arrival branch (hitSlot -> stick) instead of a stand-in
    dropRaw(i) { const d = drops[(i | 0) < 0 ? drops.length + (i | 0) : (i | 0)]; if (!d) return null;
      return { stuckSlot: d.stuckSlot, stTh: d.stTh, stOx: d.stOx, stOy: d.stOy, stOz: d.stOz,
        ex: d.ex, ey: d.ey, ez: d.ez, x: d.x, y: d.y, z: d.z, hitDone: !!d.hitDone }; },
    stickArrowIn(slot) { const s = slot | 0, B = wbf[s];
      if (!ARROW_IT || !B || !B.init) return null;
      // …including the impact bookkeeping a real landing records, or this stands in for a shot that cannot
      // ride the animal — which is precisely the behaviour the tap exists to exercise.
      const by = (B.kind | 0) === 5 ? (B.perchFeet || 0) + 3 : B.y;
      drops.push({ x: Math.round(B.x), y: Math.round(by), z: Math.round(B.z), it: ARROW_IT, ph: 0,
        born: performance.now(), T: 0, stick: true, hitSlot: s, hitDone: true,
        ex: B.x, ey: by, ez: B.z, stTh: B.th || 0, stOx: 0, stOy: 0, stOz: 0,
        stuckSlot: s, stuckBorn: B.born, q0: [0, 0, 0, 1] });
      return { stuckSlot: s, stuckBorn: B.born, drops: drops.length }; },
    cmd(line) { cmdRun(line); return { msg: cmdMsg ? cmdMsg.textContent : '', at: [Math.round(P.x), Math.round(P.y), Math.round(P.z)] }; },   // run a command line exactly as Enter does — /spawn, /locate
    // ── THE UNIFIED SUPPORT RESOLVER ── stats + the three probes it is built out of
    support() { return { on: SUP.on, queued: SUP.q.length - SUP.qh, blocked: SUP.blocked,
      cap: SUP.cap, drapeCap: SUP.drapeCap, msBudget: SUP.msBudget, stats: { ...SUP.stats }, log: SUP.log.slice(0, 24), refused: SUP.refused.slice(0, 16) }; },
    supOn(v) { if (v !== undefined) SUP.on = !!v; return SUP.on; },
    supReset() { for (const k in SUP.stats) SUP.stats[k] = 0; SUP.log.length = 0; SUP.refused.length = 0; return true; },
    supClasses() { const n = ['ignore', 'fluid', 'structure', 'drape'], o = { ignore: [], fluid: [], structure: [], drape: [] };
      for (let i = 1; i < 256; i++) o[n[SUP.CLASS[i]]].push(i);
      return { counts: { fluid: o.fluid.length, structure: o.structure.length, drape: o.drape.length }, drape: o.drape, fluid: o.fluid }; },
    whyFloating() {                                   // AIM AT THE FLOATING THING AND RUN THIS. Marches the view ray to the first solid voxel, floods its component, and reports whether the game thinks it is held up — and if not, which refusal let it stay.
      const cp = Math.cos(P.pitch), d3 = [Math.sin(P.yaw) * cp, Math.sin(P.pitch), Math.cos(P.yaw) * cp];
      for (let t = 0.6; t < 120; t += 0.25) {
        const x = Math.floor(P.x + d3[0] * t), y = Math.floor(smoothEye + d3[1] * t), z = Math.floor(P.z + d3[2] * t);
        if (y < 1 || y >= WY) break;
        const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY, v = W[ii];
        if (!v) continue;
        SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();
        const fresh = supFlood(ii, true);
        const ref = SUP.refused.filter((r) => Math.abs(r.x - x) < 40 && Math.abs(r.z - z) < 40 && Math.abs(r.y - y) < 40);
        return { hit: { x, y, z }, id: v, cls: ['ignore', 'fluid', 'structure', 'drape'][SUP.CLASS[v]],
          stamped: stampedIdx.has(ii), memoAnchored: supAnchored(ii), fresh,
          verdict: fresh.anchored ? 'HELD - the game believes this is attached, so it is not a floater'
            : (fresh.n > PH.absorbMax ? 'FLOATING - component of ' + fresh.n + ' voxels is over the ' + PH.absorbMax + ' body ceiling, so the resolver left it rather than erase it'
            : (fresh.capped ? 'FLOATING - the flood hit its cap before deciding' : 'FLOATING - detached and under the ceiling: this one is a genuine bug, not a documented refusal')),
          refusalsNearby: ref, queued: SUP.q.length - SUP.qh, blocked: SUP.blocked };
      }
      return { hit: null, verdict: 'the view ray hit nothing solid within 120 voxels' };
    },
    supProbe(x, y, z) { const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY, v = W[ii];
      return { id: v, cls: ['ignore', 'fluid', 'structure', 'drape'][SUP.CLASS[v]], conduit: stampedIdx.has(ii),
        anchored: supAnchored(ii), hmap: hmap[gwrap(x, WX) + gwrap(z, WZ) * WX], y }; },
    supTest(x, y, z) { SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();
      return supFlood(gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY, true); },   // run ONE flood at a cell and report, without acting on it (memo cleared first, so a console probe is never answered from a stale pass)
    supDirty(x, y, z) { supPush(gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY); return SUP.q.length - SUP.qh; },
    // ── TEST HOOK: WRITE VOXELS THE WAY A REAL EDIT DOES ── every floater investigation so far has had to
    // chop real scenery and hope the geometry it needed happened to be there, which is why the awkward cases
    // (a spur held sideways, an arch, a cantilever) were never actually tested. This builds an exact one.
    // Goes through gpuPatch, so it takes the identical path a chop takes: brick occupancy, the scanTop cache,
    // the navfield and — unless `track` is false — the support queue.
    setVox(list, track) {
      const cells = [];
      for (const q of list) {
        const y = q[1] | 0; if (y < 1 || y >= WY) continue;
        const ii = gwrap(q[0] | 0, WX) + y * WX + gwrap(q[2] | 0, WZ) * WX * WY;
        W[ii] = q[3] | 0; cells.push(ii);
      }
      if (cells.length) gpuPatch(cells, false, cells.length, track === undefined ? true : !!track);
      return cells.length;
    },
    supFlushNow() { return supFlush(true); },          // run the resolver to completion, ignoring the time slice — tests must not have to guess how many frames a cascade takes
    eat() { return tryEat(); },
    // ── THE FRUIT, END TO END IN ONE CALL ── every number the pick -> hand -> eat -> animate chain depends on,
    // so a verification is one line rather than five. `palette` is here because the whole feature's claim is
    // that it costs ZERO ids: the eat strips are raw RGB in ITEMMAP and never ask the table for anything, so
    // this length must read the same as it did before the fruit items existed.
    fruit() {
      const stemBrown = !!FRUIT_STEM_ID;
      return { appleIt: APPLE_IT, orangeIt: ORANGE_IT, frames: FOOD_EAT_N, fps: EAT_FPS, stripMs: +(FOOD_EAT_N * EAT_FRAME_MS).toFixed(1),
        eating: eatAnim ? { it: eatAnim.it, name: ITEM_NAMES[eatAnim.it] || null, frame: Math.floor((performance.now() - eatAnim.t0) / EAT_FRAME_MS) } : null,
        shown: shownIt, shownName: ITEM_NAMES[shownIt] || null,
        heals: { apple: (vitFoods()[APPLE_IT] || {}).hp || 0, orange: (vitFoods()[ORANGE_IT] || {}).hp || 0, meat: (vitFoods()[MEAT_IT] || {}).hp || 0 },
        hp: VIT.hp, hpMax: VIT_HP_MAX,
        fleshIds: { apple: FRUITC[0], orange: FRUITC[2] }, floodMin: FRUIT_MIN, floodCap: FRUIT_CAP,
        stem: { id: FRUIT_STEM_ID, brown: stemBrown, col: stemBrown ? palette[FRUIT_STEM_ID] : null, note: stemBrown ? 'own id' : 'no brown minted — the world stalk still wears the oak leaf id (the HELD apple is raw art and is brown either way)' },
        palette: palette.length };
    },
    // WHAT THE PICK RAY WOULD DO at one cell, without doing it: the exact verdict tryPickup and pickAim share.
    // Null means "not one pickable fruit" — not fruit at all, a 1-2 voxel cherry under FRUIT_MIN, or a pair
    // fused past FRUIT_CAP. n is the flesh voxel count, which is 19 for an untouched apple or orange.
    fruitAt(x, y, z) { const f = fruitAt(x | 0, y | 0, z | 0); return f ? { it: f.it, name: ITEM_NAMES[f.it] || null, n: f.cells.length } : null; },
    // ── THE DESERT BED (2026-08-15) ── the palindrome loop has no element and fires no events, so nothing in
    // snd() above can see it and there is no `ended` a test could count. This is the whole state: how far the
    // load got, which PASS of the palindrome is sounding and which way that pass runs, the live gain against
    // the forest bed it hands over with, and what the rebuilt buffer cost.
    desAmb(probe) { return desAmbState(probe); },   // desAmb(64) also SAMPLES the rebuilt buffer for the reflection that makes it a palindrome
    cactProbe(x, y, z) { return cactusHurtAt(x, y, z); },
    cactNear() {                                          // how far IS the nearest cactus voxel from where the player can actually stand, at body height?
      // `gap` is the number the hurt test actually asks about: the clearance between the player's COLLISION
      // BOX and the voxel's own cube, not centre-to-centre. Centre-to-centre reads ~HW (2.6) too large and was
      // what made three earlier reaches look plausible; a gap of ~0 is a body pressed against the plant, and
      // anything <= CACT_MARGIN stings. `chebyshev` is kept beside it purely so old measurements still compare.
      const cid = new Set(__vb.modelIds('cactus')); let best = 1e9, bestC = 1e9, at = null;
      const y0 = Math.floor(P.y), y1 = Math.floor(P.y + HEIGHT - 0.1);
      for (let dx = -8; dx <= 8; dx++) for (let dz = -8; dz <= 8; dz++) for (let yy = y0; yy <= y1; yy++) {
        const x = Math.floor(P.x) + dx, z = Math.floor(P.z) + dz;
        if (!cid.has(W[gwrap(x, WX) + yy * WX + gwrap(z, WZ) * WX * WY])) continue;
        const g = Math.max(Math.max(x - (P.x + HW), (P.x - HW) - (x + 1), 0), Math.max(z - (P.z + HW), (P.z - HW) - (z + 1), 0));
        if (g < best) { best = g; bestC = Math.max(Math.abs(P.x - (x + 0.5)), Math.abs(P.z - (z + 0.5))); at = [x, yy, z]; }
      }
      return { gap: best > 1e8 ? -1 : +best.toFixed(3), chebyshev: bestC > 1e8 ? -1 : +bestC.toFixed(2), margin: CACT_MARGIN, at, playerY: +P.y.toFixed(2) };
    },   // test the CONTACT TEST itself, without moving the player (tp gets resolved upward)
    cactDbg() { let n = 0; for (let i = 0; i < 256; i++) if (cactusTab[i]) n++;
      return { tabbed: n, hitHere: cactusHurtAt(P.x, P.y, P.z, P.crouch ? CR_HEIGHT : HEIGHT), pos: [+P.x.toFixed(1), +P.y.toFixed(1), +P.z.toFixed(1)], idHere: W[gwrap(Math.floor(P.x), WX) + (P.y | 0) * WX + gwrap(Math.floor(P.z), WZ) * WX * WY] }; },
    geckoDbg() { const o = []; for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j];
      if (!B || !B.init) continue; const sp = ((j - MAM_END) / DES_PER) | 0;
      if (!DESERTS[sp] || DESERTS[sp].name !== 'gecko') continue;
      // ── THE NAV STATE, NOT JUST THE DISTANCE (2026-08-17) ── the cactus stall is only legible per creature:
      // navstat's row 2 pools the worms in with the desert band, and wormPos covers slots 32-64, so neither can
      // say whether THIS gecko is braked against something. navClear is the reach the brake actually measured
      // along B.th (≤ NAV_WBCLR = the step is clamped to exactly zero), navTh/navReach are what the fan chose,
      // and trap/noMove are the counters that used to read 0 through the whole stall and now do not.
      o.push({ j, d: +Math.hypot(P.x - B.x, P.z - B.z).toFixed(1), chase: +(B.chase || 0).toFixed(1),
        navTh: +(B.navTh || 0).toFixed(2), navReach: +(B.navReach || 0).toFixed(2), navClear: +(B.navClear || 0).toFixed(2),
        trap: +(B.trap || 0).toFixed(2), noMove: +(B.noMove || 0).toFixed(2) }); } return o; },
    meatFor(j) { return dropsMeat(j | 0); },              // does a kill in this slot leave meat? the drop path's own predicate
    meatSpecies() { const o = {}; for (let i = 0; i < DESERTS.length; i++) o[DESERTS[i].name] = dropsMeat(MAM_END + i * DES_PER); return o; },
    // ── THE OAK-FOREST HALF OF THE DESERT BAND ── one tap for the three questions the two 2026-08-17 species
    // raise, because they are only meaningful together: is it ALIVE, is it in the RIGHT BIOME, and is it
    // CLASSIFIED right. `home` is bioHomeOK's own verdict at the body's position — the predicate the game
    // steers and recycles on, not a re-derivation of it — so a false here is a real trespass and not a
    // difference of opinion between the test and the game. `om` should read well over BIO_OAKLINE (0.5) for
    // every row, and over 0.85 for a body that has not moved far from where it was admitted.
    oakLife() { const o = []; const nmO = (i) => (DESERTS[i] || {}).name || '?';
      for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j]; const sp = ((j - MAM_END) / DES_PER) | 0;
        if (!DES_OAKONLY[nmO(sp)] || !B || !B.init || typeof B.x !== 'number') continue;
        o.push({ j, sp: nmO(sp), idx: (j - MAM_END) % DES_PER, kind: B.kind | 0,
          x: Math.round(B.x), y: Math.round(B.y || 0), z: Math.round(B.z),
          om: +oakM(B.x, B.z).toFixed(2), dm: +desertM(B.x, B.z).toFixed(2),
          home: bioHomeOK(BIO_OAKF, B.x, B.z), d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)),
          meat: dropsMeat(j), leash: B.hcx !== undefined }); }
      return o; },
    // ── AND WHAT EACH BEE IS DOING ── the errand state machine, read out. `m` is B.beeM (0 wander / 1 to a
    // flower / 2 sitting / 3 to a hive / 4 orbiting), `td` is how far it still has to go and `left` how long
    // it has before the state ends — an arrive-by deadline in modes 1 and 3, a leave-at clock in 2 and 4.
    // A bee stuck in mode 1 with td barely falling is the failure this was built to make visible, and it is
    // self-limiting: `left` runs out at BEE_GIVE_S and that flower is banned for BEE_BAN_S. `hiveWired` stays
    // false until beeHiveNear is wired (main/tick-nav.js, BEE_HIVE_Q), which is the one line that turns
    // modes 3 and 4 on — so a report of 'no bees ever swarm' is answered by this field alone.
    // ── AND MODE 5 IS THE ANGRY ONE (2026-08-17) ── `swarm` says whether this slot is even eligible for it
    // (desIx < BEE_HIVE_N, the same slot-number split that decides who lives at a hive), `rgd` is how far the
    // PLAYER is from the wreck this bee is defending — the number the give-up test actually reads, so a
    // 'they gave up instantly' report is answered by comparing it against `leash` — and `stungBy` is how many
    // times this individual has landed one. `angry` is the whole swarm, and it can never exceed `perHive`.
    // `stingIn` counts down the ONE swarm-wide cooldown: five bees on you and still 2 s to the next point of
    // damage is the intended reading, not a stuck clock.
    beeDbg() { const o = []; const tbB = performance.now() / 1000; let nAngry = 0;
      for (let j = MAM_END; j < DES_END; j++) { const B = wbf[j];
        const sp = ((j - MAM_END) / DES_PER) | 0; if (((DESERTS[sp] || {}).name) !== 'bee') continue;
        if (!B || !B.init) continue;
        if ((B.beeM | 0) === 5) nAngry++;
        o.push({ j, idx: (j - MAM_END) % DES_PER, m: B.beeM | 0, x: Math.round(B.x), y: Math.round(B.y || 0), z: Math.round(B.z),
          tx: B.beeTx === undefined ? null : Math.round(B.beeTx), tz: B.beeTz === undefined ? null : Math.round(B.beeTz),
          td: B.beeTx === undefined ? null : +Math.hypot(B.beeTx - B.x, B.beeTz - B.z).toFixed(1),
          left: B.beeT === undefined ? null : +(B.beeT - tbB).toFixed(1), banned: tbB < (B.beeBanT || 0),
          swarm: ((j - MAM_END) % DES_PER) < BEE_HIVE_N,
          rgd: B.beeRgX === undefined ? null : Math.round(Math.hypot(P.x - B.beeRgX, P.z - B.beeRgZ)),
          pd: Math.round(Math.hypot(B.x - P.x, B.z - P.z)), dy: Math.round((B.y || 0) - P.y), stungBy: B.beeStings | 0,
          om: +oakM(B.x, B.z).toFixed(2) }); }
      return { bees: o, hiveWired: !!beeHiveNear(P.x, P.z), sit: [BEE_SIT_S, BEE_SIT_S + BEE_SIT_J], giveUp: BEE_GIVE_S, perHive: BEE_HIVE_N, orbit: BEE_ORBIT_R,
        angry: nAngry, rage: BEE_RAGE_S, leash: BEE_RAGE_LEASH, hear: BEE_RAGE_R, hearBreak: BEE_BREAK_R, sting: BEE_STING, stingCd: BEE_STING_CD,   // `hear` is a SWAT's reach, `hearBreak` a hive's — 'only five came' now splits into 'the others were outside hearBreak' and 'they were inside it and did not answer'
        stingIn: +Math.max(0, beeStingT - tbB).toFixed(2),
        breaks: HIVE_BREAK.map((b) => ({ x: Math.round(b.x), y: Math.round(b.y), z: Math.round(b.z), age: +(tbB - b.t).toFixed(1),
          live: tbB - b.t <= BEE_RAGE_WIN, called: b.n, d: Math.round(Math.hypot(b.x - P.x, b.z - P.z)) })) }; },
    // ── THE HIVE ITSELF ── how much of the nearest one is left, and what it would take to open it. This is the
    // half of the feature the bee report cannot see: 'nothing happened when I chopped it' splits cleanly into
    // 'the hive is not open yet' (left is still over need), 'it is open and was already claimed' (broken true),
    // and 'it opened and nobody came' (broken true, beeDbg().angry 0) — three different bugs with three
    // different fixes, and no way to tell them apart from the swarm's side alone.
    hiveDbg(r) { const h = hiveNearest(P.x, P.z, r === undefined ? 400 : r);
      if (!h) return { hive: null, searched: r === undefined ? 400 : r };
      const lf = hiveLeft(h), fu = hiveFull();
      return { hive: { x: h.wx, y: h.wy, z: h.wz, tree: [h.tx, h.tz], box: [h.sx, h.sy, h.sz], rot: h.rot },
        d: Math.round(Math.hypot(h.wx - P.x, h.wz - P.z)), full: fu, left: lf,
        need: Math.floor(fu * BEE_BREAK_F), frac: +(lf / Math.max(1, fu)).toFixed(2),
        open: lf <= fu * BEE_BREAK_F, broken: HIVE_DONE.has(h.tx + '|' + h.tz) }; },
    // ── AND THE TRIGGER, WITHOUT AN AXE ── posts a break for the nearest hive and returns what happened.
    // Same reason __vb.birdKill exists: landing four accurate swings on a 50 cm box hanging in a crown 100
    // voxels up is not something a test can do reliably, and the interesting half of this feature is
    // everything AFTER the break. It is the real hiveBroke, so the ledger, the recruiting, the leash and the
    // sting are all the shipped path — only the axe is simulated away. Pass true to also VERIFY the threshold
    // rather than bypass it, which is how you check the counting half on a hive you actually chopped.
    hiveBreak(strict) { const h = hiveNearest(P.x, P.z, 400);
      if (!h) return { ok: false, why: 'no hive within 400' };
      const lf = hiveLeft(h), fu = hiveFull();
      if (strict && lf > fu * BEE_BREAK_F) return { ok: false, why: 'still ' + lf + ' of ' + fu + ' standing', left: lf, need: Math.floor(fu * BEE_BREAK_F) };
      const first = hiveBroke(h);
      return { ok: !!first, why: first ? 'broken' : 'already broken', left: lf, full: fu, d: Math.round(Math.hypot(h.wx - P.x, h.wz - P.z)), breaks: HIVE_BREAK.length }; },
    // ── AND A SWAT, WITHOUT A SWING ── posts the real beeAngered at a given bee's position. Same reason
    // hiveBreak exists beside it: landing a blow on a 3-voxel insect in flight is not something a test can do
    // reliably, and the interesting half is everything AFTER the hit. Everything downstream is the shipped
    // path — the ledger record, the recruiting, the leash and the sting — only the swing is simulated away.
    beeSwat(j) { const B = wbf[j | 0];
      if (!B || !B.init) return { ok: false, why: 'slot ' + (j | 0) + ' is not a live creature' };
      const sp = ((( j | 0) - MAM_END) / DES_PER) | 0;
      if (((DESERTS[sp] || {}).name) !== 'bee') return { ok: false, why: 'slot ' + (j | 0) + ' is not a bee' };
      const fresh = beeAngered(B.x, B.y || 0, B.z);
      return { ok: true, fresh: !!fresh, at: [Math.round(B.x), Math.round(B.z)], pd: Math.round(Math.hypot(B.x - P.x, B.z - P.z)),
        inEarshot: __vb.beeDbg().bees.filter((b) => Math.hypot(b.x - B.x, b.z - B.z) <= BEE_RAGE_R).map((b) => ({ idx: b.idx, swarm: b.swarm })) }; },
    // Is there a flower where I am standing, and at what y? The bee's own finder, asked directly — so a
    // 'the bees never visit anything' report splits into 'there are no blooms here' and 'the bee cannot get
    // to them' without guessing which.
    bloomAt(x, z) { return beeBloomAt(x === undefined ? P.x : x, z === undefined ? P.z : z); },
    bloomNear() { const f = findBeeFlower(P.x, P.z); return f ? { x: f.x, y: f.y, z: f.z, d: Math.round(Math.hypot(f.x - P.x, f.z - P.z)) } : null; },
    // ── HUNGER IS GONE (2026-08-17) ── hp IS the five-point bar the screen pixelation paints, and `red`
    // is the 0..4 it paints from. food/sat/exh/timer and the five-hit counter were deleted with the
    // mechanic; reading them here threw a ReferenceError that no static check catches, because it is
    // runtime-only and inside a function.
    vit() { return { hp: VIT.hp, hpMax: VIT_HP_MAX, red: vitRedLevel(), hurtT: +VIT.hurtT.toFixed(3),
      food: VIT.food, gold: vitGoldLevel(), exh: +VIT.exh.toFixed(3), starveT: +VIT.starveT.toFixed(2),   // ── THE HUNGER BAR ── the four numbers a test needs: the points, the 0..4 BLIT paints gold from, the accumulator that spends them, and how far into a starvation tick we are
      sprintOK: vitSprintOK(), foods: Object.keys(vitFoods()).length,
      foodTable: Object.entries(vitFoods()).map(([id, f]) => ({ id: +id, name: ITEM_NAMES[id] || null, hp: f.hp, strip: !!f.strip })) }; },   // …the table itself, because "is this edible AND does it chew" is now one declaration (sim/vitals.js) and a test should be able to read it rather than infer it
    vitSet(hp, food) { if (hp !== undefined) VIT.hp = Math.max(0, Math.min(VIT_HP_MAX, Math.round(hp)));
      if (food !== undefined) { VIT.food = Math.max(0, Math.min(VIT_FOOD_MAX, Math.round(food))); VIT.exh = 0; VIT.starveT = 0; }   // the second argument is BACK (user 2026-08-19, hunger re-introduced) — and it clears the accumulator with it, or a test that sets a full bar watches banked exhaustion spend it again on the next tick
      return __vb.vit(); },
    hearts(v) { if (v !== undefined) heartShow = v ? 1 : 0; return { shown: !!heartShow, item: HEART_IT, n: HEART_N, hp: VIT.hp, hearts: +(VIT.hp / (VIT_HP_MAX / HEART_N)).toFixed(3), pose: { ...HEART_POSE } }; },   // the floating health voxels: read the state, or pass false to hide them (the A/B lever for the composite block's cost)
    vitHurt(n, why) { vitHurt(n, why || 'a test'); return __vb.vit(); },
    rec(on) { if (on !== undefined && !!on !== !!VE.recording) veToggleRec();   // drive and inspect the SCREEN RECORDER — the capture rate is derived from the canvas size at start, so a test has no other way to see what it chose
      return { recording: !!VE.recording, canvas: [canvas.width, canvas.height], mpix: +((canvas.width * canvas.height) / 1e6).toFixed(2),
        refreshHz: VE.refreshHz || 0, capHz: +(VE.capHz || 0).toFixed(1), capEvery: VE.capEvery | 0, paintN: VE.paintN | 0,
        pushed: Math.floor((VE.paintN | 0) / Math.max(1, VE.capEvery | 0)), manualPush: !!VE.recTrack, clips: VE.clips.length }; },
    weatherAt(x, z) { const m = oakWeather(x, z);   // the WEATHER border as the snow actually sees it: the raw mask, and the wSharp window the blanket and the flakes are both dithered through. A fade that reads as a line on screen is a `sharp` that goes 0 -> 1 over too few voxels, and this is the only way to measure that without eyeballing a screenshot
      return { mask: +m.toFixed(4), sharp: +wSharp(m).toFixed(4), cherry: +cherryM(x, z).toFixed(4) }; },
    lifeGait(sl, spookMs) { const B = wbf[sl | 0]; if (!B || !B.init) return null;
      if (spookMs) B.spookT = performance.now() + spookMs;   // …and it can ARM the spook, which is what a landed hit does (sim/life/reactions.js) — the only way a test can see the flee gait without an arrow in the air   // the WALK state of a land mammal: is it fleeing, how fast are its legs, how fast is it travelling. The three the flee rule sets together
      return { slot: sl | 0, flee: !!B.aflee, spooked: performance.now() < (B.spookT || 0),
        fps: +(B.afps || 0).toFixed(1), speed: +(B.aspd || 0).toFixed(1), hits: B.hits | 0 }; },
    flamingos() { const o = []; for (let j = FLAM_0; j < FLAM_END; j++) if (wbf[j] && wbf[j].init) o.push(j); return o; },   // live flamingo slots
    grabTo(it) { const h = slots[selSlot] && slots[selSlot].it;   // WHICH HAND would a pickup of `it` fly to, given what is held right now — the predicate startGrab uses, without needing a rock on the ground to test it
      const left = (h === 2 && (it === 2 || it === 3)) || (h === 3 && it === 2);
      return { held: h || 0, picking: it | 0, hand: left ? 'left' : 'right' }; },
    grabNow() { return grabAnim ? { it: grabAnim.it, left: !!grabAnim.left, age: +(performance.now() - grabAnim.t0).toFixed(0) } : null; },   // the flight in progress, if any
    craft(act) {                                     // drive and inspect the STONE AGE bench: no arg reports, 'open'/'next'/'prev'/'ok'/'close' act
      if (act === 'open') craftOpen(); else if (act === 'next') craftCycle(1); else if (act === 'prev') craftCycle(-1);
      else if (act === 'ok') craftConfirm(); else if (act === 'close') craftClose();
      const p = craftPair();
      return { open: !!CRAFT.open, lit: !!CRAFT.lit, k: +craftK(performance.now()).toFixed(3), t0age: CRAFT.t0 ? +(performance.now() - CRAFT.t0).toFixed(0) : -1, fly: CRAFT.fly ? +(performance.now() - CRAFT.fly).toFixed(0) : 0, idx: CRAFT.idx | 0, menu: craftMenu().map((i) => ({ id: i, name: ITEM_NAMES[i] || null })),
        showing: craftItem(), showingName: ITEM_NAMES[craftItem()] || null,
        cost: craftCostFor(craftItem()), have: craftHave(), afford: craftAfford(craftItem()),
        menuCost: craftMenu().map((i) => ({ name: ITEM_NAMES[i] || i, ...craftCostFor(i) })),
        pair: p ? { rock: p.rock, stick: p.stick } : null, hand: __vb.hand(), stoneAge: !!vbAch.stoneAge,
        pick3Id: UF[UF_PICK3 + 4 + 3] };
    },
    vitDrain(n) { for (let i = 0; i < (n || 1); i++) vitExhaust(EXH_STEP); return __vb.vit(); },   // spend n HUNGER points through the real drain — the only way a test can see the gold burst, since walking is free and sprinting a whole point takes 160 m
    vitGive(it) { const k = addItem(it || MEAT_IT); for (let i = 0; i < slots.length; i++) if (slots[i] && slots[i].it === (it || MEAT_IT)) { selSlot = i; break; } return { added: k, hand: __vb.hand() }; },                      // one bite, exactly what a right-click does
    knifeId() { return KNIFE_IT; }, rockId() { return ROCK_IT; }, pickId() { return PICK_IT; }, shovelId() { return SHOVEL_IT; }, bowId() { return BOW_IT; }, meatId() { return MEAT_IT; }, hoeId() { return HOE_IT; }, spearId() { return SPEAR_IT; },
    escMenu(v) { locked = !v; lockEl.classList.toggle('hidden', locked); cursSync(); return { locked }; },   // drive the esc menu headlessly: under ?cdp there is no pointer lock, so pointerlockchange never fires and this path is otherwise untestable
    dropMeatAt(x, y, z) { if (!MEAT_IT) return null; dropMeat({ x, y, z }); return __vb.dropsInfo(); },   // drop meat on demand, for tests and for looking at it
    pickSets() { const S = (s) => [...s].sort((a, b) => a - b); const inter = (a, b) => S(a).filter((i) => b.has(i));
      return { rock: S(PICK_ROCK), stick: S(PICK_STICK), cone: S(PICK_CONE), boulder: S(PICK_BOULDER),
        collide: { coneStick: inter(PICK_CONE, PICK_STICK), coneRock: inter(PICK_CONE, PICK_ROCK), stickRock: inter(PICK_STICK, PICK_ROCK) } }; },   // WHICH WORLD ids each right-click pickup claims. The models' own .vox ids are disjoint, but the world palette is FULL and remaps by nearest colour, so two brown decorations can land on one id — and tryPickup tests these sets in a fixed order, so the first one wins.
    toolTabs() { const d = [], a = [], k = [], g = []; for (let i = 0; i < 256; i++) { if (decorTab[i]) d.push(i); if (axeOnlyTab[i]) a.push(i); if (pickOnlyTab[i]) k.push(i); if (digOnlyTab[i]) g.push(i); } return { choppable: d.length, needsCut: a.length, needsPick: k.length, needsShovel: g.length, pickIds: k, digIds: g }; },
    dropsInfo() { return drops.map((d) => ({ it: d.it, x: Math.round(d.x), y: Math.round(d.y), z: Math.round(d.z), flightS: +(d.T || 0).toFixed(2),
      stick: !!d.stick, hitSlot: d.hitSlot === undefined ? -1 : d.hitSlot, hitBird: d.hitBird === undefined ? -1 : d.hitBird, hitDone: !!d.hitDone,
      end: d.ex === undefined ? null : [+d.ex.toFixed(1), +d.ey.toFixed(1), +d.ez.toFixed(1)] })); },   // items in flight / lying on the ground — a throw lands one of these far from the player, a lob lands it near
    mouseR(v) { const was = mouse2; mouse2 = !!v;      // …and drives the BOW DRAW exactly as a real right-click does
      if (mouse2 && !was) { bowT0 = performance.now(); if (BOW_IT && heldIt() === BOW_IT) playBowStretch(); eatHold = true; }   // …and ARMS THE EATING HOLD, so a test can hold the button down and watch a stack go bite by bite. The real handler arms it from the pickup outcome; there is no pickup on this path, so a press here is always the eat-or-nothing case.
      else if (!mouse2 && was) { bowRel = performance.now();
        if (BOW_IT && heldIt() === BOW_IT) stopBowStretch();
        if (BOW_IT && heldIt() === BOW_IT && (bowRel - bowT0) > BOW_DRAW_MS * 0.5) { bowLoosed = true; if (shootArrow()) playSwish(); }   // …and the whoosh only when a shaft actually leaves — shootArrow now spends an arrow and refuses on an empty quiver
        else if (SPEAR_IT && heldIt() === SPEAR_IT && (bowRel - bowT0) > 90) throwSpear(); eatHold = false; }   // …and the SPEAR throw, so a test drives exactly what the mouse does   // …including LOOSING the arrow
      return mouse2; },
    heldShown() { return shownIt; },                  // the item id actually DRAWN in the hand — a bow reports its current draw frame
    itemEdges(id) { const it = itemsRef && itemsRef[(id | 0) - 1]; if (!it || !it.cells) return null;   // [near, far] filled slices along the item's DEPTH — the bow's string and its face
      let lo = 1e9, hi = -1e9;
      for (let y = 0; y < it.d; y++) for (let z = 0; z < it.h; z++) for (let x = 0; x < it.w; x++)
        if (it.cells[x + y * it.w + z * it.w * it.d]) { if (y < lo) lo = y; if (y > hi) hi = y; }
      return lo > hi ? null : [lo, hi]; },
    poseOf(id) { const c = pickCfgs[(id | 0)]; return c ? { ...c } : null; },   // an item's live held pose, for checking what the adjust box did
    killTest(slot) { hitCreature(slot | 0); return __vb.hitsOn(slot | 0); },
    spookOn(slot) { const B = wbf[slot | 0]; return B ? { spooked: spooked(B), msLeft: Math.max(0, Math.round((B.spookT || 0) - performance.now())), bflee: !!B.bflee, aflee: !!B.aflee, bfps: +(B.bfps || 0).toFixed(1), afps: +(B.afps || 0).toFixed(1) } : null; },   // is this creature in its double-speed panic, and how much of the window is left   // land a hit on a KNOWN creature with whatever is in hand — no aiming in the way
    hitAt(slot, token) { hitCreature(slot | 0, token); return __vb.hitsOn(slot | 0); },   // …and with an explicit swing token, so a test can land SUCCESSIVE blows (one swing = one hit is enforced on the token)
    aimKill() { const b = aimedCreature(); if (b >= 0) hitCreature(b); return b; },   // exactly what a left click does, without having to drive the mouse
    birdKills() { return birdKills; },
    birdKill(i) { const B = birds[i | 0]; const ok = birdShot(B); return { hit: ok, rag: !!(B && B.rag), parts: (B && B.ragParts) ? B.ragParts.length : 0, bodies: PH.bodies.length }; },   // test tap: kill a FLYING bird outright (arrowing one at 140 voxels is not repeatable)
    birdCount() { let n = 0; for (const B of birds) if (B && B.init) n++; return n; },   // songbirds actually in the air
    birdAt(i) { const B = birds[i | 0]; return (B && B.init) ? { x: B.x, y: B.y, z: B.z } : null; },
    tillInfo() { const q = tilled[0]; return { n: tilled.length, id: TILL_ID, col: TILL_ID ? palette[TILL_ID - 1] : null,
      sample: q ? { top: W[q.ii], below: W[q.jj], wasTop: q.prevTop, wasBelow: q.prevBelow, h: hmap[q.hi] } : null }; },
    tillAge(ms) { for (const q of tilled) q.t -= (ms | 0); return tilled.length; },   // fast-forward the regrow clock
    tillCount() { return tilled.length; },             // soil turned over and still waiting to grow back
    palAt(id) { const c = palette[(id | 0) - 1]; return c ? [c[0], c[1], c[2]] : null; },
    isFoliage(id) { return !!foliaTab[id | 0]; },       // is this voxel pine needles? (the arrow's new hitbox)
    sparkCount() { let n = 0; for (const q of sparks3d) if (q && !q.smoke && performance.now() - q.born < q.life * 1000) n++; return n; },   // live embers right now
    itemName(id) { return ITEM_NAMES[(id | 0)] || null; },   // what a hotbar slot is actually holding, by name
    itemVox(id) { const it = itemsRef && itemsRef[(id | 0) - 1]; if (!it) return -1;   // how many cells are actually filled — a slide that pushed voxels off the grid would show up here
      let n = 0; for (const c of it.cells) if (c) n++; return n; },
    itemDims(id) { const it = itemsRef && itemsRef[(id | 0) - 1]; return it ? [it.w, it.d, it.h] : null; },   // an item's grid — the numbers the shader was compiled with, so a test can prove a turn left them alone
    arrowRot(r, p) { if (r) ARROW_ROT = r.map((n) => (n | 0) & 3);                    // the arrow's own orientation + PER-FRAME offsets on the bow — the top-right panel, from a test
      if (p) ARROW_POS = (typeof p[0] === 'number' ? ARROW_POS_DEF().map(() => arrowPosClamp(p)) : ARROW_POS_DEF().map((q, i) => arrowPosClamp(p[i] || [0, 0, 0])));
      if (r || p) { if (bowRefit) bowRefit(ARROW_ROT, ARROW_POS); resetHist = 1; arwSync(); }
      return { rot: ARROW_ROT.slice(), deg: ARROW_ROT.map((q) => q * 90), pos: ARROW_POS.map((q) => q.slice()), frame: bowLock }; },
    bowLock(n) { if (n !== undefined) { bowLock = (n === null || n < 0) ? -1 : Math.min(BOW_FRAMES - 1, n | 0); arwSync(); } return bowLock; },   // pin the held bow to a draw frame, as , and . do
    bowInfo() { return { base: BOW_IT, frames: BOW_FRAMES, frame: bowFrame(performance.now()), drawing: !!mouse2 }; },        // hold/release the RIGHT button from a test (the throw's wind-up); CDP mouse events do not reach the game's own handlers under pointer lock
    swingInfo() {                                    // run this WHILE holding the tool — says whether the game sees it as a cutter
      const it = heldIt();
      return { heldItem: it, axeId: 1, knifeId: KNIFE_IT,
               isAxe: it === 1, isKnife: KNIFE_IT > 0 && it === KNIFE_IT,
               cutsWood: it === 1 || (KNIFE_IT > 0 && it === KNIFE_IT),
               isPick: PICK_IT > 0 && it === PICK_IT, isShovel: SHOVEL_IT > 0 && it === SHOVEL_IT,
               sphere: 2 * CHOP_RAD * (KNIFE_IT > 0 && it === KNIFE_IT ? KNIFE_SCALE : (PICK_IT > 0 && it === PICK_IT ? PICK_SCALE : (SHOVEL_IT > 0 && it === SHOVEL_IT ? DIG_SCALE : AXE_SCALE))),   // this tool's OWN sphere. Off its own material it works at half of this, and WOOD is always the axe's (user)
               chunk: PH.chopBite, lastChops: PH.stats.chops, lastVoxRemoved: PH.stats.voxRemoved };
    },                     // stone-knife item id — it cuts like the axe at half the sphere
    lifeDump() {                                     // live creatures + how many voxels each model carries — sizing data for the dynamic shadow volume
      const o = [];
      for (let j = 0; j < DES_END; j++) { const B = wbf[j]; if (!B || !B.init) continue;
        o.push({ j, kind: B.kind | 0, x: +B.x.toFixed(1), y: +((B.kind|0)===5 ? (B.perchFeet||0) : B.y).toFixed(1), z: +B.z.toFixed(1),
                 stamped: B.sN | 0 }); }
      return o;
    },
    decorIds() { const o = []; for (let i = 0; i < 256; i++) if (decorTab[i]) o.push(i); return o; },   // palette ids the axe can carve chunks from
    axeOnlyIds() { const o = []; for (let i = 0; i < 256; i++) if (axeOnlyTab[i]) o.push(i); return o; },   // …of those, the ones that need the AXE (wood)
    ducks() { const o = []; for (let j = DUCK_0; j < BABY_END; j++) { const B = wbf[j]; if (B && B.init && (B.kind | 0) === 3) o.push({ j, x: +B.x.toFixed(2), z: +B.z.toFixed(2), th: +(B.th || 0).toFixed(3), om: +(B.om || 0).toFixed(3), turnAcc: +((B.turnAcc || 0)).toFixed(2) }); } return o; },   // duck slots — circling shows up as turnAcc growing without bound
    // ── FLOATER PROBE ── call this WHILE a suspect voxel is on screen.
    // Floods EVERY solid voxel in a box around you, upward from the terrain at its base, and reports
    // what it cannot reach. That is the only honest definition of "floating": no path to the ground
    // through anything solid. (The first version seeded on foliage resting on solid and flooded only
    // through foliage — but canopy attaches to its trunk sideways, through WOOD, so healthy trees
    // failed it and it reported ~1200 false positives.)
    floatBase(r) { return this.floaters(r).then(() => { window.__vbFloatBase = window.__vbFloatLast; return 'baseline set (' + window.__vbFloatBase.size + ' pre-existing) — now go chop, then run __vb.floaters()'; }); },
    floaters(r) {
      const R = Math.min(64, r === undefined ? 48 : r);
      const x0 = Math.round(P.x) - R, z0 = Math.round(P.z) - R;
      const y0 = Math.max(1, Math.round(P.y) - 12), y1 = Math.min(WY - 1, Math.round(P.y) + 96);
      const bw = 2 * R + 1, bh = y1 - y0 + 1, nAll = bw * bw * bh;
      const occ = new Uint8Array(nAll), stk = new Int32Array(nAll);
      const li = (ix, iy, iz) => ix + iz * bw + iy * bw * bw;
      const wIdx = (ix, iy, iz) => gwrap(x0 + ix, WX) + (y0 + iy) * WX + gwrap(z0 + iz, WZ) * WX * WY;
      for (let iy = 0; iy < bh; iy++) for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++)
        if (W[wIdx(ix, iy, iz)]) occ[li(ix, iy, iz)] = 1;
      let sp = 0;
      for (let iy = 0; iy < 3; iy++) for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++) {
        const k = li(ix, iy, iz); if (occ[k] === 1) { occ[k] = 2; stk[sp++] = k; } }
      while (sp > 0) {
        const k = stk[--sp], ix = k % bw, iz = ((k / bw) | 0) % bw, iy = (k / (bw * bw)) | 0;
        for (let d = 0; d < 6; d++) {
          const nx = ix + (d === 0 ? 1 : d === 1 ? -1 : 0), ny = iy + (d === 2 ? 1 : d === 3 ? -1 : 0), nz = iz + (d === 4 ? 1 : d === 5 ? -1 : 0);
          if (nx < 0 || nx >= bw || nz < 0 || nz >= bw || ny < 0 || ny >= bh) continue;
          const nk = li(nx, ny, nz); if (occ[nk] === 1) { occ[nk] = 2; stk[sp++] = nk; } }
      }
      // A voxel with no 6-connected path down is NOT automatically a bug: the pine model attaches many
      // needles only DIAGONALLY, so healthy canopy fails that test while sitting visually on its tree.
      // What the eye calls "floating" is ISOLATION — a cube alone in open air. So only count detached
      // voxels with nothing still-attached within `ISO` cells; everything else is authored geometry.
      const ISO = 2;
      const attached = (ix, iy, iz) => {
        for (let dy = -ISO; dy <= ISO; dy++) for (let dz = -ISO; dz <= ISO; dz++) for (let dx = -ISO; dx <= ISO; dx++) {
          const nx = ix + dx, ny = iy + dy, nz = iz + dz;
          if (nx < 0 || nx >= bw || nz < 0 || nz >= bw || ny < 0 || ny >= bh) continue;
          if (occ[li(nx, ny, nz)] === 2) return true;
        }
        return false;
      };
      // Even isolation is not enough on its own: about 2% of an UNTOUCHED pine's canopy is attached
      // only diagonally, so whole needle tufts fail any connectivity test while looking perfectly
      // normal. There is no absolute threshold that separates that from a real floater — the only
      // sound instrument is a BEFORE/AFTER diff at the same place. __vb.floatBase() records the
      // baseline; everything reported here is what appeared SINCE.
      const byId = {}, spots = []; let green = 0, other = 0, nearCanopy = 0, fresh = 0, stampedSkip = 0;
      const base = window.__vbFloatBase, now = new Set();
      for (let k = 0; k < nAll; k++) {
        if (occ[k] !== 1) continue;
        const ix = k % bw, iz = ((k / bw) | 0) % bw, iy = (k / (bw * bw)) | 0;
        if (attached(ix, iy, iz)) { nearCanopy++; continue; }   // touching its tree — authored, not a floater
        // ── A LIVE ANIMAL IS NOT A FLOATER ── a perched songbird's grid stamp IS 6-disconnected and IS
        // isolated: it is sitting in a canopy, held up by needles this test cannot see, and it changes pose
        // at 24 fps so a baseline snapshot never matches. Every support test in the file already treats
        // stampedIdx cells as invisible; this instrument has to as well, or a still forest reports ~20
        // permanent floaters that are just birds. (MEASURED: the entire residue after an arrow sweep was
        // four cells, all four conduit.)
        if (stampedIdx.has(wIdx(ix, iy, iz))) { stampedSkip++; continue; }
        const v = W[wIdx(ix, iy, iz)];
        const wk = (x0 + ix) + ':' + (y0 + iy) + ':' + (z0 + iz);
        now.add(wk);
        if (base && base.has(wk)) continue;                    // was already there before you touched anything
        fresh++;
        byId[v] = (byId[v] || 0) + 1;
        if (foliaTab[v]) green++; else other++;
        if (spots.length < 10) spots.push([x0 + ix, y0 + iy, z0 + iz, v]);
      }
      window.__vbFloatLast = now;
      const bods = PH.bodies.filter((b) => b.n <= 60).map((b) => +b.pos[1].toFixed(2));
      return new Promise((res) => setTimeout(() => {
        const now2 = PH.bodies.filter((b) => b.n <= 60).map((b) => +b.pos[1].toFixed(2));
        res({ baseline: base ? 'set' : 'MISSING - call __vb.floatBase() first, before chopping',
               newFloatersGreen: green, newFloatersOther: other, newTotal: fresh, byId, liveStamps: stampedSkip,
               stoppedDescending: now2.filter((y, i) => bods[i] !== undefined && Math.abs(y - bods[i]) < 0.05).length,
               smallBodies: now2.length, spots });
      }, 400));
    },
    // ── GROUND TRUTH FLOATER AUDIT ── floaters() is a 6-connected heuristic seeded from the bottom of a small
    // box, so it both over-reports (a crown edge that really is attached) and needs its baseline taken at the
    // exact pose. This is the real thing: flood 26-connected — the resolver's OWN rule — from every solid voxel
    // at bedrock across a whole region, and report what it never reaches. A component touching the region wall
    // is INCONCLUSIVE, not a floater: it may well hang from geometry outside the box.
    floatAudit(rad, opts) {
      const R = Math.min(96, rad === undefined ? 64 : rad), bw = 2 * R + 1;
      const x0 = Math.round(P.x) - R, z0 = Math.round(P.z) - R;
      let ymax = 8;
      for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++) {
        const h = hmap[gwrap(x0 + ix, WX) + gwrap(z0 + iz, WZ) * WX];
        if (h > ymax) ymax = h;
      }
      // ── THE HEADROOM HAS TO CLEAR A PINE (2026-08-08) ── this was +64 while pine5.vox is MSZ = 116 tall, so
      // the box ceiling cut through the middle of every crown. A component reaching the ceiling is reported
      // INCONCLUSIVE rather than as a floater, which is precisely where a stranded cone or snow cap lives: the
      // audit that every floater pass has been measured against could not see the top half of a tree. MSZ + 44
      // clears the model, the snow stack on its apex and a boulder on a ridge.
      ymax = Math.min(WY - 1, ymax + Math.max(64, MSZ + 44));
      const bh = ymax + 1, n = bw * bw * bh;
      const occ = new Uint8Array(n), stk = new Int32Array(n);
      const li = (ix, iy, iz) => ix + iz * bw + iy * bw * bw;
      const wIdx = (ix, iy, iz) => gwrap(x0 + ix, WX) + iy * WX + gwrap(z0 + iz, WZ) * WX * WY;
      let solid = 0;
      for (let iy = 0; iy <= ymax; iy++) for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++) {
        const ii = wIdx(ix, iy, iz), v = W[ii];
        if (!v) continue;
        if (stampedIdx.has(ii)) continue;                 // a grid-stamped creature is not terrain
        if (SUP.CLASS[v] === SUP.FLUID) { occ[li(ix, iy, iz)] = 2; continue; }   // ANCHOR, exactly as supFlood treats it: a lake holds up whatever rests on it, frozen or not
        occ[li(ix, iy, iz)] = 1; solid++;
      }
      let sp = 0;
      for (let iy = 0; iy <= 1; iy++) for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++) {
        const k = li(ix, iy, iz); if (occ[k] === 1) { occ[k] = 2; stk[sp++] = k; }
      }
      for (let k = 0; k < n; k++) if (occ[k] === 2) stk[sp++] = k;   // every fluid cell seeds the walk as well as bedrock
      while (sp > 0) {                                    // 26-connected, exactly what supFlood walks
        const k = stk[--sp];
        const ix = k % bw, iz = ((k / bw) | 0) % bw, iy = (k / (bw * bw)) | 0;
        for (let d = 0; d < 27; d++) {
          const ax = ix + (d % 3) - 1, ay = iy + (((d / 3) | 0) % 3) - 1, az = iz + (((d / 9) | 0)) - 1;
          if (ax < 0 || az < 0 || ay < 0 || ax >= bw || az >= bw || ay > ymax) continue;
          const q = li(ax, ay, az); if (occ[q] === 1) { occ[q] = 2; stk[sp++] = q; }
        }
      }
      const comps = []; let unreached = 0;
      for (let k = 0; k < n; k++) {
        if (occ[k] !== 1) continue;
        let sp2 = 0, cnt = 0, wall = false, minY = 1e9, maxY = -1e9, sx = 0, sy = 0, sz = 0;
        const ids = {};
        stk[sp2++] = k; occ[k] = 3;
        while (sp2 > 0) {
          const q = stk[--sp2]; cnt++;
          const ix = q % bw, iz = ((q / bw) | 0) % bw, iy = (q / (bw * bw)) | 0;
          if (ix === 0 || iz === 0 || ix === bw - 1 || iz === bw - 1 || iy === ymax) wall = true;
          if (iy < minY) minY = iy; if (iy > maxY) maxY = iy;
          sx += ix; sy += iy; sz += iz;
          const v = W[wIdx(ix, iy, iz)]; ids[v] = (ids[v] || 0) + 1;
          for (let d = 0; d < 27; d++) {
            const ax = ix + (d % 3) - 1, ay = iy + (((d / 3) | 0) % 3) - 1, az = iz + (((d / 9) | 0)) - 1;
            if (ax < 0 || az < 0 || ay < 0 || ax >= bw || az >= bw || ay > ymax) continue;
            const w2 = li(ax, ay, az); if (occ[w2] === 1) { occ[w2] = 3; stk[sp2++] = w2; }
          }
        }
        unreached += cnt;
        comps.push({ n: cnt, wall, yLo: minY, yHi: maxY,
                     at: [x0 + Math.round(sx / cnt), Math.round(sy / cnt), z0 + Math.round(sz / cnt)],
                     ids: Object.entries(ids).sort((a, b) => b[1] - a[1]).slice(0, 3).map(([i, c2]) => i + 'x' + c2) });
      }
      comps.sort((a, b) => b.n - a.n);
      const real = comps.filter((c2) => !c2.wall);
      return { region: { x0, z0, bw, ymax }, solid, unreached,
               floaters: real.length, floaterVox: real.reduce((a, b) => a + b.n, 0),
               inconclusive: comps.length - real.length,
               top: (opts && opts.all ? comps : real).slice(0, 12) };
    },
    iceCuts() { return { n: iceCutN, frozen: iceSolid, freezeK: +freezeK.toFixed(3), choppable: !!decorTab[WATER_T], pickOnly: !!pickOnlyTab[WATER_T], solid: !!solidTab[WATER_T], WATER_T }; },   // ice-chopping state for a test
    eyeSync() { smoothEye = P.y + EYE; resetHist = 1; return smoothEye; },   // a test that moves P directly must say so: aimVox rays from smoothEye, which only a real teleport resets, so every aim after a hand-set P.y silently missed
    // ── THE OTHER HALF OF THE FLOATER QUESTION ── floatAudit reads W, and a severed chunk is NOT in W: it is an
    // off-grid rigid body. A body that stops descending with air under it looks exactly like a floating rock and
    // is invisible to every grid-based check. Reports the drop from each body to the first solid voxel beneath it.
    // ── THE GAP MUST BE MEASURED FROM THE BODY'S LOWEST VOXEL (2026-08-08) ── it compared the COM against the
    // ground under the COM's own column, so a large body reported a gap of 4-6 while sitting perfectly on the
    // floor: its underside reaches rMax BELOW its centre. Two separate investigations chased "a 625-voxel body
    // floating 5.8 above the ground" that was simply resting. `gap` is now the true clearance beneath the
    // lowest voxel, so 0 means touching and a positive number means genuinely airborne.
    // ── IS THIS BODY ACTUALLY FLOATING? ── `gap` below is the clearance under the body's single LOWEST
    // voxel, scanned in that ONE column, and that is not the same question. A felled pine lies at an angle,
    // so its lowest voxel is usually a branch tip out over a dip while the bole rests solidly on higher
    // ground somewhere else entirely — which reads as a 5-voxel gap on a tree that is plainly on the floor.
    // That is the same false positive the note above records, one refinement further in, and it is what a
    // forest-clearing run reports three of.
    // A body is RESTING iff ANY of its voxels has solid world within `tol` beneath it — the same solidTab
    // test phSolidAt uses, so this agrees with the contact generator by construction rather than by a second
    // guess. `clear` is the true minimum clearance over EVERY voxel, not one of them.
    bodySupport(tol) {
      const T = tol === undefined ? 1.5 : tol, p2 = [0, 0, 0], o2 = [0, 0, 0];
      return PH.bodies.map((b, bi) => {
        let best = 1e9, seated = 0;
        for (let i = 0; i < b.n; i++) {
          p2[0] = b.lx[i] + 0.5 - b.com[0]; p2[1] = b.ly[i] + 0.5 - b.com[1]; p2[2] = b.lz[i] + 0.5 - b.com[2];
          phQRot(b.q, p2, o2);
          const wx = b.pos[0] + o2[0], wy = b.pos[1] + o2[1], wz = b.pos[2] + o2[2];
          const gx = gwrap(Math.floor(wx), WX), gz = gwrap(Math.floor(wz), WZ) * WX * WY;
          for (let yy = Math.min(WY - 1, Math.floor(wy)); yy >= 1; yy--) {
            const id = W[gx + yy * WX + gz];
            if (!id || !solidTab[id]) continue;
            const c = wy - (yy + 1); if (c < best) best = c;
            if (c <= T) seated++;
            break;
          }
        }
        return { i: bi, n: b.n, src: b.src, sleeping: !!b.sleeping, contacts: b.contacts | 0,
                 pos: b.pos.map((q) => +q.toFixed(1)),
                 clear: best > 1e8 ? -1 : +best.toFixed(2), seated, floating: seated === 0,
                 vel: +Math.hypot(b.vel[0], b.vel[1], b.vel[2]).toFixed(2),
                 // ── THE TOPPLE STATE MATTERS MORE THAN THE POSE ── tipArm is the window between severing and
                 // the tilt starting, and it is the one phase that deliberately ZEROES the body's velocity. A
                 // trunk found sleeping with tipArm still set never toppled at all: it went to sleep waiting.
                 tipArm: !!b.tipArm, tipping: !!b.tipping, seatT: !!b.tipSeatT, sleepT: b.sleepT | 0,
                 up: +(b.ay ? b.ay[1] : 1).toFixed(3),   // 1 = still upright, 0 = flat on its side (see PH.tipDone)
                 noAbsorb: !!b.noAbsorb, ageS: +((performance.now() - b.born) / 1000).toFixed(1) };
      });
    },
    phWakeAll() { let n = 0; for (const b of PH.bodies) if (b.sleeping) { b.sleeping = false; b.sleepT = 0; n++; } return n; },   // wake every sleeping body — the A/B that separates "asleep in mid-air" from "resting on ground its lowest voxel does not sit over": a body that FALLS when woken was never supported
    bodyAudit() {
      const p = [0, 0, 0], o = [0, 0, 0];
      return PH.bodies.map((b) => {
        const x = b.pos[0], y = b.pos[1], z = b.pos[2];
        let lowY = 1e9, lowX = x, lowZ = z;
        for (let i = 0; i < b.n; i++) {                // body frame -> world, the same transform phBodySolid uses
          p[0] = b.lx[i] + 0.5 - b.com[0]; p[1] = b.ly[i] + 0.5 - b.com[1]; p[2] = b.lz[i] + 0.5 - b.com[2];
          phQRot(b.q, p, o);
          const wy = y + o[1];
          if (wy < lowY) { lowY = wy; lowX = x + o[0]; lowZ = z + o[2]; }
        }
        const gx = gwrap(Math.floor(lowX), WX), gz = gwrap(Math.floor(lowZ), WZ) * WX * WY;
        let g = 1;
        for (let yy = Math.min(WY - 1, Math.round(lowY)); yy >= 1; yy--) { const id = W[gx + yy * WX + gz]; if (id && solidTab[id]) { g = yy + 1; break; } }
        return { n: b.n, pos: [+x.toFixed(1), +y.toFixed(1), +z.toFixed(1)],
                 low: +lowY.toFixed(1), ground: g, gap: +(lowY - g).toFixed(1),
                 vel: +Math.hypot(b.vel[0], b.vel[1], b.vel[2]).toFixed(2),
                 spin: +Math.hypot(b.omega[0], b.omega[1], b.omega[2]).toFixed(2),
                 sleeping: !!b.sleeping, contacts: b.contacts | 0, src: b.src,
                 rag: !!b.rag, timed: (b.absorbAt || 0) > 0 };
      });
    },
    // ── whyFloating's MISSING HALF ── whyFloating marches W, and a rigid body is NOT in W. Aimed at one it
    // answers "the view ray hit nothing solid within 120 voxels" — which is exactly what came back from a
    // session where the player was looking straight at a floating rock. This marches the same ray against the
    // BODY grids, using the collision test the player's own feet use, and reports what that body is doing.
    // ── ONE COMMAND: "WHAT AM I LOOKING AT?" ── whyFloating marches W and whyBody marches the rigid bodies,
    // and a live session answered "nothing" to BOTH while the player was staring at a floating rock. Only so
    // many things can put voxels on screen, so this checks every one instead of guessing: world grid, rigid
    // body, grid-stamped creature, trace-injected creature, dropped item, flying bird. It also reaches much
    // further — the other two stop at 120 and 160 voxels, and something across a valley is simply past them,
    // which on its own produces exactly the pair of "nothing found" answers that came back from the session.
    lookingAt(maxT) {
      const T = Math.max(40, maxT === undefined ? 500 : maxT);
      const cp = Math.cos(P.pitch), d3 = [Math.sin(P.yaw) * cp, Math.sin(P.pitch), Math.cos(P.yaw) * cp];
      const eye = [P.x, smoothEye, P.z];
      for (let t = 0.6; t < T; t += 0.25) {            // 1. the two things that own real voxels
        const fx = P.x + d3[0] * t, fy = smoothEye + d3[1] * t, fz = P.z + d3[2] * t;
        if (fy < 1 || fy >= WY) break;
        const x = Math.floor(fx), y = Math.floor(fy), z = Math.floor(fz);
        const ii = gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY, v = W[ii];
        if (v) {
          const stamped = stampedIdx.has(ii);
          SUP.res.clear(); SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();
          const fr = supFlood(ii, true);
          // ── AND SHOW THE COLUMN ── the verdict almost always comes down to supAnchored on the very first
          // pop (visited: 1), and that oracle is pure column arithmetic. Print the arithmetic so the answer
          // is checkable instead of trusted: how much clear air is directly under this voxel, where the
          // lowest gap in the column actually is, and what hmap claims. If airBelow > 0 while anchoredO is
          // true, the oracle is provably wrong and that is the whole bug, in one line.
          const col = gwrap(x, WX) + gwrap(z, WZ) * WX;
          let airBelow = 0;
          for (let yy = y - 1; yy > 1 && !W[gwrap(x, WX) + yy * WX + gwrap(z, WZ) * WX * WY]; yy--) airBelow++;
          let lowGap = -1;
          for (let yy = 1; yy < Math.min(WY - 1, hmap[col]); yy++) if (!W[gwrap(x, WX) + yy * WX + gwrap(z, WZ) * WX * WY]) { lowGap = yy; break; }
          const colProf = [];
          for (let yy = Math.max(1, y - 14); yy <= y + 2; yy++) colProf.push(W[gwrap(x, WX) + yy * WX + gwrap(z, WZ) * WX * WY] ? 'X' : '.');
          return { kind: stamped ? 'creature (grid-stamped)' : 'world voxel', at: [x, y, z], id: v, dist: Math.round(t),
                   cls: ['ignore', 'fluid', 'structure', 'drape'][SUP.CLASS[v]], anchoredO: supAnchored(ii),
                   airBelow, lowGap, hmap: hmap[col], carved: !!supCarved[col],
                   column: 'y' + Math.max(1, y - 14) + ' ' + colProf.join('') + ' (X=solid, the hit is 3rd from the right)',
                   oracleWrong: airBelow > 0 && supAnchored(ii),
                   flood: fr, why: SUPWHY.why,
                   verdict: stamped ? 'an ANIMAL, not terrain - it is meant to be off the ground'
                     : (!fr ? 'fluid or conduit - never lifted, by design'
                     : (fr.anchored ? (airBelow > 0
                         ? 'ORACLE IS WRONG: ' + airBelow + ' voxels of clear air directly beneath this, and supAnchored still says attached (' + SUPWHY.why + '). hmap=' + hmap[col] + ' lowGap=' + lowGap + ' carved=' + !!supCarved[col]
                         : 'ATTACHED, and correctly so - the column under this is solid (' + SUPWHY.why + '). The ray hit terrain, not the floating thing: re-aim, or raise the reach.')
                     : 'the resolver says DETACHED - it should be lifting; check __vb.support().refused and the body budget')) };
        }
        const bid = phBodyIdAt(x, y, z);
        if (bid) return Object.assign({ kind: 'rigid body', at: [x, y, z], id: bid, dist: Math.round(t) }, __vb.whyBody());
      }
      // 2. nothing with voxels on the ray - so it is drawn by a path that owns no grid at all. Report the
      //    nearest candidate of each kind by perpendicular distance to the view line.
      const perp = (px, py, pz) => {
        if (!Number.isFinite(px) || !Number.isFinite(py) || !Number.isFinite(pz)) return null;   // a perched bird carries its height in perchFeet, not y — a NaN here sailed through the radius test
        const ax = px - eye[0], ay = py - eye[1], az = pz - eye[2];
        const tt = ax * d3[0] + ay * d3[1] + az * d3[2];
        if (tt < 0) return null;
        const ex = ax - d3[0] * tt, ey = ay - d3[1] * tt, ez = az - d3[2] * tt;
        return { off: Math.hypot(ex, ey, ez), t: tt };
      };
      let bestC = null, bestD = null, bestB = null;
      for (let j = 0; j < wbf.length; j++) { const B = wbf[j];
        if (!B || !B.init) continue;
        const r = perp(B.x, B.y === undefined ? B.perchFeet : B.y, B.z); if (!r || r.off > 14) continue;
        if (!bestC || r.off < bestC.off) bestC = { slot: j, kind: B.kind | 0, off: +r.off.toFixed(1), t: Math.round(r.t), pos: [Math.round(B.x), Math.round(B.y === undefined ? B.perchFeet : B.y), Math.round(B.z)], stamped: !!B.sN };
      }
      for (let j = 0; j < birds.length; j++) { const b = birds[j];
        if (!b || !b.init) continue;
        const r = perp(b.x, b.y, b.z); if (!r || r.off > 14) continue;
        if (!bestB || r.off < bestB.off) bestB = { bird: j, off: +r.off.toFixed(1), t: Math.round(r.t), pos: [Math.round(b.x), Math.round(b.y), Math.round(b.z)] };
      }
      for (let j = 0; j < drops.length; j++) { const d = drops[j];
        if (!d) continue;
        const r = perp(d.x, d.y, d.z); if (!r || r.off > 14) continue;
        if (!bestD || r.off < bestD.off) bestD = { it: d.it, off: +r.off.toFixed(1), t: Math.round(r.t), pos: [Math.round(d.x), Math.round(d.y), Math.round(d.z)] };
      }
      return { kind: bestD ? 'dropped item' : (bestC ? 'creature (trace-injected)' : (bestB ? 'flying bird' : 'nothing found')),
               nearestDrop: bestD, nearestCreature: bestC, nearestBird: bestB, reach: T,
               verdict: bestD ? 'a DROPPED ITEM - those hover and spin on purpose; not floating terrain'
                 : (bestC || bestB ? 'an ANIMAL drawn straight into the trace - it owns no world voxels and is meant to be airborne'
                 : 'nothing within ' + T + ' voxels on this ray owns voxels, and no creature or item is near it - so it is drawn by the particle/flake field (snow, sparks, splashes). Say so and I will look there.') };
    },
    whyBody() {
      const cp = Math.cos(P.pitch), d3 = [Math.sin(P.yaw) * cp, Math.sin(P.pitch), Math.cos(P.yaw) * cp];
      for (let t = 0.6; t < 160; t += 0.25) {
        const x = P.x + d3[0] * t, y = smoothEye + d3[1] * t, z = P.z + d3[2] * t;
        if (y < 1 || y >= WY) break;
        const id = phBodyIdAt(Math.floor(x), Math.floor(y), Math.floor(z));
        if (!id) continue;
        const all = __vb.bodyAudit();
        let best = null, bd = 1e9;
        for (const q of all) { const d = (q.pos[0] - x) * (q.pos[0] - x) + (q.pos[1] - y) * (q.pos[1] - y) + (q.pos[2] - z) * (q.pos[2] - z); if (d < bd) { bd = d; best = q; } }
        return { hit: [Math.floor(x), Math.floor(y), Math.floor(z)], id, body: best,
                 verdict: !best ? 'a body voxel with no matching body - report this'
                   : (best.gap <= 0.6 ? 'RESTING - its lowest voxel is on the ground, not floating'
                   : (best.sleeping ? 'ASLEEP IN MID-AIR - ' + best.gap + ' voxels of clear air under its lowest voxel, and the solver skips it entirely'
                   : 'AWAKE AND NOT FALLING - ' + best.gap + ' voxels of air beneath it, vel ' + best.vel + ', contacts ' + best.contacts)) };
      }
      return { hit: null, verdict: 'no rigid body on the view ray within 160 voxels - try __vb.whyFloating() for world voxels' };
    },
    orphStats() { return Object.assign({ queued: ORPHQ.length >> 2 }, ORPH); },   // did the generation sweep run, and did it cut anything?
    // ── VISUALLY FLOATING ROCK ── floatAudit answers the SUPPORT question (26-connected to bedrock), and a
    // boulder resting on one corner voxel passes it while looking completely airborne. This asks the question
    // the eye asks instead: take each rock-only component and count how much of its underside actually sits on
    // something. A component with no supported voxel at all is hanging, whatever the connectivity says.
    rockAudit(rad) {
      const R = Math.min(96, rad === undefined ? 64 : rad), bw = 2 * R + 1;
      const x0 = Math.round(P.x) - R, z0 = Math.round(P.z) - R;
      let ymax = 8;
      for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++) {
        const h = hmap[gwrap(x0 + ix, WX) + gwrap(z0 + iz, WZ) * WX];
        if (h > ymax) ymax = h;
      }
      ymax = Math.min(WY - 1, ymax + 72);
      const bh = ymax + 1, n = bw * bw * bh;
      const li = (ix, iy, iz) => ix + iz * bw + iy * bw * bw;
      const wi = (ix, iy, iz) => gwrap(x0 + ix, WX) + iy * WX + gwrap(z0 + iz, WZ) * WX * WY;
      const isRock = (v) => !!(v && isRockSurf(v));
      const seen = new Uint8Array(n), stk = new Int32Array(n), out = [];
      for (let k0 = 0; k0 < n; k0++) {
        if (seen[k0]) continue;
        const ix0 = k0 % bw, iz0 = ((k0 / bw) | 0) % bw, iy0 = (k0 / (bw * bw)) | 0;
        if (!isRock(W[wi(ix0, iy0, iz0)])) { seen[k0] = 1; continue; }
        let sp = 0, cnt = 0, sup = 0, wall = false, minY = 1e9;
        let sx2 = 0, sy2 = 0, sz2 = 0;
        stk[sp++] = k0; seen[k0] = 1;
        const comp = [];
        while (sp > 0) {
          const k = stk[--sp]; comp.push(k); cnt++;
          const ix = k % bw, iz = ((k / bw) | 0) % bw, iy = (k / (bw * bw)) | 0;
          if (ix === 0 || iz === 0 || ix === bw - 1 || iz === bw - 1 || iy === ymax) wall = true;
          if (iy < minY) minY = iy;
          sx2 += ix; sy2 += iy; sz2 += iz;
          if (iy > 0) { const bv = W[wi(ix, iy - 1, iz)];                 // something solid under me that is NOT my own rock
            if (bv && solidTab[bv] && !isRock(bv)) sup++;
            else if (iy - 1 <= 1) sup++; }
          for (let d = 0; d < 27; d++) {
            const ax = ix + (d % 3) - 1, ay = iy + (((d / 3) | 0) % 3) - 1, az = iz + ((d / 9) | 0) - 1;
            if (ax < 0 || ay < 0 || az < 0 || ax >= bw || az >= bw || ay > ymax) continue;
            const q = li(ax, ay, az);
            if (!seen[q] && isRock(W[wi(ax, ay, az)])) { seen[q] = 1; stk[sp++] = q; }
          }
        }
        if (cnt < 12 || wall) continue;
        let drop = 0;                                    // how far to the first solid under the component's centre
        const cix = Math.round(sx2 / cnt), ciz = Math.round(sz2 / cnt);
        for (let y = minY - 1; y >= 1; y--) { const v = W[wi(cix, y, ciz)]; if (v && solidTab[v] && !isRock(v)) break; drop++; }
        let under = 0, hang = 0, maxDrop = 0;          // UNDERSIDE survey: a big rock is rarely all-floating, it is part buried and part hanging
        for (const k of comp) {
          const ix = k % bw, iz = ((k / bw) | 0) % bw, iy = (k / (bw * bw)) | 0;
          if (iy < 1) continue;
          if (isRock(W[wi(ix, iy - 1, iz)])) continue;   // not an underside voxel - my own rock is below me
          under++;
          let dr = 0;
          for (let y = iy - 1; y >= 1; y--) { const v = W[wi(ix, y, iz)]; if (v && solidTab[v]) break; dr++; if (dr > 40) break; }
          if (dr >= 3) { hang++; if (dr > maxDrop) maxDrop = dr; }
        }
        if (hang > 0) out.push({ n: cnt, under, hang, hangPct: +(100 * hang / Math.max(1, under)).toFixed(0), maxDrop, at: [x0 + cix, minY, z0 + ciz] });
      }
      out.sort((a, b) => b.hang - a.hang);
      return { region: { x0, z0, bw }, hanging: out.length, worstHang: out.length ? out[0].hang : 0, top: out.slice(0, 6) };
    },
    seatCmp(n) { const out = []; const B = BCELL, c0x = Math.floor(P.x / B), c0z = Math.floor(P.z / B), R = n || 6;
      for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++) { const b = boulderAt(c0x + dx, c0z + dz);
        if (!b || b.size === 0) continue; const m = ROCK26[b.mi]; if (!m) continue;
        const oldR = Math.min(10, Math.max(m.sx, m.sy) >> 1);
        const oldY = Math.min(H(b.bx, b.bz), H(b.bx - oldR, b.bz), H(b.bx + oldR, b.bz), H(b.bx, b.bz - oldR), H(b.bx, b.bz + oldR));
        out.push({ size: b.size, w: Math.max(m.sx, m.sy), oldY, newY: rockSeatY(m, b.bx, b.bz), drop: oldY - rockSeatY(m, b.bx, b.bz) }); }
      return { n: out.length, changed: out.filter((o) => o.drop > 0).length, maxDrop: out.reduce((a, o) => Math.max(a, o.drop), 0), sample: out.slice(0, 8) }; },   // does the wider seat probe actually move any boulder?
    // ── DOES EVERY TRUNK REACH THE GROUND? ── floatAudit is blind to this: a pine is tall, its canopy reaches
    // the audit box wall, and a wall-touching component is reported INCONCLUSIVE rather than as a floater. So a
    // whole tree hanging in the air scores zero. This asks the trunk directly - find the lowest WOOD voxel in
    // each column and measure the drop to the first solid thing under it.
    treeAudit(rad) {
      const R = Math.min(96, rad === undefined ? 64 : rad), bw = 2 * R + 1;
      const x0 = Math.round(P.x) - R, z0 = Math.round(P.z) - R;
      const at = (x, y, z) => W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      const seen = new Set(), out = []; let attached = 0;
      for (let iz = 0; iz < bw; iz++) for (let ix = 0; ix < bw; ix++) {
        const wx = x0 + ix, wz = z0 + iz;
        let lo = -1;
        for (let y = 2; y < WY - 1; y++) { const v = at(wx, y, wz); if (v && woodTab[v]) { lo = y; break; } }
        if (lo < 0) continue;
        let drop = 0;
        for (let y = lo - 1; y >= 1; y--) { const v = at(wx, y, wz); if (v && solidTab[v]) break; drop++; if (drop > 48) break; }
        if (drop < 2) continue;                        // resting on the ground (or one voxel proud) is fine
        let run = 0;                                   // a BRANCH also has air under it, and that is not a defect: only judge a real TRUNK,
        for (let y = lo; y < WY - 1; y++) { const v = at(wx, y, wz); if (!(v && woodTab[v])) break; run++; }
        if (run < 10) continue;                        // which is a long unbroken vertical run of wood, not a couple of courses of limb
        if (lo > hmap[gwrap(wx, WX) + gwrap(wz, WZ) * WX] + 24) continue;   // and starts near the ground, not up in the canopy
        // -- AND IS IT ACTUALLY LOOSE? (2026-08-20) -- everything above is a HEURISTIC for "this looks like a
        // trunk", and on a big oak it is wrong: a mature oak throws near-vertical limbs that clear the +24
        // canopy guard by a voxel or two, run ten courses of unbroken wood, and of course have air beneath
        // them - because they are branches. Audited across 14 boxes of oak forest this reported 22 floating
        // trunks and the resolver called all 22 of them ANCHORED, class `drape`: not one was loose. A tool
        // that cries floater at every big oak is worse than no tool, so the geometry test now only nominates
        // a candidate and the SUPPORT RESOLVER returns the verdict - the same flood supWhy runs, which is what
        // the game itself uses to decide whether a voxel falls. Anchored columns are counted as `attached`
        // rather than dropped silently, so the sweep still shows its working.
        {
          const ii0 = gwrap(wx, WX) + lo * WX + gwrap(wz, WZ) * WX * WY;
          SUP.res.clear(); SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();
          const v = supFlood(ii0, true);
          if (v && v.anchored) { attached++; continue; }
        }
        const key = ((wx / 12) | 0) + ',' + ((wz / 12) | 0);   // cluster columns of the same trunk together
        if (seen.has(key)) { const e = out.find((q) => q.key === key); if (e) { e.cols++; if (drop > e.drop) e.drop = drop; } continue; }
        seen.add(key); out.push({ key, cols: 1, drop, base: lo, at: [wx, lo, wz] });
      }
      out.sort((a, b) => b.drop - a.drop);
      return { floating: out.length, attached, worstDrop: out.length ? out[0].drop : 0, top: out.slice(0, 8) };
    },
    supSeed(x, y, z) { const ii = gwrap(Math.floor(x), WX) + (y | 0) * WX + gwrap(Math.floor(z), WZ) * WX * WY; if (W[ii]) supPush(ii); return !!W[ii]; },   // hand the resolver a specific voxel to adjudicate
    // ── NAMED supWhy, NOT supProbe (2026-08-08) ── this was a SECOND `supProbe` key in the same object
    // literal, so it silently shadowed the cheap per-cell probe a few thousand lines above and every test
    // asking "what id/class/conduit is this cell" got a flood report with none of those fields instead.
    // Both questions are worth asking; they now have separate names.
    supWhy(x, y, z) { const ii = gwrap(Math.floor(x), WX) + (y | 0) * WX + gwrap(Math.floor(z), WZ) * WX * WY;
      if (!W[ii]) return { empty: true };
      SUP.res.clear(); SUP.ancS.clear(); SUP.flS.clear(); SUP.busy.clear(); supColMemo = new Map();
      const r = supFlood(ii, true);
      return r ? Object.assign({ id: W[ii], anchoredO: supAnchored(ii), why: SUPWHY.why, depthHits: SUPWHY.d }, r) : { nullFlood: true, id: W[ii] };
    },   // ask the resolver, out loud, why it thinks this voxel is held
    snowRoll(v) { if (v !== undefined) SNOW_ROLL_MAX = v | 0; return SNOW_ROLL_MAX; },   // max drop the settle-roll may take; 0 = unlimited (the old behaviour)
    snowShelf(v) { if (v !== undefined) SNOW_SHELF = v | 0; return SNOW_SHELF; },   // A/B the crown shelf rule in-session
    snowCrown(v) { if (v !== undefined) SNOW_ON_CROWN = v ? 1 : 0; return SNOW_ON_CROWN; },   // A/B canopy snow in-session
    isFoliaId(id) { return !!foliaTab[id | 0]; },       // needles/leaves — canopy snow rests on THESE, not on wood
    birdsNear(n) { const o = [], P0 = P;                 // FLYING songbirds only — they live in birds[], not the creature pool
      for (let i = 0; i < birds.length; i++) { const b = birds[i]; if (!b || !b.init) continue;
        const dx = b.x - P0.x, dz = b.z - P0.z, d = Math.hypot(dx, dz);
        if (d > (n || 400)) continue;
        o.push({ i, x: +b.x.toFixed(1), y: +b.y.toFixed(1), z: +b.z.toFixed(1), d: +d.toFixed(1), up: +(b.y - P0.y).toFixed(1) }); }
      o.sort((a, b2) => a.d - b2.d); return o; },
    bowShoot(dk) { return launchThrown(ARROW_IT, ARROW_V * (dk === undefined ? 1 : dk), ARROW_UP * (dk === undefined ? 1 : dk), 'arrow'); },   // fire the bow without a real draw, so a test can aim precisely
    phSrcStats(r) { if (r) for (const k in PHSRC) delete PHSRC[k]; return Object.assign({}, PHSRC); },
    stopCache(v) { if (v !== undefined) STOP_CACHE = v ? 1 : 0; return STOP_CACHE; },
    isConeId(id) { return !!coneTab[id | 0]; },        // pinecone palette ids — a felled pine must not leave these hanging
    idAt(x, y, z) { return W[gwrap(Math.floor(x), WX) + (y | 0) * WX + gwrap(Math.floor(z), WZ) * WX * WY]; },   // raw voxel id — colTop/colTopId are solidTab-gated and go blind to water the moment it thaws
    supCap(v) { if (v !== undefined) SUP.cap = Math.max(16, v | 0); return { cap: SUP.cap, drapeCap: SUP.drapeCap, capHits: SUP.stats.capHits, structFloods: SUP.stats.structFloods, dropped: SUP.stats.dropped }; },   // squeeze the STRUCTURE flood ceiling so a test can force the cap-hit path on a small rock
    wrefl(v) { if (v !== undefined) { wReflK = Math.max(0, Math.min(2, +v)); resetHist = 1; try { localStorage.setItem('vb_wrefl', String(wReflK)); } catch (e) {} lgtPaint(); } return wReflK; },   // WATER REFLECTION STRENGTH — the panel's slider, from the console
    lgt2(m) { if (m !== undefined) { lgtMask2 = m | 0; resetHist = 1; lgtPaint(); } return { mask2: lgtMask2, all2: LGT2_ALL, terms2: { rockSheen: 1 } }; },   // the SECOND mask (u.lgt.z) — bit 0 is the SUN SHEEN ON STONE (user 2026-08-16). __vb.lgt2(0) / __vb.lgt2(1) is the A/B: it sets resetHist, so the denoiser does not hand the previous variant's history to the next shot
    lgt(m) { if (m !== undefined) { lgtMask = m | 0; resetHist = 1; lgtPaint(); } return { mask: lgtMask, all: LGT_ALL, water: LGT_WATER, mask2: lgtMask2, terms: { sun: 1, ao: 2, creatureShadow: 4, glow: 8, reactive: 16, fog: 32, irrHistory: 64, spatial: 128, taa: 256, bodyGrain: 512, terrainGrain: 1024, creatureGrain: 2048, penumbra: 4096, caustics: 8192, bounce: 16384, skyAmbient: 32768, heldItem: 65536, volumetric: 131072,
      waterReflect: 262144, waterRefract: 524288, waterFoam: 1048576, waterIce: 2097152, waterGlisten: 4194304, waterWaves: 8388608 } }; },   // the light-debug bitmask, from the console
    physFreeze(v) { const f = v === undefined ? true : !!v;   // pin/unpin every body — lets a test aim at a KNOWN pose instead of chasing a falling one
      for (const b of PH.bodies) { b.sleeping = f; if (f) { b.vel[0] = b.vel[1] = b.vel[2] = 0; b.omega[0] = b.omega[1] = b.omega[2] = 0; } }
      return PH.bodies.map((b) => ({ vox: b.n, pos: b.pos.map((q) => +q.toFixed(2)), sleeping: b.sleeping })); },
    physValidate() {                                   // invariants the system must hold; ok:true = pass
      const out = { problems: [], bodies: PH.bodies.length, dupVoxels: 0 };
      let nan = 0;
      for (const b of PH.bodies) {
        for (const q of b.pos.concat(b.vel, b.omega, b.q)) if (!Number.isFinite(q)) nan++;
        if (b.n !== b.lx.length) out.problems.push('voxel count mismatch');
        if (b.mass !== b.n) out.problems.push('mass != voxel count');
        if (b.pos[1] < -200 || b.pos[1] > WY + 400) out.problems.push('body out of world Y');
        // NO DUPLICATE: every voxel a body owns must be GONE from the static world.
        for (let i = 0; i < b.n; i++) {
          const ii = gwrap(b.origin[0] + b.lx[i], WX) + (b.origin[1] + b.ly[i]) * WX + gwrap(b.origin[2] + b.lz[i], WZ) * WX * WY;
          if (W[ii] === b.id[i]) out.dupVoxels++;
        }
      }
      if (nan) out.problems.push(nan + ' non-finite body state values');
      if (out.dupVoxels) out.problems.push(out.dupVoxels + ' body voxels STILL present in W (duplicate)');
      out.ok = out.problems.length === 0;
      return out;
    },
    bouncyIds() { const o = []; for (let i = 0; i < 256; i++) if (mushTab[i]) o.push(i); return o; },   // palette ids flagged BOUNCY (mushroom trampoline) — a creature whose stamp shares one of these would trampoline the player
    mushProbe() {                                      // what the trampoline test sees under the player RIGHT NOW.
      const gy = Math.floor(P.y) - 1;                  // raw = the old id-only rule, live = the rule that ignores creature stamps.
      if (gy < 0 || gy >= WY) return { raw: false, live: false, ids: [] };
      const x0 = Math.floor(P.x - HW), x1 = Math.floor(P.x + HW), z0 = Math.floor(P.z - HW), z1 = Math.floor(P.z + HW);
      let raw = false; const ids = [];
      for (let z = z0; z <= z1; z++) for (let x = x0; x <= x1; x++) {
        const ii = gwrap(x, WX) + gy * WX + gwrap(z, WZ) * WX * WY;
        if (mushTab[W[ii]]) { raw = true; ids.push([W[ii], cellStamped(ii) ? 'creature' : 'world']); }
      }
      return { raw, live: onMushroom(), ids };
    },
    stampIds(a, b) { const s = new Set(); for (let j = a; j < b; j++) { const B = wbf[j];   // ids a grid-stamped creature band currently has in the world
      if (B && B.sN) for (let i = 0; i < B.sN; i++) s.add(W[B.sCells[i]]); } return [...s].sort((p, q) => p - q); },
    rd() { return { renderDist, half: HALF, windowX: WX }; },   // fixed view-distance tap
    lifedbg(m) { lifeDbg = m === undefined ? 0 : m | 0; return { mode: lifeDbg, traceInjected: LIFE_TRACE }; },   // debug views: 0 off / 1 slot ids / 2 history confidence / 3 motion / 4 denoised AO / 5 RAW sun visibility / 6 DENOISED sun visibility
    birdCensus(r, wx, wz) {                          // tally the songbird colours around any centre, straight from the placement rule
      // ── BOTH TREE GRIDS (2026-08-17) ── this walked the PINE grid only, so standing in the oak forest it
      // reported pines: 0, birds: 0 and the colour split read as broken, when in fact the oaks around you are
      // full of birds placed by the same rule on a different grid. Two enumerations, one tally, because the
      // question "what does the placement rule put near me" has two placement rules now.
      const R = r || 6000, t = [0, 0, 0, 0]; let pines = 0, oaks = 0, birds = 0;   // FOUR slots: birdColour returns 3 in the cherry forest, and `t[3]++` on a three-slot array is NaN, which then read as pink: 0 while 37% of the birds were pink
      const cx0 = wx === undefined ? P.x : wx, cz0 = wz === undefined ? P.z : wz;
      const c0x = Math.floor(cx0 / TCELL), c0z = Math.floor(cz0 / TCELL), n = Math.ceil(R / TCELL);
      for (let dz = -n; dz <= n; dz++) for (let dx = -n; dx <= n; dx++) {
        const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
        pines++;
        const k = birdsOnPine(tr.tx, tr.tz);
        for (let i = 0; i < k; i++) { t[birdColour(tr.tx, tr.tz, i)]++; birds++; }
      }
      const o0x = Math.floor(cx0 / OKCELL), o0z = Math.floor(cz0 / OKCELL), on = Math.ceil(R / OKCELL);
      for (let dz = -on; dz <= on; dz++) for (let dx = -on; dx <= on; dx++) {
        const tr = oakAt(o0x + dx, o0z + dz); if (!tr) continue;
        oaks++;
        const k = birdsOnOak(tr.wx, tr.wz, tr.k);
        for (let i = 0; i < k; i++) { t[birdColour(tr.wx, tr.wz, i)]++; birds++; }
      }
      return { radius: R, pines, oaks, trees: pines + oaks, birds, cardinal: t[0], blue: t[1], robin: t[2], pink: t[3] | 0,
        pct: t.map((x) => +((x | 0) * 100 / Math.max(1, birds)).toFixed(1)) };   // …and the PINK one, or the census reported three species summing to 61% and left the rest unnamed
    },
    // ── THE RAIN SKY, BOTH FACTORS SEPARATELY ── k is the storm CLOCK (ramps 0->1 over 10 s, back over
    // 20 s) and atCam is what the shader actually gets, k * oakM at the camera. Reported apart because
    // the two failure modes are different: k stuck at 1 after a storm is a broken ramp, atCam stuck at 0
    // during one is a broken biome weight, and a single number cannot tell you which.
    // ── THE FLYING FLOCK ── the 9 wandering songbirds, which live in DROP slots rather than creature
    // slots, so lifeAll() cannot see them and a screenshot can only ever prove they are not in ONE
    // 90-degree view. `alt` is height above the bird's own terrain sample, which is what the flight
    // floor is expressed in; `om` says which biome it is currently over.
    flyers() { return birds.map((b, i) => ({ i, init: !!b.init, x: Math.round(b.x), z: Math.round(b.z),
      y: Math.round(b.y), g: Math.round(b.g), alt: Math.round(b.y - b.g), sp: b.sp | 0, mode: b.mode | 0,
      om: +oakM(b.x, b.z).toFixed(2), d: Math.round(Math.hypot(b.x - P.x, b.z - P.z)) })); },
    rainSky() { return { k: +rainSkyK.toFixed(3), atCam: +UF[UF_RAINK].toFixed(3),
      oakAtCam: +oakM(P.x, P.z).toFixed(3), storm: !!snowOn }; },
    compass() { return { on: cmpOn, hidden: $('compass').classList.contains('hidden'), stored: localStorage.getItem('vb_cmp') }; },   // compass-toggle tap                // palette occupancy tap — edCol silently NEAREST-MATCHES once this hits 256, which would quietly wash a reskin's colours into another bird's
    // ── WHERE THE STACK BADGE ENDED UP ── the held model's projected top-right corner, which main/tick-camera.js
    // computes and BLIT draws at. `px` is the canvas pixel actually handed to the shader; `dims` is the item
    // grid the corners were built from, and a null there is the whole answer to "the badge is nowhere near the
    // item" — it means the item table lookup missed and the fallback parked the glyphs off screen.
    badgeDbg() { const it = shownIt | 0, m = itemsRef && itemsRef[it - 1];
      return { item: it, name: ITEM_NAMES[it] || null, dims: m ? [m.w, m.d, m.h] : null,
        count: +UF[UF_HELDCFG + 2].toFixed(2), px: [Math.round(UF[UF_BADGE]), Math.round(UF[UF_BADGE + 1])],
        size: +UF[UF_BADGE + 2].toFixed(2), tilt: +UF[UF_BADGE + 3].toFixed(2),
        anchor: [+UF[48].toFixed(3), +UF[49].toFixed(3), +UF[50].toFixed(3)], vox: +UF[51].toFixed(4),
        canvas: [UF[42], UF[43]], tanH: +UF[3].toFixed(3), aspect: +UF[7].toFixed(3) }; },
    itemCols(id) {                                   // distinct colours + dims of one item (1-based dit) — proves what a species actually renders with
      const it = itemsRef && itemsRef[id - 1]; if (!it) return null;
      const set = new Map();
      for (const c of it.cells) if (c) { const k = '#' + c.map((q) => q.toString(16).padStart(2, '0')).join(''); set.set(k, (set.get(k) || 0) + 1); }
      return { id, w: it.w, d: it.d, h: it.h, nVox: [...set.values()].reduce((p2, q) => p2 + q, 0),
        cols: [...set.entries()].sort((p2, q) => q[1] - p2[1]).map(([c, n]) => c + '×' + n) };
    },
    bird() { const cen = {}; for (const b of birds) if (b.init) { const nm = (FLYERS[b.sp] || {}).name || '?'; cen[nm] = (cen[nm] || 0) + 1; }
      return { n: BIRD_N, species: FLYERS.map((f) => ({ name: f.name, item0: f.item0, n: f.n, glide: f.glide })), census: cen,
      frames: BIRD_NFRAMES, glidePose: BIRD_GLIDE, blueFrames: BLUEF_NFRAMES, cardBase: BIRD_ITEM0, blueBase: BLUEF_ITEM0,
      birds: birds.map((b) => ({ init: b.init, x: +b.x.toFixed(1), y: +b.y.toFixed(1), z: +b.z.toFixed(1), mode: b.mode, glid: !!b.glid, fi: b.fi | 0, edge: !!b.edge, sp: b.sp | 0,
        d: Math.round(Math.hypot(b.x - P.x, b.z - P.z)) })) }; },   // flyer tap
    birdBox() { return { ...birdBox }; }, wormStamps() { let n = 0, live = 0, off = 0; for (let j = WORM_0; j < WORM_END; j++) { const B = wbf[j]; if (B && B.sN) { n += B.sN; live++; } if (B && B.init && (B.kind | 0) === 2) off++; } return { voxels: n, worms: live, live: off }; },   // Task 2/3 taps — `live` = active off-grid worms (worms render off-grid now)
    duckStamps() { let n = 0, live = 0, paused = 0; for (let j = DUCK_0; j < BABY_END; j++) { const B = wbf[j]; if (B && B.sN) { n += B.sN; live++; } } for (let j = WORM_0; j < WORM_END; j++) { const B = wbf[j]; if (B && B.wpause) paused++; } return { duckVoxels: n, ducks: live, wormsPaused: paused }; },   // Task 6 + worm-pause taps
    perched() { const o = []; for (let j = CARD_0; j < CARD_END; j++) { const B = wbf[j]; if (B && B.init && (B.kind | 0) === 5) { const fx = Math.floor(B.x), fz = Math.floor(B.z), fy = Math.round(B.perchFeet || 0); o.push({ j, x: fx, z: fz, feetY: fy, bird: B.bird | 0, below: this.vox(fx, fy - 1, fz), cells: B.sN | 0, want: B.sWant | 0, ids: B.sN ? [...new Set(Array.from(B.sCells.subarray(0, B.sN), (ii) => W[ii]))].sort((p2, q2) => p2 - q2) : [] }); } } return o; },   // perched-cardinal test tap
    reroll() { rerollSpawn(); return spawnBake; }, spawnBake() { return spawnBake; },   // H-key spawn reset + the bake string
    prof(on) { if (on !== undefined) profArm(on); const o = Object.fromEntries(PROF_NAMES.map((n, i) => [n, +profEma[i].toFixed(3)])); o.fps = +fpsEma.toFixed(0); return o; },
    profMin(reset) { const o = Object.fromEntries(PROF_NAMES.map((n, i) => [n, profMin[i] > 1e8 ? -1 : +profMin[i].toFixed(3)]));   // uncontended per-pass cost — the A/B statistic (see profMin above)
      o.n = profSamp; if (reset) { for (let i = 0; i < 7; i++) profMin[i] = 1e9; profSamp = 0; } return o; },
    cprof(on) { if (on !== undefined) cprofArm(on);          // CPU phase timings + per-frame GPU upload volume
      const o = Object.fromEntries(CP_NAMES.map((n, i) => [n, +cpEma[i].toFixed(3)]));
      o.cpuTotal = +CP_NAMES.reduce((a, n, i) => a + cpEma[i], 0).toFixed(3);
      o.upCalls = +cpUpN.toFixed(1); o.upKB = +(cpUpB / 1024).toFixed(1); o.fps = +fpsEma.toFixed(0); return o; },
    ft() { const n = Math.min(ftN, FTR); if (!n) return null;   // frame-time distribution → 1% lows + worst spike
      const a = Array.from(FT.subarray(0, n)).sort((p, q) => p - q);
      const b = Array.from(FTB.subarray(0, n)).sort((p, q) => p - q);
      const q = (arr, f) => +arr[Math.min(n - 1, Math.floor(n * f))].toFixed(2);
      const mean = a.reduce((p, c) => p + c, 0) / n;
      return { n, gcFrames: heapDrops, allocMBs: +(heapAlloc / 1048576).toFixed(1),
        avg: +mean.toFixed(2), p50: q(a, 0.5), p90: q(a, 0.9), p99: q(a, 0.99), max: +a[n - 1].toFixed(2),
        fps1pctLow: +(1000 / q(a, 0.99)).toFixed(1), fpsAvg: +(1000 / mean).toFixed(1),
        tickP50: q(b, 0.5), tickP99: q(b, 0.99), tickMax: +b[n - 1].toFixed(2) }; },   // tick* = CPU inside tickBody; flat tick* against a spiking max means the stall is OUTSIDE our code (pacing / present / GC)
    spikes() { return { th: cpSpikeTh, names: CP_NAMES, evNames: CPE_NAMES, list: cpSpikes.slice() }; },
    spikeTh(v) { if (v !== undefined) cpSpikeTh = v; cpSpikes.length = 0; return cpSpikeTh; },
    ftReset() { ftN = 0; ftI = 0; cpSpikes.length = 0; heapDrops = 0; heapAlloc = 0; return true; },
    mem() { const m = performance.memory || {};        // CPU heap + the static GPU allocation the world costs
      return { jsHeapMB: +((m.usedJSHeapSize || 0) / 1048576).toFixed(1), jsHeapTotalMB: +((m.totalJSHeapSize || 0) / 1048576).toFixed(1),
        worldMB: +(W.byteLength / 1048576).toFixed(1), bricksMB: +((bricks.byteLength + bricks2.byteLength + wbricks.byteLength) / 1048576).toFixed(2),
        hmapMB: +(hmap.byteLength / 1048576).toFixed(2), stagMB: +(stag.byteLength / 1048576).toFixed(2), RW, RH, CW, CH, renderScale }; },
    res(v) { if (v !== undefined) { renderScale = Math.max(0.375, Math.min(1, v)); makeTargets(true); resSync(); } return { renderScale, RW, RH }; },   // A/B the resolution scale from a test (does NOT persist — a test must not rewrite the player's vb_scale)
    async gpudiff() {                                  // read the GPU world back and diff vs CPU W — INSIDE rect only (outside is stale by design). 0 = in sync.
      patchFlush();                                    // staged voxel edits must land before the readback, or they read as false diffs
      const CH = 64 << 20, stg = device.createBuffer({ size: CH, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const WXW = WX >> 2, mx = new Uint8Array(WXW), mz = new Uint8Array(WZ);   // per-word x-mask and per-row z-mask; boundary words count as outside (no false positives)
      const gx0 = gwrap(rect.xlo, WX), xw = rect.xhi - rect.xlo, gz0 = gwrap(rect.zlo, WZ), zw = rect.zhi - rect.zlo;
      for (let wq = 0; wq < WXW; wq++) { let ok = 1; for (let k = 0; k < 4; k++) if (((wq * 4 + k - gx0 + WX) % WX) >= xw) ok = 0; mx[wq] = ok; }
      for (let z = 0; z < WZ; z++) mz[z] = ((z - gz0 + WZ) % WZ) < zw ? 1 : 0;
      let diffs = 0; const spots = [];
      for (let off = 0; off < W.byteLength; off += CH) {
        const len = Math.min(CH, W.byteLength - off);
        const enc2 = device.createCommandEncoder(); enc2.copyBufferToBuffer(worldBuf, off, stg, 0, len); device.queue.submit([enc2.finish()]);
        await stg.mapAsync(GPUMapMode.READ, 0, len);
        const g = new Uint32Array(stg.getMappedRange(0, len)), w0 = off >> 2;
        for (let i = 0; i < g.length; i++) {
          if (g[i] === W32[w0 + i]) continue;
          const wAbs = w0 + i, gz = (wAbs / (WX * WY >> 2)) | 0, rem = wAbs % (WX * WY >> 2), gy = (rem / WXW) | 0, wq = rem % WXW;
          if (!mz[gz] || !mx[wq]) continue;            // stale-by-design territory
          diffs++;
          if (spots.length < 12) spots.push([wq * 4, gy, gz, g[i] >>> 0, W32[wAbs] >>> 0]);
        }
        stg.unmap();
      }
      stg.destroy();
      return { diffs, spots };
    },
    async bdiff() {                                    // GPU-vs-CPU OCCUPANCY diff (bricks / bricks2 / wbricks). 0 = in sync.
      patchFlush();                                    // pending voxel edits carry pending brick bits
      const out = {};
      for (const [name, cpu, buf] of [['bricks', bricks, brickBuf], ['bricks2', bricks2, brick2Buf], ['wbricks', wbricks, wbrickBuf]]) {
        const stg = device.createBuffer({ size: cpu.byteLength, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        const e2 = device.createCommandEncoder(); e2.copyBufferToBuffer(buf, 0, stg, 0, cpu.byteLength); device.queue.submit([e2.finish()]);
        await stg.mapAsync(GPUMapMode.READ);
        const g = new Uint32Array(stg.getMappedRange());
        let bad = 0; const spots = [];
        for (let i = 0; i < g.length; i++) if (g[i] !== cpu[i]) { bad++; if (spots.length < 8) spots.push([i, g[i] >>> 0, cpu[i] >>> 0]); }
        out[name] = bad ? { bad, spots } : 0;
        stg.unmap(); stg.destroy();
      }
      return out;
    },
    scanCliffs(TH, LEN) { const th = TH || 25, ln = LEN || 24, runs = [];   /*TEMP-DEBUG: find long AXIS-ALIGNED hmap cliffs (rect-pit edges)*/
      for (let gz = 0; gz < WZ - 1; gz++) { let run = 0, sx = 0;
        for (let gx = 0; gx < WX; gx++) { const d = Math.abs(hmap[gx + gz * WX] - hmap[gx + (gz + 1) * WX]);
          if (d >= th) { if (!run) sx = gx; run++; } else { if (run >= ln) runs.push(['z', sx, gz, run]); run = 0; } }
        if (run >= ln) runs.push(['z', 0, gz, run]); }
      for (let gx = 0; gx < WX - 1; gx++) { let run = 0, sz = 0;
        for (let gz = 0; gz < WZ; gz++) { const d = Math.abs(hmap[gx + gz * WX] - hmap[(gx + 1) + gz * WX]);
          if (d >= th) { if (!run) sz = gz; run++; } else { if (run >= ln) runs.push(['x', gx, sz, run]); run = 0; } }
        if (run >= ln) runs.push(['x', gx, 0, run]); }
      return { rect: { xlo: rect.xlo, xhi: rect.xhi, zlo: rect.zlo, zhi: rect.zhi }, winOX, winOZ, px: P.x, pz: P.z, nRuns: runs.length, runs: runs.slice(0, 40) }; },
    hAt(x, z) { return { hmap: hmap[gwrap(Math.floor(x), WX) + gwrap(Math.floor(z), WZ) * WX], analytic: H(Math.floor(x), Math.floor(z)) }; },   /*TEMP-DEBUG*/
    sparkDbg() { return sparks3d.map((s) => s ? (s.smoke ? 'smoke' : (s.foam ? 'foam' : 'spark')) : null); },   /*TEMP-DEBUG: death-burst / clash-spark / SPLASH slot state*/
    foamId() { return FOAM_IT; },                      // the splash droplet's item id
    splashAt(x, z, k) { spawnSplash(x, z, k); return __vb.sparkDbg().slice(0, 4); },   // fire a splash on demand (test tap)
    duckEyes() { return { n: DUCKB_EYES.length, cells: DUCKB_EYES }; },   // the duckling eye voxels the tears come out of - offsets from the model's CENTRE, which is where the drop-slot tracer anchors it
    crying() { const o = []; for (let j = BABY_0; j < BABY_END; j++) { const B = wbf[j];
      if (B && B.init && B.cryTo) o.push({ slot: j, msLeft: Math.round(B.cryTo - performance.now()) }); } return o; },
    tears() { const n = performance.now(); return sparks3d.slice(TEAR_LO, TEAR_HI).filter((s) => s && s.foam && (n - s.born) / 1000 < s.life).length; },   // live TEAR droplets, tear band only
    splashLive() { const n = performance.now(); return sparks3d.slice(SPLASH_LO, SPLASH_HI).filter((s) => s && s.foam && (n - s.born) / 1000 < s.life).length; },
    splashLife() { return SPLASH_LIFE; },
    sparkAll() { return sparks3d.map((s) => s ? { born: s.born, life: s.life, foam: !!s.foam, smoke: !!s.smoke } : null); },   // every particle slot's identity — a test can watch for one being taken over mid-life
    bodyMats() { return PH.bodies.map((b) => { let crea = 0; for (let i = 0; i < b.n; i++) if (CREA_FLAG[b.id[i]]) crea++;
      return { vox: b.n, creatureVox: crea, rag: !!b.rag, src: b.src }; }); },   // how much of each live rigid body is CREATURE material — anything non-ragdoll with creature voxels is an animal that got carved up
    bodySnow() { return PH.bodies.map((b) => { let s = 0, k = 0; for (let i = 0; i < b.n; i++) { if (snowTab[b.id[i]]) s++; else if (coneTab[b.id[i]]) k++; }
      return { vox: b.n, snow: s, cone: k, src: b.src, y: +b.pos[1].toFixed(1) }; }).filter((q) => q.snow || q.cone); },   // which live bodies are CARRYING drape — the read that proves the blanket AND the cones left with the tree instead of being stranded in the air behind it (see phDrapeWith)
    ragdolls() { return { made: PH.stats.ragdolls | 0,
      live: PH.bodies.filter((b) => b.rag).map((b) => ({ vox: b.n, pos: b.pos.map((q) => +q.toFixed(1)), sleeping: !!b.sleeping })) }; },
    ragOf(slot) { const B = wbf[slot | 0]; if (!B) return null;
      return { rag: !!B.rag, hasBody: !!B.ragBody, bodyVox: B.ragBody ? B.ragBody.n : 0,
        pos: B.ragBody ? B.ragBody.pos.map((q) => +q.toFixed(1)) : null, init: !!B.init, slain: !!B.slain }; },
    splashDbg() { const now = performance.now();       // live particle heights, computed EXACTLY as the emit does (y + vy·t − grav·t²) — so a test reads the height actually drawn, not a guess
      return sparks3d.slice(0, 4).map((s) => { if (!s) return null;
        const t = (now - s.born) / 1000; if (t > s.life) return null;
        return { y: s.y + s.vy * t - (s.smoke ? 1.5 : 85) * t * t, aboveWL: s.y + s.vy * t - 85 * t * t - WL, foam: !!s.foam }; }); },
    killSlot(s) { swingStart = performance.now(); hitCreature(s | 0); return { init: !!wbf[s].init, hits: wbf[s].hits | 0, dying: !!wbf[s].dying }; },   // each tap counts as its OWN swing: the one-hit-per-swing guard keys on swingStart, which this tap otherwise never advances
    hitsOn(s) { const B = wbf[s | 0]; return B ? { hits: B.hits | 0, needs: hitsNeeded(), alive: !!B.init, dying: !!B.dying } : null; },   // `needs` is the LIVE answer for what is in the hand right now (sim/life/reactions.js), not the bare constant it used to print — with an arrow's two and the knife's two beside the default three, a fixed number here is simply wrong three ways out of four   // how many hits this creature has taken, for checking the rule in play   // run the hit on a KNOWN creature, no aiming involved
    hurtTest(slot, hold) { const B = wbf[slot | 0]; if (!B || !B.init) return null; HURT.slot = slot | 0; HURT.hold = !!hold; hurtBox(B); HURT.t0 = performance.now(); return __vb.hurtInfo(); },   // arm the wounded flash WITHOUT the hit, so a capture can be timed against it
    aimed() { return aimedCreature(); },              // which life slot the crosshair is actually on (-1 = none) — a swing test has to confirm it is ON target before it can read anything into the result
    kill() { tryKillCreature(); return sparks3d.map((s) => s ? (s.smoke ? 'smoke' : 'spark') : null); },   /*TEMP-DEBUG: force a kill-attempt from the current camera*/
    testBurst() { const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch), fx = Math.sin(P.yaw) * cp, fy = sp, fz = Math.cos(P.yaw) * cp; spawnDeathBurst(P.x + fx * 12, smoothEye + fy * 12 - 2, P.z + fz * 12); return true; },   /*TEMP-DEBUG: fire a death poof 12 vox ahead of the camera*/
    killProbe() { const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch), vx = Math.sin(P.yaw) * cp, vy = sp, vz = Math.cos(P.yaw) * cp; let best = null;   /*TEMP-DEBUG*/
      for (let wk = 0; wk < DES_END; wk++) { const B = wbf[wk]; if (!B || !B.init) continue; const dx = B.x - P.x, dy = (B.y + 2) - smoothEye, dz = B.z - P.z, dh = Math.hypot(dx, dz), d3 = Math.hypot(dx, dy, dz);
        if (!best || d3 < best.d3) best = { wk, dh: Math.round(dh), d3: Math.round(d3), dot: +((dx * vx + dy * vy + dz * vz) / d3).toFixed(2), by: Math.round(B.y) }; }
      return { player: [Math.round(P.x), Math.round(smoothEye), Math.round(P.z)], nearest: best }; },
    mamBox(it0) { const it = itemsRef && itemsRef[(it0 | 0) - 1]; if (!it || !it.cells) return null;   // the OCCUPIED extent of a model, which is not its box: a .vox frame is padded, and seating a body on the padding is how it ends up hovering
      let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9, z0 = 1e9, z1 = -1e9;
      for (let z = 0; z < it.h; z++) for (let y = 0; y < it.d; y++) for (let x = 0; x < it.w; x++) {
        if (!it.cells[x + y * it.w + z * it.w * it.d]) continue;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; if (z < z0) z0 = z; if (z > z1) z1 = z; }
      return { w: it.w, d: it.d, h: it.h, x0, x1, y0, y1, z0, z1 }; },
    colTopId(x, z, yTop) {                           // WHICH voxel is the surface here — so a test can say whether an 'intrusion' is real ground or walk-through clutter the nav field deliberately ignores
      const bx = gwrap(Math.floor(x), WX), bz = gwrap(Math.floor(z), WZ) * WX * WY;
      for (let y = Math.min(WY - 1, Math.max(1, Math.round(yTop))); y >= 1; y--) {
        const id = W[bx + y * WX + bz];
        if (id && solidTab[id] === 1) return id; }
      return 0; },
    colTop(x, z, yTop) {                             // the TRUE surface a player sees: topmost solid voxel in W, +1 = first free voxel above it. Not hmap (blind to stamped rock/decor) and not the nav field (a 2x2 MAX)
      const bx = gwrap(Math.floor(x), WX), bz = gwrap(Math.floor(z), WZ);
      for (let y = Math.min(WY - 1, Math.max(1, Math.round(yTop))); y >= 1; y--) {
        const id = W[bx + y * WX + bz * WX * WY];
        if (id && solidTab[id] === 1) return y + 1; }
      return 1; },
    mamContact() { const out = [];                     // per land mammal: does its underside TOUCH the ground under it, and does any of that ground come up THROUGH it
      for (let j = MAM_0; j < MAM_END; j++) { const B = wbf[j];
        if (!B || !B.init || (B.kind | 0) !== 2 || !B.ragIt) continue;
        const m = this.mamBox(B.ragIt); if (!m) continue;
        const s = B.ragS || 1, hw = m.w * 0.5, hd = m.d * 0.5, hh = m.h * 0.5;
        const ax = B.ragX || [1, 0, 0], ay = B.ragY || [0, 0, 1];
        const feet = B.ragA1 + (m.z0 - hh) * s;        // world Y of the UNDERSIDE of the lowest occupied layer
        let gap = 1e9, intr = -1e9, worstId = 0;
        for (let u = 0; u <= 4; u++) for (let v = 0; v <= 2; v++) {   // a 5x3 grid over the OCCUPIED footprint — the whole underside, not two columns of it
          const lx = (m.x0 + (m.x1 + 1 - m.x0) * (v / 2) - hw) * s, ly = (m.y0 + (m.y1 + 1 - m.y0) * (u / 4) - hd) * s;
          const wx = B.ragA0 + ax[0] * lx + ay[0] * ly, wz = B.ragA2 + ax[2] * lx + ay[2] * ly;
          const g = this.colTop(wx, wz, feet + 6);
          if (feet - g < gap) gap = feet - g;
          if (g - feet > intr) { intr = g - feet; worstId = this.colTopId(wx, wz, feet + 6); } }
        // how STEEP is the ground under this body, and how fast is it crossing it — the two the user named
        let lo9 = 1e9, hi9 = -1e9;
        { const th9 = B.th || 0, sx9 = Math.sin(th9), cz9 = Math.cos(th9);
          const fq = j >= FLAM_0 ? MAMFIT.flam : j >= PORC_0 ? MAMFIT.porc : (j >= SKUNK_0 ? MAMFIT.skunk : (j >= ARM_0 ? MAMFIT.arm : MAMFIT.bunny));
          for (let u = -1; u <= 1; u++) for (let v = -1; v <= 1; v++) {
            const q = navWalkStand(B.x + sx9 * (fq ? fq.hd : 3) * u + cz9 * (fq ? fq.hw : 2) * v,
                                   B.z + cz9 * (fq ? fq.hd : 3) * u - sx9 * (fq ? fq.hw : 2) * v);
            if (q < lo9) lo9 = q; if (q > hi9) hi9 = q; } }
        const spd9 = B._mcX === undefined ? 0 : Math.hypot(B.x - B._mcX, B.z - B._mcZ) / Math.max(1e-3, (performance.now() - B._mcT) / 1000);
        B._mcX = B.x; B._mcZ = B.z; B._mcT = performance.now();
        out.push({ j, kind: j >= FLAM_0 ? 'flam' : j >= PORC_0 ? 'porc' : (j >= SKUNK_0 ? 'skunk' : (j >= ARM_0 ? 'arm' : 'bunny')),
          slope: +(hi9 - lo9).toFixed(2), spd: +spd9.toFixed(1),
          gap: +gap.toFixed(2),                        // smallest clearance anywhere under the body: 0 = resting on it, > 0 = AIRBORNE by that much, < 0 = sunk in
          intr: +intr.toFixed(2),                      // deepest the ground comes up through the model
          worstId, worstCol: palette[worstId] || null,
          traced: !!(LIFE_UNI && uniTraced(B)), d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)) }); }
      return out; },
    mammals() { const b = (a, z) => { let n = 0, near = 0; const pos = []; for (let j = a; j < z; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 2) { n++; pos.push([Math.round(O.x), Math.round(O.z), Math.round(O.hx || 0), Math.round(O.hz || 0)]); if ((O.x - P.x) ** 2 + (O.z - P.z) ** 2 < 400 * 400) near++; } } return { active: n, within400: near, pos }; };   /*TEMP-DEBUG: live land-mammal census + positions/homes*/
      return { bunny: b(BUNNY_0, BUNNY_END), armadillo: b(ARM_0, ARM_END), skunk: b(SKUNK_0, SKUNK_END), porcupine: b(PORC_0, FLAM_0), flamingo: b(FLAM_0, FLAM_END), p: [Math.round(P.x), Math.round(P.z)] }; },
    edState() { return { on: ED.on, n: ED.frames.length, sel: ED.sel, paused: ED.paused, order: ED.frames.map((f) => f.name), y: ED.y, x0: ED.x0, z0: ED.z0, pw: ED.pw, pd: ED.pd, pal: palette.length }; },
    dbg() { return { smoothEye, pickRock: [...PICK_ROCK], passthru: [...PASSTHRU] }; },
    htest(n) { let bad = 0; for (let i = 0; i < (n || 1000); i++) { const x = ((Math.random() * 2e6) | 0) - 1e6, z = ((Math.random() * 2e6) | 0) - 1e6;
      const hv = H(x, z); if (hv !== makeHRow(z)(x) || hv !== makeHCol(x)(z)) bad++; } return bad; },
    gtest(n) { const s = n || 64, x0 = Math.round(P.x / 8) * 8 - (s >> 1), z0 = Math.round(P.z / 8) * 8 - (s >> 1), x1 = x0 + s, z1 = z0 + s;   // pool-vs-inline: re-run the inline sweep over pool-built terrain — 0 diffs = bit-exact (only valid on unedited ground)
      const idx = (x, y, z) => gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY;
      const snap = new Uint8Array(s * WY * s), snapH = new Int16Array(s * s);
      for (let z = z0; z < z1; z++) for (let y = 0; y < WY; y++) for (let x = x0; x < x1; x++) snap[(x - x0) + y * s + (z - z0) * s * WY] = W[idx(x, y, z)];
      for (let z = z0; z < z1; z++) for (let x = x0; x < x1; x++) snapH[(x - x0) + (z - z0) * s] = hmap[gwrap(x, WX) + gwrap(z, WZ) * WX];
      genRegion(x0, x1, z0, z1, false);
      let bad = 0; const list = [];
      for (let z = z0; z < z1; z++) for (let y = 0; y < WY; y++) for (let x = x0; x < x1; x++) if (snap[(x - x0) + y * s + (z - z0) * s * WY] !== W[idx(x, y, z)]) { if (list.length < 12) list.push([x, y, z, snap[(x - x0) + y * s + (z - z0) * s * WY], W[idx(x, y, z)]]); bad++; }
      for (let z = z0; z < z1; z++) for (let x = x0; x < x1; x++) if (snapH[(x - x0) + (z - z0) * s] !== hmap[gwrap(x, WX) + gwrap(z, WZ) * WX]) { if (list.length < 12) list.push(['h', x, z, snapH[(x - x0) + (z - z0) * s], hmap[gwrap(x, WX) + gwrap(z, WZ) * WX]]); bad++; }
      for (let z = z0; z < z1; z++) for (let y = 0; y < WY; y++) for (let x = x0; x < x1; x++) W[idx(x, y, z)] = snap[(x - x0) + y * s + (z - z0) * s * WY];   // non-destructive — put the original content back
      for (let z = z0; z < z1; z++) for (let x = x0; x < x1; x++) hmap[gwrap(x, WX) + gwrap(z, WZ) * WX] = snapH[(x - x0) + (z - z0) * s];
      return bad ? { bad, list } : 0; },
    btest(n) { const s = n || 128, x0 = Math.round(P.x / 8) * 8 - (s >> 1), z0 = Math.round(P.z / 8) * 8 - (s >> 1);   // occupancy-vs-W: recompute brick + L2 occupancy from W and compare with the live arrays
      let bad = 0; const list = [];
      for (let bz = gwrap(z0, WZ) >> 3, nz = 0; nz < s >> 3; nz++, bz = (bz + 1) % BZ) for (let bx = gwrap(x0, WX) >> 3, nx = 0; nx < s >> 3; nx++, bx = (bx + 1) % BX)
        for (let by = 0; by < BY; by++) {
          let occ = 0;
          scan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) for (let x = bx * 8; x < bx * 8 + 8; x++) if (W[x + y * WX + z * WX * WY]) { occ = 1; break scan; }
          const b = bx + by * BX + bz * BX * BY;
          const have = (bricks[b >> 5] >>> (b & 31)) & 1;
          const c = (bx >> 2) + (by >> 2) * B2X + (bz >> 2) * B2X * B2Y;
          const have2 = (bricks2[c >> 5] >>> (c & 31)) & 1;
          if (have !== occ || (occ && !have2)) { bad++; if (list.length < 12) list.push([bx * 8, by * 8, bz * 8, occ, have, have2]); }
        }
      return bad ? { bad, list } : 0; },
    pick(o) { const c = heldCfg(heldIt() || 1); Object.assign(c, o || {}); pickSave(); return JSON.stringify(c); },
    cave() { let best = null, bd = 1e18; for (let jz = -4; jz <= 4; jz++) for (let jx = -4; jx <= 4; jx++) {
      const c = caveAt(Math.floor(P.x / CAVE_CELL) + jx, Math.floor(P.z / CAVE_CELL) + jz); if (!c) continue;
      const d = (c.sx - P.x) ** 2 + (c.sz - P.z) ** 2; if (d < bd) { bd = d; best = c; } } return best; },
    water() { for (let r = 8; r < 360; r += 8) for (let a = 0; a < 6.28; a += 0.4) { const x = Math.round(P.x + Math.cos(a) * r), z = Math.round(P.z + Math.sin(a) * r); const hy = hmap[gwrap(x, WX) + gwrap(z, WZ) * WX]; if (waterAt(x, hy, z) || waterAt(x, hy + 1, z)) return [x, hy, z]; } return null; },
    peaks(minProm) { const out = []; const prom = minProm || 16;   // columns rising sharply above their surroundings = crags
      for (let gz = 32; gz < WZ - 32; gz += 4) for (let gx = 32; gx < WX - 32; gx += 4) {
        const h = hmap[gx + gz * WX];
        const around = Math.min(hmap[gx - 28 + gz * WX], hmap[gx + 28 + gz * WX], hmap[gx + (gz - 28) * WX], hmap[gx + (gz + 28) * WX]);
        if (h - around >= prom) out.push([winOX + ((gx - gwrap(winOX, WX) + WX) % WX), h, winOZ + ((gz - gwrap(winOZ, WZ) + WZ) % WZ)]);
      } return out.slice(0, 12); } };
  console.log('[vb] ready — endless window', WX, WY, WZ, 'bricks', BX, BY, BZ);

