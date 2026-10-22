#include "paste.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>

#define LOG_MODULE "paste"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "debug.h"
#include "render.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

static void
send_queued_input(struct terminal *term)
{
    /* Make sure we send any queued up non-paste data */
    if (tll_length(term->ptmx_buffers) > 0)
        fdm_event_add(term->fdm, term->ptmx, EPOLLOUT);
}

static void
reset(struct terminal *term)
{
    free(term->paste_confirm.data);
    term->paste_confirm.data = NULL;
    term->paste_confirm.len = 0;
    term->paste_confirm.size = 0;
    term->paste_confirm.lines = 0;
    term->paste_confirm.receiving = false;
    term->paste_confirm.active = false;
}

bool
paste_confirm_wanted(const struct terminal *term)
{
    switch (term->conf->security.paste_protection) {
    case PASTE_PROTECTION_NEVER:       return false;
    case PASTE_PROTECTION_UNBRACKETED: return !term->bracketed_paste;
    case PASTE_PROTECTION_ALWAYS:      return true;
    }

    BUG("Invalid paste protection mode");
    return false;
}

bool
paste_in_progress(const struct terminal *term)
{
    return term->is_sending_paste_data || term->paste_confirm.active;
}

size_t
paste_count_line_breaks(const char *data, size_t len)
{
    size_t breaks = 0;

    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            breaks++;
        else if (data[i] == '\r') {
            breaks++;
            if (i + 1 < len && data[i + 1] == '\n')
                i++;
        }
    }

    return breaks;
}

void
paste_begin(struct terminal *term)
{
    term->is_sending_paste_data = true;

    if (paste_confirm_wanted(term)) {
        reset(term);
        term->paste_confirm.receiving = true;
        term->paste_confirm.bracketed = term->bracketed_paste;
        return;
    }

    if (term->bracketed_paste)
        term_paste_data_to_slave(term, "\033[200~", 6);
}

void
paste_data(struct terminal *term, const char *data, size_t size)
{
    xassert(term->is_sending_paste_data);

    if (!term->paste_confirm.receiving) {
        term_paste_data_to_slave(term, data, size);
        return;
    }

    if (size == 0)
        return;

    if (term->paste_confirm.len + size > term->paste_confirm.size) {
        size_t new_size = term->paste_confirm.size > 0
            ? term->paste_confirm.size : 4096;
        while (new_size < term->paste_confirm.len + size)
            new_size *= 2;

        term->paste_confirm.data = xrealloc(term->paste_confirm.data, new_size);
        term->paste_confirm.size = new_size;
    }

    memcpy(&term->paste_confirm.data[term->paste_confirm.len], data, size);
    term->paste_confirm.len += size;
}

static void
send_buffered(struct terminal *term)
{
    const bool bracketed = term->paste_confirm.bracketed;

    term->is_sending_paste_data = true;

    if (bracketed)
        term_paste_data_to_slave(term, "\033[200~", 6);
    if (term->paste_confirm.len > 0) {
        term_paste_data_to_slave(
            term, term->paste_confirm.data, term->paste_confirm.len);
    }
    if (bracketed)
        term_paste_data_to_slave(term, "\033[201~", 6);

    term->is_sending_paste_data = false;
    reset(term);
    send_queued_input(term);
}

void
paste_end(struct terminal *term)
{
    xassert(term->is_sending_paste_data);

    if (!term->paste_confirm.receiving) {
        if (term->bracketed_paste)
            term_paste_data_to_slave(term, "\033[201~", 6);

        term->is_sending_paste_data = false;
        send_queued_input(term);
        return;
    }

    term->paste_confirm.receiving = false;

    const char *data = term->paste_confirm.data;
    const size_t len = term->paste_confirm.len;
    const size_t breaks = paste_count_line_breaks(data, len);

    if (breaks == 0) {
        /* Single line; nothing to confirm */
        send_buffered(term);
        return;
    }

    /* A trailing line break doesn't start a new line */
    const char last = data[len - 1];
    term->paste_confirm.lines =
        breaks + (last != '\n' && last != '\r' ? 1 : 0);

    LOG_DBG("holding back paste of %zu bytes (%zu lines)",
            len, term->paste_confirm.lines);

    /* Don't hold back keyboard input etc while waiting */
    term->is_sending_paste_data = false;
    send_queued_input(term);

    term->paste_confirm.active = true;
    render_refresh(term);
}

void
paste_confirm_accept(struct terminal *term)
{
    if (!term->paste_confirm.active)
        return;

    send_buffered(term);
    render_refresh(term);
}

void
paste_confirm_cancel(struct terminal *term)
{
    if (!term->paste_confirm.active)
        return;

    reset(term);
    render_refresh(term);
}

void
paste_confirm_input(struct seat *seat, struct terminal *term,
                    xkb_keysym_t sym)
{
    if (sym == XKB_KEY_y ||
        sym == XKB_KEY_Y ||
        sym == XKB_KEY_Return ||
        sym == XKB_KEY_KP_Enter)
    {
        seat->kbd.last_shortcut_sym = sym;
        paste_confirm_accept(term);
    }

    else if (sym == XKB_KEY_n ||
             sym == XKB_KEY_N ||
             sym == XKB_KEY_q ||
             sym == XKB_KEY_Escape ||
             (seat->kbd.ctrl && (sym == XKB_KEY_c ||
                                 sym == XKB_KEY_d ||
                                 sym == XKB_KEY_g)))
    {
        seat->kbd.last_shortcut_sym = sym;
        paste_confirm_cancel(term);
    }

    /* All other keys are ignored */
}

void
paste_confirm_prompt(const struct terminal *term, char *buf, size_t size)
{
    const size_t lines = term->paste_confirm.lines;

    if (lines <= 1)
        snprintf(buf, size, "Paste text ending in a line break? [y/N]");
    else
        snprintf(buf, size, "Paste %zu lines? [y/N]", lines);
}

#if defined(_DEBUG)
#include <fcntl.h>

static size_t
test_read_slave(int fd, char *buf, size_t size)
{
    ssize_t count = read(fd, buf, size - 1);
    if (count < 0)
        count = 0;
    buf[count] = '\0';
    return count;
}

static void
test_paste(struct terminal *term, const char *text)
{
    paste_begin(term);
    paste_data(term, text, strlen(text));
    paste_end(term);
}

UNITTEST
{
    xassert(paste_count_line_breaks("", 0) == 0);
    xassert(paste_count_line_breaks("abc", 3) == 0);
    xassert(paste_count_line_breaks("a\nb", 3) == 1);
    xassert(paste_count_line_breaks("a\rb", 3) == 1);
    xassert(paste_count_line_breaks("a\r\nb", 4) == 1);
    xassert(paste_count_line_breaks("a\r\n\r\nb\n", 7) == 3);
    xassert(paste_count_line_breaks("\n\r", 2) == 2);
}

UNITTEST
{
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 2, 10, 2);

    conf.security.paste_protection = PASTE_PROTECTION_NEVER;
    term.bracketed_paste = false;
    xassert(!paste_confirm_wanted(&term));
    term.bracketed_paste = true;
    xassert(!paste_confirm_wanted(&term));

    conf.security.paste_protection = PASTE_PROTECTION_UNBRACKETED;
    term.bracketed_paste = false;
    xassert(paste_confirm_wanted(&term));
    term.bracketed_paste = true;
    xassert(!paste_confirm_wanted(&term));

    conf.security.paste_protection = PASTE_PROTECTION_ALWAYS;
    term.bracketed_paste = false;
    xassert(paste_confirm_wanted(&term));
    term.bracketed_paste = true;
    xassert(paste_confirm_wanted(&term));

    term_test_destroy(&term);
}

UNITTEST
{
    struct config conf = {
        .security.paste_protection = PASTE_PROTECTION_UNBRACKETED,
    };
    struct terminal term;
    term_test_init(&term, &conf, 2, 10, 2);

    int fds[2];
    xassert(pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);
    term.ptmx = fds[1];

    struct seat seat = {0};
    char buf[256];

    /* Protection disabled: sent right away */
    conf.security.paste_protection = PASTE_PROTECTION_NEVER;
    test_paste(&term, "ls\rrm x\r");
    xassert(!term.paste_confirm.active);
    xassert(!term.is_sending_paste_data);
    test_read_slave(fds[0], buf, sizeof(buf));
    xassert(streq(buf, "ls\rrm x\r"));

    conf.security.paste_protection = PASTE_PROTECTION_UNBRACKETED;

    /* Single line: sent right away */
    test_paste(&term, "echo hello");
    xassert(!term.paste_confirm.active);
    xassert(!term.is_sending_paste_data);
    xassert(term.paste_confirm.data == NULL);
    test_read_slave(fds[0], buf, sizeof(buf));
    xassert(streq(buf, "echo hello"));

    /* Empty paste: nothing to confirm */
    test_paste(&term, "");
    xassert(!term.paste_confirm.active);
    xassert(test_read_slave(fds[0], buf, sizeof(buf)) == 0);

    /* Bracketed paste enabled: not protected in "unbracketed" mode */
    term.bracketed_paste = true;
    test_paste(&term, "a\nb");
    xassert(!term.paste_confirm.active);
    test_read_slave(fds[0], buf, sizeof(buf));
    xassert(streq(buf, "\033[200~a\nb\033[201~"));
    term.bracketed_paste = false;

    /* Multi-line: held back (in several chunks) */
    paste_begin(&term);
    paste_data(&term, "ls\r", 3);
    paste_data(&term, "rm -rf x", 8);
    xassert(term.is_sending_paste_data);
    xassert(term.paste_confirm.receiving);
    paste_end(&term);
    xassert(term.paste_confirm.active);
    xassert(!term.paste_confirm.receiving);
    xassert(!term.is_sending_paste_data);
    xassert(paste_in_progress(&term));
    xassert(term.paste_confirm.lines == 2);
    xassert(term.render.refresh.grid);
    xassert(test_read_slave(fds[0], buf, sizeof(buf)) == 0);

    paste_confirm_prompt(&term, buf, sizeof(buf));
    xassert(streq(buf, "Paste 2 lines? [y/N]"));

    /* Other keys are ignored */
    paste_confirm_input(&seat, &term, XKB_KEY_a);
    paste_confirm_input(&seat, &term, XKB_KEY_space);
    xassert(term.paste_confirm.active);
    xassert(test_read_slave(fds[0], buf, sizeof(buf)) == 0);

    /* 'n' cancels; nothing is sent */
    paste_confirm_input(&seat, &term, XKB_KEY_n);
    xassert(!term.paste_confirm.active);
    xassert(!paste_in_progress(&term));
    xassert(term.paste_confirm.data == NULL);
    xassert(test_read_slave(fds[0], buf, sizeof(buf)) == 0);

    /* Escape cancels too */
    test_paste(&term, "a\rb\r");
    xassert(term.paste_confirm.active);
    xassert(term.paste_confirm.lines == 2);
    paste_confirm_input(&seat, &term, XKB_KEY_Escape);
    xassert(!term.paste_confirm.active);
    xassert(test_read_slave(fds[0], buf, sizeof(buf)) == 0);

    /* So does Control+c, but not a plain 'c' */
    test_paste(&term, "a\rb");
    paste_confirm_input(&seat, &term, XKB_KEY_c);
    xassert(term.paste_confirm.active);
    seat.kbd.ctrl = true;
    paste_confirm_input(&seat, &term, XKB_KEY_c);
    seat.kbd.ctrl = false;
    xassert(!term.paste_confirm.active);
    xassert(test_read_slave(fds[0], buf, sizeof(buf)) == 0);

    /* 'y' sends it */
    test_paste(&term, "ls\r");
    xassert(term.paste_confirm.active);
    xassert(term.paste_confirm.lines == 1);
    paste_confirm_prompt(&term, buf, sizeof(buf));
    xassert(streq(buf, "Paste text ending in a line break? [y/N]"));
    paste_confirm_input(&seat, &term, XKB_KEY_y);
    xassert(seat.kbd.last_shortcut_sym == XKB_KEY_y);
    xassert(!term.paste_confirm.active);
    xassert(!paste_in_progress(&term));
    xassert(term.paste_confirm.data == NULL);
    test_read_slave(fds[0], buf, sizeof(buf));
    xassert(streq(buf, "ls\r"));

    /* Return sends it too */
    test_paste(&term, "a\rb");
    paste_confirm_input(&seat, &term, XKB_KEY_Return);
    test_read_slave(fds[0], buf, sizeof(buf));
    xassert(streq(buf, "a\rb"));

    /* "always": bracketed pastes are protected too, and still
     * bracketed when confirmed, even if the application has disabled
     * bracketed paste mode in the meantime */
    conf.security.paste_protection = PASTE_PROTECTION_ALWAYS;
    term.bracketed_paste = true;
    test_paste(&term, "x\ny\n");
    xassert(term.paste_confirm.active);
    xassert(term.paste_confirm.bracketed);
    term.bracketed_paste = false;
    paste_confirm_accept(&term);
    test_read_slave(fds[0], buf, sizeof(buf));
    xassert(streq(buf, "\033[200~x\ny\n\033[201~"));

    /* Large pastes are buffered in full */
    char large[10000];
    for (size_t i = 0; i < sizeof(large) - 1; i++)
        large[i] = i % 100 == 99 ? '\r' : 'a';
    large[sizeof(large) - 1] = '\0';

    paste_begin(&term);
    for (size_t i = 0; i < sizeof(large) - 1; i += 1000)
        paste_data(&term, &large[i], 1000 - (i + 1000 > sizeof(large) - 1));
    paste_end(&term);
    xassert(term.paste_confirm.active);
    xassert(term.paste_confirm.len == sizeof(large) - 1);
    xassert(memcmp(term.paste_confirm.data, large, sizeof(large) - 1) == 0);
    xassert(term.paste_confirm.lines == 100);
    paste_confirm_cancel(&term);

    close(fds[0]);
    close(fds[1]);
    term_test_destroy(&term);
}
#endif
