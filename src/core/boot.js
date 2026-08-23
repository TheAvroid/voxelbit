(async () => {
  const $ = (id) => document.getElementById(id);
  const loadEl = $('load'), lockEl = $('lock'), hudEl = $('hud'), crossEl = $('cross'), loadMsgEl = $('loadMsg');
  const fail = (msg) => { loadEl.classList.remove('hidden'); loadMsgEl.textContent = msg; throw new Error(msg); };   // loadMsgEl, not loadEl.textContent — the latter would wipe the background <video>
  // MOBILE DEVICE? — the play prompt tells a phone/tablet to "find a good computer" instead of "press any button" (user).
  const isMobile = (() => {
    try { if (navigator.userAgentData && typeof navigator.userAgentData.mobile === 'boolean') return navigator.userAgentData.mobile; } catch (e) {}
    return /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini|Mobile|Silk/i.test(navigator.userAgent || '')
      || (matchMedia('(pointer: coarse)').matches && matchMedia('(hover: none)').matches);   // touch-only, no hover → a handheld, even when the UA lies (modern iPadOS reports as desktop)
  })();

  // ── MOBILE: never boot the world (user) ── no WebGPU, no worldgen, no loading bar — just leave loading.mp4 playing
  // full-bleed with a two-line "find a good / computer" sign, then bail out of the whole engine before it touches the GPU.
  if (isMobile) {
    $('loadTitle').style.display = 'none';               // strip the loading UI down to the video + the message
    $('loadMeter').style.display = 'none';
    loadMsgEl.innerHTML = 'find a good<br>computer';      // two lines: "find a good" above, "computer" below
    loadMsgEl.style.cssText += ';font-size:clamp(16px,6vw,30px);line-height:1.8;letter-spacing:3px;text-align:center;animation:playHintPulse 1.8s ease-in-out infinite';   // text-align:center → "computer" sits centred UNDER "find a good", not left-aligned (user); blink like the "press any button" prompt
    loadEl.classList.remove('hidden');                   // keep the loading.mp4 overlay up for good
    return;                                               // ← handhelds stop here: the world is never generated
  }

  // ── SMOOTH LOADING METER ── The bar is a pure TIME-BASED trickle, driven by a compositor `transform` transition, so it
  // glides continuously 0→90% even while the main thread is blocked mid-worldgen. Real progress is CHUNKY — slabs land in
  // bursts with long synchronous pauses — so driving the bar off progress events always steps/stalls no matter how it's
  // eased (that was the jerk). When the world is actually ready, finishLoad() sweeps it to 100. The % text + gloss just
  // READ the bar's live animated width each frame (via the computed matrix), so they always agree with it.
  const loadFillEl = $('loadFill'), loadPctEl = $('loadPct'), loadGlossEl = $('loadGloss');
  const loadVerEl = document.querySelector('#loadTitle .tver'), loadVerT0 = performance.now(), LOAD_VER_MS = 1500;
  const verCells = loadVerEl ? [...loadVerEl.querySelectorAll('.vd')] : [];
  const verRoll = (cell, ch) => {                       // slide ONE cell to a new digit: stack the newcomer under the current one, translate up by exactly one cell, then drop the old row and reset.
    const strip = cell.firstElementChild;
    if (cell.dataset.v === ch || strip.dataset.busy) return;   // busy guard: at 150 ms a tick and 150 ms a roll they never overlap, but a slow frame must not stack two transforms on one strip
    cell.dataset.v = ch;
    const row = document.createElement('i'); row.textContent = ch; strip.appendChild(row);
    const h = strip.firstElementChild.offsetHeight;     // MEASURED, not derived — px3's line box is 2.0em and the drum must step by whatever the row actually is
    if (!cell.style.height) cell.style.height = h + 'px';
    strip.dataset.busy = '1';
    requestAnimationFrame(() => { strip.style.transition = 'transform 0.09s cubic-bezier(0.3, 0.85, 0.35, 1)'; strip.style.transform = 'translateY(' + (-h) + 'px)';
      setTimeout(() => { strip.style.transition = 'none'; strip.style.transform = 'translateY(0)';
        while (strip.children.length > 1) strip.removeChild(strip.firstElementChild);   // the arrived digit becomes the resting row
        delete strip.dataset.busy; }, 95); });   // the roll must finish well inside the 150 ms tick: at 140 ms it occasionally straddled one and the gate dropped that number (measured — the drum skipped 0.6)
  };   // the version tag counts v0.0 → v1.0 on its OWN steady clock (user: "irrelevant of the loading bar"). Tying it to the bar made it tick unevenly, because the bar is a 9 s ease-out whose rate is deliberately front-loaded. Only the LOADING one animates — the esc menu's is static.
  const VER_TO = 1.3;                                  // ── WHAT THE DRUM COUNTS UP TO ── the shipped version, and the ONE place it is written for the animation
  // (the static tag on the lock screen is in 10-body.html — keep the two in step). Scaling the ramp by it
  // rather than hardcoding 1 keeps the tick even: the counter is linear in TIME over LOAD_VER_MS, so 1.2 is
  // simply thirteen steps at the same cadence (fourteen at 1.3), and toFixed(1) still yields the two digits verSet rolls.
  let loadDone = false, loadFinishing = false;   // loadFinishing guards the crawl below: finishLoad's OWN transitionend must not restart it and drag the bar back off 100%                                 // set by finishLoad → the readout is pinned to 100% so it can't under-read the compositor mid finish-sweep (user: "the loading bar never reaches 100%")
  const readScale = () => { const t = getComputedStyle(loadFillEl).transform; return t && t !== 'none' ? (new DOMMatrixReadOnly(t)).a : 0; };
  const loadNumStep = () => {                            // the % text + sheen mirror whatever the compositor has the bar at right now
    const s = loadDone ? 1 : Math.max(0, Math.min(1, readScale()));
    loadPctEl.textContent = Math.round(s * 100) + '%';
    if (verCells.length) { const vs = (loadDone ? VER_TO : Math.min(VER_TO, (performance.now() - loadVerT0) / LOAD_VER_MS * VER_TO)).toFixed(1); verSet(vs); }   // linear in TIME → an even v0.0, v0.1 … tick up to VER_TO, each digit rolled onto its drum; clamps there however long the load takes
    loadGlossEl.style.width = (s * 100) + '%';
    if (!loadEl.classList.contains('hidden')) requestAnimationFrame(loadNumStep);
  };
  const verSet = (vs) => {                             // BOTH digits move together or neither does. Rolling them independently showed a literal "v1.9": the units cell was free and took its 1 while the
    // tenths cell was still mid-roll on its 9, so for one frame the drum read a version that never existed. The busy strip gates the whole number, not just its own column.
    if (verCells.some((c2) => c2.firstElementChild.dataset.busy)) return;
    verRoll(verCells[0], vs[0]); verRoll(verCells[1], vs[2]);
  };
  // ── THE BAR TRACKS THE BUILD NOW (user 2026-08-19: "it also seems to get stuck at 90%") ── it used to be a
  // pure CSS trickle: a 3 s ease to 90%, then a 75 s crawl from 0.90 to 0.995. Nine and a half hundredths over
  // seventy-five seconds is about a tenth of a percent a second, and decelerating — which is not "creeping",
  // it is INDISTINGUISHABLE FROM STOPPED. The bar sat on 90% for the rest of the load and the user read it,
  // correctly, as stuck.
  // The infuriating part is that real progress was already being computed and thrown away: world/build.js
  // counts finished slabs and calls setLoad(22 + done/total * 64) on every one of them, and setLoad was a
  // NO-OP kept only so the phase calls stayed harmless. So the fix is not a better fake — it is to let the
  // number that already exists drive the bar. MEASURED on this machine: assets land at 0.9 s, the 512-chunk
  // world build runs 0.9 s -> 8.5 s, occupancy and upload finish about 1.4 s after that. The three ranges
  // below are those three phases, so the bar now moves for the whole of the longest one instead of parking.
  let loadSeen = 0, loadTailArmed = false;
  const loadTo = (t, secs) => {                          // monotonic: a later phase can never pull the bar backwards
    if (loadDone || loadFinishing || t <= loadSeen) return;
    loadSeen = t;
    loadFillEl.style.transition = 'transform ' + secs + 's cubic-bezier(0.25, 0.8, 0.35, 1)';
    loadFillEl.style.transform = 'scaleX(' + t.toFixed(4) + ')';
  };
  { loadFillEl.style.transform = 'scaleX(0)'; void loadFillEl.offsetWidth;   // commit 0 first so the transition actually runs from empty
    // PHASE 1, the assets: no callback to hang off, so this stays an ease — but only to 0.20, where the build
    // takes over, rather than to 0.90, where it had nowhere left to go.
    loadFillEl.style.transition = 'transform 1.2s cubic-bezier(0.05, 0.7, 0.2, 1)';
    loadFillEl.style.transform = 'scaleX(0.20)'; loadSeen = 0.20;
    requestAnimationFrame(loadNumStep); }
  // PHASE 2, the world: driven by real slab completion out of world/build.js.
  // PHASE 3, occupancy + upload: setLoad's own last call arms a slow run to 0.97, because those two stages
  // report once and then say nothing for over a second. It is the only guessed segment left, and it is short.
  const setLoad = (p) => {
    const t = Math.max(0, Math.min(0.94, (+p || 0) / 100));
    loadTo(t, 0.45);
    if (t >= 0.85 && !loadTailArmed) { loadTailArmed = true; setTimeout(() => loadTo(0.97, 6), 60); }
  };
  const finishLoad = () => { loadFinishing = true; loadFillEl.style.transition = 'transform 0.85s cubic-bezier(0.25, 0.9, 0.3, 1)'; loadFillEl.style.transform = 'scaleX(1)'; setTimeout(() => { loadDone = true; loadPctEl.textContent = '100%'; verSet(VER_TO.toFixed(1)); loadGlossEl.style.width = '100%'; }, 850); };   // …and the FINAL number is VER_TO, not a literal: this said '1.0' from when that WAS the version, so the drum spent the whole load counting up to VER_TO and then snapped back to 1.0 at the moment the world arrived — the one frame of it anybody actually reads (user 2026-08-21 asked for v1.3 and this is why the loading screen would still have said v1.0)   // verSet, NOT textContent: assigning text here destroyed the drum markup (the .vd cells) and left plain text behind.   // world ready → GLIDE the last stretch to full over 0.85 s so it never snaps (user: "jumps from 80% to 100%"); the % follows the compositor the whole way, then pins to a true 100% once the glide has actually arrived
  const palTrace = [];                                 // ── WHERE THE 256 WENT ── palette.length sampled at every load stage, so the ceiling can be attributed to a LOADER instead of guessed at. __vb.palTrace() reads it.
  const stage = async (msg) => { try { palTrace.push([msg, palette.length, performance.now()]); } catch (e) {} loadMsgEl.textContent = msg; await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))); };

