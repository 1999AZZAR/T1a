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
    tools[n++] = nc_tool_hw_mpu6050();
    tools[n++] = nc_tool_hw_dht();
    tools[n++] = nc_tool_hw_buzzer();
    tools[n++] = nc_tool_hw_oled();
    tools[n++] = nc_tool_hw_servo();
    tools[n++] = nc_tool_hw_directio();
    tools[n++] = nc_tool_calc();
    tools[n++] = nc_tool_http_fetch();
    tools[n++] = nc_tool_list_dir(cfg);
    tools[n++] = nc_tool_env_get();
    tools[n++] = nc_tool_base64();
    tools[n++] = nc_tool_hash(cfg);
    tools[n++] = nc_tool_core_memory_append(cfg);
    tools[n++] = nc_tool_core_memory_replace(cfg);
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
    nc_path_join(mem_path, sizeof(mem_path), cfg.workspace_dir, "memory.jsonl");
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

typedef enum {
    S_WIZ_IDLE = 0,
    S_WIZ_GPIO_OP,
    S_WIZ_GPIO_PIN_W,
    S_WIZ_GPIO_VAL_W,
    S_WIZ_GPIO_PIN_R,
    S_WIZ_GPIO_MAP_NAME,
    S_WIZ_GPIO_MAP_PIN,
    S_WIZ_GPIO_UNMAP_NAME,
    S_WIZ_I2C_OP,
    S_WIZ_I2C_BUS_S,
    S_WIZ_I2C_BUS_R,
    S_WIZ_I2C_ADDR_R,
    S_WIZ_I2C_LEN_R,
    S_WIZ_I2C_BUS_W,
    S_WIZ_I2C_ADDR_W,
    S_WIZ_I2C_HEX_W,
    S_WIZ_I2C_MAP_NAME,
    S_WIZ_I2C_MAP_BUS,
    S_WIZ_I2C_MAP_ADDR,
    S_WIZ_I2C_UNMAP_NAME
} wizard_state_t;

static wizard_state_t s_wiz_state = S_WIZ_IDLE;
static char s_wiz_pin[32] = {0};
static char s_wiz_name[32] = {0};
static char s_wiz_bus[32] = {0};
static char s_wiz_addr[32] = {0};

bool nc_commands_execute(nc_agent *agent, const char *cmd, long chat_id, nc_channel *chan) {
    char reply[1024];
    char to_buf[32];
    snprintf(to_buf, sizeof(to_buf), "%ld", chat_id);

    if (s_wiz_state != S_WIZ_IDLE && cmd[0] != '/') {
        if (strcasecmp(cmd, "Cancel") == 0) {
            s_wiz_state = S_WIZ_IDLE;
            chan->send(chan, to_buf, "Cancelled.||KBD:{\"remove_keyboard\":true}");
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_OP) {
            if (strcmp(cmd, "Write") == 0) {
                s_wiz_state = S_WIZ_GPIO_PIN_W;
                chan->send(chan, to_buf, "Enter pin name or number:||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Read") == 0) {
                s_wiz_state = S_WIZ_GPIO_PIN_R;
                chan->send(chan, to_buf, "Enter pin name or number:||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Map") == 0) {
                s_wiz_state = S_WIZ_GPIO_MAP_NAME;
                chan->send(chan, to_buf, "Enter new alias name (e.g. LED):||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Unmap") == 0) {
                s_wiz_state = S_WIZ_GPIO_UNMAP_NAME;
                chan->send(chan, to_buf, "Enter alias name to remove:||KBD:{\"remove_keyboard\":true}");
            } else {
                s_wiz_state = S_WIZ_IDLE;
                chan->send(chan, to_buf, "Invalid operation. Cancelled.||KBD:{\"remove_keyboard\":true}");
            }
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_PIN_W) {
            nc_strlcpy(s_wiz_pin, cmd, sizeof(s_wiz_pin));
            s_wiz_state = S_WIZ_GPIO_VAL_W;
            chan->send(chan, to_buf, "Enter value (0 or 1):||KBD:{\"keyboard\":[[{\"text\":\"1\"},{\"text\":\"0\"}],[{\"text\":\"Cancel\"}]],\"resize_keyboard\":true,\"one_time_keyboard\":true}");
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_VAL_W) {
            char json[256];
            snprintf(json, sizeof(json), "{\"action\":\"write\",\"pin\":\"%s\",\"val\":\"%s\"}", s_wiz_pin, cmd);
            nc_tool t = nc_tool_hw_gpio();
            char hw_reply[2048];
            t.execute(&t, json, hw_reply, sizeof(hw_reply));
            s_wiz_state = S_WIZ_IDLE;
            char out[2048 + 64];
            snprintf(out, sizeof(out), "%s||KBD:{\"remove_keyboard\":true}", hw_reply);
            chan->send(chan, to_buf, out);
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_PIN_R) {
            char json[256];
            snprintf(json, sizeof(json), "{\"action\":\"read\",\"pin\":\"%s\"}", cmd);
            nc_tool t = nc_tool_hw_gpio();
            char hw_reply[2048];
            t.execute(&t, json, hw_reply, sizeof(hw_reply));
            s_wiz_state = S_WIZ_IDLE;
            char out[2048 + 64];
            snprintf(out, sizeof(out), "%s||KBD:{\"remove_keyboard\":true}", hw_reply);
            chan->send(chan, to_buf, out);
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_MAP_NAME) {
            nc_strlcpy(s_wiz_name, cmd, sizeof(s_wiz_name));
            s_wiz_state = S_WIZ_GPIO_MAP_PIN;
            chan->send(chan, to_buf, "Enter pin header/number to map to:");
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_MAP_PIN) {
            if (nc_gpio_set_alias(s_wiz_name, cmd)) {
                snprintf(reply, sizeof(reply), "success: mapped '%s' to pin '%s'", s_wiz_name, cmd);
            } else {
                snprintf(reply, sizeof(reply), "error: failed to map (invalid pin or table full)");
            }
            s_wiz_state = S_WIZ_IDLE;
            chan->send(chan, to_buf, reply);
            return true;
        }

        if (s_wiz_state == S_WIZ_GPIO_UNMAP_NAME) {
            if (nc_gpio_remove_alias(cmd)) {
                snprintf(reply, sizeof(reply), "success: removed mapping for '%s'", cmd);
            } else {
                snprintf(reply, sizeof(reply), "error: alias '%s' not found", cmd);
            }
            s_wiz_state = S_WIZ_IDLE;
            chan->send(chan, to_buf, reply);
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_OP) {
            if (strcmp(cmd, "Scan") == 0) {
                s_wiz_state = S_WIZ_I2C_BUS_S;
                chan->send(chan, to_buf, "Enter I2C bus number (e.g. 3):||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Read") == 0) {
                s_wiz_state = S_WIZ_I2C_BUS_R;
                chan->send(chan, to_buf, "Enter I2C bus number (or send alias name like OLED):||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Write") == 0) {
                s_wiz_state = S_WIZ_I2C_BUS_W;
                chan->send(chan, to_buf, "Enter I2C bus number (or send alias name like OLED):||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Map") == 0) {
                s_wiz_state = S_WIZ_I2C_MAP_NAME;
                chan->send(chan, to_buf, "Enter new alias name (e.g. OLED):||KBD:{\"remove_keyboard\":true}");
            } else if (strcmp(cmd, "Unmap") == 0) {
                s_wiz_state = S_WIZ_I2C_UNMAP_NAME;
                chan->send(chan, to_buf, "Enter alias name to remove:||KBD:{\"remove_keyboard\":true}");
            } else {
                s_wiz_state = S_WIZ_IDLE;
                chan->send(chan, to_buf, "Invalid operation. Cancelled.||KBD:{\"remove_keyboard\":true}");
            }
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_BUS_S) {
            char json[256];
            snprintf(json, sizeof(json), "{\"action\":\"scan\",\"bus\":%d}", atoi(cmd));
            nc_tool t = nc_tool_hw_i2c();
            char hw_reply[2048];
            t.execute(&t, json, hw_reply, sizeof(hw_reply));
            s_wiz_state = S_WIZ_IDLE;
            char out[2048 + 64];
            snprintf(out, sizeof(out), "%s||KBD:{\"remove_keyboard\":true}", hw_reply);
            chan->send(chan, to_buf, out);
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_BUS_R) {
            int resolved_bus, resolved_addr;
            if (nc_i2c_resolve_alias(cmd, &resolved_bus, &resolved_addr)) {
                snprintf(s_wiz_bus, sizeof(s_wiz_bus), "%d", resolved_bus);
                snprintf(s_wiz_addr, sizeof(s_wiz_addr), "%d", resolved_addr);
                s_wiz_state = S_WIZ_I2C_LEN_R;
                chan->send(chan, to_buf, "Alias found! Enter number of bytes to read:");
                return true;
            }
            nc_strlcpy(s_wiz_bus, cmd, sizeof(s_wiz_bus));
            s_wiz_state = S_WIZ_I2C_ADDR_R;
            chan->send(chan, to_buf, "Enter I2C device address (e.g. 0x3C):");
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_ADDR_R) {
            nc_strlcpy(s_wiz_addr, cmd, sizeof(s_wiz_addr));
            s_wiz_state = S_WIZ_I2C_LEN_R;
            chan->send(chan, to_buf, "Enter number of bytes to read:");
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_LEN_R) {
            char json[256];
            snprintf(json, sizeof(json), "{\"action\":\"read\",\"bus\":%d,\"addr\":%d,\"read_len\":%d}",
                atoi(s_wiz_bus), (int)strtol(s_wiz_addr, NULL, 0), atoi(cmd));
            nc_tool t = nc_tool_hw_i2c();
            char hw_reply[2048];
            t.execute(&t, json, hw_reply, sizeof(hw_reply));
            s_wiz_state = S_WIZ_IDLE;
            char out[2048 + 64];
            snprintf(out, sizeof(out), "%s||KBD:{\"remove_keyboard\":true}", hw_reply);
            chan->send(chan, to_buf, out);
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_BUS_W) {
            int resolved_bus, resolved_addr;
            if (nc_i2c_resolve_alias(cmd, &resolved_bus, &resolved_addr)) {
                snprintf(s_wiz_bus, sizeof(s_wiz_bus), "%d", resolved_bus);
                snprintf(s_wiz_addr, sizeof(s_wiz_addr), "%d", resolved_addr);
                s_wiz_state = S_WIZ_I2C_HEX_W;
                chan->send(chan, to_buf, "Alias found! Enter hex payload to write (e.g. A1B2):");
                return true;
            }
            nc_strlcpy(s_wiz_bus, cmd, sizeof(s_wiz_bus));
            s_wiz_state = S_WIZ_I2C_ADDR_W;
            chan->send(chan, to_buf, "Enter I2C device address (e.g. 0x3C):");
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_ADDR_W) {
            nc_strlcpy(s_wiz_addr, cmd, sizeof(s_wiz_addr));
            s_wiz_state = S_WIZ_I2C_HEX_W;
            chan->send(chan, to_buf, "Enter hex payload to write (e.g. A1B2):");
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_HEX_W) {
            /* sanitize: strip any double-quotes from hex payload to prevent JSON injection */
            char safe_hex[256] = {0};
            size_t hi = 0;
            for (size_t ci = 0; cmd[ci] && hi < sizeof(safe_hex) - 1; ci++) {
                if (cmd[ci] != '"' && cmd[ci] != '\\') safe_hex[hi++] = cmd[ci];
            }
            char json[512];
            snprintf(json, sizeof(json), "{\"action\":\"write\",\"bus\":%d,\"addr\":%d,\"data_hex\":\"%s\"}",
                atoi(s_wiz_bus), (int)strtol(s_wiz_addr, NULL, 0), safe_hex);
            nc_tool t = nc_tool_hw_i2c();
            char hw_reply[2048];
            t.execute(&t, json, hw_reply, sizeof(hw_reply));
            s_wiz_state = S_WIZ_IDLE;
            char out[2048 + 64];
            snprintf(out, sizeof(out), "%s||KBD:{\"remove_keyboard\":true}", hw_reply);
            chan->send(chan, to_buf, out);
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_MAP_NAME) {
            nc_strlcpy(s_wiz_name, cmd, sizeof(s_wiz_name));
            s_wiz_state = S_WIZ_I2C_MAP_BUS;
            chan->send(chan, to_buf, "Enter I2C bus number:");
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_MAP_BUS) {
            nc_strlcpy(s_wiz_bus, cmd, sizeof(s_wiz_bus));
            s_wiz_state = S_WIZ_I2C_MAP_ADDR;
            chan->send(chan, to_buf, "Enter I2C device address (e.g. 0x3C):");
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_MAP_ADDR) {
            if (nc_i2c_set_alias(s_wiz_name, atoi(s_wiz_bus), (int)strtol(cmd, NULL, 0))) {
                snprintf(reply, sizeof(reply), "success: mapped '%s' to bus %s addr %s", s_wiz_name, s_wiz_bus, cmd);
            } else {
                snprintf(reply, sizeof(reply), "error: failed to map");
            }
            s_wiz_state = S_WIZ_IDLE;
            chan->send(chan, to_buf, reply);
            return true;
        }

        if (s_wiz_state == S_WIZ_I2C_UNMAP_NAME) {
            if (nc_i2c_remove_alias(cmd)) {
                snprintf(reply, sizeof(reply), "success: removed mapping for '%s'", cmd);
            } else {
                snprintf(reply, sizeof(reply), "error: alias '%s' not found", cmd);
            }
            s_wiz_state = S_WIZ_IDLE;
            chan->send(chan, to_buf, reply);
            return true;
        }
    }

    if (cmd[0] != '/') return false;

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
            "/unmap_gpio <name>     - Remove alias\n"
            "/gpio                  - Interactive GPIO Menu\n"
            "/i2c                   - Interactive I2C Menu\n"
            "/set_gpio <pin> <val>  - Write GPIO\n"
            "/read_gpio <pin>       - Read GPIO\n"
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
    } else if (strncmp(cmd, "/unmap_gpio ", 12) == 0) {
        char name[32] = {0};
        sscanf(cmd + 12, "%31s", name);
        if (nc_gpio_remove_alias(name)) {
            snprintf(reply, sizeof(reply), "success: removed mapping for '%s'", name);
        } else {
            snprintf(reply, sizeof(reply), "error: alias '%s' not found", name);
        }
    } else if (strcmp(cmd, "/gpio") == 0) {
        s_wiz_state = S_WIZ_GPIO_OP;
        snprintf(reply, sizeof(reply), "Choose GPIO operation:||KBD:{\"keyboard\":[[{\"text\":\"Write\"},{\"text\":\"Read\"}],[{\"text\":\"Map\"},{\"text\":\"Unmap\"}],[{\"text\":\"Cancel\"}]],\"resize_keyboard\":true,\"one_time_keyboard\":true}");
    } else if (strcmp(cmd, "/i2c") == 0) {
        s_wiz_state = S_WIZ_I2C_OP;
        snprintf(reply, sizeof(reply), "Choose I2C operation:||KBD:{\"keyboard\":[[{\"text\":\"Scan\"}],[{\"text\":\"Write\"},{\"text\":\"Read\"}],[{\"text\":\"Map\"},{\"text\":\"Unmap\"}],[{\"text\":\"Cancel\"}]],\"resize_keyboard\":true,\"one_time_keyboard\":true}");
    } else if (strncmp(cmd, "/export_gpio ", 13) == 0) {
        char pin[32] = {0};
        sscanf(cmd + 13, "%31s", pin);
        char json[256];
        snprintf(json, sizeof(json), "{\"action\":\"export\",\"pin\":\"%s\"}", pin);
        nc_tool t = nc_tool_hw_gpio();
        t.execute(&t, json, reply, sizeof(reply));
    } else if (strncmp(cmd, "/unexport_gpio ", 15) == 0) {
        char pin[32] = {0};
        sscanf(cmd + 15, "%31s", pin);
        char json[256];
        snprintf(json, sizeof(json), "{\"action\":\"unexport\",\"pin\":\"%s\"}", pin);
        nc_tool t = nc_tool_hw_gpio();
        t.execute(&t, json, reply, sizeof(reply));
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
    } else if (strcmp(cmd, "/start") == 0) {
        char path[256];
        snprintf(path, sizeof(path), "%s/.t1a/workspace/chat.bin", getenv("HOME"));
        remove(path);
        agent->message_count = 1;

        nc_tool t = nc_tool_sys_info();
        char sys_info[1024] = {0};
        t.execute(&t, "{}", sys_info, sizeof(sys_info));

        nc_arena a;
        nc_arena_init(&a, 4096);
        nc_json *root = nc_json_parse(&a, sys_info, strlen(sys_info));

        nc_str hostname = nc_json_str(nc_json_get(root, "hostname"), "unknown");
        nc_str arch = nc_json_str(nc_json_get(root, "cpu_arch"), "unknown");
        nc_str load_avg = nc_json_str(nc_json_get(root, "load_avg"), "0.0 0.0 0.0");
        double uptime_days = nc_json_num(nc_json_get(root, "uptime_seconds"), 0) / 86400.0;
        double mem_free = nc_json_num(nc_json_get(root, "memory_available_kb"), 0) / (1024.0 * 1024.0);
        double mem_total = nc_json_num(nc_json_get(root, "memory_total_kb"), 0) / (1024.0 * 1024.0);
        double disk_free = nc_json_num(nc_json_get(root, "disk_available_kb"), 0) / (1024.0 * 1024.0);
        double disk_total = nc_json_num(nc_json_get(root, "disk_total_kb"), 0) / (1024.0 * 1024.0);

        snprintf(reply, sizeof(reply),
                 "```\nT1a v0.1.0 | C-Based AI Companion\n"
                 "─────────────────────────────────────────────\n"
                 "Host      : %.*s (%.*s)\n"
                 "Uptime    : %.1f days\n"
                 "Memory    : %.1f GB free / %.1f GB total\n"
                 "Storage   : %.1f GB free / %.1f GB total\n"
                 "Load Avg  : %.*s\n"
                 "─────────────────────────────────────────────\n"
                 "[OK] Context wiped. Awaiting command.\n```",
                 (int)hostname.len, hostname.ptr,
                 (int)arch.len, arch.ptr,
                 uptime_days, mem_free, mem_total, disk_free, disk_total,
                 (int)load_avg.len, load_avg.ptr);

        nc_arena_free(&a);
    } else if (strcmp(cmd, "/clear") == 0) {
        char path[256];
        snprintf(path, sizeof(path), "%s/.t1a/workspace/chat.bin", getenv("HOME"));
        remove(path);
        snprintf(path, sizeof(path), "%s/.t1a/workspace/core_memory.txt", getenv("HOME"));
        remove(path);
        snprintf(path, sizeof(path), "%s/.t1a/workspace/memory.jsonl", getenv("HOME"));
        remove(path);

        chan->send(chan, to_buf, "System Purge Activated.\nAll core memory, Guardian context, and chat history wiped.\nRestarting T1a daemon...");
        exit(0);
    } else {
        return false;
    }

    chan->send(chan, to_buf, reply);
    return true;
}
