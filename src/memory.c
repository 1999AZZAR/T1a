/*
 * Guardian Memory — Persistent entity-relation memory backend.
 *
 * Replaces flat-file TSV with guardian-style entity model.
 * Each entity has: name, type, observations[], created_at.
 * Persisted as JSON: one entity per line (JSONL) for append-friendliness.
 *
 * Search: case-insensitive keyword match on name + observations.
 * No external deps. No SQLite.
 */

#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

/* ── Constants ────────────────────────────────────────────────── */

#define GM_MAX_ENTITIES  256
#define GM_MAX_OBS       16
#define GM_FIELD_LEN     512
#define GM_PATH_LEN      1024

/* ── Context (entity struct from nc.h) ───────────────────────── */

typedef struct {
    char path[GM_PATH_LEN];
    gm_entity entities[GM_MAX_ENTITIES];
    int  entity_count;
    bool loaded;
} gm_ctx;

/* ── File I/O ─────────────────────────────────────────────────── */

static char *gm_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); *out_len = 0; return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); *out_len = 0; return NULL; }
    *out_len = fread(buf, 1, (size_t)sz, f);
    buf[*out_len] = '\0';
    fclose(f);
    return buf;
}

static bool gm_write_file(const char *path, const char *data, size_t len) {
    char tmp[GM_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        return false;
    }
    fclose(f);
    return rename(tmp, path) == 0;
}

/* ── JSON serialization ──────────────────────────────────────── */

/* Escape a string for JSON, appending to buffer */
static void gm_escape_json(const char *in, char *out, size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 6 < out_cap; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if ((unsigned char)c >= 0x20) { out[j++] = c; }
    }
    out[j] = '\0';
}

static void gm_entity_to_json(gm_entity *e, char *buf, size_t cap) {
    int off = 0;
    char name_esc[GM_FIELD_LEN * 2], type_esc[GM_FIELD_LEN * 2];
    gm_escape_json(e->name, name_esc, sizeof(name_esc));
    gm_escape_json(e->type, type_esc, sizeof(type_esc));
    off += snprintf(buf + off, cap - (size_t)off,
        "{\"n\":\"%s\",\"t\":\"%s\",\"o\":[", name_esc, type_esc);
    for (int i = 0; i < e->obs_count; i++) {
        char obs_esc[GM_FIELD_LEN * 2];
        gm_escape_json(e->observations[i], obs_esc, sizeof(obs_esc));
        off += snprintf(buf + off, cap - (size_t)off,
            "%s\"%s\"", i > 0 ? "," : "", obs_esc);
    }
    off += snprintf(buf + off, cap - (size_t)off, "],\"c\":%ld}", e->created_at);
}

/* Find an entity by name (case-insensitive) */
static gm_entity *gm_find(gm_ctx *ctx, const char *name) {
    for (int i = 0; i < ctx->entity_count; i++) {
        if (strcasecmp(ctx->entities[i].name, name) == 0)
            return &ctx->entities[i];
    }
    return NULL;
}

/* ── Load from file ──────────────────────────────────────────── */

static void gm_load(gm_ctx *ctx) {
    if (ctx->loaded) return;

    size_t len;
    char *data = gm_read_file(ctx->path, &len);
    if (!data) { ctx->loaded = true; return; }

    ctx->entity_count = 0;
    char *line = data;
    while (*line && ctx->entity_count < GM_MAX_ENTITIES) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        /* Parse JSON line: {"n":"name","t":"type","o":["obs1","obs2"],"c":ts} */
        nc_arena a;
        nc_arena_init(&a, 2048);
        nc_json *root = nc_json_parse(&a, line, strlen(line));
        if (root && root->type == NC_JSON_OBJECT) {
            gm_entity *e = &ctx->entities[ctx->entity_count++];
            memset(e, 0, sizeof(gm_entity));

            nc_str n = nc_json_str(nc_json_get(root, "n"), "");
            if (n.len > 0) { size_t cl = n.len < GM_FIELD_LEN-1 ? n.len : GM_FIELD_LEN-1; memcpy(e->name, n.ptr, cl); }

            nc_str t = nc_json_str(nc_json_get(root, "t"), "");
            if (t.len > 0) { size_t cl = t.len < GM_FIELD_LEN-1 ? t.len : GM_FIELD_LEN-1; memcpy(e->type, t.ptr, cl); }

            nc_json *obs = nc_json_get(root, "o");
            if (obs && obs->type == NC_JSON_ARRAY) {
                for (int i = 0; i < obs->array.count && i < GM_MAX_OBS; i++) {
                    nc_str o = nc_json_str(&obs->array.items[i], "");
                    if (o.len > 0) {
                        size_t cl = o.len < GM_FIELD_LEN-1 ? o.len : GM_FIELD_LEN-1;
                        memcpy(e->observations[e->obs_count], o.ptr, cl);
                        e->observations[e->obs_count][cl] = '\0';
                        e->obs_count++;
                    }
                }
            }

            e->created_at = (long)nc_json_num(nc_json_get(root, "c"), 0);
        }

        nc_arena_free(&a);
        if (eol) line = eol + 1; else break;
    }

    free(data);
    ctx->loaded = true;
    nc_log(NC_LOG_INFO, "Guardian: loaded %d entities from %s", ctx->entity_count, ctx->path);
}

/* ── Save to file (full rewrite) ─────────────────────────────── */

static void gm_save(gm_ctx *ctx) {
    size_t cap = (size_t)ctx->entity_count * 1024 + 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return;

    size_t off = 0;
    for (int i = 0; i < ctx->entity_count; i++) {
        char entity_json[1024];
        gm_entity_to_json(&ctx->entities[i], entity_json, sizeof(entity_json));
        int n = snprintf(buf + off, cap - off, "%s\n", entity_json);
        if (n > 0) off += (size_t)n;
    }

    gm_write_file(ctx->path, buf, off);
    free(buf);
}

/* ── Prune by age: remove entities older than N days ──────────── */

static void gm_prune(gm_ctx *ctx, int max_days) {
    time_t now = time(NULL);
    long cutoff = (long)(now - (time_t)max_days * 86400);
    int pruned = 0;

    for (int i = 0; i < ctx->entity_count; ) {
        if (ctx->entities[i].created_at > 0 && ctx->entities[i].created_at < cutoff) {
            for (int j = i; j < ctx->entity_count - 1; j++)
                ctx->entities[j] = ctx->entities[j + 1];
            ctx->entity_count--;
            pruned++;
        } else {
            i++;
        }
    }

    if (pruned > 0) {
        nc_log(NC_LOG_INFO, "Guardian: pruned %d old entities", pruned);
        gm_save(ctx);
    }
}

/* ── CRUD Operations ─────────────────────────────────────────── */

static bool guardian_store_entity(gm_ctx *ctx, const char *name, const char *type,
                                   const char *observation) {
    gm_load(ctx);

    gm_entity *e = gm_find(ctx, name);
    if (!e) {
        if (ctx->entity_count >= GM_MAX_ENTITIES) return false;
        e = &ctx->entities[ctx->entity_count++];
        memset(e, 0, sizeof(gm_entity));
        nc_strlcpy(e->name, name, sizeof(e->name));
        if (type && type[0]) nc_strlcpy(e->type, type, sizeof(e->type));
        e->created_at = (long)time(NULL);
    }

    if (observation && observation[0] && e->obs_count < GM_MAX_OBS) {
        nc_strlcpy(e->observations[e->obs_count], observation, GM_FIELD_LEN);
        e->obs_count++;
    }

    gm_save(ctx);
    return true;
}

static int guardian_query(gm_ctx *ctx, const char *query, char *out, size_t out_cap) {
    gm_load(ctx);

    char qbuf[256];
    size_t ql = strlen(query);
    if (ql > sizeof(qbuf) - 1) ql = sizeof(qbuf) - 1;
    memcpy(qbuf, query, ql);
    qbuf[ql] = '\0';
    for (char *p = qbuf; *p; p++) *p = tolower((unsigned char)*p);

    int off = 0;
    int count = 0;

    for (int i = 0; i < ctx->entity_count && (size_t)off < out_cap - 128; i++) {
        gm_entity *e = &ctx->entities[i];

        char elower[GM_FIELD_LEN];
        size_t el = strlen(e->name);
        for (size_t j = 0; j < el; j++) elower[j] = tolower((unsigned char)e->name[j]);
        elower[el] = '\0';

        bool matched = (strstr(elower, qbuf) != NULL);
        if (!matched) {
            for (int k = 0; k < e->obs_count; k++) {
                char olower[GM_FIELD_LEN];
                size_t ol = strlen(e->observations[k]);
                for (size_t j = 0; j < ol; j++)
                    olower[j] = tolower((unsigned char)e->observations[k][j]);
                olower[ol] = '\0';
                if (strstr(olower, qbuf)) { matched = true; break; }
            }
        }

        if (matched) {
            off += snprintf(out + off, out_cap - (size_t)off,
                "• %s (%s):\n", e->name, e->type[0] ? e->type : "entity");
            for (int k = 0; k < e->obs_count; k++) {
                off += snprintf(out + off, out_cap - (size_t)off,
                    "  - %s\n", e->observations[k]);
            }
            count++;
        }
    }

    if (count == 0)
        nc_strlcpy(out, "No matching entities.", out_cap);

    return count;
}

static bool guardian_forget(gm_ctx *ctx, const char *name) {
    gm_load(ctx);
    for (int i = 0; i < ctx->entity_count; i++) {
        if (strcasecmp(ctx->entities[i].name, name) == 0) {
            for (int j = i; j < ctx->entity_count - 1; j++)
                ctx->entities[j] = ctx->entities[j + 1];
            ctx->entity_count--;
            gm_save(ctx);
            return true;
        }
    }
    return false;
}

/* ── nc_memory backend implementation ────────────────────────── */

static bool mem_store(nc_memory *self, const char *key, const char *content) {
    gm_ctx *ctx = (gm_ctx *)self->ctx;
    /* Map memory store to guardian entity: key=name, content=observation */
    return guardian_store_entity(ctx, key, "memory", content);
}

static bool mem_recall(nc_memory *self, const char *query, char *out, size_t out_cap) {
    gm_ctx *ctx = (gm_ctx *)self->ctx;
    return guardian_query(ctx, query, out, out_cap) > 0;
}

static bool mem_forget(nc_memory *self, const char *key) {
    gm_ctx *ctx = (gm_ctx *)self->ctx;
    return guardian_forget(ctx, key);
}

static void mem_free(nc_memory *self) {
    gm_ctx *ctx = (gm_ctx *)self->ctx;
    if (ctx) {
        gm_save(ctx);
        free(ctx);
    }
    self->ctx = NULL;
}

/* ── Constructor ─────────────────────────────────────────────── */

nc_memory nc_memory_guardian(const char *path) {
    gm_ctx *ctx = (gm_ctx *)calloc(1, sizeof(gm_ctx));
    if (!ctx) return nc_memory_noop();

    nc_strlcpy(ctx->path, path, sizeof(ctx->path));
    nc_log(NC_LOG_INFO, "Guardian memory: %s", path);

    return (nc_memory){
        .backend_name = "guardian",
        .ctx     = ctx,
        .store   = mem_store,
        .recall  = mem_recall,
        .forget  = mem_forget,
        .free    = mem_free,
    };
}

/* ── Integration helper: synchronize guardian_memory tool with this backend ── */

/* The built-in guardian_memory tool (in mcp_builtin.c) operates on a global
 * static array. To avoid duplication, this function is called at startup to
 * share the same entity array between the memory backend and the tool. */
gm_ctx *gm_get_ctx(nc_memory *mem) {
    if (!mem || !mem->ctx) return NULL;
    return (gm_ctx *)mem->ctx;
}

/* Expose for mcp_builtin.c to use the same persistent backend */
int gm_entity_count(void *ctx) {
    gm_ctx *g = (gm_ctx *)ctx;
    gm_load(g);
    return g->entity_count;
}

gm_entity *gm_entity_at(void *ctx, int idx) {
    gm_ctx *g = (gm_ctx *)ctx;
    gm_load(g);
    if (idx < 0 || idx >= g->entity_count) return NULL;
    return &g->entities[idx];
}

bool gm_store_entity(void *ctx, const char *name, const char *type, const char *obs) {
    return guardian_store_entity((gm_ctx *)ctx, name, type, obs);
}

int gm_query(void *ctx, const char *query, char *out, size_t out_cap) {
    return guardian_query((gm_ctx *)ctx, query, out, out_cap);
}

bool gm_forget_entity(void *ctx, const char *name) {
    return guardian_forget((gm_ctx *)ctx, name);
}

/* ── Noop fallback (used when memory alloc fails) ────────────── */

static bool noop_store(nc_memory *self, const char *key, const char *content) {
    (void)self; (void)key; (void)content;
    return true;
}

static bool noop_recall(nc_memory *self, const char *query, char *out, size_t out_cap) {
    (void)self; (void)query;
    nc_strlcpy(out, "Memory not available.", out_cap);
    return false;
}

static bool noop_forget(nc_memory *self, const char *key) {
    (void)self; (void)key;
    return true;
}

static void noop_free(nc_memory *self) {
    (void)self;
}

nc_memory nc_memory_noop(void) {
    return (nc_memory){
        .backend_name = "noop",
        .ctx = NULL,
        .store  = noop_store,
        .recall = noop_recall,
        .forget = noop_forget,
        .free   = noop_free,
    };
}
