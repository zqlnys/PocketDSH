#ifndef POCKET_LUA_H
#define POCKET_LUA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PocketLua PocketLua;

/// channel 0 = stdout, channel 1 = stderr.
typedef void (*PocketLuaWriteFn)(void *userdata, int channel, const char *bytes, size_t length);

PocketLua *pocket_lua_new(PocketLuaWriteFn write_fn, void *userdata);
void pocket_lua_free(PocketLua *runtime);
void pocket_lua_cancel(PocketLua *runtime);

/// Runs one Lua chunk. Returns 0 on success, 1 on compile/runtime error.
/// `timeout_seconds <= 0` uses a five second default.
int pocket_lua_run(PocketLua *runtime, const char *source, size_t length, double timeout_seconds);

#ifdef __cplusplus
}
#endif

#endif
