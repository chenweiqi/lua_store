# -----------------------------------------------------------------------
# Makefile for lua_store – Lua C extension
#
# Usage:
#   make              build the shared library
#   make test         build then run test.lua
#   make clean        remove built artifacts
#
# -----------------------------------------------------------------------

CC        ?= gcc

PREFIX    ?= /usr/local
LUA_INC   ?= $(PREFIX)/include/lua
LUA       ?= $(PREFIX)/bin/lua

LUA_CFLAGS := -I$(LUA_INC)

CFLAGS := -Wall -Wextra -O2 -fPIC $(LUA_CFLAGS)

LDFLAGS = -shared
SO_EXT  = .so
TARGET := lua_store$(SO_EXT)

# -------------------------------------------------------------------------
.PHONY: all test clean

all: $(TARGET)

$(TARGET): lua_store.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: $(TARGET)
	$(LUA) test.lua

clean:
	rm -f $(TARGET)
