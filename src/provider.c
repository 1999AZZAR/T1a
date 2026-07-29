/*
 * Provider: OpenAI-compatible (used by OpenCode API).
 * OpenCode endpoint: https://opencode.ai/zen/v1
 * Models: deepseek-v4-flash-free (default), mimo-v2.5-free,
 *         nemotron-3-ultra-free, north-mini-code-free
 *
 * Handles full tool-call round-trips:
 *   - Serialize assistant messages with tool_calls
 *   - Serialize tool-result messages
 *   - Parse tool_calls from responses
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
typedef struct {
    nc_stream_cb cb;
    void *user_data;
    char line_buf[8192];
    size_t line_len;
    char *full_resp;
    size_t full_cap;
    size_t full_len;
} sse_parser_ctx;

static bool openai_stream_chunk_cb(void *user_data, const char *data, size_t len) {
    sse_parser_ctx *ctx = (sse_parser_ctx *)user_data;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            ctx->line_buf[ctx->line_len] = '\0';
            if (strncmp(ctx->line_buf, "data: ", 6) == 0) {
                const char *json_str = ctx->line_buf + 6;
                if (strcmp(json_str, "[DONE]") != 0 && ctx->cb) {
                    /* Parse chunk JSON to get content delta */
                    nc_arena a;
                    nc_arena_init(&a, ctx->line_len + 1024);
                    nc_json *root = nc_json_parse(&a, json_str, strlen(json_str));
                    if (root) {
                        nc_json *choices = nc_json_get(root, "choices");
                        if (choices && choices->type == NC_JSON_ARRAY && choices->array.count > 0) {
                            nc_json *delta = nc_json_get(&choices->array.items[0], "delta");
                            if (delta) {
                                nc_json *content = nc_json_get(delta, "content");
                                if (content && content->type == NC_JSON_STRING && content->string.len > 0) {
                                    /* Pass raw delta to upstream */
                                    ctx->cb(ctx->user_data, content->string.ptr);
                                    size_t clen = content->string.len;
                                    if (ctx->full_len + clen >= ctx->full_cap) {
                                        ctx->full_cap = (ctx->full_cap == 0) ? 4096 : ctx->full_cap * 2;
                                        while (ctx->full_len + clen >= ctx->full_cap) ctx->full_cap *= 2;
                                        ctx->full_resp = realloc(ctx->full_resp, ctx->full_cap);
                                    }
                                    if (ctx->full_resp) {
                                        memcpy(ctx->full_resp + ctx->full_len, content->string.ptr, clen);
                                        ctx->full_len += clen;
                                        ctx->full_resp[ctx->full_len] = '\0';
                                    }
                                } else {
                                    /* We explicitly drop reasoning_content from UI streaming, but MUST keep it for memory/fallback */
                                    nc_json *reasoning = nc_json_get(delta, "reasoning_content");
                                    if (reasoning && reasoning->type == NC_JSON_STRING && reasoning->string.len > 0) {
                                        size_t rlen = reasoning->string.len;
                                        size_t extra = 15; /* "<think>" + "</think>" */
                                        if (ctx->full_len + rlen + extra >= ctx->full_cap) {
                                            ctx->full_cap = (ctx->full_cap == 0) ? 4096 : ctx->full_cap * 2;
                                            while (ctx->full_len + rlen + extra >= ctx->full_cap) ctx->full_cap *= 2;
                                            ctx->full_resp = realloc(ctx->full_resp, ctx->full_cap);
                                        }
                                        if (ctx->full_resp) {
                                            memcpy(ctx->full_resp + ctx->full_len, "<think>", 7);
                                            ctx->full_len += 7;
                                            memcpy(ctx->full_resp + ctx->full_len, reasoning->string.ptr, rlen);
                                            ctx->full_len += rlen;
                                            memcpy(ctx->full_resp + ctx->full_len, "</think>", 8);
                                            ctx->full_len += 8;
                                            ctx->full_resp[ctx->full_len] = '\0';
                                        }
                                    }
                                }
                            }
                        }
                    }
                    nc_arena_free(&a);
                }
            }
            ctx->line_len = 0;
        } else if (data[i] != '\r') {
            if (ctx->line_len < sizeof(ctx->line_buf) - 1) {
                ctx->line_buf[ctx->line_len++] = data[i];
            }
        }
    }
    return true;
}


/* ── Shared helpers ───────────────────────────────────────────── */

typedef struct {
    char api_key[256];
    char api_url[256];
} provider_ctx;

typedef struct {
    nc_provider primary;
    nc_provider fallback;
    char fallback_model[128];
} chain_ctx;

/* Escape a string for JSON, writing into buf+off. Returns new offset. */
static int json_escape_into(char *buf, size_t bufsz, int off, const char *s) {
    if (!s) return off;
    for (; *s && (size_t)off < bufsz - 10; s++) {
        switch (*s) {
            case '"':  buf[off++] = '\\'; buf[off++] = '"';  break;
            case '\\': buf[off++] = '\\'; buf[off++] = '\\'; break;
            case '\n': buf[off++] = '\\'; buf[off++] = 'n';  break;
            case '\r': buf[off++] = '\\'; buf[off++] = 'r';  break;
            case '\t': buf[off++] = '\\'; buf[off++] = 't';  break;
            case '\b': buf[off++] = '\\'; buf[off++] = 'b';  break;
            case '\f': buf[off++] = '\\'; buf[off++] = 'f';  break;
            default:
                if ((unsigned char)*s >= 0x20)
                    buf[off++] = *s;
                else {
                    off += snprintf(buf + off, bufsz - (size_t)off, "\\u%04x", (unsigned char)*s);
                }
                break;
        }
    }
    return off;
}

/* Compute buffer size needed for messages (generous estimate) */
static size_t estimate_messages_size(const nc_message *msgs, int count) {
    size_t sz = 512;
    for (int i = 0; i < count; i++) {
        sz += 256;
        if (msgs[i].content)
            sz += strlen(msgs[i].content) * 2;
        for (int j = 0; j < msgs[i].tool_call_count; j++) {
            sz += 256;
            sz += strlen(msgs[i].tool_calls[j].arguments) * 2;
        }
    }
    return sz;
}

/* ══════════════════════════════════════════════════════════════════
 *  OpenAI-compatible provider (OpenCode, OpenRouter, etc.)
 *
 *  Wire format for tool calls:
 *    Assistant: { "role":"assistant", "content":null,
 *                 "tool_calls":[{"id":"call_x","type":"function",
 *                   "function":{"name":"foo","arguments":"{...}"}}] }
 *    Result:   { "role":"tool", "tool_call_id":"call_x", "content":"..." }
 * ══════════════════════════════════════════════════════════════════ */

/* Build the messages array */
static int openai_build_messages(char *buf, size_t bufsz,
                                 const nc_message *msgs, int count) {
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");

    for (int i = 0; i < count; i++) {
        if ((size_t)off >= bufsz - 10) break;
        if (i > 0) buf[off++] = ',';

        if (msgs[i].tool_call_count > 0) {
            /* Assistant message with tool_calls */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"assistant\",\"content\":");
            if (msgs[i].content && msgs[i].content[0]) {
                buf[off++] = '"';
                off = json_escape_into(buf, bufsz, off, msgs[i].content);
                buf[off++] = '"';
            } else {
                off += snprintf(buf + off, bufsz - (size_t)off, "null");
            }
            off += snprintf(buf + off, bufsz - (size_t)off, ",\"tool_calls\":[");
            for (int j = 0; j < msgs[i].tool_call_count; j++) {
                if (j > 0) buf[off++] = ',';
                const nc_tool_call *tc = &msgs[i].tool_calls[j];
                off += snprintf(buf + off, bufsz - (size_t)off,
                    "{\"id\":\"%s\",\"type\":\"function\","
                    "\"function\":{\"name\":\"%s\",\"arguments\":\"",
                    tc->id, tc->name);
                off = json_escape_into(buf, bufsz, off, tc->arguments);
                off += snprintf(buf + off, bufsz - (size_t)off, "\"}}");
            }
            off += snprintf(buf + off, bufsz - (size_t)off, "]}");

        } else if (msgs[i].tool_call_id && msgs[i].tool_call_id[0]) {
            /* Tool result message */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"",
                msgs[i].tool_call_id);
            off = json_escape_into(buf, bufsz, off, msgs[i].content);
            off += snprintf(buf + off, bufsz - (size_t)off, "\"}");

        } else {
            /* Normal message (system/user/assistant) */
            off += snprintf(buf + off, bufsz - (size_t)off,
                "{\"role\":\"%s\",\"content\":\"", msgs[i].role);
            off = json_escape_into(buf, bufsz, off, msgs[i].content);
            off += snprintf(buf + off, bufsz - (size_t)off, "\"}");
        }
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]");
    return off;
}

/* Parse tool_calls from OpenAI response JSON */
static void openai_parse_tool_calls(nc_json *tc_arr, nc_chat_response *resp) {
    if (!tc_arr || tc_arr->type != NC_JSON_ARRAY) return;

    for (int i = 0; i < tc_arr->array.count && resp->tool_call_count < NC_MAX_TOOL_CALLS; i++) {
        nc_json *tc = &tc_arr->array.items[i];
        nc_json *fn = nc_json_get(tc, "function");
        if (!fn) continue;

        nc_tool_call *out = &resp->tool_calls[resp->tool_call_count];

        nc_str id = nc_json_str(nc_json_get(tc, "id"), "");
        if (id.len > 0) {
            size_t cl = id.len < sizeof(out->id) - 1 ? id.len : sizeof(out->id) - 1;
            memcpy(out->id, id.ptr, cl);
            out->id[cl] = '\0';
        }

        nc_str name = nc_json_str(nc_json_get(fn, "name"), "");
        if (name.len > 0) {
            size_t cl = name.len < sizeof(out->name) - 1 ? name.len : sizeof(out->name) - 1;
            memcpy(out->name, name.ptr, cl);
            out->name[cl] = '\0';
        }

        nc_str args = nc_json_str(nc_json_get(fn, "arguments"), "{}");
        if (args.len > 0) {
            size_t cl = args.len < sizeof(out->arguments) - 1 ? args.len : sizeof(out->arguments) - 1;
            memcpy(out->arguments, args.ptr, cl);
            out->arguments[cl] = '\0';
        }

        resp->tool_call_count++;
    }

    resp->has_tool_calls = resp->tool_call_count > 0;
}

static bool openai_chat(nc_provider *self, const nc_chat_request *req, nc_chat_response *resp) {
    provider_ctx *ctx = (provider_ctx *)self->ctx;
    memset(resp, 0, sizeof(*resp));

    /* Build messages JSON */
    size_t msgs_buf_sz = estimate_messages_size(req->messages, req->message_count);
    char *msgs_json = (char *)malloc(msgs_buf_sz);
    if (!msgs_json) return false;
    openai_build_messages(msgs_json, msgs_buf_sz, req->messages, req->message_count);

    /* Build request body */
    size_t msgs_actual_len = strlen(msgs_json);
    size_t tools_len = req->tools_json ? strlen(req->tools_json) : 0;
    size_t body_sz = msgs_actual_len + tools_len + 4096;
    char *body = (char *)malloc(body_sz);
    if (!body) { free(msgs_json); return false; }

    int body_len;
    if (req->tools_json) {
        body_len = snprintf(body, body_sz,
            "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"stream\":%s,\"tools\":%s}",
            req->model, msgs_json, req->temperature,
            req->max_tokens > 0 ? req->max_tokens : 4096,
            req->stream_cb ? "true" : "false",
            req->tools_json);
    } else {
        body_len = snprintf(body, body_sz,
            "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"stream\":%s}",
            req->model, msgs_json, req->temperature,
            req->max_tokens > 0 ? req->max_tokens : 4096,
            req->stream_cb ? "true" : "false");
    }

    if ((size_t)body_len >= body_sz) {
        nc_log(NC_LOG_WARN, "Request body truncated (%d > %zu), reallocating", body_len, body_sz);
        body_sz = (size_t)body_len + 1;
        char *new_body = (char *)realloc(body, body_sz);
        if (!new_body) { free(body); free(msgs_json); return false; }
        body = new_body;
        if (req->tools_json) {
            body_len = snprintf(body, body_sz,
                "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"stream\":%s,\"tools\":%s}",
                req->model, msgs_json, req->temperature,
                req->max_tokens > 0 ? req->max_tokens : 4096,
                req->stream_cb ? "true" : "false",
                req->tools_json);
        } else {
            body_len = snprintf(body, body_sz,
                "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"stream\":%s}",
                req->model, msgs_json, req->temperature,
                req->max_tokens > 0 ? req->max_tokens : 4096,
                req->stream_cb ? "true" : "false");
        }
    }

    /* Headers */
    char auth_hdr[300];
    const char *headers[2] = {"Content-Type: application/json", NULL};
    int header_count = 1;
    if (ctx->api_key[0]) {
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", ctx->api_key);
        headers[header_count++] = auth_hdr;
    }

    /* URL: append /chat/completions */
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", ctx->api_url);

    /* Retry logic */
    int retries = 2;
    bool result = false;
    nc_http_response http_resp;
    memset(&http_resp, 0, sizeof(http_resp));

    sse_parser_ctx sse_ctx;
    memset(&sse_ctx, 0, sizeof(sse_ctx));
    sse_ctx.cb = req->stream_cb;
    sse_ctx.user_data = req->stream_user_data;

    while (retries-- > 0) {
        if (nc_http_post_stream(url, body, (size_t)body_len, headers, header_count, req->stream_cb ? openai_stream_chunk_cb : NULL, &sse_ctx, &http_resp)) {
            if (http_resp.status == 200) {
                break;
            } else if (http_resp.status == 429) {
                nc_log(NC_LOG_WARN, "HTTP 429: provider rate limit reached");
                break;
            } else if (http_resp.status >= 500) {
                nc_log(NC_LOG_WARN, "HTTP %d, retrying...", http_resp.status);
                nc_http_response_free(&http_resp);
                usleep(1000000);
                continue;
            } else if (http_resp.status == 400 && strstr(http_resp.body, "tool")) {
                nc_log(NC_LOG_WARN, "HTTP 400 (tool schema error), skipping retries");
                goto cleanup;
            } else {
                nc_log(NC_LOG_ERROR, "Fatal HTTP %d: %.200s", http_resp.status, http_resp.body);
                goto cleanup;
            }
        }
        nc_log(NC_LOG_WARN, "HTTP request failed, retrying...");
        nc_http_response_free(&http_resp);
        usleep(1000000);
    }

    if (http_resp.status != 200) {
        nc_log(NC_LOG_ERROR, "Provider request failed (HTTP %d)", http_resp.status);
        goto cleanup;
    }

    if (req->stream_cb) {
        if (sse_ctx.full_resp && sse_ctx.full_len > 0) {
            size_t cplen = sse_ctx.full_len < sizeof(resp->content) - 1 ? sse_ctx.full_len : sizeof(resp->content) - 1;
            memcpy(resp->content, sse_ctx.full_resp, cplen);
            resp->content[cplen] = '\0';
            resp->has_tool_calls = false;
            result = true;
        } else {
            resp->content[0] = '\0';
            resp->has_tool_calls = false;
            result = true;
        }
        if (sse_ctx.full_resp) free(sse_ctx.full_resp);
        goto cleanup;
    }

    /* Parse response JSON */
    {
        nc_arena a;
        nc_arena_init(&a, http_resp.body_len * 2 + 1024);
        nc_json *root = nc_json_parse(&a, http_resp.body, http_resp.body_len);

        if (!root) {
            nc_log(NC_LOG_ERROR, "Failed to parse provider response. Raw body: %.*s", (int)http_resp.body_len, http_resp.body);
            nc_arena_free(&a);
            goto cleanup;
        }

        nc_json *choices = nc_json_get(root, "choices");
        if (choices && choices->type == NC_JSON_ARRAY && choices->array.count > 0) {
            nc_json *choice0 = &choices->array.items[0];
            nc_json *message = nc_json_get(choice0, "message");
            if (message) {
                nc_json *content_node = nc_json_get(message, "content");
                bool got_content = false;
                if (content_node && content_node->type == NC_JSON_STRING) {
                    nc_str content = content_node->string;
                    if (content.len > 0) {
                        size_t cplen = content.len < sizeof(resp->content) - 1
                            ? content.len : sizeof(resp->content) - 1;
                        memcpy(resp->content, content.ptr, cplen);
                        resp->content[cplen] = '\0';
                        got_content = true;
                    }
                }

                /* Reasoning models (DeepSeek, MiMo, Nemotron) put response
                   in 'reasoning_content' or 'reasoning' when 'content' is null.
                   Fall back to those fields. */
                if (!got_content) {
                    nc_json *reasoning = nc_json_get(message, "reasoning_content");
                    if (!reasoning || reasoning->type != NC_JSON_STRING)
                        reasoning = nc_json_get(message, "reasoning");
                    if (reasoning && reasoning->type == NC_JSON_STRING) {
                        nc_str rtext = reasoning->string;
                        size_t cplen = rtext.len < sizeof(resp->content) - 1
                            ? rtext.len : sizeof(resp->content) - 1;
                        memcpy(resp->content, rtext.ptr, cplen);
                        resp->content[cplen] = '\0';
                    }
                }

                nc_json *tc = nc_json_get(message, "tool_calls");
                openai_parse_tool_calls(tc, resp);
            }
        }

        nc_json *usage = nc_json_get(root, "usage");
        if (usage) {
            resp->prompt_tokens = (int)nc_json_num(nc_json_get(usage, "prompt_tokens"), 0);
            resp->completion_tokens = (int)nc_json_num(nc_json_get(usage, "completion_tokens"), 0);
        }

        nc_arena_free(&a);
    }
    result = true;

cleanup:
    nc_http_response_free(&http_resp);
    free(msgs_json);
    free(body);
    return result;
}

static void provider_free(nc_provider *self) {
    free(self->ctx);
    self->ctx = NULL;
}

/* ── OpenAI-compatible provider (generic) ────────────────────── */

nc_provider nc_provider_openai(const char *api_key, const char *api_url) {
    provider_ctx *ctx = (provider_ctx *)calloc(1, sizeof(provider_ctx));
    if (!ctx)
        return (nc_provider){ .name = "openai", .ctx = NULL, .chat = NULL, .free = NULL };
    if (api_key) nc_strlcpy(ctx->api_key, api_key, sizeof(ctx->api_key));
    if (api_url) nc_strlcpy(ctx->api_url, api_url, sizeof(ctx->api_url));

    return (nc_provider){
        .name = "openai",
        .ctx  = ctx,
        .chat = openai_chat,
        .free = provider_free,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  OpenCode Provider
 *
 *  Uses OpenCode's free API: https://opencode.ai/zen/v1
 *  Main default: deepseek-v4-flash-free (1M context, $0)
 *
 *  Free models available:
 *    deepseek-v4-flash-free (1M ctx)
 *    mimo-v2.5-free         (262K ctx)
 *    nemotron-3-ultra-free  (128K ctx)
 *    north-mini-code-free   (128K ctx)
 * ══════════════════════════════════════════════════════════════════ */

#define OPENCODE_BASE_URL "https://opencode.ai/zen/v1"
#define OPENCODE_DEFAULT_MODEL "deepseek-v4-flash-free"

nc_provider nc_provider_opencode(const char *api_key) {
    /* If api_key is empty or null, use a placeholder token.
       OpenCode's free models don't strictly require auth, but
       the HTTP layer needs a non-empty Bearer token to avoid
       auth header issues. */
    const char *key = (api_key && api_key[0]) ? api_key : "noclaw-free";
    return nc_provider_openai(key, OPENCODE_BASE_URL);
}

static bool chain_chat(nc_provider *self, const nc_chat_request *req, nc_chat_response *resp) {
    chain_ctx *ctx = (chain_ctx *)self->ctx;
    if (ctx->primary.chat(&ctx->primary, req, resp)) return true;

    nc_log(NC_LOG_WARN, "Primary provider failed; falling back to %s with %s",
        ctx->fallback.name, ctx->fallback_model);
    nc_chat_request fallback_req = *req;
    fallback_req.model = ctx->fallback_model;
    return ctx->fallback.chat(&ctx->fallback, &fallback_req, resp);
}

static void chain_free(nc_provider *self) {
    chain_ctx *ctx = (chain_ctx *)self->ctx;
    if (!ctx) return;
    if (ctx->primary.free) ctx->primary.free(&ctx->primary);
    if (ctx->fallback.free) ctx->fallback.free(&ctx->fallback);
    free(ctx);
    self->ctx = NULL;
}

static nc_provider provider_with_fallback(nc_provider primary, const nc_config *cfg) {
    if (!cfg->fallback_provider[0] || !cfg->fallback_model[0]) return primary;

    const char *url = cfg->fallback_api_url;
    if (!url[0] && strcmp(cfg->fallback_provider, "kilo") == 0)
        url = "https://api.kilo.ai/api/gateway";
    if (!url[0]) return primary;

    chain_ctx *ctx = (chain_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) return primary;
    ctx->primary = primary;
    ctx->fallback = nc_provider_openai(cfg->fallback_api_key, url);
    ctx->fallback.name = cfg->fallback_provider;
    nc_strlcpy(ctx->fallback_model, cfg->fallback_model, sizeof(ctx->fallback_model));

    return (nc_provider){
        .name = "provider-chain",
        .ctx = ctx,
        .chat = chain_chat,
        .free = chain_free,
    };
}

/* ══════════════════════════════════════════════════════════════════
 *  Provider Factory
 * ══════════════════════════════════════════════════════════════════ */

nc_provider nc_provider_from_config(const nc_config *cfg) {
    const char *prov = cfg->default_provider;
    const char *key  = cfg->api_key;
    const char *url  = cfg->api_url;

    if (strcmp(prov, "opencode") == 0 || strcmp(prov, "opencode-free") == 0) {
        return provider_with_fallback(nc_provider_opencode(key), cfg);
    }

    /* Default: OpenAI-compatible (works with OpenRouter, OpenAI, etc.) */
    return provider_with_fallback(
        nc_provider_openai(key, url && url[0] ? url : "https://openrouter.ai/api/v1"), cfg);
}
