  // ── shared hierarchical DDA ── (window space, toroidal lookups — one conditional subtract per axis, no modulo). Included by TRACE
  // and by COMPOSITE (creature pixels trace a REAL sun-occlusion ray so moving things sit in tree shade exactly like the terrain).
  // The including module must declare storage buffers named `world`, `bricks`, `bricks2` (any binding slots — WGSL module scope is order-free).
  const FLAKEBLK_SRC = () => /* wgsl */`
    fn flakeBlocked(cw : vec3<f32>) -> bool {                       // a falling flake may exist ONLY in truly open air:
      // ── STORM EDGES ── snow ARRIVES from the sky and LEAVES from the sky. u.misc.y is the leading edge, sweeping
      // down at the storm's onset so flakes enter from the top of the screen instead of the whole column switching on
      // at once; u.misc.z is the trailing edge, sweeping down as it ends so the sky clears first and the last flakes
      // finish their fall. Between storms the two sit wide open and this costs one compare.
      if (cw.y < u.misc.y || cw.y > u.misc.z) { return true; }
      let ceilF = f32((u32(u.fx) >> 8u) & 31u) * 32.0;               // world ceiling (u.fx bits 8+): no solid at or above it, so a flake up there is in open air by definition
      if (cw.y - 1.0 >= ceilF) { return false; }
      if (cw.y >= f32(WY - 2)) { return false; }                     // above the world — open sky
      if (cw.y <= 2.0) { return true; }
      let a = vec3<i32>(floor(cw - vec3<f32>(0.5)));                 // (1) its full 10 cm cube must not touch ANY solid — all 8 corners tested, so it
      let b = vec3<i32>(floor(cw + vec3<f32>(0.5)));                 //     can never poke into a trunk, a leaf cluster or the ground
      if (voxAt(a) != 0u || voxAt(b) != 0u ||
          voxAt(vec3<i32>(b.x, a.y, a.z)) != 0u || voxAt(vec3<i32>(a.x, b.y, a.z)) != 0u ||
          voxAt(vec3<i32>(a.x, a.y, b.z)) != 0u || voxAt(vec3<i32>(b.x, b.y, a.z)) != 0u ||
          voxAt(vec3<i32>(b.x, a.y, b.z)) != 0u || voxAt(vec3<i32>(a.x, b.y, b.z)) != 0u) { return true; }
      let ci = vec3<i32>(floor(cw));                                 // (2) and its column must be OPEN SKY. Voxel-exact for the first 16 above — the old
      for (var k = 1; k <= 16; k++) {                                //     sparse probe ladder slipped BETWEEN thin canopy layers and cave shells, so
        let py = ci.y + k;                                           //     flakes clipped through trees, the ground and into caves…
        if (py >= WY - 2) { return false; }
        if (voxAt(vec3<i32>(ci.x, py, ci.z)) != 0u) { return true; }
      }
      let bx = ci.x >> 3; let bz = ci.z >> 3;                        // …then BRICK-level (8³ occupancy — anything solid in the 8×8 column blocks) up to
      let byTop = min(min(BY - 1, (ci.y + 128) >> 3), (i32(ceilF) - 1) >> 3);   //     +128: no gap a cave chamber or a canopy can hide in — and never past the world ceiling (nothing is up there)
      for (var by = (ci.y + 17) >> 3; by <= byTop; by++) {
        if (brickOcc(vec3<i32>(bx, by, bz))) { return true; }
      }
      return false;
    }
  `;

  const DDAW_SRC = (POOL) => /* wgsl */`
    const B2X : i32 = ${GBX >> 2}; const B2Y : i32 = ${GBY >> 2}; const B2Z : i32 = ${GBZ >> 2};
    const WTv : u32 = ${WATER_T}u; const WBv : u32 = ${WATER_B}u;   // lake surface / body voxel ids
    // ── FOLIAGE SEE-THROUGH ── when the camera clips INTO leaves, the primary ray treats foliage within folSkipD of the
    // eye as air, so the leaves you are buried in never fill the screen. A per-invocation private var (not a trace() arg)
    // so ONLY the primary ray sets it — the sun / AO / reflection traces keep foliage OPAQUE (the canopy still shadows).
    var<private> folSkipD : f32 = 0.0;
    const FOLSKIP : bool = §FOL§;                        // pipeline SPECIALIZATION: only the see-through TRACE variant (picked per frame when the eye is near foliage) pays the per-step check below — the normal-play variant compiles it out of the hottest loop entirely
    fn isFol(v : u32) -> bool { return ${foliageIds.length ? foliageIds.map((id) => 'v == ' + id + 'u').join(' || ') : 'false'}; }
    struct Hit { t : f32, vox : u32, face : u32, n : vec3<f32>, vc : vec3<i32>, }   // vc = the hit cell in the BODY's own grid (rigid bodies only) — grain has to be indexed in the object's frame, not the world's, or it crawls whenever the object moves
    fn voxAt(c : vec3<i32>) -> u32 {
      var x = c.x + i32(u.off.x); if (x >= WX) { x -= WX; }
      var z = c.z + i32(u.off.y); if (z >= WZ) { z -= WZ; }
${!POOL ? `      let i = u32(x + c.y * WX + z * WX * WY);
      return (world[i >> 2u] >> ((i & 3u) * 8u)) & 255u;` : `      // ── PAGED BRICK POOL ── an all-air brick has NO payload at all: its descriptor is 0 and the
      // 512 bytes the dense array spent on it do not exist. ~47% of the window is exactly that.
      let b = u32((x >> 3) + (c.y >> 3) * BX + (z >> 3) * BX * BY);
      let d = bdesc[b];
      if (d == 0u) { return 0u; }
      let l = u32((x & 7) + (c.y & 7) * 8 + (z & 7) * 64);
      return (pool[(d - 1u) * 128u + (l >> 2u)] >> ((l & 3u) * 8u)) & 255u;`}
    }
    fn brickOcc(c : vec3<i32>) -> bool {
      var x = c.x + (i32(u.off.x) >> 3); if (x >= BX) { x -= BX; }
      var z = c.z + (i32(u.off.y) >> 3); if (z >= BZ) { z -= BZ; }
      let i = u32(x + c.y * BX + z * BX * BY);
${!POOL ? `      return ((bricks[i >> 5u] >> (i & 31u)) & 1u) != 0u;`
        : `      return bdesc[i] != 0u;`}
    }
    fn brickOcc2(c : vec3<i32>) -> bool {
      var x = c.x + (i32(u.off.x) >> 5); if (x >= B2X) { x -= B2X; }
      var z = c.z + (i32(u.off.y) >> 5); if (z >= B2Z) { z -= B2Z; }
      let i = u32(x + c.y * B2X + z * B2X * B2Y);
      return ((bricks2[i >> 5u] >> (i & 31u)) & 1u) != 0u;
    }
    fn trace(ro : vec3<f32>, rdIn : vec3<f32>, maxT : f32, skipW : bool) -> Hit {
      var h : Hit; h.t = -1.0;
      let sgn = select(vec3<f32>(1.0), vec3<f32>(-1.0), rdIn < vec3<f32>(0.0));
      let rd = sgn * max(abs(rdIn), vec3<f32>(1e-6));
      let inv = 1.0 / rd;
      let ta = -ro * inv;
      let tb = (vec3<f32>(f32(WX), f32(WY), f32(WZ)) - ro) * inv;
      let tn = min(ta, tb); let tf = max(ta, tb);
      let t0 = max(max(tn.x, tn.y), max(tn.z, 0.0));
      let t1 = min(min(tf.x, tf.y), min(tf.z, maxT));
      if (t0 > t1) { return h; }
      var ax = 0;
      if (t0 > 0.0) { if (tn.y == t0) { ax = 1; } if (tn.z == t0) { ax = 2; } }
      else if (abs(rd.y) > abs(rd.x) && abs(rd.y) > abs(rd.z)) { ax = 1; }
      else if (abs(rd.z) > abs(rd.x)) { ax = 2; }
      let istep = vec3<i32>(sgn);
      var t = t0;
      let pc0 = ro + rd * (t + 1e-4);
      var cc = clamp(vec3<i32>(floor(pc0 / 32.0)), vec3<i32>(0), vec3<i32>(B2X - 1, B2Y - 1, B2Z - 1));
      var cNext = (vec3<f32>(cc + max(istep, vec3<i32>(0))) * 32.0 - ro) * inv;
      let cDelta = abs(inv) * 32.0;
      let bDelta = abs(inv) * 8.0;
      for (var ci = 0; ci < ${(WX >> 5) + (WY >> 5) + (WZ >> 5) + 8}; ci++) {   // L2: 32-voxel leaps through open air — long rays cross the window in ~1/4 the steps
        if (brickOcc2(cc)) {
          let bmin2 = cc * 4;
          var tb2 = t;
          var bax = ax;
          let pb = ro + rd * (tb2 + 1e-4);
          var bc = clamp(vec3<i32>(floor(pb * 0.125)), bmin2, bmin2 + 3);
          var bNext = (vec3<f32>(bc + max(istep, vec3<i32>(0))) * 8.0 - ro) * inv;
          var bxw = bmin2.x + (i32(u.off.x) >> 3); if (bxw >= BX) { bxw -= BX; }   // wrapped L1 base — off is 32-aligned so a super-cell's 4³ brick block never straddles the toroidal seam,
          var bzw = bmin2.z + (i32(u.off.y) >> 3); if (bzw >= BZ) { bzw -= BZ; }   // letting both inner loops keep a RUNNING flat index (one add per step) instead of re-wrapping per sample
          var bIdx = bxw + (bc.x - bmin2.x) + bc.y * BX + (bzw + (bc.z - bmin2.z)) * BX * BY;
          for (var bi = 0; bi < 16; bi++) {                          // L1: the 4³ bricks inside this super-cell
            let bd = ${POOL ? `bdesc[u32(bIdx)]` : `select(0u, 1u, ((bricks[u32(bIdx) >> 5u] >> (u32(bIdx) & 31u)) & 1u) != 0u)`};
            if (bd != 0u &&
                !(skipW && ((wbricks[u32(bIdx) >> 5u] >> (u32(bIdx) & 31u)) & 1u) != 0u)) {   // a WATER-ONLY brick holds nothing a skipW ray can hit — stride it like empty air (underwater camera, refraction, reflection all fly through the water volume at brick speed)
              let bmin = bc * 8;
              var tv = tb2;
              let pv = ro + rd * (tv + 1e-4);
              var vc = clamp(vec3<i32>(floor(pv)), bmin, bmin + 7);
              var vNext = (vec3<f32>(vc + max(istep, vec3<i32>(0))) - ro) * inv;
              var vax = bax;
${POOL ? `              let pbase = (bd - 1u) * 128u;
              var vIdx = (vc.x - bmin.x) + (vc.y - bmin.y) * 8 + (vc.z - bmin.z) * 64;`
        : `              var vIdx = ((bxw + (bc.x - bmin2.x)) << 3) + (vc.x - bmin.x) + vc.y * WX + (((bzw + (bc.z - bmin2.z)) << 3) + (vc.z - bmin.z)) * WX * WY;`}
              for (var j = 0; j < 32; j++) {
                let ii = u32(vIdx);
${POOL ? `                var v = (pool[pbase + (ii >> 2u)] >> ((ii & 3u) * 8u)) & 255u;`
        : `                var v = (world[ii >> 2u] >> ((ii & 3u) * 8u)) & 255u;`}
                if (skipW && (v == WTv || v == WBv)) { v = 0u; }      // water: invisible to an underwater camera
                if (FOLSKIP && folSkipD > 0.0 && tv < folSkipD && isFol(v)) { v = 0u; }   // near foliage: transparent to the clipped-in primary ray (dead-coded out of the non-FOLSKIP variant)
                if (v != 0u) {
                  h.t = tv; h.vox = v;
                  var n = vec3<f32>(0.0);
                  if (vax == 0) { n.x = -sgn.x; } else if (vax == 1) { n.y = -sgn.y; } else { n.z = -sgn.z; }
                  h.n = n;
                  h.face = u32(vax) * 2u + select(0u, 1u, (n.x + n.y + n.z) < 0.0);
                  return h;
                }
                var oob = false;
                if (vNext.x <= vNext.y && vNext.x <= vNext.z) { tv = vNext.x; vNext.x += abs(inv.x); vc.x += istep.x; vIdx += istep.x; vax = 0; oob = vc.x < bmin.x || vc.x > bmin.x + 7; }
                else if (vNext.y <= vNext.z) { tv = vNext.y; vNext.y += abs(inv.y); vc.y += istep.y; vIdx += istep.y * ${POOL ? `8` : `WX`}; vax = 1; oob = vc.y < bmin.y || vc.y > bmin.y + 7; }
                else { tv = vNext.z; vNext.z += abs(inv.z); vc.z += istep.z; vIdx += istep.z * ${POOL ? `64` : `WX * WY`}; vax = 2; oob = vc.z < bmin.z || vc.z > bmin.z + 7; }
                if (tv > t1) { return h; }
                if (oob) { break; }
              }
            }
            var oobB = false;
            if (bNext.x <= bNext.y && bNext.x <= bNext.z) { tb2 = bNext.x; bNext.x += bDelta.x; bc.x += istep.x; bIdx += istep.x; bax = 0; oobB = bc.x < bmin2.x || bc.x > bmin2.x + 3; }
            else if (bNext.y <= bNext.z) { tb2 = bNext.y; bNext.y += bDelta.y; bc.y += istep.y; bIdx += istep.y * BX; bax = 1; oobB = bc.y < bmin2.y || bc.y > bmin2.y + 3; }
            else { tb2 = bNext.z; bNext.z += bDelta.z; bc.z += istep.z; bIdx += istep.z * BX * BY; bax = 2; oobB = bc.z < bmin2.z || bc.z > bmin2.z + 3; }
            if (tb2 > t1) { return h; }
            if (oobB) { break; }
          }
        }
        var oobC = false;
        if (cNext.x <= cNext.y && cNext.x <= cNext.z) { t = cNext.x; cNext.x += cDelta.x; cc.x += istep.x; ax = 0; oobC = cc.x < 0 || cc.x >= B2X; }
        else if (cNext.y <= cNext.z) { t = cNext.y; cNext.y += cDelta.y; cc.y += istep.y; ax = 1; oobC = cc.y < 0 || cc.y >= B2Y; }
        else { t = cNext.z; cNext.z += cDelta.z; cc.z += istep.z; ax = 2; oobC = cc.z < 0 || cc.z >= B2Z; }
        if (t > t1) { return h; }
        if (oobC) { return h; }
      }
      return h;
    }
    // ── RIGID BODY TRACE ── walks the transformed voxel grid of every live body and returns the nearest
    // hit, in the SAME window-space convention trace() uses. Because it shares that convention it can be
    // used by the primary ray AND by shadow / AO / reflection rays, which is what makes a felled tree
    // light exactly like a standing one instead of needing baked AO and a box-shaped fake shadow.
    // stopAny: the caller only wants to know WHETHER something is in the way (the sun-shadow ray). The nearest
    // of several hits costs a full walk of every group the ray touches; ANY hit ends it. Same answer for an
    // occlusion test, and it can only ever return sooner.
    fn bodyTraceX(ro : vec3<f32>, rd : vec3<f32>, maxT : f32, stopAny : bool) -> Hit {
      var h : Hit; h.t = -1.0;
      let nB = i32(u.physC.x + 0.5);
      if (nB <= 0) { return h; }
      { let dc = u.physBound.xyz - ro;                              // one sphere test against ALL bodies
        let tc = dot(dc, rd);
        let r2 = u.physBound.w * u.physBound.w;
        if (dot(dc, dc) - tc * tc > r2) { return h; }               // ray misses the whole set
        if (tc < -u.physBound.w || tc - u.physBound.w > maxT) { return h; }
      }
      var best = maxT;
      // ── SLAB CULL ── the bodies are published nearest-first, so PHYS_GRP consecutive of them form a depth
      // slab and u.physG holds one sphere per slab. Missing a slab skips all of its bodies on one compare,
      // which is what keeps a felled tree's ~250 chunks off the per-ray cost of 250 rejections.
      let nG = (nB + PHYS_GRP - 1) / PHYS_GRP;
      for (var gi = 0; gi < nG; gi = gi + 1) {
        let GS = u.physG[gi];
        if (GS.w <= 0.0) { continue; }
        let dG = GS.xyz - ro;
        let tG = dot(dG, rd);
        if (tG - GS.w > best || tG + GS.w < 0.0) { continue; }
        if (dot(dG, dG) - tG * tG > GS.w * GS.w) { continue; }
        let bLo = gi * PHYS_GRP;
        var bHi = bLo + PHYS_GRP; if (bHi > nB) { bHi = nB; }
      for (var bi = bLo; bi < bHi; bi++) {
        let A = u.physB[bi * 5]; let Xa = u.physB[bi * 5 + 1]; let Ya = u.physB[bi * 5 + 2];
        let Za = u.physB[bi * 5 + 3]; let E = u.physB[bi * 5 + 4];
        let bw = i32(Xa.w + 0.5); let bh = i32(Ya.w + 0.5); let bd = i32(Za.w + 0.5);
        if (bw < 1) { continue; }
        let vsB = A.w;
        let dA = A.xyz - ro;
        // ── THE REJECT SPHERE IS ABOUT THE CENTRE OF MASS, SO ITS RADIUS MUST BE MEASURED FROM THERE ──
        // this took the box's half-diagonal, which is the right radius about the box CENTRE and the wrong
        // one about A.xyz, because A.xyz is the COM and E.xyz says where in the box that is. On a pine the
        // two are nearly the same point and nothing shows. A felled BIRCH is a long bole with the whole
        // crown's mass at one end: MEASURED on one, box 61 x 110 x 49 with the COM at y 71.4 of 110, so the
        // sphere came out 68.5 where the far corner is 83.6 away - 15 voxels of the body sticking out of
        // its own bounding sphere. Every ray aimed at that part was rejected BEFORE the DDA ran, so it drew
        // as nothing while the body was whole, simulated and solid; and as the tree fell and turned,
        // different parts crossed back inside the sphere and popped into view. Exactly the user's words:
        // "a portion of the trunk disappears, but as it falls, it reappears" (2026-08-26).
        // The farthest corner from an interior point is the one built from the larger span on each axis,
        // so this is exact, and E.xyz is already in a register - it costs three max()es.
        let hw = max(E.x, f32(bw) - E.x); let hh2 = max(E.y, f32(bh) - E.y); let hd = max(E.z, f32(bd) - E.z);
        let radB = vsB * (sqrt(hw * hw + hh2 * hh2 + hd * hd) + 1.0);
        let tcB = dot(dA, rd);
        if (tcB - radB > best || tcB + radB < 0.0) { continue; }
        if (dot(dA, dA) - tcB * tcB > radB * radB) { continue; }
        let d0 = ro - A.xyz;                                        // ray origin in body-local voxel space
        let roB = vec3<f32>(dot(d0, Xa.xyz), dot(d0, Ya.xyz), dot(d0, Za.xyz)) / vsB + E.xyz;
        var rdB = vec3<f32>(dot(rd, Xa.xyz), dot(rd, Ya.xyz), dot(rd, Za.xyz));
        if (abs(rdB.x) < 1e-6) { rdB.x = 1e-6; }
        if (abs(rdB.y) < 1e-6) { rdB.y = 1e-6; }
        if (abs(rdB.z) < 1e-6) { rdB.z = 1e-6; }
        let invB = 1.0 / rdB;
        let taB = -roB * invB;
        let tbB = (vec3<f32>(f32(bw), f32(bh), f32(bd)) - roB) * invB;
        let tnB = min(taB, tbB); let tfB = max(taB, tbB);
        let teB = max(max(tnB.x, tnB.y), max(tnB.z, 0.0));
        let tlB = min(min(tfB.x, tfB.y), tfB.z);
        if (teB >= tlB || teB * vsB > best) { continue; }
        var vax = 0;
        if (tnB.y == teB) { vax = 1; } if (tnB.z == teB) { vax = 2; }
        var vc = clamp(vec3<i32>(floor(roB + rdB * (teB + 1e-4))), vec3<i32>(0), vec3<i32>(bw - 1, bh - 1, bd - 1));
        let ist = vec3<i32>(sign(rdB));
        var vNx = (vec3<f32>(vc + max(ist, vec3<i32>(0))) - roB) * invB;
        var tH = teB;
        let off = i32(E.w + 0.5);
        var iB = off + vc.x + vc.y * bw + vc.z * bw * bh;
        // ── THE WALK IS BOUNDED BY THIS BODY'S OWN BOX, NOT BY A PINE'S (user 2026-08-26: "a portion of the
        // trunk disappears, but as it falls, it reappears") ── this read q2 < 320, and its own comment said
        // what it was sized against: the longest diagonal of a PINE box, 35 x 36 x 116. A felled BIRCH is
        // 61 x 241 x 49, whose DDA needs up to 351 cells, and the box is ~98% AIR (8,240 voxels in 430,584
        // cells) so a ray really does have to march hundreds of empty cells to reach the trunk inside it.
        // Past 320 the walk gave up and reported NO HIT, so that part of the body drew as nothing while the
        // rest of it drew normally - and as the body fell and turned, different parts came back inside the
        // budget and popped into view. "It disappeared but it is still being rendered" is exactly right.
        // bw + bh + bd is the most cells a 3D DDA can visit in a bw x bh x bd box, so this is exact for any
        // model that exists or ever will, and it costs nothing: the walk already breaks the moment it leaves
        // the box, and only a ray that was crossing the whole thing was ever near the old ceiling.
        // The same stale-literal trap as CANOPY and the hmap+118/122 bounds - see voxelbit-tallest-pine-constants.
        let qMax = bw + bh + bd;
        for (var q2 = 0; q2 < qMax; q2++) {
          let cid = bodyVox[u32(iB)];
          if (cid != 0u) {
            if (tH * vsB < best) {
              best = tH * vsB;
              h.t = best; h.vox = cid; h.vc = vc;
              var nl = vec3<f32>(0.0);
              if (vax == 0) { nl.x = -f32(ist.x); } else if (vax == 1) { nl.y = -f32(ist.y); } else { nl.z = -f32(ist.z); }
              if (stopAny) { return h; }                        // occlusion only — the face and the nearest-of-many do not matter
              h.n = normalize(Xa.xyz * nl.x + Ya.xyz * nl.y + Za.xyz * nl.z);   // TRUE rotated world normal
              if (abs(h.n.y) >= abs(h.n.x) && abs(h.n.y) >= abs(h.n.z)) { h.face = select(2u, 3u, h.n.y < 0.0); }
              else if (abs(h.n.x) >= abs(h.n.z)) { h.face = select(0u, 1u, h.n.x < 0.0); }
              else { h.face = select(4u, 5u, h.n.z < 0.0); }
            }
            break;
          }
          if (vNx.x <= vNx.y && vNx.x <= vNx.z) { tH = vNx.x; vNx.x += abs(invB.x); vc.x += ist.x; iB += ist.x; vax = 0; }
          else if (vNx.y <= vNx.z) { tH = vNx.y; vNx.y += abs(invB.y); vc.y += ist.y; iB += ist.y * bw; vax = 1; }
          else { tH = vNx.z; vNx.z += abs(invB.z); vc.z += ist.z; iB += ist.z * bw * bh; vax = 2; }
          if (any(vc < vec3<i32>(0)) || any(vc >= vec3<i32>(bw, bh, bd))) { break; }
        }
      }
      }
      return h;
    }
    fn bodyTrace(ro : vec3<f32>, rd : vec3<f32>, maxT : f32) -> Hit { return bodyTraceX(ro, rd, maxT, false); }
    // Static world + rigid bodies, nearest wins. THIS is what secondary rays call, so a falling tree
    // casts a real tree-shaped shadow, darkens its own creases, and shows up in the water.
    fn traceAll(ro : vec3<f32>, rd : vec3<f32>, maxT : f32, skipW : bool) -> Hit {
      var h = trace(ro, rd, maxT, skipW);
      let lim = select(maxT, h.t, h.t >= 0.0);
      let b = bodyTrace(ro, rd, lim);
      if (b.t >= 0.0 && (h.t < 0.0 || b.t < h.t)) { return b; }
      return h;
    }
  `;
