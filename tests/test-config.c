#if !defined(_DEBUG)
 #define _DEBUG
#endif
#undef NDEBUG

#include "../log.h"

#include "../config.c"

#define ALEN(v) (sizeof(v) / sizeof((v)[0]))

/*
 * Stubs
 */

void
user_notification_add_fmt(user_notifications_t *notifications,
                          enum user_notification_kind kind,
                          const char *fmt, ...)
{
}

static void
test_invalid_key(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                 const char *key)
{
    ctx->key = key;
    ctx->value = "value for invalid key";

    if (parse_fun(ctx)) {
        BUG("[%s].%s: did not fail to parse as expected"
            "(key should be invalid)", ctx->section, ctx->key);
    }
}

static void
test_string(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, char *const *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        const char *value;
        bool invalid;
    } input[] = {
        {"a string", "a string"},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (!streq(*ptr, input[i].value)) {
                BUG("[%s].%s=%s: set value (%s) not the expected one (%s)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_c32string(struct context *ctx, bool (*parse_fun)(struct context *ctx),
               const char *key, char32_t *const *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        const char32_t *value;
        bool invalid;
    } input[] = {
        {"a string", U"a string"},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (c32cmp(*ptr, input[i].value) != 0) {
                BUG("[%s].%s=%s: set value (%ls) not the expected one (%ls)",
                    ctx->section, ctx->key, ctx->value,
                    (const wchar_t *)*ptr,
                    (const wchar_t *)input[i].value);
            }
        }
    }
}

static void
test_boolean(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, const bool *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        bool value;
        bool invalid;
    } input[] = {
        {"1", true}, {"0", false},
        {"on", true}, {"off", false},
        {"true", true}, {"false", false},
        {"unittest-invalid-boolean-value", false, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%s) not the expected one (%s)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr ? "true" : "false",
                    input[i].value ? "true" : "false");
            }
        }
    }
}

static void
test_uint16(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const uint16_t *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        uint16_t value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"65535", 65535}, {"65536", 0, true},
        {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%hu) not the expected one (%hu)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_uint32(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const uint32_t *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        uint32_t value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"65536", 65536}, {"4294967295", 4294967295},
         {"4294967296", 0, true}, {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%u) not the expected one (%u)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_float(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const float *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        float value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"0.1", 0.1}, {"1e10", 1e10}, {"-10.7", -10.7},
        {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%f) not the expected one (%f)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_pt_or_px(struct context *ctx, bool (*parse_fun)(struct context *ctx),
              const char *key, const struct pt_or_px *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        struct pt_or_px value;
        bool invalid;
    } input[] = {
        {"12", {.pt = 12}}, {"12px", {.px = 12}},
        {"unittest-invalid-pt-or-px-value", {0}, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (memcmp(ptr, &input[i].value, sizeof(*ptr)) != 0) {
                BUG("[%s].%s=%s: "
                    "set value (pt=%f, px=%d) not the expected one (pt=%f, px=%d)",
                    ctx->section, ctx->key, ctx->value,
                    ptr->pt, ptr->px,
                    input[i].value.pt, input[i].value.px);
            }
        }
    }
}

static void
test_spawn_template(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                    const char *key, const struct config_spawn_template *ptr)
{
    static const char *const args[] = {
        "command", "arg1", "arg2", "arg3 has spaces"};

    ctx->key = key;
    ctx->value = "command arg1 arg2 \"arg3 has spaces\"";

    if (!parse_fun(ctx))
        BUG("[%s].%s=%s: failed to parse", ctx->section, ctx->key, ctx->value);

    if (ptr->argv.args == NULL)
        BUG("[%s].%s=%s: argv is NULL", ctx->section, ctx->key, ctx->value);

    for (size_t i = 0; i < ALEN(args); i++) {
        if (ptr->argv.args[i] == NULL || !streq(ptr->argv.args[i], args[i])) {
            BUG("[%s].%s=%s: set value not the expected one: "
                "mismatch of arg #%zu: expected=\"%s\", got=\"%s\"",
                ctx->section, ctx->key, ctx->value, i,
                args[i], ptr->argv.args[i]);
        }
    }

    if (ptr->argv.args[ALEN(args)] != NULL) {
        BUG("[%s].%s=%s: set value not the expected one: "
            "expected NULL terminator at arg #%zu, got=\"%s\"",
            ctx->section, ctx->key, ctx->value,
            ALEN(args), ptr->argv.args[ALEN(args)]);
    }

    /* Trigger parse failure */
    ctx->value = "command with \"unterminated quote";
    if (parse_fun(ctx)) {
        BUG("[%s].%s=%s: did not fail to parse as expected",
            ctx->section, ctx->key, ctx->value);
    }
}

static void
test_enum(struct context *ctx, bool (*parse_fun)(struct context *ctx),
          const char *key, size_t count, const char *enum_strings[static count],
          int enum_values[static count], int *ptr)
{
    ctx->key = key;

    for (size_t i = 0; i < count; i++) {
        ctx->value = enum_strings[i];
        if (!parse_fun(ctx)) {
            BUG("[%s].%s=%s: failed to parse",
                ctx->section, ctx->key, ctx->value);
        }

        if (*ptr != enum_values[i]) {
            BUG("[%s].%s=%s: set value not the expected one: expected %d, got %d",
                ctx->section, ctx->key, ctx->value, enum_values[i], *ptr);
        }
    }

    ctx->value = "invalid-enum-value";
    if (parse_fun(ctx)) {
        BUG("[%s].%s=%s: did not fail to parse as expected",
            ctx->section, ctx->key, ctx->value);
    }
}


static void
test_color(struct context *ctx, bool (*parse_fun)(struct context *ctx),
           const char *key, bool alpha_allowed, uint32_t *ptr)
{
    ctx->key = key;

    const struct {
        const char *option_string;
        uint32_t color;
        bool invalid;
    } input[] = {
        {"000000", 0},
        {"999999", 0x999999},
        {"ffffff", 0xffffff},
        {"ffffffff", 0xffffffff, !alpha_allowed},
        {"aabbccdd", 0xaabbccdd, !alpha_allowed},
        {"00", 0, true},
        {"0000", 0, true},
        {"00000", 0, true},
        {"000000000", 0, true},
        {"unittest-invalid-color", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;
        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }

            uint32_t color = input[i].color;
            if (alpha_allowed && strlen(input[i].option_string) == 6)
                color |= 0xff000000;

            if (*ptr != color) {
                BUG("[%s].%s=%s: expected 0x%08x, got 0x%08x",
                    ctx->section, ctx->key, ctx->value,
                    color, *ptr);
            }
        }
    }
}

static void
test_two_colors(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                const char *key, bool alpha_allowed,
                uint32_t *ptr1, uint32_t *ptr2)
{
    ctx->key = key;

    const struct {
        const char *option_string;
        uint32_t color1;
        uint32_t color2;
        bool invalid;
    } input[] = {
        {"000000 000000", 0, 0},

        /* No alpha */
        {"999999 888888", 0x999999, 0x888888},
        {"ffffff aaaaaa", 0xffffff, 0xaaaaaa},

        /* Both colors have alpha component */
        {"ffffffff 00000000", 0xffffffff, 0x00000000, !alpha_allowed},
        {"aabbccdd, ee112233", 0xaabbccdd, 0xee112233, !alpha_allowed},

        /* Only one color has alpha component */
        {"ffffffff 112233", 0xffffffff, 0x112233, !alpha_allowed},
        {"ffffff ff112233", 0x00ffffff, 0xff112233, !alpha_allowed},

        {"unittest-invalid-color", 0, 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;
        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }

            if (*ptr1 != input[i].color1) {
                BUG("[%s].%s=%s: expected 0x%08x, got 0x%08x",
                    ctx->section, ctx->key, ctx->value,
                    input[i].color1, *ptr1);
            }

            if (*ptr2 != input[i].color2) {
                BUG("[%s].%s=%s: expected 0x%08x, got 0x%08x",
                    ctx->section, ctx->key, ctx->value,
                    input[i].color2, *ptr2);
            }
        }
    }
}

static void
test_font_features(struct context *ctx)
{
    struct config *conf = ctx->conf;

    ctx->key = "font-features";
    ctx->value = "calt, -liga ss01  cv01=2";
    if (!parse_section_main(ctx))
        BUG("[main].font-features=%s: failed to parse", ctx->value);

    const char *expected[] = {"calt", "-liga", "ss01", "cv01=2"};
    if (conf->font_features[0].count != ALEN(expected))
        BUG("[main].font-features: got %zu features, expected %zu",
            conf->font_features[0].count, ALEN(expected));
    for (size_t i = 0; i < ALEN(expected); i++) {
        if (!streq(conf->font_features[0].arr[i], expected[i]))
            BUG("[main].font-features: feature #%zu: %s != %s",
                i, conf->font_features[0].arr[i], expected[i]);
    }

    ctx->value = "ss01 not-a-feature!";
    if (parse_section_main(ctx))
        BUG("[main].font-features=%s: did not fail to parse", ctx->value);
    if (conf->font_features[0].count != ALEN(expected))
        BUG("[main].font-features: modified by a failed parse");

    ctx->key = "font-features-italic";
    ctx->value = "-ss03";
    if (!parse_section_main(ctx) ||
        conf->font_features[2].count != 1 ||
        !streq(conf->font_features[2].arr[0], "-ss03"))
    {
        BUG("[main].font-features-italic=%s: failed to parse", ctx->value);
    }

    /* Empty value clears the list */
    ctx->key = "font-features";
    ctx->value = "";
    if (!parse_section_main(ctx) || conf->font_features[0].count != 0)
        BUG("[main].font-features=: did not clear the list");
}

static void
test_font_size_presets(struct context *ctx, struct config *conf)
{
    ctx->key = "font-size-presets";

    /* Sorted, separated by spaces and/or commas */
    ctx->value = "14 9.5,20 ,  11";
    if (!parse_section_main(ctx))
        BUG("[main].font-size-presets=%s: failed to parse", ctx->value);
    if (conf->font_size_presets.count != 4 ||
        conf->font_size_presets.sizes[0] != 9.5f ||
        conf->font_size_presets.sizes[1] != 11.f ||
        conf->font_size_presets.sizes[2] != 14.f ||
        conf->font_size_presets.sizes[3] != 20.f)
    {
        BUG("[main].font-size-presets=%s: wrong sizes", ctx->value);
    }

    /* Invalid values leave the current list as is */
    static const char *const invalid[] = {"abc", "10 0", "-1", "10x", "12 1e9"};
    for (size_t i = 0; i < ALEN(invalid); i++) {
        ctx->value = invalid[i];
        if (parse_section_main(ctx))
            BUG("[main].font-size-presets=%s: did not fail to parse", ctx->value);
        if (conf->font_size_presets.count != 4)
            BUG("[main].font-size-presets=%s: modified the list", ctx->value);
    }

    /* Empty clears the list */
    ctx->value = "";
    if (!parse_section_main(ctx) || conf->font_size_presets.count != 0)
        BUG("[main].font-size-presets=: did not clear the list");
}

static void
test_unit_float(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                const char *key, const float *ptr)
{
    /* A float in the range 0.0 - 1.0 */
    ctx->key = key;

    static const struct {
        const char *option_string;
        float value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"0.25", 0.25}, {"1", 1}, {"1.0", 1},
        {"-0.1", 0, true}, {"1.1", 0, true}, {"nan", 0, true},
        {"abc", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%f) not the expected one (%f)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_section_main(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "main", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_main, "invalid-key");

    test_string(&ctx, &parse_section_main, "shell", &conf.shell);
    test_string(&ctx, &parse_section_main, "term", &conf.term);
    test_string(&ctx, &parse_section_main, "app-id", &conf.app_id);
    test_string(&ctx, &parse_section_main, "toplevel-tag", &conf.toplevel_tag);
    test_string(&ctx, &parse_section_main, "utmp-helper", &conf.utmp_helper_path);

    test_c32string(&ctx, &parse_section_main, "word-delimiters", &conf.word_delimiters);

    test_boolean(&ctx, &parse_section_main, "login-shell", &conf.login_shell);
    test_boolean(&ctx, &parse_section_main, "box-drawings-uses-font-glyphs", &conf.box_drawings_uses_font_glyphs);
    test_boolean(&ctx, &parse_section_main, "opaque-fullscreen", &conf.opaque_fullscreen);
    test_boolean(&ctx, &parse_section_main, "selection-clear-on-copy", &conf.selection_clear_on_copy);
    test_boolean(&ctx, &parse_section_main, "vi-mode", &conf.vi_mode);
    test_unit_float(&ctx, &parse_section_main, "dim-unfocused", &conf.dim_unfocused);
    test_unit_float(&ctx, &parse_section_main, "alpha-adjustment", &conf.alpha_adjustment);
    test_font_size_presets(&ctx, &conf);
    test_boolean(&ctx, &parse_section_main, "ligatures", &conf.ligatures.enabled);
    {
        ctx.key = "ligatures";
        ctx.value = "except-cursor";
        if (!parse_section_main(&ctx) ||
            !conf.ligatures.enabled || !conf.ligatures.except_cursor)
        {
            BUG("[main].ligatures=except-cursor: failed to parse");
        }

        ctx.value = "yes";
        if (!parse_section_main(&ctx) || conf.ligatures.except_cursor)
            BUG("[main].ligatures=yes: did not reset except-cursor");
    }
    test_font_features(&ctx);
    test_boolean(&ctx, &parse_section_main, "locked-title", &conf.locked_title);
    test_boolean(&ctx, &parse_section_main, "dpi-aware", &conf.dpi_aware);
    test_boolean(&ctx, &parse_section_main, "gamma-correct-blending", &conf.gamma_correct);
    test_boolean(&ctx, &parse_section_main, "uppercase-regex-insert", &conf.uppercase_regex_insert);

    test_pt_or_px(&ctx, &parse_section_main, "font-size-adjustment", &conf.font_size_adjustment.pt_or_px);  /* TODO: test ‘N%’ values too */
    test_pt_or_px(&ctx, &parse_section_main, "line-height", &conf.line_height);
    test_pt_or_px(&ctx, &parse_section_main, "letter-spacing", &conf.letter_spacing);
    test_pt_or_px(&ctx, &parse_section_main, "horizontal-letter-offset", &conf.horizontal_letter_offset);
    test_pt_or_px(&ctx, &parse_section_main, "vertical-letter-offset", &conf.vertical_letter_offset);
    test_pt_or_px(&ctx, &parse_section_main, "underline-thickness", &conf.underline_thickness);
    test_pt_or_px(&ctx, &parse_section_main, "strikeout-thickness", &conf.strikeout_thickness);

    test_uint16(&ctx, &parse_section_main, "resize-delay-ms", &conf.resize_delay_ms);
    test_uint16(&ctx, &parse_section_main, "workers", &conf.render_worker_count);

    test_enum(&ctx, &parse_section_main, "selection-target",
              4,
              (const char *[]){"none", "primary", "clipboard", "both"},
              (int []){SELECTION_TARGET_NONE,
                       SELECTION_TARGET_PRIMARY,
                       SELECTION_TARGET_CLIPBOARD,
                       SELECTION_TARGET_BOTH},
              (int *)&conf.selection_target);

    test_enum(
        &ctx, &parse_section_main, "initial-window-mode",
        3,
        (const char *[]){"windowed", "maximized", "fullscreen"},
        (int []){STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN},
        (int *)&conf.startup_mode);

    test_enum(
        &ctx, &parse_section_main, "initial-color-theme",
        2,
        (const char *[]){"dark", "light", "1", "2"},
        (int []){COLOR_THEME_DARK, COLOR_THEME_LIGHT,
                 COLOR_THEME_DARK, COLOR_THEME_LIGHT},
        (int *)&conf.initial_color_theme);

    /* TODO: font (custom) */
    /* TODO: bold-text-in-bright (enum/boolean) */
    /* TODO: pad (geometry + optional string)*/
    /* TODO: initial-window-size-pixels (geometry) */
    /* TODO: initial-window-size-chars (geometry) */

    config_free(&conf);
}

static void
write_file(const char *dir, const char *name, const char *content)
{
    char *path = xstrjoin3(dir, "/", name);
    FILE *f = fopen(path, "w");
    if (f == NULL)
        BUG("%s: failed to create: %s", path, strerror(errno));
    fputs(content, f);
    fclose(f);
    free(path);
}

static bool
source_files_contains(const struct config *conf, const char *path)
{
    tll_foreach(conf->source_files, it) {
        if (streq(it->item, path))
            return true;
    }
    return false;
}

static void
test_include(void)
{
    char template[] = "/tmp/ankle-test-include-XXXXXX";
    const char *dir = mkdtemp(template);
    if (dir == NULL)
        BUG("failed to create temporary directory: %s", strerror(errno));

    char *sub_dir = xstrjoin(dir, "/sub");
    if (mkdir(sub_dir, 0700) < 0)
        BUG("%s: failed to create: %s", sub_dir, strerror(errno));

    /* sub/theme.ini includes colors.ini, relative to sub/ */
    write_file(dir, "sub/theme.ini", "title=from-theme\ninclude=colors.ini\n");
    write_file(dir, "sub/colors.ini", "[colors-dark]\nforeground=123456\n");
    write_file(dir, "absolute.ini", "app-id=from-absolute\n");

    char *main_path = xstrjoin(dir, "/ankle.ini");
    char *theme_path = xstrjoin(dir, "/sub/theme.ini");
    char *colors_path = xstrjoin(dir, "/sub/colors.ini");
    char *absolute_path = xstrjoin(dir, "/absolute.ini");
    char *missing_path = xstrjoin(dir, "/missing.ini");

    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "main", .path = main_path,
        .errors_are_fatal = true,
    };

    /* Relative include, resolved against the including file's
     * directory, including nested relative includes */
    ctx.key = "include";
    ctx.value = "sub/theme.ini";
    if (!parse_section_main(&ctx))
        BUG("[main].include=sub/theme.ini: failed to parse");
    if (conf.title == NULL || !streq(conf.title, "from-theme"))
        BUG("[main].include=sub/theme.ini: file not imported");
    if (conf.colors_dark.fg != 0x123456)
        BUG("[main].include=sub/theme.ini: nested relative include not imported");
    if (!source_files_contains(&conf, theme_path) ||
        !source_files_contains(&conf, colors_path))
    {
        BUG("[main].include=sub/theme.ini: included files not tracked");
    }

    /* Absolute include */
    ctx.value = absolute_path;
    if (!parse_section_main(&ctx) ||
        conf.app_id == NULL || !streq(conf.app_id, "from-absolute"))
    {
        BUG("[main].include=%s: failed to import", absolute_path);
    }

    /* ~/ is expanded to $HOME */
    const char *old_home = getenv("HOME");
    char *saved_home = old_home != NULL ? xstrdup(old_home) : NULL;
    setenv("HOME", dir, 1);
    free(conf.app_id);
    conf.app_id = NULL;
    ctx.value = "~/absolute.ini";
    if (!parse_section_main(&ctx) ||
        conf.app_id == NULL || !streq(conf.app_id, "from-absolute"))
    {
        BUG("[main].include=~/absolute.ini: failed to import");
    }
    if (saved_home != NULL)
        setenv("HOME", saved_home, 1);
    else
        unsetenv("HOME");
    free(saved_home);

    /* A missing file is an error for 'include'... */
    ctx.value = "missing.ini";
    if (parse_section_main(&ctx))
        BUG("[main].include=missing.ini: did not fail");

    /* ...but not for 'include-optional', which still tracks it (so
     * that creating it triggers a reload) */
    ctx.key = "include-optional";
    tll_free_and_free(conf.source_files, free);
    if (!parse_section_main(&ctx))
        BUG("[main].include-optional=missing.ini: failed");
    if (!source_files_contains(&conf, missing_path))
        BUG("[main].include-optional=missing.ini: not tracked");

    /* 'include-optional' imports the file when it does exist */
    free(conf.title);
    conf.title = NULL;
    ctx.value = "sub/theme.ini";
    if (!parse_section_main(&ctx) ||
        conf.title == NULL || !streq(conf.title, "from-theme"))
    {
        BUG("[main].include-optional=sub/theme.ini: failed to import");
    }

    /* 'include-optional' still fails on errors other than a missing
     * file, e.g. a directory... */
    ctx.value = "sub/theme.ini/nope";
    if (parse_section_main(&ctx))
        BUG("[main].include-optional=sub/theme.ini/nope: did not fail");

    /* ...and on errors inside the included file */
    write_file(dir, "broken.ini", "no-such-option=yes\n");
    ctx.value = "broken.ini";
    if (parse_section_main(&ctx))
        BUG("[main].include-optional=broken.ini: did not fail");

    /* Relative paths are rejected in command line overrides */
    ctx.key = "include";
    ctx.value = "sub/theme.ini";
    ctx.path = "override";
    ctx.is_override = true;
    if (parse_section_main(&ctx))
        BUG("[main].include=sub/theme.ini: relative path accepted in override");

    /* A relative including path (e.g. --config=ankle.ini) resolves
     * against the current working directory */
    ctx.is_override = false;
    ctx.path = "ankle.ini";
    char *expanded = include_path_expand(&ctx, "theme.ini");
    if (expanded == NULL || !streq(expanded, "theme.ini"))
        BUG("include_path_expand(): unexpected result: %s", expanded);
    free(expanded);

    ctx.path = "conf/ankle.ini";
    expanded = include_path_expand(&ctx, "theme.ini");
    if (expanded == NULL || !streq(expanded, "conf/theme.ini"))
        BUG("include_path_expand(): unexpected result: %s", expanded);
    free(expanded);

    config_free(&conf);

    const char *files[] = {
        "sub/theme.ini", "sub/colors.ini", "absolute.ini", "broken.ini",
    };
    for (size_t i = 0; i < ALEN(files); i++) {
        char *path = xstrjoin3(dir, "/", files[i]);
        unlink(path);
        free(path);
    }
    rmdir(sub_dir);
    rmdir(dir);

    free(sub_dir);
    free(main_path);
    free(theme_path);
    free(colors_path);
    free(absolute_path);
    free(missing_path);
}

static void
test_section_security(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "security", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_security, "invalid-key");
    test_enum(
        &ctx, &parse_section_security, "osc52", 4,
        (const char*[]){"disabled", "copy-enabled", "paste-enabled", "enabled"},
        (int []){OSC52_DISABLED, OSC52_COPY_ENABLED, OSC52_PASTE_ENABLED, OSC52_ENABLED},
        (int *)&conf.security.osc52);
    test_enum(
        &ctx, &parse_section_security, "paste-protection", 3,
        (const char*[]){"never", "unbracketed", "always"},
        (int []){PASTE_PROTECTION_NEVER, PASTE_PROTECTION_UNBRACKETED, PASTE_PROTECTION_ALWAYS},
        (int *)&conf.security.paste_protection);

    config_free(&conf);
}

static void
test_section_bell(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "bell", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_bell, "invalid-key");

    test_boolean(&ctx, &parse_section_bell, "urgent", &conf.bell.urgent);
    test_boolean(&ctx, &parse_section_bell, "notify", &conf.bell.notify);
    test_boolean(&ctx, &parse_section_bell, "system", &conf.bell.system_bell);
    test_boolean(&ctx, &parse_section_bell, "command-focused",
                 &conf.bell.command_focused);
    test_spawn_template(&ctx, &parse_section_bell, "command",
                        &conf.bell.command);

    config_free(&conf);
}

static void
test_section_desktop_notifications(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "desktop-notifications", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_desktop_notifications, "invalid-key");

    test_boolean(&ctx, &parse_section_desktop_notifications, "inhibit-when-focused", &conf.desktop_notifications.inhibit_when_focused);
    test_spawn_template(&ctx, &parse_section_desktop_notifications, "command", &conf.desktop_notifications.command);
    test_spawn_template(&ctx, &parse_section_desktop_notifications, "command-action-argument", &conf.desktop_notifications.command_action_arg);
    test_spawn_template(&ctx, &parse_section_desktop_notifications, "close", &conf.desktop_notifications.close);
    test_enum(
        &ctx, &parse_section_desktop_notifications, "command-finished",
        3,
        (const char *[]){"never", "unfocused", "always"},
        (int []){COMMAND_FINISHED_NEVER,
                 COMMAND_FINISHED_UNFOCUSED,
                 COMMAND_FINISHED_ALWAYS},
        (int *)&conf.desktop_notifications.command_finished);
    test_uint32(&ctx, &parse_section_desktop_notifications,
                "command-finished-min-duration",
                &conf.desktop_notifications.command_finished_min_duration);

    config_free(&conf);
}

static void
test_section_scrollback(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "scrollback", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_scrollback, "invalid-key");

    test_uint32(&ctx, &parse_section_scrollback, "lines",
                &conf.scrollback.lines);
    test_float(&ctx, parse_section_scrollback, "multiplier", &conf.scrollback.multiplier);
    test_boolean(&ctx, &parse_section_scrollback, "search-regex", &conf.scrollback.search_regex);
    test_boolean(&ctx, &parse_section_scrollback, "search-freeze", &conf.scrollback.search_freeze);

    test_enum(
        &ctx, &parse_section_scrollback, "indicator-position",
        3,
        (const char *[]){"none", "fixed", "relative"},
        (int []){SCROLLBACK_INDICATOR_POSITION_NONE,
            SCROLLBACK_INDICATOR_POSITION_FIXED,
            SCROLLBACK_INDICATOR_POSITION_RELATIVE},
        (int *)&conf.scrollback.indicator.position);

    /* TODO: indicator-format (enum, sort-of) */

    config_free(&conf);
}

static void
test_section_url(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "url", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_url, "invalid-key");

    test_spawn_template(&ctx, &parse_section_url, "launch", &conf.url.launch);
    test_enum(&ctx, &parse_section_url, "osc8-underline",
              2,
              (const char *[]){"url-mode", "always"},
              (int []){OSC8_UNDERLINE_URL_MODE, OSC8_UNDERLINE_ALWAYS},
              (int *)&conf.url.osc8_underline);
    test_enum(&ctx, &parse_section_url, "style",
              6, (const char *[]){"none", "single", "double", "curly", "dotted", "dashed"},
              (int []){UNDERLINE_NONE, UNDERLINE_SINGLE, UNDERLINE_DOUBLE, UNDERLINE_CURLY, UNDERLINE_DOTTED, UNDERLINE_DASHED},
              (int *)&conf.url.style);
    test_c32string(&ctx, &parse_section_url, "label-letters", &conf.url.label_letters);

    config_free(&conf);
}

static void
test_section_cursor(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "cursor", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_cursor, "invalid-key");

    test_enum(
        &ctx, &parse_section_cursor, "style",
        3,
        (const char *[]){"block", "beam", "underline"},
        (int []){CURSOR_BLOCK, CURSOR_BEAM, CURSOR_UNDERLINE},
        (int *)&conf.cursor.style);
    test_enum(
        &ctx, &parse_section_cursor, "unfocused-style",
        3,
        (const char *[]){"unchanged", "hollow", "none"},
        (int []){CURSOR_UNFOCUSED_UNCHANGED, CURSOR_UNFOCUSED_HOLLOW, CURSOR_UNFOCUSED_NONE},
        (int *)&conf.cursor.unfocused_style);
    test_boolean(&ctx, &parse_section_cursor, "blink", &conf.cursor.blink.enabled);
    test_uint32(&ctx, &parse_section_cursor, "blink-rate", &conf.cursor.blink.rate_ms);
    test_pt_or_px(&ctx, &parse_section_cursor, "beam-thickness",
                  &conf.cursor.beam_thickness);
    test_pt_or_px(&ctx, &parse_section_cursor, "underline-thickness",
                  &conf.cursor.underline_thickness);

    /* TODO: color (two RRGGBB values) */

    config_free(&conf);
}

static void
test_section_mouse(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_mouse, "invalid-key");

    test_boolean(&ctx, &parse_section_mouse, "hide-when-typing",
                 &conf.mouse.hide_when_typing);
    test_boolean(&ctx, &parse_section_mouse, "alternate-scroll-mode",
                 &conf.mouse.alternate_scroll_mode);

    config_free(&conf);
}

static void
test_section_touch(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "touch", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_touch, "invalid-key");

    test_uint32(&ctx, &parse_section_touch, "long-press-delay",
                &conf.touch.long_press_delay);

    config_free(&conf);
}

static void
test_section_colors_dark(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "colors-dark", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_colors_dark, "invalid-key");

    test_color(&ctx, &parse_section_colors_dark, "foreground", false, &conf.colors_dark.fg);
    test_color(&ctx, &parse_section_colors_dark, "background", false, &conf.colors_dark.bg);
    test_color(&ctx, &parse_section_colors_dark, "regular0", false, &conf.colors_dark.table[0]);
    test_color(&ctx, &parse_section_colors_dark, "regular1", false, &conf.colors_dark.table[1]);
    test_color(&ctx, &parse_section_colors_dark, "regular2", false, &conf.colors_dark.table[2]);
    test_color(&ctx, &parse_section_colors_dark, "regular3", false, &conf.colors_dark.table[3]);
    test_color(&ctx, &parse_section_colors_dark, "regular4", false, &conf.colors_dark.table[4]);
    test_color(&ctx, &parse_section_colors_dark, "regular5", false, &conf.colors_dark.table[5]);
    test_color(&ctx, &parse_section_colors_dark, "regular6", false, &conf.colors_dark.table[6]);
    test_color(&ctx, &parse_section_colors_dark, "regular7", false, &conf.colors_dark.table[7]);
    test_color(&ctx, &parse_section_colors_dark, "bright0", false, &conf.colors_dark.table[8]);
    test_color(&ctx, &parse_section_colors_dark, "bright1", false, &conf.colors_dark.table[9]);
    test_color(&ctx, &parse_section_colors_dark, "bright2", false, &conf.colors_dark.table[10]);
    test_color(&ctx, &parse_section_colors_dark, "bright3", false, &conf.colors_dark.table[11]);
    test_color(&ctx, &parse_section_colors_dark, "bright4", false, &conf.colors_dark.table[12]);
    test_color(&ctx, &parse_section_colors_dark, "bright5", false, &conf.colors_dark.table[13]);
    test_color(&ctx, &parse_section_colors_dark, "bright6", false, &conf.colors_dark.table[14]);
    test_color(&ctx, &parse_section_colors_dark, "bright7", false, &conf.colors_dark.table[15]);
    test_color(&ctx, &parse_section_colors_dark, "dim0", false, &conf.colors_dark.dim[0]);
    test_color(&ctx, &parse_section_colors_dark, "dim1", false, &conf.colors_dark.dim[1]);
    test_color(&ctx, &parse_section_colors_dark, "dim2", false, &conf.colors_dark.dim[2]);
    test_color(&ctx, &parse_section_colors_dark, "dim3", false, &conf.colors_dark.dim[3]);
    test_color(&ctx, &parse_section_colors_dark, "dim4", false, &conf.colors_dark.dim[4]);
    test_color(&ctx, &parse_section_colors_dark, "dim5", false, &conf.colors_dark.dim[5]);
    test_color(&ctx, &parse_section_colors_dark, "dim6", false, &conf.colors_dark.dim[6]);
    test_color(&ctx, &parse_section_colors_dark, "dim7", false, &conf.colors_dark.dim[7]);
    test_color(&ctx, &parse_section_colors_dark, "selection-foreground", false, &conf.colors_dark.selection_fg);
    test_color(&ctx, &parse_section_colors_dark, "selection-background", false, &conf.colors_dark.selection_bg);
    test_color(&ctx, &parse_section_colors_dark, "urls", false, &conf.colors_dark.url);
    test_two_colors(&ctx, &parse_section_colors_dark, "jump-labels", false,
                    &conf.colors_dark.jump_label.fg,
                    &conf.colors_dark.jump_label.bg);
    test_two_colors(&ctx, &parse_section_colors_dark, "scrollback-indicator", false,
                    &conf.colors_dark.scrollback_indicator.fg,
                    &conf.colors_dark.scrollback_indicator.bg);
    test_two_colors(&ctx, &parse_section_colors_dark, "search-box-no-match", false,
                    &conf.colors_dark.search_box.no_match.fg,
                    &conf.colors_dark.search_box.no_match.bg);
    test_two_colors(&ctx, &parse_section_colors_dark, "search-box-match", false,
                    &conf.colors_dark.search_box.match.fg,
                    &conf.colors_dark.search_box.match.bg);

    test_two_colors(&ctx, &parse_section_colors_dark, "cursor", false,
                    &conf.colors_dark.cursor.text,
                    &conf.colors_dark.cursor.cursor);

    test_enum(&ctx, &parse_section_colors_dark, "alpha-mode", 3,
              (const char *[]){"default", "matching", "all"},
              (int []){ALPHA_MODE_DEFAULT, ALPHA_MODE_MATCHING, ALPHA_MODE_ALL},
              (int *)&conf.colors_dark.alpha_mode);

    test_enum(&ctx, &parse_section_colors_dark, "dim-blend-towards", 2,
              (const char *[]){"black", "white"},
              (int []){DIM_BLEND_TOWARDS_BLACK, DIM_BLEND_TOWARDS_WHITE},
              (int *)&conf.colors_dark.dim_blend_towards);

    for (size_t i = 0; i < 255; i++) {
        char key_name[4];
        sprintf(key_name, "%zu", i);
        test_color(&ctx, &parse_section_colors_dark, key_name, false,
                   &conf.colors_dark.table[i]);
    }

    test_boolean(&ctx, &parse_section_colors_dark, "blur", &conf.colors_dark.blur);

    test_invalid_key(&ctx, &parse_section_colors_dark, "256");

    /* TODO: alpha (float in range 0-1, converted to uint16_t) */

    config_free(&conf);
}

static void
test_section_colors_light(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "colors-light", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_colors_light, "invalid-key");

    test_color(&ctx, &parse_section_colors_light, "foreground", false, &conf.colors_light.fg);
    test_color(&ctx, &parse_section_colors_light, "background", false, &conf.colors_light.bg);
    test_color(&ctx, &parse_section_colors_light, "regular0", false, &conf.colors_light.table[0]);
    test_color(&ctx, &parse_section_colors_light, "regular1", false, &conf.colors_light.table[1]);
    test_color(&ctx, &parse_section_colors_light, "regular2", false, &conf.colors_light.table[2]);
    test_color(&ctx, &parse_section_colors_light, "regular3", false, &conf.colors_light.table[3]);
    test_color(&ctx, &parse_section_colors_light, "regular4", false, &conf.colors_light.table[4]);
    test_color(&ctx, &parse_section_colors_light, "regular5", false, &conf.colors_light.table[5]);
    test_color(&ctx, &parse_section_colors_light, "regular6", false, &conf.colors_light.table[6]);
    test_color(&ctx, &parse_section_colors_light, "regular7", false, &conf.colors_light.table[7]);
    test_color(&ctx, &parse_section_colors_light, "bright0", false, &conf.colors_light.table[8]);
    test_color(&ctx, &parse_section_colors_light, "bright1", false, &conf.colors_light.table[9]);
    test_color(&ctx, &parse_section_colors_light, "bright2", false, &conf.colors_light.table[10]);
    test_color(&ctx, &parse_section_colors_light, "bright3", false, &conf.colors_light.table[11]);
    test_color(&ctx, &parse_section_colors_light, "bright4", false, &conf.colors_light.table[12]);
    test_color(&ctx, &parse_section_colors_light, "bright5", false, &conf.colors_light.table[13]);
    test_color(&ctx, &parse_section_colors_light, "bright6", false, &conf.colors_light.table[14]);
    test_color(&ctx, &parse_section_colors_light, "bright7", false, &conf.colors_light.table[15]);
    test_color(&ctx, &parse_section_colors_light, "dim0", false, &conf.colors_light.dim[0]);
    test_color(&ctx, &parse_section_colors_light, "dim1", false, &conf.colors_light.dim[1]);
    test_color(&ctx, &parse_section_colors_light, "dim2", false, &conf.colors_light.dim[2]);
    test_color(&ctx, &parse_section_colors_light, "dim3", false, &conf.colors_light.dim[3]);
    test_color(&ctx, &parse_section_colors_light, "dim4", false, &conf.colors_light.dim[4]);
    test_color(&ctx, &parse_section_colors_light, "dim5", false, &conf.colors_light.dim[5]);
    test_color(&ctx, &parse_section_colors_light, "dim6", false, &conf.colors_light.dim[6]);
    test_color(&ctx, &parse_section_colors_light, "dim7", false, &conf.colors_light.dim[7]);
    test_color(&ctx, &parse_section_colors_light, "selection-foreground", false, &conf.colors_light.selection_fg);
    test_color(&ctx, &parse_section_colors_light, "selection-background", false, &conf.colors_light.selection_bg);
    test_color(&ctx, &parse_section_colors_light, "urls", false, &conf.colors_light.url);
    test_two_colors(&ctx, &parse_section_colors_light, "jump-labels", false,
                    &conf.colors_light.jump_label.fg,
                    &conf.colors_light.jump_label.bg);
    test_two_colors(&ctx, &parse_section_colors_light, "scrollback-indicator", false,
                    &conf.colors_light.scrollback_indicator.fg,
                    &conf.colors_light.scrollback_indicator.bg);
    test_two_colors(&ctx, &parse_section_colors_light, "search-box-no-match", false,
                    &conf.colors_light.search_box.no_match.fg,
                    &conf.colors_light.search_box.no_match.bg);
    test_two_colors(&ctx, &parse_section_colors_light, "search-box-match", false,
                    &conf.colors_light.search_box.match.fg,
                    &conf.colors_light.search_box.match.bg);

    test_two_colors(&ctx, &parse_section_colors_light, "cursor", false,
                    &conf.colors_light.cursor.text,
                    &conf.colors_light.cursor.cursor);

    test_enum(&ctx, &parse_section_colors_light, "alpha-mode", 3,
              (const char *[]){"default", "matching", "all"},
              (int []){ALPHA_MODE_DEFAULT, ALPHA_MODE_MATCHING, ALPHA_MODE_ALL},
              (int *)&conf.colors_light.alpha_mode);

    test_enum(&ctx, &parse_section_colors_light, "dim-blend-towards", 2,
              (const char *[]){"black", "white"},
              (int []){DIM_BLEND_TOWARDS_BLACK, DIM_BLEND_TOWARDS_WHITE},
              (int *)&conf.colors_light.dim_blend_towards);

    for (size_t i = 0; i < 255; i++) {
        char key_name[4];
        sprintf(key_name, "%zu", i);
        test_color(&ctx, &parse_section_colors_light, key_name, false,
                   &conf.colors_light.table[i]);
    }

    test_boolean(&ctx, &parse_section_colors_light, "blur", &conf.colors_light.blur);

    test_invalid_key(&ctx, &parse_section_colors_light, "256");

    /* TODO: alpha (float in range 0-1, converted to uint16_t) */

    config_free(&conf);
}

static void
test_section_csd(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "csd", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_csd, "invalid-key");

    test_enum(
        &ctx, &parse_section_csd, "preferred",
        3,
        (const char *[]){"none", "client", "server"},
        (int []){CONF_CSD_PREFER_NONE,
                 CONF_CSD_PREFER_CLIENT,
                 CONF_CSD_PREFER_SERVER},
        (int *)&conf.csd.preferred);
    test_uint16(&ctx, &parse_section_csd, "size", &conf.csd.title_height);
    test_color(&ctx, &parse_section_csd, "color", true, &conf.csd.color.title);
    test_uint16(&ctx, &parse_section_csd, "border-width",
                &conf.csd.border_width_visible);
    test_color(&ctx, &parse_section_csd, "border-color", true,
               &conf.csd.color.border);
    test_uint16(&ctx, &parse_section_csd, "button-width",
                &conf.csd.button_width);
    test_color(&ctx, &parse_section_csd, "button-color", true,
               &conf.csd.color.buttons);
    test_color(&ctx, &parse_section_csd, "button-minimize-color", true,
               &conf.csd.color.minimize);
    test_color(&ctx, &parse_section_csd, "button-maximize-color", true,
               &conf.csd.color.maximize);
    test_color(&ctx, &parse_section_csd, "button-close-color", true,
               &conf.csd.color.quit);
    test_boolean(&ctx, &parse_section_csd, "hide-when-maximized",
                 &conf.csd.hide_when_maximized);
    test_boolean(&ctx, &parse_section_csd, "double-click-to-maximize",
                 &conf.csd.double_click_to_maximize);

    /* TODO: verify the ‘set’ bit is actually set for colors */
    /* TODO: font */

    config_free(&conf);
}

static bool
have_modifier(const config_modifier_list_t *mods, const char *mod)
{
    tll_foreach(*mods, it) {
        if (strcmp(it->item, mod) == 0)
            return true;
    }

    return false;
}

static void
test_key_binding(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                 int action, int max_action, const char *const *map,
                 struct config_key_binding_list *bindings,
                 enum key_binding_type type, bool need_argv, bool need_section_id)
{
    xassert(map[action] != NULL);
    xassert(bindings->count == 0);

    const char *key = map[action];

    /* “Randomize” which modifiers to enable */
    const bool ctrl = action % 2;
    const bool alt = action % 3;
    const bool shift = action % 4;
    const bool super = action % 5;
    const bool argv = need_argv;
    const bool section_id = need_section_id;

    xassert(!(argv && section_id));

    static const char *const args[] = {
        "command", "arg1", "arg2", "arg3 has spaces"};

    /* Generate the modifier part of the ‘value’ */
    char modifier_string[32];
    sprintf(modifier_string, "%s%s%s%s",
            ctrl ? XKB_MOD_NAME_CTRL "+" : "",
            alt ? XKB_MOD_NAME_ALT "+" : "",
            shift ? XKB_MOD_NAME_SHIFT "+" : "",
            super ? XKB_MOD_NAME_LOGO "+" : "");

    /* Use a unique symbol for this action (key bindings) */
    const xkb_keysym_t sym = XKB_KEY_a + action;

    /* Mouse button (mouse bindings) */
    const int button_idx = action % ALEN(button_map);
    const int button = button_map[button_idx].code;
    const int click_count = action % 3 + 1;

    /* Finally, generate the ‘value’ (e.g. “Control+shift+x”) */
    char value[128] = {0};

    ctx->key = key;
    ctx->value = value;

    /* First, try setting the empty string */
    if (parse_fun(ctx)) {
        BUG("[%s].%s=<empty>: did not fail to parse as expected",
            ctx->section, ctx->key);
    }

    switch (type) {
    case KEY_BINDING: {
        char sym_name[16];
        xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));

        snprintf(value, sizeof(value), "%s%s%s",
                 argv ? "[command arg1 arg2 \"arg3 has spaces\"] " : section_id ? "[foobar]" : "",
                 modifier_string, sym_name);
        break;
    }

    case MOUSE_BINDING: {
        const char *const button_name = button_map[button_idx].name;
        int chars = snprintf(
            value, sizeof(value), "%s%s%s",
            argv ? "[command arg1 arg2 \"arg3 has spaces\"] " : section_id ? "[foobar]" : "",
            modifier_string, button_name);

        xassert(click_count > 0);
        if (click_count > 1)
            snprintf(&value[chars], sizeof(value) - chars, "-%d", click_count);
        break;
    }
    }

    if (!parse_fun(ctx)) {
        BUG("[%s].%s=%s failed to parse",
            ctx->section, ctx->key, ctx->value);
    }

    const struct config_key_binding *binding =
        &bindings->arr[bindings->count - 1];

    if (argv) {
        if (binding->aux.pipe.args == NULL) {
            BUG("[%s].%s=%s: pipe argv is NULL",
                ctx->section, ctx->key, ctx->value);
        }

        for (size_t i = 0; i < ALEN(args); i++) {
            if (binding->aux.pipe.args[i] == NULL ||
                !streq(binding->aux.pipe.args[i], args[i]))
            {
                BUG("[%s].%s=%s: pipe argv not the expected one: "
                    "mismatch of arg #%zu: expected=\"%s\", got=\"%s\"",
                    ctx->section, ctx->key, ctx->value, i,
                    args[i], binding->aux.pipe.args[i]);
            }
        }

        if (binding->aux.pipe.args[ALEN(args)] != NULL) {
            BUG("[%s].%s=%s: pipe argv not the expected one: "
                "expected NULL terminator at arg #%zu, got=\"%s\"",
                ctx->section, ctx->key, ctx->value,
                ALEN(args), binding->aux.pipe.args[ALEN(args)]);
        }
    } else if (section_id) {
        if (binding->aux.regex_name == NULL) {
            BUG("[%s].%s=%s: regex name is NULL",
                ctx->section, ctx->key, ctx->value);
        }

        if (!streq(binding->aux.regex_name, "foobar")) {
            BUG("[%s].%s=%s: regex name not the expected one: "
                "expected=\"%s\", got=\"%s\"",
                ctx->section, ctx->key, ctx->value,
                "foobar", binding->aux.regex_name);
        }
    } else {
        if (binding->aux.pipe.args != NULL) {
            BUG("[%s].%s=%s: pipe argv not NULL",
                ctx->section, ctx->key, ctx->value);
        }
    }

    if (binding->action != action) {
        BUG("[%s].%s=%s: action mismatch: %d != %d",
            ctx->section, ctx->key, ctx->value, binding->action, action);
    }

    bool have_ctrl = have_modifier(&binding->modifiers, XKB_MOD_NAME_CTRL);
    bool have_alt = have_modifier(&binding->modifiers, XKB_MOD_NAME_ALT);
    bool have_shift = have_modifier(&binding->modifiers, XKB_MOD_NAME_SHIFT);
    bool have_super = have_modifier(&binding->modifiers, XKB_MOD_NAME_LOGO);

    if (have_ctrl != ctrl || have_alt != alt ||
        have_shift != shift || have_super != super)
    {
        BUG("[%s].%s=%s: modifier mismatch:\n"
            "  have:     ctrl=%d, alt=%d, shift=%d, super=%d\n"
            "  expected: ctrl=%d, alt=%d, shift=%d, super=%d",
            ctx->section, ctx->key, ctx->value,
            have_ctrl, have_alt, have_shift, have_super,
            ctrl, alt, shift, super);
    }

    switch (type) {
    case KEY_BINDING:
        if (binding->k.sym != sym) {
            BUG("[%s].%s=%s: key symbol mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value, binding->k.sym, sym);
        }
        break;

    case MOUSE_BINDING:;
        if (binding->m.button != button) {
            BUG("[%s].%s=%s: mouse button mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value, binding->m.button, button);
        }

        if (binding->m.count != click_count) {
            BUG("[%s].%s=%s: mouse button click count mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value,
                binding->m.count, click_count);
        }
        break;
    }


    free_key_binding_list(bindings);
}

enum collision_test_mode {
    FAIL_DIFFERENT_ACTION,
    FAIL_DIFFERENT_ARGV,
    FAIL_MOUSE_OVERRIDE,
    SUCCEED_SAME_ACTION_AND_ARGV,
};

static void
_test_binding_collisions(struct context *ctx,
                         int max_action, const char *const *map,
                         enum key_binding_type type,
                         enum collision_test_mode test_mode)
{
    struct config_key_binding *bindings_array =
        xcalloc(2, sizeof(bindings_array[0]));

    struct config_key_binding_list bindings = {
        .count = 2,
        .arr = bindings_array,
    };

    /* First, verify we get a collision when trying to assign the same
     * key combo to multiple actions */
    bindings.arr[0] = (struct config_key_binding){
        .action = (test_mode == FAIL_DIFFERENT_ACTION
                   ? max_action - 1 : max_action),
        .modifiers = tll_init(),
        .path = "unittest",
    };
    tll_push_back(bindings.arr[0].modifiers, xstrdup(XKB_MOD_NAME_CTRL));

    bindings.arr[1] = (struct config_key_binding){
        .action = max_action,
        .modifiers = tll_init(),
        .path = "unittest",
    };
    tll_push_back(bindings.arr[1].modifiers, xstrdup(XKB_MOD_NAME_CTRL));

    switch (type) {
    case KEY_BINDING:
        bindings.arr[0].k.sym = XKB_KEY_a;
        bindings.arr[1].k.sym = XKB_KEY_a;
        break;

    case MOUSE_BINDING:
        bindings.arr[0].m.button = BTN_LEFT;
        bindings.arr[0].m.count = 1;
        bindings.arr[1].m.button = BTN_LEFT;
        bindings.arr[1].m.count = 1;
        break;
    }

    switch (test_mode) {
    case FAIL_DIFFERENT_ACTION:
        break;

    case FAIL_MOUSE_OVERRIDE:
        tll_free_and_free(ctx->conf->mouse.selection_override_modifiers, free);
        tll_push_back(ctx->conf->mouse.selection_override_modifiers, xstrdup(XKB_MOD_NAME_CTRL));
        break;

    case FAIL_DIFFERENT_ARGV:
    case SUCCEED_SAME_ACTION_AND_ARGV:
        bindings.arr[0].aux.type = BINDING_AUX_PIPE;
        bindings.arr[0].aux.master_copy = true;
        bindings.arr[0].aux.pipe.args = xcalloc(
            4, sizeof(bindings.arr[0].aux.pipe.args[0]));
        bindings.arr[0].aux.pipe.args[0] = xstrdup("/usr/bin/foobar");
        bindings.arr[0].aux.pipe.args[1] = xstrdup("hello");
        bindings.arr[0].aux.pipe.args[2] = xstrdup("world");

        bindings.arr[1].aux.type = BINDING_AUX_PIPE;
        bindings.arr[1].aux.master_copy = true;
        bindings.arr[1].aux.pipe.args = xcalloc(
            4, sizeof(bindings.arr[1].aux.pipe.args[0]));
        bindings.arr[1].aux.pipe.args[0] = xstrdup("/usr/bin/foobar");
        bindings.arr[1].aux.pipe.args[1] = xstrdup("hello");

        if (test_mode == SUCCEED_SAME_ACTION_AND_ARGV)
            bindings.arr[1].aux.pipe.args[2] = xstrdup("world");
        break;
    }

    bool expected_result =
        test_mode == SUCCEED_SAME_ACTION_AND_ARGV ? true : false;

    if (resolve_key_binding_collisions(
            ctx->conf, ctx->section, map, &bindings, type) != expected_result)
    {
        BUG("[%s].%s vs. %s: %s",
            ctx->section, map[max_action - 1], map[max_action],
            (expected_result == true
             ? "invalid key combo collision detected"
             : "key combo collision not detected"));
    }

    if (expected_result == false) {
        if (bindings.count != 1)
            BUG("[%s]: colliding binding not removed", ctx->section);

        if (bindings.arr[0].action !=
            (test_mode == FAIL_DIFFERENT_ACTION ? max_action - 1 : max_action))
        {
            BUG("[%s]: wrong binding removed", ctx->section);
        }
    }

    free_key_binding_list(&bindings);
}

static void
test_binding_collisions(struct context *ctx,
                         int max_action, const char *const *map,
                        enum key_binding_type type)
{
    _test_binding_collisions(ctx, max_action, map, type, FAIL_DIFFERENT_ACTION);
    _test_binding_collisions(ctx, max_action, map, type, FAIL_DIFFERENT_ARGV);
    _test_binding_collisions(ctx, max_action, map, type, SUCCEED_SAME_ACTION_AND_ARGV);

    if (type == MOUSE_BINDING) {
        _test_binding_collisions(
            ctx, max_action, map, type, FAIL_MOUSE_OVERRIDE);
    }
}

static void
test_section_key_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_key_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_KEY_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_key_bindings,
            action, BIND_ACTION_KEY_COUNT - 1,
            binding_action_map, &conf.bindings.key, KEY_BINDING,
            (action >= BIND_ACTION_PIPE_SCROLLBACK && action <= BIND_ACTION_PIPE_COMMAND_OUTPUT) ||
            action == BIND_ACTION_EXEC,
            action >= BIND_ACTION_REGEX_LAUNCH && action <= BIND_ACTION_REGEX_COPY);
    }

    config_free(&conf);
}

static void
test_spawn_terminal_command(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    /* Without a command */
    ctx.key = "spawn-terminal";
    ctx.value = "Control+Shift+n";
    xassert(parse_section_key_bindings(&ctx));
    xassert(conf.bindings.key.count == 1);
    xassert(conf.bindings.key.arr[0].aux.type == BINDING_AUX_NONE);

    /* With a command */
    ctx.value = "[distrobox enter \"my box\"] Control+Shift+m";
    xassert(parse_section_key_bindings(&ctx));
    xassert(conf.bindings.key.count == 2);

    const struct config_key_binding *b = &conf.bindings.key.arr[1];
    xassert(b->action == BIND_ACTION_SPAWN_TERMINAL);
    xassert(b->aux.type == BINDING_AUX_PIPE);
    xassert(streq(b->aux.pipe.args[0], "distrobox"));
    xassert(streq(b->aux.pipe.args[1], "enter"));
    xassert(streq(b->aux.pipe.args[2], "my box"));
    xassert(b->aux.pipe.args[3] == NULL);

    /* Syntax errors */
    ctx.value = "[distrobox enter Control+Shift+o";
    xassert(!parse_section_key_bindings(&ctx));

    /* exec requires a command */
    ctx.key = "exec";
    ctx.value = "Control+Shift+a";
    xassert(!parse_section_key_bindings(&ctx));
    ctx.value = "[foo.sh --bar] Control+Shift+a";
    xassert(parse_section_key_bindings(&ctx));
    xassert(conf.bindings.key.count == 3);
    xassert(conf.bindings.key.arr[2].action == BIND_ACTION_EXEC);
    xassert(conf.bindings.key.arr[2].aux.type == BINDING_AUX_PIPE);
    xassert(streq(conf.bindings.key.arr[2].aux.pipe.args[1], "--bar"));

    config_free(&conf);
}

static void
test_section_key_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx, BIND_ACTION_KEY_COUNT - 1, binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static void
test_section_search_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "search-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_search_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_SEARCH_COUNT; action++) {
        if (search_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_search_bindings,
            action, BIND_ACTION_SEARCH_COUNT - 1,
            search_binding_action_map, &conf.bindings.search, KEY_BINDING,
            false, false);
    }

    config_free(&conf);
}

static void
test_section_search_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "search-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_SEARCH_COUNT - 1, search_binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static void
test_section_url_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "rul-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_url_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_URL_COUNT; action++) {
        if (url_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_url_bindings,
            action, BIND_ACTION_URL_COUNT - 1,
            url_binding_action_map, &conf.bindings.url, KEY_BINDING,
            false, false);
    }

    config_free(&conf);
}

static void
test_section_url_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "url-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_URL_COUNT - 1, url_binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static void
test_section_vi_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "vi-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_vi_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_VI_COUNT; action++) {
        if (vi_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_vi_bindings,
            action, BIND_ACTION_VI_COUNT - 1,
            vi_binding_action_map, &conf.bindings.vi, KEY_BINDING,
            false, false);
    }

    config_free(&conf);
}

static void
test_section_vi_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "vi-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_VI_COUNT - 1, vi_binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static bool
has_vi_mode_start_binding(const struct config *conf)
{
    for (size_t i = 0; i < conf->bindings.key.count; i++) {
        if (conf->bindings.key.arr[i].action == BIND_ACTION_VI_MODE_START)
            return true;
    }
    return false;
}

static void
test_vi_mode_start_default_binding(void)
{
    /* No default binding unless vi mode is enabled */
    struct config conf = {0};
    add_default_key_bindings(&conf);
    add_default_vi_mode_start_binding(&conf);
    xassert(!has_vi_mode_start_binding(&conf));

    /* Enabled: Control+Shift+space */
    conf.vi_mode = true;
    add_default_vi_mode_start_binding(&conf);
    xassert(has_vi_mode_start_binding(&conf));
    const struct config_key_binding *b =
        &conf.bindings.key.arr[conf.bindings.key.count - 1];
    xassert(b->action == BIND_ACTION_VI_MODE_START);
    xassert(b->k.sym == XKB_KEY_space);
    xassert(tll_length(b->modifiers) == 2);

    /* Idempotent (overrides are applied more than once in server mode) */
    const size_t count = conf.bindings.key.count;
    add_default_vi_mode_start_binding(&conf);
    xassert(conf.bindings.key.count == count);
    xassert(resolve_key_binding_collisions(
                &conf, "key-bindings", binding_action_map,
                &conf.bindings.key, KEY_BINDING));
    config_free(&conf);

    /* The user bound Control+Shift+space to something else: no
     * default binding, and no collision */
    memset(&conf, 0, sizeof(conf));
    conf.vi_mode = true;
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};
    ctx.key = "quit";
    ctx.value = "Control+Shift+space";
    xassert(parse_section_key_bindings(&ctx));
    add_default_vi_mode_start_binding(&conf);
    xassert(!has_vi_mode_start_binding(&conf));
    xassert(resolve_key_binding_collisions(
                &conf, "key-bindings", binding_action_map,
                &conf.bindings.key, KEY_BINDING));
    config_free(&conf);

    /* The user bound vi-mode-start: no additional default */
    memset(&conf, 0, sizeof(conf));
    conf.vi_mode = true;
    ctx.conf = &conf;
    ctx.key = "vi-mode-start";
    ctx.value = "Control+Alt+v";
    xassert(parse_section_key_bindings(&ctx));
    add_default_vi_mode_start_binding(&conf);
    xassert(conf.bindings.key.count == 1);
    config_free(&conf);
}

static void
test_section_mouse_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_mouse_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_mouse_bindings,
            action, BIND_ACTION_COUNT - 1,
            binding_action_map, &conf.bindings.mouse, MOUSE_BINDING,
            false, false);
    }

    config_free(&conf);
}

static void
test_section_mouse_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_COUNT - 1, binding_action_map, MOUSE_BINDING);

    config_free(&conf);
}

static void
test_section_text_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "text-bindings", .path = "unittest"};

    ctx.key = "abcd";
    ctx.value = XKB_MOD_NAME_CTRL "+" XKB_MOD_NAME_SHIFT "+x";
    xassert(parse_section_text_bindings(&ctx));

    ctx.key = "\\x07";
    xassert(parse_section_text_bindings(&ctx));

    ctx.key = "\\x1g";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\x1";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\x";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\y";
    xassert(!parse_section_text_bindings(&ctx));

#if 0
    /* Invalid modifier and key names are detected later, when a
     * layout is applied */
    ctx.key = "abcd";
    ctx.value = "InvalidMod+y";
    xassert(!parse_section_text_bindings(&ctx));
#endif
    config_free(&conf);
}

static void
test_section_environment(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "environment", .path = "unittest"};

    /* A single variable */
    ctx.key = "FOO";
    ctx.value = "bar";
    xassert(parse_section_environment(&ctx));
    xassert(tll_length(conf.env_vars) == 1);
    xassert(streq(tll_front(conf.env_vars).name, "FOO"));
    xassert(streq(tll_front(conf.env_vars).value, "bar"));

    /* Add a second variable */
    ctx.key = "BAR";
    ctx.value = "123";
    xassert(parse_section_environment(&ctx));
    xassert(tll_length(conf.env_vars) == 2);
    xassert(streq(tll_back(conf.env_vars).name, "BAR"));
    xassert(streq(tll_back(conf.env_vars).value, "123"));

    /* Replace the *value* of the first variable */
    ctx.key = "FOO";
    ctx.value = "456";
    xassert(parse_section_environment(&ctx));
    xassert(tll_length(conf.env_vars) == 2);
    xassert(streq(tll_front(conf.env_vars).name, "FOO"));
    xassert(streq(tll_front(conf.env_vars).value, "456"));
    xassert(streq(tll_back(conf.env_vars).name, "BAR"));
    xassert(streq(tll_back(conf.env_vars).value, "123"));

    config_free(&conf);
}

static void
test_section_tweak(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "tweak", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_tweak, "invalid-key");

    test_enum(
        &ctx, &parse_section_tweak, "scaling-filter",
        5,
        (const char *[]){"none", "nearest", "bilinear", "cubic", "lanczos3"},
        (int []){FCFT_SCALING_FILTER_NONE,
                 FCFT_SCALING_FILTER_NEAREST,
                 FCFT_SCALING_FILTER_BILINEAR,
                 FCFT_SCALING_FILTER_CUBIC,
                 FCFT_SCALING_FILTER_LANCZOS3},
        (int *)&conf.tweak.fcft_filter);

    test_boolean(&ctx, &parse_section_tweak, "overflowing-glyphs",
                 &conf.tweak.overflowing_glyphs);

    test_enum(
        &ctx, &parse_section_tweak, "render-timer",
        4,
        (const char *[]){"none", "osd", "log", "both"},
        (int []){RENDER_TIMER_NONE,
                 RENDER_TIMER_OSD,
                 RENDER_TIMER_LOG,
                 RENDER_TIMER_BOTH},
        (int *)&conf.tweak.render_timer);

    test_float(&ctx, &parse_section_tweak, "box-drawing-base-thickness",
                &conf.tweak.box_drawing_base_thickness);
    test_boolean(&ctx, &parse_section_tweak, "box-drawing-solid-shades",
        &conf.tweak.box_drawing_solid_shades);

#if 0  /* Must be less than 16ms */
    test_uint32(&ctx, &parse_section_tweak, "delayed-render-lower",
                &conf.tweak.delayed_render_lower_ns);
    test_uint32(&ctx, &parse_section_tweak, "delayed-render-upper",
                &conf.tweak.delayed_render_upper_ns);
#endif
    test_boolean(&ctx, &parse_section_tweak, "damage-whole-window",
                 &conf.tweak.damage_whole_window);

#if defined(FOOT_GRAPHEME_CLUSTERING)
    test_boolean(&ctx, &parse_section_tweak, "grapheme-shaping",
                 &conf.tweak.grapheme_shaping);
#else
    /* TODO: the setting still exists, but is always forced to ‘false’. */
#endif

    test_enum(
        &ctx, &parse_section_tweak, "grapheme-width-method",
        3,
        (const char *[]){"wcswidth", "double-width", "max"},
        (int []){GRAPHEME_WIDTH_WCSWIDTH,
                 GRAPHEME_WIDTH_DOUBLE,
                 GRAPHEME_WIDTH_MAX},
        (int *)&conf.tweak.grapheme_width_method);

    test_boolean(&ctx, &parse_section_tweak, "font-monospace-warn",
                 &conf.tweak.font_monospace_warn);

    test_float(&ctx, &parse_section_tweak, "bold-text-in-bright-amount",
               &conf.bold_in_bright.amount);

    test_uint32(&ctx, &parse_section_tweak, "min-stride-alignment",
                &conf.tweak.min_stride_alignment);

#if 0 /* Must be equal to, or less than INT32_MAX */
    test_uint32(&ctx, &parse_section_tweak, "max-shm-pool-size-mb",
                &conf.tweak.max_shm_pool_size);
#endif

    config_free(&conf);
}

int
main(int argc, const char *const *argv)
{
    FcInit();
    log_init(LOG_COLORIZE_AUTO, false, 0, LOG_CLASS_ERROR);
    test_section_main();
    test_include();
    test_section_security();
    test_section_bell();
    test_section_desktop_notifications();
    test_section_scrollback();
    test_section_url();
    test_section_cursor();
    test_section_mouse();
    test_section_touch();
    test_section_colors_dark();
    test_section_colors_light();
    test_section_csd();
    test_section_key_bindings();
    test_spawn_terminal_command();
    test_section_key_bindings_collisions();
    test_section_search_bindings();
    test_section_search_bindings_collisions();
    test_section_url_bindings();
    test_section_url_bindings_collisions();
    test_section_vi_bindings();
    test_section_vi_bindings_collisions();
    test_vi_mode_start_default_binding();
    test_section_mouse_bindings();
    test_section_mouse_bindings_collisions();
    test_section_text_bindings();
    test_section_environment();
    test_section_tweak();
    log_deinit();
    FcFini();
    return 0;
}
