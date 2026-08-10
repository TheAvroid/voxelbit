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
    const zb = wz * 0.008;
    const s1 = rowNoise(zb), s2 = rowNoise(zb * 2.13 + 5.3), s3 = rowNoise(zb * 4.41 + 23.8);
    const zd = wz * 0.04 + 2.1;
    const d1 = rowNoise(zd), d2 = rowNoise(zd * 2.13 + 5.3), d3 = rowNoise(zd * 4.41 + 23.8);
    const bsr = rowNoise(wz * 0.0016 + 157.3);
    const znB = wz * 0.05 + 4.2;                       // bed/beach relief fbm, row-specialized (mirrors fbm's octave constants exactly)
    const e1 = rowNoise(znB), e2 = rowNoise(znB * 2.13 + 5.3), e3 = rowNoise(znB * 4.41 + 23.8);
    return (wx) => {
      const xb = wx * 0.008;
      const b = 8 + LIFT + 88 * (s1(xb) * 0.55 + s2(xb * 2.13 + 11.7) * 0.27 + s3(xb * 4.41 + 41.2) * 0.18);
      const shoreK = Math.min(1, Math.abs(b - WL) / 12);
      const xd = wx * 0.04 + 7.3;
      let h = Math.min(HMAX, Math.max(4 + LIFT, Math.round(b + 9 * (d1(xd) * 0.55 + d2(xd * 2.13 + 11.7) * 0.27 + d3(xd * 4.41 + 41.2) * 0.18) * (0.2 + 0.8 * shoreK))));
      const b0 = bsr(wx * 0.0016 + 313.7);
      const bm = b0 >= 0.065 ? 0 : sstep(Math.min(1, (0.065 - b0) / 0.06));
      const m = bm * Math.max(0, Math.min(1, (66 + LIFT - h) / 20));
      if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(wx * 13 + 7, wz * 17 + 3) - 0.5) * 0.8);
      const rs = riverS(wx, wz);
      const xnB = wx * 0.05 + 13.7;
      const bn = e1(xnB) * 0.55 + e2(xnB * 2.13 + 11.7) * 0.27 + e3(xnB * 4.41 + 41.2) * 0.18;
      if (rs > 0.02) h = Math.min(h, Math.round(h * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(wx * 19 + 5, wz * 23 + 9) - 0.5) * 0.8));
      if (h <= WL && h >= WL - 5 && bm <= 0.25 && rs <= 0.04) h = WL + 1 + Math.max(0, Math.round((bn - 0.55) * 5));
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
    const xb = wx * 0.008;
    const s1 = colNoise(xb), s2 = colNoise(xb * 2.13 + 11.7), s3 = colNoise(xb * 4.41 + 41.2);
    const xd = wx * 0.04 + 7.3;
    const d1 = colNoise(xd), d2 = colNoise(xd * 2.13 + 11.7), d3 = colNoise(xd * 4.41 + 41.2);
    const bsc = colNoise(wx * 0.0016 + 313.7);
    const xnB = wx * 0.05 + 13.7;                      // bed/beach relief fbm, column-specialized
    const e1 = colNoise(xnB), e2 = colNoise(xnB * 2.13 + 11.7), e3 = colNoise(xnB * 4.41 + 41.2);
    return (wz) => {
      const zb = wz * 0.008;
      const b = 8 + LIFT + 88 * (s1(zb) * 0.55 + s2(zb * 2.13 + 5.3) * 0.27 + s3(zb * 4.41 + 23.8) * 0.18);
      const shoreK = Math.min(1, Math.abs(b - WL) / 12);
      const zd = wz * 0.04 + 2.1;
      let h = Math.min(HMAX, Math.max(4 + LIFT, Math.round(b + 9 * (d1(zd) * 0.55 + d2(zd * 2.13 + 5.3) * 0.27 + d3(zd * 4.41 + 23.8) * 0.18) * (0.2 + 0.8 * shoreK))));
      const b0 = bsc(wz * 0.0016 + 157.3);
      const bm = b0 >= 0.065 ? 0 : sstep(Math.min(1, (0.065 - b0) / 0.06));
      const m = bm * Math.max(0, Math.min(1, (66 + LIFT - h) / 20));
      if (m > 0) h = Math.round(h - m * (h - Math.max(6, LIFT - 40)) + (ihash(wx * 13 + 7, wz * 17 + 3) - 0.5) * 0.8);
      const rs = riverS(wx, wz);
      const znB2 = wz * 0.05 + 4.2;
      const bn = e1(znB2) * 0.55 + e2(znB2 * 2.13 + 5.3) * 0.27 + e3(znB2 * 4.41 + 23.8) * 0.18;
      if (rs > 0.02) h = Math.min(h, Math.round(h * (1 - rs) + (WL - 2 - 26 * rs) * rs + (bn - 0.5) * 9 * Math.min(1, rs * 2.2) + (ihash(wx * 19 + 5, wz * 23 + 9) - 0.5) * 0.8));
      if (h <= WL && h >= WL - 5 && bm <= 0.25 && rs <= 0.04) h = WL + 1 + Math.max(0, Math.round((bn - 0.55) * 5));
      return h;
    };
  }
  function makeMossCol(wx) {
    const xm = wx * 0.031 + 31.7;
    const m1 = colNoise(xm), m2 = colNoise(xm * 2.13 + 11.7), m3 = colNoise(xm * 4.41 + 41.2);
    return (wz) => { const zm = wz * 0.031 + 17.2; return m1(zm) * 0.55 + m2(zm * 2.13 + 5.3) * 0.27 + m3(zm * 4.41 + 23.8) * 0.18; };
  }

