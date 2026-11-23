# What AthenaSIP needs of Dagda, per shape

2026-11-23, for the stack's part A (`TODO/2026-11-20-stack-integration.md`). Dagda asked for each
shape of record AthenaSIP keeps: sizes, counts, rates, latency, durability, lifetimes, whether
anything needs a true compare-and-set, how call records are scanned, and what counters mean. Taken
from the `redis://` driver at `0.13.2` and the configuration's defaults; the scripts are in
`TODO/2026-11-23-redis-scripts-audit.md`.

Counts and rates are estimates for the deployment AthenaSIP aims at first: one organisation or a
small provider, up to 10,000 subscribers over a few realms, two or three nodes. Each is given with
the assumption behind it so it can be rescaled. Sizes are of the JSON each record is stored as.

## The shapes

| Shape | Size | Count | Writes | Reads | Lifetime | Durability |
|---|---|---|---|---|---|---|
| Realm | ~1 KB | tens | rare (admin) | every REGISTER and out-of-dialog request, cached briefly | none | durable |
| Subscriber | ~0.5 KB | up to 10,000 | rare (admin) | every REGISTER and call setup | none | durable |
| Binding (registration) | 0.4-1 KB | ~2 per subscriber: 20,000 | a refresh at about half its expiry: 20,000 / 1,800 s, about 11/s | every INVITE to a subscriber: the binding list for that subscriber | the expiry the registrar granted: 60 s to the realm's maximum (3,600 s default) | may be lost: a phone registers again within its expiry |
| Administrator (user) | ~0.3 KB | tens | rare | each sign-in | none | durable |
| Admin session | ~0.2 KB | tens | each sign-in, touched per request | every API request | idle 3,600 s, absolute 43,200 s | may be lost (sign in again) |
| Trunk | ~1 KB | tens | rare (admin), status on each registration | each call to or from a carrier | none | durable |
| Trunk lease | ~50 B | one per registering trunk | renewed every 30 s per held trunk | the same | 90 s | **compare-and-set**: exactly one holder |
| Call record | 2-8 KB (participants, each leg's dialog) | live: up to ~1,000; kept: calls per day x 30 | ~5 per call (create, ringing, answer, re-INVITEs, end) | on a request in a dialog the node does not hold; the API's lists | live while up; then 30 days (`calls.history_retention`) | durable for the retention |
| Call index per subscriber | a set of call ids | one per subscriber with calls | 2 per call per participant | presence (BLF) and the API | follows the calls | rebuilt by the reader if stale |
| Presence record (subscription, publication) | 0.3-4 KB (a PIDF body) | ~10 watches per BLF subscriber: up to 100,000 | a refresh at about half its expiry: 100,000 / 1,800 s, about 55/s; a NOTIFY's state on a change | every NOTIFY decision; each node rereads what it watches every 30 s | 60-3,600 s (`presence.min_expires`, `max_expires`) | may be lost: subscriptions are refreshed |
| Re-offer report | ~0.2 KB | one per subscriber that needed one | on a 488 re-offer | the API | 30 days | best effort |
| Counter (scripts) | 8 B | as scripts make them | per script event | per script decision | a window the script sets, 1 s to a year, or none | best effort |

Nothing is larger than about 8 KB, and no transaction spans more than one subscriber's records
except the call index (two or three subscribers) and presence (a record and two indexes).

## Latency

Only one read is on the call setup path: a subscriber's bindings when an INVITE arrives for it
(plus the realm and subscriber, cached). It wants single-digit milliseconds on the local network.
Every write is off the call path: the call record is written after the request has been forwarded,
and a REGISTER is answered after its binding is written, which a phone tolerates in the hundreds of
milliseconds.

## Compare-and-set

Only the trunk lease truly needs one: two nodes must not both register the same trunk. Everything
else is atomicity or a merge rule:

- A binding: per Call-ID the higher CSeq wins (RFC 3261 10.3). A merge rule, not a CAS.
- Counters: merged by sum, the window started by the first increment.
- The call index and presence indexes: all-or-nothing membership changes; the reader mends a stale
  index, so a lost update is a stale entry, never a wrong record.

## Call records: how they are scanned

The API lists calls newest first, all of them or one subscriber's, and the oldest are dropped after
the retention. That is a log keyed by time (Dagda's D9 with the hybrid clock), with a per-subscriber
index. Volume: at 1,000 calls a day, 30,000 records of up to 8 KB, about 240 MB at the retention.

## Counters

`athenasip.store.counter_add(name, delta, seconds)` from routing scripts: an integer, added to by
any node, optionally lapsing `seconds` after it was first made (a rate window), and removed when it
comes back to zero with no window. Reads may be a little behind; a lost increment is tolerable.
Merged by sum is exactly what they need.

## What AthenaSIP would use first

A `dagda://` driver (part D of the integration plan): register-map keys `realm/id` with
`walk_from` paging for the durable records; Dagda's TTL for bindings, sessions and presence; a D5
lease for trunks; a D9 log for call records; counters merged by sum.
