#include "pocket_plugin.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct PocketPlugin {
    lua_State *L;
    PocketPluginHostFn host;
    PocketPluginLogFn log_fn;
    void *userdata;
    atomic_bool cancelled;
    double deadline;
};

static double plugin_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void plugin_log(PocketPlugin *plugin, int level, const char *message) {
    if (plugin != NULL && plugin->log_fn != NULL && message != NULL) {
        plugin->log_fn(plugin->userdata, level, message);
    }
}

static char *plugin_json_quote(const char *text) {
    if (text == NULL) {
        text = "";
    }
    size_t length = strlen(text);
    char *output = (char *)malloc(length * 6 + 3);
    if (output == NULL) {
        return NULL;
    }

    size_t cursor = 0;
    output[cursor++] = '"';
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)text[index];
        switch (character) {
            case '"': output[cursor++] = '\\'; output[cursor++] = '"'; break;
            case '\\': output[cursor++] = '\\'; output[cursor++] = '\\'; break;
            case '\n': output[cursor++] = '\\'; output[cursor++] = 'n'; break;
            case '\r': output[cursor++] = '\\'; output[cursor++] = 'r'; break;
            case '\t': output[cursor++] = '\\'; output[cursor++] = 't'; break;
            case '\b': output[cursor++] = '\\'; output[cursor++] = 'b'; break;
            case '\f': output[cursor++] = '\\'; output[cursor++] = 'f'; break;
            default:
                if (character < 0x20) {
                    int written = snprintf(output + cursor, 7, "\\u%04x", character);
                    cursor += (size_t)written;
                } else {
                    output[cursor++] = (char)character;
                }
                break;
        }
    }
    output[cursor++] = '"';
    output[cursor] = '\0';
    return output;
}

static char *plugin_result_json(const char *content, int is_error) {
    char *quoted = plugin_json_quote(content != NULL ? content : "");
    if (quoted == NULL) {
        return NULL;
    }
    size_t needed = strlen(quoted) + 64;
    char *result = (char *)malloc(needed);
    if (result == NULL) {
        free(quoted);
        return NULL;
    }
    snprintf(result, needed, "{\"content\":%s,\"is_error\":%s}", quoted, is_error ? "true" : "false");
    free(quoted);
    return result;
}

static PocketPlugin *plugin_from_state(lua_State *L) {
    return *(PocketPlugin **)lua_getextraspace(L);
}

static void plugin_timeout_hook(lua_State *L, lua_Debug *debug) {
    (void)debug;
    PocketPlugin *plugin = plugin_from_state(L);
    if (plugin == NULL) {
        return;
    }
    if (atomic_load_explicit(&plugin->cancelled, memory_order_relaxed) ||
        plugin_now() > plugin->deadline) {
        luaL_error(L, "plugin execution interrupted: timeout or cancellation");
    }
}

static int plugin_push_host_result(
    lua_State *L,
    const char *method,
    const char *a,
    const char *b,
    const char *c
) {
    PocketPlugin *plugin = plugin_from_state(L);
    if (plugin == NULL || plugin->host == NULL) {
        lua_pushnil(L);
        return 1;
    }
    char *result = plugin->host(plugin->userdata, method, a, b, c);
    if (result == NULL) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, result);
    free(result);
    return 1;
}

static int plugin_log_function(lua_State *L) {
    PocketPlugin *plugin = plugin_from_state(L);
    const char *level = luaL_optstring(L, 1, "info");
    const char *message = luaL_optstring(L, 2, "");
    int numeric_level = 1;
    if (strcmp(level, "debug") == 0) numeric_level = 0;
    else if (strcmp(level, "warning") == 0) numeric_level = 2;
    else if (strcmp(level, "error") == 0) numeric_level = 3;
    plugin_log(plugin, numeric_level, message);
    return 0;
}

static int plugin_result_function(lua_State *L) {
    const char *content = luaL_optstring(L, 1, "");
    int is_error = lua_toboolean(L, 2);
    char *result = plugin_result_json(content, is_error);
    if (result == NULL) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, result);
    free(result);
    return 1;
}

static int plugin_workspace_list(lua_State *L) {
    const char *path = luaL_optstring(L, 1, ".");
    int depth = (int)luaL_optinteger(L, 2, 2);
    char depth_string[32];
    snprintf(depth_string, sizeof(depth_string), "%d", depth);
    return plugin_push_host_result(L, "workspace.list", path, depth_string, NULL);
}

static int plugin_workspace_read(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    return plugin_push_host_result(L, "workspace.read", path, NULL, NULL);
}

static int plugin_workspace_write(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *content = luaL_checkstring(L, 2);
    int overwrite = lua_toboolean(L, 3);
    return plugin_push_host_result(
        L,
        "workspace.write",
        path,
        content,
        overwrite ? "true" : "false"
    );
}

static int plugin_workspace_search(lua_State *L) {
    const char *query = luaL_checkstring(L, 1);
    const char *include = luaL_optstring(L, 2, "");
    return plugin_push_host_result(L, "workspace.search", query, include, NULL);
}

static int plugin_workspace_root(lua_State *L) {
    return plugin_push_host_result(L, "workspace.root", NULL, NULL, NULL);
}

static int plugin_config(lua_State *L) {
    return plugin_push_host_result(L, "plugin.config", NULL, NULL, NULL);
}

static void plugin_open_library(lua_State *L, const char *name, lua_CFunction open) {
    luaL_requiref(L, name, open, 1);
    lua_pop(L, 1);
}

static void plugin_set_field(lua_State *L, const char *name, lua_CFunction function) {
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, name);
}

static void plugin_prepare(lua_State *L) {
    plugin_open_library(L, "_G", luaopen_base);
    plugin_open_library(L, LUA_TABLIBNAME, luaopen_table);
    plugin_open_library(L, LUA_STRLIBNAME, luaopen_string);
    plugin_open_library(L, LUA_MATHLIBNAME, luaopen_math);
    plugin_open_library(L, LUA_UTF8LIBNAME, luaopen_utf8);
    plugin_open_library(L, LUA_COLIBNAME, luaopen_coroutine);

    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    lua_newtable(L);
    plugin_set_field(L, "log", plugin_log_function);
    plugin_set_field(L, "result", plugin_result_function);
    plugin_set_field(L, "workspace_list", plugin_workspace_list);
    plugin_set_field(L, "workspace_read", plugin_workspace_read);
    plugin_set_field(L, "workspace_write", plugin_workspace_write);
    plugin_set_field(L, "workspace_search", plugin_workspace_search);
    plugin_set_field(L, "workspace_root", plugin_workspace_root);
    plugin_set_field(L, "config", plugin_config);
    lua_setglobal(L, "plugin");
}

PocketPlugin *pocket_plugin_new(
    PocketPluginHostFn host,
    PocketPluginLogFn log_fn,
    void *userdata
) {
    PocketPlugin *plugin = (PocketPlugin *)calloc(1, sizeof(PocketPlugin));
    if (plugin == NULL) {
        return NULL;
    }
    plugin->host = host;
    plugin->log_fn = log_fn;
    plugin->userdata = userdata;
    atomic_init(&plugin->cancelled, false);
    plugin->L = luaL_newstate();
    if (plugin->L == NULL) {
        free(plugin);
        return NULL;
    }
    *(PocketPlugin **)lua_getextraspace(plugin->L) = plugin;
    plugin_prepare(plugin->L);
    return plugin;
}

void pocket_plugin_free(PocketPlugin *plugin) {
    if (plugin == NULL) {
        return;
    }
    if (plugin->L != NULL) {
        lua_close(plugin->L);
    }
    free(plugin);
}

void pocket_plugin_cancel(PocketPlugin *plugin) {
    if (plugin != NULL) {
        atomic_store_explicit(&plugin->cancelled, true, memory_order_relaxed);
    }
}

int pocket_plugin_load(PocketPlugin *plugin, const char *main_path) {
    if (plugin == NULL || plugin->L == NULL || main_path == NULL) {
        return 1;
    }
    lua_State *L = plugin->L;
    int status = luaL_loadfilex(L, main_path, "t");
    if (status != LUA_OK) {
        plugin_log(plugin, 3, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }
    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        plugin_log(plugin, 3, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }
    return 0;
}

int pocket_plugin_activate(PocketPlugin *plugin, const char *config_lua) {
    if (plugin == NULL || plugin->L == NULL) {
        return 1;
    }
    lua_State *L = plugin->L;
    const char *literal = config_lua != NULL ? config_lua : "{}";
    size_t chunk_length = strlen(literal) + 8;
    char *chunk = (char *)malloc(chunk_length);
    if (chunk == NULL) {
        return 1;
    }
    snprintf(chunk, chunk_length, "return %s", literal);

    int status = luaL_loadbufferx(L, chunk, strlen(chunk), "@plugin-config", "t");
    free(chunk);
    if (status != LUA_OK) {
        plugin_log(plugin, 3, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }
    status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_OK) {
        plugin_log(plugin, 3, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }

    lua_getglobal(L, "activate");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }
    lua_pushvalue(L, -2);
    status = lua_pcall(L, 1, 0, 0);
    if (status != LUA_OK) {
        plugin_log(plugin, 3, lua_tostring(L, -1));
        lua_pop(L, 2);
        return 1;
    }
    lua_pop(L, 1);
    return 0;
}

char *pocket_plugin_invoke(PocketPlugin *plugin, const char *tool, const char *args_lua) {
    if (plugin == NULL || plugin->L == NULL || tool == NULL) {
        return plugin_result_json("Plugin runtime is unavailable.", 1);
    }

    lua_State *L = plugin->L;
    atomic_store_explicit(&plugin->cancelled, false, memory_order_relaxed);
    plugin->deadline = plugin_now() + 5.0;
    lua_sethook(L, plugin_timeout_hook, LUA_MASKCOUNT, 1000);

    lua_getglobal(L, "handle_tool");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return plugin_result_json("Plugin does not define handle_tool(name, args).", 1);
    }

    lua_pushstring(L, tool);

    const char *literal = args_lua != NULL ? args_lua : "{}";
    size_t chunk_length = strlen(literal) + 8;
    char *chunk = (char *)malloc(chunk_length);
    if (chunk == NULL) {
        lua_pop(L, 2);
        return plugin_result_json("Out of memory while decoding arguments.", 1);
    }
    snprintf(chunk, chunk_length, "return %s", literal);
    int status = luaL_loadbufferx(L, chunk, strlen(chunk), "@plugin-args", "t");
    free(chunk);
    if (status != LUA_OK) {
        char *message = plugin_result_json(lua_tostring(L, -1), 1);
        lua_pop(L, 3);
        return message;
    }
    status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_OK) {
        char *message = plugin_result_json(lua_tostring(L, -1), 1);
        lua_pop(L, 3);
        return message;
    }

    status = lua_pcall(L, 2, 1, 0);
    if (status != LUA_OK) {
        char *message = plugin_result_json(lua_tostring(L, -1), 1);
        lua_pop(L, 1);
        return message;
    }

    char *result = NULL;
    if (lua_isstring(L, -1)) {
        const char *text = lua_tostring(L, -1);
        size_t length = strlen(text);
        result = (char *)malloc(length + 1);
        if (result != NULL) {
            memcpy(result, text, length + 1);
        }
    } else if (lua_istable(L, -1)) {
        const char *content = "";
        int is_error = 0;
        lua_getfield(L, -1, "content");
        if (lua_isstring(L, -1)) {
            content = lua_tostring(L, -1);
        }
        lua_getfield(L, -2, "is_error");
        is_error = lua_toboolean(L, -1);
        result = plugin_result_json(content, is_error);
        lua_pop(L, 2);
    } else {
        result = plugin_result_json("Plugin returned no result.", 1);
    }

    lua_pop(L, 1);
    return result;
}

void pocket_plugin_free_string(char *string) {
    if (string != NULL) {
        free(string);
    }
}
