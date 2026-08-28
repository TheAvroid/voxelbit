  // ── keybinds ── rebindable, persisted to localStorage
  const DEFBINDS = { forward: 'KeyW', back: 'KeyS', left: 'KeyA', right: 'KeyD', jump: 'Space', sprint: 'ShiftLeft', crouch: 'CapsLock', drop: 'KeyQ', record: 'KeyR', scaledown: 'BracketLeft', scaleup: 'BracketRight' };   // L AND V ARE BOTH UNBOUND: the water panel L used to open is off screen and v-sync was removed outright. Back-lit foliage is baked in and needs no key.   // crouch on CAPS LOCK (user 2026-08-05), off Alt   // fly on F restored AGAIN (user 2026-08-27), after one day off — the removal was the const-and-prune method the rows below use, so bringing it back is deleting that prune line and putting these two entries back, nothing else.
  const BINDNAMES = { forward: 'walk forward', back: 'walk back', left: 'strafe left', right: 'strafe right', jump: 'jump / swim up', sprint: 'sprint', crouch: 'crouch / fly down', drop: 'drop item', record: 'record screen', scaledown: 'render scale -', scaleup: 'render scale +' };
  const binds = { ...DEFBINDS };
  try { const sv = JSON.parse(localStorage.getItem('vb_binds') || '{}'); for (const k in sv) if (k in binds) binds[k] = sv[k]; } catch (e) {}
  if (binds.crouch === 'KeyC' || binds.crouch === 'AltLeft') { binds.crouch = 'CapsLock'; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // MIGRATE: crouch went C → left Alt → CAPS LOCK (user 2026-08-05), carrying saved binds with it. Alt is no longer a crouch key at all.
  if (binds.vignette !== undefined) { delete binds.vignette; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // K IS NOW UNBOUND (user 2026-08-06): the vignette had this key and nothing else does. A saved bind from before still carries it, so drop it on load or the old handler's key would linger in the rebind panel. The vignette itself is unchanged and still toggles from its settings button.
  if (binds.shaft !== undefined) { delete binds.shaft; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // L IS UNBOUND AGAIN (user 2026-08-08): the sun shafts it opened were removed for cost. Prune a saved bind or the dead key survives.
  if (binds.fol !== undefined) { delete binds.fol; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // L IS UNBOUND AGAIN (user 2026-08-08): back-lit foliage is baked in at a fixed 30%, so there is nothing to toggle. Prune a saved bind or the dead key survives.
  if (binds.vsync !== undefined) { delete binds.vsync; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // V IS NOW UNBOUND (user 2026-08-08): v-sync was removed outright — the frame loop always pipelines off the previous frame's GPU completion now. Same case as the two below: prune a saved bind so no dead key survives.
  if (binds.lit !== undefined) { delete binds.lit; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // L IS NOW UNBOUND (user 2026-08-08): the atmospheric sky it opened was removed.
  if (binds.birds !== undefined) { delete binds.birds; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // Y IS UNBOUND (user 2026-08-27) — the toggle itself is untouched: CARD_FORCE still gates the count in main/tick-life.js and __vb.cardN(0|-1) still drives it. Prune a bind saved while Y was live, or the dead key lingers in the rebind panel AND ui/input.js fires off it
  if (binds.fly !== undefined) { delete binds.fly; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // F IS UNBOUND AGAIN (user 2026-08-27) — fly keeps every line of its code and still switches on from __vb.fly(), the camera rig and the asset editor; a bind saved while F still flew has to be pruned or the dead key lingers in the rebind panel AND ui/input.js fires the toggle off it
  if (binds.dof !== undefined) { delete binds.dof; try { localStorage.setItem('vb_binds', JSON.stringify(binds)); } catch (e) {} }   // L IS NOW UNBOUND (user 2026-08-08): same case as the vignette above — a bind saved before this carries dof, so drop it on load or the dead key lingers in the rebind panel. Depth of field itself is unchanged and still toggles from its settings button.
  const prettyKey = (c) => c.replace(/^Key|^Digit/, '').replace('BracketLeft', '[').replace('BracketRight', ']')
    .replace('ShiftLeft', 'L-SHIFT').replace('ShiftRight', 'R-SHIFT').replace('ControlLeft', 'L-CTRL').replace('ControlRight', 'R-CTRL')
    .replace('AltLeft', 'L-ALT').replace('AltRight', 'R-ALT').replace('CapsLock', 'CAPS LOCK')
    .replace('Space', 'SPACE').replace('Arrow', '').toLowerCase();   // keybind menu is all lowercase (user)
  const kbPanel = $('kbPanel'), kbRows = $('kbRows');
  let listenAction = null;
  function kbRefresh() {
    kbRows.innerHTML = '';
    for (const a in DEFBINDS) {
      const row = document.createElement('div'); row.className = 'kbRow';
      const lbl = document.createElement('span'); lbl.textContent = BINDNAMES[a];
      const btn = document.createElement('button'); btn.className = 'kbKey' + (listenAction === a ? ' listen' : '');
      btn.textContent = listenAction === a ? 'press a key…' : prettyKey(binds[a]);
      btn.addEventListener('click', (e) => { e.stopPropagation(); listenAction = a; kbRefresh(); });
      row.appendChild(lbl); row.appendChild(btn); kbRows.appendChild(row);
    }
  }
  $('kbBtn').addEventListener('click', (e) => { e.stopPropagation(); kbPanel.classList.remove('hidden'); kbRefresh(); });

