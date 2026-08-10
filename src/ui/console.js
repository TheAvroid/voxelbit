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
    // stand on the first DRY column stepping out from a water spot — a lake is somewhere to arrive AT, not in
    const shoreOf = (wx, wz) => {
      for (let r = 0; r <= 700; r += 2) for (let a = 0; a < 12; a++) {   // 700 clears even the biggest reservoir — a short walk would give up mid-lake and land the player in the water
        const x = Math.round(wx + Math.cos(a * 0.5236) * r), z = Math.round(wz + Math.sin(a * 0.5236) * r);
        if (H(x, z) > WL + 1 && !nearCave(x, z)) return { x, z };
      }
      return { x: Math.round(wx), z: Math.round(wz) };
    };
    return {
      lake: () => { const g = nearestCell(RIVCELL, 8, (cx, cz) => { const R = riverAt(cx, cz); if (!R) return null;
          let b = null; for (const L of R.lakes) if (L.r >= 120 && (!b || L.r > b.r)) b = L;   // reservoirs/tail lakes — a headwater pond is not what "lake" means
          return b ? { x: b.x, z: b.z, r: b.r } : null; });
        return g ? { ...shoreOf(g.x, g.z), what: 'lake (r ' + Math.round(g.r) + ')' } : null; },
      river: () => { const g = nearestCell(RIVCELL, 8, (cx, cz) => { const R = riverAt(cx, cz); if (!R || !R.segs.length) return null;
          const s = R.segs[0]; return { x: s.sx + s.dxr * s.len * 0.5, z: s.sz + s.dzr * s.len * 0.5 }; });
        return g ? { ...shoreOf(g.x, g.z), what: 'river' } : null; },
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
    };
  })();
  const CMD_LOC_ALIAS = { water: 'lake', pond: 'lake', stream: 'river', pine: 'tree', forest: 'tree', boulder: 'rock',
    stone: 'rock', mushrooms: 'mushroom', ferns: 'fern', ravine: 'gorge', cave: 'gorge', canyon: 'gorge', logs: 'log', trees: 'tree', rocks: 'rock', lakes: 'lake' };
  const cmdLocate = (what) => {
    const key0 = String(what || '').trim().toLowerCase().replace(/\s+/g, '_');
    if (!key0) { cmdSay('locate what? ' + Object.keys(CMD_LOCATE).join(', ') + ', or any creature'); return; }
    const key = CMD_LOC_ALIAS[key0] || key0;
    // ── A LIVE CREATURE ── nearest one already in the world; no search, the pool is right here.
    if (CMD_SPECIES[key]) {
      const [lo, hi] = CMD_SPECIES[key];
      let best = null, bd = Infinity;
      for (let j = lo; j < hi; j++) { const B = wbf[j]; if (!B || !B.init) continue;
        const d = (B.x - P.x) * (B.x - P.x) + (B.z - P.z) * (B.z - P.z);
        if (d < bd) { bd = d; best = B; } }
      if (!best) { cmdSay('no ' + key + ' alive nearby — try /spawn ' + key); return; }
      cmdGoTo(Math.round(best.x), Math.round(best.z), key + ' (' + Math.round(Math.sqrt(bd)) + ' vox)');
      return;
    }
    const f = CMD_LOCATE[key];
    if (!f) { cmdSay('cannot locate "' + key0 + '" — try: ' + Object.keys(CMD_LOCATE).join(', ')); return; }
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
  const cmdGoTo = (x, z, label) => {
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
