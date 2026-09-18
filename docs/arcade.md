# Voxelbit Arcade

**Status:** concept and build plan in one document. Nothing is implemented and several things are not
decided. **Every number in here is a working figure, not a commitment** — where two of them disagree,
this document says so rather than picking one.

**How to read it.** **Part I** is what the Arcade *is* and what it costs; §7 is the one section to read
if only one gets read, and §10 lists every open question. **Part II** is the build plan for the money —
accounts, ledger, purchase, spend, treasury, payouts — which settles several of Part I's questions and
opens its own in §19. **Part III** is three paragraphs: the concept, the financial layer, and the
amendment that put a **pot** in the middle of both.

**History.** Part I was written 2026-08-22 from the owner's account of prior discussions, and Part II
the same day against it. They lived as `arcade.md` and `arcade-financial-layer.md` until they were
squashed into this file on 2026-09-17. Two decisions have been overtaken since, and each is marked
where it sits rather than quietly edited away: the **dollar peg** (§2.1, dead 2026-08-29) and the
**one-way economy** (§11.1, overtaken by the pot).

[v2-engine-rewrite.md](v2-engine-rewrite.md) describes how the world is rendered; this describes what
it is *for*. They intersect in exactly one place, and it is a large one — §7.

---

# Part I — the concept

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

**As of 2026-09-17 the unit of play is specified, and it is not quite a purchase.** A match is 5v5 over
ten minutes and the entries form a **pot**. That changes what a bit *does* — see §3.2, which is the
first thing in this document that is a specification rather than a discussion, and §3.3, which is the
consequence.

---

## 2. The currency: bits

Three anchors have been discussed. **They are not all mutually consistent, and reconciling them is
the first real decision this document is waiting on.** *(Resolved 2026-08-29 — see the amendment at the
end of §2.1.)*

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

**Option 2 was the recommendation here, and it was overtaken on 2026-08-29.** The owner ruled that a
bit is a **denomination of Bitcoin** — 1 bit = 100 satoshis = one millionth of a BTC — and that this is
a *unit*, not a peg, because there is nothing to choose. **Option 3 is therefore the live design:**
prices are fixed in bits, the dollar cost of a bit floats, and the player carries the volatility.

The three exits above stay on the page as a record of what was weighed and accepted, **not as a live
recommendation**, and the dollar peg should not be re-proposed.
**Part II** was written top-to-bottom on the peg and has not been
reworked; treat every "$1 = 1 bit" in it as stale, and see §11.1.

The consequence for everything below: **a bit is worth whatever a millionth of a Bitcoin is worth
today.** At BTC = $100,000 that is **$0.10**, which is the figure the rest of this document now uses.
Anchor C's $1 bit arrives only at a $1,000,000 Bitcoin, and is a destination rather than a price.

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
discovering after launch. *(§3.4 strikes that last sentence: under a pot there is nothing to refund.)*

### 3.2 The format, as specified

Stated by the owner 2026-09-17. It is the first hard specification the Arcade has:

| | |
|---|---|
| **players** | 10 per match, **5 v 5** |
| **length** | 10 minutes |
| **floor stake** | **1 bit per player** — about **$0.10** at BTC = $100,000 |
| **stake ladder** | 1, 10, 100, 1,000 bits — four tiers, three decades, 10x apart |
| **the pot** | every player's stake, plus anything bystanders add |
| **bystanders** | non-players can pay in, and the pot has no ceiling |

The owner's arithmetic, checked:

```
  60 min / 10 min per match   = 6 matches per hour
  6 matches x 1 bit           = 6 bits  = $0.60 per player per hour
  x 10 players                = 60 bits = $6.00 per hour at one table, at the floor
```

That is exactly right, with one deduction: six matches an hour assumes **zero** time in queue, lobby
and load. At a realistic 12–14 minute cycle it is 4.3–5 matches an hour and the floor table is
**$4.30–5.00 per hour**. The rest of this document uses the clean 6 because it is the ceiling and the
error is 20%.

### 3.3 A stake is not a price, and this is the whole of it

**That $6.00 is the pot, and the pot belongs to the winners. It is not revenue.** This is the single
most important consequence of the change, and every number downstream of it moves.

Under §3's pay-per-play a bit spent is **consumed** — it leaves the player, the house keeps it, and
Part II recognises revenue at that moment. Under a pot a
bit staked is **escrowed and redistributed** — it leaves the loser, arrives at the winner, and the
house keeps only what it takes out on the way through. Three words, and this document should use them
precisely from here on:

| term | what it means here | at the floor, per table-hour |
|---|---|---|
| **handle** | everything wagered | $6.00 |
| **rake** | the house's cut of a pot — **the only revenue** | $0.60 at 10% |
| **deposits** | new money entering, i.e. bits actually bought | whatever players top up |

**The right way to ship this is the tournament buy-in, written as two numbers.** Poker has had the
notation for fifty years: a `10 + 1` buy-in is ten bits into the pot and one bit to the house. The
player sees one price. The ledger sees two movements. The house's income stops depending on the pot
settling correctly, and §4.2's server cost is paid by a line that exists whoever wins. **Do it this way
even if the UI only ever says "1 bit to play"**, because the alternative is a revenue line that has to
be extracted from an escrow account under load.

### 3.4 What the pot settles, and what it breaks

**Settled, and it is worth noticing which way it went:** §3.1's early-elimination problem is *gone*. A
player who dies in the first thirty seconds has not overpaid for a service — they have **lost a bet**,
which is the game working as designed. Open question 2's "partial refund or free re-entry" should be
struck rather than answered.

**Broken, and these are all new:**

| problem | why the pot causes it |
|---|---|
| **abandonment** | quitting a pay-per-play match wastes your own bit. Quitting a 1,000-bit match at 5v4 costs **four teammates** their stakes. The team structure makes one player's tilt a financial event for nine other people. |
| **collusion and chip-dumping** | two accounts on opposite teams can move value from one to the other at will, by losing on purpose. It is the oldest attack on every pot-based game, and it is invisible inside any single match. |
| **stake-tier liquidity** | a 1-bit player cannot be matched against a 1,000-bit player. The queue fragments by tier x skill x region x mode, and the 1,000-bit tier is empty at launch by definition. **Liquidity, not scale, is the launch risk.** |
| **cheating becomes theft** | §7 already says the client is readable by design. What changes is that a cheat now takes money from nine identifiable people, which is a category of complaint with a legal shape rather than a support shape. |

And one that inverts an earlier decision:
§11 chose a **one-way economy with no player-to-player
trading**, explicitly to stay out of money-transmission territory. **A pot is
player-to-player transfer.** It is mediated by a game rather than by a trade window, which may or may
not matter, but the decision as written no longer describes the product and needs re-taking rather than
re-reading.

### 3.5 Bystanders are two different products, and they are not close

"Bystanders can pay in as well" has two readings. They look alike in the UI and they are a hundred
times apart in everything else, which makes this **the question to answer before any of it is built**:

| | **A. the boost** | **B. the wager** |
|---|---|---|
| what happens | a spectator adds bits to the pot and gets **nothing back** | a spectator backs a side and is **paid if it wins** |
| what it is | a tip to the winner; patronage | bookmaking on an event you are not in |
| the analogue | the crowd putting quarters on the cabinet | a sportsbook |
| regulatory weight | roughly none | a licensed business, jurisdiction by jurisdiction |
| new failure mode | none | **the players can fix the match the spectators are betting on** |
| can it ship in phase 3 | yes | no |

**Reading A is almost certainly what was meant** — "this ever increases the pot" describes money going
*in*, not odds coming *out* — and it is also the better mechanic. It is the physical arcade exactly: the
crowd around the machine, raising the stakes on a game they are not playing. It costs one ledger entry.

**Reading B is a different company.** Its insider problem is not a harder version of anti-cheat, it is
structurally worse: the ten people who decide the outcome are the ten people with the most information
about it, and five of them can agree to lose. Real sportsbooks handle that with licensing,
surveillance, betting limits and integrity agreements, none of which are engineering. If B is ever
wanted it needs, at minimum: a **broadcast delay** on the spectator feed — otherwise anyone with a
lower-latency view is betting on the past — a hard ban on players holding a position in their own
match, cross-account collusion detection, and a legal programme longer than the engine rewrite.

**Lean: ship A, and keep the word "bet" out of the product while doing it.** A spectator who adds to a
pot and gets their name on it and a share of the highlight is a tip jar with excellent theatre. A
spectator who gets paid is a sportsbook. Nothing in between is stable.

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

### 4.4 The revenue model under a pot

§4 assumed every bit spent is a bit earned. §3.3 makes that false, so the **$1.52 per player-hour** that
lands the $10 B line has to come out of the **rake**, not the handle:

| stake | handle per player-hour | rake at 5% | at 10% | at 15% |
|---|---|---|---|---|
| **1 bit (the floor)** | $0.60 | $0.03 | **$0.06** | $0.09 |
| 10 bits | $6.00 | $0.30 | $0.60 | $0.90 |
| 100 bits | $60.00 | $3.00 | $6.00 | $9.00 |
| 1,000 bits | $600.00 | $30.00 | $60.00 | $90.00 |

**The floor stake is 25x short of the model** — $0.06 against $1.52. That is not an argument against it;
a floor is supposed to be cheap. It is an argument that **the $10 B figure is carried entirely by the
upper tiers**, and that the business is now shaped like a poker room rather than an arcade: a small
number of large games paying for a large number of small ones.

What the average stake has to be, holding §4's 9 M DAU x 2 hours:

```
  rake  5%  ->  average stake ~51 bits   ($5.07 per player per match)
  rake 10%  ->  average stake ~25 bits   ($2.53 per player per match)
  rake 15%  ->  average stake ~17 bits   ($1.69 per player per match)
```

**~25 bits at a 10% rake is the line.** On a 1/10/100/1,000 ladder that is reachable — roughly a
quarter of matches at the 100-bit tier, or a handful at 1,000 — but it is an assertion about the
**mix**, and the mix rather than DAU is now the number to forecast.

#### The identity worth keeping

A stake `S` raked at `r` consumes exactly `S x r` per player per match, and that is a pay-per-play
price. **The pot does not create revenue. It decides which player pays it.**

```
  1 bit staked at a 10% rake   = 0.1 bits consumed per player-match
  a pay-per-play match (§3)    = 1.0 bits consumed per player-match
```

**The floor pot is a 90% price cut on §3's stated price, not a price.** Both can be true and probably
should be — the floor exists so the table is never empty — but it should be chosen as a loss leader
rather than arrived at by arithmetic.

#### The $10 B figure now has a third meaning

Open question 6 asked whether $10 B is gross or net. Under a pot there are three answers, and they are
10–20x apart:

| reading | at a 10% rake |
|---|---|
| **handle** — everything wagered | **$100 B** of wagering, to produce... |
| **rake** — gross gaming revenue | **$10 B**, and this is the one that matches §4 |
| **deposits** — new money in | **~$10 B**, and this is the surprising one |

**In a one-way economy the rake rate does not change annual revenue.** Bits cannot leave, so every bit
sold is eventually raked away or sits unspent: net deposits *are* the revenue, and the rake only sets
how many matches a deposited dollar buys before it is gone. A 5% rake is not half the business of a 10%
rake — it is the same business with twice the playtime per dollar, which is a better product at the
same price.

**It is not free, though, and the place it costs is the server.** Twice the playtime per dollar is
twice the match-hours charged against the same deposit. That collapses the entire cost question into
one line:

```
  a match pays for itself  <=>  rake taken per player-match   >   server cost per player-match
                                        ( S x r )                          ( c )
```

The rake **rate** never appears on its own. `S x r` is the whole of it, it is denominated in bits, and
it is the same quantity as a pay-per-play price — which is why §4.4.1 can ask whether the floor match
pays for itself without knowing the rake rate at all.

Both arguments reverse the moment a cash-out exists
(§14, phase 8), and together they are the strongest
economic reason to keep the economy one-way.

### 4.4.1 The floor match may not pay for its own server

This is where §4.2's missing number stops being background and becomes a gate. A floor match is $1.00
of pot spread over 1.67 player-hours:

| rake | revenue per player-hour | against §4.2's $0.02–0.10 server band |
|---|---|---|
| 5% | $0.030 | clears the low end; **loses at the high end** |
| 10% | $0.060 | clears the low end; **loses at the high end** |
| 15% | $0.090 | clears the low end; **loses at the high end** |

**Every rake tested lands inside the cost band.** Nobody can say today whether a 1-bit match makes or
loses money, which promotes open question 8 from background to blocking.

And the two sides move independently. Server cost is billed in dollars; the pot is fixed in bits, and
the dollar value of a bit is a Bitcoin price:

| BTC | 1 bit | floor pot | rake at 10%, per player-hour |
|---|---|---|---|
| $50,000 | $0.050 | $0.50 | $0.030 |
| $78,000 | $0.078 | $0.78 | $0.047 |
| **$100,000** | **$0.100** | **$1.00** | **$0.060** |
| $200,000 | $0.200 | $2.00 | $0.120 |
| $1,000,000 | $1.000 | $10.00 | $0.600 |

**This is §2.1's purchasing-power problem arriving on the cost side.** That the *player* carries the
volatility on the price of a match is the accepted 2026-08-29 position. That the *house* carries it on
whether the cheapest match is profitable is a consequence nobody agreed to, and the fix is small enough
to write down now: **the floor stake is a number of bits that gets re-set when the Bitcoin price moves,
not a constant.** One bit is the floor today because a bit is ten cents today.

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

### 7.1 What the format hands the engine plan

§3.2 is the first thing the Arcade has given the engine that is a **bound** rather than a genre, and it
is a generous one. **Ten players, ten minutes, one arena** is a tractable first netcode target in a way
that "Battlefield-style large-scale warfare" is not: a small fixed player count, a short session with a
defined end, and a bounded world that can be reset between matches. The destructible-terrain
replication problem above does not go away, but it is scoped to an arena that can be authored,
budgeted and snapshotted rather than to an endless world.

**If option 1 is taken, this is the spec to build the networking pillar against** — small enough to be
a first milestone and real enough to be the product.

Two requirements exist only because of the pot, and both are netcode rather than finance:

- **A spectator path.** §3.5's bystanders have to see the match they are paying into. That is
  read-only clients attached to a live match, on a different scaling curve from players: unbounded,
  and a popular 1,000-bit match is the load spike.
- **A settlement authority.** Somebody declares the winner, it has to be the server, and the result
  has to be signed and durable **before** the pot moves. Under pay-per-play the match result was a
  scoreboard; under a pot it is a financial instrument.

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

**And the pot makes that second point better than it was.** A free world that is the lobby is a good
idea. A free world where you can walk up to a match **that has money on it**, watch it, and throw bits
into the pot is the physical arcade rendered completely — the cabinet, the game, and the crowd around
it. §3.5 reading A is what makes the crowd real, and the crowd is the part with no equivalent anywhere
else: Steam has no world, Roblox has no crowd, and a physical arcade has both but cannot run a 5v5
shooter.

If the concept is ever compressed to one sentence for someone else, **that second point is the one to
lead with.** It is the part the Sandbox already proves.

---

## 10. Open questions

The owner's four, restated with what this document adds:

1. **How do bits hold stable purchasing power?** — §2.1. **Answered 2026-08-29: they do not, and that
   is the design.** A bit is 100 satoshis, prices are fixed in bits, and the player carries the dollar
   volatility. §4.4.1 records the one place that answer leaks onto the house instead.
2. **Per match, per hour, or per experience?** — §3.1. **Settled by §3.2: per match, ten minutes, with
   a stake.** The early-elimination half of the question is **struck** — under a pot, dying early is
   losing a bet, not overpaying for a service (§3.4).
3. **How are developers compensated?** — §6. Blocked on §5: publisher or platform.
4. **How much is first-party vs third-party?** — §5. The single most consequential question in the
   document; nearly everything else resolves differently on either side of it.

Added here:

5. **Which of the three currency anchors is real?** — §2. **Settled 2026-08-29: A and B.** A bit is
   100 satoshis and floats in dollars; anchor C's $1 bit is a $1,000,000 Bitcoin, and is a destination
   rather than a price.
6. **Is the $10 B figure gross transaction volume or net revenue?** — §8. **Now a three-way question,
   not a two-way one** (§4.4): handle, rake, or deposits. At a 10% rake those are $100 B, $10 B and
   ~$10 B, and only the last two describe money Voxelbit keeps.
7. **Does v2 get a networking pillar, or is v2 declared Sandbox-only?** — §7. **This one has a
   deadline**, because it changes physics and simulation decisions that Phase 2 and Phase 5 of the
   engine rewrite are about to make.
8. **What does a concurrent match cost to host?** — §4.2. **Promoted to blocking by §4.4.1**, which
   shows every plausible rake landing inside the estimated cost band: until this number exists, nobody
   can say whether a floor match makes or loses money.

From the pot (2026-09-17):

9. **Is a bystander a patron or a bettor?** — §3.5. One is a single ledger entry that ships in phase 3;
   the other is a licensed sportsbook with a match-fixing problem built into its structure. **They
   share a button and nothing else, and this is the largest fork in either document.**
10. **What is the rake, and is it taken as a buy-in fee or as a cut of the settled pot?** — §3.3,
    §4.4. Lean is the two-part buy-in (`10 + 1`), because it posts revenue at stake time and leaves
    settlement owing the house nothing.
11. **Does the floor stake float with the Bitcoin price?** — §4.4.1. It has to, or the cheapest match
    flips between profitable and loss-making on a price move nobody at Voxelbit controls.
12. **What happens to a pot when a player abandons, or when the match server dies mid-game?** — §3.4.
    The pay-per-play answer was a refund. There is no refund once the money is contested.
13. **Does the one-way economy survive a pot?** — §3.4. A pot is player-to-player value transfer, which
    is precisely what §11 ruled out by name.
14. **How is chip-dumping detected?** — §3.4. Two accounts on opposite teams move value at will by
    losing on purpose, and no single match shows it.

---

# Part II — the financial layer

**Scope:** everything between a player's card and a bit being spent on a match — accounts, the ledger,
purchase, spend, treasury, disputes, and payouts. **Out of scope:** matchmaking, the game servers
themselves, and the engine ([v2-engine-rewrite.md](v2-engine-rewrite.md)).

## 11. Decisions already taken

These were settled in discussion on 2026-08-22 and this plan assumes them. Each one closes an open
question in §10.

| decision | consequence |
|---|---|
| ~~**Bits are dollar-pegged.** $1 = 1 bit, fixed.~~ **DEAD — see §11.1** | Prices are stable for the player. Closes the §2.1 purchasing-power problem and removes crypto from the payment stack entirely. |
| **The treasury holds Bitcoin.** Settled fiat converts to BTC. | A treasury policy, *not* part of the game economy. The two are separate ledgers and separate decisions. |
| **Fiat in, via Stripe.** No player-facing crypto rail. | Stripe sees ordinary digital-goods sales. |
| **Convert at purchase, batched daily.** | The hedge is created when the liability is created. Batching amortises fixed fees. |
| **Bits are pre-purchased in packs.** | Forced by arithmetic, not preference — see §13. |
| **One-way economy at launch.** No cash-out, ~~no player-to-player trading~~ **— see §11.1** | Keeps the whole thing out of money-transmission territory. The marketplace is a later, separate programme (§14, phase 8). |

**The volatility has to sit somewhere.** A dollar-pegged bit with a BTC treasury puts it on Voxelbit
rather than on a player who bought 100 bits last month. That is the right way round: the company can
size and understand the exposure, and the player cannot. *(Overturned — see §11.1.)*

### 11.1 Two of the rows above are no longer true

**Amended 2026-09-17. The rest of this document has not been reworked: treat every "$1 = 1 bit" below
as stale.**

- **The dollar peg was overturned on 2026-08-29.** A bit is a **denomination of Bitcoin**: 1 bit = 100
  satoshis = one millionth of a BTC. It is a unit, not a peg, and it must not be re-proposed
  (§2.1). The consequences run through everything below — the liability is
  denominated in **satoshis**, the treasury becomes a **reserve** measured by coverage ratio rather
  than a separate policy, purchase needs a **live rate quote** rather than a daily fix, the money path
  is **integer satoshis with no floats anywhere**, and a balance changes only by a ledger entry, never
  by a price move. **Phase 5**'s "with a dollar-pegged bit that lag carries no liability risk" is
  exactly backwards: the ~T+2 Stripe settlement lag is a **real short position**.
- **The one-way economy no longer describes the product.** §3.2 specifies a
  **pot**: ten players stake bits and the winners take them. That is player-to-player value transfer,
  which is the thing this row ruled out by name. "No cash-out" still holds and is doing more work than
  ever (§14, phase 8), but "no player-to-player trading" has been overtaken by the game design, and
  the decision needs re-taking rather than re-reading.

### 11.2 What a pot changes in this document

| area | under pay-per-play | under a pot |
|---|---|---|
| **what a spend is** | a purchase; revenue recognises | an **escrow**; the bits are still owed to somebody |
| **what revenue is** | the bit itself | the **rake**, and nothing else (§4.4) |
| **when revenue recognises** | at spend | at **settlement**, on the rake line only |
| **the worst bug** | charged without a session | **a pot that does not settle** — ten people's money, held, with no owner |
| **fraud shape** | a stolen card buys bits | a stolen card buys bits and then **dumps them to a confederate** by losing on purpose |
| **new ledger objects** | — | a pot account per match, a rake account, a stake entry, a settlement entry |

**One design choice absorbs most of that, and it is §3.3's: take the rake as a
buy-in fee, not as a cut of the settled pot.** A `10 + 1` buy-in posts the house's revenue at stake
time, in the same transaction as the escrow, and leaves settlement as a pure redistribution that owes
the house nothing. It takes revenue recognition off the failure path entirely, which is worth more than
it costs.

---

## 12. The invariants

Written first, because this is the layer where a bug is not a graphics artifact — it is somebody's
money. These belong at the top of the ledger service as comments, in the manner of
[§13 of the engine plan](v2-engine-rewrite.md).

| invariant | symptom if violated |
|---|---|
| **Bits are created only by a settled payment or an explicit admin grant**, and both are ledger rows naming a source | bits appear from nowhere; the economy cannot be audited or reconciled |
| **Never credit on a client callback.** The Stripe webhook is the only grant path | a closed tab loses a purchase; a replayed redirect URL doubles one |
| **Every external write carries an idempotency key; every external event is processed once, keyed on its ID** | webhooks retry by design — double credits are the default failure, not an edge case |
| **The ledger is append-only. Balances are derived** (a cache that can be rebuilt from zero) | a mutable `balance` column is a support queue you cannot reconcile |
| **A debit and the thing it bought commit in one transaction, or the debit reverses** | the player pays and gets nothing; this is the single worst bug in the system |
| **The client never asserts a balance. It displays one** | the game ships as a readable HTML file — anything the client claims is a value an attacker chooses |
| **Sum of all ledger entries is zero** at all times | double-entry is broken; every downstream number is fiction |
| **Bits sold are a liability, not revenue.** Revenue recognises on spend | deferred revenue misstated; the $10 B figure means two different things (§4.4) |
| **Player economy and treasury are separate ledgers** | a Bitcoin price move appears to change how many bits players own |
| **The free Sandbox never touches the ledger** | the free tier acquires a per-session cost and an attack surface it has no reason to have |

From the pot (§3.2), and they carry the same weight as the ten above:

| invariant | symptom if violated |
|---|---|
| **A pot is a real ledger account, and it settles to exactly zero** | ten players' stakes are held *somewhere*; if that somewhere is not an account you can query a balance on, it is a rounding error with a lawyer attached |
| **No pot outlives its match.** Every open pot has a timeout and a written resolution | a crashed match server holding 1,000 bits of escrow is the pay-without-a-session bug, multiplied by ten and made contested |
| **A stake is not revenue. Only the rake line is** | deferred revenue overstated by ~10x, and the $10 B figure acquires a fourth meaning |
| **A match starts when all ten stakes are posted, or it does not start** | a 5v5 that begins 5v4 with nine paid players is a dispute, not a game |
| **The match server is the only settlement authority, and its result is signed and durable before the pot moves** | the winner is whoever reaches the ledger first |
| **A player never holds a position in a match they are playing** | the people who decide the outcome are the people betting on it |

---

## 13. The arithmetic that shapes the design

Stripe's standard online card rate is **2.9% + $0.30**. The fixed component decides the architecture:

| charge | fee | effective rate |
|---|---|---|
| **$1.00 — one match** | $0.33 | **32.9%** |
| $10 pack | $0.59 | 5.9% |
| $20 pack | $0.88 | 4.4% |
| $50 pack | $1.75 | 3.5% |
| $100 pack | $3.20 | 3.2% |

**Per-match card charges are impossible.** Bits must be bought in packs and spent from a held balance —
which is what a physical arcade does with tokens, and for the identical reason. Assume **~4.4% to
Stripe** at a $20 median pack, plus **~0.5%** exchange fee and spread, for **~5% all-in** from card to
Bitcoin in cold storage. On the $10 B target that is ~$440 M/year to payments; it belongs in the model
beside the ~$0.02–0.10 per player-hour of server cost.

### 13.1 Purchases are rare; spends are constant

The two halves of the system have opposite requirements and should not share a design:

| | rate at 9 M DAU | latency budget | requirement |
|---|---|---|---|
| **purchase** | ~7/sec avg, ~50/sec peak | 500 ms is fine | bulletproof, auditable, slow is acceptable |
| **spend** | ~310/sec avg, ~1,500/sec peak | must feel instant | fast, idempotent, reversible |

Neither number is large. **Do not overbuild this.** The hot path is 1,500 ledger appends per second at
full planetary scale, which is an ordinary database.

---

## 14. Phase order

A dependency order, not a schedule. The important property is that **phases 1–3 contain no real
money** — the entire economy is built and exercised on granted bits before Stripe is connected.

```
  0  entity + Stripe underwriting     ── calendar time, starts first, blocks 4
  1  identity                         ── blocks everything
  2  ledger                           ── blocks 3, 4
  3  spend                            ── the economy, fully testable on granted bits
  4  purchase (Stripe)                ── first real money
  5  treasury (BTC)                   ── follows first settlement
  6  disputes + support               ── must exist before scale, not before launch
  7  creator payouts                  ── only if "platform" (§5)
  8  marketplace                      ── deferred; needs legal, not engineering
```

### Phase 0 — entity and underwriting *(start immediately; it is calendar time, not work)*

The long pole here is not code. A business entity, a bank account, and Stripe's underwriting of a
game-currency platform all take real-world time, and underwriting is where a surprise stops the
project cold.

- Register the entity; open the bank account.
- Apply to Stripe and **disclose the full model up front** — one-way virtual currency now, a
  creator-payout platform later, a possible marketplace after that. Surprising your payment processor
  in year two is how accounts get frozen at the worst moment.
- Get written confirmation of what is and is not permitted, and keep it.
- Legal review can be deferred while the economy is one-way, but scope it now so that phase 8 is
  not a surprise.

**Gate:** a live Stripe account in test mode, and a written answer on the marketplace question.

### Phase 1 — identity

Nothing can hold a balance without an account. Today achievements live in `localStorage` under
`vb_ach`, which is the entire persistence story.

- Accounts, authentication (passkeys or OAuth preferred over passwords), sessions, recovery.
  **Sign in with Discord is the cheapest way to deliver this** — see
  [discord-integration.md](discord-integration.md) §2. Its §1 rule is binding here: Discord must be a
  *linked* identity with a recoverable fallback, never the only way into an account that holds money.
- **Migrate `vb_ach` to the account first.** Achievements are a zero-risk payload with real user value
  — the ideal way to shake out the account system before it holds money.
- Account recovery is a financial control once balances exist. Design it now, with the assumption that
  it will be the primary target for social engineering.

**Gate:** a player can log in on two devices and see the same achievements.

### Phase 2 — the ledger

Double-entry, append-only, and the source of truth for every bit in existence.

- Schema: `entries(id, account_id, counter_account, amount_bits, currency, kind, external_id, created_at, metadata)`.
- **Every bit has a counterparty.** A purchase credits the player and debits a `bits_issued` account;
  a spend debits the player and credits `bits_consumed`. The sum across all accounts is always zero,
  and that is a test that runs continuously.
- Balances are a materialised cache, rebuildable from the log by replay. Prove the rebuild works on
  day one, not during an incident.
- **Admin grant path** with an audit row: who granted, how many, why. This is what makes phase 3
  testable without money, and it is also the support tool in phase 6.
- `external_id` unique per source, so a replayed event cannot post twice.

**Gates:** sum-of-entries is zero under concurrent load; balance rebuild from an empty cache matches
the live cache exactly; the same event ID posted twice produces one row.

### Phase 3 — spend

The hot path, and the one place a bug costs a player something they paid for.

- Entering an experience debits its price. **The debit and the match-session grant commit together**,
  or neither does. Anything else means paying for a game that did not start.
- Idempotent on a client-supplied request ID: a double-tapped play button charges once.
- **Reversal path**, with a written policy for each case:
  - match server failed to start → full automatic refund
  - match server crashed mid-game → policy decision, but decide it now
  - early elimination → §3.1 flags this; partial refund or free re-entry
- The spend endpoint is server-authoritative and reachable only from the session service. The client
  requests a match; it does not report having paid for one.

**Gate:** kill the server between debit and session grant, repeatedly, under load; no player ends up
charged without a session, and no player gets a session without being charged.

**At the end of phase 3 the entire Arcade economy runs on granted bits.** Pricing, refunds, balances
and the full player experience can be play-tested with zero financial exposure. This is the highest-
value property of the whole ordering, and it is why Stripe comes fourth rather than first. **It is
worth even more under a pot** — collusion, abandonment and settlement can all be exercised against
granted bits, and they are the cases nobody gets right first time.

#### Phase 3a — stake, pot and settlement

§3.2 turns the spend path into an escrow. The four movements:

```
  stake     player       -> pot:<match_id>     all ten, atomically with the session grant
  fee       player       -> rake               the revenue, posted HERE, not at settlement
  boost     spectator    -> pot:<match_id>     arcade.md 3.5 reading A: no return, no position
  settle    pot:<match>  -> winners            one transaction, and it must zero the pot account
```

- **The fee and the stake are one buy-in to the player and two entries to the ledger.** This is the
  whole of §11.2's recommendation and it is cheap to build on day one and expensive to retrofit.
- **Settlement must leave the pot account at exactly zero**, in a single transaction. A job then
  asserts continuously that no pot account holds a non-zero balance and no pot is older than the
  longest possible match — that is §17's reconciliation applied to the hot path, and it is the test
  that catches everything else.
- **Every abandonment and crash case needs a written policy before this is built**, because unlike a
  refund there is no neutral answer — the money is already contested. At minimum: player quits
  (forfeit, the stake stays in the pot), server dies mid-match (refund all ten out of the pot, the one
  case where a pot returns where it came from), match never starts (refund, and it is the easy one).
- **Leave room for collusion detection**, which is a phase 6 job that phase 3 can make impossible. The
  stake and settle entries need enough metadata to ask *which accounts keep losing to which*, and that
  one query is the entire defence against chip-dumping.

**Gate:** kill the match server at each of the ten stake posts, at settlement, and between them. No pot
is ever left holding bits; no player is ever staked into a match that did not run; no match ever
settles twice.

### Phase 4 — purchase

- Pack SKUs as Stripe Prices. Start with three; a long ladder is a later optimisation.
- **Stripe Checkout** first — hosted, fastest to a working flow, handles SCA and local payment methods.
  Move to Payment Element later if the redirect hurts conversion.
- Webhook handler: verify signature, then in one transaction insert the ledger entry keyed on the
  Stripe event ID and update the cache. Return 200 only after commit.
- Client polls or re-reads balance after returning; **it never grants on the redirect.**
- **Stripe Tax** from day one. Digital goods are taxable across the EU from the first sale, and
  retrofitting tax is far worse than starting with it.
- Radar rules on velocity and geography mismatch; 3DS on larger packs.
- Local payment methods matter at global scale — cards are not dominant everywhere, and SEPA/ACH have
  much better fee shapes on large packs.

**Gates:** replay every webhook type twice and assert one credit; a full test-mode purchase-to-balance
round trip; a deliberately dropped webhook recovered by Stripe's retry with no manual intervention.

### Phase 5 — treasury

- Daily batch: read settled Stripe payouts, buy BTC for the batch total, move to custody.
- **Its own ledger**, reconciled against Stripe payouts and exchange fills independently of the player
  economy. A Bitcoin price move must never be able to change a player's bit balance.
- Batch, do not convert per transaction — fixed network and withdrawal fees behave like Stripe's $0.30.
- Stripe pays out fiat on a rolling schedule (typically ~T+2, longer on a new account), so there is
  unavoidable settlement lag. With a dollar-pegged bit that lag carries no liability risk, which is a
  second reason the peg is the right choice.
- Stripe's stablecoin payout support may remove a banking hop; confirm current availability rather
  than assuming it.
- Accounting: bits sold are **deferred revenue**; BTC is marked to fair value through income under
  current US GAAP. Reported earnings will track the Bitcoin price regardless of how the game performs.
  Decide that with an accountant deliberately.

**Gate:** a daily reconciliation report that ties Stripe settlements, exchange fills and custody
balances to the cent, and that fails loudly rather than silently.

### Phase 6 — disputes, fraud, and support

Underestimated in every project of this kind, and the reason support queues become permanent.

- Chargebacks: instantly-delivered digital goods are a high-dispute category, and stolen-card fraud
  targets game currency specifically. Handle the dispute webhook, claw back bits, and **write the
  negative-balance policy down** — the bits are usually already spent by the time the dispute lands.
- A "bits spent since purchase" signal, so a genuine player is treated differently from a card tester.
- Support console: account lookup, ledger view, refund, manual adjustment — every action an audit row.
- Rate limits on account creation and checkout-session creation.

### Phase 7 — creator payouts *(only if "platform")*

Blocked on the publisher-vs-platform question in §5, which decides whether this
phase exists at all.

- **Stripe Connect, Express accounts** — Stripe carries onboarding, KYC and US 1099 issuance.
- Per-play attribution is the easy part here, and it is a genuine advantage of pay-per-play: revenue
  belongs to a specific session of a specific game, so the split is computed, not estimated.
- Take rate via `application_fee_amount`, or separate charges and transfers if the split is decided
  after the fact.
- Payout schedule, minimum thresholds, and a hold period for dispute exposure.

### Phase 8 — marketplace *(deferred)*

Player-to-player transactions and any cash-out path. **The blocker is legal, not engineering.** A
currency players can trade and convert back to money looks like money transmission, which is a
licensing question in the US state by state and has analogues elsewhere. The entire one-way economy
ships without it, and it should stay out until §11's written answers say otherwise.

---

## 15. Architecture

The game is a single self-contained HTML file the player double-clicks, so **there is no trusted
client and there are no client-side secrets.** Everything below runs server-side.

```
  client ──── HTTPS ────► API           identity, balance read, checkout session create
                           │
                           ├──► ledger service     append-only, the source of truth
                           ├──► session service    match entry; calls spend
                           └──► Stripe

  Stripe ──── webhook ───► webhook handler ──► ledger   (the ONLY credit path)

  daily batch: Stripe payouts ──► exchange ──► custody ──► treasury ledger
```

Notes that follow from the constraints:

- **The balance is read-only to the client.** It is displayed, never asserted, never used to authorise.
- Secret keys live server-side only — an obvious rule that a single-file distributable makes absolute.
- The spend path is callable only by the session service, not by the game client.
- The free Sandbox never calls any of this. Its cost stays at zero and its attack surface stays empty.

---

## 16. What this depends on that does not exist

The financial layer needs a server tier, and §7 records that the engine plan
has none — no networking, no accounts, no backend beyond `tools/serve-nocache.py`, which is a local
static-file dev server.

**Phases 1–4 do not need the game.** Identity, ledger, purchase and a stubbed spend can be built and
tested against a fake session service while the engine work continues in parallel. The first real
coupling is phase 3's atomicity requirement, which needs a real match session to commit against.

This is worth stating plainly because it makes the financial layer **the one part of the Arcade that
can start now** without waiting on the networking decision in the engine plan.

---

## 17. Testing and gates

Ordinary test coverage is not the bar here. The specific things that must be proven:

| test | why |
|---|---|
| sum of all ledger entries = 0, continuously, under concurrent load | the one invariant that catches almost everything |
| balance cache rebuilt from an empty state matches live, exactly | proves the log is really the source of truth |
| every webhook type replayed twice → one credit | webhooks retry by design |
| kill the process between debit and session grant, repeatedly | the worst bug in the system, deliberately provoked |
| double-tapped play button → one charge | the most common real-world duplicate |
| daily reconciliation ties Stripe, exchange and custody to the cent | catches everything the unit tests do not |
| a deliberately dropped webhook recovers unattended | Stripe's retry must be sufficient with no human in the loop |

The reconciliation report is the gate that matters most. **It should fail loudly and daily rather than
be checked when someone remembers.**

---

## 18. Cost model

| line | rate | on $10 B gross |
|---|---|---|
| Stripe, $20 median pack | 4.4% | $440 M |
| exchange fee + spread | ~0.5% | $50 M |
| game servers (§4.2) | ~1–3% | $100–300 M |
| **total cost of revenue** | **~6–8%** | |

For contrast: an app-store-distributed game gives up 15–30% before any of this. Web distribution keeps
that difference, which is most of a business model and an argument for staying on the web deliberately
rather than by default.

**Under a pot this table measures the wrong thing.** Its percentages are of "gross", and gross now
means **handle** (§4.4) — but Stripe's 4.4% is charged on **deposits**, once,
and the server cost is charged per **match**, repeatedly, as the same deposit is re-wagered. At a 10%
rake those are three bases an order of magnitude apart and one percentage column cannot hold them.
Rebuild it **per deposited dollar**: payments take ~5% off the top once, and hosting is then charged
against that dollar roughly `1 / rake` times before it is gone. The condition reduces to the single
line in §4.4 — **the rake per player-match must exceed the server cost per
player-match** — and it is the reason a generous rake is not free, however good it is for the player.

---

## 19. Open decisions

1. **Pack ladder — which SKUs?** Three to start. The floor sets the effective fee rate (§13) and the
   ceiling sets the fraud exposure.
2. **Match-server crash policy.** Full refund, partial, or credit? Decide before phase 3, because the
   reversal path is built around the answer.
3. **Early-elimination policy** (§3.1). **Struck by §3.4** — under a pot there is
   nothing to refund, because dying early is losing a bet rather than overpaying for a service.
4. **Negative balances after a chargeback.** Allowed and collected against, or written off?
5. **Is the $10 B figure gross transaction volume or net revenue?** (§8, and §4.4 for the third
   reading) — a 10× difference in the business being described, and it changes the pack ladder and
   the take rate.
6. **Publisher or platform?** (§5) — decides whether phase 7 exists.
7. **Breakage policy.** Unspent bits are a real line item and some jurisdictions regulate them like
   gift cards, including escheatment. Decide whether bits expire before selling the first one, because
   changing it later is a term change on money already taken.

From the pot (2026-09-17):

8. **Rake: a buy-in fee, or a cut of the settled pot?** (§3.3) — the lean is
   the fee, so revenue posts at stake time and settlement owes the house nothing.
9. **Abandonment, and mid-match server death** (§3.4). Phase 3a cannot be built
   without an answer, and there is no neutral one once the money is contested.
10. **Bystander: patron or bettor?** (§3.5) — one is a ledger entry, the other
    is a licensed business. **This is the largest fork in either document**, and it changes Phase 0's
    underwriting disclosure before it changes anything else: a processor underwrites a virtual-currency
    game and a wagering platform as different categories, and discovering that in year two is how
    accounts get frozen.
11. **Chip-dumping.** A pot is a value-transfer channel; a stolen card that buys a $100 pack and loses
    it on purpose to a second account is the standard attack. No cash-out contains it. The marketplace
    (phase 8) uncontains it, and phase 8's legal blocker is larger with a pot upstream of it than
    it is without one.
12. **Does the floor stake float with the Bitcoin price?** (§4.4.1) — the pot is
    fixed in bits and the servers are billed in dollars, so a price move decides whether the cheapest
    match is profitable.
13. **Age gating, deposit limits and self-exclusion.** Ordinary consumer-protection machinery for
    anything with a stake on it, and it is far cheaper designed into Phase 1's identity than bolted to
    it. Scope it with the Phase 0 legal review rather than after.

---

# Part III — summary

## 20. The concept in one paragraph

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

---

## 21. The financial layer in one paragraph

The financial layer is a dollar-pegged bit balance held in an append-only double-entry ledger, funded
by Stripe pack purchases and drawn down per match, with settled fiat batch-converted to Bitcoin as a
treasury policy that is deliberately kept separate from the player economy. Card economics force the
shape: at 2.9% + $0.30, a $1 per-match charge loses a third of its value to fees, so bits are
pre-purchased in packs at ~4.4% and spent from a held balance — the same reason arcades sell tokens.
The build order puts **no real money in the first three phases**: identity, then the ledger, then
spend, so the entire economy can be exercised on granted bits before Stripe is connected in phase 4,
with treasury, disputes and creator payouts following. The two invariants that matter more than the
rest are that **the Stripe webhook is the only path that creates a bit**, and that **a debit and the
match session it paid for commit together or not at all**. And because phases 1–4 need a server tier
but not the game, this is the one part of the Arcade that can start before the engine plan answers its
networking question.

---

## 22. Amendment, 2026-09-17 — the pot

### What it does to the concept

The unit of play is now specified: **5v5, ten players, ten minutes**, a floor stake of **one bit each**
(~$0.10 at a $100,000 Bitcoin) on a 10x ladder up to 1,000, with bystanders able to pay into the pot.
That changes the model's **shape** more than its size, and the change is one sentence: **a stake is not
a price.** The pot goes to the winners, so revenue is the **rake** and not the handle — the floor
table's $6.00 an hour is $0.60 of income at a 10% cut, and $0.06 per player-hour against the $1.52 that
the $10 B line needs. The floor is 25x short by design, which makes the upper stake tiers carry the
business and the **stake mix**, not DAU, the number to forecast; ~25 bits average at a 10% rake is the
line. Two things are newly load-bearing. **Is a bystander a patron or a bettor** — one is a ledger
entry and the finest version of the arcade-crowd idea in §9, the other is a licensed sportsbook whose
insiders are the ten people deciding the outcome. And **does the floor match pay for its own server** —
at every rake tested it lands inside §4.2's cost band, so the answer is currently unknown and moves
with the Bitcoin price, which is §2.1's volatility problem reappearing on the side that did not agree
to carry it. Two decisions elsewhere are overtaken: the **dollar peg** (dead since 2026-08-29) and the
**one-way economy with no player-to-player transfer**, which is what a pot is. The good news lands on
the engine: ten players, ten minutes and one bounded arena is the first Arcade requirement small enough
to build against, and it comes with two netcode items — a spectator path and a signed settlement
authority — that pay-per-play never needed.

### What it does to the ledger

Two of §11's decisions have been overtaken: the **dollar peg** (dead since 2026-08-29 — a bit is 100
satoshis and floats) and the **one-way economy**, because §3.2's pot is
player-to-player transfer by construction. The spend path becomes an **escrow**: the ledger grows a pot
account per match, revenue moves from the bit to the **rake**, and the worst bug in the system is
promoted from "charged without a session" to **"a pot that does not settle"** — ten people's money,
held, with no owner. Most of the damage is absorbed by one choice, **take the rake as a buy-in fee at
stake time rather than as a cut of the settled pot**, which keeps revenue recognition off the failure
path and leaves settlement as pure redistribution. Three things now sit outside engineering and should
be moved into Phase 0 rather than discovered later: **whether a bystander is a patron or a bettor**
(§19.10), **chip-dumping** (§19.11), and **age gating and deposit limits** (§19.13) — and the first of
those changes what has to be disclosed to the payment processor before a line of it is written.
Everything in §§11–19 above predates all of this.
