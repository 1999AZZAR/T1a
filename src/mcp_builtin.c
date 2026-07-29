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

    if (!next) {
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

static bool json_string_copy(char *out, size_t cap, const char *src, size_t len) {
    size_t off = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (off + 2 >= cap) return false;
            out[off++] = '\\';
            out[off++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (off + 2 >= cap) return false;
            out[off++] = '\\';
            out[off++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
        } else if (c >= 0x20) {
            if (off + 1 >= cap) return false;
            out[off++] = (char)c;
        }
    }
    out[off] = '\0';
    return true;
}

static bool tavily_execute(nc_tool *self, const char *json, char *out, size_t cap) {
    const char *api_key = self ? (const char *)self->ctx : NULL;
    if (!api_key || !api_key[0]) {
        nc_strlcpy(out, "error: TAVILY_API_KEY not set. Use wikipedia_search for general knowledge.", cap);
        return true;
    }
    nc_arena a;
    nc_arena_init(&a, strlen(json)*2+2048);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) { nc_strlcpy(out, "error: invalid args", cap); nc_arena_free(&a); return false; }

    nc_str q = nc_json_str(nc_json_get(root, "query"), "");
    int max = (int)nc_json_num(nc_json_get(root, "max_results"), 5);
    if (max < 1) max = 1;
    if (max > 10) max = 10;
    if (q.len == 0) { nc_strlcpy(out, "error: missing query", cap); nc_arena_free(&a); return false; }

    size_t escaped_cap = q.len * 2 + 1;
    size_t key_cap = strlen(api_key) * 2 + 1;
    char *escaped_query = malloc(escaped_cap);
    char *escaped_key = malloc(key_cap);
    if (!escaped_query || !escaped_key ||
        !json_string_copy(escaped_query, escaped_cap, q.ptr, q.len) ||
        !json_string_copy(escaped_key, key_cap, api_key, strlen(api_key))) {
        free(escaped_query);
        free(escaped_key);
        nc_arena_free(&a);
        return false;
    }

    size_t body_cap = strlen(escaped_query) + strlen(escaped_key) + 256;
    char *body = malloc(body_cap);
    if (!body) { free(escaped_query); free(escaped_key); nc_arena_free(&a); return false; }
    int blen = snprintf(body, body_cap,
        "{\"api_key\":\"%s\",\"query\":\"%s\",\"max_results\":%d,\"include_answer\":true}",
        escaped_key, escaped_query, max);
    free(escaped_query);
    free(escaped_key);

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
                    off += snprintf(out+off, cap-(size_t)off, "%d. %.*s\n   %.*s\n   %.*s\n\n",
                        i+1, (int)tl.len, tl.ptr, (int)u.len, u.ptr, (int)c.len, c.ptr);
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

nc_tool nc_tool_tavily_search(const char *api_key) {
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
        .ctx = (void *)api_key, .execute = tavily_execute, .free = NULL,
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
                            "%d. %.*s\n   https://en.wikipedia.org/?curid=%d\n   %s\n\n",
                            i+1, (int)title.len, title.ptr, pageid, clean);
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
 *  GUARDIAN MEMORY — shared persistent backend from memory.c
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

        char nbuf[GM_FIELD_LEN], tbuf[GM_FIELD_LEN], obuf[GM_FIELD_LEN];
        size_t nl = name.len < sizeof(nbuf)-1 ? name.len : sizeof(nbuf)-1;
        size_t tl = type.len < sizeof(tbuf)-1 ? type.len : sizeof(tbuf)-1;
        size_t ol = obs.len < sizeof(obuf)-1 ? obs.len : sizeof(obuf)-1;
        memcpy(nbuf, name.ptr, nl); nbuf[nl] = '\0';
        memcpy(tbuf, type.ptr, tl); tbuf[tl] = '\0';
        memcpy(obuf, obs.ptr, ol); obuf[ol] = '\0';
        bool stored = gm_store_entity(ctx, nbuf, tbuf, obuf);
        snprintf(out, cap, stored ? "Stored: %s" : "error: store failed", nbuf);
        if (!stored) { nc_arena_free(&a); return false; }

    } else if (nc_str_eql(op, "query")) {
        nc_str query = nc_json_str(nc_json_get(root, "query"), "");
        if (query.len == 0) { nc_strlcpy(out, "error: missing query", cap); nc_arena_free(&a); return false; }
        char q[256];
        size_t ql = query.len < sizeof(q)-1 ? query.len : sizeof(q)-1;
        memcpy(q, query.ptr, ql); q[ql] = '\0';
        gm_query(ctx, q, out, cap);

    } else if (nc_str_eql(op, "forget")) {
        nc_str name = nc_json_str(nc_json_get(root, "name"), "");
        if (name.len == 0) { nc_strlcpy(out, "error: missing name", cap); nc_arena_free(&a); return false; }
        char nbuf[GM_FIELD_LEN];
        size_t nl = name.len < sizeof(nbuf)-1 ? name.len : sizeof(nbuf)-1;
        memcpy(nbuf, name.ptr, nl); nbuf[nl] = '\0';
        snprintf(out, cap, gm_forget_entity(ctx, nbuf) ? "Forgot: %s" : "Not found: %s", nbuf);

    } else if (nc_str_eql(op, "list")) {
        int count = gm_entity_count(ctx);
        int off = snprintf(out, cap, "Entities: %d\n", count);
        for (int i = 0; i < count && (size_t)off < cap-128; i++) {
            gm_entity *e = gm_entity_at(ctx, i);
            if (!e) break;
            off += snprintf(out+off, cap-(size_t)off, "%d. %s (%s) - %d obs\n",
                i+1, e->name, e->type[0] ? e->type : "entity", e->obs_count);
        }
    } else {
        nc_str op_s = nc_json_str(nc_json_get(root, "operation"), "");
        snprintf(out, cap, "error: unknown op '%.*s'", (int)op_s.len, op_s.ptr);
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
        .ctx = mem_ctx, .execute = guardian_execute, .free = NULL,
    };
}


/* ══════════════════════════════════════════════════════════════════
 *  I2C TOOL — read/write I2C devices (sensors, actuators)
 *
 *  Operations:
 *    scan          — list devices on I2C bus
 *    read          — read register from device
 *    write         — write register to device
 *    mpu_read      — read MPU6050 accel/gyro/temp
 *
 *  Uses Linux /dev/i2c-N + ioctl. Pure C. No external deps.
 * ══════════════════════════════════════════════════════════════════ */

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* ── MPU6050 constants ──────────────────────────────────────── */
#define MPU_ADDR   0x68
#define MPU_PWR    0x6B
#define MPU_ACCEL  0x3B  /* 14 bytes: accel(6) + temp(2) + gyro(6) */

/* ── Helper: read N bytes from I2C register ──────────────────── */
static bool i2c_read_reg(int bus, int addr, int reg, uint8_t *buf, int len) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    if (ioctl(fd, I2C_SLAVE, addr) < 0) { close(fd); return false; }
    uint8_t reg_buf = (uint8_t)reg;
    if (write(fd, &reg_buf, 1) != 1) { close(fd); return false; }
    int n = (int)read(fd, buf, (size_t)len);
    close(fd);
    return n == len;
}

/* ── Helper: write byte to I2C register ─────────────────────── */
static bool i2c_write_reg(int bus, int addr, int reg, uint8_t val) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    if (ioctl(fd, I2C_SLAVE, addr) < 0) { close(fd); return false; }
    uint8_t data[2] = { (uint8_t)reg, val };
    int n = (int)write(fd, data, 2);
    close(fd);
    return n == 2;
}

/* ── Tool executor ──────────────────────────────────────────── */
static bool i2c_execute(nc_tool *self, const char *json, char *out, size_t cap) {
    (void)self;
    nc_arena a;
    nc_arena_init(&a, strlen(json)*2+2048);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) {
        nc_strlcpy(out, "error: invalid args", cap);
        nc_arena_free(&a);
        return false;
    }

    nc_str op = nc_json_str(nc_json_get(root, "operation"), "");
    int bus = (int)nc_json_num(nc_json_get(root, "bus"), 1);
    int addr = (int)nc_json_num(nc_json_get(root, "address"), -1);
    int reg = (int)nc_json_num(nc_json_get(root, "register"), -1);
    int val = (int)nc_json_num(nc_json_get(root, "value"), 0);
    int len = (int)nc_json_num(nc_json_get(root, "length"), 6);

    if (op.len == 0) {
        nc_strlcpy(out, "error: missing operation", cap);
        nc_arena_free(&a);
        return false;
    }

    if (nc_str_eql(op, "scan")) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            snprintf(out, cap, "error: cannot open /dev/i2c-%d", bus);
            nc_arena_free(&a);
            return false;
        }
        int off = snprintf(out, cap, "I2C-%d scan:\n", bus);
        for (int a = 0x03; a <= 0x77 && (size_t)off < cap-32; a++) {
            if (ioctl(fd, I2C_SLAVE, a) == 0) {
                off += snprintf(out+off, cap-(size_t)off, "  0x%02X\n", a);
            }
        }
        if (off < 20) off += snprintf(out+off, cap-(size_t)off, "  (none found)\n");
        close(fd);

    } else if (nc_str_eql(op, "write")) {
        if (addr < 0 || reg < 0) {
            nc_strlcpy(out, "error: need address + register", cap);
            nc_arena_free(&a);
            return false;
        }
        bool ok = i2c_write_reg(bus, addr, reg, (uint8_t)val);
        snprintf(out, cap, ok ? "Wrote 0x%02X → 0x%02X[0x%02X]" : "error: write failed",
                 (uint8_t)val, addr, reg);

    } else if (nc_str_eql(op, "read")) {
        if (addr < 0 || reg < 0) {
            nc_strlcpy(out, "error: need address + register", cap);
            nc_arena_free(&a);
            return false;
        }
        uint8_t buf[32];
        if (len > 32) len = 32;
        if (i2c_read_reg(bus, addr, reg, buf, len)) {
            int off = snprintf(out, cap, "0x%02X[0x%02X]:", addr, reg);
            for (int i = 0; i < len; i++)
                off += snprintf(out+off, cap-(size_t)off, " %02X", buf[i]);
        } else {
            nc_strlcpy(out, "error: read failed (check address)", cap);
        }

    } else if (nc_str_eql(op, "mpu_read") || nc_str_eql(op, "mpu6050")) {
        /* MPU6050 read: wake up, then read 14 bytes */
        addr = MPU_ADDR;
        i2c_write_reg(bus, addr, MPU_PWR, 0);  /* wake up */
        usleep(10000); /* 10ms delay */
        uint8_t buf[14];
        if (i2c_read_reg(bus, addr, MPU_ACCEL, buf, 14)) {
            int16_t ax = (int16_t)(buf[0]<<8|buf[1]);
            int16_t ay = (int16_t)(buf[2]<<8|buf[3]);
            int16_t az = (int16_t)(buf[4]<<8|buf[5]);
            int16_t temp = (int16_t)(buf[6]<<8|buf[7]);
            int16_t gx = (int16_t)(buf[8]<<8|buf[9]);
            int16_t gy = (int16_t)(buf[10]<<8|buf[11]);
            int16_t gz = (int16_t)(buf[12]<<8|buf[13]);
            float temp_c = temp / 340.0f + 36.53f;
            snprintf(out, cap,
                "MPU6050 (@0x68):\n"
                "  Accel: X=%d  Y=%d  Z=%d\n"
                "  Gyro:  X=%d  Y=%d  Z=%d\n"
                "  Temp:  %.2f C\n",
                ax, ay, az, gx, gy, gz, temp_c);
        } else {
            nc_strlcpy(out, "error: MPU6050 not found (check wiring)", cap);
        }
    } else {
        snprintf(out, cap, "error: unknown op '%.*s'", (int)op.len, op.ptr);
    }

    nc_arena_free(&a);
    return true;
}

nc_tool nc_tool_i2c(void) {
    return (nc_tool){
        .def = {
            .name = "i2c",
            .description = "I2C bus master. Ops: scan (list devices), read (register[length]), write (register=value), mpu_read (MPU6050 accel/gyro/temp). Params: bus (default 1), address, register, value, length.",
            .parameters_json = "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"operation\":{\"type\":\"string\"},"
                    "\"bus\":{\"type\":\"integer\"},"
                    "\"address\":{\"type\":\"integer\"},"
                    "\"register\":{\"type\":\"integer\"},"
                    "\"value\":{\"type\":\"integer\"},"
                    "\"length\":{\"type\":\"integer\"}"
                "},"
                "\"required\":[\"operation\"]"
            "}",
        },
        .ctx = NULL, .execute = i2c_execute, .free = NULL,
    };
}

#ifdef NC_TEST
void nc_test_builtin_tools(void) {
    char out[4096];
    nc_tool reasoning = nc_tool_reasoning();

    NC_ASSERT(reasoning.execute(&reasoning,
        "{\"thought\":\"first\",\"nextThoughtNeeded\":true,\"thoughtNumber\":1,\"totalThoughts\":2}",
        out, sizeof(out)), "reasoning first step");
    NC_ASSERT(strstr(out, "\"status\":\"ok\"") != NULL, "reasoning remains active");
    NC_ASSERT(reasoning.execute(&reasoning,
        "{\"thought\":\"second\",\"nextThoughtNeeded\":false,\"thoughtNumber\":2,\"totalThoughts\":2}",
        out, sizeof(out)), "reasoning final step");
    NC_ASSERT(strstr(out, "\"n\":2") != NULL, "reasoning returns full chain");

    nc_tool tavily = nc_tool_tavily_search(NULL);
    NC_ASSERT(!tavily.execute(&tavily, "{\"query\":\"test\"}", out, sizeof(out)),
        "Tavily rejects missing key");
    NC_ASSERT(strstr(out, "TAVILY_API_KEY") != NULL, "Tavily missing-key guidance");
}
#endif
