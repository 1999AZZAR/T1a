#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>

typedef struct {
    pid_t pid;
    int fd_in;
    int fd_out;
    char rb[8192];
    size_t rb_len;
    int id_counter;
} acp_agent;

static bool acp_proc_start(acp_agent *s, const char *cmd) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[1]); close(out_pipe[0]);
        for (int i = 3; i < 1024; i++) close(i);

        char final_cmd[1024];
        if (strcmp(cmd, "codex") == 0 || strcmp(cmd, "codex --acp") == 0) {
            strcpy(final_cmd, "codex app-server --listen stdio://");
        } else if (strcmp(cmd, "gemini") == 0) {
            strcpy(final_cmd, "gemini --acp");
        } else {
            nc_strlcpy(final_cmd, cmd, sizeof(final_cmd));
        }

        char *args[64];
        int argc = 0;
        char *p = strtok(final_cmd, " ");
        while (p && argc < 63) {
            args[argc++] = p;
            p = strtok(NULL, " ");
        }
        args[argc] = NULL;

        execvp(args[0], args);
        _exit(1);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    s->pid = pid;
    s->fd_in = in_pipe[1];
    s->fd_out = out_pipe[0];
    return true;
}

static char *acp_read_msg(acp_agent *s, nc_arena *arena) {
    char *line = NULL;
    size_t line_len = 0;
    size_t line_cap = 0;

    while (1) {
        char *nl = memchr(s->rb, '\n', s->rb_len);
        if (nl) {
            size_t len = nl - s->rb;
            size_t total_len = line_len + len;
            if (line_cap <= total_len) {
                size_t new_cap = total_len + 1024;
                char *new_line = nc_arena_alloc(arena, new_cap);
                if (line) memcpy(new_line, line, line_len);
                line = new_line;
                line_cap = new_cap;
            }
            memcpy(line + line_len, s->rb, len);
            line[line_len + len] = '\0';

            size_t remain = s->rb_len - (len + 1);
            memmove(s->rb, nl + 1, remain);
            s->rb_len = remain;
            return line;
        }

        if (s->rb_len > 0) {
            size_t total_len = line_len + s->rb_len;
            if (line_cap <= total_len) {
                size_t new_cap = total_len + 4096;
                char *new_line = nc_arena_alloc(arena, new_cap);
                if (line) memcpy(new_line, line, line_len);
                line = new_line;
                line_cap = new_cap;
            }
            memcpy(line + line_len, s->rb, s->rb_len);
            line_len += s->rb_len;
            s->rb_len = 0;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s->fd_out, &fds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        int ret = select(s->fd_out + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) break;

        ssize_t n = read(s->fd_out, s->rb, sizeof(s->rb));
        if (n <= 0) break;
        s->rb_len = n;
    }
    return line;
}

static nc_json *acp_rpc_call(acp_agent *s, const char *method, const char *params_json, nc_arena *arena) {
    int id = ++s->id_counter;
    char req[8192];
    if (params_json) {
        snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}\n", method, params_json, id);
    } else {
        snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":%d}\n", method, id);
    }
    write(s->fd_in, req, strlen(req));

    time_t start = time(NULL);
    while (time(NULL) - start < 30) { /* Increased to 30s for slower OCI cold starts */
        char *line = acp_read_msg(s, arena);
        if (!line) continue;
        nc_json *root = nc_json_parse(arena, line, strlen(line));
        if (!root) continue;
        nc_json *jid = nc_json_get(root, "id");
        if (jid && jid->type == NC_JSON_NUMBER && (int)jid->number == id) {
            return nc_json_get(root, "result");
        }
    }
    return NULL;
}

static void acp_rpc_send_result(acp_agent *s, int id, const char *result_json) {
    char req[4096];
    snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id, result_json);
    write(s->fd_in, req, strlen(req));
}

static bool acp_delegate_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    char command[1024] = {0};
    char prompt[4096] = {0};

    nc_arena tmp_a;
    nc_arena_init(&tmp_a, 16384);
    nc_json *root_args = nc_json_parse(&tmp_a, args_json, strlen(args_json));
    if (root_args && root_args->type == NC_JSON_OBJECT) {
        nc_str s_cmd = nc_json_str(nc_json_get(root_args, "command"), "");
        nc_str s_prm = nc_json_str(nc_json_get(root_args, "prompt"), "");
        if (s_cmd.len > 0 && s_prm.len > 0) {
            nc_strlcpy(command, s_cmd.ptr, sizeof(command));
            nc_strlcpy(prompt, s_prm.ptr, sizeof(prompt));
        }
    }
    nc_arena_free(&tmp_a);

    if (command[0] == '\0' || prompt[0] == '\0') {
        nc_strlcpy(out, "error: missing command or prompt", out_cap);
        return false;
    }

    acp_agent s;
    memset(&s, 0, sizeof(s));
    if (!acp_proc_start(&s, command)) {
        nc_strlcpy(out, "error: failed to start ACP agent process", out_cap);
        return false;
    }

    nc_arena a;
    nc_arena_init(&a, 512 * 1024);

    /* 1. Send initialize */
    const char *init_params = "{\"protocolVersion\":1,\"capabilities\":{},\"clientInfo\":{\"name\":\"t1a-orchestrator\",\"version\":\"1.0\"}}";
    if (!acp_rpc_call(&s, "initialize", init_params, &a)) {
        nc_strlcpy(out, "error: ACP initialize failed (No JSON-RPC response from agent)", out_cap);
        nc_arena_free(&a);
        kill(s.pid, SIGTERM);
        return false;
    }

    /* 2. Create session */
    nc_json *sess_res = acp_rpc_call(&s, "session/new", "{\"cwd\":\"/tmp\",\"mcpServers\":[]}", &a);
    if (!sess_res) {
        nc_strlcpy(out, "error: ACP session/new failed (Handshake succeeded but session creation failed)", out_cap);
        nc_arena_free(&a);
        kill(s.pid, SIGTERM);
        return false;
    }
    nc_str sid = nc_json_str(nc_json_get(sess_res, "sessionId"), "");
    if (sid.len == 0) {
        nc_strlcpy(out, "error: ACP no sessionId returned from sub-agent", out_cap);
        nc_arena_free(&a);
        kill(s.pid, SIGTERM);
        return false;
    }

    /* 3. Send prompt */
    char prompt_esc[8192];
    size_t i = 0, j = 0;
    while (prompt[i] && j < sizeof(prompt_esc) - 5) {
        if (prompt[i] == '"' || prompt[i] == '\\' || prompt[i] == '\n') {
            prompt_esc[j++] = '\\';
            if (prompt[i] == '\n') prompt_esc[j++] = 'n';
            else prompt_esc[j++] = prompt[i];
        } else {
            prompt_esc[j++] = prompt[i];
        }
        i++;
    }
    prompt_esc[j] = '\0';

    char sess_prompt_params[10240];
    snprintf(sess_prompt_params, sizeof(sess_prompt_params),
             "{\"sessionId\":\"%.*s\",\"prompt\":[{\"type\":\"text\",\"text\":\"%s\"}]}",
             NC_STR_ARG(sid), prompt_esc);

    int sess_prompt_id = ++s.id_counter;
    char req[11000];
    snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"method\":\"session/prompt\",\"params\":%s,\"id\":%d}\n", sess_prompt_params, sess_prompt_id);
    write(s.fd_in, req, strlen(req));

    /* 4. Loop reading updates */
    size_t out_off = 0;
    time_t start = time(NULL);
    bool session_done = false;

    while (time(NULL) - start < 300 && !session_done) {
        char *line = acp_read_msg(&s, &a);
        if (!line) continue;

        nc_json *root = nc_json_parse(&a, line, strlen(line));
        if (!root) continue;

        nc_str method = nc_json_str(nc_json_get(root, "method"), "");
        if (nc_str_eql(method, "session/update")) {
            nc_json *params = nc_json_get(root, "params");
            nc_json *upd = nc_json_get(params, "update");
            nc_str utype = nc_json_str(nc_json_get(upd, "sessionUpdate"), "");

            if (nc_str_eql(utype, "agent_message_chunk")) {
                nc_json *content = nc_json_get(upd, "content");
                nc_str text = nc_json_str(nc_json_get(content, "text"), "");
                if (text.len > 0) {
                    size_t avail = out_cap - out_off - 1;
                    size_t cp = text.len < avail ? text.len : avail;
                    memcpy(out + out_off, text.ptr, cp);
                    out_off += cp;
                    out[out_off] = '\0';
                }
            } else if (nc_str_eql(utype, "message_delta")) {
                 nc_json *delta = nc_json_get(upd, "delta");
                 nc_str text = nc_json_str(nc_json_get(delta, "content"), "");
                 if (text.len > 0) {
                    size_t avail = out_cap - out_off - 1;
                    size_t cp = text.len < avail ? text.len : avail;
                    memcpy(out + out_off, text.ptr, cp);
                    out_off += cp;
                    out[out_off] = '\0';
                }
            }
        } else if (nc_str_eql(method, "session/request_permission")) {
            nc_json *jid = nc_json_get(root, "id");
            if (jid) acp_rpc_send_result(&s, (int)jid->number, "{\"granted\":true}");
        } else {
            nc_json *jid = nc_json_get(root, "id");
            if (jid && jid->type == NC_JSON_NUMBER && (int)jid->number == sess_prompt_id) {
                session_done = true;
            }
        }
    }

    nc_arena_free(&a);
    kill(s.pid, SIGTERM);
    waitpid(s.pid, NULL, 0);

    if (out_off == 0) nc_strlcpy(out, "ACP agent finished with no output. (Check permissions or model state)", out_cap);
    return true;
}

nc_tool nc_tool_acp_delegate(void) {
    return (nc_tool){
        .def = {
            .name = "acp_delegate",
            .description = "Delegate complex tasks to specialized agents (e.g. 'gemini', 'codex') via the Agent Client Protocol.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"The agent command, e.g. 'gemini' or 'codex'.\"},\"prompt\":{\"type\":\"string\",\"description\":\"The instructions for the sub-agent.\"}},\"required\":[\"command\",\"prompt\"]}",
        },
        .ctx = NULL,
        .execute = acp_delegate_execute,
        .free = NULL,
    };
}
