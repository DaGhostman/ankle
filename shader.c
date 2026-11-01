#include "shader.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

#include <pixman.h>

#define LOG_MODULE "shader"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "macros.h"
#include "shm.h"
#include "terminal.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

/* Upper limit on a shader file; same as Ghostty */
#define MAX_SHADER_SIZE (4 * 1024 * 1024)

enum uniform {
    U_RESOLUTION,
    U_TIME,
    U_TIME_DELTA,
    U_FRAME_RATE,
    U_FRAME,
    U_CHANNEL_TIME,
    U_CHANNEL_RESOLUTION,
    U_MOUSE,
    U_DATE,
    U_SAMPLE_RATE,
    U_CURRENT_CURSOR,
    U_PREVIOUS_CURSOR,
    U_CURRENT_CURSOR_COLOR,
    U_PREVIOUS_CURSOR_COLOR,
    U_CURRENT_CURSOR_STYLE,
    U_PREVIOUS_CURSOR_STYLE,
    U_CURSOR_VISIBLE,
    U_TIME_CURSOR_CHANGE,
    U_TIME_FOCUS,
    U_FOCUS,
    U_PALETTE,
    U_BACKGROUND_COLOR,
    U_FOREGROUND_COLOR,
    U_CURSOR_COLOR,
    U_CURSOR_TEXT,
    U_SELECTION_BACKGROUND_COLOR,
    U_SELECTION_FOREGROUND_COLOR,
    U_CHANNEL0,
    U_COUNT,
};

static const char *const uniform_names[U_COUNT] = {
    [U_RESOLUTION] = "iResolution",
    [U_TIME] = "iTime",
    [U_TIME_DELTA] = "iTimeDelta",
    [U_FRAME_RATE] = "iFrameRate",
    [U_FRAME] = "iFrame",
    [U_CHANNEL_TIME] = "iChannelTime",
    [U_CHANNEL_RESOLUTION] = "iChannelResolution",
    [U_MOUSE] = "iMouse",
    [U_DATE] = "iDate",
    [U_SAMPLE_RATE] = "iSampleRate",
    [U_CURRENT_CURSOR] = "iCurrentCursor",
    [U_PREVIOUS_CURSOR] = "iPreviousCursor",
    [U_CURRENT_CURSOR_COLOR] = "iCurrentCursorColor",
    [U_PREVIOUS_CURSOR_COLOR] = "iPreviousCursorColor",
    [U_CURRENT_CURSOR_STYLE] = "iCurrentCursorStyle",
    [U_PREVIOUS_CURSOR_STYLE] = "iPreviousCursorStyle",
    [U_CURSOR_VISIBLE] = "iCursorVisible",
    [U_TIME_CURSOR_CHANGE] = "iTimeCursorChange",
    [U_TIME_FOCUS] = "iTimeFocus",
    [U_FOCUS] = "iFocus",
    [U_PALETTE] = "iPalette",
    [U_BACKGROUND_COLOR] = "iBackgroundColor",
    [U_FOREGROUND_COLOR] = "iForegroundColor",
    [U_CURSOR_COLOR] = "iCursorColor",
    [U_CURSOR_TEXT] = "iCursorText",
    [U_SELECTION_BACKGROUND_COLOR] = "iSelectionBackgroundColor",
    [U_SELECTION_FOREGROUND_COLOR] = "iSelectionForegroundColor",
    [U_CHANNEL0] = "iChannel0",
};

/*
 * Prepended to every user shader. Declares the same uniforms (names
 * and types) as Ghostty's shadertoy prefix, so that shaders written
 * for Ghostty compile unmodified.
 */
static const char prelude[] =
    "uniform vec3  iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform float iFrameRate;\n"
    "uniform int   iFrame;\n"
    "uniform float iChannelTime[4];\n"
    "uniform vec3  iChannelResolution[4];\n"
    "uniform vec4  iMouse;\n"
    "uniform vec4  iDate;\n"
    "uniform float iSampleRate;\n"
    "uniform vec4  iCurrentCursor;\n"
    "uniform vec4  iPreviousCursor;\n"
    "uniform vec4  iCurrentCursorColor;\n"
    "uniform vec4  iPreviousCursorColor;\n"
    "uniform int   iCurrentCursorStyle;\n"
    "uniform int   iPreviousCursorStyle;\n"
    "uniform int   iCursorVisible;\n"
    "uniform float iTimeCursorChange;\n"
    "uniform float iTimeFocus;\n"
    "uniform int   iFocus;\n"
    "uniform vec3  iPalette[256];\n"
    "uniform vec3  iBackgroundColor;\n"
    "uniform vec3  iForegroundColor;\n"
    "uniform vec3  iCursorColor;\n"
    "uniform vec3  iCursorText;\n"
    "uniform vec3  iSelectionForegroundColor;\n"
    "uniform vec3  iSelectionBackgroundColor;\n"
    "\n"
    "#define CURSORSTYLE_BLOCK        0\n"
    "#define CURSORSTYLE_BLOCK_HOLLOW 1\n"
    "#define CURSORSTYLE_BAR          2\n"
    "#define CURSORSTYLE_UNDERLINE    3\n"
    "#define CURSORSTYLE_LOCK         4\n"
    "\n"
    "uniform sampler2D iChannel0;\n"
    "\n"
    "out vec4 _ankle_frag_color;\n"
    "\n"
    "#define texture2D texture\n"
    "\n"
    "void mainImage(out vec4 fragColor, in vec2 fragCoord);\n"
    "void main() { mainImage(_ankle_frag_color, gl_FragCoord.xy); }\n"
    "\n"
    "#line 1\n";

/* A single triangle covering the whole viewport; no vertex data */
static const char vertex_shader_src[] =
    "#version 330 core\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

struct program {
    GLuint id;
    GLint loc[U_COUNT];
};

struct shader_state {
    /* Shaders (re)compiled on next frame */
    bool need_reload;

    /* Shaders configured, but none usable; regular shm path is used */
    bool unusable;

    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;

    int width;
    int height;
    pixman_format_code_t format;

    /* Next upload must be a full one */
    bool full_upload;

    /* Have we presented anything yet? */
    bool have_frame;

    GLuint vao;
    GLuint vertex_shader;

    GLuint upload_tex;   /* Last frame, as rendered by pixman (top-down) */
    GLuint source_tex;   /* Same, flipped (bottom-up, like Shadertoy) */
    GLuint pass_tex[2];  /* Ping-pong buffers, when chaining shaders */
    GLuint upload_fbo;
    GLuint source_fbo;
    GLuint pass_fbo[2];

    struct program *programs;
    size_t program_count;

    /* Uniform state */
    struct timespec first_frame_time;
    struct timespec last_frame_time;
    struct timespec last_activity;
    float time;
    float time_delta;
    float frame_rate;
    int frame;

    float current_cursor[4];
    float previous_cursor[4];
    float current_cursor_color[4];
    float previous_cursor_color[4];
    int current_cursor_style;
    int previous_cursor_style;
    bool cursor_visible;
    float cursor_change_time;

    bool focused;
    float time_focus;
};

static struct {
    bool initialized;
    bool failed;
    struct wl_display *wl_display;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    int glsl_version;
} egl;

bool
shader_configured(const struct terminal *term)
{
    return term->conf->custom_shader.count > 0;
}

static double
timespec_to_secs(const struct timespec *ts)
{
    return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

static double
secs_between(const struct timespec *from, const struct timespec *to)
{
    return timespec_to_secs(to) - timespec_to_secs(from);
}

static bool
egl_init(struct wl_display *wl_display)
{
    if (egl.initialized)
        return true;
    if (egl.failed)
        return false;

    egl.failed = true;  /* Until proven otherwise */

    egl.display = eglGetPlatformDisplay(
        EGL_PLATFORM_WAYLAND_KHR, wl_display, NULL);

    if (egl.display == EGL_NO_DISPLAY) {
        LOG_ERR("failed to get an EGL display; custom shaders disabled");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl.display, &major, &minor)) {
        LOG_ERR("failed to initialize EGL (0x%04x); custom shaders disabled",
                eglGetError());
        return false;
    }

    LOG_INFO("EGL %d.%d (%s)", major, minor,
             eglQueryString(egl.display, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_ERR("EGL: OpenGL not supported; custom shaders disabled");
        goto err;
    }

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };

    EGLint count = 0;
    if (!eglChooseConfig(egl.display, config_attribs, &egl.config, 1, &count) ||
        count == 0)
    {
        LOG_ERR("EGL: no suitable framebuffer configuration; "
                "custom shaders disabled");
        goto err;
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };

    egl.context = eglCreateContext(
        egl.display, egl.config, EGL_NO_CONTEXT, context_attribs);

    if (egl.context == EGL_NO_CONTEXT) {
        LOG_ERR("EGL: failed to create an OpenGL 3.3 core context (0x%04x); "
                "custom shaders disabled", eglGetError());
        goto err;
    }

    egl.wl_display = wl_display;
    egl.initialized = true;
    egl.failed = false;
    return true;

err:
    eglTerminate(egl.display);
    egl.display = EGL_NO_DISPLAY;
    return false;
}

void
shader_fini(struct wayland *wayl)
{
    if (!egl.initialized)
        return;

    const bool token = wayl_read_suspend(wayl);

    eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl.display, egl.context);
    eglTerminate(egl.display);
    eglReleaseThread();

    egl.initialized = false;
    egl.display = EGL_NO_DISPLAY;
    egl.context = EGL_NO_CONTEXT;

    wayl_read_resume(wayl, token);
}

static bool
make_current(struct shader_state *state)
{
    if (!eglMakeCurrent(egl.display, state->egl_surface,
                        state->egl_surface, egl.context))
    {
        LOG_ERR("EGL: failed to make context current (0x%04x)", eglGetError());
        return false;
    }
    return true;
}

static void
detect_glsl_version(void)
{
    if (egl.glsl_version != 0)
        return;

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    LOG_INFO("OpenGL: %s (%s)",
             (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_RENDERER));

    /* Ghostty compiles shaders as GLSL 4.30; use that when we can */
    egl.glsl_version = (major > 4 || (major == 4 && minor >= 3)) ? 430 : 330;
}

static GLuint
compile(GLenum type, const char *const *srcs, size_t count, const char *name)
{
    GLuint id = glCreateShader(type);
    glShaderSource(id, count, srcs, NULL);
    glCompileShader(id);

    GLint ok = GL_FALSE;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);

    if (!ok) {
        GLint len = 0;
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &len);

        char *log = xmalloc(len > 0 ? len : 1);
        log[0] = '\0';
        glGetShaderInfoLog(id, len, NULL, log);

        LOG_ERR("%s: failed to compile shader:\n%s", name, log);
        free(log);
        glDeleteShader(id);
        return 0;
    }

    return id;
}

static char *
read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    char *data = NULL;
    size_t size = 0;
    size_t cap = 0;

    while (true) {
        if (size + 4096 + 1 > cap) {
            cap = cap == 0 ? 8192 : cap * 2;
            data = xrealloc(data, cap);
        }

        size_t n = fread(&data[size], 1, cap - size - 1, f);
        size += n;

        if (size > MAX_SHADER_SIZE) {
            fclose(f);
            free(data);
            errno = EFBIG;
            return NULL;
        }

        if (n == 0)
            break;
    }

    bool error = ferror(f);
    fclose(f);

    if (error) {
        free(data);
        errno = EIO;
        return NULL;
    }

    data[size] = '\0';
    *len = size;
    return data;
}

static bool
program_load(struct shader_state *state, const char *path,
             struct program *prog)
{
    size_t len;
    char *src = read_file(path, &len);
    if (src == NULL) {
        LOG_ERRNO("%s: failed to read shader", path);
        return false;
    }

    /* We supply our own #version; neutralize any in the shader */
    for (char *p = strstr(src, "#version"); p != NULL; p = strstr(p, "#version")) {
        /* Only at the start of a line */
        if (p == src || p[-1] == '\n') {
            p[0] = '/';
            p[1] = '/';
        }
        p += 2;
    }

    char version[32];
    snprintf(version, sizeof(version), "#version %d core\n", egl.glsl_version);

    const char *srcs[] = {version, prelude, src};
    GLuint fs = compile(GL_FRAGMENT_SHADER, srcs, ALEN(srcs), path);
    free(src);

    if (fs == 0)
        return false;

    GLuint id = glCreateProgram();
    glAttachShader(id, state->vertex_shader);
    glAttachShader(id, fs);
    glLinkProgram(id);
    glDetachShader(id, state->vertex_shader);
    glDetachShader(id, fs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);

    if (!ok) {
        GLint log_len = 0;
        glGetProgramiv(id, GL_INFO_LOG_LENGTH, &log_len);

        char *log = xmalloc(log_len > 0 ? log_len : 1);
        log[0] = '\0';
        glGetProgramInfoLog(id, log_len, NULL, log);

        LOG_ERR("%s: failed to link shader:\n%s", path, log);
        free(log);
        glDeleteProgram(id);
        return false;
    }

    prog->id = id;
    for (size_t i = 0; i < U_COUNT; i++)
        prog->loc[i] = glGetUniformLocation(id, uniform_names[i]);

    LOG_INFO("loaded custom shader: %s", path);
    return true;
}

static void
programs_free(struct shader_state *state)
{
    for (size_t i = 0; i < state->program_count; i++)
        glDeleteProgram(state->programs[i].id);
    free(state->programs);
    state->programs = NULL;
    state->program_count = 0;
}

static void
programs_load(struct terminal *term, struct shader_state *state)
{
    programs_free(state);

    const struct config *conf = term->conf;
    state->programs = xcalloc(conf->custom_shader.count, sizeof(state->programs[0]));

    for (size_t i = 0; i < conf->custom_shader.count; i++) {
        const char *path = conf->custom_shader.paths[i];
        const bool optional = conf->custom_shader.optional[i];

        if (optional && access(path, F_OK) < 0) {
            LOG_DBG("%s: optional shader not found, skipping", path);
            continue;
        }

        if (program_load(state, path, &state->programs[state->program_count]))
            state->program_count++;
    }

    if (state->program_count == 0)
        LOG_WARN("no usable custom shaders; rendering without them");
}

static void
textures_free(struct shader_state *state)
{
    glDeleteFramebuffers(1, &state->upload_fbo);
    glDeleteFramebuffers(1, &state->source_fbo);
    glDeleteFramebuffers(2, state->pass_fbo);
    glDeleteTextures(1, &state->upload_tex);
    glDeleteTextures(1, &state->source_tex);
    glDeleteTextures(2, state->pass_tex);

    state->upload_fbo = state->source_fbo = 0;
    state->pass_fbo[0] = state->pass_fbo[1] = 0;
    state->upload_tex = state->source_tex = 0;
    state->pass_tex[0] = state->pass_tex[1] = 0;
}

struct upload_format {
    GLenum internal;
    GLenum format;
    GLenum type;
    int bpp;
};

static bool
upload_format(pixman_format_code_t fmt, struct upload_format *res)
{
    switch ((int)fmt) {
    case PIXMAN_a8r8g8b8:
    case PIXMAN_x8r8g8b8:
        *res = (struct upload_format){GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 4};
        return true;

    case PIXMAN_a2r10g10b10:
    case PIXMAN_x2r10g10b10:
        *res = (struct upload_format){GL_RGB10_A2, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV, 4};
        return true;

    case PIXMAN_a2b10g10r10:
    case PIXMAN_x2b10g10r10:
        *res = (struct upload_format){GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4};
        return true;

#if defined(HAVE_PIXMAN_RGBA_16)
    case PIXMAN_a16b16g16r16:
        *res = (struct upload_format){GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8};
        return true;
#endif
    }

    return false;
}

static GLuint
texture_new(GLenum internal, int width, int height)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    return tex;
}

static GLuint
fbo_new(GLuint tex)
{
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LOG_WARN("incomplete framebuffer");

    return fbo;
}

static bool
textures_setup(struct shader_state *state, int width, int height,
               pixman_format_code_t format)
{
    struct upload_format uf;
    if (!upload_format(format, &uf)) {
        LOG_ERR("unsupported pixel format 0x%08x; custom shaders disabled",
                (unsigned)format);
        return false;
    }

    textures_free(state);

    state->upload_tex = texture_new(uf.internal, width, height);
    state->source_tex = texture_new(uf.internal, width, height);
    for (size_t i = 0; i < 2; i++)
        state->pass_tex[i] = texture_new(uf.internal, width, height);

    state->upload_fbo = fbo_new(state->upload_tex);
    state->source_fbo = fbo_new(state->source_tex);
    for (size_t i = 0; i < 2; i++)
        state->pass_fbo[i] = fbo_new(state->pass_tex[i]);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    state->width = width;
    state->height = height;
    state->format = format;
    state->full_upload = true;
    return true;
}

static void
state_free(struct shader_state *state)
{
    if (state == NULL)
        return;

    if (state->egl_surface != EGL_NO_SURFACE && make_current(state)) {
        programs_free(state);
        textures_free(state);
        glDeleteShader(state->vertex_shader);
        glDeleteVertexArrays(1, &state->vao);
    }

    if (state->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(egl.display, state->egl_surface);
    }

    if (state->egl_window != NULL)
        wl_egl_window_destroy(state->egl_window);

    free(state->programs);
    free(state);
}

void
shader_destroy(struct terminal *term)
{
    if (term->render.shader == NULL)
        return;

    const bool token = wayl_read_suspend(term->wl);
    state_free(term->render.shader);
    term->render.shader = NULL;
    wayl_read_resume(term->wl, token);
}

void
shader_reload(struct terminal *term)
{
    struct shader_state *state = term->render.shader;

    if (!shader_configured(term)) {
        /* Back to plain wl_shm; the next frame must be complete */
        shader_destroy(term);
        return;
    }

    if (state != NULL && state->unusable) {
        /* Start over, with a new EGL surface */
        shader_destroy(term);
        return;
    }

    if (state != NULL) {
        state->need_reload = true;
        clock_gettime(CLOCK_MONOTONIC, &state->last_activity);
    }
}

static struct shader_state *
state_new(struct terminal *term, int width, int height)
{
    if (!egl_init(term->wl->display))
        return NULL;

    struct shader_state *state = xcalloc(1, sizeof(*state));
    state->egl_surface = EGL_NO_SURFACE;

    state->egl_window = wl_egl_window_create(
        term->window->surface.surf, width, height);

    if (state->egl_window == NULL) {
        LOG_ERR("failed to create EGL window; custom shaders disabled");
        goto err;
    }

    state->egl_surface = eglCreatePlatformWindowSurface(
        egl.display, egl.config, state->egl_window, NULL);

    if (state->egl_surface == EGL_NO_SURFACE) {
        LOG_ERR("failed to create EGL surface (0x%04x); custom shaders disabled",
                eglGetError());
        goto err;
    }

    if (!make_current(state))
        goto err;

    /* We pace frames ourselves, with frame callbacks */
    eglSwapInterval(egl.display, 0);

    detect_glsl_version();

    glGenVertexArrays(1, &state->vao);

    const char *vs_srcs[] = {vertex_shader_src};
    state->vertex_shader = compile(GL_VERTEX_SHADER, vs_srcs, 1, "<builtin>");
    if (state->vertex_shader == 0)
        goto err;

    state->need_reload = true;
    state->width = width;
    state->height = height;
    state->focused = term->kbd_focus;
    return state;

err:
    state_free(state);
    return NULL;
}

static void
color_to_vec(uint32_t color, float v[static 4])
{
    v[0] = ((color >> 16) & 0xff) / 255.f;
    v[1] = ((color >> 8) & 0xff) / 255.f;
    v[2] = ((color >> 0) & 0xff) / 255.f;
    v[3] = 1.f;
}

static void
update_frame_uniforms(struct terminal *term, struct shader_state *state)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!state->have_frame) {
        state->first_frame_time = now;
        state->last_frame_time = now;
        state->last_activity = now;
        state->have_frame = true;
    }

    state->time = secs_between(&state->first_frame_time, &now);
    state->time_delta = secs_between(&state->last_frame_time, &now);
    state->last_frame_time = now;
    state->frame++;

    if (state->time_delta > 0.f) {
        const float fps = 1.f / state->time_delta;
        state->frame_rate = state->frame_rate == 0.f
            ? fps
            : state->frame_rate * 0.9f + fps * 0.1f;
    }
}

static void
update_state_uniforms(struct terminal *term, struct shader_state *state,
                      const struct shader_frame_info *info)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    state->cursor_visible = info->cursor_visible;

    if (info->cursor_drawn) {
        /* Shadertoy coordinates: origin bottom-left; the cursor's
         * (x, y) is its top-left corner */
        const float cursor[4] = {
            info->cursor_x,
            state->height - info->cursor_y,
            info->cursor_width,
            info->cursor_height,
        };

        float color[4];
        color_to_vec(info->cursor_color, color);

        if (memcmp(cursor, state->current_cursor, sizeof(cursor)) != 0 ||
            memcmp(color, state->current_cursor_color, sizeof(color)) != 0)
        {
            memcpy(state->previous_cursor, state->current_cursor, sizeof(cursor));
            memcpy(state->previous_cursor_color, state->current_cursor_color, sizeof(color));
            memcpy(state->current_cursor, cursor, sizeof(cursor));
            memcpy(state->current_cursor_color, color, sizeof(color));
            state->cursor_change_time = state->time;
            state->last_activity = now;
        }

        if ((int)info->cursor_style != state->current_cursor_style) {
            state->previous_cursor_style = state->current_cursor_style;
            state->current_cursor_style = info->cursor_style;
            state->last_activity = now;
        }
    }

    if (info->focused != state->focused) {
        state->focused = info->focused;
        if (state->focused)
            state->time_focus = state->time;
        state->last_activity = now;
    }
}

static void
set_uniforms(const struct terminal *term, const struct shader_state *state,
             const struct program *prog)
{
    const GLint *loc = prog->loc;
    const float w = state->width;
    const float h = state->height;

    if (loc[U_RESOLUTION] >= 0)
        glUniform3f(loc[U_RESOLUTION], w, h, 1.f);
    if (loc[U_TIME] >= 0)
        glUniform1f(loc[U_TIME], state->time);
    if (loc[U_TIME_DELTA] >= 0)
        glUniform1f(loc[U_TIME_DELTA], state->time_delta);
    if (loc[U_FRAME_RATE] >= 0)
        glUniform1f(loc[U_FRAME_RATE], state->frame_rate);
    if (loc[U_FRAME] >= 0)
        glUniform1i(loc[U_FRAME], state->frame);
    if (loc[U_CHANNEL_TIME] >= 0) {
        const float t[4] = {state->time, 0, 0, 0};
        glUniform1fv(loc[U_CHANNEL_TIME], 4, t);
    }
    if (loc[U_CHANNEL_RESOLUTION] >= 0) {
        const float r[4 * 3] = {w, h, 1.f};
        glUniform3fv(loc[U_CHANNEL_RESOLUTION], 4, r);
    }
    if (loc[U_MOUSE] >= 0)
        glUniform4f(loc[U_MOUSE], 0, 0, 0, 0);
    if (loc[U_DATE] >= 0) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        glUniform4f(loc[U_DATE], tm.tm_year + 1900, tm.tm_mon, tm.tm_mday,
                    tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    }
    if (loc[U_SAMPLE_RATE] >= 0)
        glUniform1f(loc[U_SAMPLE_RATE], 0.f);

    if (loc[U_CURRENT_CURSOR] >= 0)
        glUniform4fv(loc[U_CURRENT_CURSOR], 1, state->current_cursor);
    if (loc[U_PREVIOUS_CURSOR] >= 0)
        glUniform4fv(loc[U_PREVIOUS_CURSOR], 1, state->previous_cursor);
    if (loc[U_CURRENT_CURSOR_COLOR] >= 0)
        glUniform4fv(loc[U_CURRENT_CURSOR_COLOR], 1, state->current_cursor_color);
    if (loc[U_PREVIOUS_CURSOR_COLOR] >= 0)
        glUniform4fv(loc[U_PREVIOUS_CURSOR_COLOR], 1, state->previous_cursor_color);
    if (loc[U_CURRENT_CURSOR_STYLE] >= 0)
        glUniform1i(loc[U_CURRENT_CURSOR_STYLE], state->current_cursor_style);
    if (loc[U_PREVIOUS_CURSOR_STYLE] >= 0)
        glUniform1i(loc[U_PREVIOUS_CURSOR_STYLE], state->previous_cursor_style);
    if (loc[U_CURSOR_VISIBLE] >= 0)
        glUniform1i(loc[U_CURSOR_VISIBLE], state->cursor_visible);
    if (loc[U_TIME_CURSOR_CHANGE] >= 0)
        glUniform1f(loc[U_TIME_CURSOR_CHANGE], state->cursor_change_time);
    if (loc[U_TIME_FOCUS] >= 0)
        glUniform1f(loc[U_TIME_FOCUS], state->time_focus);
    if (loc[U_FOCUS] >= 0)
        glUniform1i(loc[U_FOCUS], state->focused);

    const struct colors *colors = &term->colors;
    float v[4];

    if (loc[U_PALETTE] >= 0) {
        float palette[256 * 3];
        for (size_t i = 0; i < 256; i++) {
            color_to_vec(colors->table[i], v);
            memcpy(&palette[i * 3], v, 3 * sizeof(float));
        }
        glUniform3fv(loc[U_PALETTE], 256, palette);
    }

    const uint32_t fg = term->reverse ? colors->bg : colors->fg;
    const uint32_t bg = term->reverse ? colors->fg : colors->bg;

    if (loc[U_BACKGROUND_COLOR] >= 0) {
        color_to_vec(bg, v);
        glUniform3fv(loc[U_BACKGROUND_COLOR], 1, v);
    }
    if (loc[U_FOREGROUND_COLOR] >= 0) {
        color_to_vec(fg, v);
        glUniform3fv(loc[U_FOREGROUND_COLOR], 1, v);
    }
    if (loc[U_CURSOR_COLOR] >= 0) {
        color_to_vec(colors->cursor_bg >> 31 ? colors->cursor_bg : fg, v);
        glUniform3fv(loc[U_CURSOR_COLOR], 1, v);
    }
    if (loc[U_CURSOR_TEXT] >= 0) {
        color_to_vec(colors->cursor_fg >> 31 ? colors->cursor_fg : bg, v);
        glUniform3fv(loc[U_CURSOR_TEXT], 1, v);
    }
    if (loc[U_SELECTION_BACKGROUND_COLOR] >= 0) {
        color_to_vec(colors->selection_bg >> 24 == 0 ? colors->selection_bg : fg, v);
        glUniform3fv(loc[U_SELECTION_BACKGROUND_COLOR], 1, v);
    }
    if (loc[U_SELECTION_FOREGROUND_COLOR] >= 0) {
        color_to_vec(colors->selection_fg >> 24 == 0 ? colors->selection_fg : bg, v);
        glUniform3fv(loc[U_SELECTION_FOREGROUND_COLOR], 1, v);
    }
    if (loc[U_CHANNEL0] >= 0)
        glUniform1i(loc[U_CHANNEL0], 0);
}

static void
upload(struct shader_state *state, const struct buffer *buf)
{
    struct upload_format uf;
    if (!upload_format(state->format, &uf))
        return;

    glBindTexture(GL_TEXTURE_2D, state->upload_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, buf->stride / uf.bpp);

    if (state->full_upload) {
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, buf->width, buf->height,
                        uf.format, uf.type, buf->data);
        state->full_upload = false;
    } else {
        /*
         * The buffer's frame-to-frame damage. Since we never hand the
         * buffer to the compositor, the same buffer is re-used frame
         * after frame, and this is exactly what changed since our
         * last upload.
         */
        pixman_region32_t region;
        pixman_region32_init_rect(&region, 0, 0, buf->width, buf->height);
        pixman_region32_intersect(&region, &region, &buf->dirty[0]);

        int count = 0;
        const pixman_box32_t *boxes = pixman_region32_rectangles(&region, &count);

        if (count > 64) {
            boxes = pixman_region32_extents(&region);
            count = 1;
        }

        for (int i = 0; i < count; i++) {
            const pixman_box32_t *b = &boxes[i];
            if (b->x2 <= b->x1 || b->y2 <= b->y1)
                continue;

            glPixelStorei(GL_UNPACK_SKIP_PIXELS, b->x1);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, b->y1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, b->x1, b->y1,
                            b->x2 - b->x1, b->y2 - b->y1,
                            uf.format, uf.type, buf->data);
        }

        pixman_region32_fini(&region);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    /* Flip into the source texture: Shadertoy (and Ghostty, on
     * OpenGL) have the origin in the bottom-left corner */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, state->upload_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->source_fbo);
    glBlitFramebuffer(0, 0, state->width, state->height,
                      0, state->height, state->width, 0,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static bool
draw_and_swap(struct terminal *term, struct shader_state *state)
{
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, state->width, state->height);
    glBindVertexArray(state->vao);
    glActiveTexture(GL_TEXTURE0);

    GLuint input = state->source_tex;

    for (size_t i = 0; i < state->program_count; i++) {
        const struct program *prog = &state->programs[i];
        const bool last = i + 1 == state->program_count;

        glBindFramebuffer(GL_FRAMEBUFFER, last ? 0 : state->pass_fbo[i % 2]);
        glUseProgram(prog->id);
        set_uniforms(term, state, prog);
        glBindTexture(GL_TEXTURE_2D, input);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        input = state->pass_tex[i % 2];
    }

    glUseProgram(0);
    glBindVertexArray(0);

    if (!eglSwapBuffers(egl.display, state->egl_surface)) {
        LOG_ERR("EGL: failed to swap buffers (0x%04x)", eglGetError());
        return false;
    }

    return true;
}

static bool present(struct terminal *term, struct buffer *buf,
                    const struct shader_frame_info *info);

/*
 * EGL reads from, and dispatches, its own queue on our Wayland
 * connection. That deadlocks if we're holding a prepared read
 * (which we do, except while dispatching events).
 */
bool
shader_present(struct terminal *term, struct buffer *buf,
               const struct shader_frame_info *info)
{
    const bool token = wayl_read_suspend(term->wl);
    bool ret = present(term, buf, info);
    wayl_read_resume(term->wl, token);
    return ret;
}

static bool
present(struct terminal *term, struct buffer *buf,
        const struct shader_frame_info *info)
{
    if (!shader_configured(term)) {
        if (term->render.shader != NULL)
            shader_destroy(term);
        return false;
    }

    struct shader_state *state = term->render.shader;

    if (state != NULL && state->unusable)
        return false;

    if (state == NULL) {
        if (egl.failed)
            return false;

        state = term->render.shader = state_new(term, buf->width, buf->height);
        if (state == NULL) {
            egl.failed = true;  /* Don't retry on every frame */
            return false;
        }
    }

    if (!make_current(state))
        return false;

    if (state->need_reload) {
        state->need_reload = false;
        programs_load(term, state);
        state->unusable = state->program_count == 0;
        state->full_upload = true;

        if (state->unusable) {
            /* Release the EGL surface, so the wl_shm path can be
             * used (until the next configuration reload) */
            programs_free(state);
            textures_free(state);
            glDeleteShader(state->vertex_shader);
            glDeleteVertexArrays(1, &state->vao);
            state->vertex_shader = state->vao = 0;
            eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(egl.display, state->egl_surface);
            state->egl_surface = EGL_NO_SURFACE;
            wl_egl_window_destroy(state->egl_window);
            state->egl_window = NULL;

            /* Keep an (empty) state around, as a "don't retry" marker */
            state->need_reload = false;
            return false;
        }
    }

    const pixman_format_code_t format = pixman_image_get_format(buf->pix[0]);

    if (state->upload_tex == 0 ||
        state->width != buf->width ||
        state->height != buf->height ||
        state->format != format)
    {
        wl_egl_window_resize(state->egl_window, buf->width, buf->height, 0, 0);
        if (!textures_setup(state, buf->width, buf->height, format)) {
            shader_destroy(term);
            egl.failed = true;  /* Don't retry */
            return false;
        }
    }

    update_frame_uniforms(term, state);
    update_state_uniforms(term, state, info);
    upload(state, buf);

    if (!draw_and_swap(term, state))
        return false;

    /*
     * The buffer has been copied, and was never attached to the
     * surface; it's free to be re-used immediately (and will be,
     * which means we'll only have to upload the next frame's damage)
     */
    shm_did_not_use_buf(buf);
    return true;
}

bool
shader_wants_animation_frame(const struct terminal *term)
{
    const struct shader_state *state = term->render.shader;

    if (state == NULL || state->unusable || state->egl_surface == EGL_NO_SURFACE ||
        !state->have_frame || state->program_count == 0)
    {
        return false;
    }

    switch (term->conf->custom_shader.animation) {
    case SHADER_ANIMATION_FALSE:
        return false;

    case SHADER_ANIMATION_ALWAYS:
        return true;

    case SHADER_ANIMATION_TRUE:
        return term->kbd_focus;

    case SHADER_ANIMATION_AUTO: {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        const double idle = secs_between(&state->last_activity, &now);
        return idle * 1000. < term->conf->custom_shader.animation_duration;
    }
    }

    return false;
}

bool
shader_render_animation_frame(struct terminal *term)
{
    struct shader_state *state = term->render.shader;

    if (state == NULL || state->egl_surface == EGL_NO_SURFACE ||
        state->program_count == 0)
    {
        return false;
    }

    const bool token = wayl_read_suspend(term->wl);

    bool ret = false;
    if (make_current(state)) {
        update_frame_uniforms(term, state);
        ret = draw_and_swap(term, state);
    }

    wayl_read_resume(term->wl, token);
    return ret;
}
