# lua_store
Lua C module: key-value store backed entirely by C heap memory. Data is never registered with the Lua GC, so it survives across  collectgarbage() calls and is unaffected by gc metamethods.
