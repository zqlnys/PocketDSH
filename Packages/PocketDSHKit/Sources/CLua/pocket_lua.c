#include "pocket_lua.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct PocketLua {
    lua_State *L;
    PocketLuaWriteFn write_fn;
    void *userdata;
    atomic_bool cancelled;
    double deadline;
};

static double pocket_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void pocket_emit(PocketLua *runtime, int channel, const char *bytes, size_t length) {
    if (runtime != NULL && runtime->write_fn != NULL && bytes != NULL && length > 0) {
        runtime->write_fn(runtime->userdata, channel, bytes, length);
    }
}

static void pocket_emit_cstr(PocketLua *runtime, int channel, const char *text) {
    if (text != NULL) {
        pocket_emit(runtime, channel, text, strlen(text));
    }
}

static int pocket_print(lua_State *L) {
    PocketLua *runtime = *(PocketLua **)lua_getextraspace(L);
    int count = lua_gettop(L);

    for (int index = 1; index <= count; index++) {
        size_t length = 0;
        const char *text = luaL_tolstring(L, index, &length);
        if (index > 1) {
            pocket_emit(runtime, 0, "\t", 1);
        }
        pocket_emit(runtime, 0, text, length);
        lua_pop(L, 1);
    }

    pocket_emit(runtime, 0, "\n", 1);
    return 0;
}

static void pocket_timeout_hook(lua_State *L, lua_Debug *debug) {
    (void)debug;
    PocketLua *runtime = *(PocketLua **)lua_getextraspace(L);
    if (runtime == NULL) {
        return;
    }

    if (atomic_load_explicit(&runtime->cancelled, memory_order_relaxed) ||
        pocket_now() > runtime->deadline) {
        luaL_error(L, "execution interrupted: timeout or cancellation");
    }
}

static void pocket_open_library(lua_State *L, const char *name, lua_CFunction open) {
    luaL_requiref(L, name, open, 1);
    lua_pop(L, 1);
}

static int pocket_prepare(lua_State *L) {
    /* No io/os/package/debug libraries: scripts cannot spawn processes or
       touch arbitrary files. `dofile`/`loadfile` are removed as well. */
    pocket_open_library(L, "_G", luaopen_base);
    pocket_open_library(L, LUA_TABLIBNAME, luaopen_table);
    pocket_open_library(L, LUA_STRLIBNAME, luaopen_string);
    pocket_open_library(L, LUA_MATHLIBNAME, luaopen_math);
    pocket_open_library(L, LUA_UTF8LIBNAME, luaopen_utf8);
    pocket_open_library(L, LUA_COLIBNAME, luaopen_coroutine);

    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    lua_pushcfunction(L, pocket_print);
    lua_setglobal(L, "print");

    return 0;
}

PocketLua *pocket_lua_new(PocketLuaWriteFn write_fn, void *userdata) {
    PocketLua *runtime = (PocketLua *)calloc(1, sizeof(PocketLua));
    if (runtime == NULL) {
        return NULL;
    }

    runtime->write_fn = write_fn;
    runtime->userdata = userdata;
    atomic_init(&runtime->cancelled, false);

    runtime->L = luaL_newstate();
    if (runtime->L == NULL) {
        free(runtime);
        return NULL;
    }

    *(PocketLua **)lua_getextraspace(runtime->L) = runtime;
    if (pocket_prepare(runtime->L) != 0) {
        lua_close(runtime->L);
        free(runtime);
        return NULL;
    }

    return runtime;
}

void pocket_lua_free(PocketLua *runtime) {
    if (runtime == NULL) {
        return;
    }
    if (runtime->L != NULL) {
        lua_close(runtime->L);
    }
    free(runtime);
}

void pocket_lua_cancel(PocketLua *runtime) {
    if (runtime != NULL) {
        atomic_store_explicit(&runtime->cancelled, true, memory_order_relaxed);
    }
}

int pocket_lua_run(PocketLua *runtime, const char *source, size_t length, double timeout_seconds) {
    if (runtime == NULL || runtime->L == NULL || source == NULL) {
        return 1;
    }

    runtime->deadline = pocket_now() + (timeout_seconds > 0 ? timeout_seconds : 5.0);

    lua_State *L = runtime->L;
    lua_sethook(L, pocket_timeout_hook, LUA_MASKCOUNT, 1000);

    int status = luaL_loadbufferx(L, source, length, "@pocket.lua", "t");
    if (status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        pocket_emit_cstr(runtime, 1, message);
        pocket_emit(runtime, 1, "\n", 1);
        lua_pop(L, 1);
        return 1;
    }

    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        pocket_emit_cstr(runtime, 1, message);
        pocket_emit(runtime, 1, "\n", 1);
        lua_pop(L, 1);
        return 1;
    }

    return 0;
}
