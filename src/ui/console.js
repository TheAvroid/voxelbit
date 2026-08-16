  // @module — the in-game command line (T)
  // @exports CMD, cmdMsg, cmdRun, cmdShow
  // ══ COMMAND LINE (user) ══ T opens it, Enter runs, Escape cancels. `/spawn <thing>` puts a creature
  // or an item in front of the player. While it is open the game's own keyboard is silenced — the binds
  // read raw key codes, so without that, typing "spawn" would drop the held item on the d and toggle
  // fly on the f.
  const CMD_SPECIES = {                                // name → [slot band lo, hi, B.kind]. The bands are the creature pool's own layout.
    butterfly: [0, 16, 0], moth: [0, 16, 1], duck: [16, 20, 3], worm: [32, 64, 2],
    fish: [244, 276, 6], bunny: [276, 300, 2], armadillo: [300, 324, 2], skunk: [324, 348, 2], porcupine: [348, 372, 2],
  };
  // Fields on a bag, NOT two exported `let`s: a module exports a const snapshot taken at
  // module-init, so `CMD.open` read from outside would have been frozen `false` for ever and
  // ui/input.js would have kept feeding the game keystrokes while the command line was open.
  const CMD = { open: false, escAt: -1e9 };                                 // when Escape last dismissed the line — see the esc-menu suppression below
  let cmdRelock = 0;                                   // the retry that takes the pointer back once the browser will allow it
  const cmdBar = $('cmdBar'), cmdTxt = $('cmdTxt'), cmdMsg = $('cmdMsg');
  let cmdBuf = '';                                     // the line's own text: with the pointer still locked there is nothing to focus, so it keeps its own buffer
  const cmdDraw = () => { if (cmdTxt) cmdTxt.textContent = cmdBuf; };
  const cmdSay = (t) => { if (cmdMsg) { cmdMsg.textContent = t; clearTimeout(cmdSay.t); cmdSay.t = setTimeout(() => { if (cmdMsg) cmdMsg.textContent = ''; }, 4000); } };
  const cmdShow = (on) => {
    CMD.open = !!on;
    if (!cmdBar) return;
    cmdBar.classList.toggle('hidden', !CMD.open);
    if (CMD.open) {
      keys.clear();                                  // whatever was held when it opened must not stay held
      cmdBuf = '/';                                  // just the slash (user) — the command is typed, not assumed
      cmdDraw();
      // The pointer stays LOCKED. Releasing it is what shook the view (user): the browser restores the OS
      // cursor and lays the canvas out around its own lock notice. Keystrokes come from the capture-phase
      // handler below instead, so nothing needs focus.
    } else {
      // Only the Escape path can have cost us the lock — the browser exits it on Escape whatever we do —
      // so ask for it back, and again once its post-Escape cooldown has lapsed.
      if (!locked && !dead) {
        tryLock();
        clearTimeout(cmdRelock);
        cmdRelock = setTimeout(() => { if (!locked && !dead && cmdBar.classList.contains('hidden')) tryLock(); }, 1350);
      }
    }
  };
  // where the thing lands: straight out along the view, on the ground, clear of the player
  const cmdSpot = () => {
    const d = 14, x = Math.round(P.x + Math.sin(P.yaw) * d), z = Math.round(P.z + Math.cos(P.yaw) * d);
    return { x, z, g: hmap[gwrap(x, WX) + gwrap(z, WZ) * WX] };
  };
  const cmdSpawn = (what) => {
    const key = String(what || '').trim().toLowerCase().replace(/\s+/g, '_');
    if (!key) { cmdSay('spawn what? try /spawn porcupine'); return; }
    const at = cmdSpot();
    // ── A CREATURE ──
    const sp = CMD_SPECIES[key];
    if (sp) {
      const [lo, hi, kind] = sp;
      let slot = -1;
      for (let j = lo; j < hi; j++) if (!wbf[j].init) { slot = j; break; }
      if (slot < 0) slot = lo;                         // all busy: take the band's first and recycle it
      const B = wbf[slot];
      B.x = at.x + 0.5; B.z = at.z + 0.5; B.gRef = Math.max(at.g, WL); B.y = B.gRef + 1;
      B.th = Math.random() * 6.283; B.om = 0; B.omT = 0; B.tRe = 0; B.trap = 0;
      B.born = performance.now(); B.kind = kind; B.dieT = 0; B.glow = false; B.glowT = 0;
      B.hurt = 0; B.hits = 0; B.dying = false; B.blinked = false; B.hopT0 = undefined; B.lastSwing = undefined;
      B.aframe = 0; B.ah = 0; B.rel = true;            // rel: lives past the population cap until it recycles naturally
      B.init = true;
      cmdSay('spawned ' + key + ' (slot ' + slot + ')');
      return;
    }
    // ── AN ITEM ── it arrives as a DROP, which is what "levitating in front of the player" already is.
    // The .vox FILE NAME works as well as the short name (user): /spawn stone_axe and /spawn axe both do.
    const CMD_FILES = { stone_axe: 1, stone_knife: KNIFE_IT, stone_pick: PICK_IT, stone_shovel: SHOVEL_IT,
      stone_hoe: HOE_IT, stone_spear: SPEAR_IT,
      bow: BOW_IT, base: BOW_IT, meat: MEAT_IT, raw_meat: MEAT_IT, rock: ROCK_IT, stick_1: STICK_IT,
      stick_2: STICK_IT, stick: STICK_IT, pinecone: CONE_IT, twig: STICK_IT };
    let it = CMD_FILES[key] || 0;
    if (!it) for (const k in ITEM_NAMES) { const nm = ITEM_NAMES[k]; if (nm && nm.toLowerCase().replace(/\s+/g, '_') === key) { it = +k; break; } }
    if (!it) { cmdSay('no such thing: ' + key); return; }
    drops.push({ x: at.x, y: Math.max(at.g, WL + 1), z: at.z, it, ph: Math.random() * 6.28, born: performance.now() });
    if (drops.length > 32) drops.shift();
    cmdSay('spawned ' + key + ' (item ' + it + ')');
  };
  // stand on the first DRY column stepping out from a water spot — a lake is somewhere to arrive AT, not in.
  // MODULE level, not inside the CMD_LOCATE closure it started in: /locate fish and /locate duck need exactly
  // this walk, and landing the player on the lake BED instead drowns them (tick-camera's DROWN_T).
  const cmdShore = (wx, wz) => {
    for (let r = 0; r <= 700; r += 2) for (let a = 0; a < 12; a++) {   // 700 clears even the biggest reservoir — a short walk would give up mid-lake and land the player in the water
      const x = Math.round(wx + Math.cos(a * 0.5236) * r), z = Math.round(wz + Math.sin(a * 0.5236) * r);
      if (H(x, z) > WL + 1 && !nearCave(x, z)) return { x, z };
    }
    return { x: Math.round(wx), z: Math.round(wz) };
  };
  // ══ /locate <thing> ══ walk the generator OUTWARD from the player's own cell until the thing turns up,
  // then stand the player next to it. Everything here is ANALYTIC — treeAt/boulderAt/riverAt/caveAt answer
  // for any cell in the infinite world without it being generated first — so the search is not limited to
  // what is loaded, and the arrival does a full recenter exactly as respawn() does.
  //
  // Each finder returns a WORLD COLUMN to stand at, or null. `cell` is the generator's own grid size, and
  // the ring walk visits cells nearest-first so the answer really is the closest one.
  const CMD_LOCATE = (() => {
    const nearestCell = (cell, rings, hit) => {          // spiral out over the generator's cell grid
      const c0x = Math.floor(P.x / cell), c0z = Math.floor(P.z / cell);
      for (let r = 0; r <= rings; r++) {
        let best = null, bd = Infinity;
        for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
          if (Math.max(Math.abs(dx), Math.abs(dz)) !== r) continue;   // ring surface only — the interior was covered by smaller r
          const got = hit(c0x + dx, c0z + dz); if (!got) continue;
          const d = (got.x - P.x) * (got.x - P.x) + (got.z - P.z) * (got.z - P.z);
          if (d < bd) { bd = d; best = got; }
        }
        if (best) return best;                           // nearest hit in the first ring that has one
      }
      return null;
    };
    // Is there really water here? Five samples, not one: the centre of a wide lake can sit on an island or on
    // a bar, and one dry reading would then throw away a perfectly good lake.
    const wetSpot = (x, z, r) => { const q = r * 0.5;
      return H(Math.round(x), Math.round(z)) < WL || (r > 0 && (H(Math.round(x + q), Math.round(z)) < WL
        || H(Math.round(x - q), Math.round(z)) < WL || H(Math.round(x), Math.round(z + q)) < WL
        || H(Math.round(x), Math.round(z - q)) < WL)); };
    return {
      // ── AND IT HAS TO STILL BE THERE ── riverAt is the watershed LAYOUT and knows nothing about biomes, but
      // the desert pass runs last and lifts every column with desertM > 0.5 to WL + 2, so a lake or a channel
      // that lands in the sand is erased by the generator and does not exist. H() is the height the world is
      // actually built to, desert override included, so `H < WL` is the exact question rather than a guess at a
      // desertM threshold. MEASURED before this: /locate lake from the sand landed at desertM 0.507 on a dry
      // dune, and /locate fish then watched an empty sky for its whole budget standing on it.
      lake: () => { const g = nearestCell(RIVCELL, 8, (cx, cz) => { const R = riverAt(cx, cz); if (!R) return null;
          let b = null; for (const L of R.lakes) if (L.r >= 120 && wetSpot(L.x, L.z, L.r) && (!b || L.r > b.r)) b = L;   // reservoirs/tail lakes — a headwater pond is not what "lake" means
          return b ? { x: b.x, z: b.z, r: b.r } : null; });
        return g ? { ...cmdShore(g.x, g.z), what: 'lake (r ' + Math.round(g.r) + ')' } : null; },
      river: () => { const g = nearestCell(RIVCELL, 8, (cx, cz) => { const R = riverAt(cx, cz); if (!R || !R.segs.length) return null;
          for (const s of R.segs) { const mx = s.sx + s.dxr * s.len * 0.5, mz = s.sz + s.dzr * s.len * 0.5;   // the first segment that is still WET: a stem can run out of the forest into the sand and lose its channel halfway along
            if (wetSpot(mx, mz, 0)) return { x: mx, z: mz }; }
          return null; });
        return g ? { ...cmdShore(g.x, g.z), what: 'river' } : null; },
      tree: () => { const g = nearestCell(TCELL, 40, (cx, cz) => { const t = treeAt(cx, cz); return t ? { x: t.tx, z: t.tz } : null; });
        return g ? { x: g.x + 12, z: g.z, what: 'pine' } : null; },
      rock: () => { const g = nearestCell(BCELL, 60, (cx, cz) => { const b = boulderAt(cx, cz); return b ? { x: b.bx, z: b.bz } : null; });
        return g ? { x: g.x + 8, z: g.z, what: 'rock' } : null; },
      log: () => { const g = nearestCell(LGCELL, 40, (cx, cz) => { const l = logAt(cx, cz); return l ? { x: l.wx, z: l.wz } : null; });
        return g ? { x: g.x + 6, z: g.z, what: 'fallen log' } : null; },
      // …a mushroom CLUSTER is 23 wide and only ever grows under a pine, so land clear of both: a short
      // standoff put the player inside the crown and the lift-out loop then walked them up the tree.
      mushroom: () => { const g = nearestCell(MUCELL, 40, (cx, cz) => { const m2 = mushAt(cx, cz); return m2 ? { x: m2.wx, z: m2.wz } : null; });
        return g ? { x: g.x + 16, z: g.z, what: 'mushrooms' } : null; },
      fern: () => { const g = nearestCell(F2CELL, 40, (cx, cz) => { const f = fern2At(cx, cz); return f ? { x: f.wx, z: f.wz } : null; });
        return g ? { x: g.x + 10, z: g.z, what: 'ferns' } : null; },
      // …the gorge is DRY by construction (caveAt rejects any path touching water), so no shore walk: drop
      // the player straight onto its floor — cmdGoTo's hmap read lands them at the bottom, which is the point.
      gorge: () => { const g = nearestCell(CAVE_CELL, 24, (cx, cz) => { const c2 = caveAt(cx, cz); return c2 ? { x: c2.sx, z: c2.sz } : null; });
        return g ? { x: g.x, z: g.z, what: 'gorge' } : null; },
      // ── THE TWO BIOMES ── no nearestCell walk: the border is a single wandering line in x (desert east,
      // forest west — see desertM), so marching along x at the player's own z finds it exactly and cheaply.
      // Step 16 at a time and stop DEEP inside, not at the first column that qualifies: landing on the border
      // itself puts you in the dithered blend where the sand is half forest floor, which is not "the desert".
      // Same landing test the spawn nudge uses (build.js) — dry ground, clear of a gorge.
      desert: () => biomeSeek((d) => d >= 0.995, 'the desert'),
      pine_forest: () => biomeSeek((d) => d <= 0.005, 'the pine forest'),
    };
  })();
  // Walk x outward from the player (the biome split is a line in x), both ways, and take the first column that
  // is deep inside the wanted biome AND is somewhere you can stand. 24000 voxels is far past the widest the
  // blend band can ever be, so failing to find one means something is wrong rather than "keep looking".
  const biomeSeek = (want, label) => {
    for (let r = 0; r <= 24000; r += 16) {
      for (const x of (r === 0 ? [P.x] : [P.x + r, P.x - r])) {
        const xi = Math.round(x), zi = Math.round(P.z);
        if (!want(desertM(xi, zi))) continue;
        if (H(xi, zi) <= WL + 1 || nearCave(xi, zi)) continue;
        return { x: xi, z: zi, what: label };
      }
    }
    return null;
  };
  const CMD_LOC_ALIAS = { water: 'lake', pond: 'lake', stream: 'river', forest: 'tree', boulder: 'rock',
    stone: 'rock', mushrooms: 'mushroom', ferns: 'fern', ravine: 'gorge', cave: 'gorge', canyon: 'gorge', logs: 'log', trees: 'tree', rocks: 'rock', lakes: 'lake',
    pine: 'tree', sand: 'desert', dunes: 'desert', pineforest: 'pine_forest', pines: 'pine_forest' };   // NOTE 'forest' stays pointed at 'tree' (nearest single pine) — it predates the biome and changing it would silently redirect an existing command
  // ══ /locate <creature> ══ the LIVE pool, not the generator. Every creature in the world is a wbf body sitting
  // right here, so the nearest one is a scan of a slot band and nothing more — no ring walk, no regen, instant.
  // A band is a [lo, hi) slice of wbf PLUS an optional per-body test, because a band is not always one species:
  // the flyer band is butterfly / dragonfly / firefly depending on B.kind and B.dfly, the fish band splits on
  // B.fsp and the perched songbirds on B.bird. `hab` is the habitat that species' own spawn gate admits
  // (tick-creatures) and is the whole of the fallback when none is alive.
  const CMD_LIFE_ALIAS = { mouse: 'desert_mouse', desertmouse: 'desert_mouse', mice: 'desert_mouse', rabbit: 'bunny',
    hare: 'bunny', bunnies: 'bunny', moth: 'firefly', lightningbug: 'firefly', bluebird: 'blue_bird',
    bird: 'songbird', birds: 'songbird', songbirds: 'songbird', flies: 'fly', butterflies: 'butterfly',
    dragonflies: 'dragonfly', fireflies: 'firefly', baby_duck: 'duckling', ducklings: 'duckling', bluegill: 'blue_gill' };
  // Built FRESH on every call, deliberately. DESERTS and FISHES are filled by an async loader long after this
  // fragment is evaluated, and sim/life/slots.js — which owns wbf, MAM_END and DES_PER — is concatenated BELOW
  // ui/console.js, so anything read at module-init here would be a TDZ throw or an empty list frozen for the
  // session. A command line runs at human speed; rebuilding a 25-key object costs nothing and can never go stale.
  const CMD_LIFE = () => {
    const T = {
      butterfly: { lo: 0, hi: 16, hab: 'forest', when: 'in daylight', ok: (B) => (B.kind | 0) === 0 && !B.dfly },
      dragonfly: { lo: 0, hi: 16, hab: 'water', when: 'in daylight, over water', ok: (B) => (B.kind | 0) === 0 && !!B.dfly },
      firefly: { lo: 0, hi: 16, hab: 'forest', when: 'after dark', ok: (B) => (B.kind | 0) === 1 },
      duck: { lo: 16, hi: 20, hab: 'water', shore: 1 },
      duckling: { lo: 20, hi: 32, hab: 'water', shore: 1 },
      worm: { lo: 32, hi: 64, hab: 'forest' },
      songbird: { lo: 64, hi: 244, hab: 'forest', off: 11 },
      cardinal: { lo: 64, hi: 244, hab: 'forest', off: 11, ok: (B) => (B.bird | 0) === 0 },
      blue_bird: { lo: 64, hi: 244, hab: 'forest', off: 11, ok: (B) => (B.bird | 0) === 1 },
      robin: { lo: 64, hi: 244, hab: 'forest', off: 11, ok: (B) => (B.bird | 0) === 2 },
      fish: { lo: 244, hi: 276, hab: 'water', shore: 1 },
      bunny: { lo: 276, hi: 300, hab: 'forest' },
      armadillo: { lo: 300, hi: 324, hab: 'forest' },
      skunk: { lo: 324, hi: 348, hab: 'forest' },
      porcupine: { lo: 348, hi: 372, hab: 'forest' },
    };
    for (let i = 0; i < FISHES.length; i++) T[FISHES[i].name] = { lo: 244, hi: 276, hab: 'water', shore: 1, ok: ((k) => (B) => (B.fsp | 0) === k)(i) };   // species is fixed by SLOT (B.fsp), so the name has to key off the LOADED order rather than a literal
    for (let i = 0; i < DESERTS.length; i++) { const nm = DESERTS[i].name;   // seven contiguous DES_PER blocks in load order — read off DESERTS so re-ordering the loader cannot silently rename a band
      T[nm] = { lo: MAM_END + i * DES_PER, hi: MAM_END + (i + 1) * DES_PER, hab: 'desert', line: nm === 'ant', off: nm === 'ant' ? 12 : 6 }; }
    return T;
  };
  const cmdLifeOf = (key) => {                         // name -> band, forgiving about spelling: exact, then alias, then a trailing plural
    const T = CMD_LIFE();
    let k = key;
    if (!T[k] && CMD_LIFE_ALIAS[k]) k = CMD_LIFE_ALIAS[k];
    if (!T[k] && k.length > 3 && k.slice(-1) === 's' && T[k.slice(0, -1)]) k = k.slice(0, -1);
    return T[k] ? Object.assign({ name: k }, T[k]) : null;
  };
  const cmdLifeNear = (rec) => {                       // nearest LIVE body of this species, by XZ
    let best = null, bd = Infinity;
    for (let j = rec.lo; j < rec.hi; j++) {
      const B = wbf[j];
      if (!B || !B.init || B.rag || B.slain || B.dying || B.dieT || typeof B.x !== 'number') continue;   // a ragdoll, a slain slot or a body fading out at the dusk handover is not somewhere to send the player
      if (rec.ok && !rec.ok(B)) continue;
      const d = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z);
      if (d < bd) { bd = d; best = B; }
    }
    return best;
  };
  // …and for a WATER animal, WHICH one is as much of the answer as where you stand. The landing has to be the
  // nearest dry bank to the creature (the alternative is the lake bed, where the player drowns), so a fish in
  // the middle of a 400-wide reservoir leaves you looking at it from 100 voxels away however near it happened
  // to be to where you were standing — MEASURED at 103 for a blue_gill. So prefer any live one that has a bank
  // within CMD_BANK of it, nearest-to-the-player among those, and only fall back to plain nearest when the
  // whole population is out in open water.
  const CMD_BANK = 24;
  const cmdBankPick = (rec) => {
    let inshore = null, nd = Infinity, any = null, aq = Infinity;
    for (let j = rec.lo; j < rec.hi; j++) {
      const B = wbf[j];
      if (!B || !B.init || B.rag || B.slain || B.dying || B.dieT || typeof B.x !== 'number') continue;
      if (rec.ok && !rec.ok(B)) continue;
      const sh = cmdShore(B.x, B.z), q = Math.hypot(sh.x - B.x, sh.z - B.z);
      if (q < aq) { aq = q; any = B; }
      if (q > CMD_BANK) continue;
      const d = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z);
      if (d < nd) { nd = d; inshore = B; }
    }
    return inshore || any;
  };
  const cmdLifePick = (rec) => { const B = cmdLifeNear(rec); return (B && rec.shore) ? (cmdBankPick(rec) || B) : B; };
  // Stand BESIDE it and LOOK AT IT. The standoff is taken on the player's own side of the creature, so the
  // arrival is on ground they were already travelling over rather than through the animal; fish and ducks are
  // reached from the SHORE instead, because the alternative is standing on the lake bed and drowning.
  const cmdGoLife = (B, rec) => {
    let ax = B.x, az = B.z, ay = (B.perchFeet === undefined ? (B.y || 0) : B.perchFeet), n = 1, mx = 0;   // a PERCHED songbird keeps its spawn-time glide height in B.y and its real one in perchFeet, so aiming at B.y would look straight past it into the crown
    if (rec.line) {                                    // ── THE ANT COLUMN IS ONE ANIMAL ── aim at the MIDDLE of the line, not at whichever ant happened to be nearest: the file is ANT_GAP apart over several bodies, so arriving at its head leaves the rest of it behind your shoulder
      let sx = 0, sz = 0, sy = 0; n = 0;
      for (let j = rec.lo; j < rec.hi; j++) { const A = wbf[j]; if (!A || !A.init || A.rag || A.slain || typeof A.x !== 'number') continue;
        sx += A.x; sz += A.z; sy += (A.perchFeet === undefined ? (A.y || 0) : A.perchFeet); n++; }
      if (n) { ax = sx / n; az = sz / n; ay = sy / n; } else n = 1;
      for (let j = rec.lo; j < rec.hi; j++) { const A = wbf[j]; if (!A || !A.init || A.rag || A.slain || typeof A.x !== 'number') continue;
        mx = Math.max(mx, Math.hypot(A.x - ax, A.z - az)); }
    }
    // …and the standoff for a LINE is the line's own half-length plus a body or two, capped: framing a column
    // from a fixed distance works for one ant and cuts the tail off five. Everything else takes rec.off.
    const off = rec.line ? Math.max(rec.off || 12, Math.min(30, mx + 9)) : (rec.off || 7);
    let dx = P.x - ax, dz = P.z - az; const dl = Math.hypot(dx, dz);
    if (dl < 0.5) { dx = 0; dz = 1; } else { dx /= dl; dz /= dl; }
    const spot = rec.shore ? cmdShore(ax, az) : { x: Math.round(ax + dx * off), z: Math.round(az + dz * off) };
    cmdGoTo(spot.x, spot.z, rec.name + (rec.line && n > 1 ? ' column (' + n + ')' : ''), { x: ax, y: ay, z: az });
  };
  // ── NONE ALIVE ── creatures are only ever placed in a ring around the PLAYER (LIFE_IN..LIFE_OUT in tick-life)
  // and only where their own spawn gate admits them — desert life needs desertM > 0.85, fish and ducks need real
  // water, and everything else is refused IN the desert by that same test. So "there is no ant" nearly always
  // means "you are not standing anywhere an ant can exist", and the honest answer is to travel to habitat that
  // species can spawn in and let the spawner do the rest. The watch then makes the second hop onto the animal.
  const CMD_LIFE_HAB = { desert: ['desert', 'the desert'], water: ['lake', 'the water', 'river'], forest: ['pine_forest', 'the pine forest'] };
  // Only the NEWEST /locate owns the watch. The generation is claimed by cmdLocate itself, not here, because
  // ANY later locate has to cancel a pending one — including /locate lake. Bumping it only when a watch is
  // armed left a stale ant watch alive through the next three commands and then teleported the player out of
  // wherever they had just asked to be.
  let cmdSeekTok = 0;
  const cmdSeekWatch = (rec, tries) => {
    const tok = cmdSeekTok, wall = performance.now() + 45000;   // a hard ceiling, so a world that never finishes streaming still gives an answer rather than watching for ever
    const step = () => {
      if (tok !== cmdSeekTok) return;
      const B = cmdLifePick(rec);
      if (B) { cmdGoLife(B, rec); return; }
      // The spawner REFUSES any point outside the built rect, so the first seconds after a long hop are spent
      // with the terrain still streaming in and nothing can turn up however patiently you wait. Those seconds
      // are not the creature's fault and must not come out of its budget: MEASURED, /locate fish across 1598
      // voxels gave up while the rect was still catching up and reported 'no fish' beside a full lake.
      const t9 = rectTarget();
      const streaming = rect.xlo > t9.xlo || rect.xhi < t9.xhi || rect.zlo > t9.zlo || rect.zhi < t9.zhi;
      if ((streaming || --tries > 0) && performance.now() < wall) { setTimeout(step, 500); return; }
      cmdSay('no ' + rec.name + ' has turned up' + (rec.when ? ' — they are only out ' + rec.when : ' here'));
    };
    setTimeout(step, 500);
  };
  const cmdLocate = (what) => {
    const key0 = String(what || '').trim().toLowerCase().replace(/\s+/g, '_');
    cmdSeekTok++;                                      // a new /locate always cancels the pending habitat watch, whatever it is looking for
    if (!key0) { cmdSay('locate what? ' + Object.keys(CMD_LOCATE).join(', ') + ', or any creature'); return; }
    // ── A CREATURE ── tried on the RAW word, ahead of CMD_LOC_ALIAS, so a terrain alias can never quietly
    // redirect a species name that happens to collide with one.
    const life = cmdLifeOf(key0);
    if (life) {
      const B = cmdLifePick(life);
      if (B) { cmdGoLife(B, life); return; }           // one is already spawned: the common case, and instant
      const hab = CMD_LIFE_HAB[life.hab] || CMD_LIFE_HAB.forest, ox = P.x, oz = P.z;
      // The ring walk searches outward from the PLAYER, and the desert holds no water at all, so a fish asked
      // for from deep in the sand has to be walked back to the forest before the search means anything.
      if (life.hab === 'water' && desertM(P.x, P.z) > 0.5) { const f9 = CMD_LOCATE.pine_forest(); if (f9) cmdGoTo(Math.round(f9.x), Math.round(f9.z), f9.what); }
      let spot = CMD_LOCATE[hab[0]]();
      if (!spot && hab[2]) spot = CMD_LOCATE[hab[2]]();   // a river is water too — fish and dragonflies both take one
      if (!spot) { cmdSay('no ' + life.name + ' alive, and no ' + hab[1] + ' within range'); return; }
      cmdGoTo(Math.round(spot.x), Math.round(spot.z), spot.what);
      cmdSay('no ' + life.name + ' nearby — moved to ' + hab[1] + ', ' + Math.round(Math.hypot(P.x - ox, P.z - oz)) + ' voxels, watching for one');   // AFTER cmdGoTo: it says its own line, and this is the one the player asked for
      cmdSeekWatch(life, 24);                          // 12 s AFTER the stream settles - the 2.5 s lake census plus several spawn retries
      return;
    }
    const key = CMD_LOC_ALIAS[key0] || key0;
    const f = CMD_LOCATE[key];
    if (!f) { cmdSay('cannot locate "' + key0 + '" — try: ' + Object.keys(CMD_LOCATE).join(', ') + ', or a creature (ant, gecko, scorpion, bunny, skunk, fish, duck, worm, cardinal, …)'); return; }
    const spot = f();
    if (!spot) { cmdSay('no ' + key + ' found within range'); return; }
    cmdGoTo(Math.round(spot.x), Math.round(spot.z), spot.what);
  };
  // Put the player down on solid ground at a world column, however far away — same sequence respawn() uses:
  // move, recenter (which regenerates around the new spot), THEN read the height, because hmap is wrapped
  // window data and means nothing for a column the window did not cover a moment ago.
  // Put the player down on solid ground at a world column, however far away — same sequence respawn() uses:
  // move, recenter (which regenerates around the new spot), THEN read the height, because hmap is wrapped
  // window data and means nothing for a column the window did not cover a moment ago.
  // No loading panel: it was tried and removed (user 2026-08-05). One world window means the old surroundings
  // cannot be kept on screen while the new ones generate, so the panel could only ever be an opaque cover over
  // a wait — and the wait is short enough that arriving straight away reads better than being held.
  const cmdGoTo = (x, z, label, look) => {
    const d0 = Math.round(Math.hypot(x - P.x, z - P.z));
    P.x = x + 0.5; P.z = z + 0.5; P.vy = 0; P.hvx = 0; P.hvz = 0; P.sink = 0; P.fly = false;
    // maybeRecenter only fires past its own distance threshold, and a hop SHORTER than that can still land
    // outside the built rect — where hmap and W are another column's wrapped data, so the ground reads
    // wrong and the lift-out-of-solid loop below climbs through garbage (it put the player ~100 voxels in
    // the air on /locate log). Anything not comfortably inside the rect gets a real regen first.
    const M = 24;
    if (x < rect.xlo + M || x > rect.xhi - M || z < rect.zlo + M || z > rect.zhi - M) recenter(x, z);
    else maybeRecenter();
    P.y = hmap[gwrap(x, WX) + gwrap(z, WZ) * WX];
    for (let g = 0; g < 40 && P.y < WY - 20 && !boxFree(P.x, P.y, P.z, HEIGHT); g++) P.y += 1;   // never land inside a rock or a trunk — capped, so a bad column drops you rather than launching you
    P.fallT = 0; smoothEye = P.y + EYE; resetHist = 1;
    // …and FACE IT (the creature path passes a look point; every terrain target passes nothing and is
    // untouched). An ant is one voxel tall: arriving beside it still pointing whichever way you happened to be
    // pointing is indistinguishable from arriving nowhere. Pitch is clamped inside the same +-1.55 the mouse
    // look is, and the horizontal leg is floored at 2 so a target directly underfoot cannot snap the view down.
    if (look) { const lx = look.x - P.x, lz = look.z - P.z, ly = (look.y === undefined ? P.y + EYE : look.y);
      P.yaw = Math.atan2(lx, lz);
      P.pitch = Math.max(-1.5, Math.min(1.5, Math.atan2(ly - (P.y + EYE), Math.max(2, Math.hypot(lx, lz))))); }
    cmdSay('→ ' + label + ' — ' + d0 + ' voxels');
  };
  const cmdRun = (line) => {
    const t = String(line || '').trim();
    if (!t) return;
    const m = t.match(/^\/?(\w+)\s*(.*)$/);
    if (!m) { cmdSay('?'); return; }
    const c = m[1].toLowerCase();
    if (c === 'spawn') cmdSpawn(m[2]);
    else if (c === 'locate' || c === 'find' || c === 'goto') cmdLocate(m[2]);
    else cmdSay('unknown command: ' + m[1] + ' (/spawn, /locate)');
  };
  // CAPTURE phase, on the document: this has to work even when the input never got focus, or the line
  // cannot be dismissed and the keyboard stays captured (user: "pressing esc should disable the command line").
  // A CLICK ANYWHERE dismisses the line and hands the game back (user). The pointer never left, so
  // there is nothing to re-acquire — and the click that dismissed it must not also swing the tool.
  document.addEventListener('mousedown', (e) => {
    if (!CMD.open) return;
    e.preventDefault(); e.stopPropagation();
    cmdShow(false);
  }, true);
  document.addEventListener('keydown', (e) => {
    if (!CMD.open) {
      // …and once it is closed, Escape behaves as it always did. If the re-lock was refused we are still
      // unlocked with no menu showing, so this second press is what puts the menu up (user).
      if (e.code === 'Escape' && !locked && !dead && performance.now() - CMD.escAt > 40 && vePanel.classList.contains('hidden')) lockEl.classList.remove('hidden');
      return;
    }
    e.stopPropagation();                               // every keystroke belongs to the line, never to the game's binds
    if (e.code === 'Escape') { e.preventDefault(); CMD.escAt = performance.now(); cmdShow(false); }
    else if (e.code === 'Enter' || e.code === 'NumpadEnter') { e.preventDefault(); const v = cmdBuf; cmdShow(false); cmdRun(v); }
    else if (e.code === 'Backspace') { e.preventDefault(); cmdBuf = cmdBuf.slice(0, -1); cmdDraw(); }
    else if (e.key && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) { e.preventDefault(); cmdBuf += e.key; cmdDraw(); }
  }, true);
