# Voxelbit — Discord integration

**Status:** plan. Written 2026-08-22.

**What exists today:** one button. [`src/html/10-body.html:32`](../src/html/10-body.html#L32) is an
`<a id="dcBtn">` pointing at `discord.gg/AtW5fWZtSG`. The server exists; nothing programmatic does.

**Verification note:** Discord's platform moves quickly and my information runs to May 2026. Product
names, SDK availability and limits below should be checked against `discord.com/developers/docs`
before anything depends on them. Where I am unsure, this document says so rather than guessing.

Companion documents: [arcade-financial-layer.md](arcade-financial-layer.md) (§2 here is a direct
dependency of its Phase 1), [arcade.md](arcade.md), [v2-engine-rewrite.md](v2-engine-rewrite.md).

---

## 0. "Discord integration" is six different projects

They get conflated constantly, and they differ by two orders of magnitude in cost. Naming them
separately is most of the value of this document.

| # | thing | what it actually is |
|---|---|---|
| 1 | **community server** | channels, roles, moderation. Already exists. Zero engineering. |
| 2 | **Sign in with Discord** | OAuth2 identity provider. **The one that matters.** |
| 3 | **clip sharing** | posting an exported highlight to a channel |
| 4 | **linked roles** | Discord roles gated on verified in-game achievements |
| 5 | **a bot** | slash commands, leaderboards, match results |
| 6 | **Discord Activity** | the game running *inside* Discord. A distribution bet, and the only speculative item here. |

Ranked by value per unit of work:

| rank | item | cost | why |
|---|---|---|---|
| **1** | Sign in with Discord (§2) | small | unblocks the financial layer's Phase 1 and is the identity your audience already has |
| **2** | clip sharing (§3) | small | the export pipeline already exists; this is the cheapest growth loop available |
| **3** | linked roles (§4) | small | makes achievements socially visible, which is what makes people chase them |
| **4** | a bot (§5) | medium | useful, but nothing else depends on it |
| **5** | Activity (§6) | large + uncertain | gated on a hard technical question — spike before planning |
| — | Rich Presence | **impossible from a browser** (§7) | see below before anyone promises it |

---

## 1. The one thing to decide first

**Discord OAuth is an excellent login. It is a dangerous *sole* identity.**

The financial layer attaches real money value to an account. If Discord is the only way in, then a
banned, deleted, or compromised Discord account takes a player's bit balance with it — and Voxelbit
has no way to verify who they were. That is a support catastrophe and a trust problem, and it arrives
exactly when the balance is large enough to matter.

**Rule: Discord is a linked identity, never the only one.** Every account carries a recoverable
fallback — an email address at minimum — established at or shortly after signup. Players may sign in
with Discord forever; the account does not *depend* on it.

This costs almost nothing if designed in from the start and is close to unfixable afterward. It is the
single most important line in this document.

---

## 2. Sign in with Discord

**This is Phase 1 of [arcade-financial-layer.md](arcade-financial-layer.md), delivered cheaply.** That
plan needs identity before a ledger can exist, and Discord OAuth is the lowest-friction credible
option for a gaming audience — no password, no email verification round trip, and the player is
already logged in.

Shape:

```
  game ──► /auth/discord ──► Discord consent ──► callback with code
                                                     │
                          your server exchanges code for a token,
                          reads the Discord user id, then:
                            • existing link → issue session
                            • new           → create account, link, prompt for recovery email
```

- Scopes: `identify` at minimum, `email` if you want the fallback for free. **Request nothing more** —
  scope creep on a consent screen costs conversion, and `guilds`/`guilds.join` should wait until §4
  or §5 actually needs them.
- The **Discord user id is the join key**, not the username — usernames change.
- Store the OAuth tokens server-side only, encrypted, and treat a refresh failure as "re-link needed",
  never as "account lost".
- The game is a readable HTML file, so the OAuth exchange happens entirely server-side. The client
  receives a session token and nothing else.

**Order it against the financial plan:** build this as Phase 1 there, with the achievement migration
(`vb_ach` out of `localStorage`) as the zero-risk first payload. It exercises the whole account system
before it holds a cent.

---

## 3. Clip sharing

The cheapest growth loop available, because **the hard part is already built**: `src/ui/video-editor.js`
plus `mp4-mux.js` / `mp4-remux.js` already export annotated clips, and
`memory/voxelbit-clip-compress.md` records the working `-c copy -movflags +faststart` remux recipe.

Two implementations, and they are very different:

| | **webhook to a channel** | **share to the player's own server** |
|---|---|---|
| how | your server posts to a channel webhook | OAuth with a share scope, or simply hand the player a file and a link |
| cost | trivial | moderate |
| reach | your community only | wherever that player already talks |
| moderation | you control the channel | you do not |

**Start with the first** — a `#highlights` channel your server posts to, with the player opting in per
clip. It is a day of work and it makes the community visible to itself.

Two constraints to design around: Discord enforces an upload size limit that varies by server boost
tier, so the compression recipe matters and a fallback link is needed for large clips; and **every
clip posted is a moderation surface**. Opt-in per clip, a report path, and the ability to delete
retroactively are not optional at any real volume.

---

## 4. Linked roles

Discord's role-connection metadata lets an app publish verified numeric or boolean facts about a user,
which server admins then use as role requirements. It is a small, well-defined API and a good fit for
achievements that already exist (`vb_ach`).

Examples that would work: *trees felled*, *distance travelled*, *achievements unlocked*, *biomes
visited*, and — once the Arcade exists — *matches played* or *rank*.

Why it is worth more than it looks: an achievement nobody can see is a private number. A role that
appears next to someone's name in a channel is the same number made social, and that is what makes
people pursue it. Cheap, and it makes §2's login worth having on day one rather than only once bits
exist.

Depends on: §2 (an account to attach facts to), plus achievements living server-side rather than in
`localStorage`.

---

## 5. The bot

Useful, and **nothing else depends on it** — which is why it sits fourth. Sensible surface, in order:

- `/stats <player>` — public profile, achievements, playtime.
- `/leaderboard` — needs server-side stats, so it follows §2.
- **Match results posted automatically** once the Arcade exists — the single highest-value bot
  feature, because it turns a channel into a scoreboard that updates itself.
- `/locate`-style world queries are tempting given the in-game command already exists, but they need
  the shared-world server tier and should wait for it.

Keep the bot stateless against your own API. A bot that owns data is a second source of truth, and
[arcade-financial-layer.md](arcade-financial-layer.md) §1 is emphatic about why that ends badly.

---

## 6. Discord Activities — the distribution bet

Discord's Embedded App SDK runs a web app inside Discord itself, in a voice channel, with friends
already present. On paper Voxelbit is an ideal candidate: it *is* a web app, and the hardest part of
any Activity — having something worth doing together — is already solved.

**In practice there is one question that decides the whole thing, and it must be spiked before any
planning.**

### The spike, and it is a gate

> Does WebGPU work inside the Discord client's embedded browser, on a normal player's machine, at a
> playable frame rate?

Discord's desktop client is Chromium-based, so WebGPU is *plausible* — but embedded contexts routinely
differ from the browser in exactly the ways that matter: GPU access policy, available adapter limits,
and process sandboxing. If `navigator.gpu` is absent or the adapter reports reduced limits, everything
below is moot. **This is a half-day experiment and it should happen before anyone estimates the rest.**

### Even if the spike passes, the Sandbox does not fit

Independent of WebGPU, the numbers are against it:

| constraint | voxelbit today |
|---|---|
| assets | 23 MB models + 32 MB sound = **55 MB** |
| world buffer | ~1.5 GB, with a JS heap of 1.5–1.7 GB (`memory/voxelbit-v1-plan.md`) |
| boot | ~10 s today, targeting 1 s ([v2 §9](v2-engine-rewrite.md)) |

That is a heavy load for a tab, and heavier inside another application's renderer process. **The honest
conclusion: the full Sandbox is not an Activity.** What *could* be is a purpose-built small Arcade
experience — a compact map, a trimmed asset set, a short match — using the same engine at a fraction of
the footprint.

Which reframes the item usefully: **an Activity is not a port, it is a product decision** — build a
small game for Discord, and get distribution to an audience already sitting in a voice channel with
their friends. Worth doing eventually. Not worth planning until the spike returns.

Also note Activities proxy outbound network requests through Discord's URL mapping, which the financial
layer's API calls would have to respect. Minor, but it is a real constraint on §2 inside an Activity.

---

## 7. What does not work: Rich Presence

**Rich Presence cannot be done from a browser game, and it is the thing people ask for first.**

Presence is published over a local IPC channel to the Discord desktop client — a named pipe on Windows.
A page in a browser sandbox cannot open one, and no amount of API work changes that. The only routes
are shipping a desktop wrapper (Electron/Tauri) purely to relay presence, or running as an Activity
(§6), where Discord already knows what the player is doing.

Discord's native SDK story has also changed more than once — the old Game SDK was superseded — so
**verify current SDK guidance before committing to any desktop-wrapper plan.** The browser limitation,
though, is architectural and will not have changed.

Write this down where product people can see it, because "just add Discord Rich Presence" reads like a
one-day task and is not one.

---

## 8. Phase order

```
  A  Sign in with Discord          ── = financial layer Phase 1; unblocks everything
     └─ achievements move server-side (zero-risk first payload)
  B  clip sharing to #highlights   ── independent of A; ship whenever
  C  linked roles                  ── needs A
  D  bot: /stats, /leaderboard     ── needs A
  E  WebGPU-in-Discord spike       ── half a day, any time, gates F
  F  a small Arcade Activity       ── only if E passes; a product decision, not a port
  —  Rich Presence                 ── not possible from the browser (§7)
```

**A and B can both start now.** Neither depends on the engine rewrite, the shared-world server tier, or
the networking decision the v2 plan still owes. E is half a day and should be run early simply because
its answer is load-bearing and currently unknown.

---

## 9. Open decisions

1. **Is Discord the primary login, or one of several?** §1 says it must never be the only one. Confirm,
   because the recovery design has to exist before the first account is created.
2. **Do you want Discord accounts and Voxelbit accounts to be 1:1?** Alt accounts are trivial otherwise,
   which matters if roles, leaderboards or referral credit ever have value.
3. **Clip sharing: opt-in per clip, or a connected-account toggle?** Opt-in per clip is the safe
   default and the recommendation; a persistent toggle is a moderation liability.
4. **Who moderates `#highlights`?** It needs an owner before it exists, not after.
5. **Age gating.** Discord is 13+, and the Arcade takes real money. That interacts with the financial
   layer regardless of Discord, but Discord login is where you would first learn a user's age bracket.
6. **Does the Activity bet get made at all?** Blocked on the §6 spike, and it is a product decision
   about building a second, smaller game — not a porting task.

---

## 10. One-paragraph summary

Discord integration is six unrelated projects wearing one name, and only one of them is urgent: **Sign
in with Discord is the cheapest credible way to deliver Phase 1 of the financial layer**, since a
ledger needs accounts and this audience already has an identity. The hard rule around it is that
Discord must be a *linked* identity and never the only one — a deleted or banned Discord account
cannot be allowed to take a player's bit balance with it, and that is designed in at the start or
never. After that, clip sharing is nearly free because the export pipeline already exists and is the
best growth loop available; linked roles make achievements socially visible; a bot is useful but
blocks nothing. Running Voxelbit *inside* Discord as an Activity is the one speculative item, gated on
a half-day spike into whether WebGPU works in Discord's embedded client at all — and even if it does,
the full Sandbox at 55 MB of assets and a 1.5 GB world buffer does not fit, so the real question is
whether a small purpose-built Arcade game is worth making for that audience. Rich Presence, which is
what everyone asks for first, is **not possible from a browser game** and should be recorded as such
before it gets promised.
