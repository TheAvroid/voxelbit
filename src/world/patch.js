  // ── THE CHOKE POINT ── every runtime mutation of W already passes through here, so this is where the
  // support resolver is fed: the set of cells that ACTUALLY CHANGED, not a box centred on where the tool
  // landed. `track` exists because `full` is not a valid discriminator — the pickup paths patch full=true
  // and the worm stamps patch full=false — and because two callers are genuinely not terrain edits:
  // creature grid stamps (they are CONDUIT, and re-adjudicating the world every time a bird shifts its feet
  // would be both wrong and expensive) and the frozen editor stage.
  function gpuPatch(cells, full = true, n = cells.length, track = true) {              // few voxels: STAGE the touched words for the frame's patch scatter + refresh the touched brick bits. full=false uploads ONLY the touched brick words (per-frame worm stamps — the whole-buffer re-upload is far too heavy at 60 fps)
    const bset = pgBset; bset.clear();
    let cx0 = 1e9, cx1 = -1e9, cy0 = 1e9, cy1 = -1e9, cz0 = 1e9, cz1 = -1e9;   // bbox of the cells this batch EMPTIED — see phWakeNear
    for (let q = 0; q < n; q++) {
      const ii = cells[q];
      if (patchN >= PATCHMAX) patchFlush();            // stage full mid-frame → dispatch what we have and keep going (bounded memory, unchanged result)
      patchIdx[patchN++] = ii >> 2;                    // duplicates are fine: patchEncode reads the FINAL word value from W32
      const gx = ii % WX, gy = ((ii / WX) | 0) % WY, gz = (ii / (WX * WY)) | 0;
      bset.add((gx >> 3) + (gy >> 3) * BX + (gz >> 3) * BX * BY);
      stopS[gx + gz * WX] = 0;                         // scanTop cache: this column's top may have moved
      if (track) nvTouch(gx, gz);                      // ── NAVFIELD ── every runtime TERRAIN mutation of W funnels through here, so this is the ONE place the nav column index has to be told anything (chop/dig, till, pickup, snow settle). track=false is precisely the two cases the field must IGNORE: a perched bird's grid stamp only ever overwrites air or needles with cells stampedIdx already excludes, so its solidity contribution is nil before AND after; and the editor's stage plane is a frozen borrow of the world that gets rolled back voxel for voxel.
      if (track) {
        supPush(ii);
        // ── hmap MAINTENANCE, LOWER-ONLY ── the anchor oracle is sound only while hmap is never
        // STALE-HIGH, and physChopAt, phSeparate and phBodyFromCells have always mutated W without
        // touching it (so scanTop/landSnowAt worked off a stale surface after a felling, too). Lowering is
        // the conservative direction: stale-low only means the flood takes a few more hops before it finds
        // an anchored neighbour, while stale-high would wrongly anchor. Nothing here ever RAISES it — a
        // retired chip or a landed flake must not make the ground read taller than it is.
        if (!W[ii] && gy < hmap[gx + gz * WX]) { hmap[gx + gz * WX] = gy < 1 ? 1 : gy; SUP.stats.hmapLower++; }
        if (!W[ii]) {                                  // a cell this batch emptied — the rigid bodies overhead need to hear about it
          if (gx < cx0) cx0 = gx; if (gx > cx1) cx1 = gx;
          if (gy < cy0) cy0 = gy; if (gy > cy1) cy1 = gy;
          if (gz < cz0) cz0 = gz; if (gz > cz1) cz1 = gz;
        }
      }
    }
    // ── …AND WAKE ANY SLEEPING BODY OVER IT ── once per batch, against at most PH.maxBodies bodies. A body
    // that dozed off on ground the player then carved away had nothing else in the game to tell it (see
    // phWakeNear). The box is in WINDOW coords; phWakeNear un-wraps it to world before testing the bodies.
    if (track && phWakeHook && cx1 >= cx0) phWakeHook(cx0, cx1, cy0, cy1, cz0, cz1);
    for (const b of bset) {
      const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0;
      let occ = 0;
      scan: for (let z = bz * 8; z < bz * 8 + 8; z++) for (let y = by * 8; y < by * 8 + 8; y++) {
        const rw = (y * WX + z * WX * WY + bx * 8) >> 2;             // whole u32 words — 4 voxels per test
        if (W32[rw] | W32[rw + 1]) { occ = 1; break scan; }
      }
      if (occ) bricks[b >> 5] |= 1 << (b & 31); else bricks[b >> 5] &= ~(1 << (b & 31));
      wbricks[b >> 5] &= ~(1 << (b & 31));             // any runtime edit (snow landing, worm stamp, editor…) CLEARS the water-only bit — conservative and lossless: the brick just falls back to fine-stepping
      if (!full) dirtyBW.add(b >> 5);
    }
    const c2set = pgC2set; c2set.clear();              // refresh the touched L2 super-cells too
    for (const b of bset) { const bx = b % BX, by = ((b / BX) | 0) % BY, bz = (b / (BX * BY)) | 0; c2set.add((bx >> 2) + (by >> 2) * B2X + (bz >> 2) * B2X * B2Y); }
    for (const c of c2set) {
      const cx = c % B2X, cy = ((c / B2X) | 0) % B2Y, cz = (c / (B2X * B2Y)) | 0;
      let occ = 0;
      scan2: for (let bz = cz * 4; bz < cz * 4 + 4; bz++) for (let by = cy * 4; by < cy * 4 + 4; by++) for (let bx = cx * 4; bx < cx * 4 + 4; bx++) {
        const b = bx + by * BX + bz * BX * BY;
        if ((bricks[b >> 5] >>> (b & 31)) & 1) { occ = 1; break scan2; }
      }
      if (occ) bricks2[c >> 5] |= 1 << (c & 31); else bricks2[c >> 5] &= ~(1 << (c & 31));
      if (!full) dirtyC2W.add(c >> 5);
    }
    if (full) { uploadBricks(); dirtyBW.clear(); dirtyC2W.clear(); }   // a whole-array upload supersedes every word pending from earlier this frame
  }


