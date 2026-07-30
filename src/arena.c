/*
 * Arena allocator — chunk-based bump allocator, no individual frees.
 * Allocates new chunks as needed; previous allocations always remain valid.
 * The entire arena is freed at once. Perfect for request-scoped work.
 */

#include "nc.h"
#include <stdlib.h>
#include <string.h>

static nc_arena_chunk *chunk_new(size_t data_cap) {
    nc_arena_chunk *c = (nc_arena_chunk *)malloc(sizeof(nc_arena_chunk) + data_cap);
    if (!c) return NULL;
    c->next = NULL;
    c->cap = data_cap;
    c->pos = 0;
    return c;
}

void nc_arena_init(nc_arena *a, size_t cap) {
    nc_arena_chunk *c = chunk_new(cap);
    a->head = c;
    a->current = c;
    a->chunk_size = cap;
}

void *nc_arena_alloc(nc_arena *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    nc_arena_chunk *c = a->current;
    if (!c || c->pos + size > c->cap) {
        /* Need a new chunk — at least double the request or chunk_size */
        size_t new_cap = a->chunk_size;
        if (new_cap < size) new_cap = size;
        nc_arena_chunk *nc = chunk_new(new_cap);
        if (!nc) return NULL;
        if (c) c->next = nc;
        else a->head = nc;
        a->current = nc;
        c = nc;
    }

    void *ptr = c->data + c->pos;
    c->pos += size;
    return ptr;
}

char *nc_arena_dup(nc_arena *a, const char *s, size_t len) {
    char *d = (char *)nc_arena_alloc(a, len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

void nc_arena_reset(nc_arena *a) {
    /* Free all chunks except the first, reset first chunk */
    nc_arena_chunk *c = a->head;
    if (!c) return;
    nc_arena_chunk *next = c->next;
    while (next) {
        nc_arena_chunk *tmp = next->next;
        free(next);
        next = tmp;
    }
    c->next = NULL;
    c->pos = 0;
    a->current = c;
}

void nc_arena_free(nc_arena *a) {
    nc_arena_chunk *c = a->head;
    while (c) {
        nc_arena_chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
    a->current = NULL;
    a->chunk_size = 0;
}

/* ── Tests ──────────────────────────────────────────────────────── */
