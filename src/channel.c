/*
 * Telegram channel implementation using minimalist BearSSL + native HTTP.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    char token[128];
    long last_update_id;
} tg_ctx;

static void tg_offset_path(char *buf, size_t bufsz) {
    char cfg_dir[1024];
    nc_path_join(cfg_dir, sizeof(cfg_dir), nc_home_dir(), NC_CONFIG_DIR);
    nc_path_join(buf, bufsz, cfg_dir, "telegram.offset");
}

static void tg_load_offset(tg_ctx *ctx) {
    char path[1024];
    tg_offset_path(path, sizeof(path));

    size_t len = 0;
    char *data = nc_read_file(path, &len);
    if (!data) return;

    ctx->last_update_id = strtol(data, NULL, 10);
    free(data);
}

static void tg_save_offset(const tg_ctx *ctx) {
    char cfg_dir[1024];
    char path[1024];
    char data[64];

    nc_path_join(cfg_dir, sizeof(cfg_dir), nc_home_dir(), NC_CONFIG_DIR);
    if (!nc_mkdir_p(cfg_dir)) return;

    tg_offset_path(path, sizeof(path));
    int len = snprintf(data, sizeof(data), "%ld\n", ctx->last_update_id);
    if (len <= 0) return;

    nc_write_file(path, data, (size_t)len);
}

/* Forward declaration */
static void tg_set_typing(tg_ctx *ctx, long chat_id);

/* Typing heartbeat — sends typing action every 4s while agent is busy */
typedef struct {
    tg_ctx  *ctx;
    long     chat_id;
    volatile int done;
} typing_heartbeat;

static void *typing_thread(void *arg) {
    typing_heartbeat *h = (typing_heartbeat *)arg;
    while (!h->done) {
        sleep(4);
        if (!h->done) tg_set_typing(h->ctx, h->chat_id);
    }
    return NULL;
}

static void tg_set_typing(tg_ctx *ctx, long chat_id) {
    char url[512], body[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendChatAction", ctx->token);
    snprintf(body, sizeof(body), "{\"chat_id\":%ld,\"action\":\"typing\"}", chat_id);
    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    if (nc_http_post(url, body, strlen(body), hdrs, 1, &resp)) {
        nc_http_response_free(&resp);
    }
}


static int append_escaped_text(char *body, int off, size_t body_sz, const char *text) {
    bool in_b = false, in_i = false, in_c = false;
    for (const char *p = text; *p; p++) {
        if (off > (int)body_sz - 128) break;
        if (*p == '*' && *(p+1) == '*') {
            const char *tag = in_b ? "</b>" : "<b>";
            in_b = !in_b;
            memcpy(body + off, tag, 4); off += 4;
            p++;
        } else if (*p == '*') {
            const char *tag = in_i ? "</i>" : "<i>";
            in_i = !in_i;
            memcpy(body + off, tag, 4); off += 4;
        } else if (*p == '`') {
            const char *tag = in_c ? "</code>" : "<code>";
            in_c = !in_c;
            memcpy(body + off, tag, 7); off += 7;
        } else if (*p == '<') { memcpy(body + off, "&lt;", 4); off += 4; }
        else if (*p == '>') { memcpy(body + off, "&gt;", 4); off += 4; }
        else if (*p == '&') { memcpy(body + off, "&amp;", 5); off += 5; }
        else if (*p == '"') { body[off++] = '\\'; body[off++] = '"'; }
        else if (*p == '\\') { body[off++] = '\\'; body[off++] = '\\'; }
        else if (*p == '\n') { body[off++] = '\\'; body[off++] = 'n'; }
        else if (*p == '\r') { body[off++] = '\\'; body[off++] = 'r'; }
        else if (*p == '\t') { body[off++] = '\\'; body[off++] = 't'; }
        else if ((unsigned char)*p >= 32) body[off++] = *p;
    }
    if (in_b) { memcpy(body + off, "</b>", 4); off += 4; }
    if (in_i) { memcpy(body + off, "</i>", 4); off += 4; }
    if (in_c) { memcpy(body + off, "</code>", 7); off += 7; }
    return off;
}

static long tg_send_msg_ret_id(tg_ctx *ctx, long chat_id, const char *text) {
    if (!text || !text[0]) return 0;
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", ctx->token);

    size_t body_sz = strlen(text) * 2 + 1024;
    char *body = malloc(body_sz);
    if (!body) return 0;

    int off = snprintf(body, body_sz, "{\"chat_id\":%ld,\"text\":\"", chat_id);
    off = append_escaped_text(body, off, body_sz, text);
    off += snprintf(body + off, body_sz - (size_t)off, "\",\"parse_mode\":\"HTML\"}");

    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    long msg_id = 0;
    if (nc_http_post(url, body, (size_t)off, hdrs, 1, &resp)) {
        if (resp.status == 200) {
            char *id_str = strstr(resp.body, "\"message_id\":");
            if (id_str) {
                msg_id = strtol(id_str + 13, NULL, 10);
            }
        } else {
            nc_log(NC_LOG_ERROR, "TG send msg failed (HTTP %d): %.*s", resp.status, (int)resp.body_len, resp.body);
        }
        nc_http_response_free(&resp);
    }
    free(body);
    return msg_id;
}

static void tg_edit_msg(tg_ctx *ctx, long chat_id, long msg_id, const char *text) {
    if (!text || !text[0] || msg_id == 0) return;
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/editMessageText", ctx->token);

    size_t body_sz = strlen(text) * 2 + 1024;
    char *body = malloc(body_sz);
    if (!body) return;

    int off = snprintf(body, body_sz, "{\"chat_id\":%ld,\"message_id\":%ld,\"text\":\"", chat_id, msg_id);
    off = append_escaped_text(body, off, body_sz, text);
    off += snprintf(body + off, body_sz - (size_t)off, "\",\"parse_mode\":\"HTML\"}");

    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    if (nc_http_post(url, body, (size_t)off, hdrs, 1, &resp)) {
        if (resp.status != 200) {
            nc_log(NC_LOG_ERROR, "TG edit msg failed (HTTP %d): %.*s", resp.status, (int)resp.body_len, resp.body);
        }
        nc_http_response_free(&resp);
    }
    free(body);
}

typedef struct {
    tg_ctx *ctx;
    long chat_id;
    long msg_id;
    char buf[4096];
    size_t len;
    time_t last_edit;
    time_t last_typing;
} tg_stream_state;

static bool looks_like_thinking(const char *text) {
    /* Detect common model "thinking aloud" patterns that should not be shown */
    static const char *patterns[] = {
        "The user is asking", "Let me ", "I need to ", "I should ",
        "I'll ", "I will ", "Let me check", "Let me search",
        "Let me look", "I'm going to", "First, I", "First let",
        "Step 1:", "Step 2:", NULL
    };
    for (int i = 0; patterns[i]; i++) {
        if (strncmp(text, patterns[i], strlen(patterns[i])) == 0)
            return true;
    }
    return false;
}

static void tg_stream_cb(void *user_data, const char *chunk) {
    tg_stream_state *st = (tg_stream_state *)user_data;

    /* Accumulate into buf for potential future use but NEVER send during streaming.
       The model may narrate tool plans before calling them.
       The final clean answer is sent once nc_agent_chat() returns. */
    size_t clen = strlen(chunk);
    if (st->len + clen < sizeof(st->buf) - 2) {
        memcpy(st->buf + st->len, chunk, clen);
        st->len += clen;
        st->buf[st->len] = '\0';
    }

    /* Only send typing indicator so the user sees activity */
    time_t now = time(NULL);
    if (now - st->last_typing >= 4) {
        tg_set_typing(st->ctx, st->chat_id);
        st->last_typing = now;
    }
}


static void tg_send_msg(tg_ctx *ctx, long chat_id, const char *text) {
    if (!text || !text[0]) return;
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", ctx->token);

    size_t body_sz = strlen(text) * 2 + 1024;
    char *body = malloc(body_sz);
    if (!body) return;

    int off = 0;
    off += snprintf(body + off, body_sz - (size_t)off, "{\"chat_id\":%ld,\"text\":\"", chat_id);

    const char *kbd = strstr(text, "||KBD:");
    if (kbd) {
        char tmp[4096];
        size_t text_len = kbd - text;
        if (text_len >= sizeof(tmp)) text_len = sizeof(tmp) - 1;
        memcpy(tmp, text, text_len);
        tmp[text_len] = '\0';
        off = append_escaped_text(body, off, body_sz, tmp);
        off += snprintf(body + off, body_sz - (size_t)off, "\",\"parse_mode\":\"HTML\",\"reply_markup\":%s}", kbd + 6);
    } else {
        off = append_escaped_text(body, off, body_sz, text);
        off += snprintf(body + off, body_sz - (size_t)off, "\",\"parse_mode\":\"HTML\"}");
    }

    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;
    if (nc_http_post(url, body, (size_t)off, hdrs, 1, &resp)) {
        if (resp.status != 200) {
            nc_log(NC_LOG_ERROR, "TG send msg failed (HTTP %d): %.*s", resp.status, (int)resp.body_len, resp.body);
        }
        nc_http_response_free(&resp);
    }
    free(body);
}

static void tg_poll(nc_channel *self, nc_agent *agent) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;
    char url[512], body[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates", ctx->token);

    int off = 0;
    off += snprintf(body + off, sizeof(body) - (size_t)off, "{");
    if (ctx->last_update_id > 0) {
        off += snprintf(body + off, sizeof(body) - (size_t)off, "\"offset\":%ld,", ctx->last_update_id + 1);
    }
    off += snprintf(body + off, sizeof(body) - (size_t)off, "\"timeout\":30}");

    const char *hdrs[] = {"Content-Type: application/json"};
    nc_http_response resp;

    nc_log(NC_LOG_DEBUG, "Polling Telegram...");
    if (!nc_http_post(url, body, strlen(body), hdrs, 1, &resp)) {
        nc_log(NC_LOG_ERROR, "TG poll failed (network)");
        sleep(5);
        return;
    }

    if (resp.status != 200) {
        nc_log(NC_LOG_ERROR, "TG poll failed (HTTP %d): %.*s", resp.status, (int)resp.body_len, resp.body);
        nc_http_response_free(&resp);
        sleep(5);
        return;
    }

    nc_arena scratch;
    nc_arena_init(&scratch, resp.body_len * 2 + 8192);
    nc_json *root = nc_json_parse(&scratch, resp.body, resp.body_len);
    if (!root) {
        nc_arena_free(&scratch);
        nc_http_response_free(&resp);
        return;
    }

    nc_json *ok = nc_json_get(root, "ok");
    nc_json *res = nc_json_get(root, "result");
    if (ok && ok->boolean && res && res->type == NC_JSON_ARRAY) {
        for (int i = 0; i < res->array.count; i++) {
            nc_json *upd = &res->array.items[i];
            long uid = (long)nc_json_num(nc_json_get(upd, "update_id"), 0);
            if (uid > ctx->last_update_id) {
                ctx->last_update_id = uid;
                tg_save_offset(ctx);
            }

            nc_json *msg = nc_json_get(upd, "message");
            if (!msg) continue;

            nc_json *chat = nc_json_get(msg, "chat");
            long chat_id = (long)nc_json_num(nc_json_get(chat, "id"), 0);
            nc_str text = nc_json_str(nc_json_get(msg, "text"), "");

            if (text.len > 0) {
                char *cmd = malloc(text.len + 1);
                memcpy(cmd, text.ptr, text.len);
                cmd[text.len] = '\0';

                nc_log(NC_LOG_INFO, "TG: [%ld] %s", chat_id, cmd);

                if (nc_commands_execute(agent, cmd, chat_id, self)) {
                    free(cmd);
                    continue;
                }

                /* Start typing heartbeat thread — keeps indicator alive during tool calls */
                typing_heartbeat hb = { .ctx = ctx, .chat_id = chat_id, .done = 0 };
                pthread_t hb_thread;
                tg_set_typing(ctx, chat_id);
                pthread_create(&hb_thread, NULL, typing_thread, &hb);

                const char *reply = nc_agent_chat(agent, cmd, NULL, NULL);

                hb.done = 1;
                pthread_join(hb_thread, NULL);

                tg_send_msg(ctx, chat_id, reply);
                free(cmd);
            }
        }
    }

    nc_arena_free(&scratch);
    nc_http_response_free(&resp);
}

static bool tg_send(nc_channel *self, const char *to, const char *text) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;
    if (!to) return false;
    long chat_id = atol(to);
    tg_send_msg(ctx, chat_id, text);
    return true;
}

static void tg_free(nc_channel *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_channel nc_channel_telegram(const char *token) {
    tg_ctx *ctx = calloc(1, sizeof(tg_ctx));
    nc_strlcpy(ctx->token, token, sizeof(ctx->token));
    tg_load_offset(ctx);
    return (nc_channel){
        .name = "telegram",
        .ctx = ctx,
        .poll = tg_poll,
        .send = tg_send,
        .free = tg_free
    };
}
