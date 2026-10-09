#include "fontinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <hb.h>
#include <hb-ot.h>

#define LOG_MODULE "fontinfo"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "util.h"
#include "xmalloc.h"

enum feature_source {
    SOURCE_NONE,
    SOURCE_DEFAULT,  /* Enabled by default by HarfBuzz */
    SOURCE_FONT,     /* 'fontfeatures' in the font pattern */
    SOURCE_CONFIG,   /* 'font-features' */
};

struct feature {
    hb_tag_t tag;
    unsigned value;
    enum feature_source source;
    bool in_font;
    char name[128];
};

struct features {
    struct feature *arr;
    size_t count;
};

static struct feature *
feature_get(struct features *features, hb_tag_t tag)
{
    for (size_t i = 0; i < features->count; i++) {
        if (features->arr[i].tag == tag)
            return &features->arr[i];
    }

    features->arr = xreallocarray(
        features->arr, features->count + 1, sizeof(features->arr[0]));

    struct feature *f = &features->arr[features->count++];
    *f = (struct feature){.tag = tag};
    return f;
}

static void
feature_apply(struct features *features, const char *str,
              enum feature_source source)
{
    hb_feature_t hb_feature;
    if (!hb_feature_from_string(str, -1, &hb_feature))
        return;

    struct feature *f = feature_get(features, hb_feature.tag);
    f->value = hb_feature.value;
    f->source = source;
}

static void
add_font_features(struct features *features, hb_face_t *face, hb_tag_t table)
{
    unsigned count = hb_ot_layout_table_get_feature_tags(
        face, table, 0, NULL, NULL);

    hb_tag_t *tags = xmalloc(count * sizeof(tags[0]));
    hb_ot_layout_table_get_feature_tags(face, table, 0, &count, tags);

    for (unsigned i = 0; i < count; i++) {
        struct feature *f = feature_get(features, tags[i]);
        f->in_font = true;

        if (f->name[0] != '\0')
            continue;

        /* Stylistic sets and character variants may have UI names */
        hb_ot_name_id_t label = HB_OT_NAME_ID_INVALID;
        if (!hb_ot_layout_feature_get_name_ids(
                face, table, i, &label, NULL, NULL, NULL, NULL))
        {
            continue;
        }

        unsigned len = sizeof(f->name);
        hb_ot_name_get_utf8(face, label, HB_LANGUAGE_INVALID, &len, f->name);
    }

    free(tags);
}

/* Descriptions of registered OpenType features, for fonts that don't
 * name their features */
static const struct {
    const char *tag;
    const char *desc;
} registered[] = {
    {"aalt", "Access All Alternates"},
    {"afrc", "Alternative Fractions"},
    {"c2sc", "Small Capitals From Capitals"},
    {"calt", "Contextual Alternates"},
    {"case", "Case-Sensitive Forms"},
    {"ccmp", "Glyph Composition/Decomposition"},
    {"clig", "Contextual Ligatures"},
    {"cpsp", "Capital Spacing"},
    {"curs", "Cursive Positioning"},
    {"dlig", "Discretionary Ligatures"},
    {"dnom", "Denominators"},
    {"fina", "Terminal Forms"},
    {"frac", "Fractions"},
    {"fwid", "Full Widths"},
    {"hlig", "Historical Ligatures"},
    {"init", "Initial Forms"},
    {"ital", "Italics"},
    {"kern", "Kerning"},
    {"liga", "Standard Ligatures"},
    {"lnum", "Lining Figures"},
    {"locl", "Localized Forms"},
    {"mark", "Mark Positioning"},
    {"medi", "Medial Forms"},
    {"mkmk", "Mark to Mark Positioning"},
    {"numr", "Numerators"},
    {"onum", "Oldstyle Figures"},
    {"ordn", "Ordinals"},
    {"pnum", "Proportional Figures"},
    {"rclt", "Required Contextual Alternates"},
    {"rlig", "Required Ligatures"},
    {"salt", "Stylistic Alternates"},
    {"sinf", "Scientific Inferiors"},
    {"smcp", "Small Capitals"},
    {"subs", "Subscript"},
    {"sups", "Superscript"},
    {"swsh", "Swash"},
    {"tnum", "Tabular Figures"},
    {"zero", "Slashed Zero"},
};

static const char *
registered_description(hb_tag_t tag)
{
    for (size_t i = 0; i < ALEN(registered); i++) {
        if (hb_tag_from_string(registered[i].tag, 4) == tag)
            return registered[i].desc;
    }

    char str[5];
    hb_tag_to_string(tag, str);

    /* cv01-cv99, ss01-ss20 */
    if (str[0] == 'c' && str[1] == 'v')
        return "Character Variant";
    if (str[0] == 's' && str[1] == 's')
        return "Stylistic Set";

    return "";
}

static int
feature_cmp(const void *_a, const void *_b)
{
    const struct feature *a = _a;
    const struct feature *b = _b;
    return a->tag < b->tag ? -1 : a->tag > b->tag;
}

bool
fontinfo_list_features(const struct config *conf, const char *pattern)
{
    if (pattern == NULL) {
        if (conf->fonts[0].count == 0) {
            LOG_ERR("no font configured");
            return false;
        }
        pattern = conf->fonts[0].arr[0].pattern;
    }

    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (pat == NULL) {
        LOG_ERR("%s: failed to parse font pattern", pattern);
        return false;
    }

    /* Remember the requested family, before substitutions add more */
    FcChar8 *requested_family = NULL;
    if (FcPatternGetString(pat, FC_FAMILY, 0, &requested_family) == FcResultMatch)
        requested_family = (FcChar8 *)xstrdup((const char *)requested_family);
    else
        requested_family = NULL;

    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);

    if (match == NULL) {
        LOG_ERR("%s: failed to match font", pattern);
        return false;
    }

    FcChar8 *file = NULL;
    FcChar8 *full_name = NULL;
    int index = 0;

    FcPatternGetString(match, FC_FILE, 0, &file);
    FcPatternGetString(match, FC_FULLNAME, 0, &full_name);
    FcPatternGetInteger(match, FC_INDEX, 0, &index);

    /* fontconfig always matches *something*; tell the user if it isn't
     * the requested family */
    if (requested_family != NULL) {
        bool family_matched = false;
        FcChar8 *family;
        for (int i = 0;
             FcPatternGetString(match, FC_FAMILY, i, &family) == FcResultMatch;
             i++)
        {
            if (FcStrCmpIgnoreCase(family, requested_family) == 0) {
                family_matched = true;
                break;
            }
        }

        if (!family_matched) {
            fprintf(stderr,
                    "note: no font named '%s' found; showing the closest match\n\n",
                    (const char *)requested_family);
        }

        free(requested_family);
    }

    hb_blob_t *blob = file != NULL
        ? hb_blob_create_from_file_or_fail((const char *)file)
        : NULL;

    if (blob == NULL) {
        LOG_ERR("%s: failed to load font file", file != NULL ? (const char *)file : pattern);
        FcPatternDestroy(match);
        return false;
    }

    hb_face_t *face = hb_face_create(blob, index & 0xffff);

    struct features features = {0};

    /* Features HarfBuzz enables by default, for horizontal text */
    static const char *const defaults[] = {
        "abvm", "blwm", "ccmp", "locl", "mark", "mkmk", "rlig",
        "calt", "clig", "curs", "dist", "kern", "liga", "rclt",
    };
    for (size_t i = 0; i < ALEN(defaults); i++)
        feature_apply(&features, defaults[i], SOURCE_DEFAULT);

    /* 'fontfeatures' from the font pattern, then 'font-features' */
    FcChar8 *fc_feature;
    for (int i = 0;
         FcPatternGetString(match, FC_FONT_FEATURES, i, &fc_feature) == FcResultMatch;
         i++)
    {
        feature_apply(&features, (const char *)fc_feature, SOURCE_FONT);
    }

    for (size_t i = 0; i < conf->font_features[0].count; i++)
        feature_apply(&features, conf->font_features[0].arr[i], SOURCE_CONFIG);

    add_font_features(&features, face, HB_OT_TAG_GSUB);
    add_font_features(&features, face, HB_OT_TAG_GPOS);

    qsort(features.arr, features.count, sizeof(features.arr[0]), &feature_cmp);

    printf("%s\n%s\n\n",
           full_name != NULL ? (const char *)full_name : pattern,
           (const char *)file);

    printf("  %-4s  %-7s  %-13s  %s\n", "TAG", "STATE", "ENABLED BY", "DESCRIPTION");

    for (size_t i = 0; i < features.count; i++) {
        const struct feature *f = &features.arr[i];

        /* Don't list default features the font doesn't have */
        if (!f->in_font && f->source == SOURCE_DEFAULT)
            continue;

        char tag[5];
        hb_tag_to_string(f->tag, tag);
        tag[4] = '\0';

        char state[16];
        if (f->value == 0)
            strcpy(state, "off");
        else if (f->value == 1)
            strcpy(state, "on");
        else
            snprintf(state, sizeof(state), "on (%u)", f->value);

        const char *source =
            f->source == SOURCE_DEFAULT ? "default" :
            f->source == SOURCE_FONT ? "font pattern" :
            f->source == SOURCE_CONFIG ? "font-features" : "";

        const char *desc =
            f->name[0] != '\0' ? f->name : registered_description(f->tag);

        printf("  %-4s  %-7s  %-13s  %s%s\n",
               tag, state, source, desc,
               f->in_font ? "" : " (not in this font)");
    }

    free(features.arr);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    FcPatternDestroy(match);
    return true;
}
