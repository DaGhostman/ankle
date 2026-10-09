#include "shaping.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define LOG_MODULE "shaping"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "xmalloc.h"

/* Must be a power of two */
#define BUCKET_COUNT 4096

/* Soft limit; entries used in the last frame are never evicted */
#define MAX_ENTRIES 4096

struct entry {
    struct entry *next;
    uint64_t hash;
    uint64_t last_used;  /* Frame number */

    struct fcft_font *font;
    enum fcft_subpixel subpixel;
    struct fcft_text_run *run;  /* NULL if shaping failed */

    size_t len;
    char32_t text[];
};

struct shaping_cache {
    mtx_t lock;
    struct entry *buckets[BUCKET_COUNT];
    size_t count;
    uint64_t frame;
};

static uint64_t
hash_key(const struct fcft_font *font, enum fcft_subpixel subpixel,
         size_t len, const char32_t text[static len])
{
    /* FNV-1a */
    uint64_t hash = 0xcbf29ce484222325ull;

    hash = (hash ^ (uintptr_t)font) * 0x100000001b3ull;
    hash = (hash ^ (uint64_t)subpixel) * 0x100000001b3ull;

    for (size_t i = 0; i < len; i++)
        hash = (hash ^ text[i]) * 0x100000001b3ull;

    return hash;
}

static struct entry *
find(struct shaping_cache *cache, uint64_t hash, const struct fcft_font *font,
     enum fcft_subpixel subpixel, size_t len, const char32_t text[static len])
{
    for (struct entry *e = cache->buckets[hash & (BUCKET_COUNT - 1)];
         e != NULL;
         e = e->next)
    {
        if (e->hash == hash &&
            e->font == font &&
            e->subpixel == subpixel &&
            e->len == len &&
            memcmp(e->text, text, len * sizeof(text[0])) == 0)
        {
            return e;
        }
    }

    return NULL;
}

static void
entry_destroy(struct entry *e)
{
    fcft_text_run_destroy(e->run);
    free(e);
}

struct shaping_cache *
shaping_cache_init(void)
{
    struct shaping_cache *cache = xcalloc(1, sizeof(*cache));

    if (mtx_init(&cache->lock, mtx_plain) != thrd_success) {
        LOG_ERR("failed to instantiate shaping cache mutex");
        free(cache);
        return NULL;
    }

    return cache;
}

void
shaping_cache_destroy(struct shaping_cache *cache)
{
    if (cache == NULL)
        return;

    shaping_cache_flush(cache);
    mtx_destroy(&cache->lock);
    free(cache);
}

void
shaping_cache_flush(struct shaping_cache *cache)
{
    if (cache == NULL)
        return;

    for (size_t i = 0; i < BUCKET_COUNT; i++) {
        struct entry *e = cache->buckets[i];
        while (e != NULL) {
            struct entry *next = e->next;
            entry_destroy(e);
            e = next;
        }
        cache->buckets[i] = NULL;
    }

    cache->count = 0;
}

void
shaping_cache_frame_done(struct shaping_cache *cache)
{
    if (cache == NULL)
        return;

    cache->frame++;

    if (cache->count <= MAX_ENTRIES)
        return;

    /* Evict everything that wasn't used in the frame just rendered */
    size_t evicted = 0;
    for (size_t i = 0; i < BUCKET_COUNT; i++) {
        struct entry **prev = &cache->buckets[i];
        struct entry *e = *prev;

        while (e != NULL) {
            struct entry *next = e->next;

            if (e->last_used + 1 < cache->frame) {
                *prev = next;
                entry_destroy(e);
                evicted++;
            } else
                prev = &e->next;

            e = next;
        }
    }

    xassert(evicted <= cache->count);
    cache->count -= evicted;

    LOG_DBG("evicted %zu entries, %zu remaining", evicted, cache->count);
}

const struct fcft_text_run *
shaping_cache_lookup(struct shaping_cache *cache, struct fcft_font *font,
                     enum fcft_subpixel subpixel, size_t len,
                     const char32_t text[static len])
{
    const uint64_t hash = hash_key(font, subpixel, len, text);

    mtx_lock(&cache->lock);
    struct entry *e = find(cache, hash, font, subpixel, len, text);
    if (e != NULL) {
        e->last_used = cache->frame;
        const struct fcft_text_run *run = e->run;
        mtx_unlock(&cache->lock);
        return run;
    }
    mtx_unlock(&cache->lock);

    /* Shape without holding the lock; this is the expensive part */
    struct fcft_text_run *run = fcft_rasterize_text_run_utf32(
        font, len, (const uint32_t *)text, subpixel);

    mtx_lock(&cache->lock);

    /* Another thread may have shaped the same text while we weren't
     * holding the lock */
    e = find(cache, hash, font, subpixel, len, text);
    if (e != NULL) {
        e->last_used = cache->frame;
        const struct fcft_text_run *existing = e->run;
        mtx_unlock(&cache->lock);

        fcft_text_run_destroy(run);
        return existing;
    }

    e = xmalloc(sizeof(*e) + len * sizeof(text[0]));
    *e = (struct entry){
        .hash = hash,
        .last_used = cache->frame,
        .font = font,
        .subpixel = subpixel,
        .run = run,
        .len = len,
    };
    memcpy(e->text, text, len * sizeof(text[0]));

    struct entry **bucket = &cache->buckets[hash & (BUCKET_COUNT - 1)];
    e->next = *bucket;
    *bucket = e;
    cache->count++;

    mtx_unlock(&cache->lock);
    return run;
}
