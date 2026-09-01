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
    const za = wz * 0.00098 + 77.9;                     // pineField octave A, row-specialized (mirrors fbm's constants exactly)
    const a1 = rowNoise(za), a2 = rowNoise(za * 2.13 + 5.3), a3 = rowNoise(za * 4.41 + 23.8);
    const zb2 = wz * 0.00257 + 13.7;                    // ...octave B
    const b1 = rowNoise(zb2), b2 = rowNoise(zb2 * 2.13 + 5.3), b3 = rowNoise(zb2 * 4.41 + 23.8);
    const zc = wz * 0.011 + 9.1;                       // ...and C
    const c1 = rowNoise(zc), c2 = rowNoise(zc * 2.13 + 5.3), c3 = rowNoise(zc * 4.41 + 23.8);
    const bsr = rowNoise(wz * 0.0016 + 157.3);
    const znB = wz * 0.05 + 4.2;                       // bed/beach relief fbm, row-specialized
    const e1 = rowNoise(znB), e2 = rowNoise(znB * 2.13 + 5.3), e3 = rowNoise(znB * 4.41 + 23.8);
    return (wx) => {
      const xa = wx * 0.00098 + 61.3;
      const a = a1(xa) * 0.55 + a2(xa * 2.13 + 11.7) * 0.27 + a3(xa * 4.41 + 41.2) * 0.18;
      const xb2 = wx * 0.00257 + 25.1;
      const b = b1(xb2) * 0.55 + b2(xb2 * 2.13 + 11.7) * 0.27 + b3(xb2 * 4.41 + 41.2) * 0.18;
      const xc = wx * 0.011 + 3.7;
      const c = c1(xc) * 0.55 + c2(xc * 2.13 + 11.7) * 0.27 + c3(xc * 4.41 + 41.2) * 0.18;
      let h = Math.min(HMAX, Math.max(PINE_LOW, Math.round(PINE_BASE + PINE_RELIEF * Math.pow(sstep(sstep(a * 0.585 + b * 0.320 + c * 0.095)), PINE_WET))));
      const b0 = bsr(wx * 0.0016 + 313.7);
      const bt = basinT(wx, wz); const bm = b0 >= bt ? 0 : sstep(Math.min(1, (bt - b0) / 0.06));
      const m = bm * basinLow(h, wx, wz);
      if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(wx * 13 + 7, wz * 17 + 3) - 0.5) * 0.8);
      const rs = riverS(wx, wz);
      const xnB = wx * 0.05 + 13.7;
      const bn = e1(xnB) * 0.55 + e2(xnB * 2.13 + 11.7) * 0.27 + e3(xnB * 4.41 + 41.2) * 0.18;
      if (rs > 0.02) h = Math.min(h, Math.round((h - Math.max(0, h - (WL + RIVLAND)) * rs) * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(wx * 19 + 5, wz * 23 + 9) - 0.5) * 0.8));
      // (no shoreline height edit - see the note in window.js H)
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
    const xa = wx * 0.00098 + 61.3;                     // pineField octave A, column-specialized
    const a1 = colNoise(xa), a2 = colNoise(xa * 2.13 + 11.7), a3 = colNoise(xa * 4.41 + 41.2);
    const xb2 = wx * 0.00257 + 25.1;                    // ...octave B
    const b1 = colNoise(xb2), b2 = colNoise(xb2 * 2.13 + 11.7), b3 = colNoise(xb2 * 4.41 + 41.2);
    const xc = wx * 0.011 + 3.7;                       // ...and C
    const c1 = colNoise(xc), c2 = colNoise(xc * 2.13 + 11.7), c3 = colNoise(xc * 4.41 + 41.2);
    const bsc = colNoise(wx * 0.0016 + 313.7);
    const xnB = wx * 0.05 + 13.7;                      // bed/beach relief fbm, column-specialized
    const e1 = colNoise(xnB), e2 = colNoise(xnB * 2.13 + 11.7), e3 = colNoise(xnB * 4.41 + 41.2);
    return (wz) => {
      const za = wz * 0.00098 + 77.9;
      const a = a1(za) * 0.55 + a2(za * 2.13 + 5.3) * 0.27 + a3(za * 4.41 + 23.8) * 0.18;
      const zb2 = wz * 0.00257 + 13.7;
      const b = b1(zb2) * 0.55 + b2(zb2 * 2.13 + 5.3) * 0.27 + b3(zb2 * 4.41 + 23.8) * 0.18;
      const zc = wz * 0.011 + 9.1;
      const c = c1(zc) * 0.55 + c2(zc * 2.13 + 5.3) * 0.27 + c3(zc * 4.41 + 23.8) * 0.18;
      let h = Math.min(HMAX, Math.max(PINE_LOW, Math.round(PINE_BASE + PINE_RELIEF * Math.pow(sstep(sstep(a * 0.585 + b * 0.320 + c * 0.095)), PINE_WET))));
      const b0 = bsc(wz * 0.0016 + 157.3);
      const bt = basinT(wx, wz); const bm = b0 >= bt ? 0 : sstep(Math.min(1, (bt - b0) / 0.06));
      const m = bm * basinLow(h, wx, wz);
      if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(wx * 13 + 7, wz * 17 + 3) - 0.5) * 0.8);
      const rs = riverS(wx, wz);
      const znB2 = wz * 0.05 + 4.2;
      const bn = e1(znB2) * 0.55 + e2(znB2 * 2.13 + 5.3) * 0.27 + e3(znB2 * 4.41 + 23.8) * 0.18;
      if (rs > 0.02) h = Math.min(h, Math.round((h - Math.max(0, h - (WL + RIVLAND)) * rs) * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(wx * 19 + 5, wz * 23 + 9) - 0.5) * 0.8));
      // (no shoreline height edit - see the note in window.js H)
      return h;
    };
  }
  function makeMossCol(wx) {
    const xm = wx * 0.031 + 31.7;
    const m1 = colNoise(xm), m2 = colNoise(xm * 2.13 + 11.7), m3 = colNoise(xm * 4.41 + 41.2);
    return (wz) => { const zm = wz * 0.031 + 17.2; return m1(zm) * 0.55 + m2(zm * 2.13 + 5.3) * 0.27 + m3(zm * 4.41 + 23.8) * 0.18; };
  }

