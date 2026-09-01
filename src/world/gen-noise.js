  // ── ROW-CACHED noise ── the bulk fill sweeps rows (fixed wz, ascending wx), so every vnoise stream's z-lattice
  // pair is row-constant and its x-corners only change every 1/scale columns. These samplers return BIT-IDENTICAL
  // values to vnoise/fbm/H (same float expressions, same ihash corners) at ~1 amortized hash per column instead of ~40.
  function rowNoise(zArg) {
    const iz = Math.floor(zArg), fz = sstep(zArg - iz);
    let ix = 0x7fffffff, h00 = 0, h10 = 0, h01 = 0, h11 = 0;
    return (xArg) => {
      const nix = Math.floor(xArg);
      if (nix !== ix) {
        if (nix === ix + 1) { h00 = h10; h01 = h11; h10 = ihash(nix + 1, iz); h11 = ihash(nix + 1, iz + 1); }
        else { h00 = ihash(nix, iz); h10 = ihash(nix + 1, iz); h01 = ihash(nix, iz + 1); h11 = ihash(nix + 1, iz + 1); }
        ix = nix;
      }
      const fx = sstep(xArg - ix);
      return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
    };
  }
  function makeHRow(wz) {                              // H(x, wz) specialized for one row — every float op mirrors H exactly
    const bsr = rowNoise(wz * 0.0016 + 157.3);
    const znB = wz * 0.05 + 4.2;                       // bed/beach relief fbm, row-specialized (mirrors fbm's octave constants exactly)
    const e1 = rowNoise(znB), e2 = rowNoise(znB * 2.13 + 5.3), e3 = rowNoise(znB * 4.41 + 23.8);
    return (wx) => {
      // ── THE HEIGHT FIELD IS ONE SHARED SCALAR NOW ── the row/column noise decomposition existed to
      // cache the base fbm per row; the pine field calls fbm directly through pineBase (world/window.js),
      // which is the SAME function H() calls, so this copy cannot drift from it by construction. That is
      // the whole reason gtest exists, and it is the trade the codebase already makes for oakRoll/oakBank:
      // a little per-column noise work in exchange for the three copies being incapable of disagreeing.
      let h = deepen(Math.min(HMAX, Math.max(4 + LIFT, Math.round(pineBase(wx, wz)))));   // …the SAME shared helper H() calls, in the same place in the pass order
      const b0 = bsr(wx * 0.0016 + 313.7);
      const bt = basinT(wx, wz); const bm = b0 >= bt ? 0 : sstep(Math.min(1, (bt - b0) / 0.06));   // basinT: the arctic's doubled water lives in that shared constant — see world/window.js. This is copy 2/3 (and 3/3) of the threshold; H() has the other.
      const m = bm * basinLow(h, wx, wz);              // shared helper — the arctic's higher basin ceiling lives there, and this is 2 of 3 copies
      if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 52)) + (ihash(wx * 13 + 7, wz * 17 + 3) - 0.5) * 0.8);
      const rs = riverS(wx, wz);
      const xnB = wx * 0.05 + 13.7;
      const bn = e1(xnB) * 0.55 + e2(xnB * 2.13 + 11.7) * 0.27 + e3(xnB * 4.41 + 41.2) * 0.18;
      h = Math.round(oakBank(h, wx, wz));              // ── SHALLOW OAK BANKS ── the SAME shared scalar helper H() and makeHCol call, in the same place in the pass order. The arithmetic exists once, so the three copies cannot drift; see oakBank in window.js
      if (rs > 0.02) h = Math.min(h, Math.round(h * (1 - rs) + (WL - 2 - 38 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(wx * 19 + 5, wz * 23 + 9) - 0.5) * 0.8));
      if (h <= WL && h >= WL - 5 && bm <= 0.25 && rs <= 0.04) h = WL + 1 + Math.max(0, Math.round((bn - 0.55) * 5));
      // ── THE DESERT FLAT DOES NOT FILL IN LAKES (user 2026-08-16, screenshot: a forest lake bordering the
      // desert was sliced off along a dead-straight diagonal) ── the WL+2 lift below exists so the desert never
      // sits below sea level, and it was unconditional: every column past dm 0.5 was shoved above the water,
      // INCLUDING the bed of a lake straddling the line. So the water ended exactly on the dm=0.5 iso-line,
      // which at lake scale is a straight edge, and the shore dither on the far side left a dark fringe along
      // the cut. bm/rs are the same two predicates the beach-flat line already uses to mean "this column
      // belongs to a water body". A biome decides what the shore is MADE OF, never where the water ENDS.
      const dm = desertM(wx, wz); if (dm > 0) { h = Math.round(h * (1 - dm) + (DESY + duneH(wx, wz) + (fbm(wx * 0.012 + 5.1, wz * 0.012 + 9.3) - 0.5) * DESREL) * dm); if (dm > 0.5 && bm <= 0.25 && rs <= 0.04) h = Math.max(h, WL + 2); }   // ── DESERT FLAT ── the SAME expression as H() in window.js, calling the SAME scalar desertM/fbm. These three copies of the height function have to agree bit-for-bit or the bulk fill and the placement queries disagree about where the ground is; sharing the function is what makes that true by construction rather than by careful copying.
      return h;
    };
  }
  function makeMossRow(wz) {                           // fillColumn's moss fbm, row-specialized (exact)
    const zm = wz * 0.031 + 17.2;
    const m1 = rowNoise(zm), m2 = rowNoise(zm * 2.13 + 5.3), m3 = rowNoise(zm * 4.41 + 23.8);
    return (wx) => { const xm = wx * 0.031 + 31.7; return m1(xm) * 0.55 + m2(xm * 2.13 + 11.7) * 0.27 + m3(xm * 4.41 + 41.2) * 0.18; };
  }
  function colNoise(xArg) {                            // the transposed twin: x-lattice fixed, walks z — for column-major sweeps
    const ix = Math.floor(xArg), fx = sstep(xArg - ix);
    let iz = 0x7fffffff, h00 = 0, h10 = 0, h01 = 0, h11 = 0;
    return (zArg) => {
      const niz = Math.floor(zArg);
      if (niz !== iz) {
        if (niz === iz + 1) { h00 = h01; h10 = h11; h01 = ihash(ix, niz + 1); h11 = ihash(ix + 1, niz + 1); }
        else { h00 = ihash(ix, niz); h10 = ihash(ix + 1, niz); h01 = ihash(ix, niz + 1); h11 = ihash(ix + 1, niz + 1); }
        iz = niz;
      }
      const fz = sstep(zArg - iz);
      return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
    };
  }
  function makeHCol(wx) {                              // H(wx, z) specialized for one COLUMN — exact, for x-direction bands
    const bsc = colNoise(wx * 0.0016 + 313.7);
    const xnB = wx * 0.05 + 13.7;                      // bed/beach relief fbm, column-specialized
    const e1 = colNoise(xnB), e2 = colNoise(xnB * 2.13 + 11.7), e3 = colNoise(xnB * 4.41 + 41.2);
    return (wz) => {
      // ── THE HEIGHT FIELD IS ONE SHARED SCALAR NOW ── the row/column noise decomposition existed to
      // cache the base fbm per row; the pine field calls fbm directly through pineBase (world/window.js),
      // which is the SAME function H() calls, so this copy cannot drift from it by construction. That is
      // the whole reason gtest exists, and it is the trade the codebase already makes for oakRoll/oakBank:
      // a little per-column noise work in exchange for the three copies being incapable of disagreeing.
      let h = deepen(Math.min(HMAX, Math.max(4 + LIFT, Math.round(pineBase(wx, wz)))));   // …the SAME shared helper H() calls, in the same place in the pass order
      const b0 = bsc(wz * 0.0016 + 157.3);
      const bt = basinT(wx, wz); const bm = b0 >= bt ? 0 : sstep(Math.min(1, (bt - b0) / 0.06));   // basinT: the arctic's doubled water lives in that shared constant — see world/window.js. This is copy 2/3 (and 3/3) of the threshold; H() has the other.
      const m = bm * basinLow(h, wx, wz);              // shared helper — the arctic's higher basin ceiling lives there, and this is 2 of 3 copies
      if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 52)) + (ihash(wx * 13 + 7, wz * 17 + 3) - 0.5) * 0.8);
      const rs = riverS(wx, wz);
      const znB2 = wz * 0.05 + 4.2;
      const bn = e1(znB2) * 0.55 + e2(znB2 * 2.13 + 5.3) * 0.27 + e3(znB2 * 4.41 + 23.8) * 0.18;
      h = Math.round(oakBank(h, wx, wz));              // ── SHALLOW OAK BANKS ── identical to H() and makeHRow. See the note in makeHRow.
      if (rs > 0.02) h = Math.min(h, Math.round(h * (1 - rs) + (WL - 2 - 38 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(wx * 19 + 5, wz * 23 + 9) - 0.5) * 0.8));
      if (h <= WL && h >= WL - 5 && bm <= 0.25 && rs <= 0.04) h = WL + 1 + Math.max(0, Math.round((bn - 0.55) * 5));
      // ── THE DESERT FLAT DOES NOT FILL IN LAKES (user 2026-08-16, screenshot: a forest lake bordering the
      // desert was sliced off along a dead-straight diagonal) ── the WL+2 lift below exists so the desert never
      // sits below sea level, and it was unconditional: every column past dm 0.5 was shoved above the water,
      // INCLUDING the bed of a lake straddling the line. So the water ended exactly on the dm=0.5 iso-line,
      // which at lake scale is a straight edge, and the shore dither on the far side left a dark fringe along
      // the cut. bm/rs are the same two predicates the beach-flat line already uses to mean "this column
      // belongs to a water body". A biome decides what the shore is MADE OF, never where the water ENDS.
      const dm = desertM(wx, wz); if (dm > 0) { h = Math.round(h * (1 - dm) + (DESY + duneH(wx, wz) + (fbm(wx * 0.012 + 5.1, wz * 0.012 + 9.3) - 0.5) * DESREL) * dm); if (dm > 0.5 && bm <= 0.25 && rs <= 0.04) h = Math.max(h, WL + 2); }   // ── DESERT FLAT ── identical to H() and makeHRow. See the note in makeHRow.
      return h;
    };
  }
  function makeMossCol(wx) {
    const xm = wx * 0.031 + 31.7;
    const m1 = colNoise(xm), m2 = colNoise(xm * 2.13 + 11.7), m3 = colNoise(xm * 4.41 + 41.2);
    return (wz) => { const zm = wz * 0.031 + 17.2; return m1(zm) * 0.55 + m2(zm * 2.13 + 5.3) * 0.27 + m3(zm * 4.41 + 23.8) * 0.18; };
  }

