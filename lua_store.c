/*
 * lua_store.c
 *
 * Lua C module: key-value store backed entirely by C heap memory.
 * Data is never registered with the Lua GC, so it survives across
 * collectgarbage() calls and is unaffected by __gc metamethods.
 *
 * Progressive rehashing (Redis dict.c style): two hash tables with
 * incremental bucket migration. A small number of buckets are
 * migrated on each operation, avoiding latency spikes.
 *
 * Public API:
 *   store.set(key, value)  -- save value; set(key, nil) removes the key
 *   store.get(key)         -- return stored value, or nil if missing
 *   store.del(key)         -- remove key, returns true/false
 *   store.count()          -- number of top-level keys
 *
 * Supported value types: nil, boolean, integer, float, string, table
 * (tables are deep-copied recursively into C memory).
 * Table keys must be strings or numbers; other key types are skipped.
 *
 * Requires Lua 5.3+.
 */

#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- tunables ---- */
#define INIT_BUCKETS     16
#define MAX_LOAD_FACTOR  1.0
#define MIN_LOAD_FACTOR  0.1
#define REHASH_STEPS     1

/* ------------------------------------------------------------------ types */

typedef struct SValue  SValue;
typedef struct SEntry  SEntry;
typedef struct STable  STable;
typedef struct dictht  dictht;

typedef enum {
    SV_NIL = 0,
    SV_BOOLEAN,
    SV_INTEGER,
    SV_NUMBER,
    SV_STRING,
    SV_TABLE,
} SVType;

struct SValue {
    SVType type;
    union {
        int         boolean;
        lua_Integer integer;
        lua_Number  number;
        char       *str;
        STable     *table;
    };
};

struct SEntry {
    char   *key;
    SValue  val;
    SEntry *next;
};

struct dictht {
    SEntry      **table;
    unsigned long size;
    unsigned long sizemask;
    unsigned long used;
};

struct STable {
    dictht ht[2];
    long   rehashidx;  /* -1 = no rehash in progress */
};

/* ------------------------------------------------------------------- hash */

static unsigned int key_hash(const char *s, unsigned long sizemask)
{
    unsigned int h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h & sizemask;
}

/* ----------------------------------------------------------------- init */

static void ht_init(dictht *ht, unsigned long size)
{
    ht->size     = size;
    ht->sizemask = size - 1;
    ht->used     = 0;
    ht->table    = (SEntry **)calloc(size, sizeof(SEntry *));
}

static void dict_init_if_needed(STable *t)
{
    if (t->ht[0].size == 0) {
        ht_init(&t->ht[0], INIT_BUCKETS);
        t->rehashidx = -1;
    }
}

/* ----------------------------------------------------------- rehash logic */

static int dict_rehash_step(STable *t)
{
    if (t->rehashidx == -1 || t->ht[0].size == 0)
        return 0;

    /* skip empty buckets */
    while ((unsigned long)t->rehashidx < t->ht[0].size &&
           t->ht[0].table[t->rehashidx] == NULL)
        t->rehashidx++;

    if ((unsigned long)t->rehashidx >= t->ht[0].size) {
        /* all buckets migrated */
        free(t->ht[0].table);
        t->ht[0] = t->ht[1];
        memset(&t->ht[1], 0, sizeof(dictht));
        t->rehashidx = -1;
        return 0;
    }

    /* migrate one bucket */
    SEntry *e = t->ht[0].table[t->rehashidx];
    t->ht[0].table[t->rehashidx] = NULL;
    while (e) {
        SEntry *next = e->next;
        unsigned int h = key_hash(e->key, t->ht[1].sizemask);
        e->next = t->ht[1].table[h];
        t->ht[1].table[h] = e;
        t->ht[0].used--;
        t->ht[1].used++;
        e = next;
    }
    t->rehashidx++;
    return 1;
}

static void dict_rehash_multiple(STable *t, int n)
{
    dict_init_if_needed(t);
    for (int i = 0; i < n; i++) {
        if (!dict_rehash_step(t)) break;
    }
}

static void dict_expand_if_needed(STable *t)
{
    if (t->rehashidx != -1) return;      /* already rehashing */
    if (t->ht[0].size == 0) return;      /* not initialized */
    if (t->ht[0].used >= t->ht[0].size * MAX_LOAD_FACTOR) {
        unsigned long new_size = t->ht[0].size * 2;
        if (new_size == 0) new_size = INIT_BUCKETS;
        ht_init(&t->ht[1], new_size);
        t->rehashidx = 0;
    }
}

static void dict_shrink_if_needed(STable *t)
{
    if (t->rehashidx != -1) return;
    if (t->ht[0].size == 0) return;
    if (t->ht[0].used < t->ht[0].size * MIN_LOAD_FACTOR &&
        t->ht[0].size > INIT_BUCKETS) {
        unsigned long new_size = t->ht[0].size / 2;
        if (new_size < INIT_BUCKETS) new_size = INIT_BUCKETS;
        ht_init(&t->ht[1], new_size);
        t->rehashidx = 0;
    }
}

/* ------------------------------------------------------------------- free */

static void free_svalue(SValue *v);

static void clear_table(STable *t)
{
    for (int j = 0; j < 2; j++) {
        dictht *ht = &t->ht[j];
        if (ht->table == NULL) continue;
        for (unsigned long i = 0; i < ht->size; i++) {
            SEntry *e = ht->table[i];
            while (e) {
                SEntry *nx = e->next;
                free(e->key);
                free_svalue(&e->val);
                free(e);
                e = nx;
            }
        }
        free(ht->table);
        memset(ht, 0, sizeof(dictht));
    }
    t->rehashidx = -1;
}

static void free_svalue(SValue *v)
{
    switch (v->type) {
    case SV_STRING: free(v->str);                                 break;
    case SV_TABLE:  clear_table(v->table); free(v->table);        break;
    default:                                                      break;
    }
    v->type = SV_NIL;
}

/* --------------------------------------------------------------- table ops */

static SEntry *dict_find(STable *t, const char *key)
{
    for (int j = 0; j < 2; j++) {
        dictht *ht = &t->ht[j];
        if (ht->table == NULL) continue;
        unsigned int h = key_hash(key, ht->sizemask);
        for (SEntry *e = ht->table[h]; e; e = e->next)
            if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

/* Find or create entry; frees the old value so caller can write a new one. */
static SEntry *dict_upsert(STable *t, const char *key)
{
    dict_init_if_needed(t);

    dictht *target = (t->rehashidx != -1) ? &t->ht[1] : &t->ht[0];

    /* check target table first */
    {
        unsigned int h = key_hash(key, target->sizemask);
        for (SEntry *e = target->table[h]; e; e = e->next) {
            if (strcmp(e->key, key) == 0) {
                free_svalue(&e->val);
                return e;
            }
        }
    }

    /* if rehashing, also check ht[0] for existing key */
    if (t->rehashidx != -1) {
        unsigned int h0 = key_hash(key, t->ht[0].sizemask);
        for (SEntry *e = t->ht[0].table[h0]; e; e = e->next) {
            if (strcmp(e->key, key) == 0) {
                free_svalue(&e->val);
                return e;
            }
        }
    }

    /* not found — new key. expand if needed */
    dict_expand_if_needed(t);
    target = (t->rehashidx != -1) ? &t->ht[1] : &t->ht[0];

    {
        unsigned int h = key_hash(key, target->sizemask);
        SEntry *e = (SEntry *)malloc(sizeof(SEntry));
        if (!e) return NULL;
        e->key      = strdup(key);
        e->val.type = SV_NIL;
        e->next     = target->table[h];
        target->table[h] = e;
        target->used++;
        return e;
    }
}

static int dict_delete(STable *t, const char *key)
{
    for (int j = 0; j < 2; j++) {
        dictht *ht = &t->ht[j];
        if (ht->table == NULL) continue;
        unsigned int h = key_hash(key, ht->sizemask);
        for (SEntry **pp = &ht->table[h]; *pp; pp = &(*pp)->next) {
            if (strcmp((*pp)->key, key) == 0) {
                SEntry *e = *pp;
                *pp = e->next;
                free(e->key);
                free_svalue(&e->val);
                free(e);
                ht->used--;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------ Lua value -> SValue */

static int lua_to_sval(lua_State *L, int idx, SValue *v);

static void lua_tbl_to_stable(lua_State *L, int idx, STable *t)
{
    dict_rehash_multiple(t, REHASH_STEPS);

    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        char        kbuf[64];
        const char *key = NULL;

        switch (lua_type(L, -2)) {
        case LUA_TSTRING:
            key = lua_tostring(L, -2);
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, -2))
                snprintf(kbuf, sizeof kbuf, "%lld",
                         (long long)lua_tointeger(L, -2));
            else
                snprintf(kbuf, sizeof kbuf, "%.17g",
                         (double)lua_tonumber(L, -2));
            key = kbuf;
            break;
        default:
            lua_pop(L, 1); /* skip unsupported key types */
            continue;
        }

        dict_rehash_multiple(t, REHASH_STEPS);
        SEntry *e = dict_upsert(t, key);
        if (e) lua_to_sval(L, -1, &e->val);
        lua_pop(L, 1);
    }
}

static int lua_to_sval(lua_State *L, int idx, SValue *v)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        v->type = SV_NIL;
        break;
    case LUA_TBOOLEAN:
        v->type    = SV_BOOLEAN;
        v->boolean = lua_toboolean(L, idx);
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) {
            v->type    = SV_INTEGER;
            v->integer = lua_tointeger(L, idx);
        } else {
            v->type   = SV_NUMBER;
            v->number = lua_tonumber(L, idx);
        }
        break;
    case LUA_TSTRING:
        v->type = SV_STRING;
        v->str  = strdup(lua_tostring(L, idx));
        break;
    case LUA_TTABLE:
        v->type  = SV_TABLE;
        v->table = (STable *)calloc(1, sizeof(STable));
        if (v->table)
            lua_tbl_to_stable(L, idx, v->table);
        break;
    default:
        v->type = SV_NIL;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------ SValue -> Lua value */

static void sval_to_lua(lua_State *L, const SValue *v);

static void stable_to_lua(lua_State *L, const STable *t)
{
    lua_newtable(L);
    for (int j = 0; j < 2; j++) {
        const dictht *ht = &t->ht[j];
        if (ht->table == NULL) continue;
        for (unsigned long i = 0; i < ht->size; i++) {
            for (const SEntry *e = ht->table[i]; e; e = e->next) {
                /* Restore numeric keys that were stringified on storage */
                char *end;
                long long iv = strtoll(e->key, &end, 10);
                if (*end == '\0') {
                    lua_pushinteger(L, (lua_Integer)iv);
                } else {
                    double dv = strtod(e->key, &end);
                    if (*end == '\0') lua_pushnumber(L, (lua_Number)dv);
                    else              lua_pushstring(L, e->key);
                }
                sval_to_lua(L, &e->val);
                lua_settable(L, -3);
            }
        }
    }
}

static void sval_to_lua(lua_State *L, const SValue *v)
{
    switch (v->type) {
    case SV_NIL:     lua_pushnil(L);               break;
    case SV_BOOLEAN: lua_pushboolean(L, v->boolean); break;
    case SV_INTEGER: lua_pushinteger(L, v->integer); break;
    case SV_NUMBER:  lua_pushnumber(L, v->number);   break;
    case SV_STRING:  lua_pushstring(L, v->str);      break;
    case SV_TABLE:   stable_to_lua(L, v->table);     break;
    }
}

/* ------------------------------------------------------- global store state */

static STable g_store;

/* ---------------------------------------------------------------- Lua C API */

/* store.set(key, value)  --  set(key, nil) is equivalent to del(key) */
static int l_set(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);

    dict_rehash_multiple(&g_store, REHASH_STEPS);

    if (lua_isnil(L, 2)) {
        if (dict_delete(&g_store, key))
            dict_shrink_if_needed(&g_store);
        return 0;
    }

    SEntry *e = dict_upsert(&g_store, key);
    if (!e) return luaL_error(L, "lua_store: out of memory");
    lua_to_sval(L, 2, &e->val);
    return 0;
}

/* store.get(key) -> value | nil */
static int l_get(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    dict_rehash_multiple(&g_store, REHASH_STEPS);
    const SEntry *e = dict_find(&g_store, key);
    if (!e) { lua_pushnil(L); return 1; }
    sval_to_lua(L, &e->val);
    return 1;
}

/* store.del(key) -> boolean */
static int l_del(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    dict_rehash_multiple(&g_store, REHASH_STEPS);
    int deleted = dict_delete(&g_store, key);
    if (deleted) dict_shrink_if_needed(&g_store);
    lua_pushboolean(L, deleted);
    return 1;
}

/* store.count() -> integer */
static int l_count(lua_State *L)
{
    dict_rehash_multiple(&g_store, REHASH_STEPS);
    lua_pushinteger(L, (lua_Integer)(g_store.ht[0].used + g_store.ht[1].used));
    return 1;
}

static const luaL_Reg lua_store_lib[] = {
    { "set",   l_set   },
    { "get",   l_get   },
    { "del",   l_del   },
    { "count", l_count },
    { NULL,    NULL    }
};

LUALIB_API int luaopen_lua_store(lua_State *L)
{
    luaL_newlib(L, lua_store_lib);
    return 1;
}
