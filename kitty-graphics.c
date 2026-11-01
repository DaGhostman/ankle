#include "kitty-graphics.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>

#include <png.h>
#include <zlib.h>

#define LOG_MODULE "kitty-graphics"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "composed.h"
#include "config.h"
#include "debug.h"
#include "fdm.h"
#include "grid.h"
#include "kitty-diacritics.h"
#include "render.h"
#include "srgb.h"
#include "util.h"
#include "xmalloc.h"
#include "xsnprintf.h"

/* Defined in wayland.c (stubbed in the PGO helper) */
bool wayl_do_linear_blending(const struct wayland *wayl, const struct config *conf);

#define KITTY_DEFAULT_FRAME_GAP 40
#define KITTY_MAX_RELATIVE_DEPTH 8

/*
 * Parsed control data
 */

enum {
    KEY_a, KEY_q, KEY_e, KEY_f, KEY_t, KEY_s, KEY_v, KEY_S, KEY_O, KEY_i,
    KEY_I, KEY_p, KEY_o, KEY_m, KEY_N, KEY_x, KEY_y, KEY_w, KEY_h, KEY_X,
    KEY_Y, KEY_c, KEY_r, KEY_C, KEY_U, KEY_z, KEY_P, KEY_Q, KEY_H, KEY_V,
    KEY_d, KEY_COUNT,
};

static const char key_names[KEY_COUNT] = {
    [KEY_a] = 'a', [KEY_q] = 'q', [KEY_e] = 'e', [KEY_f] = 'f',
    [KEY_t] = 't', [KEY_s] = 's', [KEY_v] = 'v', [KEY_S] = 'S',
    [KEY_O] = 'O', [KEY_i] = 'i', [KEY_I] = 'I', [KEY_p] = 'p',
    [KEY_o] = 'o', [KEY_m] = 'm', [KEY_N] = 'N', [KEY_x] = 'x',
    [KEY_y] = 'y', [KEY_w] = 'w', [KEY_h] = 'h', [KEY_X] = 'X',
    [KEY_Y] = 'Y', [KEY_c] = 'c', [KEY_r] = 'r', [KEY_C] = 'C',
    [KEY_U] = 'U', [KEY_z] = 'z', [KEY_P] = 'P', [KEY_Q] = 'Q',
    [KEY_H] = 'H', [KEY_V] = 'V', [KEY_d] = 'd',
};

/* Keys whose value is a single character */
static bool
key_is_char(int key)
{
    return key == KEY_a || key == KEY_d || key == KEY_t || key == KEY_o ||
           key == KEY_e;
}

/* Keys whose value is a signed integer */
static bool
key_is_signed(int key)
{
    return key == KEY_z || key == KEY_H || key == KEY_V;
}

struct kitty_cmd {
    uint32_t v[KEY_COUNT];   /* Unsigned values, and chars */
    int32_t sv[KEY_COUNT];   /* Signed values */
    uint64_t present;
};

static inline bool
has(const struct kitty_cmd *cmd, int key)
{
    return (cmd->present & (1ull << key)) != 0;
}

static inline uint32_t
val(const struct kitty_cmd *cmd, int key)
{
    return cmd->v[key];
}

static inline int32_t
sval(const struct kitty_cmd *cmd, int key)
{
    return cmd->sv[key];
}

static char
action(const struct kitty_cmd *cmd)
{
    return has(cmd, KEY_a) ? (char)val(cmd, KEY_a) : 't';
}

static int
key_from_char(char c)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        if (key_names[i] == c)
            return i;
    }
    return -1;
}

/*
 * Parses "k=v,k=v,..." up to (but not including) the payload
 * separator. Returns false on malformed input.
 */
static bool
parse_control(const char *s, size_t len, struct kitty_cmd *cmd)
{
    memset(cmd, 0, sizeof(*cmd));

    size_t i = 0;
    while (i < len) {
        if (s[i] == ',') {
            i++;
            continue;
        }

        if (i + 2 > len || s[i + 1] != '=')
            return false;

        const int key = key_from_char(s[i]);
        i += 2;

        size_t start = i;
        while (i < len && s[i] != ',')
            i++;
        const size_t vlen = i - start;

        if (key < 0) {
            /* Unknown key; ignore */
            continue;
        }

        if (key_is_char(key)) {
            if (vlen != 1)
                return false;
            cmd->v[key] = (unsigned char)s[start];
        } else {
            if (vlen == 0 || vlen > 11)
                return false;

            bool negative = false;
            size_t j = start;
            if (s[j] == '-') {
                if (!key_is_signed(key))
                    return false;
                negative = true;
                j++;
            }
            if (j == i)
                return false;

            uint64_t value = 0;
            for (; j < i; j++) {
                if (s[j] < '0' || s[j] > '9')
                    return false;
                value = value * 10 + (s[j] - '0');
                if (value > UINT32_MAX)
                    return false;
            }

            if (key_is_signed(key)) {
                int64_t sv = negative ? -(int64_t)value : (int64_t)value;
                if (sv < INT32_MIN || sv > INT32_MAX)
                    return false;
                cmd->sv[key] = (int32_t)sv;
                cmd->v[key] = (uint32_t)sv;
            } else
                cmd->v[key] = (uint32_t)value;
        }

        cmd->present |= 1ull << key;
    }

    return true;
}

/*
 * Base64, tolerant of missing padding and data split across chunks
 */

static int
b64_value(uint8_t c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

struct kbuf {
    uint8_t *data;
    size_t len;
    size_t size;
};

static bool
buf_reserve(struct kbuf *buf, size_t extra, size_t max)
{
    if (buf->len + extra > max)
        return false;

    if (buf->len + extra <= buf->size)
        return true;

    size_t new_size = buf->size == 0 ? 4096 : buf->size;
    while (new_size < buf->len + extra)
        new_size *= 2;
    new_size = min(new_size, max);

    buf->data = xrealloc(buf->data, new_size);
    buf->size = new_size;
    return true;
}

/* Decodes base64 'src', appending to 'out'. 'quad'/'quad_len' carry
 * partial groups between calls */
static bool
b64_decode_append(const uint8_t *src, size_t len, struct kbuf *out,
                  uint8_t quad[static 4], size_t *quad_len, size_t max)
{
    if (!buf_reserve(out, (len / 4 + 1) * 3, max))
        return false;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '=') {
            /* Padding ends the group. Some clients (e.g. chafa)
             * base64 encode each chunk separately, i.e. there may
             * be padding in the middle of the data */
            switch (*quad_len) {
            case 2:
                out->data[out->len++] = quad[0] << 2 | quad[1] >> 4;
                break;
            case 3:
                out->data[out->len++] = quad[0] << 2 | quad[1] >> 4;
                out->data[out->len++] = quad[1] << 4 | quad[2] >> 2;
                break;
            }
            *quad_len = 0;
            continue;
        }

        const int v = b64_value(src[i]);
        if (v < 0)
            continue;  /* Whitespace etc */

        quad[(*quad_len)++] = v;
        if (*quad_len == 4) {
            out->data[out->len++] = quad[0] << 2 | quad[1] >> 4;
            out->data[out->len++] = quad[1] << 4 | quad[2] >> 2;
            out->data[out->len++] = quad[2] << 6 | quad[3];
            *quad_len = 0;
        }
    }

    return true;
}

static bool
b64_decode_finish(struct kbuf *out, uint8_t quad[static 4], size_t *quad_len,
                  size_t max)
{
    if (!buf_reserve(out, 3, max))
        return false;

    switch (*quad_len) {
    case 0:
        break;
    case 1:
        /* Invalid; ignore the stray character */
        break;
    case 2:
        out->data[out->len++] = quad[0] << 2 | quad[1] >> 4;
        break;
    case 3:
        out->data[out->len++] = quad[0] << 2 | quad[1] >> 4;
        out->data[out->len++] = quad[1] << 4 | quad[2] >> 2;
        break;
    }

    *quad_len = 0;
    return true;
}

/*
 * State
 */

static size_t
storage_limit(const struct terminal *term)
{
    size_t limit = term->conf->tweak.kitty_graphics_storage_limit;
    return limit > 0
        ? limit
        : (size_t)KITTY_DEFAULT_STORAGE_LIMIT_MB * 1024 * 1024;
}

static struct kitty_graphics *
state(struct terminal *term)
{
    if (term->kitty_gfx == NULL) {
        term->kitty_gfx = xcalloc(1, sizeof(*term->kitty_gfx));
        term->kitty_gfx->anim_fd = -1;
        term->kitty_gfx->next_id = 1u << 31;
    }
    return term->kitty_gfx;
}

static struct grid *
grids(struct terminal *term, size_t idx)
{
    switch (idx) {
    case 0: return &term->normal;
    case 1: return &term->alt;
    case 2: return term->interactive_resizing.grid;
    default: return NULL;
    }
}

#define foreach_grid(term, g)                                           \
    for (size_t _gi = 0; _gi < 3; _gi++)                                \
        for (struct grid *g = grids(term, _gi); g != NULL; g = NULL)

static void
frame_free(struct kitty_frame *frame)
{
    if (frame->pix != NULL)
        pixman_image_unref(frame->pix);
    free(frame->data);
    frame->pix = NULL;
    frame->data = NULL;
}

static bool
frame_init(struct kitty_frame *frame, int width, int height, uint32_t *data)
{
    frame->data = data;
    frame->pix = pixman_image_create_bits_no_clear(
        PIXMAN_a8r8g8b8, width, height, data, width * sizeof(uint32_t));
    if (frame->pix == NULL) {
        free(data);
        frame->data = NULL;
        return false;
    }
    return true;
}

static void
virtual_free(struct kitty_virtual *v)
{
    if (v->cache != NULL)
        pixman_image_unref(v->cache);
    v->cache = NULL;
}

static void
placement_free(struct kitty_placement *p)
{
    if (p->cache != NULL)
        pixman_image_unref(p->cache);
    free(p);
}

/* Frees a placement that has already been removed from its grid,
 * along with its relative children (which are still in the grid) */
void
kitty_placement_destroy_unlinked(struct grid *grid, struct kitty_placement *p)
{
    bool found;
    do {
        found = false;
        tll_foreach(grid->kitty_placements, it) {
            if (it->item->parent != p)
                continue;

            struct kitty_placement *child = it->item;
            tll_remove(grid->kitty_placements, it);
            kitty_placement_destroy_unlinked(grid, child);
            found = true;
            break;
        }
    } while (found);

    placement_free(p);
}

/*
 * Geometry
 */

struct geometry {
    int cols;          /* Cells covered */
    int rows;
    int width;         /* Displayed image size, in pixels */
    int height;
};

static void
placement_geometry(const struct terminal *term, const struct kitty_placement *p,
                   struct geometry *g)
{
    const int cw = max(term->cell_width, 1);
    const int ch = max(term->cell_height, 1);
    const int sw = max(p->src_w, 1);
    const int sh = max(p->src_h, 1);

    int cols = p->req_cols;
    int rows = p->req_rows;

    if (cols == 0 && rows == 0) {
        g->width = sw;
        g->height = sh;
        g->cols = (p->x_off + sw + cw - 1) / cw;
        g->rows = (p->y_off + sh + ch - 1) / ch;
        return;
    }

    if (cols == 0) {
        double height_px = (double)ch * rows + p->y_off;
        double width_px = height_px * sw / sh;
        cols = max(1, (int)((width_px + cw - 1) / cw));
    } else if (rows == 0) {
        double width_px = (double)cw * cols + p->x_off;
        double height_px = width_px * sh / sw;
        rows = max(1, (int)((height_px + ch - 1) / ch));
    }

    const int box_w = max(1, cols * cw - p->x_off);
    const int box_h = max(1, rows * ch - p->y_off);

    if (p->req_cols != 0 && p->req_rows != 0) {
        /* Both given: fit, preserving the aspect ratio */
        double scale = min((double)box_w / sw, (double)box_h / sh);
        g->width = max(1, (int)(sw * scale + 0.5));
        g->height = max(1, (int)(sh * scale + 0.5));
    } else {
        g->width = box_w;
        g->height = box_h;
    }

    g->cols = cols;
    g->rows = rows;
}

int
kitty_placement_rows(const struct terminal *term, const struct kitty_placement *p)
{
    struct geometry g;
    placement_geometry(term, p, &g);
    return g.rows;
}

/*
 * Damage
 */

/* Screen (view-relative) row of an absolute grid row; may be negative,
 * or >= term->rows */
static int
view_row(const struct terminal *term, const struct grid *grid, int abs_row)
{
    const int sb = grid_row_abs_to_sb(grid, term->rows, abs_row & (grid->num_rows - 1));
    const int view_sb = grid_row_abs_to_sb(grid, term->rows, grid->view);
    return sb - view_sb;
}

static bool
placement_position(const struct terminal *term, const struct grid *grid,
                   const struct kitty_placement *p, int *row, int *col,
                   int depth);

static void
damage_screen_rect(struct terminal *term, int row0, int col0, int rows, int cols)
{
    for (int r = max(row0, 0); r < min(row0 + rows, term->rows); r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        if (row == NULL)
            continue;
        row->dirty = true;
        for (int c = max(col0, 0); c < min(col0 + cols, term->cols); c++)
            row->cells[c].attrs.clean = 0;
    }
}

static void
damage_placement(struct terminal *term, struct grid *grid,
                 const struct kitty_placement *p)
{
    if (grid != term->grid)
        return;

    int row, col;
    if (!placement_position(term, grid, p, &row, &col, 0))
        return;

    struct geometry g;
    placement_geometry(term, p, &g);
    damage_screen_rect(term, row, col, g.rows, g.cols);
}

/* Dirty all visible placeholder cells */
static void
damage_placeholders(struct terminal *term)
{
    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        if (row == NULL)
            continue;
        for (int c = 0; c < term->cols; c++) {
            if (kitty_is_placeholder(term, &row->cells[c])) {
                row->cells[c].attrs.clean = 0;
                row->dirty = true;
            }
        }
    }
}

static void
damage_image(struct terminal *term, const struct kitty_image *img)
{
    foreach_grid(term, grid) {
        tll_foreach(grid->kitty_placements, it) {
            if (it->item->img == img)
                damage_placement(term, grid, it->item);
        }
    }

    if (tll_length(img->virtuals) > 0)
        damage_placeholders(term);
}

/*
 * Placements
 */

static void
placement_unlink(struct terminal *term, struct grid *grid,
                 struct kitty_placement *p, int depth)
{
    damage_placement(term, grid, p);

    if (depth >= 32)
        return;

    /* Relative placements die with their parent */
    bool found;
    do {
        found = false;
        tll_foreach(grid->kitty_placements, it) {
            if (it->item->parent != p)
                continue;

            struct kitty_placement *child = it->item;
            tll_remove(grid->kitty_placements, it);
            placement_unlink(term, grid, child, depth + 1);
            placement_free(child);
            found = true;
            break;
        }
    } while (found);
}

/* Removes all placements (in the grid) matching 'pred' */
#define remove_placements_if(term, grid, pred)                          \
    do {                                                                \
        bool _changed;                                                  \
        do {                                                            \
            _changed = false;                                           \
            tll_foreach((grid)->kitty_placements, it) {                 \
                struct kitty_placement *p = it->item;                   \
                if (pred) {                                             \
                    tll_remove((grid)->kitty_placements, it);           \
                    placement_unlink(term, grid, p, 0);                 \
                    placement_free(p);                                  \
                    _changed = true;                                    \
                    break;                                              \
                }                                                       \
            }                                                           \
        } while (_changed);                                             \
    } while (0)

static void
image_destroy(struct terminal *term, struct kitty_image *img)
{
    struct kitty_graphics *kg = term->kitty_gfx;

    foreach_grid(term, grid) {
        remove_placements_if(term, grid, p->img == img);
        remove_placements_if(term, grid, p->parent_virtual_img == img);
    }

    if (tll_length(img->virtuals) > 0) {
        damage_placeholders(term);
        kg->virtual_count -= tll_length(img->virtuals);
    }

    tll_foreach(img->virtuals, it) {
        virtual_free(&it->item);
        tll_remove(img->virtuals, it);
    }

    for (size_t i = 0; i < img->frame_count; i++)
        frame_free(&img->frames[i]);
    free(img->frames);

    xassert(kg->used_bytes >= img->bytes);
    kg->used_bytes -= img->bytes;

    tll_foreach(kg->images, it) {
        if (it->item == img) {
            tll_remove(kg->images, it);
            break;
        }
    }

    free(img);
}

static struct kitty_image *
image_by_id(struct kitty_graphics *kg, uint32_t id)
{
    if (id == 0)
        return NULL;
    tll_foreach(kg->images, it) {
        if (it->item->id == id)
            return it->item;
    }
    return NULL;
}

static struct kitty_image *
image_by_number(struct kitty_graphics *kg, uint32_t number)
{
    if (number == 0)
        return NULL;
    tll_rforeach(kg->images, it) {
        if (it->item->number == number)
            return it->item;
    }
    return NULL;
}

static struct kitty_image *
image_from_cmd(struct kitty_graphics *kg, const struct kitty_cmd *cmd)
{
    if (val(cmd, KEY_i) != 0)
        return image_by_id(kg, val(cmd, KEY_i));
    return image_by_number(kg, val(cmd, KEY_I));
}

static bool
image_has_placements(struct terminal *term, const struct kitty_image *img)
{
    if (tll_length(img->virtuals) > 0)
        return true;

    foreach_grid(term, grid) {
        tll_foreach(grid->kitty_placements, it) {
            if (it->item->img == img)
                return true;
        }
    }
    return false;
}

/* Evicts images until 'needed' more bytes fit in the quota. Images
 * without placements go first, oldest first */
static bool
make_room(struct terminal *term, size_t needed, const struct kitty_image *keep)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    const size_t limit = storage_limit(term);

    if (needed > limit)
        return false;

    for (int pass = 0; pass < 2 && kg->used_bytes + needed > limit; pass++) {
        while (kg->used_bytes + needed > limit) {
            struct kitty_image *victim = NULL;

            tll_foreach(kg->images, it) {
                struct kitty_image *img = it->item;
                if (img == keep)
                    continue;
                if (pass == 0 && image_has_placements(term, img))
                    continue;
                if (victim == NULL || img->atime < victim->atime)
                    victim = img;
            }

            if (victim == NULL)
                break;

            LOG_DBG("evicting image %u (%zu bytes)", victim->id, victim->bytes);
            image_destroy(term, victim);
        }
    }

    return kg->used_bytes + needed <= limit;
}

/*
 * Responses
 */

static void
respond(struct terminal *term, const struct kitty_cmd *cmd, uint32_t id,
        const char *msg)
{
    const bool ok = strcmp(msg, "OK") == 0;
    const uint32_t quiet = val(cmd, KEY_q);

    if (quiet >= 2 || (ok && quiet >= 1))
        return;

    const uint32_t number = val(cmd, KEY_I);
    if (id == 0 && number == 0)
        return;

    char reply[512];
    size_t n = xsnprintf(reply, sizeof(reply), "\033_G");

    if (id != 0)
        n += xsnprintf(&reply[n], sizeof(reply) - n, "i=%u", id);
    if (number != 0) {
        n += xsnprintf(&reply[n], sizeof(reply) - n, "%sI=%u",
                       id != 0 ? "," : "", number);
    }
    if (val(cmd, KEY_p) != 0 && (action(cmd) == 'p' || action(cmd) == 'T'))
        n += xsnprintf(&reply[n], sizeof(reply) - n, ",p=%u", val(cmd, KEY_p));

    n += xsnprintf(&reply[n], sizeof(reply) - n, ";%s\033\\", msg);
    term_to_slave(term, reply, n);
}

/*
 * Loading pixel data
 */

static bool
path_is_sensitive(const char *path)
{
    static const char *const prefixes[] = {"/proc/", "/sys/", "/dev/"};
    if (strncmp(path, "/dev/shm/", 9) == 0)
        return false;
    for (size_t i = 0; i < ALEN(prefixes); i++) {
        if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    }
    return false;
}

static bool
path_is_deletable_temp(const char *path)
{
    if (strstr(path, "tty-graphics-protocol") == NULL)
        return false;

    const char *tmpdir = getenv("TMPDIR");
    const char *dirs[] = {"/tmp/", "/dev/shm/", "/var/tmp/", tmpdir};

    for (size_t i = 0; i < ALEN(dirs); i++) {
        const char *dir = dirs[i];
        if (dir == NULL || dir[0] == '\0')
            continue;

        char resolved[PATH_MAX];
        if (realpath(dir, resolved) == NULL)
            continue;

        size_t len = strlen(resolved);
        if (strncmp(path, resolved, len) == 0 &&
            (path[len] == '/' || resolved[len - 1] == '/'))
        {
            return true;
        }
    }

    return false;
}

static bool
read_fd(int fd, size_t size, size_t offset, struct kbuf *out, size_t max)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
        return false;

    if (offset > (size_t)st.st_size)
        return false;

    size_t avail = st.st_size - offset;
    if (size == 0)
        size = avail;
    if (size > avail || size > max)
        return false;

    if (size == 0)
        return false;

    void *map = mmap(NULL, size + offset, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        return false;

    out->data = xmalloc(size);
    out->size = out->len = size;
    memcpy(out->data, (const uint8_t *)map + offset, size);
    munmap(map, size + offset);
    return true;
}

static const char *const ERR_FILE = "EBADF:Failed to read image file";

/* Replaces 'payload' (a file name) with the file's contents */
static const char *
load_medium(struct terminal *term, const struct kitty_cmd *cmd,
            struct kbuf *payload)
{
    const char medium = has(cmd, KEY_t) ? (char)val(cmd, KEY_t) : 'd';

    if (medium == 'd')
        return NULL;

    if (medium != 'f' && medium != 't' && medium != 's')
        return "EINVAL:Unknown transmission medium";

    if (payload->len == 0 || payload->len >= PATH_MAX ||
        memchr(payload->data, '\0', payload->len) != NULL)
    {
        return ERR_FILE;
    }

    char name[PATH_MAX];
    memcpy(name, payload->data, payload->len);
    name[payload->len] = '\0';

    free(payload->data);
    *payload = (struct kbuf){0};

    const size_t size = val(cmd, KEY_S);
    const size_t offset = val(cmd, KEY_O);
    const size_t max = storage_limit(term);

    if (medium == 's') {
        int fd = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
        if (fd < 0) {
            LOG_DBG("%s: failed to open shared memory: %s", name, strerror(errno));
            return ERR_FILE;
        }

        bool ok = read_fd(fd, size, offset, payload, max);
        close(fd);
        shm_unlink(name);

        if (!ok)
            LOG_DBG("%s: failed to read shared memory", name);
        return ok ? NULL : ERR_FILE;
    }

    char resolved[PATH_MAX];
    if (realpath(name, resolved) == NULL) {
        LOG_DBG("%s: failed to resolve path: %s", name, strerror(errno));
        return ERR_FILE;
    }

    if (path_is_sensitive(resolved)) {
        LOG_WARN("%s: refusing to read file in a sensitive location", resolved);
        return ERR_FILE;
    }

    struct stat st;
    if (stat(resolved, &st) < 0 || !S_ISREG(st.st_mode)) {
        LOG_DBG("%s: not a regular file", resolved);
        return ERR_FILE;
    }

    int fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG_DBG("%s: failed to open: %s", resolved, strerror(errno));
        return ERR_FILE;
    }

    bool ok = read_fd(fd, size, offset, payload, max);
    close(fd);

    if (medium == 't' && path_is_deletable_temp(resolved))
        unlink(resolved);

    return ok ? NULL : ERR_FILE;
}

static const char *
inflate_buffer(struct kbuf *data, size_t expected, size_t max)
{
    struct kbuf out = {0};

    z_stream zs = {
        .next_in = data->data,
        .avail_in = data->len,
    };

    if (inflateInit(&zs) != Z_OK)
        return "EINVAL:Failed to initialize zlib";

    if (!buf_reserve(&out, expected > 0 ? expected : data->len * 4, max)) {
        inflateEnd(&zs);
        return "EFBIG:Decompressed data too large";
    }

    int ret;
    do {
        if (out.len == out.size && !buf_reserve(&out, out.size, max)) {
            inflateEnd(&zs);
            free(out.data);
            return "EFBIG:Decompressed data too large";
        }

        zs.next_out = &out.data[out.len];
        zs.avail_out = out.size - out.len;

        ret = inflate(&zs, Z_NO_FLUSH);
        out.len = out.size - zs.avail_out;

        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            free(out.data);
            return "EINVAL:Failed to decompress image data";
        }

        if (ret == Z_BUF_ERROR && zs.avail_in == 0)
            break;
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);

    free(data->data);
    *data = out;
    return NULL;
}

static inline uint32_t
premultiply(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool linear)
{
    if (linear) {
        r = srgb_decode_8_to_8(r);
        g = srgb_decode_8_to_8(g);
        b = srgb_decode_8_to_8(b);
    }

    if (a != 255) {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }

    return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static uint32_t
palette_color(const struct terminal *term, uint8_t idx)
{
    const struct colors *colors = &term->colors;

    if (idx < 248)
        return colors->table[idx];

    switch (idx) {
    case 248: return colors->bg;
    case 250: return colors->selection_bg >> 24 == 0 ? colors->selection_bg : colors->fg;
    case 251: return colors->selection_fg >> 24 == 0 ? colors->selection_fg : colors->bg;
    case 252: return colors->cursor_bg >> 31 ? colors->cursor_bg & 0xffffff : colors->fg;
    case 253: return colors->cursor_fg >> 31 ? colors->cursor_fg & 0xffffff : colors->bg;
    default:  return colors->fg;
    }
}

struct png_reader {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

static void
png_read_cb(png_structp png, png_bytep out, png_size_t count)
{
    struct png_reader *r = png_get_io_ptr(png);
    if (r->len - r->pos < count)
        png_error(png, "truncated PNG");
    memcpy(out, &r->data[r->pos], count);
    r->pos += count;
}

static void
png_error_cb(png_structp png, png_const_charp msg)
{
    LOG_DBG("PNG error: %s", msg);
    png_longjmp(png, 1);
}

static void
png_warning_cb(png_structp png, png_const_charp msg)
{
}

/* Decodes a PNG into premultiplied ARGB32 */
static const char *
decode_png(const struct terminal *term, const struct kbuf *in, bool linear,
           uint32_t **out, int *width, int *height)
{
    if (in->len < 8 || png_sig_cmp(in->data, 0, 8) != 0)
        return "EBADPNG:Not a PNG image";

    png_structp png = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, NULL, &png_error_cb, &png_warning_cb);
    if (png == NULL)
        return "ENOMEM:Failed to create PNG decoder";

    png_infop info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        return "ENOMEM:Failed to create PNG decoder";
    }

    struct png_reader reader = {.data = in->data, .len = in->len};
    uint8_t *volatile rgba = NULL;
    png_bytep *volatile row_ptrs = NULL;

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(rgba);
        free(row_ptrs);
        return "EBADPNG:Failed to decode PNG image";
    }

    png_set_read_fn(png, &reader, &png_read_cb);
    png_set_user_limits(png, KITTY_MAX_WIDTH, KITTY_MAX_HEIGHT);
    png_read_info(png, info);

    const png_uint_32 w = png_get_image_width(png, info);
    const png_uint_32 h = png_get_image_height(png, info);

    if ((size_t)w * h * 4 > storage_limit(term)) {
        png_destroy_read_struct(&png, &info, NULL);
        return "ENOSPC:Image exceeds the storage quota";
    }
    const int color_type = png_get_color_type(png, info);
    const int bit_depth = png_get_bit_depth(png, info);

    /* Normalize to 8-bit RGBA */
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    {
        png_set_gray_to_rgb(png);
    }
    if (!(color_type & PNG_COLOR_MASK_ALPHA))
        png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    rgba = xmalloc((size_t)w * h * 4);
    row_ptrs = xmalloc(h * sizeof(row_ptrs[0]));
    for (png_uint_32 y = 0; y < h; y++)
        row_ptrs[y] = (png_bytep)&rgba[(size_t)y * w * 4];

    png_read_image(png, row_ptrs);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    free(row_ptrs);

    uint32_t *pixels = (uint32_t *)rgba;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *px = &rgba[i * 4];
        pixels[i] = premultiply(px[0], px[1], px[2], px[3], linear);
    }

    *out = pixels;
    *width = w;
    *height = h;
    return NULL;
}

/* Converts raw (decompressed) data in format 'format' to ARGB32 */
static const char *
decode_pixels(const struct terminal *term, uint32_t format,
              const struct kbuf *in, int width, int height,
              uint32_t **out, int *out_width, int *out_height)
{
    const bool linear = wayl_do_linear_blending(term->wl, term->conf);

    if (format == 100)
        return decode_png(term, in, linear, out, out_width, out_height);

    size_t bpp;
    switch (format) {
    case 8: bpp = 1; break;
    case 16: bpp = 2; break;
    case 24: bpp = 3; break;
    case 32: bpp = 4; break;
    default: return "EINVAL:Unknown image format";
    }

    if (width <= 0 || height <= 0)
        return "EINVAL:Image dimensions not specified";

    if (width > KITTY_MAX_WIDTH || height > KITTY_MAX_HEIGHT)
        return "EINVAL:Image too large";

    if ((size_t)width * height * 4 > storage_limit(term))
        return "ENOSPC:Image exceeds the storage quota";

    const size_t count = (size_t)width * height;
    if (in->len < count * bpp)
        return "ENODATA:Insufficient image data";

    uint32_t *pixels = xmalloc(count * sizeof(uint32_t));
    const uint8_t *src = in->data;

    for (size_t i = 0; i < count; i++, src += bpp) {
        switch (format) {
        case 8:
        case 16: {
            uint32_t c = palette_color(term, src[0]);
            uint8_t a = format == 16 ? src[1] : 0xff;
            pixels[i] = premultiply(c >> 16, c >> 8, c, a, linear);
            break;
        }
        case 24:
            pixels[i] = premultiply(src[0], src[1], src[2], 0xff, linear);
            break;
        case 32:
            pixels[i] = premultiply(src[0], src[1], src[2], src[3], linear);
            break;
        }
    }

    *out = pixels;
    *out_width = width;
    *out_height = height;
    return NULL;
}

/* Loads the pixel data for a transmit/frame command. 'payload' is
 * the base64-decoded payload; it is consumed */
static const char *
load_pixels(struct terminal *term, const struct kitty_cmd *cmd,
            struct kbuf *payload, uint32_t **pixels, int *width, int *height)
{
    const char *err = load_medium(term, cmd, payload);
    if (err != NULL)
        return err;

    const uint32_t format = has(cmd, KEY_f) ? val(cmd, KEY_f) : 32;
    const int w = val(cmd, KEY_s);
    const int h = val(cmd, KEY_v);

    if (has(cmd, KEY_o)) {
        if (val(cmd, KEY_o) != 'z')
            return "EINVAL:Unknown compression";

        size_t expected = 0;
        if (format == 100)
            expected = val(cmd, KEY_S);
        else if (w > 0 && h > 0 && w <= KITTY_MAX_WIDTH && h <= KITTY_MAX_HEIGHT)
            expected = (size_t)w * h * (format == 8 ? 1 : format == 16 ? 2 : format == 24 ? 3 : 4);

        err = inflate_buffer(payload, expected, storage_limit(term));
        if (err != NULL)
            return err;
    }

    return decode_pixels(term, format, payload, w, h, pixels, width, height);
}

/*
 * Transmit
 */

static struct kitty_image *
image_new(struct terminal *term, const struct kitty_cmd *cmd,
          uint32_t *pixels, int width, int height, const char **err)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    const size_t bytes = (size_t)width * height * 4;

    uint32_t id = val(cmd, KEY_i);
    uint32_t number = val(cmd, KEY_I);

    /* Re-transmitting an image deletes the old one, and all its placements */
    if (id != 0) {
        struct kitty_image *old = image_by_id(kg, id);
        if (old != NULL)
            image_destroy(term, old);
    } else if (number != 0) {
        /* Allocate an unused ID */
        do {
            id = kg->next_id++;
            if (kg->next_id == 0)
                kg->next_id = 1u << 31;
        } while (id == 0 || image_by_id(kg, id) != NULL);
    }

    if (!make_room(term, bytes, NULL)) {
        free(pixels);
        *err = "ENOSPC:Image exceeds the storage quota";
        return NULL;
    }

    struct kitty_image *img = xcalloc(1, sizeof(*img));
    img->id = id;
    img->number = number;
    img->width = width;
    img->height = height;
    img->frames = xcalloc(1, sizeof(img->frames[0]));
    img->frame_count = 1;
    img->loops_wanted = 0;
    img->anim_state = KITTY_ANIM_STOPPED;
    img->atime = ++kg->clock;

    if (!frame_init(&img->frames[0], width, height, pixels)) {
        free(img->frames);
        free(img);
        *err = "ENOMEM:Failed to allocate image";
        return NULL;
    }
    img->frames[0].gap = 0;

    img->bytes = bytes;
    kg->used_bytes += bytes;
    tll_push_back(kg->images, img);
    return img;
}

static const char *place(struct terminal *term, const struct kitty_cmd *cmd,
                         struct kitty_image *img);

static void
handle_transmit(struct terminal *term, const struct kitty_cmd *cmd,
                struct kbuf *payload)
{
    const char act = action(cmd);

    uint32_t *pixels = NULL;
    int width = 0, height = 0;

    const char *err = load_pixels(term, cmd, payload, &pixels, &width, &height);
    if (err != NULL) {
        respond(term, cmd, val(cmd, KEY_i), err);
        return;
    }

    if (act == 'q') {
        /* Query: validate, but do not store */
        free(pixels);
        respond(term, cmd, val(cmd, KEY_i), "OK");
        return;
    }

    struct kitty_image *img = image_new(term, cmd, pixels, width, height, &err);
    if (img == NULL) {
        respond(term, cmd, val(cmd, KEY_i), err);
        return;
    }

    LOG_DBG("image %u (number=%u): %dx%d", img->id, img->number, width, height);

    if (act == 'T') {
        err = place(term, cmd, img);
        respond(term, cmd, img->id, err != NULL ? err : "OK");
    } else
        respond(term, cmd, img->id, "OK");
}

/*
 * Put
 */

static struct kitty_placement *
placement_find(struct grid *grid, const struct kitty_image *img, uint32_t id)
{
    if (id == 0)
        return NULL;
    tll_foreach(grid->kitty_placements, it) {
        if (it->item->img == img && it->item->id == id)
            return it->item;
    }
    return NULL;
}

static struct kitty_virtual *
virtual_find(struct kitty_image *img, uint32_t id)
{
    tll_foreach(img->virtuals, it) {
        if (id == 0 || it->item.id == id)
            return &it->item;
    }
    return NULL;
}

/* Returns true if 'p' (or its ancestors) would end up referencing 'target' */
static bool
creates_cycle(const struct kitty_placement *parent,
              const struct kitty_placement *target)
{
    for (int depth = 0; parent != NULL && depth < 64; depth++) {
        if (parent == target)
            return true;
        parent = parent->parent;
    }
    return false;
}

static int
chain_depth(const struct kitty_placement *p)
{
    int depth = 0;
    while (p != NULL && p->parent != NULL && depth < 64) {
        depth++;
        p = p->parent;
    }
    return depth;
}

static void
move_cursor_after(struct terminal *term, const struct geometry *g)
{
    const struct coord *cursor = &term->grid->cursor.point;

    int col = cursor->col + g->cols;
    int row = cursor->row + max(g->rows - 1, 0);

    if (col >= term->cols) {
        col = 0;
        row++;
    }

    const int bottom = term->scroll_region.end - 1;
    if (row > bottom && cursor->row <= bottom) {
        /* An image taller than the scroll region scrolls it at most
         * once over; term_scroll() requires a clamped amount */
        const int height =
            term->scroll_region.end - term->scroll_region.start;
        term_scroll(term, min(row - bottom, height));
        row = bottom;
    }

    term_cursor_to(term, min(row, term->rows - 1), min(col, term->cols - 1));
}

static const char *
place(struct terminal *term, const struct kitty_cmd *cmd, struct kitty_image *img)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    struct grid *grid = term->grid;

    img->atime = ++kg->clock;

    const uint32_t pid = val(cmd, KEY_p);

    if (val(cmd, KEY_U) == 1) {
        /* Virtual placement, for Unicode placeholders */
        if (has(cmd, KEY_P) || has(cmd, KEY_Q))
            return "EINVAL:Virtual placements cannot be relative";

        struct kitty_virtual *v = NULL;
        if (pid != 0) {
            tll_foreach(img->virtuals, it) {
                if (it->item.id == pid) {
                    v = &it->item;
                    break;
                }
            }
        }

        if (v == NULL) {
            tll_push_back(img->virtuals, ((struct kitty_virtual){.id = pid}));
            v = &tll_back(img->virtuals);
            kg->virtual_count++;
        } else
            virtual_free(v);

        v->cols = val(cmd, KEY_c);
        v->rows = val(cmd, KEY_r);
        v->cache_gen = 0;
        damage_placeholders(term);
        return NULL;
    }

    /* Relative placement? */
    struct kitty_placement *parent = NULL;
    struct kitty_image *parent_virtual_img = NULL;
    uint32_t parent_virtual_id = 0;

    if (has(cmd, KEY_P)) {
        struct kitty_image *pimg = image_by_id(kg, val(cmd, KEY_P));
        if (pimg == NULL)
            return "ENOPARENT:Parent image not found";

        parent = placement_find(grid, pimg, val(cmd, KEY_Q));
        if (parent == NULL) {
            if (virtual_find(pimg, val(cmd, KEY_Q)) == NULL)
                return "ENOPARENT:Parent placement not found";
            parent_virtual_img = pimg;
            parent_virtual_id = val(cmd, KEY_Q);
        }
    }

    struct kitty_placement *p = placement_find(grid, img, pid);

    if (parent != NULL) {
        if (parent == p || (p != NULL && creates_cycle(parent, p)))
            return "ECYCLE:Relative placement would create a cycle";
        if (chain_depth(parent) + 1 >= KITTY_MAX_RELATIVE_DEPTH)
            return "ETOODEEP:Relative placement chain too deep";
    }

    if (p != NULL) {
        /* Replace existing placement (move/resize) */
        damage_placement(term, grid, p);
        if (p->cache != NULL) {
            pixman_image_unref(p->cache);
            p->cache = NULL;
        }
    } else {
        p = xcalloc(1, sizeof(*p));
        p->img = img;
        p->id = pid;
        tll_push_back(grid->kitty_placements, p);
    }

    p->seq = ++kg->placement_seq;
    p->pos.col = grid->cursor.point.col;
    p->pos.row = grid_row_absolute(grid, grid->cursor.point.row);
    p->x_off = (int)min(val(cmd, KEY_X), (uint32_t)max(term->cell_width - 1, 0));
    p->y_off = (int)min(val(cmd, KEY_Y), (uint32_t)max(term->cell_height - 1, 0));

    p->src_x = min((int)val(cmd, KEY_x), img->width);
    p->src_y = min((int)val(cmd, KEY_y), img->height);
    p->src_w = val(cmd, KEY_w) != 0 ? (int)min(val(cmd, KEY_w), (uint32_t)img->width) : img->width;
    p->src_h = val(cmd, KEY_h) != 0 ? (int)min(val(cmd, KEY_h), (uint32_t)img->height) : img->height;
    p->src_w = min(p->src_w, img->width - p->src_x);
    p->src_h = min(p->src_h, img->height - p->src_y);

    p->req_cols = val(cmd, KEY_c);
    p->req_rows = val(cmd, KEY_r);
    p->z = sval(cmd, KEY_z);
    p->clip_top = p->clip_bottom = 0;

    p->parent = parent;
    p->parent_virtual_img = parent_virtual_img;
    p->parent_virtual_id = parent_virtual_id;
    p->off_h = sval(cmd, KEY_H);
    p->off_v = sval(cmd, KEY_V);

    if (p->src_w <= 0 || p->src_h <= 0) {
        /* Empty source rectangle; nothing to display */
        p->src_w = p->src_h = 0;
    }

    damage_placement(term, grid, p);

    if (parent == NULL && parent_virtual_img == NULL && val(cmd, KEY_C) != 1) {
        struct geometry g;
        placement_geometry(term, p, &g);
        move_cursor_after(term, &g);
    }

    return NULL;
}

static void
handle_put(struct terminal *term, const struct kitty_cmd *cmd)
{
    struct kitty_graphics *kg = term->kitty_gfx;

    if (has(cmd, KEY_i) && has(cmd, KEY_I)) {
        respond(term, cmd, val(cmd, KEY_i), "EINVAL:Both i and I specified");
        return;
    }

    struct kitty_image *img = image_from_cmd(kg, cmd);
    if (img == NULL) {
        respond(term, cmd, val(cmd, KEY_i), "ENOENT:Image not found");
        return;
    }

    const char *err = place(term, cmd, img);
    respond(term, cmd, img->id, err != NULL ? err : "OK");
}

/*
 * Delete
 */

static bool
placement_intersects(const struct terminal *term, const struct grid *grid,
                     const struct kitty_placement *p, int row, int col)
{
    int prow, pcol;
    if (!placement_position(term, grid, p, &prow, &pcol, 0))
        return false;

    /* placement_position() is view relative; convert to screen
     * (active area) relative */
    const int view_to_screen =
        grid_row_abs_to_sb(grid, term->rows, grid->view) -
        grid_row_abs_to_sb(grid, term->rows, grid->offset);
    prow += view_to_screen;

    struct geometry g;
    placement_geometry(term, p, &g);

    const bool row_ok = row < 0 || (row >= prow && row < prow + g.rows);
    const bool col_ok = col < 0 || (col >= pcol && col < pcol + g.cols);
    return row_ok && col_ok;
}

static bool
placement_on_screen(const struct terminal *term, const struct grid *grid,
                    const struct kitty_placement *p)
{
    int prow, pcol;
    if (!placement_position(term, grid, p, &prow, &pcol, 0))
        return false;

    const int view_to_screen =
        grid_row_abs_to_sb(grid, term->rows, grid->view) -
        grid_row_abs_to_sb(grid, term->rows, grid->offset);
    prow += view_to_screen;

    struct geometry g;
    placement_geometry(term, p, &g);
    return prow + g.rows > 0 && prow < term->rows;
}

static void
delete_unreferenced(struct terminal *term, struct kitty_image **candidates,
                    size_t count)
{
    for (size_t i = 0; i < count; i++) {
        struct kitty_image *img = candidates[i];
        if (img == NULL)
            continue;

        /* Candidates may contain duplicates */
        for (size_t j = i + 1; j < count; j++) {
            if (candidates[j] == img)
                candidates[j] = NULL;
        }

        bool alive = false;
        tll_foreach(term->kitty_gfx->images, it) {
            if (it->item == img) {
                alive = true;
                break;
            }
        }

        if (alive && !image_has_placements(term, img))
            image_destroy(term, img);
    }
}

static void
abort_upload(struct kitty_graphics *kg)
{
    free(kg->upload.cmd);
    free(kg->upload.data);
    memset(&kg->upload, 0, sizeof(kg->upload));
}

static void
handle_delete(struct terminal *term, const struct kitty_cmd *cmd)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    struct grid *grid = term->grid;

    const char what = has(cmd, KEY_d) ? (char)val(cmd, KEY_d) : 'a';
    const bool free_data = what >= 'A' && what <= 'Z';
    const char lower = free_data ? what - 'A' + 'a' : what;

    /* Images that may have become unreferenced */
    size_t cand_count = 0;
    size_t cand_size = tll_length(grid->kitty_placements) + 1;
    struct kitty_image **cands = xcalloc(cand_size, sizeof(cands[0]));

#define note(img_)                                                      \
    (cand_count < cand_size ? (cands[cand_count++] = (img_), 0) : 0)

    const int x = (int)val(cmd, KEY_x) - 1;   /* 1-based */
    const int y = (int)val(cmd, KEY_y) - 1;
    const int32_t z = sval(cmd, KEY_z);

    switch (lower) {
    case 'a':
        remove_placements_if(
            term, grid,
            (placement_on_screen(term, grid, p) ? (note(p->img), true) : false));
        break;

    case 'i':
    case 'n': {
        struct kitty_image *img = lower == 'i'
            ? image_by_id(kg, val(cmd, KEY_i))
            : image_by_number(kg, val(cmd, KEY_I));
        if (img == NULL)
            break;

        const uint32_t pid = val(cmd, KEY_p);
        if (pid != 0) {
            remove_placements_if(term, grid, p->img == img && p->id == pid);
            tll_foreach(img->virtuals, it) {
                if (it->item.id == pid) {
                    virtual_free(&it->item);
                    tll_remove(img->virtuals, it);
                    kg->virtual_count--;
                    damage_placeholders(term);
                }
            }
            (void)note(img);
        } else {
            foreach_grid(term, g)
                remove_placements_if(term, g, p->img == img);
            if (tll_length(img->virtuals) > 0) {
                kg->virtual_count -= tll_length(img->virtuals);
                tll_foreach(img->virtuals, it) {
                    virtual_free(&it->item);
                    tll_remove(img->virtuals, it);
                }
                damage_placeholders(term);
            }
            (void)note(img);
        }
        break;
    }

    case 'c': {
        const int crow = grid->cursor.point.row;
        const int ccol = grid->cursor.point.col;
        remove_placements_if(
            term, grid,
            (placement_intersects(term, grid, p, crow, ccol) ? (note(p->img), true) : false));
        break;
    }

    case 'p':
        remove_placements_if(
            term, grid,
            (placement_intersects(term, grid, p, y, x) ? (note(p->img), true) : false));
        break;

    case 'q':
        remove_placements_if(
            term, grid,
            (p->z == z && placement_intersects(term, grid, p, y, x)
             ? (note(p->img), true) : false));
        break;

    case 'x':
        remove_placements_if(
            term, grid,
            (placement_intersects(term, grid, p, -1, x) ? (note(p->img), true) : false));
        break;

    case 'y':
        remove_placements_if(
            term, grid,
            (placement_intersects(term, grid, p, y, -1) ? (note(p->img), true) : false));
        break;

    case 'z':
        remove_placements_if(
            term, grid,
            (p->z == z && placement_on_screen(term, grid, p)
             ? (note(p->img), true) : false));
        break;

    case 'r': {
        const uint32_t lo = val(cmd, KEY_x);
        const uint32_t hi = val(cmd, KEY_y);
        if (lo > hi) {
            respond(term, cmd, val(cmd, KEY_i), "EINVAL:Invalid range");
            break;
        }

        bool changed;
        do {
            changed = false;
            tll_foreach(kg->images, it) {
                struct kitty_image *img = it->item;
                if (img->id < lo || img->id > hi)
                    continue;

                if (free_data) {
                    image_destroy(term, img);
                } else {
                    foreach_grid(term, g)
                        remove_placements_if(term, g, p->img == img);
                    if (tll_length(img->virtuals) > 0) {
                        kg->virtual_count -= tll_length(img->virtuals);
                        tll_foreach(img->virtuals, vit) {
                            virtual_free(&vit->item);
                            tll_remove(img->virtuals, vit);
                        }
                        damage_placeholders(term);
                    }
                    continue;
                }
                changed = true;
                break;
            }
        } while (changed);
        break;
    }

    case 'f': {
        /* Delete animation frames, keeping the root frame */
        struct kitty_image *img = image_from_cmd(kg, cmd);
        if (img == NULL || img->frame_count <= 1)
            break;

        for (size_t i = 1; i < img->frame_count; i++) {
            kg->used_bytes -= (size_t)img->width * img->height * 4;
            img->bytes -= (size_t)img->width * img->height * 4;
            frame_free(&img->frames[i]);
        }
        img->frame_count = 1;
        img->current = 0;
        img->anim_state = KITTY_ANIM_STOPPED;
        img->generation++;
        damage_image(term, img);
        break;
    }

    default:
        respond(term, cmd, val(cmd, KEY_i), "EINVAL:Unknown delete target");
        break;
    }

#undef note

    if (free_data)
        delete_unreferenced(term, cands, cand_count);
    free(cands);
}

/*
 * Animation
 */

static void anim_schedule(struct terminal *term);

static void
timespec_add_ms(struct timespec *ts, int64_t ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

static int
timespec_cmp(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec)
        return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static int32_t
frame_gap(const struct kitty_frame *frame)
{
    return frame->gap;
}

/* Advance to the next displayable frame. Returns false if the
 * animation stopped (or is waiting for more frames) */
static bool
anim_advance(struct kitty_image *img)
{
    for (size_t tries = 0; tries <= img->frame_count; tries++) {
        size_t next = img->current + 1;

        if (next >= img->frame_count) {
            if (img->anim_state == KITTY_ANIM_LOADING)
                return false;

            img->loops_done++;
            /* v=N means loop N-1 times, i.e. play N times in total */
            if (img->loops_wanted > 1 && img->loops_done >= img->loops_wanted) {
                img->anim_state = KITTY_ANIM_STOPPED;
                return false;
            }
            next = 0;
        }

        img->current = next;
        if (frame_gap(&img->frames[next]) >= 0)
            return true;
    }

    /* All frames gapless */
    img->anim_state = KITTY_ANIM_STOPPED;
    return false;
}

static bool
fdm_anim(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;
    struct kitty_graphics *kg = term->kitty_gfx;

    uint64_t expirations;
    ssize_t ret = read(fd, &expirations, sizeof(expirations));
    if (ret < 0 && errno != EAGAIN)
        LOG_ERRNO("failed to read animation timer");

    if (kg == NULL)
        return true;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool changed = false;
    tll_foreach(kg->images, it) {
        struct kitty_image *img = it->item;
        if (img->anim_state == KITTY_ANIM_STOPPED || img->frame_count < 2)
            continue;
        if (timespec_cmp(&now, &img->next_frame_at) < 0)
            continue;

        if (anim_advance(img)) {
            img->generation++;
            damage_image(term, img);
            changed = true;

            int32_t gap = frame_gap(&img->frames[img->current]);
            img->next_frame_at = now;
            timespec_add_ms(&img->next_frame_at, gap > 0 ? gap : 0);
        } else if (img->anim_state != KITTY_ANIM_STOPPED) {
            /* Loading: wait for more frames */
            img->next_frame_at.tv_sec = INT32_MAX;
        }
    }

    if (changed)
        render_refresh(term);

    anim_schedule(term);
    return true;
}

static void
anim_schedule(struct terminal *term)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    if (kg == NULL)
        return;

    struct timespec earliest = {.tv_sec = INT64_MAX};
    bool any = false;

    tll_foreach(kg->images, it) {
        const struct kitty_image *img = it->item;
        if (img->anim_state == KITTY_ANIM_STOPPED || img->frame_count < 2)
            continue;
        if (img->next_frame_at.tv_sec == INT32_MAX)
            continue;
        if (!any || timespec_cmp(&img->next_frame_at, &earliest) < 0)
            earliest = img->next_frame_at;
        any = true;
    }

    if (!any) {
        if (kg->anim_fd >= 0) {
            fdm_del(term->fdm, kg->anim_fd);
            kg->anim_fd = -1;
        }
        return;
    }

    if (kg->anim_fd < 0) {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (fd < 0) {
            LOG_ERRNO("failed to create animation timer");
            return;
        }
        if (!fdm_add(term->fdm, fd, EPOLLIN, &fdm_anim, term)) {
            close(fd);
            return;
        }
        kg->anim_fd = fd;
    }

    if (earliest.tv_sec == 0 && earliest.tv_nsec == 0)
        earliest.tv_nsec = 1;

    struct itimerspec alarm = {.it_value = earliest};
    if (timerfd_settime(kg->anim_fd, TFD_TIMER_ABSTIME, &alarm, NULL) < 0)
        LOG_ERRNO("failed to arm animation timer");
}

static void
anim_start_frame_timer(struct kitty_image *img)
{
    clock_gettime(CLOCK_MONOTONIC, &img->next_frame_at);
    int32_t gap = frame_gap(&img->frames[img->current]);
    timespec_add_ms(&img->next_frame_at, gap > 0 ? gap : 0);
}

/* Composes 'src' (sw x sh at sx,sy) onto frame 'dst' at dx,dy */
static void
compose(pixman_image_t *src, int sx, int sy, pixman_image_t *dst,
        int dx, int dy, int w, int h, bool replace)
{
    pixman_image_composite32(
        replace ? PIXMAN_OP_SRC : PIXMAN_OP_OVER,
        src, NULL, dst, sx, sy, 0, 0, dx, dy, w, h);
}

static void
handle_frame(struct terminal *term, const struct kitty_cmd *cmd,
             struct kbuf *payload)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    struct kitty_image *img = image_from_cmd(kg, cmd);

    if (img == NULL) {
        respond(term, cmd, val(cmd, KEY_i), "ENOENT:Image not found");
        return;
    }

    /* Frame data */
    struct kitty_cmd data_cmd = *cmd;
    if (!has(cmd, KEY_s) || val(cmd, KEY_s) == 0) {
        data_cmd.v[KEY_s] = img->width - min(val(cmd, KEY_x), (uint32_t)img->width);
        data_cmd.present |= 1ull << KEY_s;
    }
    if (!has(cmd, KEY_v) || val(cmd, KEY_v) == 0) {
        data_cmd.v[KEY_v] = img->height - min(val(cmd, KEY_y), (uint32_t)img->height);
        data_cmd.present |= 1ull << KEY_v;
    }

    uint32_t *pixels = NULL;
    int width = 0, height = 0;
    const char *err = load_pixels(term, &data_cmd, payload, &pixels, &width, &height);
    if (err != NULL) {
        respond(term, cmd, img->id, err);
        return;
    }

    struct kitty_frame data_frame;
    if (!frame_init(&data_frame, width, height, pixels)) {
        respond(term, cmd, img->id, "ENOMEM:Out of memory");
        return;
    }

    const size_t frame_bytes = (size_t)img->width * img->height * 4;
    const uint32_t edit = val(cmd, KEY_r);
    struct kitty_frame *target;

    if (edit != 0) {
        if (edit > img->frame_count) {
            frame_free(&data_frame);
            respond(term, cmd, img->id, "ENOENT:Frame not found");
            return;
        }
        target = &img->frames[edit - 1];
    } else {
        if (!make_room(term, frame_bytes, img)) {
            frame_free(&data_frame);
            respond(term, cmd, img->id, "ENOSPC:Storage quota exceeded");
            return;
        }

        uint32_t *data = xmalloc(frame_bytes);
        const uint32_t base = val(cmd, KEY_c);

        if (base != 0 && base <= img->frame_count)
            memcpy(data, img->frames[base - 1].data, frame_bytes);
        else {
            /* Background color: 0xRRGGBBAA */
            const uint32_t bg = val(cmd, KEY_Y);
            const uint32_t color = premultiply(
                bg >> 24, bg >> 16, bg >> 8, bg,
                wayl_do_linear_blending(term->wl, term->conf));
            for (size_t i = 0; i < (size_t)img->width * img->height; i++)
                data[i] = color;
        }

        img->frames = xrealloc(img->frames, (img->frame_count + 1) * sizeof(img->frames[0]));
        target = &img->frames[img->frame_count];
        if (!frame_init(target, img->width, img->height, data)) {
            frame_free(&data_frame);
            respond(term, cmd, img->id, "ENOMEM:Out of memory");
            return;
        }
        target->gap = KITTY_DEFAULT_FRAME_GAP;
        img->frame_count++;
        img->bytes += frame_bytes;
        kg->used_bytes += frame_bytes;
    }

    compose(data_frame.pix, 0, 0, target->pix,
            val(cmd, KEY_x), val(cmd, KEY_y), width, height,
            val(cmd, KEY_X) == 1);
    frame_free(&data_frame);

    if (sval(cmd, KEY_z) > 0)
        target->gap = sval(cmd, KEY_z);
    else if (sval(cmd, KEY_z) < 0)
        target->gap = -1;

    if (target == &img->frames[img->current]) {
        img->generation++;
        damage_image(term, img);
    }

    /* A loading animation waiting for frames can continue */
    if (img->anim_state == KITTY_ANIM_LOADING &&
        img->next_frame_at.tv_sec == INT32_MAX)
    {
        anim_start_frame_timer(img);
        anim_schedule(term);
    }

    respond(term, cmd, img->id, "OK");
}

static void
handle_animate(struct terminal *term, const struct kitty_cmd *cmd)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    struct kitty_image *img = image_from_cmd(kg, cmd);

    if (img == NULL) {
        respond(term, cmd, val(cmd, KEY_i), "ENOENT:Image not found");
        return;
    }

    if (val(cmd, KEY_r) != 0 && sval(cmd, KEY_z) != 0) {
        const uint32_t r = val(cmd, KEY_r);
        if (r > img->frame_count) {
            respond(term, cmd, img->id, "ENOENT:Frame not found");
            return;
        }
        img->frames[r - 1].gap = sval(cmd, KEY_z) > 0 ? sval(cmd, KEY_z) : -1;
    }

    if (val(cmd, KEY_c) != 0) {
        const uint32_t c = val(cmd, KEY_c);
        if (c > img->frame_count) {
            respond(term, cmd, img->id, "ENOENT:Frame not found");
            return;
        }
        if (img->current != c - 1) {
            img->current = c - 1;
            img->generation++;
            damage_image(term, img);
            render_refresh(term);
        }
        anim_start_frame_timer(img);
    }

    if (val(cmd, KEY_v) != 0)
        img->loops_wanted = val(cmd, KEY_v) == 1 ? 0 : val(cmd, KEY_v);

    switch (val(cmd, KEY_s)) {
    case 1:
        img->anim_state = KITTY_ANIM_STOPPED;
        img->loops_done = 0;
        break;

    case 2:
    case 3: {
        const enum kitty_anim_state new_state =
            val(cmd, KEY_s) == 2 ? KITTY_ANIM_LOADING : KITTY_ANIM_RUNNING;
        if (img->anim_state == KITTY_ANIM_STOPPED ||
            img->next_frame_at.tv_sec == INT32_MAX)
        {
            anim_start_frame_timer(img);
        }
        img->anim_state = new_state;
        break;
    }
    }

    anim_schedule(term);
    respond(term, cmd, img->id, "OK");
}

static void
handle_compose(struct terminal *term, const struct kitty_cmd *cmd)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    struct kitty_image *img = image_from_cmd(kg, cmd);

    if (img == NULL) {
        respond(term, cmd, val(cmd, KEY_i), "ENOENT:Image not found");
        return;
    }

    const uint32_t src_no = val(cmd, KEY_r);
    const uint32_t dst_no = val(cmd, KEY_c);

    if (src_no == 0 || dst_no == 0 ||
        src_no > img->frame_count || dst_no > img->frame_count)
    {
        respond(term, cmd, img->id, "ENOENT:Frame not found");
        return;
    }

    const int w = val(cmd, KEY_w) != 0 ? (int)val(cmd, KEY_w) : img->width;
    const int h = val(cmd, KEY_h) != 0 ? (int)val(cmd, KEY_h) : img->height;
    const int sx = val(cmd, KEY_X), sy = val(cmd, KEY_Y);
    const int dx = val(cmd, KEY_x), dy = val(cmd, KEY_y);

    if ((int64_t)sx + w > img->width || (int64_t)sy + h > img->height ||
        (int64_t)dx + w > img->width || (int64_t)dy + h > img->height ||
        w <= 0 || h <= 0)
    {
        respond(term, cmd, img->id, "EINVAL:Rectangle out of bounds");
        return;
    }

    if (src_no == dst_no &&
        sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h)
    {
        respond(term, cmd, img->id, "EINVAL:Overlapping rectangles");
        return;
    }

    compose(img->frames[src_no - 1].pix, sx, sy, img->frames[dst_no - 1].pix,
            dx, dy, w, h, val(cmd, KEY_C) == 1);

    if (dst_no - 1 == img->current) {
        img->generation++;
        damage_image(term, img);
    }

    respond(term, cmd, img->id, "OK");
}

/*
 * Feature queries (a=q,e=<action>)
 */

static bool
feature_supported(char act, int key, const struct kitty_cmd *cmd)
{
    const uint32_t v = val(cmd, key);

    const bool transmit = act == 't' || act == 'T' || act == 'q' || act == 'f';
    const bool display = act == 'T' || act == 'p';

    switch (key) {
    case KEY_f:
        return (transmit) && (v == 8 || v == 16 || v == 24 || v == 32 || v == 100);
    case KEY_t:
        return transmit && (v == 'd' || v == 'f' || v == 't' || v == 's');
    case KEY_o:
        return transmit && v == 'z';
    case KEY_s: case KEY_v: case KEY_S: case KEY_O:
        return transmit || act == 'a';
    case KEY_N:
        return transmit && v == 1;
    case KEY_U:
        return display && v == 1;
    case KEY_C:
        return (display && v == 1) || (act == 'c' && v == 1);
    case KEY_x: case KEY_y: case KEY_w: case KEY_h:
        return display || act == 'f' || act == 'c' || act == 'd';
    case KEY_X:
        return display || act == 'c' || (act == 'f' && v == 1);
    case KEY_Y:
        return display || act == 'c' || act == 'f';
    case KEY_c: case KEY_r:
        return display || act == 'f' || act == 'a' || act == 'c';
    case KEY_z:
        return display || act == 'f' || act == 'a' || act == 'd';
    case KEY_P: case KEY_Q: case KEY_H: case KEY_V:
        return display;
    case KEY_d:
        return act == 'd' && strchr("aAcCfFiInNpPqQrRxXyYzZ", (int)v) != NULL;
    default:
        return false;
    }
}

static void
handle_feature_query(struct terminal *term, const struct kitty_cmd *cmd)
{
    const char act = (char)val(cmd, KEY_e);
    const uint32_t id = val(cmd, KEY_i);

    if (id == 0 || val(cmd, KEY_q) >= 1)
        return;

    char reply[512];
    size_t n = xsnprintf(reply, sizeof(reply), "\033_Gi=%u", id);

    if (strchr("tTqpdfac", act) != NULL) {
        n += xsnprintf(&reply[n], sizeof(reply) - n, ",e=%c", act);

        for (int key = 0; key < KEY_COUNT; key++) {
            if (!has(cmd, key))
                continue;
            if (key == KEY_a || key == KEY_e || key == KEY_i || key == KEY_I ||
                key == KEY_m || key == KEY_q)
            {
                continue;
            }
            if (val(cmd, key) == 0)
                continue;  /* Default value */
            if (!feature_supported(act, key, cmd))
                continue;

            if (key_is_char(key)) {
                n += xsnprintf(&reply[n], sizeof(reply) - n, ",%c=%c",
                               key_names[key], (char)val(cmd, key));
            } else if (key_is_signed(key)) {
                n += xsnprintf(&reply[n], sizeof(reply) - n, ",%c=%d",
                               key_names[key], sval(cmd, key));
            } else {
                n += xsnprintf(&reply[n], sizeof(reply) - n, ",%c=%u",
                               key_names[key], val(cmd, key));
            }
        }
    }

    n += xsnprintf(&reply[n], sizeof(reply) - n, ";OK\033\\");
    term_to_slave(term, reply, n);
}

/*
 * Dispatch
 */

static void
execute(struct terminal *term, const struct kitty_cmd *cmd, struct kbuf *payload)
{
    const char act = action(cmd);

    if (has(cmd, KEY_i) && has(cmd, KEY_I) && val(cmd, KEY_i) != 0 &&
        val(cmd, KEY_I) != 0)
    {
        respond(term, cmd, val(cmd, KEY_i), "EINVAL:Both i and I specified");
        return;
    }

    switch (act) {
    case 't':
    case 'T':
    case 'q':
        handle_transmit(term, cmd, payload);
        break;

    case 'p':
        handle_put(term, cmd);
        break;

    case 'f':
        handle_frame(term, cmd, payload);
        break;

    default:
        respond(term, cmd, val(cmd, KEY_i), "EINVAL:Unknown action");
        break;
    }
}

void
kitty_graphics_command(struct terminal *term, const uint8_t *data, size_t len)
{
    if (!term->conf->tweak.kitty_graphics)
        return;

    struct kitty_graphics *kg = state(term);

    const uint8_t *sep = memchr(data, ';', len);
    const size_t ctrl_len = sep != NULL ? (size_t)(sep - data) : len;
    const uint8_t *b64 = sep != NULL ? sep + 1 : NULL;
    const size_t b64_len = sep != NULL ? len - ctrl_len - 1 : 0;

    struct kitty_cmd cmd;
    if (!parse_control((const char *)data, ctrl_len, &cmd)) {
        LOG_DBG("malformed control data: %.*s", (int)ctrl_len, (const char *)data);
        if (kg->upload.active)
            abort_upload(kg);
        return;
    }

    const char act = action(&cmd);
    const size_t max = storage_limit(term);

    /* Continuation of a chunked transmission */
    if (kg->upload.active && act != 'd' && !has(&cmd, KEY_e)) {
        struct kitty_cmd *first = kg->upload.cmd;
        struct kbuf buf = {
            .data = kg->upload.data,
            .len = kg->upload.len,
            .size = kg->upload.size,
        };

        if (!kg->upload.failed &&
            !b64_decode_append(b64, b64_len, &buf,
                               kg->upload.b64_quad, &kg->upload.b64_len, max))
        {
            kg->upload.failed = true;
        }

        kg->upload.data = buf.data;
        kg->upload.len = buf.len;
        kg->upload.size = buf.size;

        if (has(&cmd, KEY_q))
            first->v[KEY_q] = val(&cmd, KEY_q);

        if (val(&cmd, KEY_m) == 1)
            return;

        /* Final chunk */
        struct kitty_cmd final = *first;
        const bool failed = kg->upload.failed ||
            !b64_decode_finish(&buf, kg->upload.b64_quad, &kg->upload.b64_len, max);

        kg->upload.data = NULL;
        abort_upload(kg);

        if (failed) {
            free(buf.data);
            respond(term, &final, val(&final, KEY_i), "EFBIG:Image data too large");
            return;
        }

        execute(term, &final, &buf);
        free(buf.data);
        return;
    }

    if (kg->upload.active)
        abort_upload(kg);

    if (act == 'q' && has(&cmd, KEY_e)) {
        handle_feature_query(term, &cmd);
        return;
    }

    switch (act) {
    case 'd':
        handle_delete(term, &cmd);
        return;
    case 'a':
        handle_animate(term, &cmd);
        return;
    case 'c':
        handle_compose(term, &cmd);
        return;
    case 'p':
        handle_put(term, &cmd);
        return;
    }

    struct kbuf payload = {0};
    uint8_t quad[4];
    size_t quad_len = 0;

    if (!b64_decode_append(b64, b64_len, &payload, quad, &quad_len, max)) {
        free(payload.data);
        respond(term, &cmd, val(&cmd, KEY_i), "EFBIG:Image data too large");
        return;
    }

    if (val(&cmd, KEY_m) == 1) {
        /* First chunk of a chunked transmission. Base64 groups may
         * straddle chunks; keep the partial group */
        kg->upload.active = true;
        kg->upload.cmd = xmemdup(&cmd, sizeof(cmd));
        kg->upload.data = payload.data;
        kg->upload.len = payload.len;
        kg->upload.size = payload.size;
        memcpy(kg->upload.b64_quad, quad, sizeof(quad));
        kg->upload.b64_len = quad_len;
        return;
    }

    if (!b64_decode_finish(&payload, quad, &quad_len, max)) {
        free(payload.data);
        respond(term, &cmd, val(&cmd, KEY_i), "EFBIG:Image data too large");
        return;
    }

    execute(term, &cmd, &payload);
    free(payload.data);
}

/*
 * Lifecycle
 */

void
kitty_grid_free(struct grid *grid)
{
    tll_foreach(grid->kitty_placements, it) {
        placement_free(it->item);
        tll_remove(grid->kitty_placements, it);
    }
}

void
kitty_clear_grid(struct terminal *term, struct grid *grid)
{
    if (term->kitty_gfx == NULL)
        return;

    tll_foreach(grid->kitty_placements, it)
        damage_placement(term, grid, it->item);
    kitty_grid_free(grid);
}

void
kitty_graphics_reset(struct terminal *term)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    if (kg == NULL)
        return;

    foreach_grid(term, grid)
        kitty_grid_free(grid);

    while (tll_length(kg->images) > 0)
        image_destroy(term, tll_front(kg->images));

    abort_upload(kg);
    kg->virtual_count = 0;
    anim_schedule(term);
    term_damage_view(term);
}

void
kitty_graphics_fini(struct terminal *term)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    if (kg == NULL)
        return;

    foreach_grid(term, grid)
        kitty_grid_free(grid);

    tll_foreach(kg->images, it) {
        struct kitty_image *img = it->item;
        tll_foreach(img->virtuals, vit)
            virtual_free(&vit->item);
        tll_free(img->virtuals);
        for (size_t i = 0; i < img->frame_count; i++)
            frame_free(&img->frames[i]);
        free(img->frames);
        free(img);
        tll_remove(kg->images, it);
    }

    if (kg->anim_fd >= 0)
        fdm_del(term->fdm, kg->anim_fd);

    abort_upload(kg);
    free(kg->render.below);
    free(kg->render.above);
    free(kg->render.above_rows);
    free(kg);
    term->kitty_gfx = NULL;
}

void
kitty_cell_size_changed(struct terminal *term)
{
    /* Scaled caches are re-validated against the cell size when
     * rendering; nothing to do but to repaint */
    if (term->kitty_gfx != NULL)
        term_damage_view(term);
}

/*
 * Scrolling
 */

static inline bool
is_anchored(const struct kitty_placement *p)
{
    return p->parent == NULL && p->parent_virtual_img == NULL;
}

/* Screen (active area) row of a placement's top row; negative if in
 * the scrollback */
static int
screen_top(const struct terminal *term, const struct grid *grid,
           const struct kitty_placement *p)
{
    return grid_row_abs_to_sb(grid, term->rows, p->pos.row) -
           (grid->num_rows - term->rows);
}

/* Called *before* the grid's offset is moved */
void
kitty_scroll_up(struct terminal *term, int rows)
{
    struct grid *grid = term->grid;
    if (likely(tll_length(grid->kitty_placements) == 0))
        return;

    /* Placements whose top row falls out of the scrollback are deleted */
    remove_placements_if(
        term, grid,
        is_anchored(p) &&
        grid_row_abs_to_sb(grid, term->rows, p->pos.row) < rows);
}

void
kitty_scroll_down(struct terminal *term, int rows)
{
    struct grid *grid = term->grid;
    if (likely(tll_length(grid->kitty_placements) == 0))
        return;

    /* Placements whose top row is freed (scrolled out at the bottom)
     * are deleted. Others may extend past the screen bottom; they are
     * clipped when rendered */
    remove_placements_if(
        term, grid,
        is_anchored(p) && screen_top(term, grid, p) >= term->rows - rows);
}

/*
 * Called *before* the rows have been moved. Placements entirely
 * inside the scroll region are moved, and clipped against the
 * region's edges. Others stay where they are on screen.
 */
void
kitty_scroll_partial(struct terminal *term, struct scroll_region region,
                     int rows, bool reverse)
{
    struct grid *grid = term->grid;
    if (likely(tll_length(grid->kitty_placements) == 0))
        return;

    const int mask = grid->num_rows - 1;
    const int old_offset = grid->offset;
    const int new_offset = reverse
        ? (old_offset - rows + grid->num_rows) & mask
        : (old_offset + rows) & mask;

    bool changed;
    do {
        changed = false;

        tll_foreach(grid->kitty_placements, it) {
            struct kitty_placement *p = it->item;
            if (!is_anchored(p))
                continue;

            /* Screen row, before the scroll */
            const int top = (p->pos.row - old_offset + grid->num_rows) & mask;
            if (top >= term->rows)
                continue;  /* In the scrollback */

            const int height = kitty_placement_rows(term, p);
            const int vis_top = top + p->clip_top;
            const int vis_bottom = top + height - p->clip_bottom;  /* Exclusive */

            int new_top = top;
            int clip_top = p->clip_top;
            int clip_bottom = p->clip_bottom;

            if (vis_top >= region.start && vis_bottom <= region.end) {
                /* Entirely inside the region; scroll it */
                new_top = reverse ? top + rows : top - rows;

                const int new_vis_top = new_top + clip_top;
                const int new_vis_bottom = new_top + height - clip_bottom;

                if (new_vis_top < region.start)
                    clip_top += region.start - new_vis_top;
                if (new_vis_bottom > region.end)
                    clip_bottom += new_vis_bottom - region.end;

                if (clip_top + clip_bottom >= height) {
                    tll_remove(grid->kitty_placements, it);
                    placement_unlink(term, grid, p, 0);
                    placement_free(p);
                    changed = true;
                    break;
                }
            }

            damage_placement(term, grid, p);
            p->pos.row = (new_offset + new_top + grid->num_rows) & mask;
            p->clip_top = clip_top;
            p->clip_bottom = clip_bottom;
        }
    } while (changed);

    /* Damage new positions, after the rows have been moved (the
     * caller damages the scroll region anyway) */
}

void
kitty_erase_screen(struct terminal *term)
{
    struct grid *grid = term->grid;
    if (likely(tll_length(grid->kitty_placements) == 0))
        return;

    remove_placements_if(
        term, grid,
        is_anchored(p) &&
        screen_top(term, grid, p) + kitty_placement_rows(term, p) > 0 &&
        screen_top(term, grid, p) < term->rows);
}

void
kitty_erase_scrollback(struct terminal *term)
{
    struct grid *grid = term->grid;
    if (likely(tll_length(grid->kitty_placements) == 0))
        return;

    remove_placements_if(
        term, grid, is_anchored(p) && screen_top(term, grid, p) < 0);
}

/*
 * Rendering
 */

/* View-relative position of a placement's top-left cell */
static bool
placement_position(const struct terminal *term, const struct grid *grid,
                   const struct kitty_placement *p, int *row, int *col,
                   int depth)
{
    if (depth > KITTY_MAX_RELATIVE_DEPTH * 2)
        return false;

    if (p->parent != NULL) {
        int prow, pcol;
        if (!placement_position(term, grid, p->parent, &prow, &pcol, depth + 1))
            return false;
        *row = prow + p->off_v;
        *col = pcol + p->off_h;
        return true;
    }

    if (p->parent_virtual_img != NULL) {
        const struct kitty_virtual *v = NULL;
        tll_foreach(p->parent_virtual_img->virtuals, it) {
            if (p->parent_virtual_id == 0 || it->item.id == p->parent_virtual_id) {
                v = &it->item;
                break;
            }
        }
        if (v == NULL || !v->visible)
            return false;
        *row = v->vis_row + p->off_v;
        *col = v->vis_col + p->off_h;
        return true;
    }

    *row = view_row(term, grid, p->pos.row);
    *col = p->pos.col;
    return true;
}

static pixman_image_t *
scale_image(pixman_image_t *src, int src_x, int src_y, int src_w, int src_h,
            int dst_w, int dst_h)
{
    pixman_image_t *dst = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, dst_w, dst_h, NULL, dst_w * sizeof(uint32_t));
    if (dst == NULL)
        return NULL;

    if (src_w == dst_w && src_h == dst_h) {
        pixman_image_composite32(
            PIXMAN_OP_SRC, src, NULL, dst, src_x, src_y, 0, 0, 0, 0, dst_w, dst_h);
        return dst;
    }

    const double sx = (double)src_w / dst_w;
    const double sy = (double)src_h / dst_h;

    pixman_transform_t t;
    pixman_transform_init_scale(
        &t, pixman_double_to_fixed(sx), pixman_double_to_fixed(sy));
    pixman_transform_translate(
        &t, NULL, pixman_int_to_fixed(src_x), pixman_int_to_fixed(src_y));
    pixman_image_set_transform(src, &t);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);

    if (sx > 2. || sy > 2.) {
        int n_params = 0;
        pixman_fixed_t *params = pixman_filter_create_separable_convolution(
            &n_params,
            pixman_double_to_fixed(max(sx, 1.)),
            pixman_double_to_fixed(max(sy, 1.)),
            PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_IMPULSE,
            PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX, 1, 1);
        pixman_image_set_filter(
            src, PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, n_params);
        free(params);
    } else
        pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);

    pixman_image_composite32(
        PIXMAN_OP_SRC, src, NULL, dst, 0, 0, 0, 0, 0, 0, dst_w, dst_h);

    pixman_image_set_transform(src, NULL);
    pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
    pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0);
    return dst;
}

static pixman_image_t *
placement_pix(struct kitty_placement *p, const struct geometry *g)
{
    struct kitty_image *img = p->img;
    if (p->src_w <= 0 || p->src_h <= 0 || g->width <= 0 || g->height <= 0)
        return NULL;

    if (p->cache != NULL &&
        p->cache_gen == img->generation + 1 &&
        p->cache_w == g->width && p->cache_h == g->height)
    {
        return p->cache;
    }

    if (p->cache != NULL)
        pixman_image_unref(p->cache);

    p->cache = scale_image(
        img->frames[img->current].pix, p->src_x, p->src_y, p->src_w, p->src_h,
        g->width, g->height);
    p->cache_gen = img->generation + 1;
    p->cache_w = g->width;
    p->cache_h = g->height;
    return p->cache;
}

static void
virtual_geometry(const struct terminal *term, const struct kitty_image *img,
                 const struct kitty_virtual *v, int *cols, int *rows,
                 int *box_x, int *box_y, int *w, int *h)
{
    const int cw = max(term->cell_width, 1);
    const int ch = max(term->cell_height, 1);

    int c = v->cols;
    int r = v->rows;

    if (c == 0 && r == 0) {
        c = (img->width + cw - 1) / cw;
        r = (img->height + ch - 1) / ch;
    } else if (c == 0) {
        c = max(1, (int)(((double)r * ch * img->width / img->height + cw - 1) / cw));
    } else if (r == 0) {
        r = max(1, (int)(((double)c * cw * img->height / img->width + ch - 1) / ch));
    }

    const int box_w = c * cw;
    const int box_h = r * ch;
    const double scale = min((double)box_w / img->width, (double)box_h / img->height);

    *w = max(1, (int)(img->width * scale + 0.5));
    *h = max(1, (int)(img->height * scale + 0.5));
    *box_x = (box_w - *w) / 2;
    *box_y = (box_h - *h) / 2;
    *cols = c;
    *rows = r;
}

static pixman_image_t *
virtual_pix(const struct terminal *term, struct kitty_image *img,
            struct kitty_virtual *v)
{
    if (v->cache != NULL &&
        v->cache_gen == img->generation + 1 &&
        v->cache_cell_w == term->cell_width &&
        v->cache_cell_h == term->cell_height)
    {
        return v->cache;
    }

    int cols, rows, w, h;
    virtual_geometry(term, img, v, &cols, &rows, &v->box_x, &v->box_y, &w, &h);

    if (v->cache != NULL)
        pixman_image_unref(v->cache);

    v->cache = scale_image(
        img->frames[img->current].pix, 0, 0, img->width, img->height, w, h);
    v->cache_gen = img->generation + 1;
    v->cache_cell_w = term->cell_width;
    v->cache_cell_h = term->cell_height;
    return v->cache;
}

/*
 * Unicode placeholders
 */

static int
diacritic_value(char32_t wc)
{
    size_t lo = 0, hi = ALEN(kitty_diacritics);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kitty_diacritics[mid] == wc)
            return mid;
        if (kitty_diacritics[mid] < wc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static const struct composed *
cell_composed(const struct terminal *term, const struct cell *cell)
{
    if (cell->wc < CELL_COMB_CHARS_LO || cell->wc > CELL_COMB_CHARS_HI)
        return NULL;
    return composed_lookup(term->composed, cell->wc - CELL_COMB_CHARS_LO);
}

bool
kitty_is_placeholder(const struct terminal *term, const struct cell *cell)
{
    if (cell->wc == KITTY_PLACEHOLDER)
        return true;
    if (cell->wc < CELL_COMB_CHARS_LO || cell->wc > CELL_COMB_CHARS_HI)
        return false;
    const struct composed *c = cell_composed(term, cell);
    return c != NULL && c->count > 0 && c->chars[0] == KITTY_PLACEHOLDER;
}

struct placeholder {
    uint32_t fg;        /* Image ID, lower 24 bits */
    uint32_t ul;        /* Placement ID */
    int row;
    int col;
    int msb;
    bool has_row, has_col, has_msb;
};

static uint32_t
underline_color_at(const struct row *row, int col)
{
    if (row->extra == NULL)
        return 0;

    const struct row_ranges *ranges = &row->extra->underline_ranges;
    for (int i = 0; i < ranges->count; i++) {
        const struct row_range *r = &ranges->v[i];
        if (col >= r->start && col <= r->end)
            return r->underline.color_src == COLOR_DEFAULT ? 0 : r->underline.color;
    }
    return 0;
}

static void
placeholder_parse(const struct terminal *term, const struct row *row, int col,
                  struct placeholder *ph)
{
    const struct cell *cell = &row->cells[col];

    *ph = (struct placeholder){
        .fg = cell->attrs.fg_src == COLOR_DEFAULT ? 0 : cell->attrs.fg,
        .ul = underline_color_at(row, col),
    };

    const struct composed *c = cell_composed(term, cell);
    if (c == NULL)
        return;

    if (c->count > 1) {
        int v = diacritic_value(c->chars[1]);
        if (v >= 0) {
            ph->row = v;
            ph->has_row = true;
        }
    }
    if (c->count > 2 && ph->has_row) {
        int v = diacritic_value(c->chars[2]);
        if (v >= 0) {
            ph->col = v;
            ph->has_col = true;
        }
    }
    if (c->count > 3 && ph->has_col) {
        int v = diacritic_value(c->chars[3]);
        if (v >= 0) {
            ph->msb = v;
            ph->has_msb = true;
        }
    }
}

/* Resolves row/column/MSB, inheriting missing values from the left */
static void
placeholder_resolve(const struct terminal *term, const struct row *row, int col,
                    struct placeholder *out)
{
    /* Find the start of the run of placeholders */
    int start = col;
    while (start > 0 && kitty_is_placeholder(term, &row->cells[start - 1]))
        start--;

    struct placeholder prev = {0};
    bool have_prev = false;

    for (int c = start; c <= col; c++) {
        struct placeholder cur;
        placeholder_parse(term, row, c, &cur);

        const bool same = have_prev && prev.fg == cur.fg && prev.ul == cur.ul;

        if (!cur.has_row) {
            if (same) {
                cur.row = prev.row;
                cur.col = prev.col + 1;
                cur.msb = prev.msb;
            }
        } else if (!cur.has_col) {
            if (same && prev.row == cur.row) {
                cur.col = prev.col + 1;
                cur.msb = prev.msb;
            }
        } else if (!cur.has_msb) {
            if (same && prev.row == cur.row && prev.col == cur.col - 1)
                cur.msb = prev.msb;
        }

        prev = cur;
        have_prev = true;
    }

    *out = prev;
}

static struct kitty_image *
placeholder_image(const struct terminal *term, const struct placeholder *ph,
                  struct kitty_virtual **v)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    const uint32_t id = (uint32_t)ph->msb << 24 | (ph->fg & 0xffffff);

    struct kitty_image *img = image_by_id(kg, id);
    if (img == NULL)
        return NULL;

    *v = virtual_find(img, ph->ul);
    if (*v == NULL && ph->ul != 0)
        *v = virtual_find(img, 0);
    return *v != NULL ? img : NULL;
}

void
kitty_render_placeholder(struct terminal *term, pixman_image_t *pix,
                         const struct row *row, int col, int x, int y)
{
    struct placeholder ph;
    placeholder_resolve(term, row, col, &ph);

    struct kitty_virtual *v;
    struct kitty_image *img = placeholder_image(term, &ph, &v);
    if (img == NULL || v->cache == NULL)
        return;

    const int cw = term->cell_width;
    const int ch = term->cell_height;

    /* This cell's rectangle, in the cols*rows box */
    const int bx = ph.col * cw;
    const int by = ph.row * ch;

    /* Box to image coordinates */
    const int ix = bx - v->box_x;
    const int iy = by - v->box_y;

    const int img_w = pixman_image_get_width(v->cache);
    const int img_h = pixman_image_get_height(v->cache);

    int x0 = max(ix, 0), y0 = max(iy, 0);
    int x1 = min(ix + cw, img_w), y1 = min(iy + ch, img_h);
    if (x0 >= x1 || y0 >= y1)
        return;

    pixman_image_composite32(
        PIXMAN_OP_OVER, v->cache, NULL, pix,
        x0, y0, 0, 0, x + (x0 - ix), y + (y0 - iy), x1 - x0, y1 - y0);
}

/* Make sure pixman's lazily computed image state is computed here,
 * on the main thread, and not concurrently in the render workers */
static void
prewarm(pixman_image_t *pix)
{
    static pixman_image_t *scratch = NULL;
    if (scratch == NULL)
        scratch = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, NULL, 4);
    pixman_image_composite32(
        PIXMAN_OP_OVER, pix, NULL, scratch, 0, 0, 0, 0, 0, 0, 1, 1);
}

/* Scans the view for placeholders, preparing scaled images, and
 * computing the virtual placements' on-screen positions */
static void
prepare_placeholders(struct terminal *term)
{
    struct kitty_graphics *kg = term->kitty_gfx;

    tll_foreach(kg->images, it) {
        tll_foreach(it->item->virtuals, vit)
            vit->item.visible = false;
    }

    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        if (row == NULL)
            continue;

        for (int c = 0; c < term->cols; c++) {
            if (!kitty_is_placeholder(term, &row->cells[c]))
                continue;

            struct placeholder ph;
            placeholder_resolve(term, row, c, &ph);

            struct kitty_virtual *v;
            struct kitty_image *img = placeholder_image(term, &ph, &v);
            if (img == NULL)
                continue;

            if (!row->cells[c].attrs.clean || v->cache == NULL ||
                v->cache_gen != img->generation + 1)
            {
                pixman_image_t *pix = virtual_pix(term, img, v);
                if (pix != NULL)
                    prewarm(pix);
                row->cells[c].attrs.clean = 0;
                row->dirty = true;
            }

            const int top = r - ph.row;
            const int left = c - ph.col;
            if (!v->visible) {
                v->visible = true;
                v->vis_row = top;
                v->vis_col = left;
            } else {
                v->vis_row = min(v->vis_row, top);
                v->vis_col = min(v->vis_col, left);
            }
        }
    }
}

static int
entry_cmp(const void *_a, const void *_b)
{
    const struct kitty_render_entry *a = _a;
    const struct kitty_render_entry *b = _b;

    if (a->z != b->z)
        return a->z < b->z ? -1 : 1;
    if (a->id != b->id)
        return a->id < b->id ? -1 : 1;
    if (a->seq != b->seq)
        return a->seq < b->seq ? -1 : 1;
    return 0;
}

static void
push_entry(struct kitty_render_entry **list, size_t *count, size_t *size,
           const struct kitty_render_entry *e)
{
    if (*count >= *size) {
        *size = *size == 0 ? 8 : *size * 2;
        *list = xrealloc(*list, *size * sizeof((*list)[0]));
    }
    (*list)[(*count)++] = *e;
}

void
kitty_render_prepare(struct terminal *term)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    if (kg == NULL)
        return;

    kg->render.below_count = 0;
    kg->render.above_count = 0;

    if (kg->render.above_rows_count != term->rows) {
        free(kg->render.above_rows);
        kg->render.above_rows = xcalloc(term->rows, sizeof(kg->render.above_rows[0]));
        kg->render.above_rows_count = term->rows;
    } else
        memset(kg->render.above_rows, 0, term->rows * sizeof(kg->render.above_rows[0]));

    if (kg->virtual_count > 0)
        prepare_placeholders(term);

    struct grid *grid = term->grid;
    if (tll_length(grid->kitty_placements) == 0)
        return;

    const int cw = term->cell_width;
    const int ch = term->cell_height;
    const int left = term->margins.left;
    const int top = term->margins.top;
    const int right = left + term->cols * cw;
    const int bottom = top + term->rows * ch;

    tll_foreach(grid->kitty_placements, it) {
        struct kitty_placement *p = it->item;

        int row, col;
        if (!placement_position(term, grid, p, &row, &col, 0))
            continue;

        struct geometry g;
        placement_geometry(term, p, &g);

        const int vis_row0 = max(row + p->clip_top, 0);
        const int vis_row1 = min(row + g.rows - p->clip_bottom, term->rows);
        if (vis_row0 >= vis_row1 || col >= term->cols || col + g.cols <= 0)
            continue;

        pixman_image_t *pix = placement_pix(p, &g);
        if (pix == NULL)
            continue;

        struct kitty_render_entry e = {
            .pix = pix,
            .x = left + col * cw + p->x_off,
            .y = top + row * ch + p->y_off,
            .z = p->z,
            .id = p->img->id,
            .seq = p->seq,
            .under_bg = p->z < INT32_MIN / 2,
        };

        e.clip.x1 = max(e.x, left);
        e.clip.y1 = max(max(e.y, top + vis_row0 * ch), top);
        e.clip.x2 = min(min(e.x + g.width, right), left + (col + g.cols) * cw);
        e.clip.y2 = min(min(e.y + g.height, top + vis_row1 * ch), bottom);

        if (e.clip.x1 >= e.clip.x2 || e.clip.y1 >= e.clip.y2)
            continue;

        if (p->z < 0) {
            push_entry(&kg->render.below, &kg->render.below_count,
                       &kg->render.below_size, &e);
        } else {
            push_entry(&kg->render.above, &kg->render.above_count,
                       &kg->render.above_size, &e);

            /* Rows that will be re-rendered, must have the entire
             * image span re-rendered, since the image is
             * composited on top of whatever was there before */
            const int c0 = max((e.clip.x1 - left) / cw, 0);
            const int c1 = min((e.clip.x2 - left + cw - 1) / cw, term->cols);

            for (int r = (e.clip.y1 - top) / ch;
                 r < min((e.clip.y2 - top + ch - 1) / ch, term->rows);
                 r++)
            {
                struct row *grid_row = grid_row_in_view(grid, r);
                if (!grid_row->dirty)
                    continue;

                for (int c = c0; c < c1; c++)
                    grid_row->cells[c].attrs.clean = 0;
            }
        }
    }

    for (size_t i = 0; i < kg->render.below_count; i++)
        prewarm(kg->render.below[i].pix);

    qsort(kg->render.below, kg->render.below_count,
          sizeof(kg->render.below[0]), &entry_cmp);
    qsort(kg->render.above, kg->render.above_count,
          sizeof(kg->render.above[0]), &entry_cmp);

    /* Remember which rows are dirty; they are cleaned when rendered */
    if (kg->render.above_count > 0) {
        for (int r = 0; r < term->rows; r++) {
            const struct row *grid_row = grid_row_in_view(grid, r);
            kg->render.above_rows[r] = grid_row->dirty;
        }
    }
}

static void
composite_clipped(const struct kitty_render_entry *e, pixman_image_t *dst,
                  int x, int y, int width, int height)
{
    int x1 = max(x, e->clip.x1);
    int y1 = max(y, e->clip.y1);
    int x2 = min(x + width, e->clip.x2);
    int y2 = min(y + height, e->clip.y2);

    if (x1 >= x2 || y1 >= y2)
        return;

    pixman_image_composite32(
        PIXMAN_OP_OVER, e->pix, NULL, dst,
        x1 - e->x, y1 - e->y, 0, 0, x1, y1, x2 - x1, y2 - y1);
}

void
kitty_render_below(const struct terminal *term, pixman_image_t *pix,
                   int x, int y, int width, int height, bool default_bg)
{
    const struct kitty_graphics *kg = term->kitty_gfx;

    for (size_t i = 0; i < kg->render.below_count; i++) {
        const struct kitty_render_entry *e = &kg->render.below[i];
        if (e->under_bg && !default_bg)
            continue;
        composite_clipped(e, pix, x, y, width, height);
    }
}

void
kitty_render_above(struct terminal *term, pixman_image_t *pix,
                   pixman_region32_t *damage)
{
    struct kitty_graphics *kg = term->kitty_gfx;
    if (kg == NULL || kg->render.above_count == 0)
        return;

    const int ch = term->cell_height;
    const int x = term->margins.left;
    const int width = term->cols * term->cell_width;

    for (int r = 0; r < term->rows; r++) {
        if (!kg->render.above_rows[r])
            continue;

        const int y = term->margins.top + r * ch;

        for (size_t i = 0; i < kg->render.above_count; i++) {
            const struct kitty_render_entry *e = &kg->render.above[i];
            composite_clipped(e, pix, x, y, width, ch);

            int x1 = max(x, e->clip.x1);
            int y1 = max(y, e->clip.y1);
            int x2 = min(x + width, e->clip.x2);
            int y2 = min(y + ch, e->clip.y2);
            if (x1 < x2 && y1 < y2)
                pixman_region32_union_rect(damage, damage, x1, y1, x2 - x1, y2 - y1);
        }
    }
}

#if defined(_DEBUG)

#include "vt.h"

static void
test_init(struct terminal *term, struct config *conf, int fds[static 2])
{
    *conf = (struct config){
        .tweak = {
            .kitty_graphics = true,
            .kitty_graphics_storage_limit = 1024 * 1024,
        },
    };

    term_test_init(term, conf, 5, 10, 8);
    term->cell_width = 10;
    term->cell_height = 20;

    xassert(pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);
    term->ptmx = fds[1];
}

static void
test_fini(struct terminal *term, int fds[static 2])
{
    term_test_destroy(term);
    close(fds[0]);
    close(fds[1]);
}

static void
test_apc(struct terminal *term, const char *seq)
{
    char buf[8192];
    size_t n = xsnprintf(buf, sizeof(buf), "\033_G%s\033\\", seq);
    vt_from_slave(term, (const uint8_t *)buf, n);
}

static const char *
test_reply(int fd)
{
    static char buf[1024];
    ssize_t count = read(fd, buf, sizeof(buf) - 1);
    buf[count < 0 ? 0 : count] = '\0';
    return buf;
}

static char *
test_b64(const void *_data, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *data = _data;
    char *out = xmalloc((len + 2) / 3 * 4 + 1);
    size_t o = 0;

    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];

        out[o++] = alphabet[(v >> 18) & 0x3f];
        out[o++] = alphabet[(v >> 12) & 0x3f];
        out[o++] = i + 1 < len ? alphabet[(v >> 6) & 0x3f] : '=';
        out[o++] = i + 2 < len ? alphabet[v & 0x3f] : '=';
    }

    out[o] = '\0';
    return out;
}

static bool
test_reply_starts(int fd, const char *prefix)
{
    const char *reply = test_reply(fd);
    if (strncmp(reply, prefix, strlen(prefix)) == 0)
        return true;
    LOG_ERR("unexpected reply: %s", reply + 1);
    return false;
}

static size_t
test_image_count(const struct terminal *term)
{
    return term->kitty_gfx != NULL ? tll_length(term->kitty_gfx->images) : 0;
}

/* Control data parsing */
UNITTEST
{
    struct kitty_cmd cmd;

    const char *s = "a=T,f=24,s=10,v=20,z=-5,i=4294967295";
    xassert(parse_control(s, strlen(s), &cmd));
    xassert(action(&cmd) == 'T');
    xassert(val(&cmd, KEY_f) == 24);
    xassert(val(&cmd, KEY_s) == 10);
    xassert(val(&cmd, KEY_v) == 20);
    xassert(sval(&cmd, KEY_z) == -5);
    xassert(val(&cmd, KEY_i) == UINT32_MAX);
    xassert(!has(&cmd, KEY_p));

    /* Unknown keys are ignored */
    s = "W=1,a=p";
    xassert(parse_control(s, strlen(s), &cmd));
    xassert(action(&cmd) == 'p');

    /* Malformed */
    s = "a=Tx";
    xassert(!parse_control(s, strlen(s), &cmd));
    s = "i=-1";
    xassert(!parse_control(s, strlen(s), &cmd));
    s = "i=4294967296";
    xassert(!parse_control(s, strlen(s), &cmd));
    s = "i";
    xassert(!parse_control(s, strlen(s), &cmd));
    s = "s=1x";
    xassert(!parse_control(s, strlen(s), &cmd));
}

/* Base64, split at odd positions, and without padding */
UNITTEST
{
    struct kbuf out = {0};
    uint8_t quad[4];
    size_t quad_len = 0;

    /* "hello world" = aGVsbG8gd29ybGQ= */
    xassert(b64_decode_append((const uint8_t *)"aGVsb", 5, &out, quad, &quad_len, 1024));
    xassert(b64_decode_append((const uint8_t *)"G8gd29y", 7, &out, quad, &quad_len, 1024));
    xassert(b64_decode_append((const uint8_t *)"bGQ", 3, &out, quad, &quad_len, 1024));
    xassert(b64_decode_finish(&out, quad, &quad_len, 1024));
    xassert(out.len == 11);
    xassert(memcmp(out.data, "hello world", 11) == 0);
    free(out.data);

    /* Each chunk padded separately */
    out = (struct kbuf){0};
    xassert(b64_decode_append((const uint8_t *)"aGk=", 4, &out, quad, &quad_len, 1024));
    xassert(b64_decode_append((const uint8_t *)"IQ==", 4, &out, quad, &quad_len, 1024));
    xassert(b64_decode_finish(&out, quad, &quad_len, 1024));
    xassert(out.len == 3 && memcmp(out.data, "hi!", 3) == 0);
    free(out.data);

    /* Size limit */
    out = (struct kbuf){0};
    xassert(!b64_decode_append((const uint8_t *)"aGVsbG8gd29ybGQ=", 16, &out, quad, &quad_len, 4));
    free(out.data);
}

/* Transmit, put, query and delete */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    /* 2x2 RGB: red, green, blue, white */
    test_apc(&term, "a=t,i=1,f=24,s=2,v=2;/wAAAP8AAAD/////");
    xassert(streq(test_reply(fds[0]), "\033_Gi=1;OK\033\\"));
    xassert(test_image_count(&term) == 1);

    struct kitty_image *img = image_by_id(term.kitty_gfx, 1);
    xassert(img != NULL);
    xassert(img->width == 2 && img->height == 2);
    xassert(img->frames[0].data[0] == 0xffff0000);
    xassert(img->frames[0].data[1] == 0xff00ff00);
    xassert(img->frames[0].data[2] == 0xff0000ff);
    xassert(img->frames[0].data[3] == 0xffffffff);
    xassert(term.kitty_gfx->used_bytes == 16);

    /* Too little data */
    test_apc(&term, "a=t,i=2,f=24,s=2,v=2;/wAA");
    xassert(test_reply_starts(fds[0], "\033_Gi=2;ENODATA:"));
    xassert(test_image_count(&term) == 1);

    /* Quiet: no OK response */
    test_apc(&term, "a=t,i=3,f=24,s=1,v=1,q=1;AAAA");
    xassert(streq(test_reply(fds[0]), ""));
    xassert(test_image_count(&term) == 2);

    /* No ID: no response */
    test_apc(&term, "a=t,f=24,s=1,v=1;AAAA");
    xassert(streq(test_reply(fds[0]), ""));

    /* Query: verified, but not stored */
    const size_t count = test_image_count(&term);
    test_apc(&term, "a=q,i=31,s=1,v=1,t=d,f=24;AAAA");
    xassert(streq(test_reply(fds[0]), "\033_Gi=31;OK\033\\"));
    xassert(test_image_count(&term) == count);

    /* Put, with a placement ID; cursor moves past the image */
    test_apc(&term, "a=p,i=1,p=7");
    xassert(streq(test_reply(fds[0]), "\033_Gi=1,p=7;OK\033\\"));
    xassert(tll_length(term.normal.kitty_placements) == 1);
    xassert(term.normal.cursor.point.col == 1);
    xassert(term.normal.cursor.point.row == 0);

    struct kitty_placement *p = tll_front(term.normal.kitty_placements);
    xassert(p->img == img && p->id == 7);
    xassert(p->pos.col == 0 && p->pos.row == 0);

    /* Same placement ID replaces (moves) the placement */
    test_apc(&term, "a=p,i=1,p=7,c=3,r=2,C=1,q=1");
    xassert(tll_length(term.normal.kitty_placements) == 1);
    xassert(p->pos.col == 1);
    xassert(term.normal.cursor.point.col == 1);  /* C=1: not moved */

    struct geometry g;
    placement_geometry(&term, p, &g);
    xassert(g.cols == 3 && g.rows == 2);
    xassert(g.width == 30 && g.height == 30);   /* Fit, aspect preserved */

    /* Unknown image */
    test_apc(&term, "a=p,i=99");
    xassert(test_reply_starts(fds[0], "\033_Gi=99;ENOENT:"));

    /* Both i and I */
    test_apc(&term, "a=p,i=1,I=2");
    xassert(test_reply_starts(fds[0], "\033_Gi=1,I=2;EINVAL:"));

    /* Lowercase delete keeps the image data */
    test_apc(&term, "a=d,d=i,i=1");
    xassert(tll_length(term.normal.kitty_placements) == 0);
    xassert(image_by_id(term.kitty_gfx, 1) != NULL);

    /* Uppercase delete frees it */
    test_apc(&term, "a=p,i=1,q=1");
    xassert(tll_length(term.normal.kitty_placements) == 1);
    test_apc(&term, "a=d,d=I,i=1");
    xassert(tll_length(term.normal.kitty_placements) == 0);
    xassert(image_by_id(term.kitty_gfx, 1) == NULL);

    /* Delete by id range */
    test_apc(&term, "a=t,i=10,f=24,s=1,v=1,q=1;AAAA");
    test_apc(&term, "a=t,i=11,f=24,s=1,v=1,q=1;AAAA");
    test_apc(&term, "a=t,i=12,f=24,s=1,v=1,q=1;AAAA");
    test_apc(&term, "a=d,d=R,x=10,y=11");
    xassert(image_by_id(term.kitty_gfx, 10) == NULL);
    xassert(image_by_id(term.kitty_gfx, 11) == NULL);
    xassert(image_by_id(term.kitty_gfx, 12) != NULL);

    test_fini(&term, fds);
}

/* Image numbers, chunking, compression and PNG */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    /* Image number: the terminal allocates an ID */
    test_apc(&term, "a=t,I=13,f=24,s=1,v=1;AAAA");
    const char *reply = test_reply(fds[0]);
    xassert(strncmp(reply, "\033_Gi=", 5) == 0);
    xassert(strstr(reply, ",I=13;OK\033\\") != NULL);
    struct kitty_image *img = image_by_number(term.kitty_gfx, 13);
    xassert(img != NULL && img->id != 0);

    /* Chunked; the base64 group straddles chunks */
    test_apc(&term, "a=t,i=5,f=32,s=1,v=2,m=1;AAAA");
    test_apc(&term, "m=1;AP");
    xassert(test_image_count(&term) == 1);
    test_apc(&term, "m=0;8AAP8=");
    xassert(streq(test_reply(fds[0]), "\033_Gi=5;OK\033\\"));
    img = image_by_id(term.kitty_gfx, 5);
    xassert(img != NULL);
    xassert(img->frames[0].data[0] == 0);           /* Transparent black */
    xassert(img->frames[0].data[1] == 0xffff0000);  /* Opaque red */

    /* A delete aborts a chunked upload */
    test_apc(&term, "a=t,i=6,f=24,s=1,v=1,m=1;AA");
    test_apc(&term, "a=d,d=I,i=99");
    test_apc(&term, "m=0;AA");
    xassert(image_by_id(term.kitty_gfx, 6) == NULL);

    /* zlib compressed RGBA */
    {
        const uint8_t raw[8] = {0x80, 0x40, 0x20, 0x80, 0x00, 0x00, 0x00, 0x00};
        uint8_t compressed[64];
        uLongf clen = sizeof(compressed);
        xassert(compress(compressed, &clen, raw, sizeof(raw)) == Z_OK);

        char *b64 = test_b64(compressed, clen);
        char seq[256];
        xsnprintf(seq, sizeof(seq), "a=t,i=7,f=32,s=2,v=1,o=z;%s", b64);
        free(b64);

        test_apc(&term, seq);
        xassert(streq(test_reply(fds[0]), "\033_Gi=7;OK\033\\"));
        img = image_by_id(term.kitty_gfx, 7);
        xassert(img != NULL);
        /* Premultiplied: 0x80 * 0x80 / 0xff = 0x40 */
        xassert(img->frames[0].data[0] == 0x80402010);
    }

    /* PNG (1x1) */
    test_apc(&term, "a=t,i=8,f=100;iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==");
    xassert(streq(test_reply(fds[0]), "\033_Gi=8;OK\033\\"));
    img = image_by_id(term.kitty_gfx, 8);
    xassert(img != NULL && img->width == 1 && img->height == 1);

    /* Broken PNG */
    test_apc(&term, "a=t,i=9,f=100;iVBORw0KGgoAAAA=");
    xassert(test_reply_starts(fds[0], "\033_Gi=9;EBADPNG:"));

    /* Files: special files are refused, with a generic error */
    char *dev_null = test_b64("/dev/null", 9);
    char seq[256];
    xsnprintf(seq, sizeof(seq), "a=t,i=20,f=24,s=1,v=1,t=f;%s", dev_null);
    free(dev_null);
    test_apc(&term, seq);
    xassert(streq(test_reply(fds[0]), "\033_Gi=20;EBADF:Failed to read image file\033\\"));

    /* Regular temporary file; deleted after reading */
    {
        char path[] = "/tmp/ankle-tty-graphics-protocol-XXXXXX";
        int fd = mkstemp(path);
        xassert(fd >= 0);
        xassert(write(fd, "\xff\x00\x00", 3) == 3);
        close(fd);

        char *b64 = test_b64(path, strlen(path));
        xsnprintf(seq, sizeof(seq), "a=t,i=21,f=24,s=1,v=1,t=t;%s", b64);
        free(b64);

        test_apc(&term, seq);
        xassert(streq(test_reply(fds[0]), "\033_Gi=21;OK\033\\"));
        xassert(access(path, F_OK) < 0);
        img = image_by_id(term.kitty_gfx, 21);
        xassert(img != NULL && img->frames[0].data[0] == 0xffff0000);
    }

    test_fini(&term, fds);
}

/* Feature queries */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    test_apc(&term, "a=q,i=31,e=T,U=1,C=1");
    const char *reply = test_reply(fds[0]);
    xassert(strncmp(reply, "\033_Gi=31,e=T,", 12) == 0);
    xassert(strstr(reply, ",U=1") != NULL);
    xassert(strstr(reply, ",C=1") != NULL);
    xassert(strstr(reply, ";OK\033\\") != NULL);

    test_apc(&term, "a=q,i=32,e=f,r=1,X=1");
    reply = test_reply(fds[0]);
    xassert(strstr(reply, "e=f") != NULL);
    xassert(strstr(reply, ",r=1") != NULL);
    xassert(strstr(reply, ",X=1") != NULL);

    /* Unknown action */
    test_apc(&term, "a=q,i=33,e=Z");
    xassert(streq(test_reply(fds[0]), "\033_Gi=33;OK\033\\"));

    /* No image is transmitted */
    xassert(test_image_count(&term) == 0);

    test_fini(&term, fds);
}

/* Scrolling, erasing and relative placements */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    test_apc(&term, "a=t,i=1,f=24,s=1,v=1,q=1;AAAA");
    test_apc(&term, "a=p,i=1,p=1,q=1");
    xassert(tll_length(term.normal.kitty_placements) == 1);

    /* Relative placement, two rows below */
    test_apc(&term, "a=p,i=1,p=2,P=1,Q=1,V=2,q=1");
    xassert(tll_length(term.normal.kitty_placements) == 2);

    /* Cycles are refused */
    test_apc(&term, "a=p,i=1,p=1,P=1,Q=2");
    xassert(test_reply_starts(fds[0], "\033_Gi=1,p=1;ECYCLE:"));

    /* Missing parent */
    test_apc(&term, "a=p,i=1,p=3,P=1,Q=9");
    xassert(test_reply_starts(fds[0], "\033_Gi=1,p=3;ENOPARENT:"));

    struct kitty_placement *p = tll_front(term.normal.kitty_placements);
    const int abs_row = p->pos.row;

    /* Placements move with the text; the absolute row doesn't change */
    term_test_write(&term, "\r\n\r\n\r\n\r\n\r\n");
    xassert(p->pos.row == abs_row);

    int row, col;
    xassert(placement_position(&term, &term.normal, p, &row, &col, 0));
    xassert(row == -1 && col == 0);

    /* Scrolled out of the scrollback: deleted, along with the
     * relative placement */
    term_test_write(&term, "\r\n\r\n");
    xassert(tll_length(term.normal.kitty_placements) == 2);
    term_test_write(&term, "\r\n");
    xassert(tll_length(term.normal.kitty_placements) == 0);

    /* ED 2 erases visible placements */
    test_apc(&term, "a=p,i=1,q=1");
    xassert(tll_length(term.normal.kitty_placements) == 1);
    kitty_erase_screen(&term);
    xassert(tll_length(term.normal.kitty_placements) == 0);

    /* Delete by cell */
    term_cursor_to(&term, 2, 3);
    test_apc(&term, "a=p,i=1,c=2,r=2,q=1");
    test_apc(&term, "a=d,d=p,x=1,y=1");
    xassert(tll_length(term.normal.kitty_placements) == 1);
    test_apc(&term, "a=d,d=p,x=5,y=4");
    xassert(tll_length(term.normal.kitty_placements) == 0);

    /* Partial scroll; the placement is entirely inside the region */
    term_cursor_to(&term, 2, 0);
    test_apc(&term, "a=p,i=1,r=2,q=1,C=1");
    p = tll_front(term.normal.kitty_placements);
    term.scroll_region = (struct scroll_region){.start = 1, .end = 4};
    term_scroll(&term, 1);
    xassert(placement_position(&term, &term.normal, p, &row, &col, 0));
    xassert(row == 1);
    xassert(p->clip_top == 0);
    term_scroll(&term, 1);
    xassert(placement_position(&term, &term.normal, p, &row, &col, 0));
    xassert(row == 0 && p->clip_top == 1);
    term_scroll(&term, 1);
    xassert(tll_length(term.normal.kitty_placements) == 0);
    term.scroll_region = (struct scroll_region){.start = 0, .end = term.rows};

    test_fini(&term, fds);
}

/* An image taller than the scroll region, placed with cursor movement,
 * scrolls the region at most once over (used to trip an assertion in
 * term_scroll_partial(), and spin in release builds) */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    test_apc(&term, "a=t,i=1,f=24,s=1,v=1,q=1;AAAA");

    term.scroll_region = (struct scroll_region){.start = 1, .end = 4};
    term_cursor_to(&term, 2, 0);
    test_apc(&term, "a=p,i=1,r=40,c=2,q=2");
    xassert(term.grid->cursor.point.row == 3);
    xassert(term.grid->cursor.point.col == 2);

    term.scroll_region = (struct scroll_region){.start = 0, .end = term.rows};
    term_cursor_to(&term, 0, 0);
    test_apc(&term, "a=p,i=1,r=51,c=1,q=2");
    xassert(term.grid->cursor.point.row == term.rows - 1);

    test_fini(&term, fds);
}

/* Animation frames and composition */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    test_apc(&term, "a=t,i=1,f=24,s=2,v=1,q=1;AAAAAAAA");

    /* New frame, with a red background, and a green pixel */
    test_apc(&term, "a=f,i=1,f=24,x=1,s=1,v=1,Y=4278190335;AP8A");
    xassert(streq(test_reply(fds[0]), "\033_Gi=1;OK\033\\"));

    struct kitty_image *img = image_by_id(term.kitty_gfx, 1);
    xassert(img->frame_count == 2);
    xassert(img->frames[1].data[0] == 0xffff0000);
    xassert(img->frames[1].data[1] == 0xff00ff00);
    xassert(img->frames[1].gap == KITTY_DEFAULT_FRAME_GAP);
    xassert(term.kitty_gfx->used_bytes == 16);

    /* Make frame 2 current */
    const uint64_t gen = img->generation;
    test_apc(&term, "a=a,i=1,c=2,q=1");
    xassert(img->current == 1);
    xassert(img->generation != gen);

    /* Compose: frame 2's green pixel onto frame 1 */
    test_apc(&term, "a=c,i=1,r=2,c=1,X=1,w=1,h=1,q=1");
    xassert(img->frames[0].data[0] == 0xff00ff00);

    /* Out of bounds */
    test_apc(&term, "a=c,i=1,r=2,c=1,w=3,h=1");
    xassert(test_reply_starts(fds[0], "\033_Gi=1;EINVAL:"));

    /* Animation stepping */
    img->anim_state = KITTY_ANIM_RUNNING;
    img->current = 0;
    xassert(anim_advance(img) && img->current == 1);
    xassert(anim_advance(img) && img->current == 0);

    img->loops_wanted = 2;   /* Loop once */
    img->loops_done = 0;
    img->current = 1;
    xassert(anim_advance(img) && img->current == 0);
    xassert(anim_advance(img) && img->current == 1);
    xassert(!anim_advance(img));
    xassert(img->anim_state == KITTY_ANIM_STOPPED);

    /* Delete frames */
    test_apc(&term, "a=d,d=f,i=1");
    xassert(img->frame_count == 1);
    xassert(term.kitty_gfx->used_bytes == 8);

    test_fini(&term, fds);
}

/* Storage quota */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);
    conf.tweak.kitty_graphics_storage_limit = 4096;

    /* 16x16 RGBA = 1024 bytes */
    char *zeros = xcalloc(1, 1024);
    char *b64 = test_b64(zeros, 1024);
    free(zeros);

    char *seq = xmalloc(strlen(b64) + 64);
    for (int i = 1; i <= 4; i++) {
        sprintf(seq, "a=t,i=%d,s=16,v=16,q=1;%s", i, b64);
        test_apc(&term, seq);
    }
    xassert(test_image_count(&term) == 4);

    /* Image 1 is displayed; 2 is the oldest unplaced image */
    test_apc(&term, "a=p,i=1,q=1");

    sprintf(seq, "a=t,i=5,s=16,v=16,q=1;%s", b64);
    test_apc(&term, seq);
    xassert(test_image_count(&term) == 4);
    xassert(image_by_id(term.kitty_gfx, 1) != NULL);
    xassert(image_by_id(term.kitty_gfx, 2) == NULL);
    xassert(image_by_id(term.kitty_gfx, 5) != NULL);

    /* Larger than the entire quota */
    test_apc(&term, "a=t,i=6,s=64,v=64,f=24;AAAA");
    xassert(test_reply_starts(fds[0], "\033_Gi=6;ENOSPC:"));

    free(seq);
    free(b64);
    test_fini(&term, fds);
}

/* Unicode placeholders: diacritics, and inheritance from the left */
UNITTEST
{
    struct config conf;
    struct terminal term;
    int fds[2];
    test_init(&term, &conf, fds);

    xassert(diacritic_value(0x0305) == 0);
    xassert(diacritic_value(0x030d) == 1);
    xassert(diacritic_value(0x030e) == 2);
    xassert(diacritic_value(0x0041) == -1);

    /* Placeholder with row=1 (U+030D), col=0 (U+0305) */
    char32_t chars[] = {KITTY_PLACEHOLDER, 0x030d, 0x0305};
    struct composed *c = xmalloc(sizeof(*c));
    *c = (struct composed){
        .chars = xmemdup(chars, sizeof(chars)),
        .count = 3,
        .key = composed_key_from_chars(chars, 3),
        .width = 1,
    };
    composed_insert(&term.composed, c);

    struct row *row = term.normal.rows[0];
    const struct attributes attrs = {.fg = 42, .fg_src = COLOR_BASE256};

    row->cells[2] = (struct cell){.wc = CELL_COMB_CHARS_LO + c->key, .attrs = attrs};
    row->cells[3] = (struct cell){.wc = KITTY_PLACEHOLDER, .attrs = attrs};
    row->cells[4] = (struct cell){.wc = KITTY_PLACEHOLDER, .attrs = attrs};

    /* Different image ID; does not inherit */
    row->cells[5] = (struct cell){.wc = KITTY_PLACEHOLDER,
                                  .attrs = {.fg = 43, .fg_src = COLOR_BASE256}};

    xassert(kitty_is_placeholder(&term, &row->cells[2]));
    xassert(kitty_is_placeholder(&term, &row->cells[3]));
    xassert(!kitty_is_placeholder(&term, &row->cells[1]));

    struct placeholder ph;
    placeholder_resolve(&term, row, 2, &ph);
    xassert(ph.fg == 42 && ph.row == 1 && ph.col == 0 && ph.msb == 0);

    placeholder_resolve(&term, row, 4, &ph);
    xassert(ph.fg == 42 && ph.row == 1 && ph.col == 2);

    placeholder_resolve(&term, row, 5, &ph);
    xassert(ph.fg == 43 && ph.row == 0 && ph.col == 0);

    /* Virtual placement, and lookup by placeholder */
    test_apc(&term, "a=T,U=1,i=42,c=3,r=2,f=24,s=1,v=1,q=1;AAAA");
    xassert(term.kitty_gfx->virtual_count == 1);
    xassert(tll_length(term.normal.kitty_placements) == 0);
    xassert(term.normal.cursor.point.col == 0);   /* Cursor not moved */

    struct kitty_virtual *v;
    placeholder_resolve(&term, row, 4, &ph);
    xassert(placeholder_image(&term, &ph, &v) == image_by_id(term.kitty_gfx, 42));
    xassert(v->cols == 3 && v->rows == 2);

    test_apc(&term, "a=d,d=I,i=42");
    xassert(term.kitty_gfx->virtual_count == 0);

    composed_free(term.composed);
    test_fini(&term, fds);
}

#endif /* _DEBUG */
