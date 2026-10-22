#include "search.h"

#include <regex.h>
#include <string.h>
#include <uchar.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "search"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "commands.h"
#include "config.h"
#include "extract.h"
#include "grid.h"
#include "input.h"
#include "key-binding.h"
#include "misc.h"
#include "quirks.h"
#include "render.h"
#include "selection.h"
#include "shm.h"
#include "unicode-mode.h"
#include "util.h"
#include "xmalloc.h"

/*
 * Ensures a "new" viewport doesn't contain any unallocated rows.
 *
 * This is done by first checking if the *first* row is NULL. If so,
 * we move the viewport *forward*, until the first row is non-NULL. At
 * this point, the entire viewport should be allocated rows only.
 *
 * If the first row already was non-NULL, we instead check the *last*
 * row, and if it is NULL, we move the viewport *backward* until the
 * last row is non-NULL.
 */
static int
ensure_view_is_allocated(struct terminal *term, int new_view)
{
    struct grid *grid = term->grid;
    int view_end = (new_view + term->rows - 1) & (grid->num_rows - 1);

    if (grid->rows[new_view] == NULL) {
        while (grid->rows[new_view] == NULL)
            new_view = (new_view + 1) & (grid->num_rows - 1);
    }

    else if (grid->rows[view_end] == NULL) {
        while (grid->rows[view_end] == NULL) {
            new_view--;
            if (new_view < 0)
                new_view += grid->num_rows;
            view_end = (new_view + term->rows - 1) & (grid->num_rows - 1);
        }
    }

#if defined(_DEBUG)
    for (size_t r = 0; r < term->rows; r++)
        xassert(grid->rows[(new_view + r) & (grid->num_rows - 1)] != NULL);
#endif

    return new_view;
}

static bool
search_ensure_size(struct terminal *term, size_t wanted_size)
{
    while (wanted_size >= term->search.sz) {
        size_t new_sz = term->search.sz == 0 ? 64 : term->search.sz * 2;
        char32_t *new_buf = realloc(term->search.buf, new_sz * sizeof(term->search.buf[0]));

        if (new_buf == NULL) {
            LOG_ERRNO("failed to resize search buffer");
            return false;
        }

        term->search.buf = new_buf;
        term->search.sz = new_sz;
    }

    return true;
}

static bool
has_wrapped_around_left(const struct terminal *term, int abs_row_no)
{
    int rebased_row = grid_row_abs_to_sb(term->grid, term->rows, abs_row_no);
    return rebased_row == term->grid->num_rows - 1 || term->grid->rows[abs_row_no] == NULL;
}

static bool
has_wrapped_around_right(const struct terminal *term, int abs_row_no)
{
    int rebased_row = grid_row_abs_to_sb(term->grid, term->rows, abs_row_no);
    return rebased_row == 0;
}

void
search_regex_free(struct terminal *term)
{
    if (term->search.regex.compiled)
        regfree(&term->search.regex.preg);
    free(term->search.regex.pattern);
    term->search.regex.pattern = NULL;
    term->search.regex.compiled = false;
}

static void
search_freeze(struct terminal *term)
{
    if (term->search.frozen)
        return;

    LOG_DBG("search: freezing client output");
    term_ptmx_pause(term);
    term->search.frozen = true;
}

static void
search_unfreeze(struct terminal *term)
{
    if (!term->search.frozen)
        return;

    LOG_DBG("search: un-freezing client output");
    term->search.frozen = false;

    /* An interactive resize resumes reading when it's done */
    if (term->interactive_resizing.grid == NULL)
        term_ptmx_resume(term);
}

static void
search_cancel_keep_selection(struct terminal *term)
{
    struct wl_window *win = term->window;
    wayl_win_subsurface_destroy(&win->search);

    if (term->search.len > 0) {
        free(term->search.last.buf);
        term->search.last.buf = term->search.buf;
        term->search.last.len = term->search.len;
    } else
        free(term->search.buf);

    term->search.buf = NULL;
    term->search.len = term->search.sz = 0;

    term->search.cursor = 0;
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    term->is_searching = false;
    term->render.search_glyph_offset = 0;

    search_regex_free(term);
    term->search.regex.enabled = false;
    search_unfreeze(term);

    /* Reset IME state */
    if (term_ime_is_enabled(term)) {
        term_ime_disable(term);
        term_ime_enable(term);
    }

    term_xcursor_update(term);
    render_refresh(term);
}

void
search_begin(struct terminal *term)
{
    LOG_DBG("search: begin");

    search_cancel_keep_selection(term);
    selection_cancel(term);

    /* Reset IME state */
    if (term_ime_is_enabled(term)) {
        term_ime_disable(term);
        term_ime_enable(term);
    }

    /* On-demand instantiate wayland surface */
    bool ret = wayl_win_subsurface_new(
        term->window, &term->window->search, false);
    xassert(ret);

    const struct grid *grid = term->grid;
    term->search.original_view = grid->view;
    term->search.view_followed_offset = grid->view == grid->offset;
    term->is_searching = true;

    term->search.len = 0;
    term->search.sz = 64;
    term->search.buf = xmalloc(term->search.sz * sizeof(term->search.buf[0]));
    term->search.buf[0] = U'\0';

    term->search.regex.enabled = term->conf->scrollback.search_regex;
    if (term->conf->scrollback.search_freeze)
        search_freeze(term);

    term_xcursor_update(term);
    render_refresh_search(term);
}

void
search_cancel(struct terminal *term)
{
    if (!term->is_searching)
        return;

    search_cancel_keep_selection(term);
    selection_cancel(term);
}

void
search_selection_cancelled(struct terminal *term)
{
    term->search.match = (struct coord){-1, -1};
    term->search.match_len = 0;
    render_refresh_search(term);
}

static void
search_update_selection(struct terminal *term, const struct range *match)
{
    struct grid *grid = term->grid;
    int start_row = match->start.row;
    int start_col = match->start.col;
    int end_row = match->end.row;
    int end_col = match->end.col;

    xassert(start_row >= 0);
    xassert(start_row < grid->num_rows);

    bool move_viewport = true;

    int view_end = (grid->view + term->rows - 1) & (grid->num_rows - 1);
    if (view_end >= grid->view) {
        /* Viewport does *not* wrap around */
        if (start_row >= grid->view && end_row <= view_end)
            move_viewport = false;
    } else {
        /* Viewport wraps */
        if (start_row >= grid->view || end_row <= view_end)
            move_viewport = false;
    }

    if (move_viewport) {
        int rebased_new_view = grid_row_abs_to_sb(grid, term->rows, start_row);

        rebased_new_view -= term->rows / 2;
        rebased_new_view =
            min(max(rebased_new_view, 0), grid->num_rows - term->rows);

        const int old_view = grid->view;
        int new_view = grid_row_sb_to_abs(grid, term->rows, rebased_new_view);

        /* Scrollback may not be completely filled yet */
        {
            const int mask = grid->num_rows - 1;
            while (grid->rows[new_view] == NULL)
                new_view = (new_view + 1) & mask;
        }

#if defined(_DEBUG)
        /* Verify all to-be-visible rows have been allocated */
        for (int r = 0; r < term->rows; r++)
            xassert(grid->rows[(new_view + r) & (grid->num_rows - 1)] != NULL);
#endif

#if defined(_DEBUG)
        {
            int rel_start_row = grid_row_abs_to_sb(grid, term->rows, start_row);
            int rel_view = grid_row_abs_to_sb(grid, term->rows, new_view);
            xassert(rel_view <= rel_start_row);
            xassert(rel_start_row < rel_view + term->rows);
        }
#endif

        /* Update view */
        grid->view = new_view;
        if (new_view != old_view)
            term_damage_view(term);
    }

    if (start_row != term->search.match.row ||
        start_col != term->search.match.col ||

        /* Pointer leave events trigger selection_finalize() :/ */
        !term->selection.ongoing)
    {
        int selection_row = start_row - grid->view + grid->num_rows;
        selection_row &= grid->num_rows - 1;

        selection_start(
            term, start_col, selection_row, SELECTION_CHAR_WISE, false);

        term->search.match.row = start_row;
        term->search.match.col = start_col;
    }

    /* Update selection endpoint */
    {
        int selection_row = end_row - grid->view + grid->num_rows;
        selection_row &= grid->num_rows - 1;
        selection_update(term, end_col, selection_row);
    }
}

static ssize_t
matches_cell(const struct terminal *term, const struct cell *cell, size_t search_ofs)
{
    assert(search_ofs < term->search.len);

    char32_t base = cell->wc;
    const struct composed *composed = NULL;

    if (base >= CELL_COMB_CHARS_LO && base <= CELL_COMB_CHARS_HI)
    {
        composed = composed_lookup(term->composed, base - CELL_COMB_CHARS_LO);
        base = composed->chars[0];
    }

    if (composed == NULL && base == 0 && term->search.buf[search_ofs] == U' ')
        return 1;

    if (hasc32upper(term->search.buf)) {
        if (c32ncmp(&base, &term->search.buf[search_ofs], 1) != 0)
            return -1;
    } else {
        if (c32ncasecmp(&base, &term->search.buf[search_ofs], 1) != 0)
            return -1;
    }

    if (composed != NULL) {
        if (search_ofs + composed->count > term->search.len)
            return -1;

        for (size_t j = 1; j < composed->count; j++) {
            if (composed->chars[j] != term->search.buf[search_ofs + j])
                return -1;
        }
    }

    return composed != NULL ? composed->count : 1;
}

/*
 * Regex search
 *
 * The scrollback is matched one logical line at a time; i.e. rows
 * joined across soft line wraps. Each line is converted to UTF-8,
 * with a map from byte offsets back to cells.
 */

struct regex_line {
    int first_sb_row;   /* Scrollback relative */
    int last_sb_row;    /* Scrollback relative */

    char *text;
    size_t len;
    size_t size;
    int *cells;         /* Cell index (row offset * cols + col) of each byte */
};

static bool
regex_compile(struct terminal *term)
{
    char *pattern = ac32tombs(term->search.buf);
    if (pattern == NULL)
        return false;

    if (term->search.regex.pattern != NULL &&
        streq(pattern, term->search.regex.pattern))
    {
        free(pattern);
        return term->search.regex.compiled;
    }

    search_regex_free(term);
    term->search.regex.pattern = pattern;

    /* Like the literal search: case sensitive only with upper case letters */
    int flags = REG_EXTENDED;
    if (!hasc32upper(term->search.buf))
        flags |= REG_ICASE;

    int r = regcomp(&term->search.regex.preg, pattern, flags);
    term->search.regex.compiled = r == 0;

    if (r != 0)
        LOG_DBG("search: invalid regex: %s", pattern);

    return term->search.regex.compiled;
}

static const struct row *
row_sb(const struct terminal *term, int sb_row)
{
    const struct grid *grid = term->grid;
    return grid->rows[grid_row_sb_to_abs(grid, term->rows, sb_row)];
}

static void
regex_line_append(struct regex_line *line, const char *bytes, size_t count,
                  int cell)
{
    if (line->len + count + 1 > line->size) {
        size_t new_size = line->size > 0 ? line->size * 2 : 256;
        while (new_size < line->len + count + 1)
            new_size *= 2;

        line->text = xrealloc(line->text, new_size);
        line->cells = xreallocarray(line->cells, new_size, sizeof(line->cells[0]));
        line->size = new_size;
    }

    for (size_t i = 0; i < count; i++) {
        line->text[line->len] = bytes[i];
        line->cells[line->len] = cell;
        line->len++;
    }
}

static void
regex_line_append_char(struct regex_line *line, char32_t wc, int cell)
{
    char utf8[MB_LEN_MAX];
    size_t count = c32rtomb(utf8, wc, &(mbstate_t){0});

    if (count == (size_t)-1) {
        utf8[0] = '?';
        count = 1;
    }

    regex_line_append(line, utf8, count, cell);
}

/* Extracts the logical line containing the scrollback relative row
 * 'sb_row'. Returns false if the row isn't allocated */
static bool
regex_line_extract(const struct terminal *term, int sb_row,
                   struct regex_line *line)
{
    const int last_sb = term->grid->num_rows - 1;

    if (row_sb(term, sb_row) == NULL)
        return false;

    int first = sb_row;
    while (first > 0) {
        const struct row *prev = row_sb(term, first - 1);
        if (prev == NULL || prev->linebreak)
            break;
        first--;
    }

    int last = sb_row;
    while (last < last_sb) {
        const struct row *row = row_sb(term, last);
        if (row->linebreak || row_sb(term, last + 1) == NULL)
            break;
        last++;
    }

    line->first_sb_row = first;
    line->last_sb_row = last;
    line->len = 0;

    /* Length (in bytes) up to, and including, the last non-empty cell */
    size_t content_len = 0;

    for (int r = first; r <= last; r++) {
        const struct row *row = row_sb(term, r);

        for (int c = 0; c < term->cols; c++) {
            const int cell_idx = (r - first) * term->cols + c;
            char32_t wc = row->cells[c].wc;

            if (wc >= CELL_SPACER)
                continue;

            if (wc == 0) {
                regex_line_append(line, " ", 1, cell_idx);
                continue;
            }

            if (wc >= CELL_COMB_CHARS_LO && wc <= CELL_COMB_CHARS_HI) {
                const struct composed *composed =
                    composed_lookup(term->composed, wc - CELL_COMB_CHARS_LO);
                for (size_t i = 0; i < composed->count; i++)
                    regex_line_append_char(line, composed->chars[i], cell_idx);
            } else
                regex_line_append_char(line, wc, cell_idx);

            content_len = line->len;
        }
    }

    /* Trailing empty cells aren't part of the line */
    line->len = content_len;

    if (line->size == 0)
        regex_line_append(line, "", 0, 0);
    line->text[line->len] = '\0';
    return true;
}

static int
regex_line_pos(const struct terminal *term, const struct regex_line *line,
               size_t byte_ofs)
{
    /* Linear, scrollback relative, cell position */
    return line->first_sb_row * term->cols + line->cells[byte_ofs];
}

/*
 * Finds the match in 'line' whose start is closest to 'start' (in
 * 'direction'), within 'limit' cells. 'start' and the returned
 * position are linear, scrollback relative, cell positions.
 *
 * Returns the distance of the best match, or -1 if there's no match.
 */
static long
regex_line_best_match(const struct terminal *term, const regex_t *preg,
                      const struct regex_line *line, bool backward,
                      long start, long limit, long *match_start,
                      long *match_end)
{
    const long total = (long)term->grid->num_rows * term->cols;
    long best = -1;

    for (size_t ofs = 0; ofs <= line->len;) {
        regmatch_t m;
        if (regexec(preg, &line->text[ofs], 1, &m,
                    ofs > 0 ? REG_NOTBOL : 0) != 0)
        {
            break;
        }

        const size_t so = ofs + m.rm_so;
        const size_t eo = ofs + m.rm_eo;

        if (eo == so) {
            /* Ignore empty matches */
            ofs = so + 1;
            continue;
        }

        const long pos = regex_line_pos(term, line, so);
        const long dist = backward
            ? (start - pos + total) % total
            : (pos - start + total) % total;

        if (dist <= limit && (best < 0 || dist < best)) {
            best = dist;
            *match_start = pos;
            *match_end = regex_line_pos(term, line, eo - 1);
        }

        ofs = eo;
    }

    return best;
}

static bool
find_next_regex(struct terminal *term, enum search_direction direction,
                struct coord abs_start, struct coord abs_end,
                struct range *match)
{
    if (!regex_compile(term))
        return false;

    const struct grid *grid = term->grid;
    const regex_t *preg = &term->search.regex.preg;
    const bool backward = direction != SEARCH_FORWARD;
    const int cols = term->cols;
    const long total = (long)grid->num_rows * cols;

    const long start =
        (long)grid_row_abs_to_sb(grid, term->rows, abs_start.row) * cols +
        abs_start.col;
    const long end =
        (long)grid_row_abs_to_sb(grid, term->rows, abs_end.row) * cols +
        abs_end.col;
    const long limit = backward
        ? (start - end + total) % total
        : (end - start + total) % total;

    struct regex_line line = {0};
    long found_start = -1, found_end = -1;

    /*
     * Matches in the line where the search starts, but on the "wrong"
     * side of 'start', come last, after having wrapped around the
     * scrollback.
     */
    long wrapped_start = -1, wrapped_end = -1;

    int sb_row = start / cols;
    int rows_left = grid->num_rows;

    for (bool first_line = true; rows_left > 0; first_line = false) {
        if (!regex_line_extract(term, sb_row, &line)) {
            rows_left--;
            sb_row = backward
                ? (sb_row == 0 ? grid->num_rows - 1 : sb_row - 1)
                : (sb_row == grid->num_rows - 1 ? 0 : sb_row + 1);
            continue;
        }

        const long line_first = (long)line.first_sb_row * cols;
        const long line_last = (long)line.last_sb_row * cols + cols - 1;

        if (!first_line) {
            /* Stop when the entire line is outside the search range */
            const long nearest = backward
                ? (start - line_last + total) % total
                : (line_first - start + total) % total;
            if (nearest > limit)
                break;
        }

        long s, e;
        if (first_line) {
            /* Matches on the right side of 'start' */
            const long side_limit = backward
                ? start - line_first
                : line_last - start;

            if (regex_line_best_match(
                    term, preg, &line, backward, start,
                    min(side_limit, limit), &s, &e) >= 0)
            {
                found_start = s;
                found_end = e;
                break;
            }

            /* Matches on the wrong side, i.e. after wrapping around */
            if (regex_line_best_match(
                    term, preg, &line, backward, start, limit, &s, &e) >= 0)
            {
                wrapped_start = s;
                wrapped_end = e;
            }
        } else if (regex_line_best_match(
                       term, preg, &line, backward, start, limit, &s, &e) >= 0)
        {
            found_start = s;
            found_end = e;
            break;
        }

        rows_left -= line.last_sb_row - line.first_sb_row + 1;
        sb_row = backward
            ? (line.first_sb_row == 0 ? grid->num_rows - 1 : line.first_sb_row - 1)
            : (line.last_sb_row == grid->num_rows - 1 ? 0 : line.last_sb_row + 1);
    }

    free(line.text);
    free(line.cells);

    if (found_start < 0) {
        found_start = wrapped_start;
        found_end = wrapped_end;
    }

    if (found_start < 0)
        return false;

    struct coord match_start = {
        .col = found_start % cols,
        .row = grid_row_sb_to_abs(grid, term->rows, found_start / cols),
    };
    struct coord match_end = {
        .col = found_end % cols,
        .row = grid_row_sb_to_abs(grid, term->rows, found_end / cols),
    };

    /* Include the remaining cells of a double width character */
    const struct row *row = grid->rows[match_end.row];
    while (match_end.col + 1 < cols &&
           row->cells[match_end.col + 1].wc > CELL_SPACER)
    {
        match_end.col++;
    }

    *match = (struct range){.start = match_start, .end = match_end};
    return true;
}

/* Escapes regex special characters, for text added to the search
 * string from the grid (e.g. extend-char) */
static char32_t *
regex_escape(const char32_t *text, size_t len, size_t *new_len)
{
    char32_t *escaped = xmalloc((len * 2 + 1) * sizeof(escaped[0]));
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        if (c32chr(U".[]{}()\\*+?^$|", text[i]) != NULL)
            escaped[j++] = U'\\';
        escaped[j++] = text[i];
    }

    escaped[j] = U'\0';
    *new_len = j;
    return escaped;
}

static bool
find_next(struct terminal *term, enum search_direction direction,
          struct coord abs_start, struct coord abs_end, struct range *match)
{
    if (term->search.regex.enabled)
        return find_next_regex(term, direction, abs_start, abs_end, match);

#define ROW_DEC(_r) ((_r) = ((_r) - 1 + grid->num_rows) & (grid->num_rows - 1))
#define ROW_INC(_r) ((_r) = ((_r) + 1) & (grid->num_rows - 1))

    struct grid *grid = term->grid;
    const bool backward = direction != SEARCH_FORWARD;

    LOG_DBG("%s: start: %dx%d, end: %dx%d", backward ? "backward" : "forward",
            abs_start.row, abs_start.col, abs_end.row, abs_end.col);

    xassert(abs_start.row >= 0);
    xassert(abs_start.row < grid->num_rows);
    xassert(abs_start.col >= 0);
    xassert(abs_start.col < term->cols);

    xassert(abs_end.row >= 0);
    xassert(abs_end.row < grid->num_rows);
    xassert(abs_end.col >= 0);
    xassert(abs_end.col < term->cols);

    for (int match_start_row = abs_start.row, match_start_col = abs_start.col;
         ;
         backward ? ROW_DEC(match_start_row) : ROW_INC(match_start_row)) {

        const struct row *row = grid->rows[match_start_row];
        if (row == NULL) {
            if (match_start_row == abs_end.row)
                break;
            continue;
        }

        for (;
             backward ? match_start_col >= 0 : match_start_col < term->cols;
             backward ? match_start_col-- : match_start_col++)
        {
            if (matches_cell(term, &row->cells[match_start_col], 0) < 0) {
                if (match_start_row == abs_end.row &&
                    match_start_col == abs_end.col)
                {
                    break;
                }
                continue;
            }

            /*
             * Got a match on the first letter. Now we'll see if the
             * rest of the search buffer matches.
             */

            LOG_DBG("search: initial match at row=%d, col=%d",
                    match_start_row, match_start_col);

            int match_end_row = match_start_row;
            int match_end_col = match_start_col;
            const struct row *match_row = row;
            size_t match_len = 0;

            for (size_t i = 0; i < term->search.len;) {
                if (match_end_col >= term->cols) {
                    ROW_INC(match_end_row);
                    match_end_col = 0;

                    match_row = grid->rows[match_end_row];
                    if (match_row == NULL)
                        break;
                }

                if (match_row->cells[match_end_col].wc >= CELL_SPACER) {
                    match_end_col++;
                    continue;
                }

                ssize_t additional_chars = matches_cell(
                    term, &match_row->cells[match_end_col], i);
                if (additional_chars < 0)
                    break;

                i += additional_chars;
                match_len += additional_chars;
                match_end_col++;

                while (match_end_col < term->cols &&
                       match_row->cells[match_end_col].wc > CELL_SPACER)
                {
                    match_end_col++;
                }
            }

            if (match_len != term->search.len) {
                /* Didn't match (completely) */

                if (match_start_row == abs_end.row &&
                    match_start_col == abs_end.col)
                {
                    break;
                }

                continue;
            }

            *match = (struct range){
                .start = {match_start_col, match_start_row},
                .end = {match_end_col - 1, match_end_row},
            };

            return true;
        }

        if (match_start_row == abs_end.row && match_start_col == abs_end.col)
            break;

        match_start_col = backward ? term->cols - 1 : 0;
    }

    return false;
}

void
search_find_next(struct terminal *term, enum search_direction direction)
{
    struct grid *grid = term->grid;

    if (term->search.len == 0) {
        term->search.match = (struct coord){-1, -1};
        term->search.match_len = 0;
        selection_cancel(term);
        return;
    }

    struct coord start = term->search.match;
    size_t len = term->search.match_len;

    xassert((len == 0 && start.row == -1 && start.col == -1) ||
           (len > 0 && start.row >= 0 && start.col >= 0));

    if (len == 0) {
        /* No previous match, start from the top, or bottom, of the scrollback */
        switch (direction) {
        case SEARCH_FORWARD:
            start.row = grid_row_absolute_in_view(grid, 0);
            start.col = 0;
            break;

        case SEARCH_BACKWARD:
        case SEARCH_BACKWARD_SAME_POSITION:
            start.row = grid_row_absolute_in_view(grid, term->rows - 1);
            start.col = term->cols - 1;
            break;
        }
    } else {
        /* Continue from last match */
        xassert(start.row >= 0);
        xassert(start.col >= 0);

        switch (direction) {
        case SEARCH_BACKWARD_SAME_POSITION:
            break;

        case SEARCH_BACKWARD:
            if (--start.col < 0) {
                start.col = term->cols - 1;
                start.row += grid->num_rows - 1;
                start.row &= grid->num_rows - 1;
            }
            break;

        case SEARCH_FORWARD:
            if (++start.col >= term->cols) {
                start.col = 0;
                start.row++;
                start.row &= grid->num_rows - 1;
            }
            break;
        }

        xassert(start.row >= 0);
        xassert(start.row < grid->num_rows);
        xassert(start.col >= 0);
        xassert(start.col < term->cols);
    }

    LOG_DBG(
        "update: %s: starting at row=%d col=%d "
        "(offset = %d, view = %d)",
        direction != SEARCH_FORWARD ? "backward" : "forward",
        start.row, start.col,
        grid->offset, grid->view);

    struct coord end = start;
    switch (direction) {
    case SEARCH_FORWARD:
        /* Search forward, until we reach the cell *before* current start */
        if (--end.col < 0) {
            end.col = term->cols - 1;
            end.row += grid->num_rows - 1;
            end.row &= grid->num_rows - 1;
        }
        break;

    case SEARCH_BACKWARD:
    case SEARCH_BACKWARD_SAME_POSITION:
        /* Search backwards, until we reach the cell *after* current start */
        if (++end.col >= term->cols) {
            end.col = 0;
            end.row++;
            end.row &= grid->num_rows - 1;
        }
        break;
    }

    struct range match;
    bool found = find_next(term, direction, start, end, &match);

    if (found) {
        LOG_DBG("primary match found at %dx%d",
                match.start.row, match.start.col);
        search_update_selection(term, &match);
        term->search.match = match.start;
        term->search.match_len = term->search.len;
    } else {
        LOG_DBG("no match");
        term->search.match = (struct coord){-1, -1};
        term->search.match_len = 0;
        selection_cancel(term);
    }
#undef ROW_DEC
}

struct search_match_iterator
search_matches_new_iter(struct terminal *term)
{
    return (struct search_match_iterator){
        .term = term,
        .start = {0, 0},
    };
}

struct range
search_matches_next(struct search_match_iterator *iter)
{
    struct terminal *term = iter->term;
    struct grid *grid = term->grid;

    if (term->search.match_len == 0)
        goto no_match;

    if (iter->start.row >= term->rows)
        goto no_match;

    xassert(iter->start.row >= 0);
    xassert(iter->start.row < term->rows);
    xassert(iter->start.col >= 0);
    xassert(iter->start.col < term->cols);

    struct coord abs_start = iter->start;
    abs_start.row = grid_row_absolute_in_view(grid, abs_start.row);

    struct coord abs_end = {
        term->cols - 1,
        grid_row_absolute_in_view(grid, term->rows - 1)};

    /* BUG: matches *starting* outside the view, but ending *inside*, aren't matched */
    struct range match;
    bool found = find_next(term, SEARCH_FORWARD, abs_start, abs_end, &match);
    if (!found)
        goto no_match;

    LOG_DBG("match at (absolute coordinates) %dx%d-%dx%d",
            match.start.row, match.start.col,
            match.end.row, match.end.col);

    /* Convert absolute row numbers back to view relative */
    match.start.row = match.start.row - grid->view + grid->num_rows;
    match.start.row &= grid->num_rows - 1;
    match.end.row = match.end.row - grid->view + grid->num_rows;
    match.end.row &= grid->num_rows - 1;

    LOG_DBG("match at (view-local coordinates) %dx%d-%dx%d, view=%d",
            match.start.row, match.start.col,
            match.end.row, match.end.col, grid->view);

    /* Assert match end comes *after* the match start */
    xassert(match.end.row > match.start.row ||
            (match.end.row == match.start.row &&
             match.end.col >= match.start.col));

    /* Assert the match starts at, or after, the iterator position */
    xassert(match.start.row > iter->start.row ||
            (match.start.row == iter->start.row &&
             match.start.col >= iter->start.col));

    /* Continue at next column, next time */
    iter->start.row = match.start.row;
    iter->start.col = match.start.col + 1;

    if (iter->start.col >= term->cols) {
        iter->start.col = 0;
        iter->start.row++;  /* Overflow is caught in next iteration */
    }

    xassert(iter->start.row >= 0);
    xassert(iter->start.row <= term->rows);
    xassert(iter->start.col >= 0);
    xassert(iter->start.col < term->cols);
    return match;

no_match:
    iter->start.row = -1;
    iter->start.col = -1;
    return (struct range){{-1, -1}, {-1,  -1}};
}

static void
add_wchars(struct terminal *term, char32_t *src, size_t count)
{
    /* Strip non-printable characters */
    for (size_t i = 0, j = 0, orig_count = count; i < orig_count; i++) {
        if (isc32print(src[i]))
            src[j++] = src[i];
        else
            count--;
    }

    if (!search_ensure_size(term, term->search.len + count))
        return;

    xassert(term->search.len + count < term->search.sz);

    memmove(&term->search.buf[term->search.cursor + count],
            &term->search.buf[term->search.cursor],
            (term->search.len - term->search.cursor) * sizeof(char32_t));

    memcpy(&term->search.buf[term->search.cursor], src, count * sizeof(char32_t));

    term->search.len += count;
    term->search.cursor += count;
    term->search.buf[term->search.len] = U'\0';
}

void
search_add_chars(struct terminal *term, const char *src, size_t count)
{
    size_t chars = mbsntoc32(NULL, src, count, 0);
    if (chars == (size_t)-1) {
        LOG_ERRNO("failed to convert %.*s to Unicode", (int)count, src);
        return;
    }

    char32_t c32s[chars + 1];
    mbsntoc32(c32s, src, count, chars);
    add_wchars(term, c32s, chars);
}

enum extend_direction {SEARCH_EXTEND_LEFT, SEARCH_EXTEND_RIGHT};

static bool
coord_advance_left(const struct terminal *term, struct coord *pos,
                   const struct row **row)
{
    const struct grid *grid = term->grid;
    struct coord new_pos = *pos;

    if (--new_pos.col < 0) {
        new_pos.row = (new_pos.row - 1 + grid->num_rows) & (grid->num_rows - 1);
        new_pos.col = term->cols - 1;

        if (has_wrapped_around_left(term, new_pos.row))
            return false;

        if (row != NULL)
            *row = grid->rows[new_pos.row];
    }

    *pos = new_pos;
    return true;
}

static bool
coord_advance_right(const struct terminal *term, struct coord *pos,
                    const struct row **row)
{
    const struct grid *grid = term->grid;
    struct coord new_pos = *pos;

    if (++new_pos.col >= term->cols) {
        new_pos.row = (new_pos.row + 1) & (grid->num_rows - 1);
        new_pos.col = 0;

        if (has_wrapped_around_right(term, new_pos.row))
            return false;

        if (row != NULL)
            *row = grid->rows[new_pos.row];
    }

    *pos = new_pos;
    return true;
}

static bool
search_extend_find_char(const struct terminal *term, struct coord *target,
                        enum extend_direction direction)
{
    if (term->search.match_len == 0)
        return false;

    struct coord pos = direction == SEARCH_EXTEND_LEFT
        ? selection_get_start(term) : selection_get_end(term);
    xassert(pos.row >= 0);
    xassert(pos.row < term->grid->num_rows);

    *target = pos;

    const struct row *row = term->grid->rows[pos.row];

    while (true) {
        switch (direction) {
        case SEARCH_EXTEND_LEFT:
            if (!coord_advance_left(term, &pos, &row))
                return false;
            break;

        case SEARCH_EXTEND_RIGHT:
            if (!coord_advance_right(term, &pos, &row))
                return false;
            break;
        }

        const char32_t wc = row->cells[pos.col].wc;

        if (wc >= CELL_SPACER || wc == U'\0')
            continue;

        *target = pos;
        return true;
    }
}

static bool
search_extend_find_char_left(const struct terminal *term, struct coord *target)
{
    return search_extend_find_char(term, target, SEARCH_EXTEND_LEFT);
}

static bool
search_extend_find_char_right(const struct terminal *term, struct coord *target)
{
    return search_extend_find_char(term, target, SEARCH_EXTEND_RIGHT);
}

static bool
search_extend_find_word(const struct terminal *term, bool spaces_only,
                        struct coord *target, enum extend_direction direction)
{
    if (term->search.match_len == 0)
        return false;

    struct grid *grid = term->grid;
    struct coord pos = direction == SEARCH_EXTEND_LEFT
        ? selection_get_start(term)
        : selection_get_end(term);

    xassert(pos.row >= 0);
    xassert(pos.row < grid->num_rows);

    *target = pos;

    /* First character to consider is the *next* character */
    switch (direction) {
    case SEARCH_EXTEND_LEFT:
        if (!coord_advance_left(term, &pos, NULL))
            return false;
        break;

    case SEARCH_EXTEND_RIGHT:
        if (!coord_advance_right(term, &pos, NULL))
            return false;
        break;
    }

    xassert(pos.row >= 0);
    xassert(pos.row < grid->num_rows);
    xassert(grid->rows[pos.row] != NULL);

    /* Find next word boundary */
    switch (direction) {
    case SEARCH_EXTEND_LEFT:
        selection_find_word_boundary_left(term, &pos, spaces_only);
        break;

    case SEARCH_EXTEND_RIGHT:
        selection_find_word_boundary_right(term, &pos, spaces_only, false);
        break;
    }

    *target = pos;
    return true;
}

static bool
search_extend_find_word_left(const struct terminal *term, bool spaces_only,
                             struct coord *target)
{
    return search_extend_find_word(term, spaces_only, target, SEARCH_EXTEND_LEFT);
}

static bool
search_extend_find_word_right(const struct terminal *term, bool spaces_only,
                              struct coord *target)
{
    return search_extend_find_word(term, spaces_only, target, SEARCH_EXTEND_RIGHT);
}

static bool
search_extend_find_line(const struct terminal *term, struct coord *target,
                        enum extend_direction direction)
{
    if (term->search.match_len == 0)
        return false;

    struct coord pos = direction == SEARCH_EXTEND_LEFT
        ? selection_get_start(term) : selection_get_end(term);

    xassert(pos.row >= 0);
    xassert(pos.row < term->grid->num_rows);

    *target = pos;

    const struct grid *grid = term->grid;

    switch (direction) {
    case SEARCH_EXTEND_LEFT:
        pos.row = (pos.row - 1 + grid->num_rows) & (grid->num_rows - 1);
        if (has_wrapped_around_left(term, pos.row))
            return false;
        break;

    case SEARCH_EXTEND_RIGHT:
        pos.row = (pos.row + 1) & (grid->num_rows - 1);
        if (has_wrapped_around_right(term, pos.row))
            return false;
        break;
    }

    *target = pos;
    return true;
}

static bool
search_extend_find_line_up(const struct terminal *term, struct coord *target)
{
    return search_extend_find_line(term, target, SEARCH_EXTEND_LEFT);
}

static bool
search_extend_find_line_down(const struct terminal *term, struct coord *target)
{
    return search_extend_find_line(term, target, SEARCH_EXTEND_RIGHT);
}

static void
search_extend_left(struct terminal *term, const struct coord *target)
{
    if (term->search.match_len == 0)
        return;

    const struct coord last_coord = selection_get_start(term);
    struct coord pos = *target;
    const struct row *row = term->grid->rows[pos.row];

    const bool move_cursor = term->search.cursor != 0;

    struct extraction_context *ctx = extract_begin(SELECTION_NONE, false);
    if (ctx == NULL)
        return;

    while (pos.col != last_coord.col || pos.row != last_coord.row) {
        if (!extract_one(term, row, &row->cells[pos.col], pos.col, ctx))
            break;
        if (!coord_advance_right(term, &pos, &row))
            break;
    }

    char32_t *new_text;
    size_t new_len;

    if (!extract_finish_wide(ctx, &new_text, &new_len))
        return;

    if (term->search.regex.enabled) {
        char32_t *escaped = regex_escape(new_text, new_len, &new_len);
        free(new_text);
        new_text = escaped;
    }

    if (!search_ensure_size(term, term->search.len + new_len))
        return;

    memmove(&term->search.buf[new_len], &term->search.buf[0],
            term->search.len * sizeof(term->search.buf[0]));

    size_t actually_copied = 0;
    for (size_t i = 0; i < new_len; i++) {
        if (new_text[i] == U'\n') {
            /* extract() adds newlines, which we never match against */
            continue;
        }

        term->search.buf[actually_copied++] = new_text[i];
        term->search.len++;
    }

    xassert(actually_copied <= new_len);
    if (actually_copied < new_len) {
        memmove(
            &term->search.buf[actually_copied], &term->search.buf[new_len],
            (term->search.len - actually_copied) * sizeof(term->search.buf[0]));
    }

    term->search.buf[term->search.len] = U'\0';
    free(new_text);

    if (move_cursor)
        term->search.cursor += actually_copied;

    struct range match = {.start = *target, .end = selection_get_end(term)};
    search_update_selection(term, &match);

    term->search.match_len = term->search.len;
}

static void
search_extend_right(struct terminal *term, const struct coord *target)
{
    if (term->search.match_len == 0)
        return;

    struct coord pos = selection_get_end(term);
    const struct row *row = term->grid->rows[pos.row];

    const bool move_cursor = term->search.cursor == term->search.len;

    struct extraction_context *ctx = extract_begin(SELECTION_NONE, false);
    if (ctx == NULL)
        return;

    do {
        if (!coord_advance_right(term, &pos, &row))
            break;
        if (!extract_one(term, row, &row->cells[pos.col], pos.col, ctx))
            break;
    } while (pos.col != target->col || pos.row != target->row);

    char32_t *new_text;
    size_t new_len;

    if (!extract_finish_wide(ctx, &new_text, &new_len))
        return;

    if (term->search.regex.enabled) {
        char32_t *escaped = regex_escape(new_text, new_len, &new_len);
        free(new_text);
        new_text = escaped;
    }

    if (!search_ensure_size(term, term->search.len + new_len))
        return;

    for (size_t i = 0; i < new_len; i++) {
        if (new_text[i] == U'\n') {
            /* extract() adds newlines, which we never match against */
            continue;
        }

        term->search.buf[term->search.len++] = new_text[i];
    }

    term->search.buf[term->search.len] = U'\0';
    free(new_text);

    if (move_cursor)
        term->search.cursor = term->search.len;

    struct range match = {.start = term->search.match, .end = *target};
    search_update_selection(term, &match);
    term->search.match_len = term->search.len;
}

static size_t
distance_next_word(const struct terminal *term)
{
    size_t cursor = term->search.cursor;

    /* First eat non-whitespace. This is the word we're skipping past */
    while (cursor < term->search.len) {
        if (isc32space(term->search.buf[cursor++]))
            break;
    }

    xassert(cursor == term->search.len || isc32space(term->search.buf[cursor - 1]));

    /* Now skip past whitespace, so that we end up at the beginning of
     * the next word */
    while (cursor < term->search.len) {
        if (!isc32space(term->search.buf[cursor++]))
            break;
    }

    xassert(cursor == term->search.len || !isc32space(term->search.buf[cursor - 1]));

    if (cursor < term->search.len && !isc32space(term->search.buf[cursor]))
        cursor--;

    return cursor - term->search.cursor;
}

static size_t
distance_prev_word(const struct terminal *term)
{
    int cursor = term->search.cursor;

    /* First, eat whitespace prefix */
    while (cursor > 0) {
        if (!isc32space(term->search.buf[--cursor]))
            break;
    }

    xassert(cursor == 0 || !isc32space(term->search.buf[cursor]));

    /* Now eat non-whitespace. This is the word we're skipping past */
    while (cursor > 0) {
        if (isc32space(term->search.buf[--cursor]))
            break;
    }

    xassert(cursor == 0 || isc32space(term->search.buf[cursor]));
    if (cursor > 0 && isc32space(term->search.buf[cursor]))
        cursor++;

    return term->search.cursor - cursor;
}

static void
from_clipboard_cb(char *text, size_t size, void *user)
{
    struct terminal *term = user;
    search_add_chars(term, text, size);
}

static void
from_clipboard_done(void *user)
{
    struct terminal *term = user;

    LOG_DBG("search: buffer: %ls", (const wchar_t *)term->search.buf);
    search_find_next(term, SEARCH_BACKWARD_SAME_POSITION);
    render_refresh_search(term);
}

static bool
execute_binding(struct seat *seat, struct terminal *term,
                const struct key_binding *binding, uint32_t serial,
                bool *update_search_result, enum search_direction *direction,
                bool *redraw)
{
    *update_search_result = *redraw = false;
    const enum bind_action_search action = binding->action;

    struct grid *grid = term->grid;

    switch (action) {
    case BIND_ACTION_SEARCH_NONE:
        return false;

    case BIND_ACTION_SEARCH_SCROLLBACK_UP_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, term->rows);
            return true;
        }
        return false;

    case BIND_ACTION_SEARCH_SCROLLBACK_UP_HALF_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, max(term->rows / 2, 1));
            return true;
        }
        break;

    case BIND_ACTION_SEARCH_SCROLLBACK_UP_LINE:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, 1);
            return true;
        }
        break;

    case BIND_ACTION_SEARCH_SCROLLBACK_DOWN_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, term->rows);
            return true;
        }
        return false;

    case BIND_ACTION_SEARCH_SCROLLBACK_DOWN_HALF_PAGE:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, max(term->rows / 2, 1));
            return true;
        }
        break;

    case BIND_ACTION_SEARCH_SCROLLBACK_DOWN_LINE:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, 1);
            return true;
        }
        break;

    case BIND_ACTION_SEARCH_SCROLLBACK_HOME:
        if (term->grid == &term->normal) {
            cmd_scrollback_up(term, term->grid->num_rows);
            return true;
        }
        break;

    case BIND_ACTION_SEARCH_SCROLLBACK_END:
        if (term->grid == &term->normal) {
            cmd_scrollback_down(term, term->grid->num_rows);
            return true;
        }
        break;

    case BIND_ACTION_SEARCH_CANCEL:
        if (term->search.view_followed_offset)
            grid->view = grid->offset;
        else {
            grid->view = ensure_view_is_allocated(
                term, term->search.original_view);
        }
        term_damage_view(term);
        search_cancel(term);
        return true;

    case BIND_ACTION_SEARCH_COMMIT:
        selection_finalize(seat, term, serial);
        search_cancel_keep_selection(term);
        return true;

    case BIND_ACTION_SEARCH_FIND_PREV:
        if (term->search.last.buf != NULL && term->search.len == 0) {
            add_wchars(term, term->search.last.buf, term->search.last.len);

            free(term->search.last.buf);
            term->search.last.buf = NULL;
            term->search.last.len = 0;
        }

        *direction = SEARCH_BACKWARD;
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_FIND_NEXT:
        if (term->search.last.buf != NULL && term->search.len == 0) {
            add_wchars(term, term->search.last.buf, term->search.last.len);

            free(term->search.last.buf);
            term->search.last.buf = NULL;
            term->search.last.len = 0;
        }

        *direction = SEARCH_FORWARD;
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_EDIT_LEFT:
        if (term->search.cursor > 0) {
            term->search.cursor--;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_EDIT_LEFT_WORD: {
        size_t diff = distance_prev_word(term);
        term->search.cursor -= diff;
        xassert(term->search.cursor <= term->search.len);

        if (diff > 0)
            *redraw = true;
        return true;
    }

    case BIND_ACTION_SEARCH_EDIT_RIGHT:
        if (term->search.cursor < term->search.len) {
            term->search.cursor++;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_EDIT_RIGHT_WORD: {
        size_t diff = distance_next_word(term);
        term->search.cursor += diff;
        xassert(term->search.cursor <= term->search.len);

        if (diff > 0)
            *redraw = true;
        return true;
    }

    case BIND_ACTION_SEARCH_EDIT_HOME:
        if (term->search.cursor != 0) {
            term->search.cursor = 0;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_EDIT_END:
        if (term->search.cursor != term->search.len) {
            term->search.cursor = term->search.len;
            *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_PREV:
        if (term->search.cursor > 0) {
            memmove(
                &term->search.buf[term->search.cursor - 1],
                &term->search.buf[term->search.cursor],
                (term->search.len - term->search.cursor) * sizeof(char32_t));
            term->search.cursor--;
            term->search.buf[--term->search.len] = U'\0';
            *update_search_result = *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_PREV_WORD: {
        size_t diff = distance_prev_word(term);
        size_t old_cursor = term->search.cursor;
        size_t new_cursor = old_cursor - diff;

        if (diff > 0) {
            memmove(&term->search.buf[new_cursor],
                    &term->search.buf[old_cursor],
                    (term->search.len - old_cursor) * sizeof(char32_t));

            term->search.len -= diff;
            term->search.cursor = new_cursor;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_DELETE_NEXT:
        if (term->search.cursor < term->search.len) {
            memmove(
                &term->search.buf[term->search.cursor],
                &term->search.buf[term->search.cursor + 1],
                (term->search.len - term->search.cursor - 1) * sizeof(char32_t));
            term->search.buf[--term->search.len] = U'\0';
            *update_search_result = *redraw = true;
        }
        return true;

    case BIND_ACTION_SEARCH_DELETE_NEXT_WORD: {
        size_t diff = distance_next_word(term);
        size_t cursor = term->search.cursor;

        if (diff > 0) {
            memmove(&term->search.buf[cursor],
                    &term->search.buf[cursor + diff],
                    (term->search.len - (cursor + diff)) * sizeof(char32_t));

            term->search.len -= diff;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_DELETE_TO_START: {
        if (term->search.cursor > 0) {
            memmove(&term->search.buf[0],
                    &term->search.buf[term->search.cursor],
                    (term->search.len - term->search.cursor)
                        * sizeof(char32_t));

            term->search.len -= term->search.cursor;
            term->search.cursor = 0;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_DELETE_TO_END: {
        if (term->search.cursor < term->search.len) {
            term->search.buf[term->search.cursor] = '\0';
            term->search.len = term->search.cursor;
            *update_search_result = *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_CHAR: {
        struct coord target;
        if (search_extend_find_char_right(term, &target)) {
            search_extend_right(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_WORD: {
        struct coord target;
        if (search_extend_find_word_right(term, false, &target)) {
            search_extend_right(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_WORD_WS: {
        struct coord target;
        if (search_extend_find_word_right(term, true, &target)) {
            search_extend_right(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_LINE_DOWN: {
        struct coord target;
        if (search_extend_find_line_down(term, &target)) {
            search_extend_right(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_BACKWARD_CHAR: {
        struct coord target;
        if (search_extend_find_char_left(term, &target)) {
            search_extend_left(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_BACKWARD_WORD: {
        struct coord target;
        if (search_extend_find_word_left(term, false, &target)) {
            search_extend_left(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_BACKWARD_WORD_WS: {
        struct coord target;
        if (search_extend_find_word_left(term, true, &target)) {
            search_extend_left(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_EXTEND_LINE_UP: {
        struct coord target;
        if (search_extend_find_line_up(term, &target)) {
            search_extend_left(term, &target);
            *update_search_result = false;
            *redraw = true;
        }
        return true;
    }

    case BIND_ACTION_SEARCH_CLIPBOARD_PASTE:
        text_from_clipboard(
            seat, term, false, &from_clipboard_cb, &from_clipboard_done, term);
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_PRIMARY_PASTE:
        text_from_primary(
            seat, term, false, &from_clipboard_cb, &from_clipboard_done, term);
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_UNICODE_INPUT:
        unicode_mode_activate(term);
        return true;

    case BIND_ACTION_SEARCH_TOGGLE_REGEX:
        term->search.regex.enabled = !term->search.regex.enabled;
        *update_search_result = *redraw = true;
        return true;

    case BIND_ACTION_SEARCH_COUNT:
        BUG("Invalid action type");
        return true;
    }

    BUG("Unhandled action type");
    return false;
}

void
search_input(struct seat *seat, struct terminal *term,
             const struct key_binding_set *bindings, uint32_t key,
             xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
             const xkb_keysym_t *raw_syms, size_t raw_count,
             uint32_t serial)
{
    LOG_DBG("search: input: sym=%d/0x%x, mods=0x%08x, consumed=0x%08x",
            sym, sym, mods, consumed);

    enum xkb_compose_status compose_status = seat->kbd.xkb_compose_state != NULL
      ? xkb_compose_state_get_status(seat->kbd.xkb_compose_state)
      : XKB_COMPOSE_NOTHING;

    enum search_direction search_direction = SEARCH_BACKWARD_SAME_POSITION;
    bool update_search_result = false;
    bool redraw = false;

    /*
     * Key bindings
     */

    /* Match untranslated symbols */
    tll_foreach(bindings->search, it) {
        const struct key_binding *bind = &it->item;

        if (bind->mods != mods || bind->mods == 0)
            continue;

        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i]) {
                if (execute_binding(seat, term, bind, serial,
                                    &update_search_result, &search_direction,
                                    &redraw))
                {
                    seat->kbd.last_shortcut_sym = sym;
                    goto update_search;
                }
                return;
            }
        }
    }

    /* Match translated symbol */
    tll_foreach(bindings->search, it) {
        const struct key_binding *bind = &it->item;

        if (bind->k.sym == sym &&
            bind->mods == (mods & ~consumed)) {

            if (execute_binding(seat, term, bind, serial,
                                &update_search_result, &search_direction,
                                &redraw))
            {
                seat->kbd.last_shortcut_sym = sym;
                goto update_search;
            }
            return;
        }
    }

    /* Match raw key code */
    tll_foreach(bindings->search, it) {
        const struct key_binding *bind = &it->item;

        if (bind->mods != mods || bind->mods == 0)
            continue;

        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key) {
                if (execute_binding(seat, term, bind, serial,
                                    &update_search_result, &search_direction,
                                    &redraw))
                {
                    seat->kbd.last_shortcut_sym = sym;
                    goto update_search;
                }
                return;
            }
        }
    }

    uint8_t buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(
            seat->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);
    } else if (compose_status == XKB_COMPOSE_CANCELLED ||
               compose_status == XKB_COMPOSE_COMPOSING) {
        count = 0;
    } else {
        count = xkb_state_key_get_utf8(
            seat->kbd.xkb_state, key, (char *)buf, sizeof(buf));
    }

    update_search_result = redraw = count > 0;
    search_direction = SEARCH_BACKWARD_SAME_POSITION;

    if (count == 0)
        return;

    search_add_chars(term, (const char *)buf, count);

update_search:
    LOG_DBG("search: buffer: %ls", (const wchar_t *)term->search.buf);
    if (update_search_result)
        search_find_next(term, search_direction);
    if (redraw)
        render_refresh_search(term);
}

#if defined(_DEBUG)
#include <fcntl.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/timerfd.h>

static void
test_set_search(struct terminal *term, const char *text)
{
    free(term->search.buf);

    term->search.len = strlen(text);
    term->search.sz = term->search.len + 1;
    term->search.buf = xmalloc(term->search.sz * sizeof(term->search.buf[0]));

    for (size_t i = 0; i < term->search.len; i++)
        term->search.buf[i] = (unsigned char)text[i];
    term->search.buf[term->search.len] = U'\0';
}

/* Searches the entire scrollback, starting at (row, col), like
 * search_find_next() does */
static bool
test_find(struct terminal *term, enum search_direction direction,
          int row, int col, struct range *match)
{
    const int mask = term->grid->num_rows - 1;
    struct coord start = {col, row};
    struct coord end = start;

    if (direction == SEARCH_FORWARD) {
        if (--end.col < 0) {
            end.col = term->cols - 1;
            end.row = (end.row - 1) & mask;
        }
    } else {
        if (++end.col >= term->cols) {
            end.col = 0;
            end.row = (end.row + 1) & mask;
        }
    }

    return find_next(term, direction, start, end, match);
}

static bool
test_match_is(const struct range *match,
              int start_row, int start_col, int end_row, int end_col)
{
    return match->start.row == start_row && match->start.col == start_col &&
           match->end.row == end_row && match->end.col == end_col;
}

UNITTEST
{
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 6, 10, 8);

    /* Row 1 soft-wraps into row 2 */
    term_test_write(&term, "foo bar\r\n");
    term_test_write(&term, "ip-10-0-10-127.ec2\r\n");
    term_test_write(&term, "Foo baz\r\n");
    term_test_write(&term, "x1 x22");

    term.search.regex.enabled = true;
    struct range m;

    /* Matches span soft-wrapped rows */
    test_set_search(&term, "ip.*ec2");
    xassert(test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(test_match_is(&m, 1, 0, 2, 7));

    /* Smart case: case insensitive without upper case letters... */
    test_set_search(&term, "^foo");
    xassert(test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(test_match_is(&m, 3, 0, 3, 2));

    /* ...and case sensitive with */
    test_set_search(&term, "FOO");
    xassert(!test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    test_set_search(&term, "Foo b");
    xassert(test_find(&term, SEARCH_FORWARD, 0, 0, &m));
    xassert(test_match_is(&m, 3, 0, 3, 4));

    /* Forward, continuing after the previous match, wrapping around */
    test_set_search(&term, "ba.");
    xassert(test_find(&term, SEARCH_FORWARD, 0, 0, &m));
    xassert(test_match_is(&m, 0, 4, 0, 6));
    xassert(test_find(&term, SEARCH_FORWARD, 0, 5, &m));
    xassert(test_match_is(&m, 3, 4, 3, 6));
    xassert(test_find(&term, SEARCH_FORWARD, 3, 5, &m));
    xassert(test_match_is(&m, 0, 4, 0, 6));

    /* Backward, skipping a match after the start position on the
     * same row, and wrapping around */
    xassert(test_find(&term, SEARCH_BACKWARD, 3, 3, &m));
    xassert(test_match_is(&m, 0, 4, 0, 6));
    xassert(test_find(&term, SEARCH_BACKWARD, 0, 3, &m));
    xassert(test_match_is(&m, 3, 4, 3, 6));

    /* Backward, from the same position */
    xassert(test_find(&term, SEARCH_BACKWARD_SAME_POSITION, 3, 4, &m));
    xassert(test_match_is(&m, 3, 4, 3, 6));

    /* Line anchors; trailing empty cells aren't part of a line */
    test_set_search(&term, "z$");
    xassert(test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(test_match_is(&m, 3, 6, 3, 6));
    test_set_search(&term, "^x1 x22$");
    xassert(test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(test_match_is(&m, 4, 0, 4, 5));

    /* A hard line break isn't matched across */
    test_set_search(&term, "bar.*ip");
    xassert(!test_find(&term, SEARCH_BACKWARD, 5, 9, &m));

    /* Empty matches are ignored */
    test_set_search(&term, "z*");
    xassert(test_find(&term, SEARCH_FORWARD, 0, 0, &m));
    xassert(test_match_is(&m, 3, 6, 3, 6));

    /* Invalid regex doesn't match, and is re-compiled when changed */
    test_set_search(&term, "(");
    xassert(!test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(!term.search.regex.compiled);
    test_set_search(&term, "(baz)");
    xassert(test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(term.search.regex.compiled);
    xassert(test_match_is(&m, 3, 4, 3, 6));

    /* Limited range, like when highlighting matches in the view */
    test_set_search(&term, "ba.");
    xassert(find_next(&term, SEARCH_FORWARD,
                      (struct coord){0, 0}, (struct coord){9, 2}, &m));
    xassert(test_match_is(&m, 0, 4, 0, 6));
    xassert(!find_next(&term, SEARCH_FORWARD,
                       (struct coord){5, 0}, (struct coord){9, 2}, &m));

    /* Literal search is unaffected */
    term.search.regex.enabled = false;
    test_set_search(&term, "ba.");
    xassert(!test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    test_set_search(&term, "10-127");
    xassert(test_find(&term, SEARCH_BACKWARD, 5, 9, &m));
    xassert(test_match_is(&m, 1, 8, 2, 3));

    search_regex_free(&term);
    xassert(term.search.regex.pattern == NULL);
    free(term.search.buf);
    term_test_destroy(&term);
}

UNITTEST
{
    size_t len;
    char32_t *escaped = regex_escape(U"a.b*c(d)|e", 10, &len);
    xassert(len == 15);
    xassert(c32cmp(escaped, U"a\\.b\\*c\\(d\\)\\|e") == 0);
    free(escaped);
}

static bool
test_fd_handler(struct fdm *fdm, int fd, int events, void *data)
{
    char buf[16];
    ssize_t count = read(fd, buf, sizeof(buf));
    (void)count;
    (*(int *)data)++;
    return true;
}

UNITTEST
{
    /* search-freeze: client output isn't read while searching */
    struct config conf = {0};
    struct terminal term;
    term_test_init(&term, &conf, 2, 10, 2);

    struct fdm *fdm = fdm_init();
    xassert(fdm != NULL);

    int fds[2];
    xassert(pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);

    int reads = 0;
    xassert(fdm_add(fdm, fds[0], EPOLLIN, &test_fd_handler, &reads));
    term.fdm = fdm;
    term.ptmx = fds[0];

    /* A timer, so that fdm_poll() never blocks for long */
    int timer_fires = 0;
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    xassert(timer_fd >= 0);
    xassert(fdm_add(fdm, timer_fd, EPOLLIN, &test_fd_handler, &timer_fires));

    const struct itimerspec timeout = {
        .it_interval = {.tv_nsec = 20 * 1000000},
        .it_value = {.tv_nsec = 20 * 1000000},
    };
    xassert(timerfd_settime(timer_fd, 0, &timeout, NULL) == 0);

    search_freeze(&term);
    xassert(term.search.frozen);

    /* Resuming (e.g. after an interactive resize) is ignored */
    term_ptmx_resume(&term);

    xassert(write(fds[1], "x", 1) == 1);
    for (int i = 0; i < 3; i++)
        xassert(fdm_poll(fdm));
    xassert(reads == 0);

    search_unfreeze(&term);
    xassert(!term.search.frozen);

    while (reads == 0)
        xassert(fdm_poll(fdm));
    xassert(reads == 1);

    fdm_del(fdm, timer_fd);
    fdm_del(fdm, fds[0]);
    close(fds[1]);
    fdm_destroy(fdm);
    term_test_destroy(&term);
}
#endif
