# The Redis driver's scripts and commands: an audit

2026-11-23, against `src/datastores/redis_datastore.cpp` at `0.13.2`. For the stack's part A
(`TODO/2026-11-20-stack-integration.md`): every key a script touches must be declared in `KEYS`,
which Redis Cluster requires and Mimir's gates check; and which records truly need a
compare-and-set, for Dagda.

## What the driver sends

Commands sent directly: `PING`, `GET`, `MGET`, `SET` (alone, or with `EX seconds`), `EXISTS`,
`DEL`, `SADD`, `SREM`, `SMEMBERS`, `EVAL`. Nothing else: no `SCAN`, no transactions (`MULTI`), no
pub/sub, no streams. Inside the scripts: `GET`, `SET ... EX`, `EXPIRE`, `INCRBY`, `TTL`, `DEL`,
`SADD`, `SREM`, and `cjson.decode`.

Every record is a JSON string under one key; every list is a set of member names that index those
keys (`athena:index:*`), read with `SMEMBERS` and then `MGET`. An index that names a record that has
gone is mended by the reader (`SREM`), so an index may trail its records but never leads them.

## The five scripts

| Script | Keys declared | Keys touched | Conforms |
|---|---|---|---|
| `lease` (`athena:lease:<name>`) | 1 | the lease | yes |
| `counter_add` (`athena:counter:<name>`) | 1 | the counter | yes |
| `_index_call_subscribers` | one per subscriber on the call | the same | yes, but several keys in one script: Redis Cluster refuses it unless they share a hash slot, and Mimir's one-key rule refuses it today |
| `presence_put` | 1, the record | the record, and up to four index keys built inside the script from `ARGV` prefixes and the old record's `aor` and `node_id` | **no** |
| `presence_delete` | 1, the record | the record, and two index keys built from the old record | **no** |

The two presence scripts cannot simply declare their keys: which index keys they touch depends on
the old record, which they read inside the script.

## What truly needs atomicity

- **A trunk's lease** (`lease`): a real compare-and-set on one key. Two nodes asking at once must
  not both win; the holder renews only while it still holds it. This is Dagda's D5 lease.
- **A shared counter** (`counter_add`): an atomic increment on one key, its window started by the
  first increment. Counters merged by sum would do.
- **A call's subscriber indexes** (`_index_call_subscribers`): one membership change on each
  participant's index, all or none, so a node dying midway leaves no half-written index. Not a
  compare-and-set: the index is a cache the reader mends, so a lost update shows as a stale index
  entry, never a wrong record.
- **A presence record and its indexes** (`presence_put`, `presence_delete`): the record and the two
  indexes that name it move together, so a subscription that moves to another holder leaves the old
  holder's index in the same step.
- **A registration binding needs none**, despite appearances: per Call-ID the higher CSeq wins
  (RFC 3261 10.3), which is a merge rule, not a compare-and-set. The driver does not enforce it
  today (a plain `SET` of the binding); a `dagda://` driver would.

Nothing else is read and then written on what it read.

## Recommendation for the two presence scripts

Read the old record first, outside the script, and pass every key in `KEYS`: the record, the old
and new presentity indexes, the old and new holder indexes. Pass the old value in `ARGV`; the script
writes only if the record still holds it, and otherwise returns 0 for the caller to read again and
retry, with a bound and a backoff. Every key is then declared, the update stays all-or-nothing, and
a concurrent change is retried rather than lost.

That makes them conform to `KEYS`, but they remain multi-key, as the call index does. For Redis
Cluster the keys would need one hash slot (a `{presentity}` hash tag on the record and its
presentity index puts those together, but the holder index is per node), and Mimir would need
multi-key writes, which wait on Dannan's atomic commit across replica sets. So the multi-key part
is for Mimir and Dagda to answer, not something the driver can remove alone.
