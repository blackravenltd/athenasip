# The Redis driver's scripts, verbatim

Every Lua script `redis://` sends with `EVAL`, exactly as sent: the body is given once as a JSON
string (every byte, the leading newline and indentation included) and once as text. Mimir's
differential gate replays them against Valkey; `TODO/2026-11-23-redis-scripts-audit.md` says which
declare every key they touch. Each script lives as a `static constexpr` string in
`src/datastores/redis_datastore.cpp`; a change to one changes this file in the same commit.

The driver sends `EVAL <body> <numkeys> <keys...> <args...>`, every argument a bulk string.

## `lease`

`RedisDatastore::lease`, `kTake`. A trunk's lease: take it if free, renew it if this node holds it.

- KEYS: 1
- Example KEYS: `athena:lease:trunk:acme`
- Example ARGV: `node-a`, `120`

Body, exactly:

```json
"\n    local held = redis.call('GET', KEYS[1])\n    if not held then redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2]) return 1 end\n    if held == ARGV[1] then redis.call('EXPIRE', KEYS[1], ARGV[2]) return 1 end\n    return 0\n  "
```

As text:

```lua
    local held = redis.call('GET', KEYS[1])
    if not held then redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2]) return 1 end
    if held == ARGV[1] then redis.call('EXPIRE', KEYS[1], ARGV[2]) return 1 end
    return 0
```

## `counter_add`

`RedisDatastore::counter_add`, `kAdd`. A shared counter: add, start its window on the first add, remove it when it comes back to nothing with no window.

- KEYS: 1
- Example KEYS: `athena:counter:calls:example.com`
- Example ARGV: `1`, `0`

Body, exactly:

```json
"\n    local value = redis.call('INCRBY', KEYS[1], ARGV[1])\n    local lapses = redis.call('TTL', KEYS[1])\n    if tonumber(ARGV[2]) > 0 and lapses < 0 then redis.call('EXPIRE', KEYS[1], ARGV[2]) end\n    if value == 0 and lapses < 0 and tonumber(ARGV[2]) == 0 then redis.call('DEL', KEYS[1]) end\n    return value\n  "
```

As text:

```lua
    local value = redis.call('INCRBY', KEYS[1], ARGV[1])
    local lapses = redis.call('TTL', KEYS[1])
    if tonumber(ARGV[2]) > 0 and lapses < 0 then redis.call('EXPIRE', KEYS[1], ARGV[2]) end
    if value == 0 and lapses < 0 and tonumber(ARGV[2]) == 0 then redis.call('DEL', KEYS[1]) end
    return value
```

## `call subscriber index`

`RedisDatastore::_index_call_subscribers`, `kIndex`. Add the call to, or remove it from, the index of every subscriber on it (1 adds, 0 removes).

- KEYS: one per subscriber on the call
- Example KEYS: `athena:index:subscriber-calls:sip:alice@example.com`, `athena:index:subscriber-calls:sip:bob@example.com`
- Example ARGV: `call-1@example.com`, `1`

Body, exactly:

```json
"\n    for _, key in ipairs(KEYS) do\n      if ARGV[2] == '1' then redis.call('SADD', key, ARGV[1]) else redis.call('SREM', key, ARGV[1]) end\n    end\n    return #KEYS\n  "
```

As text:

```lua
    for _, key in ipairs(KEYS) do
      if ARGV[2] == '1' then redis.call('SADD', key, ARGV[1]) else redis.call('SREM', key, ARGV[1]) end
    end
    return #KEYS
```

## `presence_put`

`RedisDatastore::presence_put`, `kPut`. Write a presence record and move it between indexes. Touches index keys built from ARGV[6], ARGV[7] and the old record: not KEYS-conforming.

- KEYS: 1
- Example KEYS: `athena:presence:subscription:sub-1`
- Example ARGV: `{"kind":"subscription","id":"sub-1","aor":"sip:alice@example.com","node_id":"node-a","content_type":"","body":"","expires_at":1795000000}`, `3600`, `sub-1`, `sip:alice@example.com`, `node-a`, `athena:index:presence:subscription:`, `athena:index:presence-node:`, `subscription:sub-1`

Body, exactly:

```json
"\n    local old = redis.call('GET', KEYS[1])\n    if old then\n      local was = cjson.decode(old)\n      if was.aor ~= ARGV[4] then redis.call('SREM', ARGV[6] .. was.aor, ARGV[3]) end\n      if was.node_id ~= '' and was.node_id ~= ARGV[5] then redis.call('SREM', ARGV[7] .. was.node_id, ARGV[8]) end\n    end\n    redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])\n    redis.call('SADD', ARGV[6] .. ARGV[4], ARGV[3])\n    if ARGV[5] ~= '' then redis.call('SADD', ARGV[7] .. ARGV[5], ARGV[8]) end\n    return 1\n  "
```

As text:

```lua
    local old = redis.call('GET', KEYS[1])
    if old then
      local was = cjson.decode(old)
      if was.aor ~= ARGV[4] then redis.call('SREM', ARGV[6] .. was.aor, ARGV[3]) end
      if was.node_id ~= '' and was.node_id ~= ARGV[5] then redis.call('SREM', ARGV[7] .. was.node_id, ARGV[8]) end
    end
    redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2])
    redis.call('SADD', ARGV[6] .. ARGV[4], ARGV[3])
    if ARGV[5] ~= '' then redis.call('SADD', ARGV[7] .. ARGV[5], ARGV[8]) end
    return 1
```

## `presence_delete`

`RedisDatastore::presence_delete`, `kDelete`. Delete a presence record and take it out of its indexes. Touches index keys built from ARGV[1], ARGV[2] and the old record: not KEYS-conforming.

- KEYS: 1
- Example KEYS: `athena:presence:subscription:sub-1`
- Example ARGV: `athena:index:presence:subscription:`, `athena:index:presence-node:`, `sub-1`, `subscription:sub-1`

Body, exactly:

```json
"\n    local old = redis.call('GET', KEYS[1])\n    if old then\n      local was = cjson.decode(old)\n      redis.call('SREM', ARGV[1] .. was.aor, ARGV[3])\n      if was.node_id ~= '' then redis.call('SREM', ARGV[2] .. was.node_id, ARGV[4]) end\n      redis.call('DEL', KEYS[1])\n    end\n    return 1\n  "
```

As text:

```lua
    local old = redis.call('GET', KEYS[1])
    if old then
      local was = cjson.decode(old)
      redis.call('SREM', ARGV[1] .. was.aor, ARGV[3])
      if was.node_id ~= '' then redis.call('SREM', ARGV[2] .. was.node_id, ARGV[4]) end
      redis.call('DEL', KEYS[1])
    end
    return 1
```
