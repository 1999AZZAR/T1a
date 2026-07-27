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
 *  WIKIPEDIA SEARCH — free, no API key, clean encyclopedia content
 *
 *  Uses Wikipedia Action API.
 *  Returns top 5 article titles + snippets + URLs.
 * ══════════════════════════════════════════════════════════════════ */

#define WIKI_API_URL "https://en.wikipedia.org/w/api.php"

static void wiki_strip_html(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (const char *p = in; *p && j < cap-1; p++) {
        if (*p == '<') { while (*p && *p != '>') p++; if (!*p) break; }
        else if (*p == '&') {
            if      (strncmp(p, "&amp;", 5) == 0)  { out[j++]='&'; p+=4; }
            else if (strncmp(p, "&lt;", 4) == 0)   { out[j++]='<'; p+=3; }
            else if (strncmp(p, "&gt;", 4) == 0)   { out[j++]='>'; p+=3; }
            else if (strncmp(p, "&quot;", 6) == 0)  { out[j++]='"'; p+=5; }
            else if (strncmp(p, "&#039;", 6) == 0) { out[j++]='\''; p+=5; }
            else out[j++] = *p;
        } else {
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

static bool wiki_execute(nc_tool *self, const char *json, char *out, size_t cap) {
    (void)self;
    nc_arena a;
    nc_arena_init(&a, strlen(json)*2+2048);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) { nc_strlcpy(out, "error: invalid args", cap); nc_arena_free(&a); return false; }

    nc_str q = nc_json_str(nc_json_get(root, "query"), "");
    if (q.len == 0) { nc_strlcpy(out, "error: missing query", cap); nc_arena_free(&a); return false; }

    /* Build URL with percent-encoded query */
    char query_enc[512];
    size_t qe = 0;
    for (size_t i = 0; i < q.len && qe < sizeof(query_enc)-4; i++) {
        char c = q.ptr[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')
            { if (c == ' ') query_enc[qe++]='+'; else query_enc[qe++]=c; }
        else { qe += snprintf(query_enc+qe, sizeof(query_enc)-qe, "%%%02X", (unsigned char)c); }
    }
    query_enc[qe] = '\0';

    size_t url_sz = 1024 + qe;
    char *url = malloc(url_sz);
    snprintf(url, url_sz, "%s?action=query&list=search&srsearch=%s&format=json&srlimit=5&srprop=snippet|wordcount|timestamp",
             WIKI_API_URL, query_enc);

    nc_http_response resp = {0};
    bool ok = false;

    if (nc_http_get(url, NULL, 0, &resp)) {
        if (resp.status == 200) {
            nc_arena pa;
            nc_arena_init(&pa, resp.body_len*2+4096);
            nc_json *res = nc_json_parse(&pa, resp.body, resp.body_len);
            if (res) {
                nc_json *query_node = nc_json_get(res, "query");
                nc_json *search = query_node ? nc_json_get(query_node, "search") : NULL;
                int off = 0;

                if (search && search->type == NC_JSON_ARRAY) {
                    for (int i = 0; i < search->array.count && (size_t)off < cap-256; i++) {
                        nc_json *item = &search->array.items[i];
                        nc_str title = nc_json_str(nc_json_get(item, "title"), "");
                        nc_str snippet = nc_json_str(nc_json_get(item, "snippet"), "");
                        int pageid = (int)nc_json_num(nc_json_get(item, "pageid"), 0);

                        /* Strip HTML tags and entities from snippet */
                        char clean[1024];
                        wiki_strip_html(snippet.ptr, clean, sizeof(clean));

                        off += snprintf(out+off, cap-(size_t)off,
                            "%d. %s\n   https://en.wikipedia.org/?curid=%d\n   %s\n\n",
                            i+1, title.ptr, pageid, clean);
                    }
                }
                if (off == 0) nc_strlcpy(out, "No Wikipedia results found.", cap);
                ok = true;
            }
            nc_arena_free(&pa);
        } else {
            snprintf(out, cap, "error: HTTP %d from Wikipedia", resp.status);
        }
        nc_http_response_free(&resp);
    } else {
        nc_strlcpy(out, "error: HTTP request to Wikipedia failed", cap);
    }

    free(url);
    nc_arena_free(&a);
    return ok;
}

nc_tool nc_tool_wikipedia_search(void) {
    return (nc_tool){
        .def = {
            .name = "wikipedia_search",
            .description = "Search Wikipedia for encyclopedia articles on any topic. Returns top 5 results with titles, URLs, and summaries. Free, no API key needed.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"query\":{\"type\":\"string\",\"description\":\"Search query\"}"
                "},"
                "\"required\":[\"query\"]"
            "}",
        },
        .ctx = NULL, .execute = wiki_execute, .free = NULL,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  GUARDIAN MEMORY — pure C, self-contained, no external deps
 *
 *  Entity-relation graph with static arrays and keyword search.
 *  Self-contained like sequentialthinking — no dependency on memory.c.
 * ══════════════════════════════════════════════════════════════════ */

#define G_ENTS 256
#define G_OBS  16
#define G_LEN  512

typedef struct { char n[G_LEN], t[G_LEN], o[G_OBS][G_LEN]; int oc; } g_ent;
static g_ent g_ents[G_ENTS];
static int g_ec = 0;

static int g_find(const char *name) {
    for (int i = 0; i < g_ec; i++)
        if (strcmp(g_ents[i].n, name) == 0) return i;
    return -1;
}

static bool guardian_execute(nc_tool *self, const char *json, char *out, size_t cap) {
    (void)self;
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

        int idx = g_find(name.ptr);
        if (idx < 0) {
            if (g_ec >= G_ENTS) { nc_strlcpy(out, "error: limit reached", cap); nc_arena_free(&a); return false; }
            idx = g_ec++;
            size_t nl = name.len < G_LEN-1 ? name.len : G_LEN-1;
            memcpy(g_ents[idx].n, name.ptr, nl); g_ents[idx].n[nl] = '\0';
            g_ents[idx].t[0] = '\0';
        }
        if (type.len > 0) {
            size_t tl = type.len < G_LEN-1 ? type.len : G_LEN-1;
            memcpy(g_ents[idx].t, type.ptr, tl); g_ents[idx].t[tl] = '\0';
        }
        if (obs.len > 0 && g_ents[idx].oc < G_OBS) {
            size_t ol = obs.len < G_LEN-1 ? obs.len : G_LEN-1;
            memcpy(g_ents[idx].o[g_ents[idx].oc], obs.ptr, ol);
            g_ents[idx].o[g_ents[idx].oc][ol] = '\0';
            g_ents[idx].oc++;
        }
        snprintf(out, cap, "Stored: %s (%d obs)", g_ents[idx].n, g_ents[idx].oc);

    } else if (nc_str_eql(op, "query")) {
        nc_str query = nc_json_str(nc_json_get(root, "query"), "");
        if (query.len == 0) { nc_strlcpy(out, "error: missing query", cap); nc_arena_free(&a); return false; }
        char q[256];
        size_t ql = query.len < sizeof(q)-1 ? query.len : sizeof(q)-1;
        memcpy(q, query.ptr, ql); q[ql] = '\0';
        for (char *p = q; *p; p++) *p = tolower((unsigned char)*p);

        int off = 0;
        for (int i = 0; i < g_ec && (size_t)off < cap-128; i++) {
            char nl[G_LEN]; size_t nl_len = strlen(g_ents[i].n);
            for (size_t j = 0; j < nl_len; j++) nl[j] = tolower((unsigned char)g_ents[i].n[j]);
            nl[nl_len] = '\0';

            bool match = (strstr(nl, q) != NULL);
            if (!match)
                for (int k = 0; k < g_ents[i].oc; k++) {
                    char ol[G_LEN]; size_t ol_len = strlen(g_ents[i].o[k]);
                    for (size_t j = 0; j < ol_len; j++) ol[j] = tolower((unsigned char)g_ents[i].o[k][j]);
                    ol[ol_len] = '\0';
                    if (strstr(ol, q)) { match = true; break; }
                }

            if (match) {
                off += snprintf(out+off, cap-(size_t)off, "-%s (%s):\n",
                    g_ents[i].n, g_ents[i].t[0]?g_ents[i].t:"entity");
                for (int k = 0; k < g_ents[i].oc; k++)
                    off += snprintf(out+off, cap-(size_t)off, " - %s\n", g_ents[i].o[k]);
            }
        }
        if (off == 0) nc_strlcpy(out, "No match.", cap);

    } else if (nc_str_eql(op, "forget")) {
        nc_str name = nc_json_str(nc_json_get(root, "name"), "");
        if (name.len == 0) { nc_strlcpy(out, "error: missing name", cap); nc_arena_free(&a); return false; }
        int idx = g_find(name.ptr);
        if (idx >= 0) {
            for (int j = idx; j < g_ec-1; j++) g_ents[j] = g_ents[j+1];
            g_ec--;
            snprintf(out, cap, "Forgot: %s", name.ptr);
        } else snprintf(out, cap, "Not found: %s", name.ptr);

    } else if (nc_str_eql(op, "list")) {
        int off = snprintf(out, cap, "Entities: %d\n", g_ec);
        for (int i = 0; i < g_ec && (size_t)off < cap-128; i++)
            off += snprintf(out+off, cap-(size_t)off, "%d. %s (%s) — %d obs\n",
                i+1, g_ents[i].n, g_ents[i].t[0]?g_ents[i].t:"entity", g_ents[i].oc);
    } else {
        nc_str op_s = nc_json_str(nc_json_get(root, "operation"), "");
        snprintf(out, cap, "error: unknown op '%.*s'", (int)op_s.len, op_s.ptr);
    }

    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_guardian_memory(void) {
    return (nc_tool){
        .def = {
            .name = "guardian_memory",
            .description = "Entity-relation memory. Ops: store (name+obs), query (keyword), forget, list.",
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
        .ctx = NULL, .execute = guardian_execute, .free = NULL,
    };
}
