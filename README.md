# lua_store
Lua C module: key-value store backed entirely by C heap memory. Data is never registered with the Lua GC, so it survives across  collectgarbage() calls and is unaffected by gc metamethods.

## Features

- Progressive rehashing, Redis `dict.c` style
- Two hash tables with incremental bucket migration
- Small number of buckets migrated per operation to avoid latency spikes

## Public API

- `store.set(key, value)` - save value; `set(key, nil)` removes the key
- `store.get(key)` - return stored value, or `nil` if missing
- `store.del(key)` - remove key, returns `true`/`false`
- `store.count()` - number of top-level keys

## Supported value types

- `nil`
- `boolean`
- `integer`
- `float`
- `string`
- `table` (deep-copied recursively into C memory)

Table keys must be strings or numbers; other key types are skipped.

## Requirements

- Lua 5.3+