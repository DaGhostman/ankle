#pragma once

#include <stddef.h>
#include <uchar.h>

#include <fcft/fcft.h>

/*
 * Cache of shaped text runs, used when rendering ligatures.
 *
 * fcft does not cache text-runs (every call re-shapes the text), so
 * we keep the results around, keyed on font, subpixel mode and the
 * run's text.
 *
 * Note that the bundled (patched) fcft owns the glyph images of
 * text-runs; the cache must be flushed before fonts are destroyed.
 *
 * Lookups are thread safe, and may be done concurrently from the
 * render worker threads. Runs returned by lookups stay valid until
 * the next call to shaping_cache_frame_done() or
 * shaping_cache_flush(), which must *not* be called while lookups
 * are in progress.
 */
struct shaping_cache;

struct shaping_cache *shaping_cache_init(void);
void shaping_cache_destroy(struct shaping_cache *cache);

/* Drop all entries. Must be called whenever the fonts change */
void shaping_cache_flush(struct shaping_cache *cache);

/* Evicts old entries, if the cache has grown too large */
void shaping_cache_frame_done(struct shaping_cache *cache);

/* Returns NULL if the text could not be shaped */
const struct fcft_text_run *shaping_cache_lookup(
    struct shaping_cache *cache, struct fcft_font *font,
    enum fcft_subpixel subpixel, size_t len, const char32_t text[static len]);
