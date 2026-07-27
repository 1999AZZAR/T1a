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
    char soul_path[1024], user_path[1024], ident_path[1024];
    nc_path_join(soul_path, sizeof(soul_path), agent->config->config_dir, "SOUL.md");
    nc_path_join(user_path, sizeof(user_path), agent->config->config_dir, "USER.md");
    nc_path_join(ident_path, sizeof(ident_path), agent->config->config_dir, "IDENTITY.md");

    size_t s_len, u_len, i_len;
    char *soul = nc_read_file(soul_path, &s_len);
    char *user = nc_read_file(user_path, &u_len);
    char *ident = nc_read_file(ident_path, &i_len);

    snprintf(buf, cap, 
             "IDENTITY:\n%s\n\nSOUL: %s\n\nUSER: %s\n\n"
             "RULES:\n"
             "1. Answer from knowledge first. Tool only if needed.\n"
             "2. Never explain tool choice. Just call.\n"
             "3. Final answer only — no plans, no thinking aloud.\n"
             "4. Brevity mandatory. Zero fluff. Keyword-driven.\n"
             "5. sequentialthinking for complex multi-step reasoning.\n"
             "6. tavily_search for web, wikipedia_search for facts.\n"
             "7. guardian_memory persists context between conversations.",
             ident ? ident : "Minimalist command unit.",
             soul ? soul : "Helpful assistant.",
             user ? user : "Unknown user.");

    if (soul) free(soul);
    if (user) free(user);
    if (ident) free(ident);
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

/* ── Add message to history ───────────────────────────────────── */

static void agent_push_msg(nc_agent *agent, const char *role, const char *content,
                           const char *tool_call_id,
                           const nc_tool_call *tool_calls, int tool_call_count) {
    if (agent->message_count >= NC_MAX_MESSAGES) {
        int keep = NC_MAX_MESSAGES * 3 / 4;
        /* Shift messages to keep last 75% + system prompt at [0] */
        memmove(&agent->messages[1], &agent->messages[agent->message_count - keep],
                (size_t)keep * sizeof(nc_message));
        agent->message_count = 1 + keep;
        
        /* Trigger garbage collection to free space from dropped messages */
        agent_compact_memory(agent);
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

const char *nc_agent_chat(nc_agent *agent, const char *user_input) {
    agent_push_msg(agent, "user", user_input, NULL, NULL, 0);

    if (!agent->cached_tools_json)
        agent->cached_tools_json = build_tools_json(agent);

    const char *tools_json = agent->cached_tools_json;
    int max_iterations = 16;

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
            .model = agent->config->default_model,
            .temperature = agent->config->default_temperature,
            .tools_json = tools_json,
            .max_tokens = 8192,
        };

        nc_chat_response resp;
        if (!agent->provider->chat(agent->provider, &req, &resp)) {
            static const char err_msg[] = "error: communication failure with provider.";
            return nc_arena_dup(&agent->arena, err_msg, sizeof(err_msg) - 1);
        }

        if (!resp.has_tool_calls) {
            const char *reply = nc_arena_dup(&agent->arena, resp.content, strlen(resp.content));
            agent_push_msg(agent, "assistant", resp.content, NULL, NULL, 0);
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
            } else {
                snprintf(agent->tool_result_buf, agent->tool_result_cap,
                         "error: unknown tool '%s'", tc->name);
            }

            agent_push_msg(agent, "tool", agent->tool_result_buf, tc->id, NULL, 0);
        }
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
