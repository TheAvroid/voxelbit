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
    if (!slots.some((s) => !s)) return;                // only an EMPTY slot auto-grabs; topping up an existing stack stays a deliberate right-click
    const tNow = performance.now();
    let bi = -1, bd = AUTO_PICK_R * AUTO_PICK_R;       // nearest wins, so a pile drains one item per flight instead of grabbing at random
    for (let i = 0; i < drops.length; i++) {
      const dr = drops[i];
      if (dr.T && (tNow - dr.born) / 1000 < dr.T) continue;   // still mid-toss — let it land before it can be re-grabbed (else a Q-toss boomerangs straight back)
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
  let sfxVol = 1;   // ── SFX (user 2026-08-07) ── a second bus under the master, for the sounds the WORLD MAKES: every
                    // tool, hit, footstep, bowstring, pickup and jingle. The forest loop is the one thing it does not
                    // touch — an SFX slider that also rode the ambience would just be the master slider twice over.
                    // Resets to 100% on refresh and persists to vb_sfx, matching sndVol exactly rather than inventing a
                    // second rule for two sliders sitting in the same box (see the volume-resets-to-100 note below).
  let musVol = 1;   // ── MUSIC (user 2026-08-08) ── the third bus: the score, and nothing else. Same rule as the two
                    // sliders it sits under — starts at 100% on every refresh and persists to vb_mus — so the sound
                    // box has one behaviour and not three. Only the anthem rides it today (see ANTHEM_AT below).
  let sndVol = 1;   // start at FULL VOLUME on refresh (user 2026-08-06). Was 0 (always-muted, 2026-08-02); a page load still does NOT restore vb_vol, it just starts at 100% instead of 0%.
  // ── TEMPORARY: SILENT ON REFRESH, FOR DEVELOPMENT (user 2026-08-15) ── the three buses above all deliberately
  // reset to 100% on every page load, which is right for players and wrong for someone reloading the build a
  // hundred times an afternoon. This overrides master and music to 0 AFTER their declarations so the reasoning
  // above stays intact and undoing it is deleting these two lines — not reconstructing three defaults. The
  // sliders still work normally once moved. REMOVE BEFORE SHIPPING.
  sndVol = 0; musVol = 0;
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
  const busVol = (bus) => bus === BUS_SFX ? sfxVol : bus === BUS_MUS ? musVol : 1;   // BUS_AMB rides the master alone
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
  const ANTHEM_AT = 60;                                // seconds of GAMEPLAY before the FIRST track — not of page life, see the play clock in tickBody (user 2026-08-08: was 120)
  const ANTHEM_GAP = 60;                               // …and a minute of gameplay of SILENCE between one track ending and the next starting (user)
  // ── THE SET (user 2026-08-08) ── all four anthem cuts, in rotation, then silence for the rest of the
  // session. Built from source/audio/anthem/*.wav by the same recipe as the first one and each other sound in
  // the game: AAC-LC 320k, 48 kHz stereo, .mp4 — the four together are 13 MB against 95 MB of source PCM.
  // The per-track GAIN is not a guess: every file was measured with ebur128 and levelled against red carpet,
  // which is the anchor at 0.14 because its own 0.14 was tuned by ear against high_score.mp4. Integrated
  // loudness, and the gain that lands each at the same perceived level:
  //     red carpet  -15.1 LUFS -> 0.140   (the anchor)
  //     achievement -14.1      -> 0.125   (1 dB hotter, so 1 dB down)
  //     to glory    -14.1      -> 0.125
  //     award       -19.1      -> 0.222   (4 dB quieter, so 4 dB up)
  const ANTHEM_SET = [['red_carpet', 0.140], ['achievement', 0.125], ['award', 0.222], ['to_glory', 0.125]];
  const anthemSnds = ANTHEM_SET.map(([n9, g9]) => regMus(new Audio('sound/music/' + n9 + '.mp4'), g9));
  const anthemSnd = anthemSnds[0];                     // the first cut, still named for the taps that read it
  let playSecs = 0, anthemDone = false;                // the play clock, and the set-is-finished latch
  let anthemIdx = 0, anthemNextAt = ANTHEM_AT;         // which cut comes next, and the play-clock time it is due
  const playAnthem = () => {
    const a9 = anthemSnds[anthemIdx++];
    anthemNextAt = Infinity;                           // nothing is due while one is playing — the ended handler below schedules the next
    if (anthemIdx >= anthemSnds.length) anthemDone = true;   // that was the fourth: the set is spent and the clock can stop
    try { a9.currentTime = 0; const p9 = a9.play(); if (p9) p9.catch(() => { if (!anthemDone) anthemNextAt = playSecs + ANTHEM_GAP; }); } catch (e) { if (!anthemDone) anthemNextAt = playSecs + ANTHEM_GAP; }
  };
  // A refused or failed play must not stall the set: the catch above re-arms the next one on the same gap it
  // would have got anyway. `ended` is the normal path — the gap is measured from the moment a track FINISHES,
  // so it is a minute of silence rather than a minute of overlap.
  for (const a9 of anthemSnds) a9.addEventListener('ended', () => { if (!anthemDone) anthemNextAt = playSecs + ANTHEM_GAP; });
  const SWAP_MS = 240;                                 // how long a tool swap takes to rise back into frame — short enough that it never delays a swing
  let swapT0 = -1e9, prevHeldIt = -1;                  // …and when the current one started (see the view-model block)
  let swingStart = -1e9, mouse0 = false, mouse2 = false;   // mouse2: RIGHT button HELD — the wind-up for a rock throw (right-hold, then left-click)               // left-click SWING — Minecraft-style forward chop; HOLDING the button keeps swinging
  let pendKillT = 0;                                   // pending creature-hit: armed at swing start, FIRES ~250 ms in — when the axe visually LANDS on screen (user: only register the hit when the axe hits), not at the click
  const swishSnd = regSnd(new Audio('sound/bow/swish.mp4'), 0.36);   // swing whoosh — cut 40% (user), then a further 40% (0.6 → 0.36)
  const playSwish = () => { try { swishSnd.currentTime = 0; const pr = swishSnd.play(); if (pr) pr.catch(() => {}); } catch (err) {} };
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
  const TOOLHIT_N = 4;
  const toolHitSnds = [];
  for (let i = 0; i < TOOLHIT_N; i++) {
    const src = 'sound/tool_hit/0' + i + '.mp4';
    toolHitSnds.push([regSnd(new Audio(src), 0.2036), regSnd(new Audio(src), 0.2036)]);   // two voices per take so the same one can overlap itself
  }
  let toolHitV = 0;
  const TOOLHIT_BASS_DB = 10.9, TOOLHIT_BASS_HZ = 190;   // lift below ~190 Hz (user 2026-08-07: "increase the bass") — the weight of a struck tool lives under the crack, and these takes are recorded thin
  // BASS +40% (user 2026-08-07), 8 dB → 10.9: read as AMPLITUDE, the way every other percentage in this file
  // is. The shelf was lifting the low band 10^(8/20) = 2.51x; 2.51 x 1.4 = 3.52x, which is 10.9 dB. Scaling
  // the dB number itself by 1.4 would have said 11.2 dB — a quarter of a dB apart, inaudible either way, so
  // the reading costs nothing and amplitude is the one that stays consistent with the volume cuts above.
  // ── ROUTE EVERY TAKE, NOT JUST THE ONE PLAYING ── an element may be handed to createMediaElementSource
  // ONCE, ever, and the video recorder claims every registered sound when a capture starts. Tapping lazily
  // per-take lost that race: measured, 6 of 8 voices had the filter and the two whose take had not come up yet
  // were claimed dry by the recorder and could never be filtered again for the rest of the session. So all
  // eight are routed together, on the first input event — which always precedes both the first swing and the
  // first capture, and means the context is running rather than suspended when it happens.
  let toolHitRouted = false;
  const routeToolHits = () => {
    if (toolHitRouted || !audioCtx()) return;
    for (const t of toolHitSnds) for (const a of t) bassTap(a, TOOLHIT_BASS_DB, TOOLHIT_BASS_HZ);
    toolHitRouted = true;
  };
  document.addEventListener('pointerdown', routeToolHits);
  document.addEventListener('keydown', routeToolHits);
  const playToolHit = () => { const t = toolHitSnds[(Math.random() * TOOLHIT_N) | 0]; const a = t[toolHitV++ & 1];
    routeToolHits();                                   // belt and braces if no gesture listener ever ran
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
  const blockSnds = [regSnd(new Audio('sound/block.mp4'), 0.1305), regSnd(new Audio('sound/block.mp4'), 0.1305)];
  let blockV = 0;
  const playBlocked = () => { const a = blockSnds[blockV++ & 1];
    try { a.currentTime = 0; const p = a.play(); if (p) p.catch(() => {}); } catch (e) {} };
  // ── EATING (user 2026-08-07) ── the sound existed and nothing could trigger it, because nothing could be
  // eaten: raw meat could be picked up, held and dropped, and that was all. So this is the smallest consume
  // action that gives the sound a cause — RIGHT-CLICK with food in hand takes one bite off the stack. It is
  // second in line behind the pickup, so right-clicking a rock at your feet still grabs the rock rather than
  // silently eating your dinner; `grabAnim` is the flag that says the pickup claimed the click. HOLDING the
  // button keeps eating (user 2026-08-11), one bite per EAT_MS, exactly as holding the left button keeps swinging.
  const EAT_MS = 900;                                  // ── THE BITE FLOOR ── one bite per 900 ms, and it is now the EATING CADENCE as well as a click limiter: a fast click must not run through a whole stack in a second, and neither must a HELD right button (user 2026-08-11 — see the auto-repeat in the tick loop). tryEat is the single gate for both paths, so a bite costs the same wherever the call came from.
  let eatT = -1e9;
  let eatHold = false;                                 // ── IS THE HELD RIGHT BUTTON AN EATING HOLD? ── armed at mousedown ONLY when the pickup did not claim that click, cleared at mouseup. Without it the hold path would quietly overrule the pickup-wins rule above: `grabAnim` is only true while the item is IN FLIGHT, so a right-hold on a rock at your feet would grab the rock and then start eating the meat still in your hand the moment the flight landed. One press = one decision, grab or eat, for as long as it is held.
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
    if (VIT.food >= VIT_FOOD_MAX) return false;         // a full bar refuses the bite, so food is never wasted
    const now9 = performance.now();
    if (now9 - eatT < EAT_MS) return false;
    eatT = now9;
    try { eatSnd.currentTime = 0; const p = eatSnd.play(); if (p) p.catch(() => {}); } catch (e) {}
    vitEat(sel.it);                                     // hunger + saturation, by the food's own numbers
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
