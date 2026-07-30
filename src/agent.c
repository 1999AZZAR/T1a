/*
 * Agent loop: manages conversation history, dispatches tool calls,
 * and drives the provider in a loop until the LLM produces a final response.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ── Init / Free ──────────────────────────────────────────────── */

static void load_sys_prompt(nc_agent *agent, char *buf, size_t cap) {
    char soul_path[1024], user_path[1024], ident_path[1024], core_path[1024];
    nc_path_join(soul_path, sizeof(soul_path), agent->config->config_dir, "SOUL.md");
    nc_path_join(user_path, sizeof(user_path), agent->config->config_dir, "USER.md");
    nc_path_join(ident_path, sizeof(ident_path), agent->config->config_dir, "IDENTITY.md");
    nc_path_join(core_path, sizeof(core_path), agent->config->workspace_dir, "core_memory.txt");

    size_t s_len, u_len, i_len, c_len;
    char *soul = nc_read_file(soul_path, &s_len);
    char *user = nc_read_file(user_path, &u_len);
    char *ident = nc_read_file(ident_path, &i_len);
    char *core = nc_read_file(core_path, &c_len);

    char hw_info[128];
    nc_detect_hardware(hw_info, sizeof(hw_info));

    snprintf(buf, cap,
             "IDENTITY:\n%s\n\nSOUL: %s\n\nUSER: %s\n\n"
             "CORE MEMORY (Mutable Facts/Prefs):\n%s\n\n"
             "HARDWARE ENVIRONMENT:\n%s\n\n"
             "## ABSOLUTE RULES (violating these is a critical failure):\n"
             "1. NEVER think aloud in your response. NEVER write sentences like \"Let me...\", \"I need to...\", \"The user is asking...\", or \"Let me check...\". Output ONLY the final answer.\n"
             "2. DO NOT narrate what you are about to do. Just do it by calling the appropriate tool.\n"
             "3. If the answer requires fetching data (search, URL, time, calc, file, system), you MUST call the tool first. Then produce a short final answer from the tool result.\n"
             "4. Final answer: telegraphic, keyword-driven, no filler, no preamble. Drop pronouns. Use standard Markdown formatting (**bold**, *italic*, `code`).\n\n"
             "## TOOL AUTODETECT — MANDATORY (call immediately, no announcement):\n"
             "- URL in message -> call `http_fetch`\n"
             "- current events / news / recent facts -> call `tavily_search`\n"
             "- general knowledge / topics / facts -> call `wikipedia_search`\n"
             "- current time/date -> call `get_time`\n"
             "- math/calculation -> call `calc`\n"
             "- system/OS/CPU/hardware -> call `sys_info` or `shell`\n"
             "- file path in message -> call `file_read` or `list_dir`\n"
             "NEVER guess. ALWAYS fetch.",
             ident ? ident : "Minimalist command unit.",
             soul ? soul : "Helpful assistant.",
             user ? user : "Administrator.",
             core ? core : "No core memory.",
             hw_info);

    if (soul) free(soul);
    if (user) free(user);
    if (ident) free(ident);
    if (core) free(core);
}

static void agent_save_chat(nc_agent *agent) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.t1a/workspace/chat.bin", getenv("HOME"));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t magic = 0x4843434E;
    fwrite(&magic, 4, 1, f);
    uint32_t count = agent->message_count;
    fwrite(&count, 4, 1, f);
    for (uint32_t i = 0; i < count; i++) {
        nc_message *msg = &agent->messages[i];
        uint32_t rlen = msg->role ? strlen(msg->role) : 0;
        fwrite(&rlen, 4, 1, f);
        if (rlen) fwrite(msg->role, 1, rlen, f);
        uint32_t clen = msg->content ? strlen(msg->content) : 0;
        fwrite(&clen, 4, 1, f);
        if (clen) fwrite(msg->content, 1, clen, f);
    }
    fclose(f);
}

static void agent_load_chat(nc_agent *agent) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.t1a/workspace/chat.bin", getenv("HOME"));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x4843434E) { fclose(f); return; }
    uint32_t count = 0;
    if (fread(&count, 4, 1, f) != 1 || count > NC_MAX_MESSAGES) { fclose(f); return; }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rlen = 0, clen = 0;
        if (fread(&rlen, 4, 1, f) != 1) break;
        char *role = NULL, *content = NULL;
        if (rlen) {
            role = nc_arena_alloc(&agent->arena, rlen + 1);
            if (fread(role, 1, rlen, f) != rlen) break;
            role[rlen] = '\0';
        }
        if (fread(&clen, 4, 1, f) != 1) break;
        if (clen) {
            content = nc_arena_alloc(&agent->arena, clen + 1);
            if (fread(content, 1, clen, f) != clen) break;
            content[clen] = '\0';
        }
        if (i > 0 && role && content) {
            if (agent->message_count < NC_MAX_MESSAGES) {
                nc_message *msg = &agent->messages[agent->message_count++];
                memset(msg, 0, sizeof(*msg));
                msg->role = role;
                msg->content = content;
            }
        }
    }
    fclose(f);
}

void nc_agent_init(nc_agent *agent, nc_config *cfg, nc_provider *prov,
                   nc_tool *tools, int tool_count, nc_memory *mem) {
    memset(agent, 0, sizeof(*agent));
    agent->config = cfg;
    agent->provider = prov;
    agent->tools = tools;
    agent->tool_count = tool_count;
    agent->memory = mem;

    /* Increased arena for better reasoning on small SBCs */
    nc_arena_init(&agent->arena, 256 * 1024);

    char prompt_buf[12288];
    load_sys_prompt(agent, prompt_buf, sizeof(prompt_buf));

    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, "system", 6),
        .content = nc_arena_dup(&agent->arena, prompt_buf, strlen(prompt_buf)),
    };
    agent->message_count = 1;
    agent_load_chat(agent);
}

/* ── Build tools JSON for the provider ────────────────────────── */

static const char *build_tools_json(nc_agent *agent) {
    if (agent->tool_count == 0) return NULL;

    /* 256KB for tool definitions (MCP tools have large schemas) */
    static const size_t bufsz = 262144;
    char *buf = (char *)nc_arena_alloc(&agent->arena, bufsz);
    if (!buf) return NULL;
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");

    for (int i = 0; i < agent->tool_count; i++) {
        if (i > 0) off += snprintf(buf + off, bufsz - (size_t)off, ",");
        off += snprintf(buf + off, bufsz - (size_t)off,
            "{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"",
            agent->tools[i].def.name);
        /* Escape description (may contain newlines from MCP tools) */
        const char *desc = agent->tools[i].def.description;
        if (desc) {
            for (; *desc && (size_t)off < bufsz - 10; desc++) {
                switch (*desc) {
                    case '"':  buf[off++] = '\\'; buf[off++] = '"';  break;
                    case '\\': buf[off++] = '\\'; buf[off++] = '\\'; break;
                    case '\n': buf[off++] = '\\'; buf[off++] = 'n';  break;
                    case '\r': buf[off++] = '\\'; buf[off++] = 'r';  break;
                    case '\t': buf[off++] = '\\'; buf[off++] = 't';  break;
                    default:
                        if ((unsigned char)*desc >= 0x20)
                            buf[off++] = *desc;
                        break;
                }
            }
        }
        off += snprintf(buf + off, bufsz - (size_t)off,
            "\",\"parameters\":%s}}",
            agent->tools[i].def.parameters_json);
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]");
    return buf;
}

/* ── Memory Compaction ────────────────────────────────────────── */

/* Compacts the agent's arena by creating a new one and copying only active messages. */
static void agent_compact_memory(nc_agent *agent) {
    nc_log(NC_LOG_INFO, "Compacting memory arena...");

    nc_arena new_arena;
    nc_arena_init(&new_arena, 256 * 1024);

    /* Rebuild messages array in the new arena */
    for (int i = 0; i < agent->message_count; i++) {
        nc_message *msg = &agent->messages[i];

        msg->role = nc_arena_dup(&new_arena, msg->role, strlen(msg->role));
        if (msg->content)
            msg->content = nc_arena_dup(&new_arena, msg->content, strlen(msg->content));

        if (msg->tool_call_id)
            msg->tool_call_id = nc_arena_dup(&new_arena, msg->tool_call_id, strlen(msg->tool_call_id));

        if (msg->tool_calls && msg->tool_call_count > 0) {
            nc_tool_call *new_tcs = (nc_tool_call *)nc_arena_alloc(&new_arena,
                msg->tool_call_count * sizeof(nc_tool_call));
            if (new_tcs) {
                memcpy(new_tcs, msg->tool_calls, msg->tool_call_count * sizeof(nc_tool_call));
                msg->tool_calls = new_tcs;
            }
        }
    }

    nc_arena_free(&agent->arena);
    agent->arena = new_arena;
    agent->cached_tools_json = NULL;
}

int nc_agent_compact_context(nc_agent *agent) {
    if (!agent || agent->message_count <= 1) return 0;

    int old_count = agent->message_count;
    int history_count = old_count - 1;
    if (history_count < 4) {
        agent_compact_memory(agent);
        return 0;
    }

    int keep = history_count * 3 / 4;
    int start = old_count - keep;

    /* Never retain an orphaned assistant/tool sequence. */
    while (start < old_count && strcmp(agent->messages[start].role, "user") != 0)
        start++;

    if (start >= old_count) {
        agent_compact_memory(agent);
        return 0;
    }

    char *summary = NULL;
    if (agent->provider && agent->provider->chat && agent->config &&
        agent->config->small_model[0] && start > 1) {
        nc_message *summary_messages = calloc((size_t)start, sizeof(*summary_messages));
        if (summary_messages) {
            summary_messages[0] = (nc_message){
                .role = "system",
                .content = "Summarize the conversation context compactly. Preserve decisions, user preferences, unresolved tasks, and tool findings. Return only the summary.",
            };
            memcpy(&summary_messages[1], &agent->messages[1],
                (size_t)(start - 1) * sizeof(*summary_messages));

            nc_chat_request req = {
                .messages = summary_messages,
                .message_count = start,
                .model = agent->config->small_model,
                .temperature = 0.2,
                .tools_json = NULL,
                .max_tokens = 1024,
            };
            nc_chat_response resp;
            if (agent->provider->chat(agent->provider, &req, &resp) && resp.content[0])
                summary = strdup(resp.content);
            free(summary_messages);
        }
    }

    keep = old_count - start;
    int retained_at = summary ? 2 : 1;
    memmove(&agent->messages[retained_at], &agent->messages[start],
            (size_t)keep * sizeof(nc_message));
    if (summary) {
        agent->messages[1] = (nc_message){
            .role = "system",
            .content = summary,
        };
    }
    agent->message_count = retained_at + keep;
    agent_compact_memory(agent);
    agent_save_chat(agent);
    free(summary);
    return old_count - agent->message_count;
}

/* ── Add message to history ───────────────────────────────────── */

static void agent_push_msg(nc_agent *agent, const char *role, const char *content,
                           const char *tool_call_id,
                           const nc_tool_call *tool_calls, int tool_call_count) {
    if (agent->message_count >= NC_MAX_MESSAGES) {
        int removed = nc_agent_compact_context(agent);
        nc_log(NC_LOG_INFO, "Context limit reached: compacted %d messages", removed);
    }

    nc_message *msg = &agent->messages[agent->message_count++];
    memset(msg, 0, sizeof(*msg));
    msg->role = nc_arena_dup(&agent->arena, role, strlen(role));
    msg->content = content ? nc_arena_dup(&agent->arena, content, strlen(content)) : NULL;
    msg->tool_call_id = tool_call_id
        ? nc_arena_dup(&agent->arena, tool_call_id, strlen(tool_call_id))
        : NULL;

    if (tool_calls && tool_call_count > 0) {
        msg->tool_calls = (nc_tool_call *)nc_arena_alloc(
            &agent->arena, (size_t)tool_call_count * sizeof(nc_tool_call));
        if (msg->tool_calls) {
            memcpy(msg->tool_calls, tool_calls, (size_t)tool_call_count * sizeof(nc_tool_call));
            msg->tool_call_count = tool_call_count;
        }
    }
}

static nc_tool *find_tool(nc_agent *agent, const char *name) {
    for (int i = 0; i < agent->tool_count; i++) {
        if (strcmp(agent->tools[i].def.name, name) == 0)
            return &agent->tools[i];
    }
    return NULL;
}

/* ── Chat ── */

const char *nc_agent_chat(nc_agent *agent, const char *user_input, nc_stream_cb stream_cb, void *stream_user_data) {
    agent_push_msg(agent, "user", user_input, NULL, NULL, 0);

    if (!agent->cached_tools_json)
        agent->cached_tools_json = build_tools_json(agent);

    const char *tools_json = agent->cached_tools_json;
    int max_iterations = 16;
    bool use_small_model = false;
    bool finalizing = false;

    if (!agent->tool_result_buf) {
        agent->tool_result_cap = 262144;
        agent->tool_result_buf = (char *)malloc(agent->tool_result_cap);
        if (!agent->tool_result_buf) return "error: OOM allocating tool buffer";
    }

    for (int iter = 0; iter < max_iterations; iter++) {
        if (iter == max_iterations - 1) {
            agent_push_msg(agent, "system",
                "System: Maximum tool iteration limit reached. "
                "Provide a final response based on information gathered so far.",
                NULL, NULL, 0);
            tools_json = NULL;
        }

        nc_chat_request req = {
            .messages = agent->messages,
            .message_count = agent->message_count,
            .model = use_small_model ? agent->config->small_model : agent->config->default_model,
            .temperature = agent->config->default_temperature,
            .tools_json = tools_json,
            .max_tokens = 8192,
            .stream_cb = (!use_small_model) ? stream_cb : NULL,
            .stream_user_data = stream_user_data,
        };

        nc_log(NC_LOG_DEBUG, "Inference lane: %s (%s)",
            use_small_model ? "small" : "main", req.model);
        nc_chat_response resp;
        if (!agent->provider->chat(agent->provider, &req, &resp)) {
            static const char err_msg[] = "error: communication failure with provider.";
            return nc_arena_dup(&agent->arena, err_msg, sizeof(err_msg) - 1);
        }

        if (!resp.has_tool_calls) {
            if (use_small_model && !finalizing) {
                agent_push_msg(agent, "system",
                    "Tool work is complete. Produce the final user-facing answer from the gathered results. Do not call more tools.",
                    NULL, NULL, 0);
                use_small_model = false;
                finalizing = true;
                tools_json = NULL;
                continue;
            }
            char clean_resp[8192];
            size_t c_len = 0;
            char *p = resp.content;
            while (*p) {
                char *start = strstr(p, "<think>");
                if (start) {
                    size_t n = start - p;
                    if (n > 0 && c_len + n < sizeof(clean_resp)) {
                        memcpy(clean_resp + c_len, p, n);
                        c_len += n;
                    }
                    char *end = strstr(start + 7, "</think>");
                    if (end) {
                        p = end + 8;
                    } else {
                        p = start + strlen(start);
                        break;
                    }
                } else {
                    size_t n = strlen(p);
                    if (c_len + n < sizeof(clean_resp)) {
                        memcpy(clean_resp + c_len, p, n);
                        c_len += n;
                    }
                    break;
                }
            }
            clean_resp[c_len] = '\0';

            /* Strip leading/trailing whitespace */
            char *start_ptr = clean_resp;
            while (*start_ptr == ' ' || *start_ptr == '\n' || *start_ptr == '\r') start_ptr++;
            char *end_ptr = clean_resp + strlen(clean_resp) - 1;
            while (end_ptr > start_ptr && (*end_ptr == ' ' || *end_ptr == '\n' || *end_ptr == '\r')) *end_ptr-- = '\0';

            if (!start_ptr[0]) {
                /* If everything was inside <think>, the model probably put its answer there. */
                /* Let's just use the raw response but strip the literal tags to avoid ugliness */
                c_len = 0;
                p = resp.content;
                while (*p) {
                    if (strncmp(p, "<think>", 7) == 0) {
                        p += 7;
                    } else if (strncmp(p, "</think>", 8) == 0) {
                        p += 8;
                    } else {
                        if (c_len < sizeof(clean_resp) - 1) {
                            clean_resp[c_len++] = *p;
                        }
                        p++;
                    }
                }
                clean_resp[c_len] = '\0';

                start_ptr = clean_resp;
                while (*start_ptr == ' ' || *start_ptr == '\n' || *start_ptr == '\r') start_ptr++;
                end_ptr = clean_resp + strlen(clean_resp) - 1;
                while (end_ptr > start_ptr && (*end_ptr == ' ' || *end_ptr == '\n' || *end_ptr == '\r')) *end_ptr-- = '\0';

                if (!start_ptr[0]) {
                    start_ptr = "Acknowledged.";
                }
            }

            /* If the reply looks like model reasoning prose, try to extract only
               the last paragraph which is usually the actual answer */
            static const char *think_patterns[] = {
                "The user is asking", "Let me ", "I need to ", "I should ",
                "I'll ", "I will ", "Let me check", "Let me search",
                "First, I", "First let", NULL
            };
            bool is_thinking_prose = false;
            for (int pi = 0; think_patterns[pi]; pi++) {
                if (strncmp(start_ptr, think_patterns[pi], strlen(think_patterns[pi])) == 0) {
                    is_thinking_prose = true;
                    break;
                }
            }
            if (is_thinking_prose) {
                /* Find the last double-newline separated paragraph as the real answer */
                char *last_para = start_ptr;
                char *scan = start_ptr;
                while (*scan) {
                    if (scan[0] == '\n' && scan[1] == '\n') {
                        char *candidate = scan + 2;
                        while (*candidate == '\n' || *candidate == ' ') candidate++;
                        if (*candidate) last_para = candidate;
                    }
                    scan++;
                }
                /* If last paragraph also looks like thinking, use full text (better than nothing) */
                bool last_is_thinking = false;
                for (int pi = 0; think_patterns[pi]; pi++) {
                    if (strncmp(last_para, think_patterns[pi], strlen(think_patterns[pi])) == 0) {
                        last_is_thinking = true;
                        break;
                    }
                }
                if (!last_is_thinking) start_ptr = last_para;
            }

            const char *reply = nc_arena_dup(&agent->arena, start_ptr, strlen(start_ptr));
            agent_push_msg(agent, "assistant", start_ptr, NULL, NULL, 0);
            agent_save_chat(agent);
            return reply;
        }

        nc_log(NC_LOG_INFO, "T1a executing %d tools (iteration %d)", resp.tool_call_count, iter + 1);

        agent_push_msg(agent, "assistant",
                       resp.content[0] ? resp.content : NULL,
                       NULL,
                       resp.tool_calls, resp.tool_call_count);

        for (int i = 0; i < resp.tool_call_count; i++) {
            nc_tool_call *tc = &resp.tool_calls[i];
            nc_tool *tool = find_tool(agent, tc->name);

            if (agent->config->max_actions_per_hour > 0) {
                time_t now = time(NULL);
                if (now - agent->hour_window_start >= 3600) {
                    agent->hour_window_start = now;
                    agent->actions_this_hour = 0;
                }
                if (agent->actions_this_hour >= agent->config->max_actions_per_hour) {
                    snprintf(agent->tool_result_buf, agent->tool_result_cap,
                        "error: rate limit exceeded (%d actions/hour)",
                        agent->config->max_actions_per_hour);
                    agent_push_msg(agent, "tool", agent->tool_result_buf, tc->id, NULL, 0);
                    continue;
                }
                agent->actions_this_hour++;
            }

            agent->tool_result_buf[0] = '\0';
            if (tool) {
                tool->execute(tool, tc->arguments,
                              agent->tool_result_buf, agent->tool_result_cap);

                size_t res_len = strlen(agent->tool_result_buf);
                if (res_len > 4096) {
                    const char trunc_msg[] = "\n...[output truncated at 4KB]";
                    size_t t_len = sizeof(trunc_msg) - 1;
                    agent->tool_result_buf[4096 - t_len] = '\0';
                    strcat(agent->tool_result_buf, trunc_msg);
                }
            } else {
                snprintf(agent->tool_result_buf, agent->tool_result_cap,
                         "error: unknown tool '%s'", tc->name);
            }

            agent_push_msg(agent, "tool", agent->tool_result_buf, tc->id, NULL, 0);
        }
        use_small_model = true;
        finalizing = false;
    }

    return "error: maximum autonomy iterations reached.";
}

void nc_agent_reset(nc_agent *agent) {
    size_t role_len = strlen(agent->messages[0].role);
    size_t content_len = strlen(agent->messages[0].content);

    char *role_copy = (char *)malloc(role_len + 1);
    char *content_copy = (char *)malloc(content_len + 1);
    if (!role_copy || !content_copy) {
        free(role_copy);
        free(content_copy);
        return;
    }
    memcpy(role_copy, agent->messages[0].role, role_len + 1);
    memcpy(content_copy, agent->messages[0].content, content_len + 1);

    nc_arena_reset(&agent->arena);
    agent->cached_tools_json = NULL;

    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, role_copy, role_len),
        .content = nc_arena_dup(&agent->arena, content_copy, content_len),
    };
    agent->message_count = 1;

    free(role_copy);
    free(content_copy);
}

void nc_agent_free(nc_agent *agent) {
    free(agent->tool_result_buf);
    agent->tool_result_buf = NULL;
    agent->cached_tools_json = NULL;
    nc_arena_free(&agent->arena);
    agent->message_count = 0;
}

#ifdef NC_TEST
typedef struct {
    int call_count;
    char models[3][128];
} agent_test_provider_ctx;

static bool agent_test_chat(nc_provider *self, const nc_chat_request *req,
                            nc_chat_response *resp) {
    agent_test_provider_ctx *ctx = (agent_test_provider_ctx *)self->ctx;
    int call = ctx->call_count++;
    if (call < 3)
        nc_strlcpy(ctx->models[call], req->model, sizeof(ctx->models[call]));
    memset(resp, 0, sizeof(*resp));

    if (call == 0) {
        resp->has_tool_calls = true;
        resp->tool_call_count = 1;
        nc_strlcpy(resp->tool_calls[0].id, "call_1", sizeof(resp->tool_calls[0].id));
        nc_strlcpy(resp->tool_calls[0].name, "mock_tool", sizeof(resp->tool_calls[0].name));
        nc_strlcpy(resp->tool_calls[0].arguments, "{}", sizeof(resp->tool_calls[0].arguments));
    } else if (call == 1) {
        nc_strlcpy(resp->content, "small draft", sizeof(resp->content));
    } else {
        nc_strlcpy(resp->content, "main final", sizeof(resp->content));
    }
    return true;
}

static bool agent_test_tool(nc_tool *self, const char *args, char *out, size_t cap) {
    nc_strlcpy(out, "tool result", cap);
    return true;
}

void nc_test_agent_context(void) {
    nc_agent agent;
    memset(&agent, 0, sizeof(agent));
    nc_arena_init(&agent.arena, 64 * 1024);

    agent.messages[0].role = nc_arena_dup(&agent.arena, "system", 6);
    agent.messages[0].content = nc_arena_dup(&agent.arena, "prompt", 6);
    agent.message_count = NC_MAX_MESSAGES;

    for (int i = 1; i < NC_MAX_MESSAGES; i++) {
        const char *role = i % 2 ? "user" : "assistant";
        agent.messages[i].role = nc_arena_dup(&agent.arena, role, strlen(role));
        agent.messages[i].content = nc_arena_dup(&agent.arena, "message", 7);
    }

    int removed = nc_agent_compact_context(&agent);
    NC_ASSERT(removed > 0, "context compaction removes old messages");
    NC_ASSERT(agent.message_count <= NC_MAX_MESSAGES * 3 / 4,
        "context compaction keeps at most 75 percent");
    NC_ASSERT(strcmp(agent.messages[0].role, "system") == 0,
        "context compaction preserves system prompt");
    NC_ASSERT(strcmp(agent.messages[1].role, "user") == 0,
        "context compaction starts at user boundary");

    nc_agent_free(&agent);

    nc_config cfg;
    nc_config_defaults(&cfg);
    agent_test_provider_ctx provider_ctx = {0};
    nc_provider provider = {
        .name = "mock",
        .ctx = &provider_ctx,
        .chat = agent_test_chat,
        .free = NULL,
    };
    nc_tool tool = {
        .def = { .name = "mock_tool", .description = "test", .parameters_json = "{}" },
        .execute = agent_test_tool,
    };
    nc_memory memory = nc_memory_noop();
    nc_agent_init(&agent, &cfg, &provider, &tool, 1, &memory);
    const char *reply = nc_agent_chat(&agent, "test routing", NULL, NULL);

    NC_ASSERT(provider_ctx.call_count == 3, "model lanes perform three-stage tool flow");
    NC_ASSERT(strcmp(provider_ctx.models[0], cfg.default_model) == 0, "initial request uses main model");
    NC_ASSERT(strcmp(provider_ctx.models[1], cfg.small_model) == 0, "tool continuation uses small model");
    NC_ASSERT(strcmp(provider_ctx.models[2], cfg.default_model) == 0, "final response uses main model");
    NC_ASSERT(strcmp(reply, "main final") == 0, "small draft is not user-visible");
    nc_agent_free(&agent);
}
#endif
