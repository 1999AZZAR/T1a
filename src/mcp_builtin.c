/*
 * mcp_builtin.c — Built-in MCP tools replacing external Node.js MCP servers.
 *
 * Replaces:
 *   - @modelcontextprotocol/server-sequential-thinking → reasoning
 *   - mcp-remote tavily → tavily_search  
 *   - @modelcontextprotocol/server-memory → guardian_memory
 *
 * All pure C, BearSSL only. None need Node.js.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════════
 *  SEQUENTIAL THINKING
 * ══════════════════════════════════════════════════════════════════ */

#define MAX_THOUGHTS 64
#define THOUGHT_LEN 2048

typedef struct {
    int  thought_number, total_thoughts;
    char thought[THOUGHT_LEN];
    bool is_revision;
    int  revises_thought, branch_from_thought;
    char branch_id[64];
    bool needs_more_thoughts;
} thought_entry;

static thought_entry g_thoughts[MAX_THOUGHTS];
static int g_thought_count = 0;

static void reasoning_reset(void) {
    g_thought_count = 0;
    memset(g_thoughts, 0, sizeof(g_thoughts));
}

static bool reasoning_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    nc_arena a;
    nc_arena_init(&a, strlen(args_json) * 2 + 2048);
    nc_json *root = nc_json_parse(&a, args_json, strlen(args_json));
    if (!root || root->type != NC_JSON_OBJECT) {
        nc_strlcpy(out, "{\"error\":\"invalid JSON\"}", out_cap);
        nc_arena_free(&a);
        return false;
    }

    nc_str t = nc_json_str(nc_json_get(root, "thought"), "");
    bool next = nc_json_bool(nc_json_get(root, "nextThoughtNeeded"), true);
    int num = (int)nc_json_num(nc_json_get(root, "thoughtNumber"), g_thought_count + 1);
    int total = (int)nc_json_num(nc_json_get(root, "totalThoughts"), 1);
    bool rev = nc_json_bool(nc_json_get(root, "isRevision"), false);
    int revises = (int)nc_json_num(nc_json_get(root, "revisesThought"), 0);
    int bfrom = (int)nc_json_num(nc_json_get(root, "branchFromThought"), 0);
    nc_str bid = nc_json_str(nc_json_get(root, "branchId"), "");
    bool more = nc_json_bool(nc_json_get(root, "needsMoreThoughts"), false);

    if (g_thought_count < MAX_THOUGHTS && t.len > 0) {
        thought_entry *e = &g_thoughts[g_thought_count++];
        e->thought_number = num;
        e->total_thoughts = total;
        size_t cplen = t.len < THOUGHT_LEN-1 ? t.len : THOUGHT_LEN-1;
        memcpy(e->thought, t.ptr, cplen); e->thought[cplen] = '\0';
        e->is_revision = rev; e->revises_thought = revises;
        e->branch_from_thought = bfrom; e->needs_more_thoughts = more;
        if (bid.len > 0 && bid.len < sizeof(e->branch_id)-1) {
            memcpy(e->branch_id, bid.ptr, bid.len);
            e->branch_id[bid.len] = '\0';
        }
    }

    if (!next || !more) {
        int off = snprintf(out, out_cap, "{\"done\":true,\"n\":%d,\"thoughts\":[", g_thought_count);
        for (int i = 0; i < g_thought_count && (size_t)off < out_cap - 128; i++) {
            char esc[THOUGHT_LEN*2];
            int eo = 0;
            for (const char *p = g_thoughts[i].thought; *p && eo < (int)sizeof(esc)-4; p++) {
                if (*p == '"' || *p == '\\') esc[eo++] = '\\';
                esc[eo++] = *p;
            }
            esc[eo] = '\0';
            off += snprintf(out+off, out_cap-(size_t)off, "%s{\"n\":%d,\"t\":%d,\"text\":\"%s\"}",
                i>0?",":"", g_thoughts[i].thought_number, g_thoughts[i].total_thoughts, esc);
        }
        off += snprintf(out+off, out_cap-(size_t)off, "]}");
        reasoning_reset();
    } else
        snprintf(out, out_cap, "{\"status\":\"ok\",\"thought\":%d}", num);

    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_reasoning(void) {
    return (nc_tool){
        .def = {
            .name = "sequentialthinking",
            .description = "Break down complex problems step by step, one thought at a time.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"thought\":{\"type\":\"string\"},"
                    "\"nextThoughtNeeded\":{\"type\":\"boolean\"},"
                    "\"thoughtNumber\":{\"type\":\"integer\"},"
                    "\"totalThoughts\":{\"type\":\"integer\"},"
                    "\"isRevision\":{\"type\":\"boolean\"},"
                    "\"revisesThought\":{\"type\":\"integer\"},"
                    "\"branchFromThought\":{\"type\":\"integer\"},"
                    "\"branchId\":{\"type\":\"string\"},"
                    "\"needsMoreThoughts\":{\"type\":\"boolean\"}"
                "},"
                "\"required\":[\"thought\"]"
            "}",
        },
        .ctx = NULL, .execute = reasoning_execute, .free = NULL,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  TAVILY SEARCH  (direct HTTPS, no npx)
 * ══════════════════════════════════════════════════════════════════ */

#define TAVILY_URL "https://api.tavily.com/search"

static bool tavily_execute(nc_tool *self, const char *json, char *out, size_t cap) {
    (void)self;
    nc_arena a;
    nc_arena_init(&a, strlen(json)*2+2048);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) { nc_strlcpy(out, "error: invalid args", cap); nc_arena_free(&a); return false; }

    nc_str q = nc_json_str(nc_json_get(root, "query"), "");
    int max = (int)nc_json_num(nc_json_get(root, "max_results"), 5);
    if (max < 1) max = 1; if (max > 10) max = 10;
    if (q.len == 0) { nc_strlcpy(out, "error: missing query", cap); nc_arena_free(&a); return false; }

    char *body = malloc(q.len + 256);
    int blen = snprintf(body, q.len+256,
        "{\"api_key\":\"***\",\"query\":\"%.*s\",\"max_results\":%d,\"include_answer\":true}",
        (int)q.len, q.ptr, max);

    const char *hdr[] = {"Content-Type: application/json"};
    nc_http_response resp = {0};

    if (!nc_http_post(TAVILY_URL, body, (size_t)blen, hdr, 1, &resp)) {
        nc_strlcpy(out, "error: request failed", cap);
        free(body); nc_arena_free(&a); return false;
    }

    bool ok = false;
    if (resp.status == 200) {
        nc_arena pa;
        nc_arena_init(&pa, resp.body_len*2+2048);
        nc_json *res = nc_json_parse(&pa, resp.body, resp.body_len);
        if (res) {
            nc_json *ans = nc_json_get(res, "answer");
            nc_json *arr = nc_json_get(res, "results");
            int off = 0;
            if (ans && ans->type==NC_JSON_STRING && ans->string.len > 0)
                off += snprintf(out+off, cap-(size_t)off, "Answer: %.*s\n\n", (int)ans->string.len, ans->string.ptr);
            if (arr && arr->type==NC_JSON_ARRAY) {
                for (int i = 0; i < arr->array.count && (size_t)off < cap-128; i++) {
                    nc_json *it = &arr->array.items[i];
                    nc_str tl = nc_json_str(nc_json_get(it,"title"),"");
                    nc_str u = nc_json_str(nc_json_get(it,"url"),"");
                    nc_str c = nc_json_str(nc_json_get(it,"content"),"");
                    off += snprintf(out+off, cap-(size_t)off, "%d. %.*s\n   %.*s\n\n", i+1, (int)tl.len,tl.ptr, (int)c.len,c.ptr);
                }
            }
            if (off == 0) nc_strlcpy(out, "No results.", cap);
            ok = true;
        }
        nc_arena_free(&pa);
    }
    nc_http_response_free(&resp);
    free(body); nc_arena_free(&a);
    return ok;
}

nc_tool nc_tool_tavily_search(void) {
    return (nc_tool){
        .def = {
            .name = "tavily_search",
            .description = "Web search via Tavily AI. Params: query (req), max_results (1-10).",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"query\":{\"type\":\"string\"},"
                    "\"max_results\":{\"type\":\"integer\"}"
                "},"
                "\"required\":[\"query\"]"
            "}",
        },
        .ctx = NULL, .execute = tavily_execute, .free = NULL,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  GUARDIAN MEMORY — shared persistent backend from memory.c
 *
 *  Uses memory.c's gm_store_entity/gm_query/gm_forget_entity API.
 *  All data persists to JSONL file. Tool .ctx = gm_ctx pointer.
 * ══════════════════════════════════════════════════════════════════ */

static bool guardian_execute(nc_tool *self, const char *json, char *out, size_t cap) {
    void *ctx = self ? self->ctx : NULL;
    if (!ctx) { nc_strlcpy(out, "error: memory not initialized", cap); return false; }

    nc_arena a;
    nc_arena_init(&a, strlen(json)*2+4096);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) { nc_strlcpy(out, "error: invalid JSON", cap); nc_arena_free(&a); return false; }

    nc_str op = nc_json_str(nc_json_get(root, "operation"), "");

    if (nc_str_eql(op, "store") || op.len == 0) {
        nc_str name = nc_json_str(nc_json_get(root, "name"), "");
        nc_str type = nc_json_str(nc_json_get(root, "type"), "");
        nc_str obs  = nc_json_str(nc_json_get(root, "observation"), "");
        if (name.len == 0) { nc_strlcpy(out, "error: missing name", cap); nc_arena_free(&a); return false; }
        char nbuf[256], tbuf[64], obuf[1024];
        size_t nl = name.len < sizeof(nbuf)-1 ? name.len : sizeof(nbuf)-1;
        memcpy(nbuf, name.ptr, nl); nbuf[nl] = '\0';
        size_t tl = type.len < sizeof(tbuf)-1 ? type.len : sizeof(tbuf)-1;
        memcpy(tbuf, type.ptr, tl); tbuf[tl] = '\0';
        size_t ol = obs.len < sizeof(obuf)-1 ? obs.len : sizeof(obuf)-1;
        memcpy(obuf, obs.ptr, ol); obuf[ol] = '\0';
        snprintf(out, cap, gm_store_entity(ctx, nbuf, tbuf, obuf) ? "Stored: %s" : "error: store failed", nbuf);

    } else if (nc_str_eql(op, "query")) {
        nc_str query = nc_json_str(nc_json_get(root, "query"), "");
        if (query.len == 0) { nc_strlcpy(out, "error: missing query", cap); nc_arena_free(&a); return false; }
        char qbuf[256];
        size_t ql = query.len < sizeof(qbuf)-1 ? query.len : sizeof(qbuf)-1;
        memcpy(qbuf, query.ptr, ql); qbuf[ql] = '\0';
        gm_query(ctx, qbuf, out, cap);

    } else if (nc_str_eql(op, "forget")) {
        nc_str name = nc_json_str(nc_json_get(root, "name"), "");
        if (name.len == 0) { nc_strlcpy(out, "error: missing name", cap); nc_arena_free(&a); return false; }
        char nbuf[256];
        size_t nl = name.len < sizeof(nbuf)-1 ? name.len : sizeof(nbuf)-1;
        memcpy(nbuf, name.ptr, nl); nbuf[nl] = '\0';
        snprintf(out, cap, gm_forget_entity(ctx, nbuf) ? "Forgot: %s" : "Not found: %s", nbuf);

    } else if (nc_str_eql(op, "list")) {
        int off = snprintf(out, cap, "Entities: %d\n", gm_entity_count(ctx));
        for (int i = 0; (size_t)off < cap-128; i++) {
            struct gm_entity *e = gm_entity_at(ctx, i);
            if (!e) break;
            off += snprintf(out+off, cap-(size_t)off, "%d. %s (%s) — %d obs\n",
                i+1, e->name, e->type[0]?e->type:"entity", e->obs_count);
        }
    } else {
        snprintf(out, cap, "error: unknown op '%.*s'", (int)op.len, op.ptr);
    }

    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_guardian_memory(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "guardian_memory",
            .description = "Persistent entity memory. Ops: store (name+obs), query (keyword), forget, list.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"operation\":{\"type\":\"string\"},"
                    "\"name\":{\"type\":\"string\"},"
                    "\"type\":{\"type\":\"string\"},"
                    "\"observation\":{\"type\":\"string\"},"
                    "\"query\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"operation\"]"
            "}",
        },
        .ctx = mem_ctx,
        .execute = guardian_execute,
        .free = NULL,
    };
}
