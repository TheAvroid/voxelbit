# Voxelbit Arcade — concept

**Status:** concept. Nothing is implemented and nothing is decided. Written 2026-08-22 from the
owner's account of prior discussions. **Every number in here is a working figure, not a commitment** —
where two of them disagree, this document says so rather than picking one.

Companion documents: [v2-engine-rewrite.md](v2-engine-rewrite.md) describes how the world is rendered;
this describes what it is *for*. They intersect in exactly one place, and it is a large one — §7.
[arcade-financial-layer.md](arcade-financial-layer.md) is the build plan for the money — accounts,
ledger, purchase, spend, treasury, payouts — and it settles several of the open questions below.

---

## 0. The three layers

The Arcade is the middle rung of a three-step progression:

| layer | what it is | price |
|---|---|---|
| **Voxelbit Sandbox** | free exploration, building, world interaction — what `game/index.html` is today | free |
| **Voxelbit Arcade** | paid, high-quality competitive and social experiences built on the same tech | **bits per play** |
| **Voxelbit Reality** | the long-term evolution toward ultra-high-resolution voxel worlds | undefined |

The Sandbox is not a demo of the Arcade. It is free, permanently, and it is also the **lobby** — the
place a player already is when they decide to spend. That relationship is the most distinctive part of
the whole concept and §9 comes back to it.

---

## 1. Core concept — pay for what you consume

**You do not buy the game. You pay for the experience you play.**

The model is defined as much by what it rejects as by what it proposes: no $70 up front, no
subscription as the primary mechanic, no battle pass, no advertising. A player holds a balance and
spends it on individual sessions, the way a physical arcade works — which is where the name comes from
and, more importantly, where the unit economics come from.

---

## 2. The currency: bits

Three anchors have been discussed. **They are not all mutually consistent, and reconciling them is
the first real decision this document is waiting on.**

| anchor | as stated | what it implies |
|---|---|---|
| **A. The peg** | 1,000,000 bits = 1 Bitcoin | a bit is 100 satoshis — a clean, memorable unit |
| **B. The unit price** | 1 bit ≈ $0.07 | with A, this sets **BTC = $70,000** |
| **C. The session price** | ~$1 per game / session | with the "1 bit per match" example in §3, this sets **1 bit = $1** |

A and B agree exactly — `1,000,000 × $0.07 = $70,000` — so those two are one consistent system.

**C is the odd one out, and it is 14× away.** There is one arithmetic reconciliation and it is worth
stating plainly, because it may well be the actual intent: **1 bit = $1 exactly when 1 BTC =
$1,000,000.** At a million-dollar Bitcoin, all three anchors hold simultaneously and the whole scheme
becomes elegant — a bit is a dollar, a match costs a bit, a million bits is a coin. If that is the
design, it should be written down as the design, because it means the pricing is denominated for a
future BTC price rather than today's.

### 2.1 The purchasing-power problem

This is the owner's own listed open question and it deserves the sharpest possible statement, because
it is a **product** problem before it is a finance problem:

If a bit is pegged to Bitcoin, **the shelf price of every game in the Arcade moves with the Bitcoin
market.** A player buys 100 bits on Monday and comes back a month later to find each match costs twice
what it did, or half. Simultaneously, Voxelbit's revenue per match doubles or halves in dollar terms
while its server costs do not move at all. Nobody in the transaction gets what they wanted from it.

Three exits, and they are genuinely different products:

1. **Peg to Bitcoin.** Maximum thematic coherence, maximum volatility, and the pricing problem above is
   permanent and unfixable — it is the definition of the peg.
2. **Peg to the dollar.** Bits become a stable closed-loop credit: $1 = 1 bit, always, and the Bitcoin
   relationship becomes flavour rather than mechanism. Simplest to operate, simplest to explain, and
   it is what essentially every successful game currency does.
3. **Float, with bits as the unit of account.** Prices are quoted in bits and stay fixed in bits;
   the *dollar* cost of buying bits floats with whatever backs them. The player experiences stable
   prices; the treasury absorbs the volatility.

**Option 2 is the recommendation unless the Bitcoin relationship is load-bearing for a reason not
captured here.** A currency whose entire job is to price a 10-minute match should be boring.

---

## 3. Pay-per-play

Worked examples from the discussions:

| experience | price |
|---|---|
| a quick Call of Duty-style match | ~1 bit |
| a larger Battlefield-style game | ~10 bits |
| alternative framing | ~$1 per game / session |

The **10× spread** between the two examples is the informative part: it says price is expected to track
session length and scale, not to be flat across the catalogue. That is a real design position and it
should survive whatever happens to §2.

### 3.1 The charging unit is unresolved, and each option fails differently

| unit | fails when |
|---|---|
| **per match** | a player who dies in the first 30 seconds pays the same as one who wins a 20-minute round. It quietly rewards long matches and punishes bad luck. |
| **per hour** | the meter is running. Players watch the clock instead of the game, and the worst moment in the product is the one where somebody logs off to save money. |
| **per experience / session** | easiest to understand and hardest to price — one number has to work across a 6-minute deathmatch and a 45-minute battle royale. |

**Lean: per match, with the price set per-experience.** It matches the arcade metaphor the concept is
named after, it is the only one where the player knows the cost before committing, and the §3 examples
are already written in it. The first-30-seconds problem is real and has a known answer — a partial
refund or a free re-entry on an early elimination — which is worth designing deliberately rather than
discovering after launch.

---

## 4. The revenue model — the arithmetic, checked

The target discussed was **$10 billion per year**, from roughly **9 million daily players at ~2 hours
each, spending $1–2 per hour**. That arithmetic holds:

```
  $10,000,000,000 / 365 days          = $27.4 M per day
  $27.4 M / 9,000,000 daily players   = $3.04 per player per day
  $3.04 / 2 hours                     = $1.52 per player-hour     ← inside the $1–2 band
```

Sensitivity, holding 9 M DAU × 2 h:

| spend per player-hour | annual revenue |
|---|---|
| $1.00 | $6.6 B |
| $1.50 | $9.9 B |
| $2.00 | $13.1 B |

So the model is internally consistent, and $1.50/player-hour is almost exactly the $10 B line.

### 4.1 What $1.52 per player-hour means in actual purchases

This is where §2's unresolved bit price stops being academic:

| pricing | sessions needed per player per day | plausible? |
|---|---|---|
| $1 per session | **3 sessions** | yes — that is a normal evening |
| 1 bit per match at $0.07 | **43 matches** | no |

**The $0.07 bit and the $10 B model cannot both be right at one bit per match.** Either a bit is worth
much more than seven cents, or a match costs far more than one bit. This is the same 14× from §2
showing up in the revenue model, which is a good sign that it is one error and not two.

### 4.2 The cost side is unusually good, and it is the strongest argument for the model

Pixel streaming was **cancelled** on 2026-08-02 (`memory/voxelbit-v1-plan.md`) and the game runs on
the player's own GPU in their own browser. That decision was made for other reasons, but it hands the
Arcade its best economic property: **the marginal cost of rendering a play session is approximately
zero.** The old streaming plan needed ~$0.30 per player-hour just to break even on server GPUs; the
Arcade keeps that $0.30 as margin.

The honest offset: everything in §5 is competitive multiplayer, which needs authoritative servers.
That is CPU and bandwidth rather than GPU — an order of magnitude cheaper than pixel streaming, and
still not free. It needs a real per-concurrent-match cost estimate before any of the above is quoted
to anyone.

### 4.3 Scale check

9 M DAU each playing 2 hours a day is **top-tier-platform scale** — the company this describes is one
of the largest in the industry, not a successful game. That does not make it a bad target; it makes it
a target that has to be stated as one. Any comparison to Roblox, Steam or Fortnite in a pitch should
be checked against current published figures rather than taken from this document.

---

## 5. What runs in the Arcade

Genres named so far:

- Call of Duty-style shooters
- Battlefield-style large-scale warfare
- Fortnite-style experiences
- Battle royale
- Hunger Games-style games

**Every one of these is competitive, server-authoritative multiplayer.** Whether or not it was framed
as an engineering decision, it is the largest one in this document — see §7.

The stated intent is that **Voxelbit provides the platform** — the world, the technology, the identity,
the economy, the infrastructure — while many games and creators sit on top of it.

That single sentence contains the biggest unresolved structural question in the concept: **is Voxelbit
a publisher or a platform?**

| | publisher | platform |
|---|---|---|
| content | first-party, few titles, high quality | third-party, many titles, variable quality |
| revenue | keeps ~all of it | takes a percentage (§8) |
| the hard part | building five AAA-scale games | creator tools, moderation, quality control, payouts |
| 9 M DAU comes from | five extraordinary games | thousands of ordinary ones |

Roblox is the second column and reached scale on it. Nothing decides this yet, and almost every other
open question resolves differently depending on the answer.

---

## 6. Creator compensation

Listed by the owner as an open question, and it belongs directly under §5: if third parties build the
games, the revenue split *is* the product. It sets who shows up to build, what they build, and whether
the catalogue reaches the scale §4 needs. No model has been proposed yet.

Worth noting that the pay-per-play structure makes this **easier** than most platforms: revenue is
attributable to a specific session of a specific game, so a per-play split is directly computable with
none of the attribution guesswork a subscription or a storefront has to do.

---

## 7. What the Arcade needs that the engine does not have

**This is the section to read if only one gets read.**

There is **no networking anywhere in the engine plan.** [v2-engine-rewrite.md](v2-engine-rewrite.md)
contains zero references to multiplayer, netcode, servers, or replication; so does `CLAUDE.md`, and so
does the current 29,000-line `src/`. The v2 plan is a single-player local simulation from top to
bottom — the 12-pass frame graph, the worker physics over a `SharedArrayBuffer`, the GPU worldgen with
the CPU reading back — and all of it assumes exactly **one authoritative simulation, running locally**.

Every genre in §5 requires the opposite. Specifically, the Arcade needs:

| system | why it does not exist yet |
|---|---|
| **Server-authoritative simulation** | the client currently *is* the authority for everything |
| **Replication + lag compensation** | no concept of remote state at all |
| **Matchmaking, sessions, lobbies** | no server tier of any kind |
| **Accounts and identity** | achievements persist in `localStorage` (`vb_ach`) |
| **Anti-cheat** | see below — this one is structurally hard here |
| **Payments, wallet, ledger** | real money in and out, in a BTC-denominated unit |

**Anti-cheat deserves its own paragraph.** Voxelbit ships as a single self-contained HTML file the
player double-clicks. The client is fully readable and modifiable **by design** — that is a property of
the distribution model and a good one for a sandbox. For a paid competitive shooter it means the server
must own every outcome that matters, with the client demoted to input and presentation. That is a
from-scratch architecture, not a hardening pass.

**And the specific collision is physics.** §6 of the engine plan moves physics into a worker with
persistent connectivity labels — a design for one local simulation. Replicating destructible voxel
terrain across clients is one of the genuinely hard problems in networked games, and voxelbit's
signature moment, a 100-foot tree collapsing into a pile of chunks, is precisely the case that is most
expensive to replicate. A Battlefield-style game on this engine *is* the destruction — it cannot be
quietly scoped out.

**None of this is an argument against the Arcade.** It is an argument that the Arcade is a **second
engineering program of comparable size to the renderer rewrite**, and that the engine plan should say
so out loud. Two ways to do that, and either is fine as long as it is chosen:

1. Add a networking pillar to the v2 plan, and accept that it changes physics and simulation decisions
   before those are built rather than after.
2. Record explicitly that **v2 is Sandbox-only** and the Arcade rides on a later engine, so that no v2
   decision is quietly made on the assumption that multiplayer will fit in afterwards.

Option 1 is more work now. Option 2 is a decision to rebuild the simulation layer twice. The one
outcome to avoid is making neither choice and discovering the answer during Phase 5.

---

## 8. The marketplace and the fee

A marketplace / exchange with user-to-user transactions has been discussed, with **10% on
transactions** floated as the take rate.

Two things to pin down before that number means anything:

- **10% of what?** Bit purchases, game entry fees, and player-to-player trades are three different
  businesses with three different regulatory shapes and three different sizes. The fee could apply to
  one, two, or all three.
- **Is the $10 B figure gross or net?** 10% of $10 B in transaction volume is **$1 B of revenue**, not
  $10 B. §4's arithmetic is written as revenue Voxelbit keeps; if the Arcade is a platform taking a
  percentage (§5), the gross volume required is ~10× larger and the DAU target moves with it.

Also worth naming once, factually: a system where players buy a Bitcoin-denominated unit, spend it,
trade it with each other, and potentially cash it out is a **money-handling system**. That carries
compliance obligations which vary by jurisdiction and platform, and app-store policies in particular
have specific rules about in-app currencies with real-world value. It is a server program with a legal
surface, not a graphics problem, and it needs its own budget line.

---

## 9. The strongest version of the idea

> A universal game arcade: players enter a free persistent world, then spend bits to step into
> individual experiences — paying for what they actually play rather than buying every game
> separately.

The nearest comparisons are **Steam + Roblox + Fortnite Creative + a physical arcade**, unified by one
currency and one economy.

What is genuinely differentiated here is **not** the currency and **not** the creator platform — both
exist elsewhere and are done well. It is the combination of two things nobody has put together:

1. **Physical-arcade unit economics applied to AAA-scale experiences.** A quarter in the slot, for a
   game that looks like Battlefield.
2. **A free persistent world that is itself the lobby.** Steam has no world; its lobby is a store page.
   Roblox's lobby is a menu. Voxelbit's is a place the player already wants to be, that already runs,
   and that already looks the way §8 of the engine plan wants it to look. The transition from
   *wandering* to *playing* can be a door you walk through rather than a purchase you make.

If the concept is ever compressed to one sentence for someone else, **that second point is the one to
lead with.** It is the part the Sandbox already proves.

---

## 10. Open questions

The owner's four, restated with what this document adds:

1. **How do bits hold stable purchasing power?** — §2.1. Three named exits; the recommendation is a
   dollar peg unless the Bitcoin relationship is load-bearing for a reason not recorded here.
2. **Per match, per hour, or per experience?** — §3.1. Lean is per match with per-experience pricing,
   plus an explicit answer to the early-elimination case.
3. **How are developers compensated?** — §6. Blocked on §5: publisher or platform.
4. **How much is first-party vs third-party?** — §5. The single most consequential question in the
   document; nearly everything else resolves differently on either side of it.

Added here:

5. **Which of the three currency anchors is real?** — §2. They are 14× apart, and the same 14× breaks
   the revenue model in §4.1. If the intent is a $1,000,000 Bitcoin, say so.
6. **Is the $10 B figure gross transaction volume or net revenue?** — §8. It is a 10× difference in the
   size of the required business.
7. **Does v2 get a networking pillar, or is v2 declared Sandbox-only?** — §7. **This one has a
   deadline**, because it changes physics and simulation decisions that Phase 2 and Phase 5 of the
   engine rewrite are about to make.
8. **What does a concurrent match cost to host?** — §4.2. The zero-marginal-cost story is the model's
   best property and it needs a real number under it before it is quoted.

---

## 11. One-paragraph summary

Voxelbit Arcade is the paid layer above a free Sandbox: players wander a persistent voxel world for
nothing and spend **bits** to enter individual experiences, priced per play the way a physical arcade
is — roughly a bit for a Call of Duty-style match, ten for a Battlefield-style one. The long-term
target discussed is **$10 B a year**, which checks out arithmetically at ~9 M daily players × 2 hours ×
$1.52 per player-hour, and which is top-tier-platform scale rather than successful-game scale. The
economics are helped enormously by the 2026-08-02 decision to run on the player's own hardware — the
marginal cost of a rendered session is near zero, where the abandoned streaming plan needed ~$0.30 per
player-hour just to break even. Three things are genuinely unresolved and two of them are
load-bearing: the currency's three anchors disagree by 14×, and that same 14× breaks the revenue
arithmetic; the publisher-or-platform question decides the creator model, the content strategy and the
DAU math all at once; and **the engine has no networking of any kind**, while every genre named for the
Arcade is server-authoritative multiplayer — which makes the Arcade a second engineering program the
size of the renderer rewrite, and makes "networking pillar, or Sandbox-only?" a decision the v2 plan
needs before Phase 2 rather than after.
