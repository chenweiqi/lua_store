-- test.lua  –  lua_store module test suite

-- Make the shared library discoverable in the current directory
--package.cpath = "./?.so;./?.dll;" .. package.cpath

local store = require("lua_store")

-- ---- minimal test harness -----------------------------------------------
local pass, fail = 0, 0

local function check(name, cond)
    if cond then
        io.write(("  [PASS] %s\n"):format(name))
        pass = pass + 1
    else
        io.write(("  [FAIL] %s\n"):format(name))
        fail = fail + 1
    end
end

-- =========================================================================
print("=== lua_store test suite ===\n")

-- ---- 1. primitive types -------------------------------------------------
print("-- 1. primitives --")
store.set("s",  "hello")
store.set("i",  42)
store.set("f",  3.14)
store.set("bt", true)
store.set("bf", false)

check("string",        store.get("s")  == "hello")
check("integer",       store.get("i")  == 42)
check("float",         math.abs(store.get("f") - 3.14) < 1e-9)
check("boolean true",  store.get("bt") == true)
check("boolean false", store.get("bf") == false)
check("missing key",   store.get("__missing__") == nil)

-- ---- 2. overwrite -------------------------------------------------------
print("\n-- 2. overwrite --")
store.set("s", "world")
check("overwrite string",  store.get("s") == "world")
store.set("i", 100)
check("overwrite integer", store.get("i") == 100)

-- ---- 3. del -------------------------------------------------------------
print("\n-- 3. del --")
store.set("tmp", "x")
check("del returns true",        store.del("tmp")         == true)
check("value gone after del",    store.get("tmp")         == nil)
check("del missing returns false",store.del("__missing__") == false)

-- ---- 4. set(key,nil) acts like del --------------------------------------
print("\n-- 4. set nil --")
store.set("s", nil)
check("set nil removes key", store.get("s") == nil)

-- ---- 5. flat table ------------------------------------------------------
print("\n-- 5. flat table --")
local t1 = { name = "lua", version = 54, active = true, ratio = 0.5 }
store.set("t1", t1)
local r1 = store.get("t1")
check("table string field",  r1.name    == "lua")
check("table integer field", r1.version == 54)
check("table bool field",    r1.active  == true)
check("table float field",   math.abs(r1.ratio - 0.5) < 1e-9)

-- ---- 6. array-like table (integer keys 1..N) ----------------------------
print("\n-- 6. array --")
local arr = { 10, 20, 30, 40 }
store.set("arr", arr)
local ra = store.get("arr")
check("array[1]", ra[1] == 10)
check("array[2]", ra[2] == 20)
check("array[3]", ra[3] == 30)
check("array[4]", ra[4] == 40)

-- ---- 7. nested table ----------------------------------------------------
print("\n-- 7. nested table --")
local nested = { top = "yes", inner = { mid = { deep = 99 } } }
store.set("nested", nested)
local rn = store.get("nested")
check("nested top field",  rn.top           == "yes")
check("nested deep field", rn.inner.mid.deep == 99)

-- ---- 8. count -----------------------------------------------------------
print("\n-- 8. count --")
-- Clean up all keys used so far, start fresh
for _, k in ipairs({"i","f","bt","bf","t1","arr","nested"}) do
    store.del(k)
end
-- Store should now be empty (s and tmp were already removed)
check("empty store count", store.count() == 0)

store.set("c1", 1)
store.set("c2", 2)
store.set("c3", 3)
check("count after 3 sets", store.count() == 3)

store.del("c2")
check("count after 1 del",  store.count() == 2)

store.set("c1", nil)   -- nil-set also removes
check("count after nil-set", store.count() == 1)

store.del("c3")
check("count back to 0", store.count() == 0)

print("gc count", collectgarbage("count"))
-- ---- 9. GC independence (the key feature) -------------------------------
print("\n-- 9. GC independence --")
do
    -- Create a table, store it, then drop all Lua-side references.
    local tmp = { score = 999, tag = "gc_test", sub = { ok = true } }
    store.set("gc_data", tmp)
    tmp = nil   -- no more Lua references to the original table
end

-- Force two full GC cycles – any Lua-managed copy would be collected.
collectgarbage("collect")
collectgarbage("collect")
print("gc count", collectgarbage("count"))

local gr = store.get("gc_data")
check("data survives GC",           gr ~= nil)
check("integer field after GC",     gr ~= nil and gr.score    == 999)
check("string field after GC",      gr ~= nil and gr.tag      == "gc_test")
check("nested bool field after GC", gr ~= nil and gr.sub ~= nil and gr.sub.ok == true)

-- Verify independence: modify returned table, re-fetch, original unchanged
if gr then
    gr.score = 0
    local gr2 = store.get("gc_data")
    check("stored copy unaffected by mutation", gr2.score == 999)
end

store.del("gc_data")

-- ---- 10. large number of keys (hash collision stress) -------------------
print("\n-- 10. stress (512 keys) --")
local N = 512
for j = 1, N do
    store.set("key_" .. j, j * 10)
end
local ok = true
for j = 1, N do
    if store.get("key_" .. j) ~= j * 10 then ok = false break end
end
check("all 512 values correct", ok)
for j = 1, N do store.del("key_" .. j) end
check("count is 0 after bulk del", store.count() == 0)

print(("\n=== %d passed, %d failed ==="):format(pass, fail))
if fail > 0 then os.exit(1) end

print("\n=== lua_store pressure test suite ===")

local data_count = 500000
print("data count", data_count)

print("\n-- 1.set data --")
local test_time = os.time()
for k=1, data_count do
    -- Create a table, store it, then drop all Lua-side references.
    local tmp = { score = 999, tag = "gc_test", sub = { ok = true }, k=k, val = {1,2,3,4} }
    store.set("gc_data"..k, tmp)
    tmp = nil   -- no more Lua references to the original table
end
print("set data time", string.format("%ds", os.time() - test_time))

print("\n-- 2.force two full GC cycles --")
test_time = os.time()
collectgarbage("collect")
collectgarbage("collect")
print("gc count", collectgarbage("count"))
print("force gc time", string.format("%ds", os.time() - test_time))
print("store count", store.count())

print("\n-- 3.traverse data --")
test_time = os.time()
for k=1, data_count do
    local gr = store.get("gc_data"..k)
    if gr == nil then
        print("data missing after GC")
    end
    if gr ~= nil and gr.score    ~= 999 then
        print("integer field after GC")
    end
    if gr ~= nil and gr.tag      ~= "gc_test" then
        print("string field after GC")
    end
    if gr ~= nil and gr.sub == nil or gr.sub.ok ~= true then
        print("nested bool field after GC")
    end
    if gr ~= nil and gr.k ~= k then
        print("gr.k field after GC")
    end
    if gr ~= nil and #gr.val ~= 4 then
        print("gr.val field after GC")
    end
end
print("traverse time", string.format("%ds", os.time() - test_time))

print("\n-- 4.delete data --")
test_time = os.time()
for k=1, data_count do
    store.del("gc_data"..k)
end
print("delete time", string.format("%ds", os.time() - test_time))

print("\n-- 5.force two full GC cycles --")
test_time = os.time()
collectgarbage("collect")
collectgarbage("collect")
print("gc count", collectgarbage("count"))
print("force gc time", string.format("%ds", os.time() - test_time))
print("store count", store.count())

print("\n-- 6.check data exists --")
test_time = os.time()
for k=1, data_count do
    local gr = store.get("gc_data"..k)
    if gr ~= nil then
        print("data delete fail")
    end
end
print("check time", string.format("%ds", os.time() - test_time))

print("\n=== lua_store pressure test passed ===\n")