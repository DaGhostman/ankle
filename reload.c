#include "reload.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>

#include <tllist.h>

#define LOG_MODULE "reload"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "notify.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

/* Editors often write files in several steps; wait for them to finish */
#define RELOAD_DELAY_MS 150

struct watch {
    int wd;
    char *name;  /* File name, in the watched directory */
};

struct config_reloader {
    struct fdm *fdm;
    struct wayland *wayl;
    struct server *server;

    struct config *initial;  /* Owned by the caller */
    struct config *current;  /* Either 'initial', or owned by us */

    char *conf_path;         /* NULL: search the default locations */
    config_override_t overrides;
    bool as_server;

    int inotify_fd;
    int timer_fd;
    tll(struct watch) watches;
};

/* There's only ever one, but key binding handlers have no way to
 * reach it */
static struct config_reloader *the_reloader = NULL;

static void
schedule_reload(struct config_reloader *reloader)
{
    const struct itimerspec timeout = {
        .it_value = {
            .tv_sec = RELOAD_DELAY_MS / 1000,
            .tv_nsec = (RELOAD_DELAY_MS % 1000) * 1000000,
        },
    };

    if (timerfd_settime(reloader->timer_fd, 0, &timeout, NULL) < 0)
        LOG_ERRNO("failed to arm configuration reload timer");
}

static void
watches_clear(struct config_reloader *reloader)
{
    tll_foreach(reloader->watches, it) {
        /* The same directory may be watched for multiple files */
        bool wd_still_used = false;
        tll_foreach(reloader->watches, it2) {
            if (it2 != it && it2->item.wd == it->item.wd) {
                wd_still_used = true;
                break;
            }
        }

        if (!wd_still_used)
            inotify_rm_watch(reloader->inotify_fd, it->item.wd);

        free(it->item.name);
        tll_remove(reloader->watches, it);
    }
}

static void
watch_file(struct config_reloader *reloader, const char *path)
{
    /*
     * Watch the directory, rather than the file itself; files are
     * often replaced (written to a temporary file, then renamed),
     * which a watch on the file wouldn't catch.
     */
    char *dir_copy = xstrdup(path);
    char *name_copy = xstrdup(path);
    const char *dir = dirname(dir_copy);
    const char *name = basename(name_copy);

    int wd = inotify_add_watch(
        reloader->inotify_fd, dir,
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);

    if (wd < 0)
        LOG_ERRNO("%s: failed to watch for changes", dir);
    else {
        LOG_DBG("watching %s (in %s)", name, dir);
        tll_push_back(reloader->watches, ((struct watch){.wd = wd, .name = xstrdup(name)}));
    }

    free(dir_copy);
    free(name_copy);
}

static void
watches_setup(struct config_reloader *reloader)
{
    watches_clear(reloader);

    if (!reloader->current->auto_reload)
        return;

    tll_foreach(reloader->current->source_files, it) {
        const char *path = it->item;
        watch_file(reloader, path);

        /* Configs are often symlinks (e.g. managed dotfiles); also
         * watch the file the link points to */
        char *real = realpath(path, NULL);
        if (real != NULL && !streq(real, path))
            watch_file(reloader, real);
        free(real);
    }
}

static void
notify_failure(struct config_reloader *reloader, const struct config *failed)
{
    const char *reason = "unknown error";
    tll_foreach(failed->notifications, it) {
        if (it->item.kind == USER_NOTIFICATION_ERROR) {
            reason = it->item.text;
            break;
        }
    }

    LOG_ERR("configuration not reloaded: %s", reason);

    if (tll_length(reloader->wayl->terms) == 0)
        return;

    struct terminal *term = tll_front(reloader->wayl->terms);
    struct notification notif = {
        .title = xstrdup("ankle: configuration not reloaded"),
        .body = xstrdup(reason),
        .expire_time = -1,

        /* The user is likely editing the configuration in a focused
         * terminal; still show the notification */
        .force = true,
    };

    /* notify_notify() takes ownership of what it needs; free the rest */
    notify_notify(term, &notif);
    notify_free(term, &notif);
}

static void
reload(struct config_reloader *reloader)
{
    struct config *old = reloader->current;
    struct config *new = xmalloc(sizeof(*new));
    user_notifications_t notifications = tll_init();

    /* Errors are fatal; keep the old configuration on any error */
    if (!config_load(new, reloader->conf_path, &notifications,
                     &reloader->overrides, true, reloader->as_server))
    {
        notify_failure(reloader, new);
        config_free(new);
        free(new);
        user_notifications_free(&notifications);

        /* The broken file may have been added, or moved */
        return;
    }

    user_notifications_free(&notifications);

    /* Options set on the command line, rather than in the configuration */
    new->presentation_timings = old->presentation_timings;
    new->hold_at_exit = old->hold_at_exit;
    free(new->server_socket_path);
    new->server_socket_path = old->server_socket_path != NULL
        ? xstrdup(old->server_socket_path) : NULL;

    if (reloader->server != NULL)
        server_config_reloaded(reloader->server, new);
    else {
        tll_foreach(reloader->wayl->terms, it) {
            if (it->item->conf == old)
                term_config_reload(it->item, new);
        }
    }

    reloader->current = new;

    if (old != reloader->initial) {
        config_free(old);
        free(old);
    }

    /* Includes may have changed */
    watches_setup(reloader);

    LOG_INFO("configuration reloaded");
}

static bool
fdm_inotify(struct fdm *fdm, int fd, int events, void *data)
{
    struct config_reloader *reloader = data;

    if (events & EPOLLHUP)
        return false;

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(fd, buf, sizeof(buf));

    if (len < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return true;
        LOG_ERRNO("failed to read inotify events");
        return false;
    }

    bool changed = false;

    for (const char *p = buf; p < buf + len; ) {
        const struct inotify_event *ev = (const struct inotify_event *)p;
        p += sizeof(*ev) + ev->len;

        if (ev->len == 0)
            continue;

        tll_foreach(reloader->watches, it) {
            if (it->item.wd == ev->wd && streq(it->item.name, ev->name)) {
                LOG_DBG("%s: changed", ev->name);
                changed = true;
                break;
            }
        }
    }

    if (changed)
        schedule_reload(reloader);

    return true;
}

static bool
fdm_timer(struct fdm *fdm, int fd, int events, void *data)
{
    struct config_reloader *reloader = data;

    if (events & EPOLLHUP)
        return false;

    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return true;
        LOG_ERRNO("failed to read configuration reload timer");
        return false;
    }

    reload(reloader);
    return true;
}

struct config_reloader *
config_reloader_init(struct fdm *fdm, struct wayland *wayl,
                     struct server *server, struct config *conf,
                     const char *conf_path,
                     const config_override_t *overrides, bool as_server)
{
    xassert(the_reloader == NULL);

    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0) {
        LOG_ERRNO("failed to create inotify instance");
        return NULL;
    }

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        LOG_ERRNO("failed to create configuration reload timer");
        close(inotify_fd);
        return NULL;
    }

    struct config_reloader *reloader = xmalloc(sizeof(*reloader));
    *reloader = (struct config_reloader){
        .fdm = fdm,
        .wayl = wayl,
        .server = server,
        .initial = conf,
        .current = conf,
        .conf_path = conf_path != NULL ? xstrdup(conf_path) : NULL,
        .overrides = tll_init(),
        .as_server = as_server,
        .inotify_fd = inotify_fd,
        .timer_fd = timer_fd,
        .watches = tll_init(),
    };

    tll_foreach(*overrides, it)
        tll_push_back(reloader->overrides, xstrdup(it->item));

    if (!fdm_add(fdm, inotify_fd, EPOLLIN, &fdm_inotify, reloader)) {
        close(inotify_fd);
        close(timer_fd);
        goto err;
    }

    if (!fdm_add(fdm, timer_fd, EPOLLIN, &fdm_timer, reloader)) {
        fdm_del(fdm, inotify_fd);
        close(timer_fd);
        goto err;
    }

    watches_setup(reloader);
    the_reloader = reloader;
    return reloader;

err:
    LOG_ERR("failed to register configuration reload FDs");
    tll_free_and_free(reloader->overrides, free);
    free(reloader->conf_path);
    free(reloader);
    return NULL;
}

void
config_reloader_destroy(struct config_reloader *reloader)
{
    if (reloader == NULL)
        return;

    watches_clear(reloader);
    fdm_del(reloader->fdm, reloader->inotify_fd);
    fdm_del(reloader->fdm, reloader->timer_fd);

    if (reloader->current != reloader->initial) {
        config_free(reloader->current);
        free(reloader->current);
    }

    tll_free_and_free(reloader->overrides, free);
    free(reloader->conf_path);
    free(reloader);
    the_reloader = NULL;
}

void
config_reload_request(void)
{
    if (the_reloader == NULL) {
        LOG_WARN("configuration reloading is not available");
        return;
    }

    /* Deferred, since we may be called from e.g. a key binding
     * handler, which is still using the current key bindings */
    schedule_reload(the_reloader);
}
