  window.__vb = { P, tp(x, y, z, yaw, pitch) { P.x = x; P.y = y; P.z = z; if (yaw !== undefined) P.yaw = yaw; if (pitch !== undefined) P.pitch = pitch;
      maybeRecenter();                                 // recenter FIRST — the pop-out below must read fresh terrain, not stale wrapped data
      while (P.y < WY - 20 && !boxFree(P.x, P.y, P.z, HEIGHT) && !waterAt(Math.floor(P.x), Math.floor(P.y + 8), Math.floor(P.z))) P.y += 1;
      P.vy = 0; smoothEye = P.y + EYE; resetHist = 1; }, fly() { P.fly = true; }, tod(t) { tday = t; resetHist = 1; }, give() { addItem(2); }, giveIt(id) { const k = addItem(id | 0); if (k >= 0) selSlot = k; return { held: heldIt(), knifeId: KNIFE_IT }; },   // …and SELECT it: addItem only fills a slot, and a knife sitting unselected in the hotbar still swings the axe   // put a specific item in hand (tests: the knife's two-hit kill needs the knife actually held)
    palLen() { return { len: palette.length, over: palette.length > 256 }; },
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
      return { len: palette.length, free: 256 - palette.length, over: palOver, groups: dup.length,
        wasted: dup.reduce((n, d) => n + d.ids.length - 1, 0), reclaimable: safe.length,
        buckets: bucket.size, near, safe, dup }; },
    uniInfo() { return { on: LIFE_UNI, visW: VIS_W, sec: UF[1530], blue: BLUEB_ITEM0, robin: ROBIN_ITEM0, birdsDrawn: uniBirdN, birdsWant: uniBirdWant, cursor: UF[1103] }; },
    itemInfo() { return { n: itemsRef ? itemsRef.length : 0, cells: itemMapF32.length >> 2, card: CARD_ITEM0, bunny: BUNNY_ITEM0, arm: ARMADILLO_ITEM0, armN: ARMADILLO_NFRAMES, skunk: SKUNK_ITEM0, skunkN: SKUNK_NFRAMES, porc: PORCUPINE_ITEM0, porcN: PORCUPINE_NFRAMES, worm: WORM_ITEM0 }; },   // item-table census for the unification tests
   // the 8-bit palette ceiling — breaching it corrupts voxel SOLIDITY, not just colour
    hurtInfo() { return { flash: +UF[1871].toFixed(3), world: [UF[1868] + winOX, UF[1869], UF[1870] + winOZ], half: [UF[1872], UF[1873], UF[1874]], dyn: UF[1875], slot: HURT.slot }; },   // dyn = the dynamic-life slot the wounded animal is DRAWN in (0 = grid-stamped, matched by bounds instead)   // the knife hit-flash box the tracer is tinting inside right now
    swing(offMs) { swingStart = performance.now() - (offMs || 0); }, drop() { dropHeld(); },
    sel(i) { selSlot = Math.max(0, Math.min(slots.length - 1, i | 0)); },
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
      return { snowOn, endIn: Math.round(snowEndT - t), nextIn: Math.round(snowNextT - t) }; }, wormPos() { const o = []; for (let j = 32; j < 64; j++) { const B = wbf[j];
      if (B && B.init) o.push({ j, x: +B.x.toFixed(2), z: +B.z.toFixed(2), trap: +(B.trap || 0).toFixed(2) }); } return o; },   // worm stuck-test tap
    flyers() { const o = []; for (let j = 0; j < 16; j++) { const B = wbf[j];
      if (B && B.init) o.push({ j, kind: B.kind | 0, x: +B.x.toFixed(2), z: +B.z.toFixed(2), hx: B.hx, hz: B.hz, hcx: B.hcx, hcz: B.hcz }); } return o; },   // flyer test tap
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
      for (let j = 244; j < 276; j++) { const B = wbf[j]; if (B && B.init) o.push({ j, sp: (FISHES[B.fsp || 0] || {}).name, sch: B.school, x: +B.x.toFixed(1), y: +B.y.toFixed(1), z: +B.z.toFixed(1), spd: +(B.spd || 0).toFixed(1), trap: +(B.trap || 0).toFixed(1), inSolid: bodyClip(B), d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)), air: B.jumpV !== undefined, arm: B.jumpArm !== undefined, flee: ((B.fleeT || 0) * 1000 > performance.now()) || spooked(B), vyS: +(B.vyS || 0).toFixed(1) }); }
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
                           setDone: anthemDone, playing: cur >= 0 ? ANTHEM_SET[cur][0] : null,
                           t: cur >= 0 ? +anthemSnds[cur].currentTime.toFixed(2) : 0,
                           tracks: ANTHEM_SET.map(([n9, g9], i9) => ({ name: n9, gain: g9, vol: +anthemSnds[i9].volume.toFixed(4), playing: !anthemSnds[i9].paused })),
                           bus: 'mus' }; },
    vig(v) { if (v !== undefined) { cineBlurK = v; blurLock = true; } return { uniform: UF[1268], on: vigOn, cineWeight: cineBlurK }; },   // cinematic vignette tap: read the live weight, or PIN it (blurLock) to A/B the effect at a fixed strength
    vigFree() { blurLock = false; }, get swingStart() { return swingStart; }, get locked() { return locked; }, edFrameBox() { const n = ED.frames.length; if (!n) return null; const f = ED.frames[((ED.sel % n) + n) % n]; return { bx: f.bx, bz: f.bz, sx: f.sx, sy: f.sy, sz: f.sz, y: ED.y }; },   // test taps
    solidAt(x, y, z) { const id = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY]; return !!(id && solidTab[id]); },
    solidAt2(x, y, z) { return solid(x, y, z); },      // what the PLAYER actually collides with (cones exempted) — solidAt reports the raw table
    boxFreeAt(x, y, z) { return boxFree(x, y, z, HEIGHT); },   // can the player's whole box sit here?
    lifeUidAt(s) { return lifeUid[s | 0]; },           // which pool slot is DRAWN in drop slot s (2000+slot), or -1
    lifeBudget() { return { base: 25, total: DROP_SLOTS, drawn: lifeUid.filter((u) => u >= 2000).length }; },
    // ── WHO IS ALIVE, AND WHO IS ACTUALLY ON SCREEN ── per kind. The gap between `active` and `drawn` is the
    // slot famine: the trace-injected creatures share whatever is left of the 64 drop slots after the flying
    // songbirds, and the surplus is dropped by DISTANCE ALONE, so a dense near kind can starve a far one out
    // of the frame entirely. dHiddenMin is the closest creature of that kind that did NOT get drawn — the
    // number that tells you a duck 60 voxels away is invisible. Grid-stamped kinds live in W, not in a slot,
    // so they report drawn: -1 rather than a misleading 0.
    lifeCensus() {
      const STAMPED = LIFE_UNI ? { cardinal: 1 } : { cardinal: 1, bunny: 1, armadillo: 1, skunk: 1, porcupine: 1 };   // under ?uni none of the five is stamped any more - they are in the drop-slot ledger like every other creature, so report their real drawn/hidden counts instead of the -1 placeholder
      const bandOf = (wk) => wk < 16 ? 'flyer' : wk < 20 ? 'duckMom' : wk < 32 ? 'duckling' : wk < 64 ? 'worm'
        : wk < 244 ? 'cardinal' : wk < 276 ? 'fish' : wk < 300 ? 'bunny' : wk < 324 ? 'armadillo' : wk < 348 ? 'skunk' : 'porcupine';
      const shown = new Set();
      for (let s = 0; s < DROP_SLOTS; s++) { const u = lifeUid[s]; if (u >= 2000) shown.add(u - 2000); }
      const acc = {};
      for (let wk = 0; wk < 372; wk++) { const B = wbf[wk];
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
      return { slots: { firstCreature: 25, flock: BIRD_N - 1, flockDrawn: BIRD_SLOTS, firstFree: 25 + BIRD_SLOTS, total: DROP_SLOTS, forTraced: DROP_SLOTS - (25 + BIRD_SLOTS) }, kinds: acc };
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
      for (let j = 244; j < 276; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 6) continue;
        o.push({ j, x: +B.x.toFixed(1), y: +B.y.toFixed(1), z: +B.z.toFixed(1), th: +(B.th || 0).toFixed(2),
          spd: +(B.spd || 0).toFixed(1), bk: !!B.bkOn, tight: !!B.bkTight, flee: !!B.fleeing,
          reach: +(B.navReach === undefined ? -1 : B.navReach).toFixed(1),
          rep: +Math.hypot(B.repX || 0, B.repZ || 0).toFixed(1),
          trap: +(B.trap || 0).toFixed(2), air: B.jumpV !== undefined,
          d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)) }); }
      return o; },
    mamSeatCmp() { const o = [];                     // the OLD stamped seat (one heightmap column) vs the ONE seat both paths use now — the gap is the correction
      for (let j = 276; j < 372; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const fit = j >= 348 ? MAMFIT.porc : (j >= 324 ? MAMFIT.skunk : (j >= 300 ? MAMFIT.arm : MAMFIT.bunny));
        o.push({ j, kind: j >= 348 ? 'porc' : (j >= 324 ? 'skunk' : (j >= 300 ? 'arm' : 'bunny')),
          oldG: Math.round(__vb.bfSurf(B.x, B.z)), newG: +mamSeatG(B, fit).toFixed(2),
          navCtr: +navWalkStand(B.x, B.z).toFixed(2), surfCtr: +__vb.bfSurf(B.x, B.z).toFixed(2) }); }
      return o; },
    sndRouting() {                                   // which registered sounds are routed through an effect, and whether the RECORDER could tap them
      const out = sndReg.map((s) => ({ src: (s.a.src || '').split('/').slice(-2).join('/'),
        fx: !!s.a._sfxOut, veTapped: !!s.a._veTap, vol: +s.a.volume.toFixed(3) }));
      return { sounds: out.filter((o) => o.fx || o.src.indexOf('hit.mp4') >= 0), sharedCtx: !!sfxAC, veCtxIsShared: VE.ac === null || VE.ac === sfxAC }; },
    veTapAll() { const d = veAudioDest(); for (const s of sndReg) veTapEl(s.a);   // force the recorder's tap exactly as starting a capture does
      return { dest: !!d, tapped: sndReg.filter((s) => s.a._veTap).length, total: sndReg.length,
        fxTapped: sndReg.filter((s) => s.a._sfxOut && s.a._veTap).length, fxTotal: sndReg.filter((s) => s.a._sfxOut).length }; },
    mamSeatProbe() { const o = [];                   // PAIRED density comparison on one pose: how much ground does each resolution MISS versus a fine grid?
      for (let j = 276; j < 372; j++) { const B = wbf[j]; if (!B || !B.init || (B.kind | 0) !== 2) continue;
        const fit = j >= 348 ? MAMFIT.porc : (j >= 324 ? MAMFIT.skunk : (j >= 300 ? MAMFIT.arm : MAMFIT.bunny));
        const st = mamSeatSteps(fit);
        o.push({ kind: j >= 348 ? 'porc' : (j >= 324 ? 'skunk' : (j >= 300 ? 'arm' : 'bunny')),
          steps: st, s3: mamSeatN(B, fit, 1, 1), sD: mamSeatN(B, fit, st[0], st[1]), sT: mamSeatN(B, fit, 6, 6) }); }
      return o; },
    mushAt, MUCELL, boulderAt, BCELL,                // …and the mushroom and boulder placers, for the same reason: a floater test has to be able to stand in front of one
    logAt, LGCELL,                             // the generator's own fallen-log placement, so a test can drive straight to one instead of hunting the forest for a 14%-per-96m candidate
    logIds() { if (!LOGV) return null;              // which palette ids the ground log is built from, and whether each is one the PINE TRUNK uses
      const ids = [...new Set(LOGV.vox.map((p) => p >>> 24))];
      return ids.map((i) => ({ id: i, col: palette[i], isTrunkBark: woodIds.indexOf(i) >= 0, wood: !!woodTab[i] })); },
    barkIds() { return woodIds.map((i) => ({ id: i, col: palette[i] })); },
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
    // the full chop signature, so a test can vary ONE parameter at a time — the material filter in particular,
    // which decides whether the flood is asked about a tree that still has all its needles or one it does not.
    physChopFull(x, y, z, rad, minBite, bite, woodOnly) {
      const S = treeShapeAt(x | 0, z | 0); if (!S) return { hit: false, why: 'no pine here' };
      return physChopAt(x | 0, y | 0, z | 0, rad, S, minBite, bite, woodOnly ? ((v) => !!woodTab[v]) : undefined);
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
    eat() { return tryEat(); },                      // one bite, exactly what a right-click does
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
      if (mouse2 && !was) { bowT0 = performance.now(); if (BOW_IT && heldIt() === BOW_IT) playBowStretch(); }
      else if (!mouse2 && was) { bowRel = performance.now();
        if (BOW_IT && heldIt() === BOW_IT) stopBowStretch();
        if (BOW_IT && heldIt() === BOW_IT && (bowRel - bowT0) > BOW_DRAW_MS * 0.5) { bowLoosed = true; shootArrow(); playSwish(); }
        else if (SPEAR_IT && heldIt() === SPEAR_IT && (bowRel - bowT0) > 90) throwSpear(); }   // …and the SPEAR throw, so a test drives exactly what the mouse does   // …including LOOSING the arrow
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
      for (let j = 0; j < 372; j++) { const B = wbf[j]; if (!B || !B.init) continue;
        o.push({ j, kind: B.kind | 0, x: +B.x.toFixed(1), y: +((B.kind|0)===5 ? (B.perchFeet||0) : B.y).toFixed(1), z: +B.z.toFixed(1),
                 stamped: B.sN | 0 }); }
      return o;
    },
    decorIds() { const o = []; for (let i = 0; i < 256; i++) if (decorTab[i]) o.push(i); return o; },   // palette ids the axe can carve chunks from
    axeOnlyIds() { const o = []; for (let i = 0; i < 256; i++) if (axeOnlyTab[i]) o.push(i); return o; },   // …of those, the ones that need the AXE (wood)
    ducks() { const o = []; for (let j = 16; j < 32; j++) { const B = wbf[j]; if (B && B.init && (B.kind | 0) === 3) o.push({ j, x: +B.x.toFixed(2), z: +B.z.toFixed(2), th: +(B.th || 0).toFixed(3), om: +(B.om || 0).toFixed(3), turnAcc: +((B.turnAcc || 0)).toFixed(2) }); } return o; },   // duck slots — circling shows up as turnAcc growing without bound
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
      const seen = new Set(), out = [];
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
        const key = ((wx / 12) | 0) + ',' + ((wz / 12) | 0);   // cluster columns of the same trunk together
        if (seen.has(key)) { const e = out.find((q) => q.key === key); if (e) { e.cols++; if (drop > e.drop) e.drop = drop; } continue; }
        seen.add(key); out.push({ key, cols: 1, drop, base: lo, at: [wx, lo, wz] });
      }
      out.sort((a, b) => b.drop - a.drop);
      return { floating: out.length, worstDrop: out.length ? out[0].drop : 0, top: out.slice(0, 8) };
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
    lgt2(m) { if (m !== undefined) { lgtMask2 = m | 0; resetHist = 1; lgtPaint(); } return { mask2: lgtMask2, all2: LGT2_ALL, terms2: {} }; },   // the SECOND mask (u.lgt.z) — currently EMPTY (see LGT2_ALL); left wired so a 25th term has somewhere to go
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
      const R = r || 6000, t = [0, 0, 0]; let pines = 0, birds = 0;
      const c0x = Math.floor((wx === undefined ? P.x : wx) / TCELL), c0z = Math.floor((wz === undefined ? P.z : wz) / TCELL), n = Math.ceil(R / TCELL);
      for (let dz = -n; dz <= n; dz++) for (let dx = -n; dx <= n; dx++) {
        const tr = treeAt(c0x + dx, c0z + dz); if (!tr) continue;
        pines++;
        const k = birdsOnPine(tr.tx, tr.tz);
        for (let i = 0; i < k; i++) { t[birdColour(tr.tx, tr.tz, i)]++; birds++; }
      }
      return { radius: R, pines, birds, cardinal: t[0], blue: t[1], robin: t[2],
        pct: t.map((x) => +(x * 100 / Math.max(1, birds)).toFixed(1)) };
    },
    compass() { return { on: cmpOn, hidden: $('compass').classList.contains('hidden'), stored: localStorage.getItem('vb_cmp') }; },   // compass-toggle tap                // palette occupancy tap — edCol silently NEAREST-MATCHES once this hits 256, which would quietly wash a reskin's colours into another bird's
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
    birdBox() { return { ...birdBox }; }, wormStamps() { let n = 0, live = 0, off = 0; for (let j = 32; j < 64; j++) { const B = wbf[j]; if (B && B.sN) { n += B.sN; live++; } if (B && B.init && (B.kind | 0) === 2) off++; } return { voxels: n, worms: live, live: off }; },   // Task 2/3 taps — `live` = active off-grid worms (worms render off-grid now)
    duckStamps() { let n = 0, live = 0, paused = 0; for (let j = 16; j < 32; j++) { const B = wbf[j]; if (B && B.sN) { n += B.sN; live++; } } for (let j = 32; j < 64; j++) { const B = wbf[j]; if (B && B.wpause) paused++; } return { duckVoxels: n, ducks: live, wormsPaused: paused }; },   // Task 6 + worm-pause taps
    perched() { const o = []; for (let j = 64; j < 244; j++) { const B = wbf[j]; if (B && B.init && (B.kind | 0) === 5) { const fx = Math.floor(B.x), fz = Math.floor(B.z), fy = Math.round(B.perchFeet || 0); o.push({ j, x: fx, z: fz, feetY: fy, bird: B.bird | 0, below: this.vox(fx, fy - 1, fz), cells: B.sN | 0, want: B.sWant | 0, ids: B.sN ? [...new Set(Array.from(B.sCells.subarray(0, B.sN), (ii) => W[ii]))].sort((p2, q2) => p2 - q2) : [] }); } } return o; },   // perched-cardinal test tap
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
    crying() { const o = []; for (let j = 20; j < 32; j++) { const B = wbf[j];
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
    hitsOn(s) { const B = wbf[s | 0]; return B ? { hits: B.hits | 0, needs: HITS_TO_KILL, alive: !!B.init, dying: !!B.dying } : null; },   // how many hits this creature has taken, for checking the rule in play   // run the hit on a KNOWN creature, no aiming involved
    hurtTest(slot, hold) { const B = wbf[slot | 0]; if (!B || !B.init) return null; HURT.slot = slot | 0; HURT.hold = !!hold; hurtBox(B); HURT.t0 = performance.now(); return __vb.hurtInfo(); },   // arm the wounded flash WITHOUT the hit, so a capture can be timed against it
    aimed() { return aimedCreature(); },              // which life slot the crosshair is actually on (-1 = none) — a swing test has to confirm it is ON target before it can read anything into the result
    kill() { tryKillCreature(); return sparks3d.map((s) => s ? (s.smoke ? 'smoke' : 'spark') : null); },   /*TEMP-DEBUG: force a kill-attempt from the current camera*/
    testBurst() { const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch), fx = Math.sin(P.yaw) * cp, fy = sp, fz = Math.cos(P.yaw) * cp; spawnDeathBurst(P.x + fx * 12, smoothEye + fy * 12 - 2, P.z + fz * 12); return true; },   /*TEMP-DEBUG: fire a death poof 12 vox ahead of the camera*/
    killProbe() { const cp = Math.cos(P.pitch), sp = Math.sin(P.pitch), vx = Math.sin(P.yaw) * cp, vy = sp, vz = Math.cos(P.yaw) * cp; let best = null;   /*TEMP-DEBUG*/
      for (let wk = 0; wk < 372; wk++) { const B = wbf[wk]; if (!B || !B.init) continue; const dx = B.x - P.x, dy = (B.y + 2) - smoothEye, dz = B.z - P.z, dh = Math.hypot(dx, dz), d3 = Math.hypot(dx, dy, dz);
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
      for (let j = 276; j < 372; j++) { const B = wbf[j];
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
          const fq = j >= 348 ? MAMFIT.porc : (j >= 324 ? MAMFIT.skunk : (j >= 300 ? MAMFIT.arm : MAMFIT.bunny));
          for (let u = -1; u <= 1; u++) for (let v = -1; v <= 1; v++) {
            const q = navWalkStand(B.x + sx9 * (fq ? fq.hd : 3) * u + cz9 * (fq ? fq.hw : 2) * v,
                                   B.z + cz9 * (fq ? fq.hd : 3) * u - sx9 * (fq ? fq.hw : 2) * v);
            if (q < lo9) lo9 = q; if (q > hi9) hi9 = q; } }
        const spd9 = B._mcX === undefined ? 0 : Math.hypot(B.x - B._mcX, B.z - B._mcZ) / Math.max(1e-3, (performance.now() - B._mcT) / 1000);
        B._mcX = B.x; B._mcZ = B.z; B._mcT = performance.now();
        out.push({ j, kind: j >= 348 ? 'porc' : (j >= 324 ? 'skunk' : (j >= 300 ? 'arm' : 'bunny')),
          slope: +(hi9 - lo9).toFixed(2), spd: +spd9.toFixed(1),
          gap: +gap.toFixed(2),                        // smallest clearance anywhere under the body: 0 = resting on it, > 0 = AIRBORNE by that much, < 0 = sunk in
          intr: +intr.toFixed(2),                      // deepest the ground comes up through the model
          worstId, worstCol: palette[worstId] || null,
          traced: !!(LIFE_UNI && uniTraced(B)), d: Math.round(Math.hypot(B.x - P.x, B.z - P.z)) }); }
      return out; },
    mammals() { const b = (a, z) => { let n = 0, near = 0; const pos = []; for (let j = a; j < z; j++) { const O = wbf[j]; if (O && O.init && (O.kind | 0) === 2) { n++; pos.push([Math.round(O.x), Math.round(O.z), Math.round(O.hx || 0), Math.round(O.hz || 0)]); if ((O.x - P.x) ** 2 + (O.z - P.z) ** 2 < 400 * 400) near++; } } return { active: n, within400: near, pos }; };   /*TEMP-DEBUG: live land-mammal census + positions/homes*/
      return { bunny: b(276, 300), armadillo: b(300, 324), skunk: b(324, 348), porcupine: b(348, 372), p: [Math.round(P.x), Math.round(P.z)] }; },
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

