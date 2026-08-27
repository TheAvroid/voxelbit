  // ── BOW DRAW (user) ── the right button down pulls 00→02 and HOLDS at 02; releasing runs 03→06 out
  // and returns to rest. Stepped at 24 fps like every other animation here — a frame is picked, not
  // interpolated, so the bow reads as drawn art rather than a tween.
  const BOW_DRAW_MS = 260;                             // 00 -> 02, the pull
  const BOW_REL_MS = 130;                              // the loose and the return to rest — TWICE the speed of the pull (user), which is BOW_DRAW_MS
  let shownIt = 0;                                     // what the hand is actually drawing (see __vb.heldShown)
  let bowT0 = -1e9, bowRel = -1e9;                     // when the draw started / when it was loosed
  let bowLoosed = false;                               // true from the loose until the bow settles back to rest — the frames without the arrow
  const BOW_PULLV = [0, 2, 4, 2, -1, 0];               // the string's travel per draw frame, in VOXELS: base.vox's own depths (3,5,7,5,2,3) minus its resting 3
  const bowPullV = (now) => {                          // how far back the string has drawn the arrow, in voxels
    if (!BOW_FRAMES) return 0;
    const f = bowFrame(now);
    return BOW_PULLV[Math.min(f, BOW_PULLV.length - 1)] || 0;
  };
  const bowAtRest = (now) => !mouse2 && !((now - bowRel) / BOW_REL_MS < 1);   // …the strip is back at frame 0 and the bow is nocked again
  const bowFrame = (now) => {                          // which frame of the strip to show; 0 is at rest
    if (!BOW_FRAMES) return 0;
    if (bowLock >= 0) return Math.min(BOW_FRAMES - 1, bowLock);   // …unless a frame is PINNED for tuning (, and .) — the draw holds still so the arrow can be placed on it (user)
    if (mouse2) { const k = Math.min(1, (now - bowT0) / BOW_DRAW_MS); return Math.min(2, Math.floor(k * 3)); }   // pull to 02 and HOLD there (user)
    const e = (now - bowRel) / BOW_REL_MS;
    if (e < 0 || e >= 1) return 0;                     // long since loosed — back at rest
    return Math.min(BOW_FRAMES - 1, 3 + Math.floor(e * (BOW_FRAMES - 3)));   // …then run 03..06 out (user)
  };
  const AUTO_PICK_R = 16;                              // AUTO-PICKUP radius (vox) — walk this close and a FREE hand slot vacuums the item up
  function autoPickup() {                              // proximity grab: RADIAL, not the view-ray march tryPickup() uses — you never have to aim at it
    if (grabAnim || dead) return;                      // one flight at a time — startGrab would clobber the item already in the air
    // ── IT TOPS UP A STACK NOW (user 2026-08-20: "I can pick up the steak if I have an empty hand, but its not
    // stacking to the steak in hand like it used to") ── this was `slots.some((s) => !s)`: only an EMPTY slot
    // auto-grabbed, and topping up an existing stack was a deliberate right-click. That rule was written when
    // the hotbar had spare slots. SLOT_MAX is 5 and the player now spawns with axe, pick, shovel and bow, so
    // the moment anything lands in the fifth slot there is no empty slot left and this early return killed
    // auto-pickup ENTIRELY — walk over a steak while holding steaks and nothing happens, which is exactly the
    // report. The question is asked PER DROP now, with canAdd, which is the same test tryPickup's own drop
    // loop uses: a drop is grabbed if there is anywhere for it to go, whether that is an empty slot or a stack
    // of its own kind with room. Nothing else can auto-grab that could not before — canAdd is false for a full
    // hotbar with no matching stack, which is what the old line was really trying to say.
    const tNow = performance.now();
    let bi = -1, bd = AUTO_PICK_R * AUTO_PICK_R;       // nearest wins, so a pile drains one item per flight instead of grabbing at random
    for (let i = 0; i < drops.length; i++) {
      const dr = drops[i];
      if (dr.T && (tNow - dr.born) / 1000 < dr.T) continue;   // still mid-toss — let it land before it can be re-grabbed (else a Q-toss boomerangs straight back)
      if (!canAdd(dr.it)) continue;                    // …and there has to be somewhere for it to go — see the note above
      const ox = dr.x + 0.5 - P.x, oy = dr.y + dropAnchor(dr) - smoothEye, oz = dr.z + 0.5 - P.z;
      const d2 = ox * ox + oy * oy + oz * oz;
      if (d2 < bd) { bd = d2; bi = i; }
    }
    if (bi < 0) return;
    const dr = drops[bi];
    startGrab(dr.it, dr.x + 0.5, dr.y + dropAnchor(dr), dr.z + 0.5, dr.spin === undefined ? tNow * 0.0012 + dr.ph : dr.spin, dropLevitating(dr));
    drops.splice(bi, 1);
  }
  document.addEventListener('contextmenu', (e) => e.preventDefault());
  // ── MASTER VOLUME ── every sound registers its BASE volume; the menu slider scales them all (persisted vb_vol)
  // ── AND ALL FOUR ARE REMEMBERED ACROSS A REFRESH (user 2026-08-21: "have the browser remember my settings on
  // refresh") ── every one of these four sliders already WROTE its key; none of them read it back, so the value
  // survived only until F5. That was deliberate once (the "volume-resets-to-100 rule" the comments above argue
  // for, so a fresh load always begins at full volume) and it is what the user has now asked to change, so the
  // rule is gone rather than argued with. Read with the same guarded parse vb_sens uses below: a corrupt or
  // out-of-range entry falls back to the default rather than muting the game, and localStorage in a private
  // window throws on access, which is why every one of these sits in its own try.
  const volGet = (k, d) => { try { const v = parseFloat(localStorage.getItem(k)); return (v >= 0 && v <= 1) ? v : d; } catch (e) { return d; } };
  let sfxVol = volGet('vb_sfx', 1);   // ── SFX (user 2026-08-07) ── a second bus under the master, for the sounds the WORLD MAKES: every
                    // tool, hit, footstep, bowstring, pickup and jingle. The forest loop is the one thing it does not
                    // touch — an SFX slider that also rode the ambience would just be the master slider twice over.
                    // Resets to 100% on refresh and persists to vb_sfx, matching sndVol exactly rather than inventing a
                    // second rule for two sliders sitting in the same box (see the volume-resets-to-100 note below).
  let musVol = volGet('vb_mus', 1);   // ── MUSIC (user 2026-08-08) ── the third bus: the score, and nothing else. Same rule as the two
                    // sliders it sits under — starts at 100% on every refresh and persists to vb_mus — so the sound
                    // box has one behaviour and not three. Only the anthem rides it today (see ANTHEM_AT below).
  let ambVol = volGet('vb_amb', 1);   // ── AMBIENCE (user 2026-08-20: "add a new audio slider called ambience. this adjusts the ambience
                    // sound. song birds/wind etc. put it under music") ── the fourth bus, and the one BUS_AMB has
                    // been riding without since it was named: busVol answered a literal 1 for it, so the forest
                    // and desert beds moved only with the master. Same rule as the two above it — 100% on every
                    // refresh, persisted to vb_amb — so the sound box has one behaviour and not four.
                    // It reaches the desert bed as well as the forest one for free: both register on BUS_AMB and
                    // ambBiomeTick recomputes their levels through sndLevel every frame, so a drag moves the bed
                    // that is playing right now rather than the next one to start.
  let sndVol = volGet('vb_vol', 0.8);   // ── AND IT STARTS AT 80% (user 2026-08-26: "turn the default sound to 80%") ── the DEFAULT only, so a player who has ever touched the slider keeps their own number: volGet returns the saved vb_vol when there is one   // ── SOUND IS BACK ON (user 2026-08-20: "turn the volume on by default") ── it was muted earlier the same day ("turn off volume by default"), which was itself a repeat of 2026-08-18, and this is the third time the switch has moved. 1 is the 2026-08-06 full-volume start. Like the two buses above it this is the value on every REFRESH, not a
                    // preference: a saved vb_vol is written by the slider and re-read there, so a player who turns it down
                    // still gets their setting for that session — this only decides where a fresh load begins.
  // ── MOUSE LOOK SENSITIVITY ── slider 0..100% maps linearly onto the yaw/pitch multiplier; 50% == the tuned default (0.0022 rad/px), 100% == 2x (persisted vb_sens)
  let lookSens = 0.3; try { const v = parseFloat(localStorage.getItem('vb_sens')); if (v >= 0 && v <= 1) lookSens = v; } catch (e) {}   // BASE sensitivity 30% (user); a saved vb_sens still overrides
  const lookMul = () => 0.0044 * lookSens;             // 0.5 → 0.0022; keeps the historical feel dead-centre on the slider
  const sndReg = [];
  const SND_GAIN = 1.25;                               // master loudness trim — every registered sound, +25% overall (user 2026-07-17); clamp keeps HTMLAudio's 0..1 legal
  // ── ONE LEVEL FUNCTION ── three places used to spell the same product out by hand (register, re-apply,
  // and the pool's per-play recompute), so a second bus would have had to be remembered in all three. Now the
  // chain lives once: base x master trim x volume slider x this sound's own bus x this shot's own gain.
  // ── AND THE BUS IS A NUMBER, NOT A SECOND BOOLEAN (user 2026-08-08, adding music) ── with two flags
  // "not sfx" would have meant the forest bed AND the score at once, and applyVol could no longer tell which
  // slider owns a sound. Every registered sound names exactly one bus instead.
  const BUS_AMB = 0, BUS_SFX = 1, BUS_MUS = 2;
  const busVol = (bus) => bus === BUS_SFX ? sfxVol : bus === BUS_MUS ? musVol : ambVol;   // …and BUS_AMB has its own slider now, where it used to ride the master alone
  const sndLevel = (base, bus, gain) => Math.min(1, base * SND_GAIN * sndVol * busVol(bus) * (gain === undefined ? 1 : gain));
  const regBus = (a, base, bus) => { a.volume = sndLevel(base, bus); sndReg.push({ a, base, bus }); return a; };
  const regSnd = (a, base, sfx) => regBus(a, base, sfx === false ? BUS_AMB : BUS_SFX);   // SFX unless a caller opts out — a new sound is an effect until someone says otherwise, so nothing can silently escape the slider
  const regMus = (a, base) => regBus(a, base, BUS_MUS);   // …and music says so outright: master x music slider, never the sfx one
  const applyVol = () => { for (const s of sndReg) s.a.volume = sndLevel(s.base, s.bus); };
  // ── ONE AudioContext FOR THE WHOLE GAME ── createMediaElementSource may be called ONCE per media element,
  // ever. The video-editor tap (veTapEl) already claims every registered sound, so a second context for effects
  // would win the race on some elements and lose it on others — and whichever lost would go MISSING from
  // exported clips. Sharing the context also lets a filtered sound feed the recorder's destination, which nodes
  // from two different contexts could never do.
  let sfxAC = null;
  const audioCtx = () => {
    if (!sfxAC) { try { sfxAC = new (window.AudioContext || window.webkitAudioContext)(); } catch (e) { sfxAC = null; } }
    if (sfxAC && sfxAC.state === 'suspended') { try { sfxAC.resume(); } catch (e) {} }
    return sfxAC;
  };
  // Route an element through a LOW-SHELF and remember the output node. Everything below the corner is lifted,
  // so the hit lands heavier without landing louder — the element's own `volume` is pre-graph and still carries
  // the master/settings level. Built lazily on first play: by then the click-to-enter gesture has happened, so
  // the context is running rather than suspended (a suspended context would play the element silently).
  const bassTap = (a, dB, hz) => {
    if (a._sfxOut) return true;
    const ac = audioCtx(); if (!ac) return false;
    try {
      const src = ac.createMediaElementSource(a);
      const f = ac.createBiquadFilter(); f.type = 'lowshelf'; f.frequency.value = hz; f.gain.value = dB;
      src.connect(f); f.connect(ac.destination);
      a._sfxOut = f; return true;
    } catch (e) { return false; }   // no Web Audio here — the element still plays dry rather than not at all
  };
  let ambBiomeTick = () => {};                          // set by the ambience block below; called once a frame from the camera tick
  let desAmbState = () => ({ phase: 'idle' });          // …and the desert bed's own test tap, assigned in the same block (see __vb.desAmb)
  // ── forest ambience ── constant background loop from page load. Browsers block autoplay before the first
  // user gesture, so a blocked play() is retried on the first pointer/key input (the click-to-enter covers it).
  // Tries forest_ambience.* first and falls back to ambience.mp4 so dropping the new file in just works.
  {
    const amb = new Audio();
    amb.loop = true; regSnd(amb, 0.625, false);        // +25% 2026-07-16 (was 0.5). NOT an effect: this is the bed everything else sits on, so only the master slider moves it
    const srcs = ['sound/forest_ambience.mp4', 'sound/forest_ambience.mp3', 'sound/forest_ambience.wav', 'sound/ambience.mp4'];
    let si = 0;
    const ambPlay = () => { try { const p = amb.play(); if (p) p.catch(() => {}); } catch (e) {} };
    amb.addEventListener('error', () => { if (si < srcs.length) { amb.src = srcs[si++]; ambPlay(); } });
    amb.src = srcs[si++]; ambPlay();
    // ── THE DESERT'S OWN BED (user 2026-08-15) ── the exact counterweight of the forest one, in the same block
    // so the two can share one weight: the forest plays at (1 - desertM) and the desert at desertM, which sum
    // to 1 everywhere. That is what makes the handover provable rather than tuned — there is no crossing where
    // both are at full, and none where neither is playing.
    //
    // ── WHY WEB AUDIO, AND NOT A SECOND <audio loop> ── the ask was a PALINDROME: play the take, then play it
    // BACKWARDS, then forwards again, for ever, so the recycle is smooth. A media element cannot run backwards
    // at all (playbackRate must be positive), and plain loop=true clicks, because the last sample of a wind
    // recording and its first sample are two unrelated points. So the file is fetched and decoded ONCE and
    // rebuilt as a single buffer holding s[0..N-1] followed by s[N-2..1]; looping THAT natively is the
    // palindrome. Both of its joins are sample-exact reflections — the turn reads s[N-2], s[N-1], s[N-2] and
    // the wrap reads s[1], s[0], s[1] — so there is no discontinuity to hear, and no scheduler to drift,
    // because the browser's own loop does the recycling. The endpoints are visited once each, not twice, which
    // is why the buffer is 2N-2 frames and not 2N.
    //
    // COST, stated plainly: the palindrome is ~50 MB of float for this 66 s stereo take (~75 MB for the ~0.2 s
    // the decoded source is still alive beside it). It is built lazily, only if the player actually goes near
    // the sand, and never twice; the copy is sliced across frames so it is never one long stall.
    const DES_AMB_BASE = 0.1425;   // +25% (user 2026-08-16), 0.114 -> 0.1425. The 0.114 was the LUFS-matched level; this is a deliberate lift above the match, not a re-measurement                        // MEASURED, not guessed: the take is -27.5 LUFS integrated against the forest bed's -42.3, so 0.625 * 10^(-14.8/20) = 0.114 lands the two at the same perceived level. A crossfade between beds of different loudness reads as a volume ramp, which is the one thing the handover must not do
    const DES_AMB_SRCS = ['sound/desert_ambience.mp4', 'sound/desert_ambience.mp3', 'sound/desert_ambience.wav'];
    const DES_AMB_NEAR = 0.02;                         // start FETCHING at the first hint of sand — desertM > 0 covers the whole 450-voxel blend band, so the decode and the rebuild finish long before the bed is loud enough to notice arriving
    const DES_AMB_CHUNK = 1 << 20;                     // frames of palindrome copied per rendered frame. The whole 12.6 M-sample copy in one go is a 30-50 ms stall (a dropped frame); a slice this size is ~2 ms
    let dAmbLvl = 0;                                   // what applyVol / the tick last asked for — the shim's own `volume`
    let dAmbG = null, dAmbSrc = null, dAmbFwd = null, dAmbPal = null;
    let dAmbCh = 0, dAmbI = 0, dAmbSide = 0, dAmbBuild = false;
    let dAmbT0 = 0, dAmbHalf = 0, dAmbSi = 0, dAmbUrl = null, dAmbPhase = 'idle';
    // A GainNode has no `volume`, so the master slider could never reach one. This shim hands sndReg exactly the
    // property it writes, and carries `_sfxOut` so the video recorder's tap (veTapEl) finds the gain node and
    // mixes the desert into an export — the same door a bass-filtered effect already goes through.
    const dAmbEl = { _sfxOut: null,
      get volume() { return dAmbLvl; },
      set volume(v) { dAmbLvl = v; if (dAmbG) dAmbG.gain.value = v; } };
    regSnd(dAmbEl, DES_AMB_BASE, false);               // BUS_AMB like the forest bed: a bed is not an effect, so only the master slider moves it
    // Fetch → decode → hand the buffer to the staged rebuild. Every step is async and every failure walks to the
    // next candidate file, so a missing or undecodable take costs silence rather than an exception on the frame
    // the player first sees sand.
    const dAmbLoad = () => {
      if (dAmbPhase !== 'idle') return;
      const ac = audioCtx(); if (!ac) { dAmbPhase = 'nowebaudio'; return; }
      dAmbPhase = 'fetch'; dAmbUrl = DES_AMB_SRCS[dAmbSi++];
      fetch(dAmbUrl).then((r) => { if (!r.ok) throw new Error('http ' + r.status); return r.arrayBuffer(); })
        .then((ab) => { dAmbPhase = 'decode'; return ac.decodeAudioData(ab); })
        .then((buf) => {
          if (buf.length < 4) throw new Error('too short');
          dAmbFwd = buf; dAmbPal = ac.createBuffer(buf.numberOfChannels, buf.length * 2 - 2, buf.sampleRate);
          dAmbCh = 0; dAmbI = 0; dAmbSide = 0; dAmbBuild = true; dAmbPhase = 'build';
        })
        .catch(() => { dAmbPhase = dAmbSi < DES_AMB_SRCS.length ? 'idle' : 'failed'; });   // 'idle' re-arms the next tick on the NEXT candidate; 'failed' is terminal and never retried, so a 404 cannot become a fetch every frame
    };
    // One slice of the palindrome, driven from the tick. Typed-array ops only (set / slice / reverse), never an
    // element loop — the same 4 MB of copying is several times cheaper through them.
    const dAmbStep = () => {                           // true once the whole buffer is written
      const N = dAmbFwd.length, C = dAmbFwd.numberOfChannels, M = N - 2;
      let left = DES_AMB_CHUNK;
      while (left > 0 && dAmbCh < C) {
        const s2 = dAmbFwd.getChannelData(dAmbCh), o2 = dAmbPal.getChannelData(dAmbCh);
        if (dAmbSide === 0) {                          // the outward half, s[0..N-1], straight through
          const n = Math.min(left, N - dAmbI);
          if (n > 0) o2.set(s2.subarray(dAmbI, dAmbI + n), dAmbI);
          dAmbI += n; left -= n;
          if (dAmbI >= N) { dAmbSide = 1; dAmbI = 0; }
        } else {                                       // …and the return, o[N+j] = s[N-2-j], stopping one short of s[0] so the wrap reflects instead of repeating a sample
          const n = Math.min(left, M - dAmbI);
          if (n > 0) { const c = s2.slice(N - 1 - dAmbI - n, N - 1 - dAmbI); c.reverse(); o2.set(c, N + dAmbI); }
          dAmbI += n; left -= n;
          if (dAmbI >= M) { dAmbSide = 0; dAmbI = 0; dAmbCh++; }
        }
      }
      return dAmbCh >= C;
    };
    const dAmbGo = () => {
      dAmbBuild = false;
      const ac = audioCtx(); if (!ac) { dAmbPhase = 'failed'; return; }
      try {
        dAmbG = ac.createGain(); dAmbG.gain.value = dAmbLvl; dAmbG.connect(ac.destination);
        dAmbEl._sfxOut = dAmbG;                        // from here the recorder's tap has something to grab
        dAmbSrc = ac.createBufferSource(); dAmbSrc.buffer = dAmbPal; dAmbSrc.loop = true; dAmbSrc.connect(dAmbG);
        dAmbHalf = (dAmbFwd.length - 1) / dAmbFwd.sampleRate;   // one PASS, in seconds: half the palindrome. Read BEFORE the source buffer is let go
        dAmbT0 = ac.currentTime; dAmbSrc.start();      // started at gain 0 unless the player is already on sand — it runs for the rest of the session and only the gain ever moves, exactly as the forest loop does
        dAmbFwd = null;                                // the decoded take now lives inside the palindrome; holding it as well would be 25 MB for nothing
        dAmbPhase = 'play';
      } catch (e) { dAmbPhase = 'failed'; }
    };
    // Which pass is running, DERIVED from the audio clock rather than counted: the source never restarts and
    // never fires an event, so there is nothing to count — and a derived answer cannot drift from what is
    // actually being heard.
    const dAmbPass = () => {
      if (dAmbPhase !== 'play' || !(dAmbHalf > 0)) return null;
      const ac = sfxAC; if (!ac) return null;
      const el = Math.max(0, ac.currentTime - dAmbT0), n = Math.floor(el / dAmbHalf);
      return { pass: n, dir: (n & 1) ? 'rev' : 'fwd', t: el - n * dAmbHalf, el };
    };
    // The palindrome is checked by REFLECTION, not against the source take — the decoded take is released
    // the moment the rebuild lands, so it is not there to compare with. It does not need to be: writing
    // o[i] = s[i] below N and o[i] = s[L-i] above it makes o[i] === o[L-i] true for every i in 1..L-1, and
    // THAT is the whole claim — mirrored about the turn at N-1 and about the wrap at 0. A sampled check is
    // enough because a build that went wrong went wrong in bulk (a slice at the wrong offset, a chunk
    // boundary off by one), never in one lone sample.
    const dAmbProbe = (n) => {
      if (!dAmbPal || dAmbPhase !== 'play') return null;
      const L = dAmbPal.length, C = dAmbPal.numberOfChannels;
      let bad = 0, worst = 0;
      for (let c = 0; c < C; c++) {
        const o2 = dAmbPal.getChannelData(c);
        for (let k = 0; k < n; k++) {
          const i = 1 + ((Math.random() * (L - 1)) | 0);
          const d = Math.abs(o2[i] - o2[L - i]);
          if (d > 0) { bad++; if (d > worst) worst = d; }
        }
      }
      return { n: n * C, bad, worst };
    };
    desAmbState = (probe) => {
      const p = dAmbPass(), ac = sfxAC;
      return { phase: dAmbPhase, url: dAmbUrl, dm: +Math.max(0, Math.min(1, desertM(P.x, P.z))).toFixed(3),
               gain: +dAmbLvl.toFixed(5), forest: +amb.volume.toFixed(5), playing: dAmbPhase === 'play', audible: dAmbLvl > 0,
               pass: p ? p.pass : null, dir: p ? p.dir : null, passT: p ? +p.t.toFixed(3) : null,
               half: +dAmbHalf.toFixed(3), elapsed: p ? +p.el.toFixed(3) : null,
               frames: dAmbPal ? dAmbPal.length : 0, ch: dAmbPal ? dAmbPal.numberOfChannels : 0,
               mb: dAmbPal ? +(dAmbPal.length * dAmbPal.numberOfChannels * 4 / 1048576).toFixed(1) : 0,
               loop: !!(dAmbSrc && dAmbSrc.loop), dur: dAmbPal ? +(dAmbPal.length / dAmbPal.sampleRate).toFixed(3) : 0,
               probe: probe > 0 ? dAmbProbe(probe | 0) : null,
               ctx: ac ? ac.state : null, ctxT: ac ? +ac.currentTime.toFixed(3) : null };
    };
    // ── NO FOREST BIRDSONG OVER THE SAND (user 2026-08-15) ── the ambience bed is a PINE FOREST recording, so
    // it has no business playing in the desert. Faded rather than stopped: the loop keeps running and only its
    // gain moves, so walking back into the trees brings it in smoothly and the element never has to re-buffer
    // or re-hit the autoplay block. Cross-faded over the blend band itself (1 - desertM), which is the same
    // weight the ground colour and the height use, so the sound follows the treeline you can actually see.
    // Re-applied every frame on purpose: applyVol() rewrites every registered element when a slider moves and
    // would otherwise restore full forest volume in the middle of the desert.
    ambBiomeTick = () => {
      const dm = Math.max(0, Math.min(1, desertM(P.x, P.z)));
      const v = sndLevel(0.625, BUS_AMB) * (1 - dm);
      if (amb.volume !== v) amb.volume = v;            // compared against the ELEMENT and not a remembered value: applyVol() writes it behind our back, and a remembered one would then agree with itself while the forest played at full volume in the middle of the desert
      if (dm > DES_AMB_NEAR) dAmbLoad();               // …the sand is in sight: start the fetch, once, ever
      if (dAmbBuild && dAmbStep()) dAmbGo();           // …and rebuild the palindrome a slice at a time until it is whole
      const dv = sndLevel(DES_AMB_BASE, BUS_AMB) * dm;   // the other side of the same weight: the two gains sum to one, so a step into the sand is a handover and never a gap or a doubling
      if (dAmbLvl !== dv) dAmbEl.volume = dv;
    };
    const kick = () => { if (amb.paused) ambPlay(); if (dAmbPhase !== 'idle') audioCtx(); };   // …and un-suspend the context the desert bed plays through, which the browser also holds until a gesture
    document.addEventListener('pointerdown', kick);
    document.addEventListener('keydown', kick);
  }
  // ── THE ANTHEM (user 2026-08-08) ── sound/music/red_carpet.mp4, built from source/audio/anthem/red carpet.wav
  // by the recipe every other sound ships under (AAC-LC 320k, 48 kHz stereo, .mp4 container): the 19 MB of 24-bit
  // PCM it started as would otherwise be the largest single file the game downloads, by a factor of seven.
  // BASE 0.14 — measured, the track is -15.2 LUFS integrated, within 0.1 dB of high_score.mp4's -15.1, so the
  // achievement jingle's own twice-tuned 0.09 is the anchor: this sits ~4 dB over it (a moment, not a cue) and
  // still under the tool hits. On the MUSIC bus, so nothing but the master and the new slider can move it.
  const ANTHEM_AT = 120;                               // seconds of GAMEPLAY before the FIRST track (user 2026-08-26: "have the music start 120 seconds after the game has refreshed") - 60 -> 120, which also makes it equal to ANTHEM_GAP, so the opening silence and every silence after it are the same two minutes — not of page life, see the play clock in tickBody (user 2026-08-20: 120 -> 60 -> 30 -> 10 -> 3, and the clock no longer starts on its own — see anthemArmed)
  const ANTHEM_GAP = 120;                              // …and TWO minutes of gameplay of SILENCE between one track ending and the next starting (user 2026-08-24: "make sure the songs have 120 seconds between them", raised from 60). Gameplay seconds, not wall clock: the play clock in tickBody stops with the game, so a track cannot come due while the esc menu is up
  // ── THE SET (user 2026-08-08, re-cut 2026-08-18) ── the soundtrack, in rotation, then silence for the rest
  // of the session. It moved from game/sound/music/*.mp4 to game/sound/soundtrack/*.mp3 and gained two cuts,
  // `ceremony` and `winner`, so it is SIX now rather than four.
  //
  // ── THE GAIN IS ONE NUMBER NOW, AND THAT IS THE POINT ── it used to be four different numbers, because the
  // four files ranged over 5 dB of integrated loudness and each needed its own correction. The files are
  // levelled at the SOURCE now (user 2026-08-18: "make sure all of the songs in the soundtrack have the same
  // dB level"): every one was two-pass `loudnorm`-ed to I=-16 LUFS / TP=-1.5 dBTP and measures -16.2 to the
  // tenth, so a per-track gain would be correcting a difference that no longer exists.
  //     measured after processing: achievement / award / ceremony / red_carpet / to_glory / winner
  //                                  all -16.2 LUFS, spread 0.0 LU
  // 0.159 CARRIES THE OLD LEVEL FORWARD rather than being a fresh guess: the old anchor was 0.125 at -14.1
  // LUFS, the files are now -16.2, i.e. 2.1 dB quieter, and 0.125 * 10^(2.1/20) = 0.159. So the score plays at
  // exactly the loudness it did before, which is the number that was tuned by ear against high_score.mp4.
  // If a future track is added, level it to -16 with the same recipe and it inherits this gain — that is what
  // levelling at the source buys.
  //
  // HALF A SECOND OF SILENCE TOPS AND TAILS EVERY CUT (user 2026-08-18). The sources ran from 0.46 s to 2.40 s
  // of lead-in; each was trimmed to nothing and then padded back to exactly 0.5 s, so every track opens and
  // closes on the same beat instead of on whatever its export happened to leave.
  const ANTHEM_GAIN = 0.159;
  const ANTHEM_SET = [['red_carpet', ANTHEM_GAIN], ['achievement', ANTHEM_GAIN], ['award', ANTHEM_GAIN],
                      ['to_glory', ANTHEM_GAIN], ['ceremony', ANTHEM_GAIN], ['winner', ANTHEM_GAIN],
                      ['nomination', ANTHEM_GAIN]];   // matched to the set at -16.2 LUFS / 44.1k / 192k with 0.5 s pads, so it shares ANTHEM_GAIN
  // ── SHUFFLED EVERY SESSION (user 2026-08-17: "can you re-order the songs that play") ── the set used to run
  // in the order it is written above, every session, so the same track always opened and `to_glory` was only
  // ever heard by someone who played for a solid five minutes. A Fisher-Yates over a COPY, so ANTHEM_SET itself
  // stays the documented reference order the loudness table above is written against.
  // THE GAIN TRAVELS WITH THE NAME, which is the whole reason this shuffles the PAIRS rather than the names:
  // the four gains are not decorative, they are ebur128 measurements levelling -19.1 LUFS against -14.1, and
  // pairing `award` with red_carpet's 0.140 would play it 4 dB quiet.
  // Math.random is right here and a seed would be wrong: this is presentation, not world generation. It is
  // read once at load, touches nothing the generator or a test hashes, and deliberately differs run to run.
  // ── …EXCEPT `award`, WHICH ALWAYS OPENS (user 2026-08-19: "play award.mp3 first in the playlist. shuffle
  // everything else") ── so it is pulled OUT before the shuffle and put back at the front, rather than shuffled
  // and then swapped into place: swapping would displace whichever track the shuffle had put first, which is a
  // second, silent rule about the order. The filter is by NAME and keeps the pair intact, so award still
  // carries its own measured gain (see the note above) and a rename cannot quietly drop the pin — if the name
  // ever stops matching, the set simply shuffles whole and nothing breaks.
  const ANTHEM_FIRST = 'award';
  const anthemPin = ANTHEM_SET.filter((p9) => p9[0] === ANTHEM_FIRST);
  const anthemOrder = ANTHEM_SET.filter((p9) => p9[0] !== ANTHEM_FIRST);
  for (let i9 = anthemOrder.length - 1; i9 > 0; i9--) { const j9 = (Math.random() * (i9 + 1)) | 0;
    const t9 = anthemOrder[i9]; anthemOrder[i9] = anthemOrder[j9]; anthemOrder[j9] = t9; }
  anthemOrder.unshift(...anthemPin);
  const anthemSnds = anthemOrder.map(([n9, g9]) => regMus(new Audio('sound/soundtrack/' + n9 + '.mp3'), g9));   // soundtrack/*.mp3, not music/*.mp4 — see the set above. The old .mp4s are left in place; nothing reads them.
  console.log('[vb] anthem order:', anthemOrder.map((t9) => t9[0]).join(' -> '));
  const anthemSnd = anthemSnds[0];                     // the first cut, still named for the taps that read it
  // ── THE CLOCK DOES NOT START UNTIL THE PLAYER PRESSES 1 (user 2026-08-20: "make it where the first song
  // starts when the user presses 1. 3 seconds after the player presses 1") ── the play clock used to run from
  // the moment the player took the controls, so the opening track was tied to nothing the player did. Armed by
  // the 1 key instead (ui/input.js), and ANTHEM_AT is the 3 seconds between the press and the music.
  // ── ARMED FROM THE START AGAIN (user 2026-08-21: "have the songs play regularly now. after 60 seconds, then
  // the first song plays") ── this was a latch the 1 key set (2026-08-20), then one a recording start set
  // (2026-08-21 morning); both triggers are gone and the clock simply runs. It still ticks only while `locked`
  // (main/tick-body.js), so the 60 s is 60 s of GAMEPLAY — the count does not run down behind the start prompt
  // or the esc menu. Everything after the first cut is unchanged: ANTHEM_GAP still spaces them.
  let anthemArmed = true;
  let playSecs = 0, anthemDone = false;                // the play clock, and the set-is-finished latch
  let anthemIdx = 0, anthemNextAt = ANTHEM_AT;         // which cut comes next, and the play-clock time it is due
  const playAnthem = () => {
    const a9 = anthemSnds[anthemIdx++];
    anthemNextAt = Infinity;                           // nothing is due while one is playing — the ended handler below schedules the next
    if (anthemIdx >= anthemSnds.length) anthemReshuffle(a9);   // that was the LAST of the set — draw a fresh order and keep going, rather than latching anthemDone and going silent for the session
    try { a9.currentTime = 0; const p9 = a9.play(); if (p9) p9.catch(() => { if (!anthemDone) anthemNextAt = playSecs + ANTHEM_GAP; }); } catch (e) { if (!anthemDone) anthemNextAt = playSecs + ANTHEM_GAP; }
  };
  // A refused or failed play must not stall the set: the catch above re-arms the next one on the same gap it
  // would have got anyway. `ended` is the normal path — the gap is measured from the moment a track FINISHES,
  // so it is a minute of silence rather than a minute of overlap.
  for (const a9 of anthemSnds) a9.addEventListener('ended', () => { if (!anthemDone) anthemNextAt = playSecs + ANTHEM_GAP; });
  // ── THE RECORDER STARTS THE MUSIC NOW, AND THE 1 KEY IS GONE (user 2026-08-21: "when the player presses r
  // to record, the award song plays 3 seconds later. remove the 1 keybind" ... "if the user presses r again in
  // the same session, the award song plays again and so on") ── the soundtrack is scoring the CLIP, so the
  // thing that starts it is the thing that starts the clip.
  // ── STALE FROM HERE TO THE RESHUFFLE NOTE (flagged 2026-08-26) ── the paragraphs above and below describe a
  // 1-key trigger and a recorder trigger, and BOTH ARE GONE: ui/video-editor.js does not mention the anthem at
  // all any more, anthemArmed is simply true from the start, and the only live use of ANTHEM_AT is the first
  // track of the session. In particular ANTHEM_AT is NOT "3 seconds between a trigger and the first note" - it
  // is 120 seconds of gameplay from the moment the player takes the controls. Kept for the history, not as a
  // description of the code.
  // CALLED FROM veStartRec, NOT FROM A KEY, so the #veRecBtn button arms it exactly as R does — the trigger is
  // "a recording began", and there are two ways to begin one.
  // IT ALWAYS OPENS ON AWARD, and anthemIdx = 0 IS that, by construction rather than by name: ANTHEM_FIRST is
  // pulled out of the shuffle and unshifted to the front of anthemOrder (see above), so slot 0 is award every
  // session. The rest of the set follows it on the usual ANTHEM_GAP, in this session's shuffled order.
  // AND IT IS A RESET, EVERY TIME — that is the second half of the request. A second recording in the same
  // session gets award again, not "the next song in rotation", so anthemIdx goes back to 0 and the spent-set
  // latch is cleared. Anything still playing from the last take is stopped first: without that a track from
  // recording #1 would still be running when recording #2's award starts and the two would sound over each
  // other. currentTime is rewound with the pause so a stopped track does not resume mid-phrase if the rotation
  // reaches it again later.
  // ── AND WHEN THE SET RUNS OUT IT RESHUFFLES INSTEAD OF STOPPING (user 2026-08-21: "then a reshuffle") ──
  // playAnthem latched anthemDone the moment the last cut STARTED, and tickBody's clock is gated on !anthemDone,
  // so a long session went permanently silent after one pass of seven tracks. This draws a fresh order and keeps
  // going. Fisher-Yates over BOTH arrays with the SAME permutation: anthemOrder carries the names the debug tap
  // reports and anthemSnds the Audio elements, so shuffling one and not the other would report every track under
  // a neighbour's name. The elements themselves are reordered, never rebuilt — their `ended` listeners ride along.
  // AWARD IS NOT RE-PINNED. ANTHEM_FIRST is about how a SESSION opens (user 2026-08-19: "play award.mp3 first in
  // the playlist. shuffle everything else"); re-pinning it every cycle would make the one track you are certain
  // to have heard already the one you hear most.
  const anthemReshuffle = (justPlayed) => {
    for (let i9 = anthemOrder.length - 1; i9 > 0; i9--) {
      const j9 = (Math.random() * (i9 + 1)) | 0;
      let t9 = anthemOrder[i9]; anthemOrder[i9] = anthemOrder[j9]; anthemOrder[j9] = t9;
      t9 = anthemSnds[i9]; anthemSnds[i9] = anthemSnds[j9]; anthemSnds[j9] = t9;
    }
    // …and the new cycle never opens on the cut that is playing RIGHT NOW: a 1-in-7 immediate repeat is the one
    // ordering a listener would actually notice, and a single swap removes it without biasing anything else.
    if (justPlayed && anthemSnds[0] === justPlayed && anthemSnds.length > 1) {
      let t9 = anthemOrder[0]; anthemOrder[0] = anthemOrder[1]; anthemOrder[1] = t9;
      t9 = anthemSnds[0]; anthemSnds[0] = anthemSnds[1]; anthemSnds[1] = t9;
    }
    anthemIdx = 0; anthemDone = false;
    console.log('[vb] anthem RESHUFFLE:', anthemOrder.map((t9) => t9[0]).join(' -> '));
  };
  const SWAP_MS = 240;                                 // how long a tool swap takes to rise back into frame — short enough that it never delays a swing
  let swapT0 = -1e9, prevHeldIt = -1, prevHeldSlot;    // …and when the current one started (see the view-model block). prevHeldSlot is the SLOT OBJECT the hand was last drawing from — undefined to start, so the first frame with anything in hand counts as a change   // …and when the current one started (see the view-model block)
  let lSwapT0 = -1e9, prevLeftIt = -1;                 // …and the OFF hand's own pair (user 2026-08-19: the left hand should share the right's mechanics). Its own clock, because the two hands change item independently — a craft pair forming swaps the LEFT hand while the right keeps what it was holding
  let swingStart = -1e9, mouse0 = false, mouse2 = false;   // mouse2: RIGHT button HELD — the wind-up for a rock throw (right-hold, then left-click)               // left-click SWING — Minecraft-style forward chop; HOLDING the button keeps swinging
  let pendKillT = 0;                                   // pending creature-hit: armed at swing start, FIRES ~250 ms in — when the axe visually LANDS on screen (user: only register the hit when the axe hits), not at the click
  const swishSnd = regSnd(new Audio('sound/bow/swish.mp4'), 0.36);   // swing whoosh — cut 40% (user), then a further 40% (0.6 → 0.36)
  const playSwish = () => { try { swishSnd.currentTime = 0; const pr = swishSnd.play(); if (pr) pr.catch(() => {}); } catch (err) {} };
  // ── GAME OVER (user 2026-08-17) ── one voice, SFX bus, fired from die() in input.js so every death routes
  // through it exactly as the game-over screen does. The file shipped as game_over.wav and nothing referenced
  // it; converted to AAC .mp4 like every other sound here (48 kHz stereo, 320 k), which also dropped the cover
  // art the .wav was carrying - left in, ffmpeg tries to write it as a video stream and the encode fails.
  // BASE: game_over is -14.7 LUFS against swish's -20.2, so matching swish's 0.36 by loudness gives
  // 0.36 * 10^(-5.5/20) = 0.191 - and the user asked for half of it. Carried as the base rather than baked
  // into the file so the number stays visible and adjustable, the way every other level here is.
  const gameOverSnd = regSnd(new Audio('sound/game_over.mp4'), 0.0955);
  const playGameOver = () => { try { gameOverSnd.currentTime = 0; const pr = gameOverSnd.play(); if (pr) pr.catch(() => {}); } catch (err) {} };
  // ── THE BOW'S OWN VOICES (user 2026-08-07) ── all four files have shipped in sound/bow/ since the bow did;
  // only swish was ever wired, and then as the GENERIC swing whoosh for every tool, so the bow itself was
  // silent. One voice per stage of the shot: the string creaking as it is drawn, the shaft leaving, the string
  // settling and re-nocking, and the hit at the far end.
  const sndPool = (src, base, n8) => {                 // a single Audio element cannot overlap ITSELF, and arrows land in twos and threes — a shot fired before the last one lands would cut its own impact short
    const v8 = []; for (let i = 0; i < n8; i++) v8.push(regSnd(new Audio(src), base));
    let k8 = 0;
    return (gain) => { const a8 = v8[k8++ % v8.length];
      try { a8.currentTime = 0;
        a8.volume = sndLevel(base, BUS_SFX, gain);
        const p8 = a8.play(); if (p8) p8.catch(() => {}); } catch (e) {} }; };
  // ── SOMETHING BROKE (user 2026-08-07) ── the sound of a swing that actually bites: chopped wood,
  // mined stone, dug soil, tilled earth, a fern knocked apart. It replaces the swish that used to fire on
  // the SWING itself, which played whether or not the tool connected with anything — so the loudest thing
  // in a miss was the tool. BACK TO THE 4-TAKE POOL (user 2026-08-07): sound/tool_hit/00..03, picked at
  // random per strike, after a spell on the single sound/hit.mp4 — four takes keep a held chop from turning
  // into a machine gun of the identical transient. The two-element inner array still matters: a held click
  // auto-repeats every 570 ms and the takes run ~550 ms, so the tail would otherwise clip its own next hit.
  // BASE 0.362, NOT the 0.1171875 hit.mp4 had ended up at: measured, the takes are -19.8 LUFS integrated
  // against hit.mp4's -10.0, i.e. 9.8 dB quieter for the same number — which is exactly why hit.mp4 kept
  // needing to be turned down. 0.1171875 x 10^(9.8/20) = 0.362 lands the pool at the loudness the volume
  // walk-down had arrived at, so swapping the files back does not undo that tuning.
  // DOWN 25% TO 0.2715 (user 2026-08-07): loudness-matching hit.mp4 got the pool to parity, but parity was
  // still too hot in play. 0.362 x 0.75 — a linear amplitude cut, i.e. -2.5 dB, not a 25% drop in perceived
  // loudness (that would be nearer -6 dB).
  // AND 25% AGAIN TO 0.2036 (user 2026-08-07): 0.2715 x 0.75, the same linear cut a second time, leaving the
  // pool 5 dB under the loudness-matched number it started the day at. The bass shelf below went UP in the
  // same breath, which is the point of doing both at once: the strike keeps its weight while the crack
  // stops carrying the mix.
  // ── A SHUFFLE BAG, NOT A COIN (user 2026-08-26: "its like I hear the same sound multiple times in a row") ──
  // Every take pool here drew with Math.random() on each swing, which is uniform and is NOT what "varied"
  // sounds like: an independent draw repeats itself 1/n of the time, so the four generic takes said the same
  // thing twice in a row on a QUARTER of swings and the five wood takes on a fifth of them. A held chop
  // repeats every 570 ms, so that is an audible stutter every few seconds - and it was audible.
  // A bag is the fix the soundtrack already uses (see anthemReshuffle): Fisher-Yates a permutation, hand out
  // one take at a time, reshuffle when it is spent. Every take is then heard once before any is heard twice.
  // AND THE SEAM IS THE PART THAT IS EASY TO MISS. A fresh shuffle can open with the take the last bag closed
  // on, which is exactly the back-to-back repeat this exists to remove - rarer than before but not gone. So a
  // new bag whose first entry matches the last one played swaps it with the end. That cannot loop and it costs
  // one compare per reshuffle.
  // With n = 2 (the rock pair) a bag is strict alternation, which is the most variety two takes can give.
  const sndBag = (n) => {
    const order = []; for (let i = 0; i < n; i++) order.push(i);
    let at = n, last = -1;                             // start SPENT, so the first draw shuffles rather than always opening on take 0
    return () => {
      if (at >= n) {
        for (let k = n - 1; k > 0; k--) { const j = (Math.random() * (k + 1)) | 0; const t = order[k]; order[k] = order[j]; order[j] = t; }
        if (n > 1 && order[0] === last) { const t = order[0]; order[0] = order[n - 1]; order[n - 1] = t; }
        at = 0;
      }
      last = order[at++];
      return last;
    };
  };
  // ── sound/tool_hit/ IS GONE ── the four generic takes, their two-voice pool, their shuffle bag and the
  // 190 Hz bass shelf that was tuned for them all lived here. Deleted with the files (user 2026-08-26); the
  // three material families in playToolHit are the whole impact set now. routeToolHits went with them: it
  // existed solely to tap that shelf onto those eight voices before the video recorder could claim them, and
  // the rock/wood/leaf takes are deliberately not tapped (see the note below), so there is nothing left to
  // route. If a shelf is ever wanted on the material takes, re-read that note first — the ONE-SHOT
  // createMediaElementSource rule it records is the reason the old one ran on the first input event.
  // ── AND STONE HAS ITS OWN IMPACT (user 2026-08-26) ── the four takes above are the sound of something
  // COMING APART and they played for every tool on every material; a pick on rock is a different event and
  // now sounds like one. The two takes are the two hits inside the delivered rock_sounds.mp3, split on the
  // 0.42 s of silence between them (0.085-0.935 and 1.335-2.210), downmixed to mono and levelled to the same
  // -20.2 LUFS as each other. Source kept at source/audio/environment/rock_sounds.mp3, which is gitignored
  // like every other source: only the two cuts ship.
  // BASE 0.213, not a guess: the tool_hit takes are -19.8 LUFS at 0.2036, these measure -20.2, so
  // 0.2036 x 10^(0.4/20) = 0.213 puts a pick on rock at exactly the weight of an axe on a trunk. Two voices
  // per take for the same reason the takes above have them - a held click auto-repeats every 570 ms and the
  // tail must not cut itself off.
  // DELIBERATELY NOT BASS-TAPPED. The deleted generic takes carried a 10.9 dB shelf under 190 Hz, tuned
  // by ear against those four recordings; these are a different recording and are shipped as delivered. If
  // they want weight, tap them the same way - but note the ONE-SHOT createMediaElementSource rule, so
  // they would have to be routed with the others, not lazily.
  // ── RE-CUT FROM A CLEAN TAKE, AND THERE ARE FIVE (user 2026-08-26) ── the first pair came out of a
  // recording with WIND under it. MEASURED on the replacement: a steady noise floor at -48.6 dB RMS against
  // hits at -20 to -27, and band by band the wind sat below ~2.5 kHz, worst under 100 Hz (15.7 dB SNR there
  // against 39.7 above 8 kHz) while the rock's own energy peaks at 800 Hz - 2.5 kHz.
  // So it is two steps, not one: a 120 Hz high-pass takes the rumble the hit barely uses, and afftdn does the
  // 250-2500 Hz overlap that no EQ can, profiled on the 0.45 s of pure wind the recording opens with.
  // MEASURED after: wind -48.6 -> -68.9 (20.4 dB down) with the hit moving 0.17 dB. nr=20 rather than the 30
  // that was also tried - both leave the hit alone, and the quieter setting is the one less likely to warble.
  // NOTE afftdn is an FFT filter and DELAYS the signal ~50 ms, so the cut points were re-found on the CLEANED
  // file; reusing the raw timestamps shifts every take into the previous one's tail.
  // BASE 0.275, and it is peak-limited rather than loudness-matched: these are peaky (crest ~22 dB), so
  // levelling them to tool_hit's -21.5 dB RMS would have clipped. They are pushed to -1.5 dBTP instead, which
  // lands the set at -24.1 RMS, and the base makes up the 2.6 dB: 0.2036 x 10^(2.6/20).
  // ── ONE LEVEL FOR EVERY IMPACT TAKE (user 2026-08-26: "make the Db is consistent across all of the
  // impact sounds") ── the three sets were levelled to their own ceilings and carried three different bases
  // to compensate. They matched by EAR, which is not the same as matching, and the files did not: wood -21.7,
  // rock -24.1, leaf -28.4 dB RMS. Every take is now normalised to the SAME -28.5, so the eleven of them
  // measure -28.5 to -28.8 - a 0.28 dB spread - and one base serves all three.
  // WHY -28.5 AND NOT LOUDER: the target is set by the peakiest take, because none of them may pass
  // -1.5 dBTP. Crest factors are wood 8-10 dB, rock 20-23, leaf 27, so the LEAF is the binding one at
  // -1.5 - 26.8 = -28.3. Anything hotter clips the leaf before the wood is even close.
  // 0.464 then carries the set back to the tool_hit anchor: those measure -21.5 dB RMS at 0.2036, this is
  // 7.15 dB quieter as a file, and 0.2036 x 10^(7.15/20) = 0.464. Perceived weight is unchanged; what moved
  // is that it is now ONE number, so the next set only has to hit -28.5 to belong.
  // NOTE this also flattens the take-to-take variation each set came with (wood spread 2.5 dB, rock 2.6).
  // That is what "consistent" asks for; if the hits want their natural unevenness back, normalise per SET
  // to this mean instead of per FILE and the bases stay exactly as they are.
  const IMPACT_BASE = 0.464;
  const ROCKHIT_N = 5;
  const rockHitSnds = [];
  for (let i = 0; i < ROCKHIT_N; i++) {
    const src = 'sound/impact_sounds/rock/0' + i + '.mp4';
    rockHitSnds.push([regSnd(new Audio(src), IMPACT_BASE), regSnd(new Audio(src), IMPACT_BASE)]);
  }
  let rockHitV = 0;
  const rockHitPick = sndBag(ROCKHIT_N);
  // ── AND WOOD UNDER AN AXE HAS ITS OWN TOO (user 2026-08-26) ── same move as the rock takes above. The
  // delivered wood_sounds.mp4 holds SIX hits and the sixth is deliberately dropped on the user's
  // instruction, so this is five: the silences between them are 1.5 s wide, which makes the cuts unambiguous
  // (3.535, 5.381, 7.250, 9.003, 10.814 - and the 12.799 that is not used).
  // LEVELLED ON RMS, NOT LUFS. These are 0.37-0.48 s and the EBU gate wants 400 ms blocks, so integrated
  // loudness is unusable on them - two of the five measured -70 LUFS, i.e. the gate found nothing at all.
  // Peak and RMS are length-independent and the clips are close to the tool_hit takes in length (0.53-0.58),
  // so RMS is the honest comparison: the source set sits at -35.1 dB against tool_hit's -21.5, and one common
  // +13.6 dB brings the set to a -21.7 mean while preserving the 2.5 dB of spread the takes came with.
  // Peaks land at -10 to -12 dB, nowhere near clipping, so the base is simply tool_hit's own 0.2036.
  // NOT BASS-TAPPED, like the rock takes - see the note there.
  const WOODHIT_N = 5;
  const woodHitSnds = [];
  for (let i = 0; i < WOODHIT_N; i++) {
    const src = 'sound/impact_sounds/wood/0' + i + '.mp4';
    woodHitSnds.push([regSnd(new Audio(src), IMPACT_BASE), regSnd(new Audio(src), IMPACT_BASE)]);
  }
  let woodHitV = 0;
  const woodHitPick = sndBag(WOODHIT_N);
  // ── AND FOLIAGE (user 2026-08-26: "anytime any of the tools or weapons hit a leaf or foilage") ── no tool
  // gate on this one, deliberately: a leaf gives way to anything, which is the same rule the leaf CARVE
  // already follows (see phChopLeaves in sim/tools.js, reached with no tool test at all).
  // leaf_impact.mp4 needed no denoising - it is digital silence either side of one sound at 1.00-1.72 s, so
  // "isolate it" is a trim and nothing more. ONE take, so no bag: sndBag(1) would return 0 for ever anyway.
  // BASE 0.451: peak-limited to -1.5 dBTP like the rock takes, which leaves it at -28.4 dB RMS, and the base
  // carries the 6.9 dB back up to tool_hit's -21.5. That puts a leaf at the same WEIGHT as a struck trunk,
  // which is a choice rather than a measurement - if a leaf should be softer than an axe in oak, this is the
  // one number to drop.
  // ── AND THE LEAF PLAYS WELL UNDER THE REST (user 2026-08-26: "lower the leaf sound by 25%", then "turn the
  // leaf volume down by another 25%") ── as a LEVEL, not as a file: the eleven takes stay normalised to the
  // same -28.5 dB RMS, which is what "consistent" bought, and only the playback gain moves.
  // TWO cuts of 25%, COMPOUNDED rather than added: 0.75 x 0.75 = 0.5625, so 0.464 -> 0.261. Read as amplitude
  // like every other percentage in this file, which makes it -5.0 dB against the rock and wood takes.
  // Taking 50% off in one step would have been a different (and quieter) number - 0.232 - and the second
  // request was "another 25%", i.e. a quarter off what it is NOW.
  // Foliage giving way to a blade should not weigh anything like stone breaking.
  const LEAF_BASE = IMPACT_BASE * 0.75 * 0.75;
  const LEAFHIT_N = 1;
  const leafHitSnds = [];
  for (let i = 0; i < LEAFHIT_N; i++) {
    const src = 'sound/impact_sounds/leaf/0' + i + '.mp4';
    leafHitSnds.push([regSnd(new Audio(src), LEAF_BASE), regSnd(new Audio(src), LEAF_BASE)]);
  }
  let leafHitV = 0;
  const leafHitPick = sndBag(LEAFHIT_N);
  const playToolHit = () => {
    // Which material this blow landed on, set by chopSwing on the swing it just spent (sim/tools.js).
    // tool_hit is still the fallback and still carries every other case: a knife on wood, a shovel in soil,
    // a pick on anything that is not stone.
    const rock = !!CHOP_AIM.rockHit, wood = !rock && !!CHOP_AIM.woodHit;
    const leaf = !rock && !wood && !!CHOP_AIM.foliaHit;
    // ── THE GENERIC CLICK IS GONE (user 2026-08-26: "can you remove the lego sound entirely from the files?
    // I thought I did but its still playing the sound") ── it WAS deleted, from sound/tool_hit/; it came back
    // because I restored those four files on the same day, having read the 404s they left as a fault. They are
    // deleted again, for good, and the code that reached for them with them.
    // AND WHAT IT COVERED IS NOW SILENT, by choice (user 2026-08-26: "its ok, make the hits silent … I'll
    // fill it in later with other sounds"). Measured across the palette before deciding, because it is more
    // than the odd case the old note claimed: soil and sand (every `dig` id), the plain stone variants,
    // mushroom caps and fern fronds all landed on the generic take, so a shovel in earth makes no sound at
    // all until those are recorded. That is the intended state, not an oversight — three families play, and
    // everything else waits. Adding one is a bag, a base and an arm on the chain below, exactly as these are.
    if (!rock && !wood && !leaf) return;
    const t = rock ? rockHitSnds[rockHitPick()]
            : wood ? woodHitSnds[woodHitPick()]
                   : leafHitSnds[leafHitPick()];
    const a = t[(rock ? rockHitV++ : wood ? woodHitV++ : leafHitV++) & 1];
    try { a.currentTime = 0; const p = a.play(); if (p) p.catch(() => {}); } catch (e) {} };
  // ── AND WHEN IT DOESN'T (user 2026-08-07) ── the dull thud of a tool that landed square on material it
  // cannot work: the pick on soil or a trunk, the shovel on stone, bare hands on a boulder. The four takes
  // above are the sound of something COMING APART, so they were the wrong answer for a swing that moved
  // nothing — and silence was the wrong answer too, because a miss and a bounce felt identical.
  // Base 0.29, set when the takes were 0.625: block.mp4 is 6.7 LUFS hotter than they are over the same 0.55 s, so
  // matching the number would have made the failure the loudest thing in the game. Two voices for the same
  // reason the takes have them — a held click auto-repeats every 570 ms and the tail must not clip itself.
  // DOWN 40% TO 0.174 (user 2026-08-07, "the anti hit sound" — this one: the swing that connects and does
  // nothing): 0.29 x 0.6, linear, i.e. -4.4 dB. It falls in step with the 25% the takes above just took, so
  // a bounce stays QUIETER than a bite rather than creeping over it as the bite came down.
  // ── DOWN A FURTHER 25% TO 0.1305 (user 2026-08-07) ── 0.174 x 0.75, linear (-2.5 dB), read as amplitude
  // like every other percentage here. The tool-hit takes are untouched at 0.2036, so the gap between a bite
  // and a bounce widens again rather than the whole pair sliding down together.
  // ── WALKING ON GRASS (user 2026-08-26: "play this sound anytime the player is walking on grass … repeat it
  // when neccessary … make sure it blends in smoothly") ── ONE looping element rather than a step-triggered
  // one-shot, because that is what the recording is: 6.2 s of someone walking, not a single footfall. Its
  // ends were cut MID-SILENCE, in the gaps between two footfalls, and the leading and trailing silence were
  // trimmed to sum to the recording's own 0.70 s stride — so the wrap lands exactly where the next step would
  // have, and loop=true needs no crossfade to hide a seam that is not there. (tools: the source was 47.6 s of
  // h264+aac VIDEO, 12.9 MB; the shipped file is audio-only mono AAC, 83 KB. See source/audio/environment.)
  // BASE 0.62 against an I of -39.1 LUFS: the forest bed measures -42.3 and is the thing this has to sit just
  // above, because a footstep you cannot hear over the wind is not a footstep. The recording arrived at -56
  // LUFS with only 20 dB of peak headroom, so the +17 dB it took to get here is most of what there was.
  // FADED, NOT SWITCHED: stopping dead on the frame the player releases a key clicks, and starting dead drops
  // you into the middle of a footfall. STEP_FADE ramps the element's own gain, and the element keeps running
  // while faded out so the loop's phase carries on — start walking again and you rejoin the stride you left.
  const STEP_DBG = { walk: false, grass: false, spd: 0, at: 0, playing: false };   // what the tick decided and what the element is doing — __vb.stepDbg()
  const STEP_FADE = 0.09;
  const STEP_BASE = 0.496;                             // -20% (user 2026-08-26: "lower the footsteps volume by 20%") — 0.62 x 0.8, read as AMPLITUDE like every other percentage in this file
  const stepGrass = regSnd(new Audio('sound/footsteps/grass.mp4'), STEP_BASE);
  stepGrass.loop = true;
  let stepWant = 0, stepAt = 0, stepStarted = false;
  // ── AND TWICE THE CADENCE WHEN SPRINTING (user 2026-08-26: "when the player sprints, play the grass steps
  // a twice speed") ── playbackRate on the element, so the stride doubles and the loop's own seam stays where
  // it was: at 2x the 6.208 s loop is 3.104 s and the wrap still lands in the same mid-silence gap it was cut
  // in, because the cut is a property of the audio and not of the rate.
  // preservesPitch is set EXPLICITLY rather than left to the default. It is true by default in current
  // browsers, which is what we want — a footfall an octave up is a different surface, not a faster walker —
  // but the default is the kind of thing that differs across engines, and the vendor-prefixed spellings are
  // still what older WebKit and Gecko read.
  const stepRate = (r) => {
    if (stepGrass.playbackRate !== r) stepGrass.playbackRate = r;
    if (stepGrass.preservesPitch === false) stepGrass.preservesPitch = true;
  };
  try { stepGrass.preservesPitch = true; stepGrass.mozPreservesPitch = true; stepGrass.webkitPreservesPitch = true; } catch (e) {}
  const stepSurface = (on, dt, fast) => {              // called once a frame from the camera tick
    stepWant = on ? 1 : 0;
    stepRate(fast ? 2 : 1);
    STEP_DBG.rate = stepGrass.playbackRate;
    const k = 1 - Math.exp(-(dt > 0 ? dt : 0.016) / STEP_FADE);
    stepAt += (stepWant - stepAt) * k;
    if (stepAt < 0.004) { stepAt = 0; if (stepStarted) { try { stepGrass.pause(); } catch (e) {} stepStarted = false; }
      STEP_DBG.at = 0; STEP_DBG.playing = false; return; }
    // Level BEFORE play(), not after: the element carries its REGISTERED volume while paused, so starting it
    // first would sound one frame at full before the ramp caught up — a click on every footfall you begin.
    // Re-applied every frame for the reason the ambience block records: applyVol() rewrites every registered
    // element when a slider moves, so a remembered value would go stale the moment the player touched one.
    const v9 = sndLevel(STEP_BASE, BUS_SFX, stepAt);
    if (stepGrass.volume !== v9) stepGrass.volume = v9;
    if (!stepStarted) { stepStarted = true; try { const p = stepGrass.play(); if (p) p.catch(() => { stepStarted = false; }); } catch (e) { stepStarted = false; } }
    STEP_DBG.at = stepAt; STEP_DBG.playing = stepStarted && !stepGrass.paused;
  };
  const blockSnds = [regSnd(new Audio('sound/block.mp4'), 0.1305), regSnd(new Audio('sound/block.mp4'), 0.1305)];
  let blockV = 0;
  // ── AND THE BOUNCE KNOWS WHAT IT BOUNCED OFF (user 2026-08-26: "sometimes the lego sound still plays when
  // the player hits the birch tree") ── block.mp4 is a hard plastic knock, and it was the ONE impact left in
  // the game that never asked what it landed on: every other route here is material-aware (rock takes on
  // stone, wood takes on wood, the leaf take on anything soft), so a swing that connects with a birch and
  // fails to bite was the only way to still get the old generic click off a tree. That is the report.
  // It is deliberately NOT just silenced, and not promoted to a full bite either: a bounce still has to sound
  // different from a chop or the two become indistinguishable and the swing stops reading as wasted. So it
  // plays the tool's own MATERIAL take at the bounce's quieter level — a dull wood knock on wood, a stone
  // knock on stone — and keeps block.mp4 for everything else, which is what it was recorded for.
  // BLOCK_DUCK carries the takes down to the bounce: they are levelled to IMPACT_BASE for a bite, and the
  // whole point of the block level is that a bounce sits under a bite (see the note above).
  const BLOCK_DUCK = 0.64;
  const playBlocked = (id) => {
    const wood = id !== undefined && !!woodTab[id], rock = !wood && id !== undefined && !!pickOnlyTab[id];
    // …and SOFT counts too: a cactus that refuses the bite was answering with the generic knock, which is
    // the one sound "anything green in the desert gets the leaf sound" is meant to replace. leafSndTab is the
    // same table playToolHit reads, so the bounce and the bite can never disagree about what a plant is.
    const soft = !wood && !rock && id !== undefined && !!leafSndTab[id];
    if (wood || rock || soft) {
      const t = wood ? woodHitSnds[woodHitPick()] : rock ? rockHitSnds[rockHitPick()] : leafHitSnds[leafHitPick()];
      const a = t[(wood ? woodHitV++ : rock ? rockHitV++ : leafHitV++) & 1];
      const wasVol = a.volume;
      try { a.volume = Math.max(0, Math.min(1, wasVol * BLOCK_DUCK)); a.currentTime = 0;
        const p = a.play(); if (p) p.catch(() => {});
        a.addEventListener('ended', () => { try { a.volume = wasVol; } catch (e) {} }, { once: true });
      } catch (e) { try { a.volume = wasVol; } catch (e2) {} }
      return;
    }
    const a = blockSnds[blockV++ & 1];
    try { a.currentTime = 0; const p = a.play(); if (p) p.catch(() => {}); } catch (e) {} };
  // ── EATING (user 2026-08-07) ── the sound existed and nothing could trigger it, because nothing could be
  // eaten: raw meat could be picked up, held and dropped, and that was all. So this is the smallest consume
  // action that gives the sound a cause — RIGHT-CLICK with food in hand takes one bite off the stack. It is
  // second in line behind the pickup, so right-clicking a rock at your feet still grabs the rock rather than
  // silently eating your dinner; `grabAnim` is the flag that says the pickup claimed the click. HOLDING the
  // button keeps eating (user 2026-08-11), one bite per EAT_MS, exactly as holding the left button keeps swinging.
  const EAT_MS = 900;                                  // ── THE BITE FLOOR ── one bite per 900 ms, and it is now the EATING CADENCE as well as a click limiter: a fast click must not run through a whole stack in a second, and neither must a HELD right button (user 2026-08-11 — see the auto-repeat in the tick loop). tryEat is the single gate for both paths, so a bite costs the same wherever the call came from.   // ── AND IT STAYS 900 AT FIVE HEALTH POINTS (2026-08-17) ── a bite is now 20% of the bar, so this number alone decides how fast a stack of meat can undo a near-death: 3.6 s to climb 1 → 5. That is long enough that healing is a thing you stop and do rather than a keypress, and short enough not to be a chore — and lengthening it would be exactly the wrong lever, because the point of this rework is that the player thinks about food LESS, not that eating is slower.
  let eatT = -1e9;
  let eatHold = false;                                 // ── IS THE HELD RIGHT BUTTON AN EATING HOLD? ── armed at mousedown ONLY when the pickup did not claim that click, cleared at mouseup. Without it the hold path would quietly overrule the pickup-wins rule above: `grabAnim` is only true while the item is IN FLIGHT, so a right-hold on a rock at your feet would grab the rock and then start eating the meat still in your hand the moment the flight landed. One press = one decision, grab or eat, for as long as it is held.
  // ── AND THE FRUIT IS ACTUALLY CHEWED (user 2026-08-17: "play the apple eating animation as there should
  // already be one") ── assets/held-items.js carves apple/00.vox and orange.vox into a run of consecutive item
  // ids each, and this is the clock that walks them. It lives here, beside tryEat, because a bite is the ONLY
  // thing that starts it: the strip is not a loop the hand idles through, it is one fruit eaten, once, per click.
  // 24 FPS, which is the house rate for every animation in this game unless the user says otherwise, so the
  // twenty-one frames run 875 ms, filling EAT_MS (900) almost exactly — so a HELD right button reads as one
  // continuous bite-swallow-bite rather than a flicker of half-eaten apples.
  // ── IT WAS THIRTEEN FRAMES AND 542 MS UNTIL 2026-08-19, AND THE 358 MS THAT LEFT OVER WAS A BUG TWICE ──
  // first as a fresh item appearing mid-chew (nothing was drawing the gap, so the hand fell back to the stack),
  // then, once the last frame was held across it, as a frame that visibly stuck: 358 ms is 8.6x the 41.7 ms
  // every other frame gets, and the eye reads that as the animation jamming on its final pose. The window is
  // filled with real frames now. See the EAT_N block in assets/held-items.js for why 21 is the ceiling.
  // ── 8 FPS WAS TRIED AND TAKEN BACK OUT, SAME DAY (user 2026-08-17: "play the apple at 8 frames a second",
  // then "erase that 8 fps for the apple, have it match the orange") ── recorded because the rate is a thing
  // that will be reached for again, and because it does not stand alone: ONE clock walks both fruit, so
  // slowing the apple slowed the orange with it, and at 8 the strip ran 1625 ms — past EAT_MS, which meant a
  // HELD right button restarted it at 900 and you never once saw a fruit eaten to its core. Anything under
  // ~15 fps has that problem here; a slower chew needs EAT_MS moved with it, and EAT_MS is the healing
  // cadence the block above argues for, not an animation number.
  // NOT a `let` inside a module, and not read back out of the hotbar: the last apple of a stack empties its
  // slot on the same frame the animation starts, so anything that asked the hand what it was holding would
  // stop drawing the fruit one frame into being eaten. tick-camera reads THIS instead, which is why the core
  // still gets eaten in front of you when it was your last one.
  let eatAnim = null;                                  // {t0, it} — the strip playing in the hand, or null
  const EAT_FPS = 24, EAT_FRAME_MS = 1000 / EAT_FPS;   // the house rate, and ONE clock for both fruit — see the note above
  const eatAnimFrame = (now) => {                      // which frame of the strip the hand should draw, or -1 for "not eating". Self-clearing: the frame past the last one ends the animation.
    if (!eatAnim) return -1;
    const f = Math.floor((now - eatAnim.t0) / EAT_FRAME_MS);
    // ── THE REMNANT HOLDS UNTIL THE NEXT BITE IS LEGAL (user 2026-08-19: "when eating the meat, if its
    // stacked, it loads the new one before the first one is done playing the eating animation") ── the
    // block above has ALWAYS described this hold, but it was never implemented: the strip is FOOD_EAT_N/EAT_FPS
    // and a bite is gated at EAT_MS, so retiring the animation the instant its last frame had been shown handed
    // the hand straight back to heldIt() — THE NEXT WHOLE ITEM OFF THE STACK — for whatever was left of the bite.
    // On a stack of one that read as an apple core vanishing early; on a stack of meat it read as a fresh steak
    // appearing mid-chew. This is now a 25 ms backstop rather than the 358 ms it closed when it was written:
    // EAT_N was raised to fill the window with frames instead, because a third of a second of held pose reads
    // as a stuck frame (user, same day). Keeping it costs nothing and it still guarantees the invariant: the new
    // item appears on the frame the new bite starts and not before, whatever EAT_N and EAT_MS are set to. Keyed off EAT_MS rather than a hold constant
    // of its own so the two cannot drift apart: move the cadence and the hold follows it.
    // The selSlot test ends the hold early if you scroll away mid-chew — the remnant is only the right thing
    // to draw while that slot is still the one in your hand; the strip itself is not gated on it, because
    // eating the LAST of a stack empties the slot without changing which slot is selected.
    if (f >= FOOD_EAT_N) {
      if (now - eatAnim.t0 < EAT_MS && selSlot === eatAnim.sl) return FOOD_EAT_N - 1;
      eatAnim = null; return -1;
    }
    return f < 0 ? 0 : f;
  };
  // DERIVED from the vitals' food table rather than listed here: what is edible and what it restores are the
  // same fact, and keeping two lists in step by hand is how a food ends up chewable but nourishing nothing.
  const EDIBLE = () => new Set(Object.keys(vitFoods()).map(Number).filter(Boolean));
  const eatSnd = regSnd(new Audio('sound/eat.mp4'), 0.105);   // 0.7 −40% (user 2026-08-07), then −50% and another −50% (user 2026-08-11)
  // ── SNATCHED OUT OF THE AIR (user 2026-08-08) ── sound/pick_up.mp4 is the file that shipped as sound/hit.mp4
  // until the four-take tool_hit pool replaced it as the swing sound; it has been unused since and is now the
  // pickup. It plays for a LEVITATING drop only — raw steak off a kill, a Q-tossed item, a loosed arrow, while
  // each is still hovering and spinning — and stays silent for one that has already settled onto the ground.
  // BASE 0.043945312: the 0.1171875 it was tuned to as hit.mp4, HALVED (user), and then DOWN A FURTHER 25%
  // (user 2026-08-08) — read as amplitude like every other percentage in this file, so -6 dB and then -2.5 dB,
  // 0.1171875 -> 0.05859375 -> 0.043945312. One voice is enough — only one grab flight is ever in the air.
  // It fires the instant the item is SNATCHED (see startGrab), not when the flight lands in the hand.
  const pickUpSnd = regSnd(new Audio('sound/pick_up.mp4'), 0.043945312);
  let pickUpPlays = 0;                                 // fired count — currentTime rewinds on every play, so a test has nothing else to count
  const playPickUp = () => { pickUpPlays++;
    try { pickUpSnd.currentTime = 0; const p7 = pickUpSnd.play(); if (p7) p7.catch(() => {}); } catch (err) {} };
  const tryEat = () => {
    const sel = slots[selSlot];
    if (dead || ED.on || grabAnim || !sel || !EDIBLE().has(sel.it)) return false;
    if (VIT.hp >= VIT_HP_MAX && VIT.food >= VIT_FOOD_MAX) return false;   // ── NOTHING TO GAIN REFUSES THE BITE ── so a steak is never spent on nothing. It asked about HEALTH ALONE until hunger came back (user 2026-08-19), and health alone is wrong the moment there are two bars: a player at full health with an empty stomach could not eat, so the hunger bar could only ever go down and starvation was unavoidable. The condition is now the SAME one sim/vitals.js vitEat refuses on — deliberately duplicated rather than exported, because this is the CLICK gate (it also owns the sound, the chew and the stack spend) and vitEat is the authority; if they ever disagree the click is refused and nothing is spent, which is the safe direction
    const now9 = performance.now();
    if (now9 - eatT < EAT_MS) return false;
    eatT = now9;
    const bit = sel.it;                                 // captured BEFORE the stack is spent: the line below can empty the slot, and the animation still has to know what it was eating
    try { eatSnd.currentTime = 0; const p = eatSnd.play(); if (p) p.catch(() => {}); } catch (e) {}
    vitEat(bit);                                        // +1 health point, by the food's own number in vitFoods()
    // ── ONE ANIMATION FOR EVERY EDIBLE, PRESENT AND FUTURE (user 2026-08-18) ── armed off the FOOD TABLE's own
    // `strip` flag (sim/vitals.js) rather than off a hand-written list of ids. That list was a second definition
    // of "what is edible" living a file away from the real one, and the two could disagree: a food added to the
    // table without being added here would eat silently, with no chew at all. Now the table is the only place a
    // food is declared and this line cannot fall behind it. The clock, the cadence and the sound are already
    // shared — this was the last thing that was not.
    const fd9 = vitFoods()[bit];
    if (FOOD_EAT_N && fd9 && fd9.strip) eatAnim = { t0: now9, it: bit, base: fd9.eat || bit, sl: selSlot };   // `base` is what the frame counter indexes; it differs from `it` only for the WORM, whose held id is the head of a crawl cycle rather than of an eat strip   // strip:false foods (the worm) chew on the same EAT_MS clock and make the same sound; they simply have no carved frames to walk, and indexing a FOOD_EAT_N run off a crawl cycle would show the next creature's frames
    if (--sel.n <= 0) { slots[selSlot] = null; slotTidy(); }
    return true;
  };
  const bowStretchSnd = regSnd(new Audio('sound/bow/stretch.mp4'), 0.55);   // the DRAW — creaks under the pull and is cut the moment the string is released, so a half-draw never rings on
  // ── the string settling back to rest with a fresh arrow on it. sound/bow/reload.mp4 was removed from the
  // tree on 2026-08-07 while the rest of the bow's audio was being wired, so this walks the usual format
  // fallbacks the ambience loader uses: drop reload.mp4 (or .mp3/.wav) back in and the nock speaks again with
  // no code change. Until then the re-nock is silent — play() rejects and is caught, nothing breaks.
  const bowReloadSnd = regSnd(new Audio(), 0.45);
  { const cands = ['sound/bow/reload.mp4', 'sound/bow/reload.mp3', 'sound/bow/reload.wav'];
    let ci = 0;
    bowReloadSnd.addEventListener('error', () => { if (ci < cands.length) bowReloadSnd.src = cands[ci++]; });
    bowReloadSnd.src = cands[ci++]; }
  const playBowImpact = sndPool('sound/bow/impact.mp4', 0.6, 4);
  const playBowStretch = () => { try { bowStretchSnd.currentTime = 0; const p = bowStretchSnd.play(); if (p) p.catch(() => {}); } catch (e) {} };
  const stopBowStretch = () => { try { bowStretchSnd.pause(); bowStretchSnd.currentTime = 0; } catch (e) {} };
  const playBowReload = () => { try { bowReloadSnd.currentTime = 0; const p = bowReloadSnd.play(); if (p) p.catch(() => {}); } catch (e) {} };
  // …and the hit is quieter the further off it lands. There is no positional audio in this engine, so this is
  // one honest distance term rather than a pan: full within a dozen voxels, floored so a long shot still reads.
  const playArrowHit = (wx, wy, wz) => {
    const d8 = Math.hypot(wx - P.x, wy - smoothEye, wz - P.z);
    playBowImpact(Math.max(0.14, Math.min(1, 1 - (d8 - 12) / 190))); };
  // ── ONE VOICE FOR EVERY BLOW THAT LANDS ON A LIVING THING (user 2026-08-08) ── sound/life_hit.mp4, built
  // from the .wav that was sitting unconverted in sound/ by the recipe every other cue ships under (AAC-LC,
  // 48 kHz stereo; the source now lives with the rest of the animal audio in source/audio/life/). It fires
  // for the tool, the weapon, the arrow, the spear, a thrown rock and bare hands alike — the tool decides how
  // much damage lands, not whether the blow is audible.
  // BASE 0.1032, measured rather than guessed, and measured against what tool_hit ACTUALLY PLAYS AT rather
  // than against its declared base: the pool is registered at 0.2715 but playToolHit passes a 0.75 gain, so
  // the element goes out at 0.2545 (verified live). life_hit is -13.9 LUFS integrated against those takes'
  // -19.8, so matching perceived level wants -13.9 + 20log10(V) = -19.8 + 20log10(0.2545), i.e. V = 0.129 at
  // full gain, i.e. a base of 0.129 / 1.25 (SND_GAIN) = 0.1032. Hitting an animal and hitting a tree then land
  // as the same weight of blow. Reading the declared base instead put this 2.5 dB hot.
  // POOLED THREE DEEP for the same reason the bow's impact is: arrows land in twos and threes, and a held
  // chop repeats every 570 ms, so one element would keep cutting its own transient short.
  // Distance-faded on the SAME curve as the arrow's impact, so a shaft that finds a bird a hundred voxels out
  // is heard as a distant tick rather than at point-blank.
  const lifeHitPool = sndPool('sound/life_hit.mp4', 0.1032, 3);
  const playLifeHit = (wx, wy, wz) => {
    const d9 = Math.hypot(wx - P.x, wy - smoothEye, wz - P.z);
    lifeHitPool(Math.max(0.14, Math.min(1, 1 - (d9 - 12) / 190))); };
  let bobPh = 0, bobAmp = 0;                           // walk/sprint view-model bob state
  let camBobY = 0;                                     // …and the CAMERA's share of it (user 2026-08-19). Vertical only — see the note where it is written, in main/tick-camera.js
