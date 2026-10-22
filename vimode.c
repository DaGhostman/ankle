#include "vimode.h"

#include <ctype.h>

#define LOG_MODULE "vimode"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "commands.h"
#include "config.h"
#include "grid.h"
#include "render.h"
#include "selection.h"
#include "util.h"

/*
 * vi mode: a keyboard driven cursor, moving through the scrollback
 * and the screen, with vi-like motions, and visual mode selections.
 *
 * Internally, positions use scrollback relative row numbers (0 is the
 * oldest row in the scrollback, and the bottom row of the screen is
 * the last row). The cursor and the visual mode anchor are stored
 * with absolute row numbers, so that they stay on the same content
 * when the terminal scrolls.
 *
 * A position's column may be equal to the number of columns. This
 * is a "virtual newline", following a row that ends with a hard line
 * break, and is treated as whitespace by the word motions. The cursor
 * itself never stays on it.
 */

struct vipos {
    int row;  /* Scrollback relative */
    int col;
};

static int
sb_start(const struct terminal *term)
{
    return grid_sb_start_ignore_uninitialized(term->grid, term->rows);
}

static int
abs_to_rel(const struct terminal *term, int abs_row)
{
    return grid_row_abs_to_sb_precalc_sb_start(
        term->grid, sb_start(term), abs_row);
}

static int
rel_to_abs(const struct terminal *term, int rel_row)
{
    return grid_row_sb_to_abs_precalc_sb_start(
        term->grid, sb_start(term), rel_row);
}

/* Scrollback relative row number of the screen's bottom row */
static int
last_row(const struct terminal *term)
{
    return abs_to_rel(term, grid_row_absolute(term->grid, term->rows - 1));
}

/* Scrollback relative row number of the view's top row */
static int
view_top(const struct terminal *term)
{
    return abs_to_rel(term, term->grid->view);
}

static const struct row *
get_row(const struct terminal *term, int rel_row)
{
    const struct row *row = term->grid->rows[rel_to_abs(term, rel_row)];
    xassert(row != NULL);
    return row;
}

static const struct cell *
get_cell(const struct terminal *term, const struct vipos *p)
{
    xassert(p->col < term->cols);
    return &get_row(term, p->row)->cells[p->col];
}

static struct vipos
cursor_pos(const struct terminal *term)
{
    struct vipos p = {
        .row = abs_to_rel(term, term->vimode.cursor.row),
        .col = term->vimode.cursor.col,
    };

    /* The cursor's row may have been scrolled out of the scrollback */
    p.row = min(p.row, last_row(term));
    p.col = min(max(p.col, 0), term->cols - 1);
    return p;
}

enum char_class {CLASS_SPACE, CLASS_PUNCT, CLASS_WORD};

static enum char_class
char_class(const struct terminal *term, const struct vipos *p, bool big_word)
{
    if (p->col >= term->cols)
        return CLASS_SPACE;  /* Virtual newline */

    const char32_t wc = get_cell(term, p)->wc;

    if (wc == 0 || wc == U' ' || wc == U'\t')
        return CLASS_SPACE;
    if (big_word)
        return CLASS_WORD;

    if (wc < 0x80)
        return isalnum((int)wc) || wc == U'_' ? CLASS_WORD : CLASS_PUNCT;

    /* Non-ASCII (including composed characters) */
    if (wc < CELL_COMB_CHARS_LO &&
        term->conf->word_delimiters != NULL &&
        c32chr(term->conf->word_delimiters, wc) != NULL)
    {
        return CLASS_PUNCT;
    }

    return CLASS_WORD;
}

static bool
is_spacer(const struct terminal *term, const struct vipos *p)
{
    return p->col < term->cols && get_cell(term, p)->wc >= CELL_SPACER;
}

/* Steps to the next position. Returns false at the end */
static bool
pos_next(const struct terminal *term, struct vipos *p)
{
    const int last = last_row(term);
    struct vipos q = *p;

    do {
        if (q.col >= term->cols) {
            if (q.row >= last)
                return false;
            q.row++;
            q.col = 0;
        } else if (q.col == term->cols - 1) {
            if (q.row >= last)
                return false;

            if (get_row(term, q.row)->linebreak)
                q.col = term->cols;
            else {
                q.row++;
                q.col = 0;
            }
        } else
            q.col++;
    } while (is_spacer(term, &q));

    *p = q;
    return true;
}

/* Steps to the previous position. Returns false at the start */
static bool
pos_prev(const struct terminal *term, struct vipos *p)
{
    struct vipos q = *p;

    do {
        if (q.col >= term->cols)
            q.col = term->cols - 1;
        else if (q.col == 0) {
            if (q.row == 0)
                return false;
            q.row--;
            q.col = get_row(term, q.row)->linebreak
                ? term->cols : term->cols - 1;
        } else
            q.col--;
    } while (is_spacer(term, &q));

    *p = q;
    return true;
}

static struct vipos
motion_word_forward(const struct terminal *term, struct vipos p, bool big)
{
    const enum char_class c = char_class(term, &p, big);

    if (c != CLASS_SPACE) {
        do {
            if (!pos_next(term, &p))
                return p;
        } while (char_class(term, &p, big) == c);
    }

    while (char_class(term, &p, big) == CLASS_SPACE) {
        if (!pos_next(term, &p))
            return p;
    }

    return p;
}

static struct vipos
motion_word_end(const struct terminal *term, struct vipos p, bool big)
{
    if (!pos_next(term, &p))
        return p;

    while (char_class(term, &p, big) == CLASS_SPACE) {
        if (!pos_next(term, &p))
            return p;
    }

    const enum char_class c = char_class(term, &p, big);

    while (true) {
        struct vipos q = p;
        if (!pos_next(term, &q) || char_class(term, &q, big) != c)
            return p;
        p = q;
    }
}

static struct vipos
motion_word_backward(const struct terminal *term, struct vipos p, bool big)
{
    if (!pos_prev(term, &p))
        return p;

    while (char_class(term, &p, big) == CLASS_SPACE) {
        if (!pos_prev(term, &p))
            return p;
    }

    const enum char_class c = char_class(term, &p, big);

    while (true) {
        struct vipos q = p;
        if (!pos_prev(term, &q) || char_class(term, &q, big) != c)
            return p;
        p = q;
    }
}

static int
first_non_blank(const struct terminal *term, int row)
{
    for (int col = 0; col < term->cols; col++) {
        const struct vipos p = {row, col};
        if (char_class(term, &p, true) != CLASS_SPACE)
            return col;
    }
    return 0;
}

static int
last_non_empty(const struct terminal *term, int row)
{
    const struct row *r = get_row(term, row);

    for (int col = term->cols - 1; col >= 0; col--) {
        const char32_t wc = r->cells[col].wc;
        if (wc != 0 && wc < CELL_SPACER)
            return col;
    }
    return 0;
}

/* Scrolls the view so that the cursor is visible */
static void
ensure_cursor_visible(struct terminal *term)
{
    const int cursor = cursor_pos(term).row;
    const int top = view_top(term);

    if (cursor < top)
        cmd_scrollback_up(term, top - cursor);
    else if (cursor > top + term->rows - 1)
        cmd_scrollback_down(term, cursor - (top + term->rows - 1));
}

static void
dirty_cursor_cell(struct terminal *term)
{
    struct row *row = term->grid->rows[
        term->vimode.cursor.row & (term->grid->num_rows - 1)];
    if (row == NULL)
        return;

    const struct vipos p = {.col = min(term->vimode.cursor.col, term->cols - 1)};
    row->dirty = true;
    row->cells[p.col].attrs.clean = 0;
}

static void
update_selection(struct terminal *term)
{
    if (term->vimode.visual == VIMODE_VISUAL_NONE)
        return;

    const int last = last_row(term);
    struct vipos a = {
        .row = min(abs_to_rel(term, term->vimode.anchor.row), last),
        .col = term->vimode.anchor.col,
    };
    struct vipos b = cursor_pos(term);

    struct vipos start, end;
    enum selection_kind kind;

    switch (term->vimode.visual) {
    case VIMODE_VISUAL_NONE:
        return;

    case VIMODE_VISUAL_CHAR:
        kind = SELECTION_CHAR_WISE;
        if (a.row < b.row || (a.row == b.row && a.col <= b.col)) {
            start = a;
            end = b;
        } else {
            start = b;
            end = a;
        }
        break;

    case VIMODE_VISUAL_LINE:
        kind = SELECTION_LINE_WISE;
        start = (struct vipos){min(a.row, b.row), 0};
        end = (struct vipos){max(a.row, b.row), term->cols - 1};

        /* Expand to (soft wrapped) logical lines */
        while (start.row > 0 && !get_row(term, start.row - 1)->linebreak)
            start.row--;
        while (end.row < last && !get_row(term, end.row)->linebreak)
            end.row++;
        break;

    case VIMODE_VISUAL_BLOCK:
        kind = SELECTION_BLOCK;
        start = (struct vipos){min(a.row, b.row), min(a.col, b.col)};
        end = (struct vipos){max(a.row, b.row), max(a.col, b.col)};
        break;
    }

    /* Include the whole of a multi-column character at the end */
    while (end.col < term->cols - 1) {
        const struct vipos next = {end.row, end.col + 1};
        if (get_cell(term, &next)->wc < CELL_SPACER)
            break;
        end.col++;
    }

    selection_set(
        term, kind,
        (struct coord){start.col, rel_to_abs(term, start.row)},
        (struct coord){end.col, rel_to_abs(term, end.row)});
}

static void
set_cursor(struct terminal *term, struct vipos p)
{
    dirty_cursor_cell(term);

    p.row = min(max(p.row, 0), last_row(term));
    p.col = min(max(p.col, 0), term->cols - 1);

    /* Never on the padding of a multi-column character */
    while (p.col > 0 && get_cell(term, &p)->wc >= CELL_SPACER)
        p.col--;

    term->vimode.cursor = (struct coord){p.col, rel_to_abs(term, p.row)};

    ensure_cursor_visible(term);
    dirty_cursor_cell(term);
    update_selection(term);
    render_refresh(term);
}

/* Scrolls the view, keeping the cursor inside it */
static void
scroll_view(struct terminal *term, int rows)
{
    if (rows < 0)
        cmd_scrollback_up(term, -rows);
    else
        cmd_scrollback_down(term, rows);

    struct vipos p = cursor_pos(term);
    const int top = view_top(term);
    p.row = min(max(p.row, top), top + term->rows - 1);
    set_cursor(term, p);
}

static void
toggle_visual(struct terminal *term, int visual)
{
    if (term->vimode.visual == visual) {
        term->vimode.visual = VIMODE_VISUAL_NONE;
        selection_cancel(term);
        return;
    }

    if (term->vimode.visual == VIMODE_VISUAL_NONE)
        term->vimode.anchor = term->vimode.cursor;
    else {
        /* Switching kind; the selection is re-created from scratch */
        selection_cancel(term);
    }

    term->vimode.visual = visual;
    update_selection(term);
}

static int
find_prompt(const struct terminal *term, int row, int direction)
{
    const int last = last_row(term);

    for (int r = row + direction; r >= 0 && r <= last; r += direction) {
        if (get_row(term, r)->shell_integration.prompt_marker)
            return r;
    }
    return -1;
}

static void
execute_action(struct seat *seat, struct terminal *term,
               enum bind_action_vi action, uint32_t serial)
{
    const int count = max(term->vimode.count, 1);
    term->vimode.count = 0;

    struct vipos p = cursor_pos(term);

    switch (action) {
    case BIND_ACTION_VI_NONE:
    case BIND_ACTION_VI_COUNT:
        return;

    case BIND_ACTION_VI_CANCEL:
        if (term->vimode.visual != VIMODE_VISUAL_NONE) {
            term->vimode.visual = VIMODE_VISUAL_NONE;
            selection_cancel(term);
        } else
            vimode_cancel(term);
        return;

    case BIND_ACTION_VI_EXIT:
        vimode_cancel(term);
        return;

    case BIND_ACTION_VI_LEFT:
        for (int i = 0; i < count && p.col > 0; i++) {
            do {
                p.col--;
            } while (p.col > 0 && is_spacer(term, &p));
        }
        break;

    case BIND_ACTION_VI_RIGHT:
        for (int i = 0; i < count; i++) {
            struct vipos q = p;
            do {
                q.col++;
            } while (q.col < term->cols && is_spacer(term, &q));

            if (q.col >= term->cols)
                break;
            p = q;
        }
        break;

    case BIND_ACTION_VI_UP:
        p.row -= count;
        break;

    case BIND_ACTION_VI_DOWN:
        p.row += count;
        break;

    case BIND_ACTION_VI_WORD_FORWARD:
    case BIND_ACTION_VI_BIG_WORD_FORWARD:
        for (int i = 0; i < count; i++)
            p = motion_word_forward(
                term, p, action == BIND_ACTION_VI_BIG_WORD_FORWARD);
        break;

    case BIND_ACTION_VI_WORD_BACKWARD:
    case BIND_ACTION_VI_BIG_WORD_BACKWARD:
        for (int i = 0; i < count; i++)
            p = motion_word_backward(
                term, p, action == BIND_ACTION_VI_BIG_WORD_BACKWARD);
        break;

    case BIND_ACTION_VI_WORD_END:
    case BIND_ACTION_VI_BIG_WORD_END:
        for (int i = 0; i < count; i++)
            p = motion_word_end(
                term, p, action == BIND_ACTION_VI_BIG_WORD_END);
        break;

    case BIND_ACTION_VI_LINE_START:
        p.col = 0;
        break;

    case BIND_ACTION_VI_LINE_FIRST_NON_BLANK:
        p.col = first_non_blank(term, p.row);
        break;

    case BIND_ACTION_VI_LINE_END:
        p.col = last_non_empty(term, p.row);
        break;

    case BIND_ACTION_VI_SCREEN_TOP:
        p.row = view_top(term);
        p.col = first_non_blank(term, p.row);
        break;

    case BIND_ACTION_VI_SCREEN_MIDDLE:
        p.row = view_top(term) + (term->rows - 1) / 2;
        p.col = first_non_blank(term, p.row);
        break;

    case BIND_ACTION_VI_SCREEN_BOTTOM:
        p.row = view_top(term) + term->rows - 1;
        p.col = first_non_blank(term, p.row);
        break;

    case BIND_ACTION_VI_SCROLLBACK_TOP:
        p.row = 0;
        p.col = first_non_blank(term, p.row);
        break;

    case BIND_ACTION_VI_SCROLLBACK_BOTTOM:
        p.row = last_row(term);
        p.col = first_non_blank(term, p.row);
        break;

    case BIND_ACTION_VI_PAGE_UP:
    case BIND_ACTION_VI_PAGE_DOWN:
    case BIND_ACTION_VI_HALF_PAGE_UP:
    case BIND_ACTION_VI_HALF_PAGE_DOWN: {
        const bool half = action == BIND_ACTION_VI_HALF_PAGE_UP ||
                          action == BIND_ACTION_VI_HALF_PAGE_DOWN;
        const bool up = action == BIND_ACTION_VI_PAGE_UP ||
                        action == BIND_ACTION_VI_HALF_PAGE_UP;
        const int rows = count * max(half ? term->rows / 2 : term->rows, 1);

        /* Move the view, and the cursor along with it */
        if (up)
            cmd_scrollback_up(term, rows);
        else
            cmd_scrollback_down(term, rows);

        p.row += up ? -rows : rows;
        break;
    }

    case BIND_ACTION_VI_SCROLL_UP:
        scroll_view(term, -count);
        return;

    case BIND_ACTION_VI_SCROLL_DOWN:
        scroll_view(term, count);
        return;

    case BIND_ACTION_VI_PROMPT_PREV:
    case BIND_ACTION_VI_PROMPT_NEXT: {
        const int direction = action == BIND_ACTION_VI_PROMPT_PREV ? -1 : 1;
        for (int i = 0; i < count; i++) {
            const int r = find_prompt(term, p.row, direction);
            if (r < 0)
                break;
            p = (struct vipos){r, 0};
        }
        break;
    }

    case BIND_ACTION_VI_VISUAL:
        toggle_visual(term, VIMODE_VISUAL_CHAR);
        return;

    case BIND_ACTION_VI_VISUAL_LINE:
        toggle_visual(term, VIMODE_VISUAL_LINE);
        return;

    case BIND_ACTION_VI_VISUAL_BLOCK:
        toggle_visual(term, VIMODE_VISUAL_BLOCK);
        return;

    case BIND_ACTION_VI_YANK:
        if (term->vimode.visual == VIMODE_VISUAL_NONE)
            return;

        selection_to_clipboard(seat, term, serial);
        term->vimode.visual = VIMODE_VISUAL_NONE;
        selection_cancel(term);
        return;
    }

    set_cursor(term, p);
}

void
vimode_begin(struct terminal *term)
{
    if (term->vimode.active)
        return;

    struct grid *grid = term->grid;
    const int mask = grid->num_rows - 1;

    /* Start at the terminal's cursor, if it is in the view */
    const int cursor_abs = grid_row_absolute(grid, grid->cursor.point.row);
    const int cursor_in_view = (cursor_abs - grid->view + grid->num_rows) & mask;

    struct coord start;
    if (cursor_in_view < term->rows)
        start = (struct coord){grid->cursor.point.col, cursor_abs};
    else
        start = (struct coord){0, (grid->view + term->rows - 1) & mask};

    LOG_DBG("vi mode: start at %dx%d", start.row, start.col);

    term->vimode.active = true;
    term->vimode.visual = VIMODE_VISUAL_NONE;
    term->vimode.count = 0;
    term->vimode.cursor = start;

    set_cursor(term, cursor_pos(term));
}

void
vimode_cancel(struct terminal *term)
{
    if (!term->vimode.active)
        return;

    if (term->vimode.visual != VIMODE_VISUAL_NONE)
        selection_cancel(term);

    dirty_cursor_cell(term);

    term->vimode.active = false;
    term->vimode.visual = VIMODE_VISUAL_NONE;
    term->vimode.count = 0;
    render_refresh(term);
}

struct coord
vimode_cursor_in_view(const struct terminal *term)
{
    if (!term->vimode.active)
        return (struct coord){-1, -1};

    const struct grid *grid = term->grid;
    const int row = (term->vimode.cursor.row - grid->view + grid->num_rows)
        & (grid->num_rows - 1);

    if (row >= term->rows)
        return (struct coord){-1, -1};

    return (struct coord){min(term->vimode.cursor.col, term->cols - 1), row};
}

void
vimode_input(struct seat *seat, struct terminal *term,
             const struct key_binding_set *bindings, uint32_t key,
             xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
             const xkb_keysym_t *raw_syms, size_t raw_count,
             uint32_t serial)
{
    /* Count prefix, e.g. '5j' */
    if ((mods & ~consumed) == 0 &&
        ((sym >= XKB_KEY_1 && sym <= XKB_KEY_9) ||
         (sym == XKB_KEY_0 && term->vimode.count > 0)))
    {
        term->vimode.count = min(
            term->vimode.count * 10 + (int)(sym - XKB_KEY_0), 99999);
        return;
    }

    /* Match untranslated symbols */
    tll_foreach(bindings->vi, it) {
        const struct key_binding *bind = &it->item;
        if (bind->mods != mods || bind->mods == 0)
            continue;

        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i]) {
                execute_action(seat, term, bind->action, serial);
                return;
            }
        }
    }

    /* Match translated symbol */
    tll_foreach(bindings->vi, it) {
        const struct key_binding *bind = &it->item;

        if (bind->k.sym == sym && bind->mods == (mods & ~consumed)) {
            execute_action(seat, term, bind->action, serial);
            return;
        }
    }

    /* Match raw key code */
    tll_foreach(bindings->vi, it) {
        const struct key_binding *bind = &it->item;
        if (bind->mods != mods || bind->mods == 0)
            continue;

        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key) {
                execute_action(seat, term, bind->action, serial);
                return;
            }
        }
    }

    /* Unbound keys are ignored; they're never sent to the application */
    term->vimode.count = 0;
}

#if defined(_DEBUG)
static struct vipos
test_cursor(const struct terminal *term)
{
    return cursor_pos(term);
}

#define xassert_cursor(term, r, c) do {                               \
        const struct vipos _p = test_cursor(term);                     \
        if (_p.row != (r) || _p.col != (c)) {                          \
            BUG("vi mode cursor at %d,%d, expected %d,%d",             \
                _p.row, _p.col, (r), (c));                             \
        }                                                              \
    } while (0)

static void
test_action(struct terminal *term, enum bind_action_vi action)
{
    execute_action(NULL, term, action, 1);
}

UNITTEST
{
    /* Motions */
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 4, 10, 16);

    /*
     * Rows (scrollback relative):
     *   0: "alpha beta"   (fills the row, hard line break)
     *   1: "x"
     *   2: "foo.bar(ba"   (soft wrapped...)
     *   3: "z)qux"        (...continues here)
     *   4: "  last"
     * Row 0 is in the scrollback, rows 1-4 on the screen.
     */
    term_test_write(&term, "alpha beta\r\nx\r\nfoo.bar(baz)qux\r\n  last");
    xassert(last_row(&term) == 4);
    xassert(!get_row(&term, 2)->linebreak);

    vimode_begin(&term);
    xassert(term.vimode.active);
    xassert_cursor(&term, 4, 6);

    /* The vi cursor is the one rendered */
    struct coord in_view = vimode_cursor_in_view(&term);
    xassert(in_view.row == 3 && in_view.col == 6);

    test_action(&term, BIND_ACTION_VI_SCROLLBACK_TOP);
    xassert_cursor(&term, 0, 0);
    xassert(view_top(&term) == 0);  /* View scrolled along */
    in_view = vimode_cursor_in_view(&term);
    xassert(in_view.row == 0 && in_view.col == 0);

    /* w: words, punctuation, hard line breaks and soft wraps */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 0, 6);  /* beta */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 1, 0);  /* x (row 0 is full, but hard broken) */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 2, 0);  /* foo */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 2, 3);  /* . */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 2, 4);  /* bar */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 2, 7);  /* ( */
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 2, 8);  /* baz */

    /* e: the word continues on the soft wrapped row */
    test_action(&term, BIND_ACTION_VI_WORD_END);
    xassert_cursor(&term, 3, 0);
    test_action(&term, BIND_ACTION_VI_WORD_BACKWARD);
    xassert_cursor(&term, 2, 8);

    /* WORDs */
    test_action(&term, BIND_ACTION_VI_LINE_START);
    xassert_cursor(&term, 2, 0);
    test_action(&term, BIND_ACTION_VI_BIG_WORD_END);
    xassert_cursor(&term, 3, 4);
    test_action(&term, BIND_ACTION_VI_LINE_START);
    test_action(&term, BIND_ACTION_VI_UP);
    test_action(&term, BIND_ACTION_VI_BIG_WORD_FORWARD);
    xassert_cursor(&term, 4, 2);  /* last */
    test_action(&term, BIND_ACTION_VI_BIG_WORD_BACKWARD);
    xassert_cursor(&term, 2, 0);

    /* Line motions */
    test_action(&term, BIND_ACTION_VI_LINE_END);
    xassert_cursor(&term, 2, 9);
    test_action(&term, BIND_ACTION_VI_RIGHT);
    xassert_cursor(&term, 2, 9);  /* Doesn't wrap */
    test_action(&term, BIND_ACTION_VI_LINE_START);
    test_action(&term, BIND_ACTION_VI_LEFT);
    xassert_cursor(&term, 2, 0);

    test_action(&term, BIND_ACTION_VI_SCROLLBACK_BOTTOM);
    xassert_cursor(&term, 4, 2);  /* First non-blank */
    test_action(&term, BIND_ACTION_VI_LINE_START);
    test_action(&term, BIND_ACTION_VI_LINE_FIRST_NON_BLANK);
    xassert_cursor(&term, 4, 2);
    test_action(&term, BIND_ACTION_VI_LINE_END);
    xassert_cursor(&term, 4, 5);

    /* Counts */
    term.vimode.count = 3;
    test_action(&term, BIND_ACTION_VI_LEFT);
    xassert_cursor(&term, 4, 2);
    xassert(term.vimode.count == 0);
    term.vimode.count = 2;
    test_action(&term, BIND_ACTION_VI_WORD_BACKWARD);
    xassert_cursor(&term, 3, 1);  /* qux, then ) */
    term.vimode.count = 99;
    test_action(&term, BIND_ACTION_VI_UP);
    xassert_cursor(&term, 0, 1);  /* Clamped */

    /* End of the scrollback */
    test_action(&term, BIND_ACTION_VI_SCROLLBACK_BOTTOM);
    test_action(&term, BIND_ACTION_VI_LINE_END);
    test_action(&term, BIND_ACTION_VI_WORD_FORWARD);
    xassert_cursor(&term, 4, 9);
    test_action(&term, BIND_ACTION_VI_DOWN);
    xassert_cursor(&term, 4, 9);

    /* Exiting */
    test_action(&term, BIND_ACTION_VI_EXIT);
    xassert(!term.vimode.active);
    in_view = vimode_cursor_in_view(&term);
    xassert(in_view.row < 0);

    term_test_destroy(&term);
}

UNITTEST
{
    /* Visual modes */
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 4, 10, 16);

    term_test_write(&term, "alpha beta\r\nx\r\nfoo.bar(baz)qux\r\n  last");
    const int row2 = rel_to_abs(&term, 2);
    const int row3 = rel_to_abs(&term, 3);

    vimode_begin(&term);
    set_cursor(&term, (struct vipos){2, 0});

    /* v + e: "foo" */
    test_action(&term, BIND_ACTION_VI_VISUAL);
    xassert(term.vimode.visual == VIMODE_VISUAL_CHAR);
    test_action(&term, BIND_ACTION_VI_WORD_END);
    xassert(term.selection.kind == SELECTION_CHAR_WISE);
    xassert(term.selection.coords.start.row == row2 && term.selection.coords.start.col == 0);
    xassert(term.selection.coords.end.row == row2 && term.selection.coords.end.col == 2);
    xassert(grid_row(term.grid, 1)->cells[2].attrs.selected);
    xassert(!grid_row(term.grid, 1)->cells[3].attrs.selected);

    char *text = selection_to_text(&term);
    xassert(streq(text, "foo"));
    free(text);

    /* Moving before the anchor */
    test_action(&term, BIND_ACTION_VI_UP);
    xassert(term.selection.coords.start.col == 2);
    xassert(term.selection.coords.end.row == row2 && term.selection.coords.end.col == 0);
    test_action(&term, BIND_ACTION_VI_DOWN);

    /* V: the whole logical (soft wrapped) line */
    test_action(&term, BIND_ACTION_VI_VISUAL_LINE);
    xassert(term.vimode.visual == VIMODE_VISUAL_LINE);
    xassert(term.selection.kind == SELECTION_LINE_WISE);
    xassert(term.selection.coords.start.row == row2 && term.selection.coords.start.col == 0);
    xassert(term.selection.coords.end.row == row3 && term.selection.coords.end.col == 9);

    text = selection_to_text(&term);
    xassert(strncmp(text, "foo.bar(baz)qux", 15) == 0);
    free(text);

    /* Control+v: a block between the anchor and the cursor */
    test_action(&term, BIND_ACTION_VI_DOWN);
    test_action(&term, BIND_ACTION_VI_VISUAL_BLOCK);
    xassert(term.selection.kind == SELECTION_BLOCK);
    xassert(term.selection.coords.start.row == row2 && term.selection.coords.start.col == 0);
    xassert(term.selection.coords.end.row == row3 && term.selection.coords.end.col == 2);
    xassert(grid_row(term.grid, 2)->cells[1].attrs.selected);
    xassert(!grid_row(term.grid, 2)->cells[3].attrs.selected);

    /* Same key again: back to normal mode, no selection */
    test_action(&term, BIND_ACTION_VI_VISUAL_BLOCK);
    xassert(term.vimode.visual == VIMODE_VISUAL_NONE);
    xassert(term.selection.coords.start.row < 0);
    xassert(!grid_row(term.grid, 2)->cells[1].attrs.selected);

    /* cancel: first leaves visual mode, then vi mode */
    test_action(&term, BIND_ACTION_VI_VISUAL);
    test_action(&term, BIND_ACTION_VI_CANCEL);
    xassert(term.vimode.active);
    xassert(term.vimode.visual == VIMODE_VISUAL_NONE);
    xassert(term.selection.coords.start.row < 0);
    test_action(&term, BIND_ACTION_VI_CANCEL);
    xassert(!term.vimode.active);

    /* yank (on empty cells, so that nothing is sent to the
     * non-existing compositor): back to normal mode, no selection */
    vimode_begin(&term);
    set_cursor(&term, (struct vipos){1, 5});
    test_action(&term, BIND_ACTION_VI_VISUAL);
    test_action(&term, BIND_ACTION_VI_RIGHT);
    xassert(term.selection.coords.end.row >= 0);
    test_action(&term, BIND_ACTION_VI_YANK);
    xassert(term.vimode.active);
    xassert(term.vimode.visual == VIMODE_VISUAL_NONE);
    xassert(term.selection.coords.start.row < 0);

    /* yank without a selection does nothing */
    test_action(&term, BIND_ACTION_VI_YANK);
    xassert(term.vimode.active);

    /* Leaving vi mode clears the visual selection */
    test_action(&term, BIND_ACTION_VI_VISUAL);
    test_action(&term, BIND_ACTION_VI_RIGHT);
    vimode_cancel(&term);
    xassert(term.selection.coords.start.row < 0);

    term_test_destroy(&term);
}

UNITTEST
{
    /* Scrolling, screen relative motions and prompts */
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 4, 6, 32);

    /* 20 rows: "r0" ... "r19"; prompts on rows 5 and 12 */
    for (int i = 0; i < 20; i++) {
        char line[16];
        snprintf(line, sizeof(line), i < 19 ? "r%d\r\n" : "r%d", i);
        term_test_write(&term, line);

        if (i == 5 || i == 12) {
            struct row *r = term.grid->rows[
                grid_row_absolute(term.grid, term.grid->cursor.point.row - 1)];
            r->shell_integration.prompt_marker = true;
        }
    }

    xassert(last_row(&term) == 19);
    xassert(get_row(&term, 5)->shell_integration.prompt_marker);

    vimode_begin(&term);
    xassert_cursor(&term, 19, 3);
    xassert(view_top(&term) == 16);

    /* H, M, L */
    test_action(&term, BIND_ACTION_VI_SCREEN_TOP);
    xassert_cursor(&term, 16, 0);
    test_action(&term, BIND_ACTION_VI_SCREEN_MIDDLE);
    xassert_cursor(&term, 17, 0);
    test_action(&term, BIND_ACTION_VI_SCREEN_BOTTOM);
    xassert_cursor(&term, 19, 0);

    /* Control+u: half a page, the view follows */
    test_action(&term, BIND_ACTION_VI_HALF_PAGE_UP);
    xassert_cursor(&term, 17, 0);
    xassert(view_top(&term) == 14);

    /* Control+b: a page */
    test_action(&term, BIND_ACTION_VI_PAGE_UP);
    xassert_cursor(&term, 13, 0);
    xassert(view_top(&term) == 10);

    /* Control+y: the view scrolls, the cursor stays (it is still in
     * view) */
    test_action(&term, BIND_ACTION_VI_SCROLL_UP);
    xassert(view_top(&term) == 9);
    xassert_cursor(&term, 12, 0);  /* Pushed up, it was on the bottom row */

    /* Control+e: scrolling the cursor out of the view moves it */
    term.vimode.count = 3;
    test_action(&term, BIND_ACTION_VI_SCROLL_DOWN);
    xassert(view_top(&term) == 12);
    xassert_cursor(&term, 12, 0);

    /* Control+f, Control+d: down again, never past the bottom */
    test_action(&term, BIND_ACTION_VI_PAGE_DOWN);
    xassert_cursor(&term, 16, 0);
    test_action(&term, BIND_ACTION_VI_HALF_PAGE_DOWN);
    xassert_cursor(&term, 18, 0);
    term.vimode.count = 5;
    test_action(&term, BIND_ACTION_VI_HALF_PAGE_DOWN);
    xassert_cursor(&term, 19, 0);
    xassert(view_top(&term) == 16);

    /* Prompts */
    test_action(&term, BIND_ACTION_VI_PROMPT_PREV);
    xassert_cursor(&term, 12, 0);
    xassert(view_top(&term) == 12);
    test_action(&term, BIND_ACTION_VI_PROMPT_PREV);
    xassert_cursor(&term, 5, 0);
    test_action(&term, BIND_ACTION_VI_PROMPT_PREV);
    xassert_cursor(&term, 5, 0);  /* No more prompts */
    test_action(&term, BIND_ACTION_VI_PROMPT_NEXT);
    xassert_cursor(&term, 12, 0);

    /* Starting vi mode while scrolled back, with the terminal cursor
     * out of view: start at the bottom of the view */
    vimode_cancel(&term);
    xassert(view_top(&term) != 16);
    vimode_begin(&term);
    xassert_cursor(&term, view_top(&term) + 3, 0);

    term_test_destroy(&term);
}
UNITTEST
{
    /* Key input: count prefix, and binding lookup */
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 4, 20, 16);
    term_test_write(&term, "a b c d e f g h");

    struct key_binding_set bindings = {.vi = tll_init()};
    tll_push_back(bindings.vi, ((struct key_binding){
        .type = KEY_BINDING, .action = BIND_ACTION_VI_WORD_BACKWARD,
        .k = {.sym = XKB_KEY_b}}));
    tll_push_back(bindings.vi, ((struct key_binding){
        .type = KEY_BINDING, .action = BIND_ACTION_VI_LINE_START,
        .k = {.sym = XKB_KEY_0}}));
    tll_push_back(bindings.vi, ((struct key_binding){
        .type = KEY_BINDING, .action = BIND_ACTION_VI_LINE_END,
        .k = {.sym = XKB_KEY_dollar}}));

    vimode_begin(&term);
    xassert_cursor(&term, 0, 15);

#define press(_sym) vimode_input(NULL, &term, &bindings, 0, _sym, 0, 0, NULL, 0, 1)

    /* "2b" */
    press(XKB_KEY_2);
    xassert(term.vimode.count == 2);
    press(XKB_KEY_b);
    xassert_cursor(&term, 0, 12);
    xassert(term.vimode.count == 0);

    /* "0" without a count is a motion... */
    press(XKB_KEY_0);
    xassert_cursor(&term, 0, 0);

    /* ...but part of the count after a digit: "10" */
    press(XKB_KEY_dollar);
    press(XKB_KEY_1);
    press(XKB_KEY_0);
    xassert(term.vimode.count == 10);
    xassert_cursor(&term, 0, 14);

    /* Unbound keys are swallowed, and reset the count */
    press(XKB_KEY_z);
    xassert(term.vimode.count == 0);
    xassert_cursor(&term, 0, 14);
    xassert(term.vimode.active);

    /* Modifiers prevent the count: Control+1 is not a digit */
    vimode_input(NULL, &term, &bindings, 0, XKB_KEY_1, 1 << 2, 0, NULL, 0, 1);
    xassert(term.vimode.count == 0);

#undef press

    tll_free(bindings.vi);
    vimode_cancel(&term);
    term_test_destroy(&term);
}
#endif /* _DEBUG */
