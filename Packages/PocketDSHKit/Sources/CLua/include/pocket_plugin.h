#ifndef POCKET_PLUGIN_H
#define POCKET_PLUGIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PocketPlugin PocketPlugin;

typedef void (*PocketPluginLogFn)(void *userdata, int level, const char *message);

/// Returns a malloc-allocated C string. The bridge frees it with free().
/// a/b/c are optional UTF-8 arguments; the meaning depends on method.
typedef char *(*PocketPluginHostFn)(
    void *userdata,
    const char *method,
    const char *a,
    const char *b,
    const char *c
);

PocketPlugin *pocket_plugin_new(
    PocketPluginHostFn host,
    PocketPluginLogFn log_fn,
    void *userdata
);
void pocket_plugin_free(PocketPlugin *plugin);
int pocket_plugin_load(PocketPlugin *plugin, const char *main_path);
int pocket_plugin_activate(PocketPlugin *plugin, const char *config_lua);
void pocket_plugin_cancel(PocketPlugin *plugin);

/// Returns a malloc-allocated JSON string, or NULL on allocation failure.
char *pocket_plugin_invoke(
    PocketPlugin *plugin,
    const char *tool,
    const char *args_lua
);
void pocket_plugin_free_string(char *string);

#ifdef __cplusplus
}
#endif

#endif
