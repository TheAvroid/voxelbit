  // @module — fragmented-MP4 → flat-MP4 remux, so an export opens in an NLE
  // @exports veMp4Parse, veRemuxFmp4, rmBox, rmTable
  // ═══════════════ MAKE EVERY EXPORT OPEN IN DAVINCI RESOLVE ═══════════════ (user 2026-08-21)
  // MediaRecorder can only emit a FRAGMENTED mp4. Its `moov` is ~1.2 KB and the `stts`/`stsz`/`stsc`/`stco` sample
  // tables inside it are EMPTY — the real samples are described by `trun` boxes in the 20-odd `moof` fragments spread
  // through the file. Browsers and ffmpeg read that happily. DaVinci Resolve does not: it reads the moov sample
  // tables, counts zero samples, and refuses the file, which is why dragging an export into it did nothing at all.
  // Diagnosed on `voxelbit-clip (32).mp4` — `mvex`+`trex` present, `nb_frames=N/A` on both streams, 20 moof/mdat pairs.
  // This walks the fragments, rebuilds the flat sample tables the moov should have had, and re-emits ftyp+moov+mdat.
  // ── IT IS A REMUX, NOT A RE-ENCODE ── every compressed byte is passed through untouched, so it costs no quality
  // (the export is already a second encode; a third was never acceptable — see the standing no-quality-sacrifice rule)
  // and near-zero time. It also never reads the media into JS: the sample bytes are carried as `blob.slice()`
  // references and only the ~42 KB of box headers is ever parsed, so a 900 MB export costs 900 MB of nothing.
  // The per-sample durations from the `trun`s are written into `stts` VERBATIM, so the file keeps the exact frame
  // timing the encoder produced. Forcing a uniform cadence here would resample motion, which is the same mistake as
  // dropping `-fps_mode passthrough` from the ffmpeg recipe — don't.
  const RM_HDR = 8, RM_NONSYNC = 0x00010000;              // sample_is_non_sync_sample is bit 16 of the 32-bit sample_flags; the low 16 are sample_degradation_priority
  const rmType = (dv, p) => String.fromCharCode(dv.getUint8(p), dv.getUint8(p + 1), dv.getUint8(p + 2), dv.getUint8(p + 3));
  const rmWalk = (dv, start, end, cb) => {                // iterate the child boxes of one already-in-memory range
    let p = start;
    while (p + RM_HDR <= end) {
      let sz = dv.getUint32(p), h = RM_HDR;
      const ty = rmType(dv, p + 4);
      if (sz === 1) { if (p + 16 > end) break; sz = Number(dv.getBigUint64(p + 8)); h = 16; }
      else if (sz === 0) sz = end - p;                    // "to end of file" — legal for the last box
      if (sz < h || p + sz > end) break;                  // truncated or nonsense: stop rather than read past it
      cb(ty, p + h, p + sz, p);
      p += sz;
    }
  };
  const rmFind = (dv, start, end, ty) => { let r = null; rmWalk(dv, start, end, (t, b, e, s) => { if (t === ty && !r) r = { body: b, end: e, start: s }; }); return r; };
  const rmPath = (dv, box, ...tys) => { let cur = box; for (const t of tys) { if (!cur) return null; cur = rmFind(dv, cur.body, cur.end, t); } return cur; };

  // ── TOP-LEVEL SCAN WITHOUT READING THE FILE ── only the 8/16-byte headers are fetched, so the multi-MB `mdat`
  // payloads are located and then left on disk. 42 slices for a 360 MB export.
  const rmScan = async (blob) => {
    const out = [];
    let p = 0;
    while (p + RM_HDR <= blob.size) {
      const buf = await blob.slice(p, Math.min(p + 16, blob.size)).arrayBuffer();
      if (buf.byteLength < RM_HDR) break;
      const dv = new DataView(buf);
      let sz = dv.getUint32(0), h = RM_HDR;
      const ty = rmType(dv, 4);
      if (sz === 1) { if (buf.byteLength < 16) break; sz = Number(dv.getBigUint64(8)); h = 16; }
      else if (sz === 0) sz = blob.size - p;
      if (sz < h || p + sz > blob.size) break;
      out.push({ ty, start: p, body: p + h, end: p + sz });
      p += sz;
    }
    return out;
  };

  // ── BOX WRITERS ── every table is sized exactly, so the moov can be built twice (once to learn its length, once
  // with the real chunk offsets that length determines) and come out byte-identical both times.
  const rmBox = (ty, ...parts) => {                       // parts: Uint8Array pieces of the payload
    let n = 0; for (const q of parts) n += q.length;
    const out = new Uint8Array(RM_HDR + n), dv = new DataView(out.buffer);
    dv.setUint32(0, out.length);
    for (let i = 0; i < 4; i++) out[4 + i] = ty.charCodeAt(i);
    let o = RM_HDR; for (const q of parts) { out.set(q, o); o += q.length; }
    return out;
  };
  const rmU32 = (...v) => { const a = new Uint8Array(v.length * 4), d = new DataView(a.buffer); v.forEach((x, i) => d.setUint32(i * 4, x >>> 0)); return a; };
  const rmTable = (ty, ver, rows, wide) => {              // a full-box table: version/flags, entry_count, then `rows` flat u32s
    const a = new Uint8Array(8 + rows.length * 4), d = new DataView(a.buffer);
    d.setUint32(0, (ver & 0xff) << 24);
    d.setUint32(4, rows.length / wide);
    for (let i = 0; i < rows.length; i++) d.setUint32(8 + i * 4, rows[i] >>> 0);
    return rmBox(ty, a);
  };

  // ── FULL-BOX DURATION PATCHING ── mvhd/tkhd/mdhd are copied from the source byte-for-byte (matrix, display size,
  // language, volume and all) and only their duration field is rewritten, because a fragmented init writes 0 there.
  // Chrome emits VERSION 1 of all three, so the 64-bit layout is the live path and the v0 branch is the safety net.
  const rmCopyPatch = (src, box, kind, dur) => {
    const out = src.slice(box.start, box.end), dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
    const ver = dv.getUint8(RM_HDR);
    const at = RM_HDR + (kind === 'tkhd' ? (ver === 1 ? 28 : 20) : (ver === 1 ? 24 : 16));   // byte offset of the DURATION field inside the box body. mvhd/mdhd share a layout (…creation, modification, timescale, duration); tkhd carries track_ID + a reserved word where the other two carry timescale, so its duration sits one word further in
    if (ver === 1) dv.setBigUint64(at, BigInt(Math.max(0, Math.round(dur))));
    else dv.setUint32(at, Math.min(0xfffffffe, Math.max(0, Math.round(dur))));
    return out;
  };
  const rmTimescale = (dv, box, kind) => {                // mvhd/mdhd only
    const ver = dv.getUint8(box.body);
    return dv.getUint32(box.body + (ver === 1 ? 20 : 12));
  };

  // ═══════════════ THE PARSE ═══════════════
  // Reads a fragmented mp4 down to a flat per-track sample list and returns null for anything it cannot safely
  // handle. It backs TWO callers: the remux below, and the WebCodecs export in ui/video-editor.js, which needs
  // exactly the same thing — every sample's byte range, decode time and sync flag — to feed a VideoDecoder.
  // Sample fields: { size, dur, cts, sync, off, dts } with `off` absolute in `blob` and `dts` in the track timescale.
  const veMp4Parse = async (blob) => {
    const boxes = await rmScan(blob);
    const moovB = boxes.find((b) => b.ty === 'moov'), ftypB = boxes.find((b) => b.ty === 'ftyp');
    const moofs = boxes.filter((b) => b.ty === 'moof');
    if (!moovB) return null;                              // not an mp4 at all. A file with no `moof` is NOT rejected here: that is the flat case, and since the WebCodecs recorder muxes its own takes it is now the COMMON one
    const moovRaw = new Uint8Array(await blob.slice(moovB.start, moovB.end).arrayBuffer());
    const mdv = new DataView(moovRaw.buffer);
    const mv = { body: RM_HDR, end: moovRaw.length, start: 0 };   // `moovRaw` is the moov box itself, re-based to 0
    const fragmented = !!rmFind(mdv, mv.body, mv.end, 'mvex');

    // ── TRACK TEMPLATES ── everything the flat moov needs that is NOT a sample table, kept as raw source bytes.
    const mvhdB = rmFind(mdv, mv.body, mv.end, 'mvhd');
    if (!mvhdB) return null;
    const movTs = rmTimescale(mdv, mvhdB, 'mvhd');
    const tracks = new Map();
    rmWalk(mdv, mv.body, mv.end, (ty, b, e, s) => {
      if (ty !== 'trak') return;
      const trak = { body: b, end: e, start: s };
      const tkhd = rmFind(mdv, b, e, 'tkhd'), mdia = rmFind(mdv, b, e, 'mdia');
      if (!tkhd || !mdia) return;
      const mdhd = rmFind(mdv, mdia.body, mdia.end, 'mdhd'), hdlr = rmFind(mdv, mdia.body, mdia.end, 'hdlr');
      const minf = rmFind(mdv, mdia.body, mdia.end, 'minf');
      if (!mdhd || !hdlr || !minf) return;
      const stbl = rmPath(mdv, minf, 'stbl'), stsd = rmPath(mdv, minf, 'stbl', 'stsd');
      if (!stbl || !stsd) return;
      const tver = mdv.getUint8(tkhd.body);
      const id = mdv.getUint32(tkhd.body + (tver === 1 ? 20 : 12));
      const mhd = rmFind(mdv, minf.body, minf.end, 'vmhd') || rmFind(mdv, minf.body, minf.end, 'smhd');
      const dinf = rmFind(mdv, minf.body, minf.end, 'dinf');
      if (!mhd || !dinf) return;
      const kind = rmType(mdv, mhd.start + 4) === 'vmhd' ? 'video' : 'audio';
      let avcC = null;                                    // the decoder's `description`. A visual sample entry buries its child boxes behind 78 bytes of fixed fields, so the search has to start past them
      if (kind === 'video') rmWalk(mdv, stsd.body + 8, stsd.end, (t2, b2, e2) => { if (avcC) return; const c = rmFind(mdv, b2 + 78, e2, 'avcC'); if (c) avcC = moovRaw.slice(c.body, c.end); });
      tracks.set(id, { id, kind, avcC, tkhd, mdhd, hdlr, mhd, dinf, stsd, stbl, mediaTs: rmTimescale(mdv, mdhd, 'mdhd'),
                       defDur: 0, defSize: 0, defFlags: 0, defDesc: 1,
                       samples: [], chunks: [], sumDur: 0, t0: 0, tfdtSeen: false });
    });
    if (!tracks.size) return null;

    // ── A FLAT FILE ALREADY HAS ITS SAMPLE TABLES ── and since the WebCodecs recorder muxes its takes itself
    // (see veWCRecStart), the common case is now a file with no `mvex` at all. Refusing those here is what silently
    // sent the export back to the MediaRecorder path — it looked like a recorder bug and was a parser gap.
    if (!fragmented) {
      for (const t of tracks.values()) {
        const rd = (ty) => rmFind(mdv, t.stbl.body, t.stbl.end, ty);
        const stsz = rd('stsz'), stts = rd('stts'), stsc = rd('stsc'), stco = rd('stco'), co64 = rd('co64'), stss = rd('stss'), ctts = rd('ctts');
        if (!stsz || !stts || !stsc || !(stco || co64)) continue;
        const n = mdv.getUint32(stsz.body + 8), uni = mdv.getUint32(stsz.body + 4);
        if (!n) continue;
        const size = (i) => (uni || mdv.getUint32(stsz.body + 12 + i * 4));
        const dur = new Array(n);                         // stts is run-length: (count, delta) pairs
        { let i = 0; const e = mdv.getUint32(stts.body + 4);
          for (let k = 0; k < e && i < n; k++) { const c = mdv.getUint32(stts.body + 8 + k * 8), d = mdv.getUint32(stts.body + 12 + k * 8);
            for (let j = 0; j < c && i < n; j++) dur[i++] = d; }
          while (i < n) dur[i++] = dur[i - 2] || 0; }
        const cts = new Array(n).fill(0);
        if (ctts) { let i = 0; const e = mdv.getUint32(ctts.body + 4), v1 = mdv.getUint8(ctts.body) === 1;
          for (let k = 0; k < e && i < n; k++) { const c = mdv.getUint32(ctts.body + 8 + k * 8);
            const o = v1 ? mdv.getInt32(ctts.body + 12 + k * 8) : mdv.getUint32(ctts.body + 12 + k * 8);
            for (let j = 0; j < c && i < n; j++) cts[i++] = o; } }
        const sync = new Array(n).fill(!stss);            // no stss ⇒ EVERY sample is a sync sample, which is the audio case
        if (stss) { const e = mdv.getUint32(stss.body + 4);
          for (let k = 0; k < e; k++) { const q = mdv.getUint32(stss.body + 8 + k * 4) - 1; if (q >= 0 && q < n) sync[q] = true; } }
        const nch = mdv.getUint32((stco || co64).body + 4);
        const chOff = (i) => (co64 ? Number(mdv.getBigUint64(co64.body + 8 + i * 8)) : mdv.getUint32(stco.body + 8 + i * 4));
        const runs = [];                                  // stsc gives (first_chunk, samples_per_chunk) and runs until the next entry
        { const e = mdv.getUint32(stsc.body + 4);
          for (let k = 0; k < e; k++) runs.push({ first: mdv.getUint32(stsc.body + 8 + k * 12) - 1, per: mdv.getUint32(stsc.body + 12 + k * 12), desc: mdv.getUint32(stsc.body + 16 + k * 12) }); }
        let si = 0, clock = 0;
        for (let c = 0; c < nch && si < n; c++) {
          let r = runs[0];
          for (const q of runs) if (q.first <= c) r = q;
          let off = chOff(c);
          for (let j = 0; j < r.per && si < n; j++) {
            const sz = size(si);
            t.samples.push({ size: sz, dur: dur[si], sync: sync[si], cts: cts[si], off, dts: clock });
            off += sz; clock += dur[si]; si++;
          }
        }
        t.sumDur = clock; t.t0 = 0;
      }
      const liveFlat = [...tracks.values()].filter((t) => t.samples.length);
      if (!liveFlat.length) return null;
      return { movTs, moovRaw, mvhdB, ftypB, tracks, live: liveFlat, chunkOrder: [], fragmented };
    }

    // ── trex DEFAULTS ── a trun may omit duration/size/flags per sample and inherit them from tfhd, which may in
    // turn inherit from here. Miss this chain and every sample lands with a zero duration.
    const mvex = rmFind(mdv, mv.body, mv.end, 'mvex');
    rmWalk(mdv, mvex.body, mvex.end, (ty, b) => {
      if (ty !== 'trex') return;
      const t = tracks.get(mdv.getUint32(b + 4));
      if (!t) return;
      t.defDesc = mdv.getUint32(b + 8) || 1; t.defDur = mdv.getUint32(b + 12); t.defSize = mdv.getUint32(b + 16); t.defFlags = mdv.getUint32(b + 20);
    });

    // ── WALK THE FRAGMENTS ── one `trun` becomes one CHUNK, which keeps stsc to a couple of entries and preserves the
    // original track interleave (the chunks are emitted in file order below, not grouped by track).
    if (!moofs.length) return null;                       // fragmented per the moov, but carrying no fragments
    const chunkOrder = [];
    for (const mf of moofs) {
      const raw = new Uint8Array(await blob.slice(mf.start, mf.end).arrayBuffer());
      const fdv = new DataView(raw.buffer);
      const moofBase = mf.start;                          // absolute; `default-base-is-moof` and the spec default both land here
      let prevRunEnd = moofBase;                          // spec: with no base-data-offset, a later traf starts where the previous one's data ended
      rmWalk(fdv, RM_HDR, raw.length, (ty, tb, te) => {
        if (ty !== 'traf') return;
        const tfhd = rmFind(fdv, tb, te, 'tfhd');
        if (!tfhd) return;
        let p = tfhd.body + 4;
        const fl = fdv.getUint32(tfhd.body) & 0xffffff;
        const trackId = fdv.getUint32(p); p += 4;
        const t = tracks.get(trackId);
        let base = (fl & 0x020000) ? moofBase : prevRunEnd;
        if (fl & 0x000001) { base = Number(fdv.getBigUint64(p)); p += 8; }
        const desc = (fl & 0x000002) ? (p += 4, fdv.getUint32(p - 4)) : (t ? t.defDesc : 1);
        const dDur = (fl & 0x000008) ? (p += 4, fdv.getUint32(p - 4)) : (t ? t.defDur : 0);
        const dSize = (fl & 0x000010) ? (p += 4, fdv.getUint32(p - 4)) : (t ? t.defSize : 0);
        const dFlags = (fl & 0x000020) ? (p += 4, fdv.getUint32(p - 4)) : (t ? t.defFlags : 0);
        if (!t) return;
        const tfdt = rmFind(fdv, tb, te, 'tfdt');
        if (tfdt) {                                       // ── ABSORB A TIMELINE GAP ── the export PAUSES the recorder across every clip seek (see veExport), so a fragment can legitimately start later than the samples before it imply. Stretch the previous sample to cover it, exactly as a muxer would, or the tracks drift apart by the gap
          const dver = fdv.getUint8(tfdt.body);
          const bt = dver === 1 ? Number(fdv.getBigUint64(tfdt.body + 4)) : fdv.getUint32(tfdt.body + 4);
          if (t.tfdtSeen && t.samples.length) { const last = t.samples[t.samples.length - 1], nd = Math.max(0, last.dur + (bt - t.sumDur)); t.sumDur += nd - last.dur; last.dur = nd; }   // ── tfdt IS AUTHORITATIVE, IN BOTH DIRECTIONS ── the per-sample durations in a trun are the encoder's estimate and drift from it (measured: +217 ticks over 101 frames on clip 29), while baseMediaDecodeTime is where the fragment REALLY starts. Absorb the difference into the previous sample either way. Growing alone was not enough — the common case is the run OVERSHOOTING, which then pushed every later frame late and the tracks apart
          if (!t.tfdtSeen) { t.sumDur = bt; t.t0 = bt; }   // a first fragment may legitimately begin at a non-zero media time; that offset is a START, so it is subtracted back out of every duration below
          t.tfdtSeen = true;
        }
        let runCursor = null;
        rmWalk(fdv, tb, te, (ty2, rb) => {
          if (ty2 !== 'trun') return;
          const rfl = fdv.getUint32(rb) & 0xffffff, n = fdv.getUint32(rb + 4);
          let q = rb + 8;
          let start = runCursor === null ? base : runCursor;
          if (rfl & 0x000001) { start = base + fdv.getInt32(q); q += 4; }
          const firstFlags = (rfl & 0x000004) ? (q += 4, fdv.getUint32(q - 4)) : null;
          const first = t.samples.length;
          let bytes = 0;
          for (let i = 0; i < n; i++) {
            const dur = (rfl & 0x000100) ? (q += 4, fdv.getUint32(q - 4)) : dDur;
            const size = (rfl & 0x000200) ? (q += 4, fdv.getUint32(q - 4)) : dSize;
            const sfl = (rfl & 0x000400) ? (q += 4, fdv.getUint32(q - 4)) : (i === 0 && firstFlags !== null ? firstFlags : dFlags);
            const cts = (rfl & 0x000800) ? (q += 4, fdv.getInt32(q - 4)) : 0;   // read SIGNED: a v1 ctts is negative-capable and misreading it as unsigned would put frames a billion ticks apart
            t.samples.push({ size, dur, sync: !(sfl & RM_NONSYNC), cts, off: start + bytes, dts: t.sumDur });   // `off`/`dts` are for the demux caller; the remux itself works off the chunk ranges and never reads them
            t.sumDur += dur; bytes += size;
          }
          const ch = { trackId, srcStart: start, bytes, count: n, first, desc: desc || 1, out: 0 };
          t.chunks.push(ch); chunkOrder.push(ch);
          runCursor = start + bytes;
          prevRunEnd = runCursor;
        });
      });
    }
    const live = [...tracks.values()].filter((t) => t.samples.length);
    if (!live.length) return null;
    return { movTs, moovRaw, mvhdB, ftypB, tracks, live, chunkOrder, fragmented };
  };

  // ═══════════════ THE REMUX ═══════════════
  // Returns a new Blob, or null when there is nothing to do (already flat, or a container this cannot safely touch).
  // Null is not an error — the caller downloads the original, which is exactly what shipped before this existed.
  const veRemuxFmp4 = async (blob) => {
    const P = await veMp4Parse(blob);
    if (!P || !P.fragmented) return null;                 // already flat: its tables are real and there is nothing to rebuild
    const { movTs, moovRaw, mvhdB, ftypB, live, chunkOrder } = P;

    // ── LAYOUT ── ftyp + moov + mdat, in that order, which IS faststart. The moov's length depends on nothing but the
    // entry counts, so it is built once with placeholder offsets purely to measure it, then again for real.
    const ftyp = ftypB ? new Uint8Array(await blob.slice(ftypB.start, ftypB.end).arrayBuffer())
                       : rmBox('ftyp', new Uint8Array([0x69, 0x73, 0x6f, 0x6d, 0, 0, 2, 0, 0x69, 0x73, 0x6f, 0x6d, 0x69, 0x73, 0x6f, 0x32, 0x61, 0x76, 0x63, 0x31, 0x6d, 0x70, 0x34, 0x31]));
    let mdatBytes = 0; for (const ch of chunkOrder) mdatBytes += ch.bytes;
    const wide = mdatBytes + 0x10000 > 0xffffffff;        // a >4 GB payload needs a 64-bit mdat and co64 offsets; the slack covers ftyp+moov, which cannot be sized until after this decision
    const mdatHdr = wide ? 16 : RM_HDR;

    const buildMoov = () => {
      const parts = [rmCopyPatch(moovRaw, mvhdB, 'mvhd', Math.max(...live.map((t) => ((t.sumDur - t.t0) / t.mediaTs) * movTs)))];
      for (const t of live) {
        const stts = [];                                  // run-length: {count, delta}
        for (const s of t.samples) { const n = stts.length; if (n && stts[n - 1] === s.dur) stts[n - 2]++; else stts.push(1, s.dur); }
        const sizes = t.samples.map((s) => s.size);
        const sync = [], ctts = [];
        let allSync = true, anyCts = false;
        t.samples.forEach((s, i) => { if (s.sync) sync.push(i + 1); else allSync = false; if (s.cts) anyCts = true; });
        for (const s of t.samples) { const n = ctts.length; if (n && ctts[n - 1] === s.cts) ctts[n - 2]++; else ctts.push(1, s.cts); }
        const stsc = [];
        t.chunks.forEach((ch, i) => { const n = stsc.length; if (n && stsc[n - 2] === ch.count && stsc[n - 1] === ch.desc) return; stsc.push(i + 1, ch.count, ch.desc); });
        const offs = [];
        for (const ch of t.chunks) { if (wide) { offs.push(Math.floor(ch.out / 0x100000000), ch.out >>> 0); } else offs.push(ch.out); }
        const stblParts = [moovRaw.slice(t.stsd.start, t.stsd.end), rmTable('stts', 0, stts, 2), rmTable('stsc', 0, stsc, 3)];
        const szA = new Uint8Array(12 + sizes.length * 4), szD = new DataView(szA.buffer);
        szD.setUint32(4, 0); szD.setUint32(8, sizes.length);
        sizes.forEach((v, i) => szD.setUint32(12 + i * 4, v));
        stblParts.push(rmBox('stsz', szA));
        stblParts.push(rmTable(wide ? 'co64' : 'stco', 0, offs, wide ? 2 : 1));
        if (!allSync) stblParts.push(rmTable('stss', 0, sync, 1));
        if (anyCts) stblParts.push(rmTable('ctts', ctts.some((v, i) => i % 2 === 1 && v < 0) ? 1 : 0, ctts, 2));
        const trakDur = ((t.sumDur - t.t0) / t.mediaTs) * movTs;
        parts.push(rmBox('trak',
          rmCopyPatch(moovRaw, t.tkhd, 'tkhd', trakDur),
          rmBox('mdia',
            rmCopyPatch(moovRaw, t.mdhd, 'mdhd', t.sumDur - t.t0),
            moovRaw.slice(t.hdlr.start, t.hdlr.end),
            rmBox('minf', moovRaw.slice(t.mhd.start, t.mhd.end), moovRaw.slice(t.dinf.start, t.dinf.end), rmBox('stbl', ...stblParts)))));
      }
      return rmBox('moov', ...parts);
    };
    const probe = buildMoov();                            // measured only — its chunk offsets are all zero
    let cursor = ftyp.length + probe.length + mdatHdr;
    for (const ch of chunkOrder) { ch.out = cursor; cursor += ch.bytes; }
    const moov = buildMoov();
    if (moov.length !== probe.length) return null;        // the two passes must agree or every offset written is wrong; bail to the original rather than ship a broken file

    const mhdr = new Uint8Array(mdatHdr), hd = new DataView(mhdr.buffer);
    if (wide) { hd.setUint32(0, 1); hd.setBigUint64(8, BigInt(mdatBytes + 16)); } else hd.setUint32(0, mdatBytes + RM_HDR);
    for (let i = 0; i < 4; i++) mhdr[4 + i] = 'mdat'.charCodeAt(i);   // the type word sits at 4 in BOTH layouts — a 64-bit box is size=1, type, then largesize
    const parts = [ftyp, moov, mhdr];
    for (const ch of chunkOrder) parts.push(blob.slice(ch.srcStart, ch.srcStart + ch.bytes));   // the media rides through as slice REFERENCES — never read into JS, so file size costs no heap
    return new Blob(parts, { type: 'video/mp4' });
  };
