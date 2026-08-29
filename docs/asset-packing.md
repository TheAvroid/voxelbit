# Asset packing — protecting the asset tree, and the boot win that pays for it

**Status:** plan. Nothing is implemented. Written 2026-08-28, from a session that started with the
question *"can the game still run in the browser but have all the files protected in a back end
database?"* and ended somewhere else.

**Scope:** how `game/assets` is delivered — the pack format, the loader change, the build change.
**Out of scope:** the audio tree (see §7), engine/source protection (see §1), and anything requiring
player accounts.

---

## 0. Decisions taken

Settled in discussion on 2026-08-28.

| decision | reason |
|---|---|
| **No backend database, no auth-gated assets.** | Needs accounts, sessions, hosting and an outage surface, and forces an audio rewrite. Not cheap, and the ceiling is the same (§1). |
| **A build-time pack file instead.** One opaque blob + an offset manifest, decoded in memory. | No server, no accounts, offline still works. Gets most of the practical benefit. |
| **Compress the pack.** | 3.6x smaller (§3), and it removes the plaintext `VOX ` magic bytes that would otherwise let anyone carve the pack apart (§4). |
| **Eager pack for the boot set, range-requested tail for the rest.** | A single 29 MB download at boot would be *worse* than today's lazy fetching. |
| **Audio stays as plain files.** | `new Audio()` cannot carry auth, and routing it through a blob URL trades streaming for buffering. Not worth it (§7). |

---

## 1. The ceiling, stated once

Anything the browser executes or renders arrives at the browser in usable form. A database, session
tokens and encryption all end in the same place: the bytes are on the client and devtools can dump
them. There is no arrangement of a backend that changes this. Real protection means the code runs
somewhere the player cannot reach — pixel streaming, cancelled 2026-08-02.

So the axis is not *protected / unprotected*, it is *how much effort extraction costs*. Today it costs
zero: every asset sits at a guessable same-origin URL.

**And note where the value actually is.** `game/index.html` is 3.6 MB of unminified, comment-rich JS
and WGSL — the worldgen, the physics, the denoiser, with the reasoning written out in the comments.
Anyone who wants the engine reads that file; they do not need the rock models. Packing the assets is
worth doing for §3, but it is not the thing standing between this project and a copycat. Minification
is untouched low-hanging fruit; WASM for the hot logic is the only step that meaningfully protects
the engine, and it is a large job.

---

## 2. Why this is cheap *here*

Three pieces already exist:

- **`tools/bundle.py` already walks `game/assets`.** It regenerates `src/assets/vox-index.js` from
  exactly that walk on every build (see `vox_index()` / `refresh_vox_index()`). Emitting
  `assets.pak` + offsets alongside it extends existing machinery.
- **The loaders already funnel.** ~18 `fetch()` sites in `src/`, most going through `fetchBytes()` /
  `pf()` helpers. Repointing them at a pack lookup is a small diff.
- **The encode step has precedent.** 21 `.b64.js` files in the asset tree already base64 their `.vox`
  frames. They are deliberately unused, but for an unrelated reason — the comment at
  `src/assets/held-items.js:346` rejects them as a `file://` workaround and "a second loading
  mechanism for one asset." That objection dissolves when the pack *replaces* the fetch path instead
  of sitting beside it.

---

## 3. Measured, 2026-08-28

The numbers that actually justify the work.

| fact | value |
|---|---|
| `game/assets` | 1043 files, 29 MB — 992 `.vox`, 27 `.js`, 13 `.json`, 9 `.pxo`, 2 `.png` |
| `game/sound` | 46 files, 32 MB — 36 `.mp4`, 9 `.mp3` |
| whole asset tree, tar + gzip -9 | **8.03 MB** (3.6x) |
| `assets/decoration/desert_rocks.json` | 5,535,021 -> 1,378,516 (24%) |
| `assets/decoration/rocks26.json` | 3,227,971 -> 757,323 (23%) |
| `assets/foilage/oak_trees/oak_7.vox` | 346,556 -> 145,760 (42%) |
| compression configured in `game/.htaccess` | **none** |
| `game/index.html` | 3.63 MB, unminified |

**Read that third-from-last row again.** All 29 MB ships uncompressed today. Enabling transfer
compression is a one-line server change and is worth doing *whether or not the pack is ever built* —
it is the single cheapest boot win on the table and it is independent of everything else here.

Two further boot effects the pack brings:

- **Request count collapses.** Hundreds of small `.vox` fetches become one blob plus a lazy tail.
- **The 404-probe walks die.** Strip loaders currently discover length by fetching until one 404s.
  The comment at `src/assets/held-items.js:93` records bass/blue_gill/catfish running "~39 fetches
  strictly one-at-a-time" for this reason. A manifest knows the counts up front. (The 17 boot 404s
  the test gate calls normal are this same behaviour.)

Against a boot at ~11.7 s versus the 7 s target, this — not the protection — is the argument.

---

## 4. Format sketch

```
assets.pak    [magic][u32 count][entry table][deflated payload...]
              entry := u32 nameHash, u32 offset, u32 rawLen, u32 packedLen
```

- **Content-addressed.** Entries key on a hash of the logical name, not the path. Filenames stop
  existing in the shipped artefact.
- **Deflate per entry**, not whole-file, so a single asset can be pulled from the tail by range
  request without inflating everything before it.
- **XOR the stream** with a build-time constant on top of the compression. This is obfuscation, not
  cryptography — say so in the code so nobody later mistakes it for security.

**The carving trap.** A raw concatenation defeats itself. The base64 in `eat.b64.js` opens `Vk9YIM`,
which decodes to `VOX ` — MagicaVoxel's magic bytes in plaintext. Anyone can split a naive pack on
`VOX ` boundaries and recover all 992 models in an afternoon. Compression removes the boundaries
incidentally; the XOR makes reconstructing them tedious.

What this buys, honestly: no `wget assets/foilage/pine5.vox`, no browsable directory of 992 named
models, one opaque entry in the Network tab. Extraction moves from *trivial* to *a project*. Someone
determined still dumps the decoded buffers from devtools.

---

## 5. Migration

1. **`tools/bundle.py`** — reuse the existing `game/assets` walk. Emit `game/assets.pak` and a
   generated manifest fragment next to `src/assets/vox-index.js`. Split eager vs tail using the
   prefetch lists in `src/assets/held-items.js` as the boot set.
2. **One loader shim** — `loadAsset(name)` resolving from the in-memory pack, falling back to a range
   request into the tail pak. Everything else keeps its current call shape.
3. **Repoint the ~18 fetch sites.** Most are already behind `fetchBytes()` / `pf()`.
4. **Delete the 404-probe walks.** Frame counts come from the manifest now.
5. **Leave the 21 `.b64.js` files alone.** Still unused, still the authored fallback.

---

## 6. Gotchas

- **The strip loaders infer length from 404s.** Anything still walking-until-missing must be converted
  in the same change, or it will walk off the end of a manifest that cannot 404.
- **`game/.htaccess` sets `COEP: require-corp`** for SharedArrayBuffer. A same-origin pack file is
  fine — this only becomes a problem if assets ever move to another origin, which is one more reason
  not to. Verify `crossOriginIsolated` is still `true` after the change.
- **The editor and console load assets by path** (`__vb.edLoad('assets/life/frog.vox')`,
  `src/ui/console.js:171`). These are dev paths and should keep reading loose files — do not route
  them through the pack.
- **`src/assets/vox-index.js` is generated and gate-checked.** `bundle.py` prints `STALE` when it does
  not match the tree; the pack needs the same staleness check, or a stale pack ships silently.

---

## 7. What this does not cover

- **Audio.** 32 MB in `sound/`, loaded via `new Audio('sound/...')`. Media elements cannot send an
  auth header, and fetching bytes into a `blob:` URL means buffering whole tracks instead of letting
  the browser stream them. On a load screen already over budget that is a regression. The soundtrack
  stays extractable, and that is the accepted trade.
- **The engine.** See §1. This document protects the replaceable half.
- **A determined attacker.** See §4.

---

## 8. Open

- Eager/tail split point — needs a measurement of what the boot set actually touches, not a guess
  from the prefetch lists.
- Whether transfer compression at the server makes per-entry deflate redundant for the eager blob
  (it does not for the range-requested tail).
- Whether `.pxo` and the two `.png` belong in the pack at all.
