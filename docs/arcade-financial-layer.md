# Voxelbit Arcade — the financial layer

**Status:** plan. Nothing is implemented. Written 2026-08-22 against [arcade.md](arcade.md) and the
decisions taken in session on the same day.

**Scope:** everything between a player's card and a bit being spent on a match — accounts, the ledger,
purchase, spend, treasury, disputes, and payouts. **Out of scope:** matchmaking, the game servers
themselves, and the engine ([v2-engine-rewrite.md](v2-engine-rewrite.md)).

---

## 0. Decisions already taken

These were settled in discussion on 2026-08-22 and this plan assumes them. Each one closes an open
question in [arcade.md §10](arcade.md).

| decision | consequence |
|---|---|
| **Bits are dollar-pegged.** $1 = 1 bit, fixed. | Prices are stable for the player. Closes the §2.1 purchasing-power problem and removes crypto from the payment stack entirely. |
| **The treasury holds Bitcoin.** Settled fiat converts to BTC. | A treasury policy, *not* part of the game economy. The two are separate ledgers and separate decisions. |
| **Fiat in, via Stripe.** No player-facing crypto rail. | Stripe sees ordinary digital-goods sales. |
| **Convert at purchase, batched daily.** | The hedge is created when the liability is created. Batching amortises fixed fees. |
| **Bits are pre-purchased in packs.** | Forced by arithmetic, not preference — see §2. |
| **One-way economy at launch.** No cash-out, no player-to-player trading. | Keeps the whole thing out of money-transmission territory. The marketplace is a later, separate programme (§9). |

**The volatility has to sit somewhere.** A dollar-pegged bit with a BTC treasury puts it on Voxelbit
rather than on a player who bought 100 bits last month. That is the right way round: the company can
size and understand the exposure, and the player cannot.

---

## 1. The invariants

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
| **Bits sold are a liability, not revenue.** Revenue recognises on spend | deferred revenue misstated; the $10 B figure means two different things (§8) |
| **Player economy and treasury are separate ledgers** | a Bitcoin price move appears to change how many bits players own |
| **The free Sandbox never touches the ledger** | the free tier acquires a per-session cost and an attack surface it has no reason to have |

---

## 2. The arithmetic that shapes the design

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

### 2.1 Purchases are rare; spends are constant

The two halves of the system have opposite requirements and should not share a design:

| | rate at 9 M DAU | latency budget | requirement |
|---|---|---|---|
| **purchase** | ~7/sec avg, ~50/sec peak | 500 ms is fine | bulletproof, auditable, slow is acceptable |
| **spend** | ~310/sec avg, ~1,500/sec peak | must feel instant | fast, idempotent, reversible |

Neither number is large. **Do not overbuild this.** The hot path is 1,500 ledger appends per second at
full planetary scale, which is an ordinary database.

---

## 3. Phase order

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
  7  creator payouts                  ── only if "platform" (arcade.md §5)
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
- Legal review can be deferred while the economy is one-way, but scope it now so §9 is not a surprise.

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
  - early elimination → [arcade.md §3.1](arcade.md) flags this; partial refund or free re-entry
- The spend endpoint is server-authoritative and reachable only from the session service. The client
  requests a match; it does not report having paid for one.

**Gate:** kill the server between debit and session grant, repeatedly, under load; no player ends up
charged without a session, and no player gets a session without being charged.

**At the end of phase 3 the entire Arcade economy runs on granted bits.** Pricing, refunds, balances
and the full player experience can be play-tested with zero financial exposure. This is the highest-
value property of the whole ordering, and it is why Stripe comes fourth rather than first.

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

Blocked on the publisher-vs-platform question in [arcade.md §5](arcade.md), which decides whether this
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
ships without it, and it should stay out until §0's written answers say otherwise.

---

## 4. Architecture

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

## 5. What this depends on that does not exist

The financial layer needs a server tier, and [arcade.md §7](arcade.md) records that the engine plan
has none — no networking, no accounts, no backend beyond `tools/serve-nocache.py`, which is a local
static-file dev server.

**Phases 1–4 do not need the game.** Identity, ledger, purchase and a stubbed spend can be built and
tested against a fake session service while the engine work continues in parallel. The first real
coupling is phase 3's atomicity requirement, which needs a real match session to commit against.

This is worth stating plainly because it makes the financial layer **the one part of the Arcade that
can start now** without waiting on the networking decision in the engine plan.

---

## 6. Testing and gates

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

## 7. Cost model

| line | rate | on $10 B gross |
|---|---|---|
| Stripe, $20 median pack | 4.4% | $440 M |
| exchange fee + spread | ~0.5% | $50 M |
| game servers ([arcade.md §4.2](arcade.md)) | ~1–3% | $100–300 M |
| **total cost of revenue** | **~6–8%** | |

For contrast: an app-store-distributed game gives up 15–30% before any of this. Web distribution keeps
that difference, which is most of a business model and an argument for staying on the web deliberately
rather than by default.

---

## 8. Open decisions

1. **Pack ladder — which SKUs?** Three to start. The floor sets the effective fee rate (§2) and the
   ceiling sets the fraud exposure.
2. **Match-server crash policy.** Full refund, partial, or credit? Decide before phase 3, because the
   reversal path is built around the answer.
3. **Early-elimination policy** ([arcade.md §3.1](arcade.md)). Partial refund or free re-entry — this
   is a game-design decision with a financial implementation.
4. **Negative balances after a chargeback.** Allowed and collected against, or written off?
5. **Is the $10 B figure gross transaction volume or net revenue?** ([arcade.md §8](arcade.md)) —
   a 10× difference in the business being described, and it changes the pack ladder and the take rate.
6. **Publisher or platform?** ([arcade.md §5](arcade.md)) — decides whether phase 7 exists.
7. **Breakage policy.** Unspent bits are a real line item and some jurisdictions regulate them like
   gift cards, including escheatment. Decide whether bits expire before selling the first one, because
   changing it later is a term change on money already taken.

---

## 9. One-paragraph summary

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
