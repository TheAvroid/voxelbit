  // @module — write a flat mp4 from freshly encoded samples
  // @exports veMp4Mux, veAvcStsd
  // ═══════════════ THE MUXER ═══════════════ (user 2026-08-21)
  // ui/mp4-remux.js rebuilds the tables of a file that already exists. This builds one from nothing: it takes the
  // EncodedVideoChunks a WebCodecs VideoEncoder produced, plus the AAC samples copied straight out of the recording,
  // and writes the mp4 around them. It exists because MediaRecorder cannot be told what time a frame is.
  // ── WHY THE EXPORT STOPPED USING MediaRecorder ── measured on four shipped exports: EVERY frame interval in them is
  // an integer multiple of 8.33 ms, the user's 120 Hz refresh period. MediaRecorder timestamps each requestFrame() by
  // the WALL CLOCK at the moment the compositor happened to present a frame, so the source's real frame times are
  // discarded and replaced by the refresh clock. Even the best export had only 48.6% of frames on a steady cadence —
  // 25.7% were held for three refreshes instead of two. That is the stutter, it is baked in at capture time, and no
  // encoder setting can undo it. A VideoEncoder honours the timestamp handed to it, so the export can lay frames on an
  // exactly even grid instead. Nothing else fixes this; the pump, the codec and the quiet gate were all tried first.
  const MX_HDR = 8;
  const mxNow = 0;                                        // creation/modification time: fixed, not Date.now(). A muxer that stamps the clock produces a different file for the same input, which makes every A/B byte-compare useless
  const mxU8 = (...v) => new Uint8Array(v);
  const mxU16 = (v) => new Uint8Array([(v >> 8) & 0xff, v & 0xff]);
  const mxU32s = (...v) => { const a = new Uint8Array(v.length * 4), d = new DataView(a.buffer); v.forEach((x, i) => d.setUint32(i * 4, x >>> 0)); return a; };
  const mxU64 = (v) => { const a = new Uint8Array(8), d = new DataView(a.buffer); d.setBigUint64(0, BigInt(Math.max(0, Math.round(v)))); return a; };
  const mxStr = (s, n) => { const a = new Uint8Array(n); for (let i = 0; i < Math.min(s.length, n - 1); i++) a[i + 1] = s.charCodeAt(i); a[0] = Math.min(s.length, n - 1); return a; };   // a "Pascal" fixed-width string — the shape `compressorname` in a visual sample entry wants
  const MX_MATRIX = mxU32s(0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000);   // the identity display matrix, 16.16 / 2.30 fixed point

  // ── AN AVC SAMPLE ENTRY ── built from the encoder's own `description`, which IS an avcC box payload (SPS+PPS). It
  // has to come from the encoder rather than be copied from the recording: the export re-encodes at its own bitrate
  // and, with ?vescale, its own size, so the recording's SPS would describe the wrong stream.
  const veAvcStsd = (w, h, avcC) => rmBox('stsd', mxU32s(0, 1), rmBox('avc1',
    mxU8(0, 0, 0, 0, 0, 0), mxU16(1),                    // reserved[6], data_reference_index = 1 (the self-contained `url ` in dinf)
    mxU16(0), mxU16(0), mxU32s(0, 0, 0),                 // pre_defined, reserved, pre_defined[3]
    mxU16(w), mxU16(h),
    mxU32s(0x00480000, 0x00480000, 0),                   // 72 dpi horizontal and vertical, then reserved
    mxU16(1), mxStr('voxelbit', 32), mxU16(0x18), mxU16(0xffff),   // frame_count = 1, compressorname, depth = 24, pre_defined = -1
    rmBox('avcC', avcC)));

  // tracks: [{ kind:'video'|'audio', timescale, width, height, stsd (a complete stsd BOX), samples:[{ size, dur, cts,
  //            sync }], data:[BlobPart] parallel to samples }]. Returns a Blob: ftyp + moov + mdat, in that order,
  //            which IS faststart, with the tables Resolve needs already populated.
  const veMp4Mux = (tracks, movieTs) => {
    const live = tracks.filter((t) => t.samples.length);
    if (!live.length) return null;
    movieTs = movieTs || 1000;

    // ── CHUNKING ── one chunk per track per second of media. A single chunk per track would be legal and smaller, but
    // it puts each track's data in one contiguous slab, so a player has to seek the whole file's length every second.
    // Interleaving by time is what every other muxer does and costs a few hundred bytes of stco.
    for (const t of live) {
      t.chunks = [];
      let i = 0, clock = 0;
      while (i < t.samples.length) {
        const first = i, at = clock;
        let bytes = 0;
        while (i < t.samples.length && (clock - at) < t.timescale) { bytes += t.samples[i].size; clock += t.samples[i].dur; i++; }
        t.chunks.push({ first, count: i - first, bytes, at, out: 0 });
      }
      t.dur = clock;
    }
    const order = [];                                     // every chunk from every track, in TIME order, so the file interleaves
    for (const t of live) for (const c of t.chunks) order.push({ t, c });
    order.sort((a, b) => (a.c.at / a.t.timescale) - (b.c.at / b.t.timescale));

    let mdatBytes = 0; for (const q of order) mdatBytes += q.c.bytes;
    const wide = mdatBytes + 0x100000 > 0xffffffff;       // >4 GB of media needs a 64-bit mdat and co64 offsets
    const mdatHdr = wide ? 16 : MX_HDR;
    const movDur = Math.max(...live.map((t) => (t.dur / t.timescale) * movieTs));

    const buildMoov = () => {
      const parts = [rmBox('mvhd', mxU8(1, 0, 0, 0), mxU64(mxNow), mxU64(mxNow), mxU32s(movieTs), mxU64(movDur),
                           mxU32s(0x10000), mxU16(0x100), mxU16(0), mxU32s(0, 0), MX_MATRIX, mxU32s(0, 0, 0, 0, 0, 0),
                           mxU32s(live.length + 1))];      // version 1 throughout: a long 4K take can exceed a 32-bit duration, and mixing versions is how the duration field ends up written at the wrong offset
      live.forEach((t, ti) => {
        const id = ti + 1, vid = t.kind === 'video';
        const stts = [], ctts = [], sync = [], stsc = [], offs = [];
        let allSync = true, anyCts = false;
        for (const s of t.samples) { const n = stts.length; if (n && stts[n - 1] === s.dur) stts[n - 2]++; else stts.push(1, s.dur); }
        t.samples.forEach((s, i) => { if (s.sync) sync.push(i + 1); else allSync = false; if (s.cts) anyCts = true; });
        for (const s of t.samples) { const n = ctts.length; if (n && ctts[n - 1] === (s.cts | 0)) ctts[n - 2]++; else ctts.push(1, s.cts | 0); }
        t.chunks.forEach((c, i) => { const n = stsc.length; if (n && stsc[n - 2] === c.count) return; stsc.push(i + 1, c.count, 1); });
        for (const c of t.chunks) { if (wide) offs.push(Math.floor(c.out / 0x100000000), c.out >>> 0); else offs.push(c.out); }
        const szA = new Uint8Array(12 + t.samples.length * 4), szD = new DataView(szA.buffer);
        szD.setUint32(8, t.samples.length);
        t.samples.forEach((s, i) => szD.setUint32(12 + i * 4, s.size));
        const stbl = [t.stsd, rmTable('stts', 0, stts, 2), rmTable('stsc', 0, stsc, 3), rmBox('stsz', szA),
                      rmTable(wide ? 'co64' : 'stco', 0, offs, wide ? 2 : 1)];
        if (!allSync) stbl.push(rmTable('stss', 0, sync, 1));
        if (anyCts) stbl.push(rmTable('ctts', ctts.some((v, i) => i % 2 === 1 && v < 0) ? 1 : 0, ctts, 2));
        parts.push(rmBox('trak',
          rmBox('tkhd', mxU8(1, 0, 0, 3), mxU64(mxNow), mxU64(mxNow), mxU32s(id, 0), mxU64((t.dur / t.timescale) * movieTs),
                mxU32s(0, 0), mxU16(0), mxU16(vid ? 0 : 1), mxU16(vid ? 0 : 0x100), mxU16(0), MX_MATRIX,
                mxU32s(vid ? (t.width << 16) : 0, vid ? (t.height << 16) : 0)),   // tkhd flags 3 = enabled | in-movie; width/height here are 16.16 DISPLAY size, which is what an NLE reads for the timeline resolution
          rmBox('mdia',
            rmBox('mdhd', mxU8(1, 0, 0, 0), mxU64(mxNow), mxU64(mxNow), mxU32s(t.timescale), mxU64(t.dur), mxU16(0x55c4), mxU16(0)),   // 0x55c4 = 'und' packed as three 5-bit letters
            rmBox('hdlr', mxU32s(0, 0), mxU8(...(vid ? [0x76, 0x69, 0x64, 0x65] : [0x73, 0x6f, 0x75, 0x6e])), mxU32s(0, 0, 0), mxU8(0)),
            rmBox('minf',
              vid ? rmBox('vmhd', mxU8(0, 0, 0, 1), mxU16(0), mxU32s(0), mxU16(0)) : rmBox('smhd', mxU32s(0), mxU16(0), mxU16(0)),
              rmBox('dinf', rmBox('dref', mxU32s(0, 1), rmBox('url ', mxU32s(1)))),   // flag 1 on the url = the media is in THIS file
              rmBox('stbl', ...stbl)))));
      });
      return rmBox('moov', ...parts);
    };
    const probe = buildMoov();                            // built once only to learn its length, which is what the chunk offsets below depend on
    const ftyp = rmBox('ftyp', mxU8(0x69, 0x73, 0x6f, 0x6d), mxU32s(512), mxU8(0x69, 0x73, 0x6f, 0x6d, 0x69, 0x73, 0x6f, 0x32, 0x61, 0x76, 0x63, 0x31, 0x6d, 0x70, 0x34, 0x31));
    let cursor = ftyp.length + probe.length + mdatHdr;
    for (const q of order) { q.c.out = cursor; cursor += q.c.bytes; }
    const moov = buildMoov();
    if (moov.length !== probe.length) return null;        // the two passes must agree or every offset just written is wrong

    const mhdr = new Uint8Array(mdatHdr), hd = new DataView(mhdr.buffer);
    if (wide) { hd.setUint32(0, 1); hd.setBigUint64(8, BigInt(mdatBytes + 16)); } else hd.setUint32(0, mdatBytes + MX_HDR);
    for (let i = 0; i < 4; i++) mhdr[4 + i] = 'mdat'.charCodeAt(i);
    const out = [ftyp, moov, mhdr];
    for (const q of order) for (let i = 0; i < q.c.count; i++) out.push(q.t.data[q.c.first + i]);   // Blob parts, so an audio sample can ride through as a slice of the recording and never enter JS memory
    return new Blob(out, { type: 'video/mp4' });
  };
