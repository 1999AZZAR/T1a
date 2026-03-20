#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int nc_cmd_gateway(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    nc_config_load(&cfg);
    nc_config_apply_env(&cfg);

    nc_provider prov = nc_provider_from_config(&cfg, false);
    if (cfg.fallback_provider[0] && cfg.fallback_api_key[0]) {
        nc_provider fallback = nc_provider_from_config(&cfg, true);
        prov = nc_provider_chain(prov, fallback, cfg.fallback_model);
        nc_log(NC_LOG_INFO, "  Fallback: %s (%s)", cfg.fallback_provider, 
               cfg.fallback_model[0] ? cfg.fallback_model : "default model");
    }

    char mem_path[1024];
    nc_path_join(mem_path, sizeof(mem_path), cfg.workspace_dir, "memories.tsv");
    nc_memory mem = nc_memory_flat(mem_path);
    nc_tool tools[NC_MAX_TOOLS];
    int tool_count = nc_register_default_tools(tools, &cfg, &mem);

    nc_agent agent;
    nc_agent_init(&agent, &cfg, &prov, tools, tool_count, &mem);

    nc_gateway gw;
    nc_gateway_init(&gw, &cfg, &agent);
    
    nc_log(NC_LOG_INFO, "Gateway starting on %s:%d", cfg.gateway_host, cfg.gateway_port);
    nc_gateway_run(&gw);
    return 0;
}

int nc_cmd_status(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    nc_config_load(&cfg);
    printf("noclaw Unit Status\n");
    printf("  Version:  %s\n", NC_VERSION);
    printf("  Model:    %s\n", cfg.default_model);
    printf("  Provider: %s\n", cfg.default_provider);
    return 0;
}

int nc_cmd_onboard(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
            nc_strlcpy(cfg.api_key, argv[++i], sizeof(cfg.api_key));
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            nc_strlcpy(cfg.default_provider, argv[++i], sizeof(cfg.default_provider));
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            nc_strlcpy(cfg.default_model, argv[++i], sizeof(cfg.default_model));
        }
    }

    nc_mkdir_p(cfg.config_dir);
    nc_mkdir_p(cfg.workspace_dir);
    nc_config_save(&cfg);

    printf("Onboarding complete.\n");
    printf("  Config:    %s\n", cfg.config_path);
    printf("  Workspace: %s\n", cfg.workspace_dir);
    printf("  Provider:  %s\n", cfg.default_provider);
    if (cfg.api_key[0])
        printf("  API Key:   set (%zu chars)\n", strlen(cfg.api_key));
    else
        printf("  API Key:   NOT SET (export NOCLAW_API_KEY or re-run with --api-key)\n");
    return 0;
}

int nc_cmd_doctor(int argc, char **argv) {
    (void)argc; (void)argv;

    nc_config cfg;
    nc_config_defaults(&cfg);
    bool cfg_ok = nc_config_load(&cfg);
    nc_config_apply_env(&cfg);

    int issues = 0;
    printf("noclaw Doctor\n");
    printf("=============\n\n");

    printf("  Version:   %s\n", NC_VERSION);

    if (cfg_ok) {
        printf("  Config:    %s  [OK]\n", cfg.config_path);
    } else {
        printf("  Config:    %s  [MISSING]\n", cfg.config_path);
        printf("             Run `noclaw onboard` to create.\n");
        issues++;
    }

    if (nc_file_exists(cfg.workspace_dir)) {
        printf("  Workspace: %s  [OK]\n", cfg.workspace_dir);
    } else {
        printf("  Workspace: %s  [MISSING]\n", cfg.workspace_dir);
        issues++;
    }

    if (cfg.api_key[0]) {
        printf("  API Key:   set (%zu chars)  [OK]\n", strlen(cfg.api_key));
    } else {
        printf("  API Key:   NOT SET  [FAIL]\n");
        issues++;
    }

    printf("  Provider:  %s\n", cfg.default_provider);
    printf("  Model:     %s\n", cfg.default_model);

    char mem_path[1024];
    nc_path_join(mem_path, sizeof(mem_path), cfg.workspace_dir, "memories.tsv");
    if (nc_file_exists(mem_path)) {
        printf("  Memory:    %s  [OK]\n", mem_path);
    } else {
        printf("  Memory:    %s  [NOT YET CREATED]\n", mem_path);
    }

    char mcp_path[1024];
    nc_path_join(mcp_path, sizeof(mcp_path), cfg.config_dir, "mcp.json");
    if (nc_file_exists(mcp_path)) {
        printf("  MCP:       %s  [OK]\n", mcp_path);
    } else {
        printf("  MCP:       %s  [NOT FOUND]\n", mcp_path);
    }

    char soul_path[1024];
    nc_path_join(soul_path, sizeof(soul_path), cfg.config_dir, "SOUL.md");
    printf("  SOUL.md:   %s\n", nc_file_exists(soul_path) ? "present" : "not found (using default)");

    printf("\n");
    if (issues == 0)
        printf("All checks passed.\n");
    else
        printf("%d issue(s) found.\n", issues);

    return issues > 0 ? 1 : 0;
}
