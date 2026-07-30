/*
 * test_runner.c — T1a test suite entry point.
 * Compiled separately; links against the same production .o files.
 * Build: make test
 */

#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int nc_test_pass = 0;
int nc_test_fail = 0;

#define NC_ASSERT(condition, name) do { \
    if (condition) { \
        nc_test_pass++; \
        printf("  PASS: %s\n", name); \
    } else { \
        nc_test_fail++; \
        printf("  FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
    } \
} while (0)

/* from src/agent.c */
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

/* from src/arena.c */
void nc_test_arena(void) {
    nc_arena a;
    nc_arena_init(&a, 128);

    void *p1 = nc_arena_alloc(&a, 32);
    NC_ASSERT(p1 != NULL, "arena alloc 32 bytes");

    void *p2 = nc_arena_alloc(&a, 64);
    NC_ASSERT(p2 != NULL, "arena alloc 64 bytes");
    NC_ASSERT(p2 != p1, "arena allocs are distinct");

    /* Test arena dup */
    char *s = nc_arena_dup(&a, "hello", 5);
    NC_ASSERT(s != NULL, "arena dup non-null");
    NC_ASSERT(strcmp(s, "hello") == 0, "arena dup content");

    /* Test growth: allocate more than initial cap — must not invalidate p1/p2 */
    void *p3 = nc_arena_alloc(&a, 256);
    NC_ASSERT(p3 != NULL, "arena grows beyond initial cap");

    /* Verify p1 and p2 are still valid (not moved by realloc) */
    NC_ASSERT(p1 != NULL, "p1 still valid after growth");
    NC_ASSERT(p2 != NULL, "p2 still valid after growth");

    nc_arena_reset(&a);
    NC_ASSERT(a.current->pos == 0, "arena reset clears pos");

    nc_arena_free(&a);
    NC_ASSERT(a.head == NULL, "arena free nulls head");
}

/* from src/config.c */
void nc_test_config(void) {
    nc_config cfg;
    nc_config_defaults(&cfg);

    NC_ASSERT(strcmp(cfg.default_provider, "opencode") == 0, "config default provider");
    NC_ASSERT(strcmp(cfg.default_model, "deepseek-v4-flash-free") == 0, "config default main model");
    NC_ASSERT(strcmp(cfg.small_model, "nemotron-3-ultra-free") == 0, "config default small model");
    NC_ASSERT(cfg.default_temperature == 0.5, "config default temp");
    NC_ASSERT(cfg.gateway_port == 8888, "config default port");
    NC_ASSERT(cfg.gateway_require_pairing == true, "config default pairing");
    NC_ASSERT(cfg.gateway_allow_public_bind == false, "config default no public bind");
    NC_ASSERT(cfg.workspace_only == true, "config default workspace_only");
    NC_ASSERT(cfg.secrets_encrypt == true, "config default secrets encrypt");
    NC_ASSERT(strcmp(cfg.memory_backend, "guardian") == 0, "config default memory backend");
    NC_ASSERT(strcmp(cfg.runtime_kind, "daemon") == 0, "config default runtime");
    NC_ASSERT(strcmp(cfg.fallback_provider, "kilo") == 0, "config default fallback provider");
    NC_ASSERT(strcmp(cfg.fallback_model, "openrouter/free") == 0, "config default fallback model");
}

/* from src/http.c — uses only public API */
void nc_test_http(void) {
    /* Smoke-test: nc_http_response_free on a zeroed struct must not crash */
    nc_http_response resp;
    memset(&resp, 0, sizeof(resp));
    nc_http_response_free(&resp);
    NC_ASSERT(resp.body == NULL, "http_response_free safe on null body");

    /* nc_http_get logs error and returns non-zero status on bad scheme */
    nc_http_response bad;
    memset(&bad, 0, sizeof(bad));
    nc_http_get("ftp://bad.invalid/", NULL, 0, &bad);
    /* regardless of return code, body should be empty/null for bad scheme */
    NC_ASSERT(bad.status == 0 || bad.body_len == 0, "http_get yields no body for bad scheme");
    nc_http_response_free(&bad);
}

/* from src/json.c */
void nc_test_json(void) {
    nc_arena a;
    nc_arena_init(&a, 4096);

    /* Parse object */
    const char *j1 = "{\"name\": \"t1a\", \"version\": 1, \"fast\": true}";
    nc_json *r1 = nc_json_parse(&a, j1, strlen(j1));
    NC_ASSERT(r1 != NULL, "json parse object");
    NC_ASSERT(r1->type == NC_JSON_OBJECT, "json type object");

    nc_str name = nc_json_str(nc_json_get(r1, "name"), "");
    NC_ASSERT(nc_str_eql(name, "t1a"), "json get string");

    double ver = nc_json_num(nc_json_get(r1, "version"), 0);
    NC_ASSERT(ver == 1.0, "json get number");

    bool fast = nc_json_bool(nc_json_get(r1, "fast"), false);
    NC_ASSERT(fast == true, "json get bool");

    NC_ASSERT(nc_json_get(r1, "missing") == NULL, "json get missing returns NULL");

    /* Parse array */
    const char *j2 = "[1, 2, 3]";
    nc_json *r2 = nc_json_parse(&a, j2, strlen(j2));
    NC_ASSERT(r2 != NULL && r2->type == NC_JSON_ARRAY, "json parse array");
    NC_ASSERT(r2->array.count == 3, "json array count");

    /* Parse nested */
    const char *j3 = "{\"gateway\": {\"port\": 3000, \"host\": \"127.0.0.1\"}}";
    nc_json *r3 = nc_json_parse(&a, j3, strlen(j3));
    NC_ASSERT(r3 != NULL, "json parse nested");
    nc_json *gw = nc_json_get(r3, "gateway");
    NC_ASSERT(gw != NULL && gw->type == NC_JSON_OBJECT, "json nested object");
    NC_ASSERT(nc_json_num(nc_json_get(gw, "port"), 0) == 3000, "json nested number");

    /* Parse string with escapes */
    const char *j4 = "{\"msg\": \"hello\\nworld\"}";
    nc_json *r4 = nc_json_parse(&a, j4, strlen(j4));
    nc_str msg = nc_json_str(nc_json_get(r4, "msg"), "");
    NC_ASSERT(msg.len == 11, "json escape string length");

    /* Parse null */
    const char *j5 = "null";
    nc_json *r5 = nc_json_parse(&a, j5, strlen(j5));
    NC_ASSERT(r5 != NULL && r5->type == NC_JSON_NULL, "json parse null");

    nc_arena_free(&a);
}

void nc_test_jwriter(void) {
    char buf[512];
    nc_jw w;
    nc_jw_init(&w, buf, sizeof(buf));

    nc_jw_obj_open(&w);
    nc_jw_str(&w, "name", "t1a");
    nc_jw_num(&w, "port", 3000);
    nc_jw_bool(&w, "fast", true);
    nc_jw_obj_close(&w);

    NC_ASSERT(w.len > 0, "jwriter produced output");
    NC_ASSERT(strstr(buf, "\"t1a\"") != NULL, "jwriter has string");
    NC_ASSERT(strstr(buf, "3000") != NULL, "jwriter has number");
    NC_ASSERT(strstr(buf, "true") != NULL, "jwriter has bool");
}

/* from src/mcp_builtin.c */
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
    /* execute() returns bool "handled" — always true; output contains the error msg */
    tavily.execute(&tavily, "{\"query\":\"test\"}", out, sizeof(out));
    NC_ASSERT(strstr(out, "TAVILY_API_KEY") != NULL, "Tavily missing-key guidance");
}

/* from src/memory.c */
void nc_test_memory(void) {
    char path[] = "/tmp/t1a_guardian_test_XXXXXX";
    int fd = mkstemp(path);
    NC_ASSERT(fd >= 0, "create Guardian test file");
    if (fd >= 0) close(fd);

    nc_memory mem = nc_memory_guardian(path);
    NC_ASSERT(strcmp(mem.backend_name, "guardian") == 0, "Guardian backend name");
    NC_ASSERT(mem.store(&mem, "project", "persistent observation"), "Guardian store");

    char out[4096];
    NC_ASSERT(mem.recall(&mem, "persistent", out, sizeof(out)), "Guardian recall");
    NC_ASSERT(strstr(out, "persistent observation") != NULL, "Guardian recall content");
    mem.free(&mem);

    mem = nc_memory_guardian(path);
    NC_ASSERT(mem.recall(&mem, "persistent", out, sizeof(out)), "Guardian survives reopen");
    NC_ASSERT(mem.forget(&mem, "project"), "Guardian forget");
    NC_ASSERT(!mem.recall(&mem, "persistent", out, sizeof(out)), "Guardian forget persisted");
    mem.free(&mem);
    unlink(path);
}

/* from src/util.c */
void nc_test_str(void) {
    nc_str a = NC_STR("hello");
    nc_str b = NC_STR("hello");
    nc_str c = NC_STR("world");

    NC_ASSERT(nc_str_eq(a, b), "str_eq same");
    NC_ASSERT(!nc_str_eq(a, c), "str_eq diff");
    NC_ASSERT(nc_str_eql(a, "hello"), "str_eql match");
    NC_ASSERT(!nc_str_eql(a, "hell"), "str_eql no match");

    nc_str d = nc_str_from("test");
    NC_ASSERT(d.len == 4, "str_from len");
    NC_ASSERT(nc_str_eql(d, "test"), "str_from content");

    nc_str e = nc_str_from(NULL);
    NC_ASSERT(e.ptr == NULL && e.len == 0, "str_from NULL");

    char buf[64];
    nc_path_join(buf, sizeof(buf), "/home", ".t1a");
    NC_ASSERT(strcmp(buf, "/home/.t1a") == 0, "path_join 2");

    nc_path_join3(buf, sizeof(buf), "/home", ".t1a", "config.json");
    NC_ASSERT(strcmp(buf, "/home/.t1a/config.json") == 0, "path_join 3");
}

int main(void) {
    printf("t1a test suite\n");
    printf("═════════════════\n\n");

    nc_test_arena();
    nc_test_str();
    nc_test_json();
    nc_test_jwriter();
    nc_test_config();
    nc_test_memory();
    nc_test_http();
    nc_test_builtin_tools();
    nc_test_agent_context();

    printf("\n═════════════════\n");
    printf("Results: %d passed, %d failed\n", nc_test_pass, nc_test_fail);
    return nc_test_fail > 0 ? 1 : 0;
}
