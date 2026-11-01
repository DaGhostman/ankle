#pragma once

/*
 * Kitty terminal graphics protocol
 * (https://sw.kovidgoyal.net/kitty/graphics-protocol/)
 *
 * Images are stored per terminal (shared by the normal and alternate
 * screens), while placements belong to a grid, and are anchored to
 * an absolute grid row, just like sixels. That way they scroll
 * with the text without any extra work.
 *
 * Unlike sixels, kitty placements are *not* erased when text is
 * printed over them. They can be drawn either above, or below the
 * text (negative z-index).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include <pixman.h>
#include <tllist.h>

#include "terminal.h"

/* The Unicode placeholder character */
#define KITTY_PLACEHOLDER 0x10eeeeu

/* Default storage quota, in MB */
#define KITTY_DEFAULT_STORAGE_LIMIT_MB 320u

#define KITTY_MAX_WIDTH  10000u
#define KITTY_MAX_HEIGHT 10000u

/* Limit the size of a single APC sequence (base64 data) */
#define KITTY_MAX_APC_SIZE (64u * 1024u * 1024u)

struct kitty_frame {
    pixman_image_t *pix;   /* PIXMAN_a8r8g8b8, premultiplied */
    uint32_t *data;
    int32_t gap;           /* Milliseconds. Negative means "gapless" */
};

struct kitty_virtual {
    uint32_t id;           /* Placement ID */
    int cols;
    int rows;

    /* Scaled image cache, for the current cell size */
    pixman_image_t *cache;
    uint64_t cache_gen;
    int cache_cell_w;
    int cache_cell_h;
    int box_x;             /* Image offset within the cols*rows box */
    int box_y;

    /* Position of the top-left placeholder cell, in screen
     * coordinates. Updated each frame; only valid if 'visible' */
    bool visible;
    int vis_row;
    int vis_col;
};

enum kitty_anim_state {
    KITTY_ANIM_STOPPED,
    KITTY_ANIM_LOADING,
    KITTY_ANIM_RUNNING,
};

struct kitty_image {
    uint32_t id;
    uint32_t number;
    int width;
    int height;

    struct kitty_frame *frames;
    size_t frame_count;
    size_t current;        /* Index of the currently displayed frame */

    /* Bumped whenever the displayed pixels change */
    uint64_t generation;

    enum kitty_anim_state anim_state;
    uint32_t loops_wanted; /* 0 = infinite */
    uint32_t loops_done;
    struct timespec next_frame_at;

    size_t bytes;          /* Pixel data size, accounted in the quota */
    uint64_t atime;        /* Last-used "time", for eviction */

    tll(struct kitty_virtual) virtuals;
};

struct kitty_placement {
    struct kitty_image *img;
    uint32_t id;           /* Placement ID, 0 if none */

    struct coord pos;      /* col, and *absolute* grid row */
    int x_off;             /* Pixel offset within the first cell */
    int y_off;

    int src_x, src_y, src_w, src_h;
    int req_cols;          /* Requested columns/rows; 0 means "auto" */
    int req_rows;

    int32_t z;
    uint64_t seq;          /* Creation order */

    /* Cell rows cut off by partial (scroll region) scrolling */
    int clip_top;
    int clip_bottom;

    /* Relative placements */
    struct kitty_placement *parent;
    struct kitty_image *parent_virtual_img;  /* Parent is a virtual placement */
    uint32_t parent_virtual_id;
    int32_t off_h;
    int32_t off_v;

    /* Scaled image cache */
    pixman_image_t *cache;
    uint64_t cache_gen;
    int cache_w;
    int cache_h;
};

struct kitty_render_entry {
    pixman_image_t *pix;
    int x, y;              /* Screen position of the image's top-left pixel */
    pixman_box32_t clip;   /* Visible part, in screen coordinates */
    int32_t z;
    uint32_t id;
    uint64_t seq;
    bool under_bg;         /* z < INT32_MIN / 2 */
};

struct kitty_graphics {
    tll(struct kitty_image *) images;  /* Oldest first */
    uint32_t next_id;
    uint64_t clock;
    uint64_t placement_seq;
    size_t used_bytes;
    size_t virtual_count;

    /* Chunked transmission in progress */
    struct {
        bool active;
        bool failed;
        void *cmd;         /* struct kitty_cmd */
        uint8_t *data;
        size_t len;
        size_t size;
        uint8_t b64_quad[4];
        size_t b64_len;
    } upload;

    int anim_fd;

    /* Per-frame render state, built on the main thread before the
     * render workers are started. Read-only while rendering */
    struct {
        struct kitty_render_entry *below;
        size_t below_count;
        size_t below_size;

        struct kitty_render_entry *above;
        size_t above_count;
        size_t above_size;

        bool *above_rows;  /* Rows needing the "above" pass */
        int above_rows_count;
    } render;
};

/* Called with the APC payload, minus the leading 'G' */
void kitty_graphics_command(struct terminal *term, const uint8_t *data, size_t len);

void kitty_graphics_fini(struct terminal *term);
void kitty_graphics_reset(struct terminal *term);

/* Grid/screen hooks */
void kitty_scroll_up(struct terminal *term, int rows);
void kitty_scroll_down(struct terminal *term, int rows);
void kitty_scroll_partial(
    struct terminal *term, struct scroll_region region, int rows, bool reverse);
void kitty_erase_screen(struct terminal *term);
void kitty_erase_scrollback(struct terminal *term);
void kitty_clear_grid(struct terminal *term, struct grid *grid);
void kitty_grid_free(struct grid *grid);
void kitty_cell_size_changed(struct terminal *term);
void kitty_placement_destroy_unlinked(struct grid *grid, struct kitty_placement *p);

/* Number of cell rows a placement spans */
int kitty_placement_rows(const struct terminal *term, const struct kitty_placement *p);

/* Rendering */
void kitty_render_prepare(struct terminal *term);
void kitty_render_below(
    const struct terminal *term, pixman_image_t *pix, int x, int y,
    int width, int height, bool default_bg);
bool kitty_is_placeholder(const struct terminal *term, const struct cell *cell);
void kitty_render_placeholder(
    struct terminal *term, pixman_image_t *pix, const struct row *row,
    int col, int x, int y);
void kitty_render_above(
    struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage);

static inline bool
kitty_has_below(const struct terminal *term)
{
    return term->kitty_gfx != NULL && term->kitty_gfx->render.below_count > 0;
}

static inline bool
kitty_has_virtual(const struct terminal *term)
{
    return term->kitty_gfx != NULL && term->kitty_gfx->virtual_count > 0;
}
