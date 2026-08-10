  function tick(now) {                                 // an uncaught exception used to kill the loop = PERMANENT FREEZE (nothing requeues rAF). Now it's logged once with its stack (__vbErr) and the loop survives.
    try { tickBody(now); }
    catch (e) {
      tickErrN++;
      if (tickErrN <= 5 || tickErrN % 300 === 0) vbNoteErr('tick exception #' + tickErrN, e);
      if (!tickReq && tickErrN < 3600) requestAnimationFrame(tick);   // keep the game alive on a transient error; a solid minute of failing frames gives up (something is truly broken)
    }
  }
