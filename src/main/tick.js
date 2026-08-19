  function tick(now) {                                 // an uncaught exception used to kill the loop = PERMANENT FREEZE (nothing requeues rAF). Now it's logged once with its stack (__vbErr) and the loop survives.
    try { tickBody(now);
      // ── A HEALTHY FRAME CLEARS THE RUN ── tickErrN is the give-up counter and nothing ever reset it, so it
      // counted CUMULATIVE session errors, not consecutive ones: a fault firing once every few seconds crossed
      // 3600 after an hour of otherwise perfect play and killed the loop for good, looking like a late-session
      // memory or driver problem rather than the thing that had been ticking over since minute two. It also made
      // the banner below fire on 60 scattered errors and call a healthy game "throwing every frame".
      if (tickErrN) { tickErrN = 0; const d9 = document.getElementById('tickDead'); if (d9) d9.remove(); } }
    catch (e) {
      tickErrN++;
      if (tickErrN <= 5 || tickErrN % 300 === 0) vbNoteErr('tick exception #' + tickErrN, e);
      // ── A REPEATING FAULT MUST SAY SO ON SCREEN (user 2026-08-18) ── this catch is what keeps a transient
      // error from killing the loop, and it is also what made a real crash invisible for hours: a ReferenceError
      // thrown by every fish placement produced NO uncaught exception for any tool to collect, passed the linter,
      // and left a perfectly rendered frame on the canvas because tickBody aborts BEFORE the render. Three gates
      // all reported a healthy game whose loop was dead. The catch is right; the silence was not.
      // 60 frames of solid failure is a second — far past "transient" and far short of the 3600 give-up — and it
      // raises the same banner a lost GPU device raises, for the same reason: the picture freezing at the last
      // good frame is exactly what both look like, and the player should never have to guess which.
      if (tickErrN >= 60 && !document.getElementById('tickDead')) {   // >= not ===, because a frame that throws while the banner is being built would skip an exact match and never warn again
        const b = document.createElement('div');
        b.id = 'tickDead';
        b.style.cssText = 'position:fixed;left:0;right:0;top:0;z-index:98;padding:8px 12px;background:rgba(40,8,12,0.92);color:#ff8a95;font:8px "px3",Consolas,monospace;line-height:1.6;text-align:center';
        b.textContent = 'the game loop is throwing every frame — ' + String((e && (e.message || e)) || 'unknown').slice(0, 160) + '  (see __vb.errLog())';
        document.body.appendChild(b);
      }
      if (!tickReq && tickErrN < 3600) requestAnimationFrame(tick);   // keep the game alive on a transient error; a solid minute of failing frames gives up (something is truly broken)
    }
  }
