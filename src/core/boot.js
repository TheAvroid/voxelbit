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
  const VER_TO = 1.1;                                  // ── WHAT THE DRUM COUNTS UP TO ── the shipped version, and the ONE place it is written for the animation
  // (the static tag on the lock screen is in 10-body.html). Scaling the ramp by it rather than hardcoding 1
  // keeps the tick even: the counter is linear in TIME over LOAD_VER_MS, so 1.1 is simply twelve steps
  // instead of eleven at the same cadence, and toFixed(1) still yields the two digits verSet rolls.
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
  { loadFillEl.style.transform = 'scaleX(0)'; void loadFillEl.offsetWidth;   // commit 0 first so the transition actually runs from empty
    // 9s -> 3s (2026-08-09): the ease was tuned when boot took ~10 s. Boot is now ~2 s, so the bar was only
    // reaching 58-68% before the world landed and finishLoad had to glide the last 33-40 points — the same
    // lurch the 0.85 s glide was added to remove, just at the other end. Measured bar position at finishLoad
    // across two pinned worlds: 9s -> 60/67%, 3.8s -> 78/83%, 3s -> 81/86%, 2.2s -> 86/90%. 3 s halves the
    // jump on a fast machine while still giving a slow one three seconds of CLIMBING bar before the crawl
    // below takes over — going shorter buys a little more on fast boxes and parks slow ones in the crawl.
    loadFillEl.style.transition = 'transform 3s cubic-bezier(0.05, 0.7, 0.2, 1)';   // front-loaded ease-out: quick, responsive start that decelerates and lingers
    loadFillEl.style.transform = 'scaleX(0.9)';
    // ── AND THEN IT KEEPS CREEPING ── the ease above ENDS at 90% and used to sit there until finishLoad, so any
    // world that took longer than nine seconds showed a bar parked at 90% for the rest of the wait (user
    // 2026-08-07: "hits 90% where it freezes there for a few seconds"). Chain a long slow crawl that approaches
    // full without ever arriving, so the meter is always moving and 0→100 reads as one continuous sweep.
    // finishLoad overrides it the moment the world is really ready, and loadFinishing stops ITS transitionend
    // from starting the crawl again and pulling the bar back off 100%.
    setTimeout(() => {                                 // a TIMER, not transitionend: the element's transitionend
      if (loadDone || loadFinishing) return;           // proved unreliable to hook here, and this cannot be missed
      loadFillEl.style.transition = 'transform 75s cubic-bezier(0.12, 0.62, 0.3, 1)';
      loadFillEl.style.transform = 'scaleX(0.995)';
    }, 3050);
    requestAnimationFrame(loadNumStep); }
  const setLoad = () => {};                              // real progress no longer drives the BAR (the trickle owns it); kept as a no-op so the phase calls below stay harmless
  const finishLoad = () => { loadFinishing = true; loadFillEl.style.transition = 'transform 0.85s cubic-bezier(0.25, 0.9, 0.3, 1)'; loadFillEl.style.transform = 'scaleX(1)'; setTimeout(() => { loadDone = true; loadPctEl.textContent = '100%'; verSet('1.0'); loadGlossEl.style.width = '100%'; }, 850); };   // verSet, NOT textContent: assigning text here destroyed the drum markup (the .vd cells) and left plain text behind.   // world ready → GLIDE the last stretch to full over 0.85 s so it never snaps (user: "jumps from 80% to 100%"); the % follows the compositor the whole way, then pins to a true 100% once the glide has actually arrived
  const palTrace = [];                                 // ── WHERE THE 256 WENT ── palette.length sampled at every load stage, so the ceiling can be attributed to a LOADER instead of guessed at. __vb.palTrace() reads it.
  const stage = async (msg) => { try { palTrace.push([msg, palette.length]); } catch (e) {} loadMsgEl.textContent = msg; await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))); };

