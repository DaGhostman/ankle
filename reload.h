#pragma once

#include <stdbool.h>

#include "config.h"
#include "fdm.h"
#include "server.h"
#include "wayland.h"

/*
 * Reloads the configuration when its files change (auto-reload-config),
 * or when requested with the config-reload key binding.
 *
 * The configuration is loaded into a new config struct. If it fails to
 * load, the old configuration is kept, and the user is notified. If it
 * succeeds, all terminals are switched over to it.
 */
struct config_reloader;

/*
 * 'conf' is owned by the caller, and must stay alive until the
 * reloader is destroyed. 'server' is NULL unless running in server
 * mode.
 */
struct config_reloader *config_reloader_init(
    struct fdm *fdm, struct wayland *wayl, struct server *server,
    struct config *conf, const char *conf_path,
    const config_override_t *overrides, bool as_server);

/* Must be called after all terminals have been destroyed */
void config_reloader_destroy(struct config_reloader *reloader);

/* Schedules a reload (safe to call from e.g. key binding handlers) */
void config_reload_request(void);
