#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int nc_register_default_tools(nc_tool *tools, const nc_config *cfg, nc_memory *mem) {
    int n = 0;
    tools[n++] = nc_tool_shell(cfg);
    tools[n++] = nc_tool_file_read(cfg);
    tools[n++] = nc_tool_file_write(cfg);
    tools[n++] = nc_tool_memory_store(mem);
    tools[n++] = nc_tool_memory_recall(mem);
    tools[n++] = nc_tool_get_time();
    tools[n++] = nc_tool_sys_info();
    tools[n++] = nc_tool_hw_gpio();
    tools[n++] = nc_tool_hw_i2c();
    tools[n++] = nc_tool_calc();
    tools[n++] = nc_tool_http_fetch();
    tools[n++] = nc_tool_list_dir(cfg);
    tools[n++] = nc_tool_env_get();
    tools[n++] = nc_tool_base64();
    tools[n++] = nc_tool_hash(cfg);
    tools[n++] = nc_tool_core_memory_append(cfg);
    tools[n++] = nc_tool_core_memory_replace(cfg);
    tools[n++] = nc_tool_acp_delegate();
    /* Built-in MCP tools (replace external Node.js MCP servers) */
    tools[n++] = nc_tool_reasoning();
    tools[n++] = nc_tool_tavily_search(getenv("TAVILY_API_KEY"));
    tools[n++] = nc_tool_wikipedia_search();
    tools[n++] = nc_tool_guardian_memory(mem->ctx);
    /* External MCP servers (still available if configured) */
    n = nc_mcp_register_all(cfg, tools, n);
    return n;
}

int nc_cmd_agent(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    nc_config_load(&cfg);
    nc_config_apply_env(&cfg);

    nc_log_min_level = NC_LOG_INFO;

    const char *chan_name = "cli";
    const char *msg_arg = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            chan_name = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            msg_arg = argv[++i];
        }
    }

    nc_provider prov = nc_provider_from_config(&cfg);

    char mem_path[1024];
    nc_path_join(mem_path, sizeof(mem_path), cfg.workspace_dir, "guardian.jsonl");
    nc_memory mem = nc_memory_guardian(mem_path);
    nc_tool tools[NC_MAX_TOOLS];
    int tool_count = nc_register_default_tools(tools, &cfg, &mem);

    nc_agent agent;
    nc_agent_init(&agent, &cfg, &prov, tools, tool_count, &mem);

    if (msg_arg) {
        printf("%s\n", nc_agent_chat(&agent, msg_arg, NULL, NULL));
        nc_agent_free(&agent);
        mem.free(&mem);
        if (prov.free) prov.free(&prov);
        return 0;
    }

    nc_channel ch;
    if (strcmp(chan_name, "telegram") == 0) {
        /* Priority to ENV, then config file */
        const char *env_token = getenv("NOCLAW_TELEGRAM_TOKEN");
        const char *token = (env_token && env_token[0]) ? env_token : cfg.telegram_token;
        ch = nc_channel_telegram(token);
    } else {
        ch = nc_channel_cli();
    }

    nc_log(NC_LOG_INFO, "T1a v%s -- %s mode", NC_VERSION, chan_name);
    nc_log(NC_LOG_INFO, "  Provider: %s", cfg.default_provider);
    nc_log(NC_LOG_INFO, "  Main:     %s", cfg.default_model);
    nc_log(NC_LOG_INFO, "  Small:    %s", cfg.small_model);
    if (cfg.fallback_provider[0])
        nc_log(NC_LOG_INFO, "  Fallback: %s/%s", cfg.fallback_provider, cfg.fallback_model);
    nc_log(NC_LOG_INFO, "  Tools:    %d loaded", tool_count);

    while (1) {
        ch.poll(&ch, &agent);
        usleep(50000);
    }

    return 0;
}

bool nc_commands_execute(nc_agent *agent, const char *cmd, long chat_id, nc_channel *chan) {
    if (cmd[0] != '/') return false;

    char reply[1024];
    char to_buf[32];
    snprintf(to_buf, sizeof(to_buf), "%ld", chat_id);

    if (strcmp(cmd, "/status") == 0) {
        snprintf(reply, sizeof(reply),
            "T1a Unit Status\n\n"
            "- Main model: %s\n"
            "- Small model: %s\n"
            "- Tools: %d active\n"
            "- Memory: %s\n"
            "- Uptime: Stable",
            agent->config->default_model, agent->config->small_model,
            agent->tool_count, agent->config->memory_backend);
    } else if (strcmp(cmd, "/restart") == 0) {
        chan->send(chan, to_buf, "Restarting T1a binary...");
        exit(0);
    } else if (strcmp(cmd, "/reset") == 0) {
        nc_agent_reset(agent);
        snprintf(reply, sizeof(reply), "Conversation reset. Brain is fresh now.");
    } else if (strcmp(cmd, "/compact") == 0) {
        int removed = nc_agent_compact_context(agent);
        if (removed > 0)
            snprintf(reply, sizeof(reply), "Context compacted. Removed %d old messages; %d remain.",
                removed, agent->message_count - 1);
        else
            snprintf(reply, sizeof(reply), "Context is already compact.");
    } else if (strcmp(cmd, "/help") == 0) {
        snprintf(reply, sizeof(reply),
            "🤖 *T1a Unit Commands*\n\n"
            "/status  - Show unit health\n"
            "/reset   - Clear chat history\n"
            "/compact - Trim oldest context\n"
            "/restart - Force binary reboot\n"
            "/map_gpio <name> <pin> - Map alias\n"
            "/set_gpio <pin> <val> - Write GPIO\n"
            "/read_gpio <pin>      - Read GPIO\n"
            "/i2c_scan <bus>       - Scan I2C bus\n"
            "/i2c_read <bus> <addr> <len>\n"
            "/i2c_write <bus> <addr> <hex>\n"
            "/help    - Show this list\n\n"
            "🛠 *Auto-Detected Abilities:*\n"
            "- Web browsing & search\n"
            "- Hardware GPIO & I2C control\n"
            "- Persistent memory (guardian)");
    } else if (strncmp(cmd, "/map_gpio ", 10) == 0) {
        char name[32] = {0}, pin_str[32] = {0};
        sscanf(cmd + 10, "%31s %31s", name, pin_str);
        if (nc_gpio_set_alias(name, pin_str)) {
            snprintf(reply, sizeof(reply), "success: mapped '%s' to pin '%s'", name, pin_str);
        } else {
            snprintf(reply, sizeof(reply), "error: failed to map (invalid pin or table full)");
        }
    } else if (strncmp(cmd, "/set_gpio ", 10) == 0) {
        char pin[32] = {0}, val[32] = {0};
        sscanf(cmd + 10, "%31s %31s", pin, val);
        char json[256];
        snprintf(json, sizeof(json), "{\"action\":\"write\",\"pin\":\"%s\",\"val\":\"%s\"}", pin, val);
        nc_tool t = nc_tool_hw_gpio();
        t.execute(&t, json, reply, sizeof(reply));
    } else if (strncmp(cmd, "/read_gpio ", 11) == 0) {
        char pin[32] = {0};
        sscanf(cmd + 11, "%31s", pin);
        char json[256];
        snprintf(json, sizeof(json), "{\"action\":\"read\",\"pin\":\"%s\"}", pin);
        nc_tool t = nc_tool_hw_gpio();
        t.execute(&t, json, reply, sizeof(reply));
    } else if (strncmp(cmd, "/i2c_scan ", 10) == 0) {
        char bus[32] = {0};
        sscanf(cmd + 10, "%31s", bus);
        char json[256];
        snprintf(json, sizeof(json), "{\"action\":\"scan\",\"bus\":%d}", atoi(bus));
        nc_tool t = nc_tool_hw_i2c();
        t.execute(&t, json, reply, sizeof(reply));
    } else if (strncmp(cmd, "/i2c_read ", 10) == 0) {
        char bus[32] = {0}, addr[32] = {0}, len[32] = {0};
        sscanf(cmd + 10, "%31s %31s %31s", bus, addr, len);
        char json[256];
        snprintf(json, sizeof(json), "{\"action\":\"read\",\"bus\":%d,\"addr\":%d,\"read_len\":%d}", atoi(bus), (int)strtol(addr,NULL,0), atoi(len));
        nc_tool t = nc_tool_hw_i2c();
        t.execute(&t, json, reply, sizeof(reply));
    } else if (strncmp(cmd, "/i2c_write ", 11) == 0) {
        char bus[32] = {0}, addr[32] = {0}, hex[256] = {0};
        sscanf(cmd + 11, "%31s %31s %255s", bus, addr, hex);
        char json[512];
        snprintf(json, sizeof(json), "{\"action\":\"write\",\"bus\":%d,\"addr\":%d,\"data_hex\":\"%s\"}", atoi(bus), (int)strtol(addr,NULL,0), hex);
        nc_tool t = nc_tool_hw_i2c();
        t.execute(&t, json, reply, sizeof(reply));
    } else {
        return false;
    }

    chan->send(chan, to_buf, reply);
    return true;
}
