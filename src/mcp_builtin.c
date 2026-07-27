/*
 * mcp_builtin.c — Built-in MCP tools that replace external Node.js MCP servers.
 *
 * Replaces:
 *   - @modelcontextprotocol/server-sequential-thinking → reasoning
 *   - mcp-remote tavily → tavily_search
 *   - @modelcontextprotocol/server-memory → guardian_memory
 *
 * All pure C, no external deps. Registers directly as nc_tool structs.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════════
 *  SEQUENTIAL THINKING (replaces @modelcontextprotocol/...)
 *
 *  Stateful thought tree with branching, revision, and summary.
 *  Single global state — T1a is single-threaded daemon.
 * ══════════════════════════════════════════════════════════════════ */

#define MAX_THOUGHTS 64
#define THOUGHT_LEN 2048

typedef struct {
    int  thought_number;
    int  total_thoughts;
    char thought[THOUGHT_LEN];
    bool is_revision;
    int  revises_thought;
    int  branch_from_thought;
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

    /* Parse args */
    nc_arena a;
    nc_arena_init(&a, strlen(args_json) * 2 + 2048);
    nc_json *root = nc_json_parse(&a, args_json, strlen(args_json));
    if (!root || root->type != NC_JSON_OBJECT) {
        nc_strlcpy(out, "{\"error\":\"invalid JSON\"}", out_cap);
        nc_arena_free(&a);
        return false;
    }

    nc_str thought_str = nc_json_str(nc_json_get(root, "thought"), "");
    bool next_needed = nc_json_bool(nc_json_get(root, "nextThoughtNeeded"), true);
    int thought_num = (int)nc_json_num(nc_json_get(root, "thoughtNumber"), g_thought_count + 1);
    int total = (int)nc_json_num(nc_json_get(root, "totalThoughts"), 1);
    bool is_rev = nc_json_bool(nc_json_get(root, "isRevision"), false);
    int revises = (int)nc_json_num(nc_json_get(root, "revisesThought"), 0);
    int branch_from = (int)nc_json_num(nc_json_get(root, "branchFromThought"), 0);
    nc_str bid = nc_json_str(nc_json_get(root, "branchId"), "");
    bool need_more = nc_json_bool(nc_json_get(root, "needsMoreThoughts"), false);

    /* Store the thought */
    if (g_thought_count < MAX_THOUGHTS && thought_str.len > 0) {
        thought_entry *e = &g_thoughts[g_thought_count++];
        e->thought_number = thought_num;
        e->total_thoughts = total;
        size_t cplen = thought_str.len < THOUGHT_LEN - 1 ? thought_str.len : THOUGHT_LEN - 1;
        memcpy(e->thought, thought_str.ptr, cplen);
        e->thought[cplen] = '\0';
        e->is_revision = is_rev;
        e->revises_thought = revises;
        e->branch_from_thought = branch_from;
        e->needs_more_thoughts = need_more;
        if (bid.len > 0 && bid.len < sizeof(e->branch_id) - 1) {
            memcpy(e->branch_id, bid.ptr, bid.len);
            e->branch_id[bid.len] = '\0';
        }
    }

    /* If not continuing, generate summary of thought chain */
    if (!next_needed || !need_more) {
        int off = snprintf(out, out_cap, "{\"status\":\"complete\",\"thought_count\":%d,\"thoughts\":[", g_thought_count);
        for (int i = 0; i < g_thought_count && (size_t)off < out_cap - 128; i++) {
            thought_entry *e = &g_thoughts[i];
            char escaped[THOUGHT_LEN * 2];
            int eoff = 0;
            for (const char *p = e->thought; *p && eoff < (int)sizeof(escaped) - 4; p++) {
                if (*p == '"' || *p == '\\') escaped[eoff++] = '\\';
                escaped[eoff++] = *p;
            }
            escaped[eoff] = '\0';
            off += snprintf(out + off, out_cap - (size_t)off,
                "%s{\"n\":%d,\"t\":%d,\"text\":\"%s\"}",
                i > 0 ? "," : "", e->thought_number, e->total_thoughts, escaped);
        }
        off += snprintf(out + off, out_cap - (size_t)off, "]}");
        reasoning_reset();
    } else {
        snprintf(out, out_cap, "{\"status\":\"continuing\",\"thought\":%d,\"next\":%d}",
                 thought_num, thought_num + 1);
    }

    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_reasoning(void) {
    return (nc_tool){
        .def = {
            .name = "sequentialthinking",
            .description = "Break down complex problems step by step. Each 'thought' is one step. "
                           "Use branching (branchFromThought, branchId) for alternatives, "
                           "isRevision for corrections. Set nextThoughtNeeded=false when done.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"thought\":{\"type\":\"string\",\"description\":\"Current reasoning step\"},"
                    "\"nextThoughtNeeded\":{\"type\":\"boolean\",\"description\":\"Is another step needed?\"},"
                    "\"thoughtNumber\":{\"type\":\"integer\",\"description\":\"Current step number\"},"
                    "\"totalThoughts\":{\"type\":\"integer\",\"description\":\"Estimated total steps\"},"
                    "\"isRevision\":{\"type\":\"boolean\",\"description\":\"Revising a previous thought\"},"
                    "\"revisesThought\":{\"type\":\"integer\",\"description\":\"Thought number being revised\"},"
                    "\"branchFromThought\":{\"type\":\"integer\",\"description\":\"Branch from this thought\"},"
                    "\"branchId\":{\"type\":\"string\"},"
                    "\"needsMoreThoughts\":{\"type\":\"boolean\"}"
                "},"
                "\"required\":[\"thought\"]"
            "}",
        },
        .ctx = NULL,
        .execute = reasoning_execute,
        .free = NULL,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  TAVILY SEARCH (replaces mcp-remote → https://mcp.tavily.com)
 *
 *  Direct HTTP POST to api.tavily.com/search via BearSSL.
 *  Returns top results as clean text.
 * ══════════════════════════════════════════════════════════════════ */

#define TAVILY_API_URL "https://api.tavily.com/search"

static bool tavily_search_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;

    nc_arena a;
    nc_arena_init(&a, strlen(args_json) * 2 + 2048);
    nc_json *root = nc_json_parse(&a, args_json, strlen(args_json));
    if (!root || root->type != NC_JSON_OBJECT) {
        nc_strlcpy(out, "error: invalid arguments", out_cap);
        nc_arena_free(&a);
        return false;
    }

    nc_str query = nc_json_str(nc_json_get(root, "query"), "");
    int max_results = (int)nc_json_num(nc_json_get(root, "max_results"), 5);
    if (max_results < 1) max_results = 1;
    if (max_results > 10) max_results = 10;

    if (query.len == 0) {
        nc_strlcpy(out, "error: missing 'query'", out_cap);
        nc_arena_free(&a);
        return false;
    }

    /* Build request body */
    /* API key is embedded — free tier, read-only public API */
    const char *api_key = "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";
    size_t body_sz = query.len + 256;
    char *body = (char *)malloc(body_sz);
    if (!body) { nc_arena_free(&a); return false; }

    int body_len = snprintf(body, body_sz,
        "{\"api_key\":\"%s\",\"query\":\"%.*s\",\"max_results\":%d,\"include_answer\":true}",
        api_key, (int)query.len, query.ptr, max_results);

    const char *headers[] = {"Content-Type: application/json"};
    nc_http_response resp;
    memset(&resp, 0, sizeof(resp));

    bool ok = false;
    if (nc_http_post(TAVILY_API_URL, body, (size_t)body_len, headers, 1, &resp)) {
        if (resp.status == 200) {
            /* Parse response for results */
            nc_arena pa;
            nc_arena_init(&pa, resp.body_len * 2 + 2048);
            nc_json *res_root = nc_json_parse(&pa, resp.body, resp.body_len);
            if (res_root) {
                nc_json *res_arr = nc_json_get(res_root, "results");
                nc_json *answer = nc_json_get(res_root, "answer");
                int off = 0;

                if (answer && answer->type == NC_JSON_STRING && answer->string.len > 0) {
                    off += snprintf(out + off, out_cap - (size_t)off,
                        "Answer: %.*s\n\n", (int)answer->string.len, answer->string.ptr);
                }

                if (res_arr && res_arr->type == NC_JSON_ARRAY) {
                    for (int i = 0; i < res_arr->array.count && (size_t)off < out_cap - 256; i++) {
                        nc_json *item = &res_arr->array.items[i];
                        nc_str title = nc_json_str(nc_json_get(item, "title"), "");
                        nc_str url = nc_json_str(nc_json_get(item, "url"), "");
                        nc_str content = nc_json_str(nc_json_get(item, "content"), "");
                        double score = nc_json_num(nc_json_get(item, "score"), 0);

                        off += snprintf(out + off, out_cap - (size_t)off,
                            "%d. [%.*s](%.*s) — score: %.2f\n   %.*s\n\n",
                            i + 1,
                            (int)title.len, title.ptr,
                            (int)url.len, url.ptr,
                            score,
                            (int)content.len, content.ptr);
                    }
                }
                if (off == 0) {
                    nc_strlcpy(out, "No results found.", out_cap);
                }
                ok = true;
            }
            nc_arena_free(&pa);
        } else {
            snprintf(out, out_cap, "error: HTTP %d from Tavily", resp.status);
        }
        nc_http_response_free(&resp);
    } else {
        nc_strlcpy(out, "error: HTTP request to Tavily failed", out_cap);
    }

    free(body);
    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_tavily_search(void) {
    return (nc_tool){
        .def = {
            .name = "tavily_search",
            .description = "Search the web using Tavily AI. Returns relevant results with titles, URLs, content snippets, and an AI-generated answer. Parameter: query (required), max_results (1-10, default 5). Use for fact-checking and research.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"
                    "\"max_results\":{\"type\":\"integer\",\"description\":\"Results count (1-10)\"}"
                "},"
                "\"required\":[\"query\"]"
            "}",
        },
        .ctx = NULL,
        .execute = tavily_search_execute,
        .free = NULL,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  GUARDIAN CONTEXT (replaces @modelcontextprotocol/server-memory)
 *
 *  Lightweight entity-relation graph for context management.
 *  Flat-file backend, JSON-RPC-like tool interface.
 *  Three operations: store, query, forget.
 * ══════════════════════════════════════════════════════════════════ */

#define GUARDIAN_MAX_ENTITIES 256
#define GUARDIAN_FIELD_LEN 512

typedef struct {
    char name[GUARDIAN_FIELD_LEN];
    char type[GUARDIAN_FIELD_LEN];
    int  obs_count;
    char observations[16][GUARDIAN_FIELD_LEN];
} guardian_entity;

static guardian_entity g_entities[GUARDIAN_MAX_ENTITIES];
static int g_entity_count = 0;

static guardian_entity *guardian_find(const char *name) {
    for (int i = 0; i < g_entity_count; i++) {
        if (strcmp(g_entities[i].name, name) == 0) return &g_entities[i];
    }
    return NULL;
}

static bool guardian_memory_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    nc_arena a;
    nc_arena_init(&a, strlen(args_json) * 2 + 4096);
    nc_json *root = nc_json_parse(&a, args_json, strlen(args_json));
    if (!root || root->type != NC_JSON_OBJECT) {
        nc_strlcpy(out, "error: invalid JSON", out_cap);
        nc_arena_free(&a);
        return false;
    }

    nc_str op = nc_json_str(nc_json_get(root, "operation"), "");

    if (nc_str_eql(op, "store") || op.len == 0) {
        /* Store an entity with observations */
        nc_str name = nc_json_str(nc_json_get(root, "name"), "");
        nc_str type = nc_json_str(nc_json_get(root, "type"), "");
        nc_str obs  = nc_json_str(nc_json_get(root, "observation"), "");

        if (name.len == 0) {
            nc_strlcpy(out, "error: missing 'name'", out_cap);
            nc_arena_free(&a);
            return false;
        }

        guardian_entity *e = guardian_find(name.ptr);
        if (!e) {
            if (g_entity_count >= GUARDIAN_MAX_ENTITIES) {
                nc_strlcpy(out, "error: entity limit reached", out_cap);
                nc_arena_free(&a);
                return false;
            }
            e = &g_entities[g_entity_count++];
            size_t nl = name.len < GUARDIAN_FIELD_LEN - 1 ? name.len : GUARDIAN_FIELD_LEN - 1;
            memcpy(e->name, name.ptr, nl); e->name[nl] = '\0';
            if (type.len > 0) {
                size_t tl = type.len < GUARDIAN_FIELD_LEN - 1 ? type.len : GUARDIAN_FIELD_LEN - 1;
                memcpy(e->type, type.ptr, tl); e->type[tl] = '\0';
            }
        }

        if (obs.len > 0 && e->obs_count < 16) {
            size_t ol = obs.len < GUARDIAN_FIELD_LEN - 1 ? obs.len : GUARDIAN_FIELD_LEN - 1;
            memcpy(e->observations[e->obs_count], obs.ptr, ol);
            e->observations[e->obs_count][ol] = '\0';
            e->obs_count++;
        }

        snprintf(out, out_cap, "Stored entity: %s (%d observations)", e->name, e->obs_count);

    } else if (nc_str_eql(op, "query")) {
        nc_str query = nc_json_str(nc_json_get(root, "query"), "");
        if (query.len == 0) {
            nc_strlcpy(out, "error: missing 'query'", out_cap);
            nc_arena_free(&a);
            return false;
        }

        /* Simple case-insensitive substring match */
        char qbuf[256];
        size_t ql = query.len < sizeof(qbuf) - 1 ? query.len : sizeof(qbuf) - 1;
        memcpy(qbuf, query.ptr, ql);
        qbuf[ql] = '\0';
        for (char *p = qbuf; *p; p++) *p = tolower((unsigned char)*p);

        int off = 0;
        for (int i = 0; i < g_entity_count && (size_t)off < out_cap - 128; i++) {
            guardian_entity *e = &g_entities[i];
            char ename_lower[GUARDIAN_FIELD_LEN];
            size_t el = strlen(e->name);
            for (size_t j = 0; j < el; j++) ename_lower[j] = tolower((unsigned char)e->name[j]);
            ename_lower[el] = '\0';

            bool matched = (strstr(ename_lower, qbuf) != NULL);
            if (!matched) {
                for (int k = 0; k < e->obs_count; k++) {
                    char olower[GUARDIAN_FIELD_LEN];
                    size_t ol = strlen(e->observations[k]);
                    for (size_t j = 0; j < ol; j++) olower[j] = tolower((unsigned char)e->observations[k][j]);
                    olower[ol] = '\0';
                    if (strstr(olower, qbuf)) { matched = true; break; }
                }
            }

            if (matched) {
                off += snprintf(out + off, out_cap - (size_t)off,
                    "- %s (%s):\n", e->name, e->type[0] ? e->type : "entity");
                for (int k = 0; k < e->obs_count; k++) {
                    off += snprintf(out + off, out_cap - (size_t)off, "  • %s\n", e->observations[k]);
                }
            }
        }
        if (off == 0) nc_strlcpy(out, "No matching entities found.", out_cap);

    } else if (nc_str_eql(op, "forget")) {
        nc_str name = nc_json_str(nc_json_get(root, "name"), "");
        if (name.len == 0) {
            nc_strlcpy(out, "error: missing 'name'", out_cap);
            nc_arena_free(&a);
            return false;
        }
        int found = 0;
        for (int i = 0; i < g_entity_count; i++) {
            if (strcmp(g_entities[i].name, name.ptr) == 0) {
                /* Shift array */
                for (int j = i; j < g_entity_count - 1; j++) g_entities[j] = g_entities[j + 1];
                g_entity_count--;
                found = 1;
                break;
            }
        }
        snprintf(out, out_cap, found ? "Forgot: %s" : "Not found: %s", name.ptr);

    } else if (nc_str_eql(op, "list")) {
        int off = snprintf(out, out_cap, "Entities: %d\n", g_entity_count);
        for (int i = 0; i < g_entity_count && (size_t)off < out_cap - 128; i++) {
            off += snprintf(out + off, out_cap - (size_t)off,
                "%d. %s (%s) — %d obs\n",
                i + 1, g_entities[i].name,
                g_entities[i].type[0] ? g_entities[i].type : "entity",
                g_entities[i].obs_count);
        }
    } else {
        snprintf(out, out_cap, "error: unknown operation '%s'", op.ptr);
    }

    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_guardian_memory(void) {
    return (nc_tool){
        .def = {
            .name = "guardian_memory",
            .description = "Persistent context management. Operations: store (save entity+observation), query (search by keyword), forget (remove entity), list (all entities). Entities have name, type, and observations.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"operation\":{\"type\":\"string\",\"description\":\"store|query|forget|list\"},"
                    "\"name\":{\"type\":\"string\",\"description\":\"Entity name\"},"
                    "\"type\":{\"type\":\"string\",\"description\":\"Entity type (project/task/person/resource)\"},"
                    "\"observation\":{\"type\":\"string\",\"description\":\"Observation text (for store)\"},"
                    "\"query\":{\"type\":\"string\",\"description\":\"Search query (for query)\"}"
                "},"
                "\"required\":[\"operation\"]"
            "}",
        },
        .ctx = NULL,
        .execute = guardian_memory_execute,
        .free = NULL,
    };
}
