#pragma once

/*
 * Custom (post-processing) shaders, compatible with Ghostty's
 * "custom-shader" option.
 *
 * The terminal is still rendered on the CPU, with pixman, into the
 * usual shm buffer. When one or more shaders are configured, the
 * finished frame is uploaded to an OpenGL texture (damaged areas
 * only), run through the shader chain, and presented with EGL
 * instead of being attached to the surface directly.
 *
 * Without any configured shaders, none of this is initialized, and
 * the regular wl_shm path is used.
 */

#include <stdbool.h>
#include <stdint.h>

struct terminal;
struct buffer;
struct wayland;

/* Cursor styles, as exposed to shaders (Ghostty's CURSORSTYLE_*) */
enum shader_cursor_style {
    SHADER_CURSOR_BLOCK = 0,
    SHADER_CURSOR_BLOCK_HOLLOW = 1,
    SHADER_CURSOR_BAR = 2,
    SHADER_CURSOR_UNDERLINE = 3,
};

/* Per-frame terminal state, collected by the renderer */
struct shader_frame_info {
    /* Cursor is drawn this frame. When false, the previous cursor
     * rectangle is kept (e.g. blinking off) */
    bool cursor_drawn;

    /* Cursor enabled by the application (DECTCEM) */
    bool cursor_visible;

    /* Cursor rectangle, in buffer pixels, top-left origin */
    int cursor_x;
    int cursor_y;
    int cursor_width;
    int cursor_height;

    uint32_t cursor_color;  /* 0xRRGGBB */
    enum shader_cursor_style cursor_style;

    bool focused;
};

#if defined(ANKLE_SHADERS_ENABLED) && ANKLE_SHADERS_ENABLED

/* True if shaders are configured for this terminal */
bool shader_configured(const struct terminal *term);

/*
 * Present 'buf' through the shader chain. Returns false if shaders
 * are not in use (not configured, failed to initialize, or none
 * compiled); the caller must then attach the buffer as usual.
 *
 * On success, the surface has been committed, and 'buf' is no longer
 * needed by the compositor.
 */
bool shader_present(struct terminal *term, struct buffer *buf,
                    const struct shader_frame_info *info);

/* True if an animation frame should be rendered (the terminal content
 * itself hasn't changed) */
bool shader_wants_animation_frame(const struct terminal *term);

/* Re-run the shader chain on the last uploaded frame, and commit */
bool shader_render_animation_frame(struct terminal *term);

/* Configuration (or shader files) changed; recompile on next frame */
void shader_reload(struct terminal *term);

/* Release all GL/EGL resources owned by the terminal's window */
void shader_destroy(struct terminal *term);

/* Release global EGL state. Must be called before disconnecting from
 * the Wayland display */
void shader_fini(struct wayland *wayl);

#else

static inline bool shader_configured(const struct terminal *term) { return false; }
static inline bool shader_present(struct terminal *term, struct buffer *buf,
                                  const struct shader_frame_info *info) { return false; }
static inline bool shader_wants_animation_frame(const struct terminal *term) { return false; }
static inline bool shader_render_animation_frame(struct terminal *term) { return false; }
static inline void shader_reload(struct terminal *term) {}
static inline void shader_destroy(struct terminal *term) {}
static inline void shader_fini(struct wayland *wayl) {}

#endif
