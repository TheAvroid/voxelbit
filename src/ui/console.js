  // @module — the in-game command line (T)
  // @exports CMD, cmdMsg, cmdRun, cmdShow
  // ══ COMMAND LINE (user) ══ T opens it, Enter runs, Escape cancels. `/spawn <thing>` puts a creature
  // or an item in front of the player. While it is open the game's own keyboard is silenced — the binds
  // read raw key codes, so without that, typing "spawn" would drop the held item on the d and toggle
  // fly on the f.
  // name → [slot band lo, hi, B.kind]. The bands are the creature pool's own layout, and this is a FUNCTION for
  // the same reason CMD_LIFE below is one: sim/life/slots.js owns the slot ladder those boundaries come from and
  // is concatenated UNDER ui/console.js, so an object literal reading them at module-init would be a TDZ throw
  // and a black screen. A command line runs at human speed; rebuilding nine entries per /spawn costs nothing.
  const CMD_SPECIES = () => ({
    butterfly: [FLY_0, FLY_END, 0], moth: [FLY_0, FLY_END, 1], duck: [DUCK_0, DUCK_END, 3], worm: [WORM_0, WORM_END, 2],
    fish: [FISH_0, FISH_END, 6], bunny: [BUNNY_0, BUNNY_END, 2], armadillo: [ARM_0, ARM_END, 2], skunk: [SKUNK_0, SKUNK_END, 2], porcupine: [PORC_0, FLAM_0, 2], flamingo: [FLAM_0, FLAM_END, 2],
  });
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
      cmdBuf = '';                                   // ── BLANK (user 2026-08-17: "dont automatically type / when the player presses t") ── T used to pre-type the slash. Nothing depended on it and nothing about HOW a command is typed changes: cmdRun's `^\/?` has always made the leading slash OPTIONAL, so `/spawn apple` and `spawn apple` both routed before this and both still do. This is the prompt going quiet, not the slash going away.
      cmdDraw();                                     // …and nothing here assumed a character was in the buffer: ''.slice(0, -1) is '', Enter on an empty line returns from cmdRun's own `if (!t) return`, and the green > is a separate element (#cmdCaret) rather than part of the text
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

  // ══════════════════════ EVERY .vox IN THE GAME ══════════════════════
  // /spawn reaches all 914 of them and /locate reaches the ones the world is actually built out of (user
  // 2026-08-17: "have the /locate command work for every single .vox file … same thing applies to /spawn").
  // VOXDEX (assets/vox-index.js) is the asset tree, written by tools/bundle.py from a walk of game/assets on
  // every build, so it cannot drift from what is on disk — see that file for why a manifest and not a 404 walk.
  //
  // Unpacked ONCE, on the first command that needs it, so the boot pays nothing. `path` keeps the real case
  // (the fetch URL is case-sensitive: decoration/rocks/BIG_1_BiG_0.vox) and `key` is its lowercase twin, which
  // is what every lookup compares. `dir` is every folder AND every ancestor of one, each with the half-open
  // range of files under it: bundle.py emits folders parent-first with siblings in order, so a whole subtree is
  // one unbroken run and "how many files are under life/tropical_life/pelican" is two integers, not a scan.
  let VOXI = null;
  const voxIndex = () => {
    if (VOXI) return VOXI;
    const path = [], key = [], dir = [], dlo = [], dhi = [], seen = new Map();
    for (const grp of VOXDEX.split(';')) {
      const c = grp.indexOf(':');
      if (c < 0) continue;                             // an empty VOXDEX splits to [''] — only reachable if game/assets went missing at build time, and a junk '' path is a worse answer than none
      const d = grp.slice(0, c), at = path.length;
      for (const n of grp.slice(c + 1).split(',')) { const p = d ? d + '/' + n : n; path.push(p); key.push(p.toLowerCase()); }
      for (let a = d; a; ) {                           // this folder and every folder above it
        let j = seen.get(a);
        if (j === undefined) { j = dir.length; seen.set(a, j); dir.push(a); dlo.push(at); dhi.push(at); }
        dhi[j] = path.length;                          // parent-first + siblings in order ⇒ a subtree is contiguous, so the range only ever grows at the top
        const s = a.lastIndexOf('/'); if (s < 0) break;
        a = a.slice(0, s);
      }
    }
    VOXI = { path, key, dir, dkey: dir.map((d) => d.toLowerCase()), dlo, dhi };
    return VOXI;
  };
  // ── HOW A PLAYER NAMES A FILE ── the canonical name is the path under game/assets with the .vox dropped,
  // and ANY TAIL of it is accepted while it names exactly one thing: `palm_tree`, `oak_3`, `food/apple/07`,
  // `rocks/Big_2_BiG_0`. A FOLDER is a name too and resolves to its first file, because that is what a player
  // means by "apple" when food/apple is thirteen bite frames. What this deliberately does NOT do is guess: 70
  // of these files are called 00.vox, so `/spawn 00` comes back with a list and no world edit. A file beats a
  // folder of the same name (it is the more specific answer), and a query that names nothing at all falls back
  // to "contains", which is what a half-remembered name is.
  const voxNorm = (s) => String(s || '').trim().toLowerCase().replace(/\\/g, '/').replace(/\s+/g, '_')
    .replace(/\.vox$/, '').replace(/^(?:game\/)?assets\//, '').replace(/^\/+/, '').replace(/\/+$/, '');
  const voxTail = (k, q) => k === q || (k.length > q.length && k.charCodeAt(k.length - q.length - 1) === 47 && k.endsWith(q));   // 47 = '/', so `oak_3` matches foilage/oak_trees/oak_3 and `k_3` matches nothing
  // WHICH file stands for a folder. The first NUMBERED one, not simply the first: an animation folder holds
  // its frames as 00, 01, 02 … beside a `base.vox`, and base.vox is the multi-model SOURCE SCENE the frames
  // were baked out of — it sorts first alphabetically and it is the one file in the folder that does not
  // stand up on its own (parseVoxModel would hand back model 1 of 120). /spawn cardinal wants the bird.
  const voxRep = (I, lo, hi) => { for (let i = lo; i < hi; i++) { const k = I.key[i], n = k.slice(k.lastIndexOf('/') + 1);
      if (n && !/\D/.test(n)) return i; } return lo; };
  const voxFind = (q0) => {
    const q = voxNorm(q0), I = voxIndex();
    if (!q) return {};
    const files = [], dirs = [];
    for (let i = 0; i < I.key.length; i++) if (voxTail(I.key[i], q)) files.push(i);
    for (let i = 0; i < I.dkey.length; i++) if (voxTail(I.dkey[i], q)) dirs.push(i);
    if (files.length === 1) return { i: files[0] };
    if (!files.length && dirs.length === 1) { const d = dirs[0]; return { i: voxRep(I, I.dlo[d], I.dhi[d]), of: I.dhi[d] - I.dlo[d], folder: I.dir[d] }; }
    if (files.length || dirs.length) return { amb: 1, files, dirs };
    for (let i = 0; i < I.key.length; i++) if (I.key[i].indexOf(q) >= 0) files.push(i);
    return files.length ? { amb: 1, files, dirs, fuzzy: 1 } : {};
  };
  // #cmdMsg is white-space: nowrap, so a "did you mean" that lists everything runs off the edge of the screen
  // and reads as nothing at all. Three, then a count.
  const voxList = (r) => { const I = voxIndex(), out = [];
    for (const d of r.dirs) if (out.length < 3) out.push(I.dir[d] + '/');
    for (const i of r.files) if (out.length < 3) out.push(I.path[i]);
    const tot = r.dirs.length + r.files.length;
    return out.join('  ') + (tot > out.length ? '  (+' + (tot - out.length) + ' more)' : ''); };

  // ══ A /spawn MINTS NOTHING ══ the hard bound on this whole feature, and the constraint everything else here
  // bends around. A voxel id is 8 BITS: entry 256 does not exist, it wraps, and it takes the voxel's SOLIDITY
  // with it (see the ceiling notes in assets/palette.js). The table ends at ~255 of 256, and the game is still
  // minting at RUNTIME — edCol adds a species' colours the first time it is stamped or ragdolled — so a slot
  // spent here is a slot a bunny does not get later, and the symptom is a creature quietly wearing somebody
  // else's browns for the rest of the session. An arbitrary .vox carries arbitrary colour and there are 914 of
  // them, so ANY per-spawn mint budget is a slow leak with the player holding the tap.
  // So the budget is ZERO, and the bound is exact: palette.length is the same after fifty spawns as it was at
  // boot. Nothing a player types on this line can change what any existing voxel in the world looks like.
  //
  // Three steps, and the FIRST is why this looks right for nearly everything: an EXACT match. Most of these
  // files are the source art for something already in the world, so the pine's bark, the rocks' greys, the
  // stone tools and every loaded creature come back on their own authored ids — a spawned model is then the
  // same MATERIAL as the real one, solid where it should be solid and choppable where it should be choppable,
  // for free and without a single table lookup being taught about it.
  //   * palOwn is deliberately NOT skipped on the exact path, where palShare does skip it. A reserved id means
  //     "this set of ids identifies that model", and an exact colour hit means this IS that model: spawning
  //     pinecone.vox should give a pinecone that picks up as a pinecone. The near/nearest paths DO skip it,
  //     because a near miss borrowing a cone's id is precisely the bug palOwn exists to stop.
  //   * DECOR_MIN is not a floor here either, for the same reason: matching down into the solid range is what
  //     makes a spawned boulder something you walk on rather than through.
  //   * CREA_FLAG ids are skipped on the near/nearest paths — that flag is how the tracer and the hit flash
  //     ask "is this voxel part of an animal", and a gun barrel answering yes is a wrong answer.
  const voxColC = new Map();                           // colour → { id, e } — 255 comparisons per DISTINCT colour, once per session
  let voxBan = null;
  const voxCol = (r, g, b) => {
    const k = (r << 16) | (g << 8) | b;
    let h = voxColC.get(k);
    if (h !== undefined) return h;
    if (!voxBan) { voxBan = new Uint8Array(256);       // ids a spawn must never write however close the colour: water and lava are FLUIDS and snow melts, so a grey-blue gun barrel landing on WATER_T would hang a lake in the air
      for (const v of [WATER_T, WATER_B, LAVA_T, LAVA_B, LAVA_R, LAVA_Y, SNOW[0], SNOW[1]]) voxBan[v] = 1; }
    for (let i = 1; i < palette.length; i++) { const c = palette[i];
      if (c && !voxBan[i] && c[0] === r && c[1] === g && c[2] === b) { h = { id: i, e: 0 }; break; } }
    if (h === undefined) {
      let bi = -1, bd = 1e9;
      for (let i = 1; i < palette.length; i++) { const c = palette[i];
        if (!c || voxBan[i] || palOwn.has(i) || CREA_FLAG[i]) continue;
        const d = (c[0] - r) * (c[0] - r) + (c[1] - g) * (c[1] - g) + (c[2] - b) * (c[2] - b);
        if (d < bd) { bd = d; bi = i; } }
      if (bi < 0) bi = palNearest(r, g, b);            // cannot happen with a loaded palette; a substitute is still better than a wrapped id
      const c = palette[bi];
      h = { id: bi, e: Math.max(Math.abs(c[0] - r), Math.abs(c[1] - g), Math.abs(c[2] - b)) };   // max-channel, the same error edSnapErr reports — it is what the player would see
    }
    voxColC.set(k, h);
    return h;
  };
  const VOX_CAP = 200000;                              // voxels one /spawn may write. Nothing shipped is close — the biggest, foilage/oak_trees/oak_7.vox, is 86,365 — so this is the bound that stops a hand-made monster from locking the tab, not a limit on the art
  const VOX_SLICE = 24000;                             // …and how many go into ONE gpuPatch. Comfortably under PATCHMAX (65,536 staged words), so a big model can never force a mid-call patchFlush — which would submit a voxel patch AHEAD of a pending strip scatter and let stale wrapped terrain stomp it (the ordering note in main/tick-passes.js). The remainder lands on the next tick, so an oak draws in over three or four frames instead of in one 30 ms hitch.
  const VOX_KEEP = 12;                                 // parsed models held. oak_7 is ~1 MB of packed voxels, so this is not a cache to let grow to 914
  const voxModels = new Map();
  const voxLoad = async (path) => {
    if (voxModels.has(path)) return voxModels.get(path);
    let bv = null;
    try { const r = await fetch('assets/' + path + '.vox'); if (r.ok) bv = new Uint8Array(await r.arrayBuffer()); } catch (e) { bv = null; }
    let rec = null;
    if (bv && bv.length) try {
      // voxColsUsed reads the colours a file's voxels actually reference and mints NOTHING — that is what it
      // exists for. Resolving all of them first and handing parseVoxModel a COMPLETE colMap means its
      // palShare/addCol branches are never reached: the arrangement assets/bow.js already uses for the beehive.
      const cmap = new Map(); let ex = 0, near = 0, snap = 0, worst = 0;
      for (const c of voxColsUsed(bv)) { const h = voxCol(c[0], c[1], c[2]);
        cmap.set((c[0] << 16) | (c[1] << 8) | c[2], h.id);
        if (!h.e) ex++; else if (h.e <= PAL_TOL) near++; else { snap++; if (h.e > worst) worst = h.e; } }
      rec = { m: parseVoxModel(bv, false, false, cmap), snap, worst };   // the FIRST model in the file. 42 of the 914 are multi-model, and a multi-model .vox is an animation (ui/editor.js says so) — so model 1 is frame 1, which is the pose to stand in the world
      console.log('[vb] /spawn', path, rec.m.sx + 'x' + rec.m.sy + 'x' + rec.m.sz, rec.m.vox.length, 'voxels;',
        cmap.size, 'colours →', ex, 'exact,', near, 'within PAL_TOL', PAL_TOL + ',', snap, 'snapped (worst ' + worst + '/255), 0 minted');   // the whole palette accounting, once per file: the HUD line has room for the headline and this has room for the proof
    } catch (e) { console.warn('[vb] /spawn', path, e); rec = null; }
    if (voxModels.size >= VOX_KEEP) voxModels.delete(voxModels.keys().next().value);   // insertion order: the oldest goes
    voxModels.set(path, rec);
    return rec;
  };
  // ── WHERE A SPAWN LANDS ── on whatever the crosshair is resting on (user). The same walk __vb.aimVox does:
  // the same voxRay and the same three refusals — surface scatter, open water, a snow blanket — so the model
  // sits on the ground you are looking at and not on the first snowflake in front of it. Nothing under the
  // crosshair (sky, or past VOX_REACH) falls back to cmdSpot, the ground straight ahead, which is where
  // /spawn has put a creature since the day it was written.
  const VOX_REACH = 96;
  const voxAim = () => {
    const cp = Math.cos(P.pitch), vx = Math.sin(P.yaw) * cp, vy = Math.sin(P.pitch), vz = Math.cos(P.yaw) * cp;
    let hit = null;
    voxRay(P.x, smoothEye, P.z, vx, vy, vz, VOX_REACH, (x, y, z) => {
      if (y < 1 || y >= WY) return 0;
      const v = W[gwrap(x, WX) + y * WX + gwrap(z, WZ) * WX * WY];
      if (!v || floatTab[v] || (isWater(v) && !solidTab[v]) || snowTab[v]) return 0;
      hit = { x, y: y + 1, z }; return 1;              // ON it, not in it
    });
    if (hit) return hit;
    const s = cmdSpot();
    return { x: s.x, y: Math.max(s.g, WL + 1), z: s.z };
  };
  // ── THE STAMP ── worldgen's own mode 1: fill empty cells, overwrite soft decor, never eat terrain. That is
  // what stampOak and every plant pass use, so a model dropped on a hillside is CLIPPED by the hill exactly as
  // a generated one is instead of carving a shelf out of it. Not stampModel itself for one reason: it writes W
  // and returns nothing, and a runtime edit has to hand gpuPatch the cells it actually changed. Rotation goes
  // with it — rot 0, the author's own facing, anchored bottom-centre like every other stamp in the game.
  //
  // gpuPatch runs with track=FALSE, deliberately. A spawn is authored geometry placed where the player
  // pointed, and it only ever ADDS voxels, so there is nothing for the support resolver to re-adjudicate that
  // could be right — and two things that would be wrong. 86k cells is well past SUP.cap (32,768), so the flood
  // saturates and leaves a backlog; and a model whose skirt overhangs a slope is, to the resolver, an
  // unsupported cluster, which it answers by ERASING. "Spawn me an oak" does not mean "and then delete it".
  // stampModel does not touch the queue either — it writes W, and hmap only in mode 2.
  // The navfield DOES need telling, because a spawned boulder is a real obstacle a creature must walk around,
  // so nvTouch is called per touched column afterwards — that is the one half of `track` that applies here.
  const voxStamp = (m, at, done) => {
    const bx = at.x - (m.sx >> 1), bz = at.z - (m.sy >> 1);   // …captured ONCE, with the player's box, so the model stays one coherent object across the slices below even if the player walks off mid-stamp
    const px = P.x, pz = P.z, py = P.y, cols = new Set();
    let mi = 0, put = 0, clip = 0;
    const slice = () => {
      const cells = [];
      while (mi < m.vox.length && cells.length < VOX_SLICE) {
        const p = m.vox[mi++], ax = bx + (p & 255), az = bz + ((p >> 8) & 255), ay = at.y + ((p >> 16) & 255);
        if (ay < 1 || ay >= WY) { clip++; continue; }
        if (ax < rect.xlo || ax >= rect.xhi || az < rect.zlo || az >= rect.zhi) { clip++; continue; }   // outside the built window W is another column's WRAPPED data, so a write there corrupts terrain somewhere else entirely
        if (Math.abs(ax + 0.5 - px) < HW + 1 && Math.abs(az + 0.5 - pz) < HW + 1 && ay > py - 2 && ay < py + HEIGHT) { clip++; continue; }   // never seal the player inside the thing they just asked for
        const gx = gwrap(ax, WX), gz = gwrap(az, WZ), ii = gx + ay * WX + gz * WX * WY;
        const cur = W[ii];
        if (cur !== 0 && cur < DECOR_MIN) { clip++; continue; }   // stampModel's mode-1 test, verbatim
        W[ii] = p >>> 24; cells.push(ii); cols.add(gx + gz * WX);
      }
      if (cells.length) { gpuPatch(cells, false, cells.length, false); put += cells.length; }
      if (mi < m.vox.length) { setTimeout(slice, 0); return; }   // W is written slice by slice too, not all at once: a band shift landing between two slices then re-generates terrain the stamp has not reached yet, instead of quietly erasing half of one it had already written
      for (const c of cols) nvTouch(c % WX, (c / WX) | 0);
      done(put, clip);
    };
    slice();
  };
  const cmdSpawnVox = (r) => {
    const path = voxIndex().path[r.i];
    cmdSay('loading ' + path + '…');
    voxLoad(path).then((rec) => {
      if (!rec) { cmdSay('assets/' + path + '.vox could not be read'); return; }
      const n = rec.m.vox.length;
      if (n > VOX_CAP) { cmdSay(path + ' is ' + n + ' voxels — over the ' + VOX_CAP + ' /spawn cap'); return; }
      voxStamp(rec.m, voxAim(), (put, clip) => {
        if (!put) { cmdSay(path + ' had nowhere to go — all ' + clip + ' voxels landed inside terrain or outside the built world. Aim at open ground'); return; }
        cmdSay('spawned ' + path + (r.of > 1 ? ' (1 of ' + r.of + ' in ' + r.folder + ')' : '')
          + ' — ' + put + ' voxels' + (clip ? ', ' + clip + ' clipped' : '')
          + (rec.snap ? ', ' + rec.snap + ' colour' + (rec.snap > 1 ? 's' : '') + ' snapped (worst ' + rec.worst + '/255, palette is full)' : ''));
      });
    });
  };
  const cmdSpawn = (what) => {
    const key = String(what || '').trim().toLowerCase().replace(/\s+/g, '_');
    if (!key) { cmdSay('spawn what? a creature (/spawn porcupine), an item (/spawn stone_axe) or any .vox (/spawn palm_tree)'); return; }
    const at = cmdSpot();
    // ── A CREATURE ──
    const sp = CMD_SPECIES()[key];
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
    if (it) {
      drops.push({ x: at.x, y: Math.max(at.g, WL + 1), z: at.z, it, ph: Math.random() * 6.28, born: performance.now() });
      if (drops.length > 32) drops.shift();
      cmdSay('spawned ' + key + ' (item ' + it + ')');
      return;
    }
    // ── ANY .vox IN THE GAME ── LAST, so nothing above it changes meaning: /spawn rock is still the pickable
    // hand stone and /spawn bow is still the bow, even though decoration/rock.vox and bow_arrow/bow/base.vox
    // are both in the index. A creature and an item are LIVE things with behaviour; this is the raw model,
    // stamped into the world at the crosshair.
    const V = voxFind(what);
    if (V.i !== undefined) { cmdSpawnVox(V); return; }
    if (V.amb) { cmdSay('"' + key + '" is ' + (V.files.length + V.dirs.length) + ' different assets: ' + voxList(V)); return; }
    cmdSay('no such thing: ' + key + ' — and no .vox under game/assets matches it');
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
      // …and the OTHER forest's tree. Stood off 24 rather than the pine's 12: an oak crown is up to 118 voxels
      // across, so landing at the pine's distance puts the player under the canopy instead of in front of it.
      oak: () => { const g = nearestCell(OKCELL, 40, (cx, cz) => { const t = oakAt(cx, cz); return t ? { x: t.wx, z: t.wz } : null; });
        return g ? { x: g.x + 24, z: g.z, what: 'oak' } : null; },
      // …and the THIRD forest's tree. Stood off 16: a birch crown is narrower than an oak's 118 but
      // wider than a pine, so it sits between their two numbers.
      birch: () => { const g = nearestCell(BKCELL, 40, (cx, cz) => { const t = birchAt(cx, cz); return t ? { x: t.wx, z: t.wz } : null; });
        return g ? { x: g.x + 16, z: g.z, what: 'birch' } : null; },
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
      // ══ THE REST OF THE SCATTERS ══ added so /locate can answer for the whole decoration and foliage set
      // rather than the nine things it had names for (user 2026-08-17). Every one of them is the same three
      // lines as the finders above — a nearestCell walk over that generator's OWN cell grid — because the
      // generator is already an analytic query over the infinite world and there was never anything else to
      // build. What is NOT here is as deliberate: a .vox that no pass places gets told so (see CMD_VOX_PLACE).
      cactus: () => { const g = nearestCell(CACCELL, 40, (cx, cz) => { const c2 = cactusAt(cx, cz); return c2 ? { x: c2.wx, z: c2.wz } : null; });
        return g ? { x: g.x + 14, z: g.z, what: 'cactus' } : null; },   // 14 = the widest saguaro's half-extent + a body, so you arrive beside it and not inside its spines (which hurt — cactusHurtAt)
      shrub: () => { const g = nearestCell(SHCELL, 40, (cx, cz) => { const s2 = shrubAt(cx, cz); return s2 ? { x: s2.wx, z: s2.wz } : null; });
        return g ? { x: g.x + 8, z: g.z, what: 'desert shrub', look: { x: g.x, z: g.z, foot: 1 } } : null; },
      desert_rock: () => { const g = nearestCell(DRCELL, 24, (cx, cz) => { const d2 = drockAt(cx, cz); return d2 ? { x: d2.wx, z: d2.wz } : null; });
        return g ? { x: g.x + 20, z: g.z, what: 'desert rock' } : null; },
      // …FOREST LITTER is two voxels tall, so a fixed standoff is not the problem — the GAZE is. Arriving five
      // voxels from a pinecone still pointing wherever you happened to be pointing is indistinguishable from
      // arriving nowhere, exactly as it was for the ant, so these pass cmdGoTo a look point with `foot` set:
      // the height is not known until after the recenter, and `foot` means "it is lying on the ground you just
      // landed on", which cmdGoTo resolves once P.y is real.
      stick: () => { const g = nearestCell(SCELL, 40, (cx, cz) => { const s2 = stickAt(cx, cz); return s2 ? { x: s2.wx, z: s2.wz } : null; });
        return g ? { x: g.x + 5, z: g.z, what: 'a stick', look: { x: g.x, z: g.z, foot: 1 } } : null; },
      pinecone: () => { const g = nearestCell(PCCELL, 40, (cx, cz) => { const p2 = pconeAt(cx, cz); return p2 ? { x: p2.wx, z: p2.wz } : null; });
        return g ? { x: g.x + 5, z: g.z, what: 'a pinecone', look: { x: g.x, z: g.z, foot: 1 } } : null; },
      // …a lilypad floats ON the water, so it takes the lake's own shore walk for the reason the fish do: the
      // alternative is standing on the lakebed and drowning. The look point is the pad itself, at water level.
      lilypad: () => { const g = nearestCell(LILYCELL, 24, (cx, cz) => { const l2 = lilyAt(cx, cz); return l2 ? { x: l2.wx, z: l2.wz } : null; });
        return g ? { ...cmdShore(g.x, g.z), what: 'lilypads', look: { x: g.x, y: WL + 2, z: g.z } } : null; },
      // …and the two things that hang IN an oak rather than standing on the ground. hiveAt is a projection of
      // oakAt, so the walk is the oak's; the standoff is taken from the TRUNK column and the gaze from the
      // hive's own centre, which is the only way to see a 5-voxel box up in a 118-voxel crown.
      beehive: () => { const g = nearestCell(OKCELL, 40, (cx, cz) => { const h = hiveAt(cx, cz); return h ? { x: h.tx, z: h.tz, hv: h } : null; });
        return g ? { x: g.x + 22, z: g.z, what: 'beehive', look: { x: g.hv.wx, y: g.hv.wy, z: g.hv.wz } } : null; },
      fruit: () => { const g = nearestCell(OKCELL, 40, (cx, cz) => { const t = oakAt(cx, cz); return (t && t.fn) ? { x: t.wx, z: t.wz } : null; });
        return g ? { x: g.x + 24, z: g.z, what: 'an oak in fruit' } : null; },
      // …the gorge is DRY by construction (caveAt rejects any path touching water), so no shore walk: drop
      // the player straight onto its floor — cmdGoTo's hmap read lands them at the bottom, which is the point.
      gorge: () => { const g = nearestCell(CAVE_CELL, 24, (cx, cz) => { const c2 = caveAt(cx, cz); return c2 ? { x: c2.sx, z: c2.sz } : null; });
        return g ? { x: g.x, z: g.z, what: 'gorge' } : null; },
      // ── THE THREE BIOMES ── no nearestCell walk: every border is a single wandering line in x (oak forest
      // west, then pine, then desert east — see oakM and desertM), so marching along x at the player's own z
      // finds any of them exactly and cheaply.
      // Step 16 at a time and stop DEEP inside, not at the first column that qualifies: landing on the border
      // itself puts you in the dithered blend where the sand is half forest floor, which is not "the desert".
      // Same landing test the spawn nudge uses (build.js) — dry ground, clear of a gorge.
      desert: () => biomeSeek((x, z) => desertM(x, z) >= 0.995, 'the desert'),
      // ── AND PINE FOREST IS NOW A CONJUNCTION, NOT THE ABSENCE OF SAND ── it used to be `desertM <= 0.005`,
      // which was exact while there were two biomes and is WRONG the moment there are three: the oak forest
      // satisfies it perfectly, so /locate pine_forest from the oak side answered "you are already there".
      // ── AND THE BIRCH TERM, WHICH THE TWO NOTES ABOVE PREDICTED WOULD BE NEEDED ────────────
      // Both of them say it outright: a biome test written as the ABSENCE of the others breaks the
      // moment another arrives. The birch forest arrived in this very commit, and birch ground has
      // desertM 0 and oakM 0 -- so it satisfied this test perfectly and /locate pine_forest walked
      // you into a birch wood and announced the pines. Third time this exact shape has bitten;
      // adding the term rather than rewriting the pattern, because the pattern is right and it is
      // the MISSING CONJUNCT that is wrong each time.
      pine_forest: () => biomeSeek((x, z) => desertM(x, z) <= 0.005 && oakM(x, z) <= 0.005 && birchM(x, z) <= 0.005, 'the pine forest'),
      birch_forest: () => biomeSeek((x, z) => birchM(x, z) >= 0.995, 'the birch forest'),
      // ── AND OAK IS A CONJUNCTION NOW TOO (2026-08-18) ── the note above this predicted exactly this: a fourth
      // biome breaks any "biome" test written as the absence of the others. The cherry forest sits INSIDE oakM
      // (cherryM is a sub-region of it — see world/window.js), so `oakM >= 0.995` is satisfied perfectly from
      // deep inside the blossom and /locate oak_forest answered "you are already there" while standing under a
      // pink crown. pine_forest above needs no cherry term for the mirror reason: cherry ground has oakM 1, so
      // its `oakM <= 0.005` already excludes it.
      oak_forest: () => biomeSeek((x, z) => oakM(x, z) >= 0.995 && cherryM(x, z) <= 0.005, 'the oak forest'),
      cherry_forest: () => biomeSeek((x, z) => cherryM(x, z) >= 0.995, 'the cherry forest'),
    };
  })();
  // Walk x outward from the player (the biome split is a line in x), both ways, and take the first column that
  // is deep inside the wanted biome AND is somewhere you can stand. 24000 voxels is far past the widest the
  // blend band can ever be, so failing to find one means something is wrong rather than "keep looking".
  const biomeSeek = (want, label) => {
    for (let r = 0; r <= 24000; r += 16) {
      for (const x of (r === 0 ? [P.x] : [P.x + r, P.x - r])) {
        const xi = Math.round(x), zi = Math.round(P.z);
        if (!want(xi, zi)) continue;              // `want` takes the COLUMN, not one mask's value: with three biomes a test can need two masks at once (see pine_forest)
        if (H(xi, zi) <= WL + 1 || nearCave(xi, zi)) continue;
        return { x: xi, z: zi, what: label };
      }
    }
    return null;
  };
  const CMD_LOC_ALIAS = { water: 'lake', pond: 'lake', stream: 'river', forest: 'tree', boulder: 'rock',
    stone: 'rock', mushrooms: 'mushroom', ferns: 'fern', ravine: 'gorge', cave: 'gorge', canyon: 'gorge', logs: 'log', trees: 'tree', rocks: 'rock', lakes: 'lake',
    pine: 'tree', sand: 'desert', dunes: 'desert', pineforest: 'pine_forest', pines: 'pine_forest',
    birches: 'birch_forest', birchforest: 'birch_forest', birchwood: 'birch_forest', birch_wood: 'birch_forest',   // singular 'birch' is the TREE, plural is the BAND -- the same split pine/pines already uses

    oaks: 'oak', oakforest: 'oak_forest', oakwood: 'oak_forest',   // NOTE 'forest' stays pointed at 'tree' (nearest single pine) — it predates the biome and changing it would silently redirect an existing command
    cacti: 'cactus', saguaro: 'cactus', cactuses: 'cactus', shrubs: 'shrub', bush: 'shrub', bushes: 'shrub',
    desert_rocks: 'desert_rock', desertrock: 'desert_rock', sticks: 'stick', twigs: 'stick', pinecones: 'pinecone', cones: 'pinecone',
    lily: 'lilypad', lilypads: 'lilypad', lillypad: 'lilypad', lillypads: 'lilypad', pads: 'lilypad',
    honeycomb: 'beehive', hives: 'beehive', apple: 'fruit', apples: 'fruit', orange: 'fruit', oranges: 'fruit', fruits: 'fruit' };   // 'hive' is NOT here on purpose: CMD_LIFE_ALIAS already spends it on the bee, cmdLocate asks the creature table first, and /locate hive meaning "find me a bee" is the older answer. 'beehive' names the box itself.
  // ══ /locate <creature> ══ the LIVE pool, not the generator. Every creature in the world is a wbf body sitting
  // right here, so the nearest one is a scan of a slot band and nothing more — no ring walk, no regen, instant.
  // A band is a [lo, hi) slice of wbf PLUS an optional per-body test, because a band is not always one species:
  // the flyer band is butterfly / dragonfly / firefly depending on B.kind and B.dfly, the fish band splits on
  // B.fsp and the perched songbirds on B.bird. `hab` is the habitat that species' own spawn gate admits
  // (tick-creatures) and is the whole of the fallback when none is alive.
  const CMD_LIFE_ALIAS = { mouse: 'desert_mouse', desertmouse: 'desert_mouse', mice: 'desert_mouse', rabbit: 'bunny',
    hare: 'bunny', bunnies: 'bunny', moth: 'firefly', lightningbug: 'firefly', bluebird: 'blue_bird',
    bird: 'songbird', birds: 'songbird', songbirds: 'songbird', flies: 'fly', butterflies: 'butterfly',
    dragonflies: 'dragonfly', fireflies: 'firefly', baby_duck: 'duckling', ducklings: 'duckling', bluegill: 'blue_gill',
    snake: 'grass_snake', snakes: 'grass_snake', grasssnake: 'grass_snake', grass: 'grass_snake', honeybee: 'bee', hive: 'bee' };   // 'snake' is the grass snake and not the cobra on purpose: it is the one the user asked for by that word, and /locate cobra still answers exactly

  // Built FRESH on every call, deliberately. DESERTS and FISHES are filled by an async loader long after this
  // fragment is evaluated, and sim/life/slots.js — which owns wbf and the whole slot ladder — is concatenated BELOW
  // ui/console.js, so anything read at module-init here would be a TDZ throw or an empty list frozen for the
  // session. A command line runs at human speed; rebuilding a 25-key object costs nothing and can never go stale.
  const CMD_LIFE = () => {
    const T = {
      butterfly: { lo: FLY_0, hi: FLY_END, hab: 'forest', when: 'in daylight', ok: (B) => (B.kind | 0) === 0 && !B.dfly },
      dragonfly: { lo: FLY_0, hi: FLY_END, hab: 'water', when: 'in daylight, over water', ok: (B) => (B.kind | 0) === 0 && !!B.dfly },
      firefly: { lo: FLY_0, hi: FLY_END, hab: 'forest', when: 'after dark', ok: (B) => (B.kind | 0) === 1 },
      duck: { lo: DUCK_0, hi: DUCK_END, hab: 'water', shore: 1 },
      duckling: { lo: BABY_0, hi: BABY_END, hab: 'water', shore: 1 },
      worm: { lo: WORM_0, hi: WORM_END, hab: 'forest' },
      songbird: { lo: CARD_0, hi: CARD_END, hab: 'forest', off: 11 },
      cardinal: { lo: CARD_0, hi: CARD_END, hab: 'forest', off: 11, ok: (B) => (B.bird | 0) === 0 },
      blue_bird: { lo: CARD_0, hi: CARD_END, hab: 'forest', off: 11, ok: (B) => (B.bird | 0) === 1 },
      robin: { lo: CARD_0, hi: CARD_END, hab: 'forest', off: 11, ok: (B) => (B.bird | 0) === 2 },
      fish: { lo: FISH_0, hi: FISH_END, hab: 'water', shore: 1 },
      bunny: { lo: BUNNY_0, hi: BUNNY_END, hab: 'forest' },
      armadillo: { lo: ARM_0, hi: ARM_END, hab: 'forest' },
      skunk: { lo: SKUNK_0, hi: SKUNK_END, hab: 'forest' },
      porcupine: { lo: PORC_0, hi: FLAM_0, hab: 'pineforest' },   // hi is FLAM_0, not MAM_END: MAM_END grew to take in the flamingo band, so this walked 589..637 and /locate porcupine routed the player to a live flamingo
      flamingo: { lo: FLAM_0, hi: FLAM_END, hab: 'cherryforest' },   // the blossom's own bird — BIO_CHERRY, and the only creature that is is the one land mammal for which "the forest" means exactly one of the two
    };
    for (let i = 0; i < FISHES.length; i++) T[FISHES[i].name] = { lo: FISH_0, hi: FISH_END, hab: 'water', shore: 1, ok: ((k) => (B) => (B.fsp | 0) === k)(i) };   // species is fixed by SLOT (B.fsp), so the name has to key off the LOADED order rather than a literal
    for (let i = 0; i < DESERTS.length; i++) { const nm = DESERTS[i].name;   // seven contiguous DES_PER blocks in load order — read off DESERTS so re-ordering the loader cannot silently rename a band
      // ── hab IS WHERE THE FALLBACK SENDS YOU, so the mouse cannot claim to be desert-only any more: it was
      // given a second home in the oak forest (user 2026-08-17) and its slot band carries both populations.
      // …and the two 2026-08-17 species are not in the desert AT ALL. 'oakforest' is an existing one-place tag
      // (CMD_LIFE_HAB) rather than a two-place one like the mouse's 'sandoroak', because that is the literal
      // truth: DES_OAKONLY gives them zero of the desert head-count, so /locate bee out on the sand should
      // walk you to the oak wood and not stand you in the dunes watching an empty sky.
      T[nm] = { lo: MAM_END + i * DES_PER, hi: MAM_END + (i + 1) * DES_PER,
        hab: DES_OAKONLY[nm] ? 'oakforest' : (nm === 'desert_mouse' ? 'sandoroak' : 'desert'), line: nm === 'ant', off: nm === 'ant' ? 12 : 6 }; }
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
  // ── WHERE TO WALK YOU WHEN NONE IS ALIVE ── keyed by the `hab` tag on each species above. 'forest' means
  // EITHER forest and is resolved against where you are standing (see cmdHabSpot): most forest life —
  // songbirds, worms, bunnies, butterflies — lives in the oak wood and the pine wood alike, and sending
  // someone standing in one of them 400+ voxels into the other was the old behaviour and was simply wrong.
  // The two narrow tags exist for the two species that really are one-sided.
  const CMD_LIFE_HAB = { desert: ['desert', 'the desert'], water: ['lake', 'the water', 'river'],
    cherryforest: ['cherry_forest', 'the cherry forest'],
    forest: ['pine_forest', 'the pine forest'], pineforest: ['pine_forest', 'the pine forest'],
    oakforest: ['oak_forest', 'the oak forest'], sandoroak: ['desert', 'the desert'] };
  // 'forest' and 'sandoroak' both name two places, so the answer depends on where the player is: if they
  // are already standing in one of the creature's homes there is nothing to walk to, and the honest reply
  // is to stay put and watch. Returns null for "you are already there".
  const cmdHabSpot = (tag) => {
    const om = oakM(P.x, P.z), dm = desertM(P.x, P.z);
    if (tag === 'forest' && dm <= 0.5) return null;                       // either forest, and you are in one
    if (tag === 'forest') return CMD_LOCATE.pine_forest();                // out in the sand: the pine wood is the near one
    if (tag === 'sandoroak' && (dm > 0.85 || om > 0.5)) return null;      // mouse country, both halves
    if (tag === 'sandoroak') return CMD_LOCATE.oak_forest() || CMD_LOCATE.desert();
    return undefined;                                                     // not a two-place tag — the caller uses the table
  };
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
  // Lifted out of cmdLocate unchanged, so that a .vox PATH can reach it too: `/locate life/cardinal/rotate/03`
  // is a question about the cardinal, and the answer is the whole of this — the live pool, then its habitat,
  // then the watch — not a second, worse copy of it.
  const cmdLocLife = (life) => {
    const B = cmdLifePick(life);
    if (B) { cmdGoLife(B, life); return; }           // one is already spawned: the common case, and instant
    const hab = CMD_LIFE_HAB[life.hab] || CMD_LIFE_HAB.forest, ox = P.x, oz = P.z;
    // ── A TWO-PLACE HABITAT MAY ALREADY BE UNDERFOOT ── resolved before the table is used, so /locate bunny
    // from the oak forest watches where you stand instead of marching you into the pines. undefined means
    // "not a two-place tag, use the table"; null means "you are already in one of its homes".
    const two = cmdHabSpot(life.hab);
    if (two === null) { cmdSay('no ' + life.name + ' nearby — you are already in ' + hab[1].replace(/^the /, 'their range, ') + 'watching for one'); cmdSeekWatch(life, 24); return; }
    if (two) { cmdGoTo(Math.round(two.x), Math.round(two.z), two.what);
      cmdSay('no ' + life.name + ' nearby — moved to ' + two.what + ', watching for one'); cmdSeekWatch(life, 24); return; }
    // The ring walk searches outward from the PLAYER, and the desert holds no water at all, so a fish asked
    // for from deep in the sand has to be walked back to the forest before the search means anything.
    if (life.hab === 'water' && desertM(P.x, P.z) > 0.5) { const f9 = CMD_LOCATE.pine_forest(); if (f9) cmdGoTo(Math.round(f9.x), Math.round(f9.z), f9.what); }
    let spot = CMD_LOCATE[hab[0]]();
    if (!spot && hab[2]) spot = CMD_LOCATE[hab[2]]();   // a river is water too — fish and dragonflies both take one
    if (!spot) { cmdSay('no ' + life.name + ' alive, and no ' + hab[1] + ' within range'); return; }
    cmdGoTo(Math.round(spot.x), Math.round(spot.z), spot.what);
    cmdSay('no ' + life.name + ' nearby — moved to ' + hab[1] + ', ' + Math.round(Math.hypot(P.x - ox, P.z - oz)) + ' voxels, watching for one');   // AFTER cmdGoTo: it says its own line, and this is the one the player asked for
    cmdSeekWatch(life, 24);                          // 12 s AFTER the stream settles - the 2.5 s lake census plus several spawn retries
  };

  // ══ /locate FOR A FILE ══ and the honest half of "make it work for every .vox". It cannot, and pretending
  // otherwise is the failure mode worth designing against: MOST of the 914 files are not in the world in any
  // findable sense — held tools, gun fire and reload frames, the whole tropical set, source art for a bake
  // nobody stamps. A .vox that no generator places exists NOWHERE, so there is no column to walk to, and
  // teleporting somewhere plausible and announcing an arrival is a lie the player then has to disprove by
  // looking around. Saying "nothing in the world is made of that, but /spawn will put one here" answers what
  // they actually wanted — to find their asset in the game — and costs them one command instead of a search.
  //
  // Two routes exist, and both go into machinery that is already here rather than beside it:
  //   * anything under life/<species>/ IS a creature, and cmdLifeOf + cmdLocLife already own finding one. A
  //     species with no slot band (chicken, frog, flamingo, pelican, seagull, the six tropical fish) falls
  //     through to "not placed", which is the literal truth: nothing spawns them.
  //   * everything else is matched against this prefix table. It is short because the honest answer IS short:
  //     these are the files a generator stamps, and the rest of the tree is not stamped by anything.
  const CMD_VOX_PLACE = [
    ['foilage/pine5', 'tree'],                         // THE pine — assets/palette.js fetches this one file and stampTree plants it
    ['foilage/oak_trees/', 'oak'], ['foilage/cactus/', 'cactus'], ['foilage/desert_shrub/', 'shrub'],
    ['decoration/rocks/', 'rock'],                     // rocks26: boulderAt's three big tiers in the forest, drockAt's in the desert. 'rock' walks to the nearer forest field
    ['decoration/rock', 'rock'], ['decoration/mushroom', 'mushroom'], ['decoration/log', 'log'],
    ['decoration/stick_', 'stick'], ['decoration/pinecone', 'pinecone'], ['decoration/lillypad_', 'lilypad'],
    ['decoration/beehive', 'beehive'], ['food/apple', 'fruit'],
  ];
  // life/<species>/… and life/tropical_life/<species>/… — and life/ant.vox, which is a species at the top
  // level rather than in a folder of frames.
  const voxLifeOf = (k) => { const s = k.split('/');
    if (s[0] !== 'life' || s.length < 2) return null;
    const nm = s[1] === 'tropical_life' ? (s[2] || '') : s[1];
    return nm ? cmdLifeOf(nm) : null; };
  const cmdLocVox = (V) => {
    const I = voxIndex(), path = I.path[V.i], k = I.key[V.i];
    const life = voxLifeOf(k);
    if (life) { cmdLocLife(life); return; }
    let tgt = null;
    for (const [pre, t] of CMD_VOX_PLACE) if (k.slice(0, pre.length) === pre) { tgt = t; break; }
    if (tgt) {
      const spot = CMD_LOCATE[tgt]();
      if (!spot) { cmdSay('nothing in range is built from ' + path + '.vox — the ' + tgt + ' pass has none nearby'); return; }
      cmdGoTo(Math.round(spot.x), Math.round(spot.z), spot.what, spot.look);
      return;
    }
    cmdSay(path + '.vox is not placed by any generator — nothing in the world is made of it. /spawn ' + (V.folder || path) + ' puts one in front of you');
  };
  const cmdLocate = (what) => {
    const key0 = String(what || '').trim().toLowerCase().replace(/\s+/g, '_');
    cmdSeekTok++;                                      // a new /locate always cancels the pending habitat watch, whatever it is looking for
    if (!key0) { cmdSay('locate what? ' + Object.keys(CMD_LOCATE).join(', ') + ', any creature, or any .vox path'); return; }
    // ── A CREATURE ── tried on the RAW word, ahead of CMD_LOC_ALIAS, so a terrain alias can never quietly
    // redirect a species name that happens to collide with one.
    const life = cmdLifeOf(key0);
    if (life) { cmdLocLife(life); return; }
    const key = CMD_LOC_ALIAS[key0] || key0;
    const f = CMD_LOCATE[key];
    if (f) {
      const spot = f();
      if (!spot) { cmdSay('no ' + key + ' found within range'); return; }
      cmdGoTo(Math.round(spot.x), Math.round(spot.z), spot.what, spot.look);
      return;
    }
    // ── AND ANY .vox IN THE GAME ── LAST, exactly as in /spawn, so no name above can change meaning.
    const V = voxFind(what);
    if (V.i !== undefined) { cmdLocVox(V); return; }
    if (V.amb) { cmdSay('"' + key0 + '" is ' + (V.files.length + V.dirs.length) + ' different assets: ' + voxList(V)); return; }
    cmdSay('cannot locate "' + key0 + '" — try: ' + Object.keys(CMD_LOCATE).join(', ') + ', a creature (ant, gecko, bunny, fish, cardinal, …), or a .vox path');
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
    // ── AND ARRIVING SOMEWHERE IS NOT FALLING (user 2026-08-21: "teleporting seems to hurt the player. for
    // example typing /locate") ── P.fallPk is the HIGHEST POINT OF THE CURRENT FALL and it is a world y, so it
    // means nothing once the player is standing somewhere else: hop from a ridge at y 280 to a valley floor at
    // y 180 and the landing below computes drop = 100 voxels = 10 m and charges for it. Clearing fallT alone
    // was never enough — the damage curve in tick-body.js deliberately does NOT read fallT (see the note there
    // about staircases), it reads peak-minus-landing.
    // noFall = 1 as well, and not instead: the lift-out-of-solid loop above can leave the player up to 40
    // voxels over the ground, so there IS a real fall about to happen, and it is one the player did not ask
    // for. It is the same pair the fly toggle already sets when it drops you (ui/input.js).
    P.fallT = 0; P.fallPk = undefined; P.noFall = 1; smoothEye = P.y + EYE; resetHist = 1;
    // …and FACE IT (the creature path passes a look point; every terrain target passes nothing and is
    // untouched). An ant is one voxel tall: arriving beside it still pointing whichever way you happened to be
    // pointing is indistinguishable from arriving nowhere. Pitch is clamped inside the same +-1.55 the mouse
    // look is, and the horizontal leg is floored at 2 so a target directly underfoot cannot snap the view down.
    // …and `foot` is the height that cannot be passed in: a stick or a pinecone lies on the ground of the
    // column you are about to arrive at, and that ground is not known until P.y is set two lines above this.
    if (look) { const lx = look.x - P.x, lz = look.z - P.z, ly = look.foot ? P.y + 1 : (look.y === undefined ? P.y + EYE : look.y);
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
  let escBack = 0;                                     // an Escape press on a menu that is already up = "put me back in", fired on keyup so the browser is done with the key
  document.addEventListener('keyup', (e) => {
    if (e.code !== 'Escape' || !escBack) return;
    escBack = 0;
    if (!locked && !dead && !CMD.open) tryLock();
  });
  document.addEventListener('keydown', (e) => {
    if (!CMD.open) {
      // …and once it is closed, Escape behaves as it always did. If the re-lock was refused we are still
      // unlocked with no menu showing, so this second press is what puts the menu up (user).
      // ── AND ESCAPE COMES BACK IN (user 2026-08-16: "let esc bring the player back into the game from having
      // been on the esc menu") ── this line used to do one thing, show the menu, which meant a second press on a
      // menu that was already up re-showed it and looked like Escape had stopped working. Which of the two it
      // means is decided by whether the menu is on screen: not showing → this is the press that opens it;
      // already showing → the player is asking to return, so ask for the pointer back. A press that lands
      // inside Chrome's ~1.25 s post-Escape lock cooldown is simply refused, and pointerlockerror puts the menu
      // straight back up, so an impatient double-tap costs nothing and the next press works.
      if (e.code === 'Escape' && !locked && !dead && performance.now() - CMD.escAt > 40 && vePanel.classList.contains('hidden')) {
        // …but the RE-LOCK is not requested here. Chrome's own Escape handling runs on this same KEYDOWN and
        // exits pointer lock unconditionally, so a lock taken on keydown was handed back a moment later and the
        // menu returned on its own — "works briefly, then displays the esc menu again" (user 2026-08-16). The
        // request is moved to keyup below, which is still a user gesture but lands after the browser has
        // finished with the key. Only the menu-opening half of the press belongs on keydown.
        if (lockEl.classList.contains('hidden')) lockEl.classList.remove('hidden');
        else escBack = 1;                                  // armed here, fired on keyup
      }
      return;
    }
    e.stopPropagation();                               // every keystroke belongs to the line, never to the game's binds
    if (e.code === 'Escape') { e.preventDefault(); CMD.escAt = performance.now(); cmdShow(false); }
    else if (e.code === 'Enter' || e.code === 'NumpadEnter') { e.preventDefault(); const v = cmdBuf; cmdShow(false); cmdRun(v); }
    else if (e.code === 'Backspace') { e.preventDefault(); cmdBuf = cmdBuf.slice(0, -1); cmdDraw(); }
    else if (e.key && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) { e.preventDefault(); cmdBuf += e.key; cmdDraw(); }
  }, true);
