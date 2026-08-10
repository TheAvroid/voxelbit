  let cineMode = false;                                // C — CINEMATIC still hold (frozen camera + subtle sway, HUD hidden)
  let cineBlurK = 0;                                   // eased 0..1 cinematic-vignette weight — ramped, never hard-toggled, or the blur POPS on at the C keypress
  let blurLock = false;                                // __vb.vig(v) pins the weight for A/B testing; vigFree() hands it back to the easing
  const CINE_VIG = 0.55;                               // CINEMATIC VIGNETTE depth at full weight (0 = none, 1 = corners to black). Motion blur was removed at user request 2026-07-18; this lane replaced it. The shader clamps the smear, so this saturates on fast banks (blur stays motion-proportional, never a full wipe). Tune live with __vb.blur(v).
  // ── CINEMATIC STILL HOLD ── (user 2026-07-19) C freezes the camera exactly where it was pressed for a clean, UI-less
  // framed shot. There is no flythrough any more — the only motion is a subtle multi-frequency handheld sway (see
  // cineUpdate), so the view reads like a locked-off camera on a soft mount rather than a bird in flight.
  const cine = { baseYaw: 0, basePitch: 0, baseX: 0, baseY: 0, baseZ: 0, bobT: 0, roll: 0 };
  const cineUpdate = (dt) => {                          // STILL HOLD (user): the cinematic camera no longer flies — it stays exactly where C was pressed and only breathes with a subtle handheld sway
    P.fly = true; P.vy = 0; P.onGround = false;         // hold physics off so gravity / ground-stick never tug the framed shot
    cine.bobT += dt;
    const t = cine.bobT;
    // Layered slow sines at incommensurate rates read as an organic hand-held drift rather than a visible loop. The
    // amplitudes are deliberately tiny — a few thousandths of a radian of look and a sub-voxel rise — so the frame
    // stays put and only gently breathes; nothing here advances the camera through the world.
    P.yaw   = cine.baseYaw   + Math.sin(t * 0.35) * 0.030 + Math.sin(t * 0.19) * 0.018;
    P.pitch = cine.basePitch + Math.sin(t * 0.27) * 0.024 + Math.sin(t * 0.13) * 0.012;
    const rollT = Math.sin(t * 0.23) * 0.030;           // a whisper of horizon roll
    cine.roll += (rollT - cine.roll) * (1 - Math.exp(-3 * dt));   // eased so the entry from level is smooth, not a step
    P.roll = cine.roll;
    P.x = cine.baseX; P.z = cine.baseZ;                 // position LOCKED — no flythrough
    P.y = cine.baseY + Math.sin(t * 0.31) * 1.05;       // one slow breath of vertical drift
  };
  let tickReq = false, tickErrN = 0;                   // freeze forensics: did THIS frame schedule the next one yet / how many frames have thrown
