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

/* ── Shared helpers ───────────────────────────────────────────── */

typedef struct {
    char api_key[256];
    char api_url[256];
} provider_ctx;

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
            "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"tools\":%s}",
            req->model, msgs_json, req->temperature,
            req->max_tokens > 0 ? req->max_tokens : 4096,
            req->tools_json);
    } else {
        body_len = snprintf(body, body_sz,
            "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d}",
            req->model, msgs_json, req->temperature,
            req->max_tokens > 0 ? req->max_tokens : 4096);
    }

    if ((size_t)body_len >= body_sz) {
        nc_log(NC_LOG_WARN, "Request body truncated (%d > %zu), reallocating", body_len, body_sz);
        body_sz = (size_t)body_len + 1;
        char *new_body = (char *)realloc(body, body_sz);
        if (!new_body) { free(body); free(msgs_json); return false; }
        body = new_body;
        if (req->tools_json) {
            body_len = snprintf(body, body_sz,
                "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d,\"tools\":%s}",
                req->model, msgs_json, req->temperature,
                req->max_tokens > 0 ? req->max_tokens : 4096,
                req->tools_json);
        } else {
            body_len = snprintf(body, body_sz,
                "{\"model\":\"%s\",\"messages\":%s,\"temperature\":%.2f,\"max_tokens\":%d}",
                req->model, msgs_json, req->temperature,
                req->max_tokens > 0 ? req->max_tokens : 4096);
        }
    }

    /* Headers */
    char auth_hdr[300];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", ctx->api_key);
    const char *headers[] = {
        "Content-Type: application/json",
        auth_hdr,
    };

    /* URL: append /chat/completions */
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", ctx->api_url);

    /* Retry logic */
    int retries = 2;
    bool result = false;
    nc_http_response http_resp;
    memset(&http_resp, 0, sizeof(http_resp));

    while (retries-- > 0) {
        if (nc_http_post(url, body, (size_t)body_len, headers, 2, &http_resp)) {
            if (http_resp.status == 200) {
                break;
            } else if (http_resp.status == 429 || http_resp.status >= 500) {
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
        nc_log(NC_LOG_ERROR, "Provider communication failed after retries.");
        goto cleanup;
    }

    /* Parse response JSON */
    {
        nc_arena a;
        nc_arena_init(&a, http_resp.body_len * 2 + 1024);
        nc_json *root = nc_json_parse(&a, http_resp.body, http_resp.body_len);

        if (!root) {
            nc_log(NC_LOG_ERROR, "Failed to parse provider response");
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
 *  Default model: deepseek-v4-flash-free (1M context, $0)
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

/* ══════════════════════════════════════════════════════════════════
 *  Provider Factory
 * ══════════════════════════════════════════════════════════════════ */

nc_provider nc_provider_from_config(const nc_config *cfg) {
    const char *prov = cfg->default_provider;
    const char *key  = cfg->api_key;
    const char *url  = cfg->api_url;

    if (strcmp(prov, "opencode") == 0 || strcmp(prov, "opencode-free") == 0) {
        return nc_provider_opencode(key);
    }

    /* Default: OpenAI-compatible (works with OpenRouter, OpenAI, etc.) */
    return nc_provider_openai(key, url && url[0] ? url : "https://openrouter.ai/api/v1");
}
