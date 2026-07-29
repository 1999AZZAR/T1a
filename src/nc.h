#ifndef NC_H
#define NC_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define NC_VERSION       "0.1.0"
#define NC_CONFIG_DIR    ".noclaw"
#define NC_CONFIG_FILE   "config.json"
#define NC_WORKSPACE_DIR "workspace"

#define NC_ARENA_DEFAULT_CAP (64 * 1024)

typedef struct nc_arena_chunk {
    struct nc_arena_chunk *next;
    size_t cap;
    size_t pos;
    uint8_t data[];
} nc_arena_chunk;

typedef struct nc_arena {
    nc_arena_chunk *head;
    nc_arena_chunk *current;
    size_t chunk_size;
} nc_arena;

void  nc_arena_init(nc_arena *a, size_t cap);
void *nc_arena_alloc(nc_arena *a, size_t size);
char *nc_arena_dup(nc_arena *a, const char *s, size_t len);
void  nc_arena_reset(nc_arena *a);
void  nc_arena_free(nc_arena *a);

typedef struct nc_str {
    const char *ptr;
    size_t      len;
} nc_str;

#define NC_STR(lit)     ((nc_str){ .ptr = (lit), .len = sizeof(lit) - 1 })
#define NC_STR_NULL     ((nc_str){ .ptr = NULL, .len = 0 })
#define NC_STR_FMT      "%.*s"
#define NC_STR_ARG(s)   (int)(s).len, (s).ptr

bool   nc_str_eq(nc_str a, nc_str b);
bool   nc_str_eql(nc_str a, const char *b);
nc_str nc_str_from(const char *s);

typedef enum {
    NC_JSON_NULL,
    NC_JSON_BOOL,
    NC_JSON_NUMBER,
    NC_JSON_STRING,
    NC_JSON_ARRAY,
    NC_JSON_OBJECT,
} nc_json_type;

typedef struct nc_json nc_json;
struct nc_json {
    nc_json_type type;
    union {
        bool       boolean;
        double     number;
        nc_str     string;
        struct { nc_json *items; int count; } array;
        struct { nc_str *keys; nc_json *vals; int count; } object;
    };
};

nc_json *nc_json_parse(nc_arena *a, const char *src, size_t len);
nc_json *nc_json_get(nc_json *obj, const char *key);
nc_str   nc_json_str(nc_json *v, const char *fallback);
double   nc_json_num(nc_json *v, double fallback);
bool     nc_json_bool(nc_json *v, bool fallback);

typedef struct nc_jw {
    char  *buf;
    size_t cap;
    size_t len;
    int    depth;
    bool   needs_comma;
} nc_jw;

void nc_jw_init(nc_jw *w, char *buf, size_t cap);
void nc_jw_obj_open(nc_jw *w);
void nc_jw_obj_close(nc_jw *w);
void nc_jw_arr_open(nc_jw *w, const char *key);
void nc_jw_arr_close(nc_jw *w);
void nc_jw_str(nc_jw *w, const char *key, const char *val);
void nc_jw_num(nc_jw *w, const char *key, double val);
void nc_jw_bool(nc_jw *w, const char *key, bool val);
void nc_jw_raw(nc_jw *w, const char *key, const char *raw);

typedef enum {
    NC_LOG_DEBUG,
    NC_LOG_INFO,
    NC_LOG_WARN,
    NC_LOG_ERROR,
} nc_log_level;

void nc_log(nc_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

extern nc_log_level nc_log_min_level;

typedef struct nc_config {
    char config_dir[512];
    char config_path[1024];
    char workspace_dir[1024];
    char api_key[256];
    char api_url[256];
    char default_provider[64];
    char default_model[128];
    char small_model[128];
    double default_temperature;
    char     gateway_host[64];
    uint16_t gateway_port;
    bool     gateway_require_pairing;
    bool     gateway_allow_public_bind;
    char memory_backend[32];
    bool memory_auto_save;
    char autonomy_level[32];
    bool workspace_only;
    int  max_actions_per_hour;
    bool heartbeat_enabled;
    int  heartbeat_interval_minutes;
    bool secrets_encrypt;
    char sandbox_backend[32];
    char telegram_token[256];
    char discord_token[256];
    char slack_token[256];
    char identity_format[32];
    char runtime_kind[32];
    bool   cost_enabled;
    double cost_daily_limit_usd;
    double cost_monthly_limit_usd;

    /* Fallback provider */
    char fallback_provider[64];
    char fallback_model[128];
    char fallback_api_key[256];
    char fallback_api_url[256];
} nc_config;

bool nc_config_load(nc_config *cfg);
bool nc_config_save(const nc_config *cfg);
void nc_config_defaults(nc_config *cfg);
void nc_config_apply_env(nc_config *cfg);

#define NC_MAX_TOOL_CALLS 16
#define NC_MAX_MESSAGES 256

typedef struct nc_tool_call {
    char id[64];
    char name[64];
    char arguments[8192];
} nc_tool_call;

typedef struct nc_message {
    const char *role;
    const char *content;
    const char *tool_call_id;
    nc_tool_call *tool_calls;
    int           tool_call_count;
} nc_message;

typedef void (*nc_stream_cb)(void *user_data, const char *chunk);

typedef struct nc_chat_request {
    const nc_message *messages;
    int               message_count;
    const char       *model;
    double            temperature;
    const char       *tools_json;
    int               max_tokens;
    nc_stream_cb      stream_cb;
    void             *stream_user_data;
} nc_chat_request;

typedef struct nc_chat_response {
    char         content[8192];
    nc_tool_call tool_calls[NC_MAX_TOOL_CALLS];
    int          tool_call_count;
    int          prompt_tokens;
    int          completion_tokens;
    bool         has_tool_calls;
} nc_chat_response;

typedef struct nc_provider nc_provider;
struct nc_provider {
    const char *name;
    void       *ctx;
    bool (*chat)(nc_provider *self, const nc_chat_request *req, nc_chat_response *resp);
    void (*free)(nc_provider *self);
};

nc_provider nc_provider_openai(const char *api_key, const char *api_url);
nc_provider nc_provider_opencode(const char *api_key);
nc_provider nc_provider_from_config(const nc_config *cfg);

typedef struct nc_incoming_msg {
    char sender[128];
    char content[4096];
    char channel_name[32];
    bool is_group;
} nc_incoming_msg;

typedef struct nc_channel nc_channel;


typedef struct nc_memory nc_memory;
struct nc_memory {
    const char *backend_name;
    void       *ctx;
    bool (*store)(nc_memory *self, const char *key, const char *content);
    bool (*recall)(nc_memory *self, const char *query, char *out, size_t out_cap);
    bool (*forget)(nc_memory *self, const char *key);
    void (*free)(nc_memory *self);
};

typedef struct nc_agent {
    nc_config   *config;
    nc_provider *provider;
    struct nc_tool *tools;
    int          tool_count;
    nc_memory   *memory;
    nc_message   messages[NC_MAX_MESSAGES];
    int          message_count;
    nc_arena     arena;
    const char  *cached_tools_json;
    char        *tool_result_buf;
    size_t       tool_result_cap;
    int          actions_this_hour;
    time_t       hour_window_start;
} nc_agent;

struct nc_channel {
    const char *name;
    void       *ctx;
    void (*poll)(nc_channel *self, nc_agent *agent);
    bool (*send)(nc_channel *self, const char *to, const char *text);
    void (*free)(nc_channel *self);
};

nc_channel nc_channel_cli(void);
nc_channel nc_channel_telegram(const char *bot_token);
nc_channel nc_channel_discord(const char *bot_token);
nc_channel nc_channel_slack(const char *bot_token);

typedef struct nc_tool_def {
    const char *name;
    const char *description;
    const char *parameters_json;
} nc_tool_def;

typedef struct nc_tool nc_tool;
struct nc_tool {
    nc_tool_def def;
    void       *ctx;
    bool (*execute)(nc_tool *self, const char *args_json, char *out, size_t out_cap);
    void (*free)(nc_tool *self);
};

nc_tool nc_tool_shell(const nc_config *cfg);
nc_tool nc_tool_file_read(const nc_config *cfg);
nc_tool nc_tool_file_write(const nc_config *cfg);
nc_tool nc_tool_memory_store(void *mem_ctx);
nc_tool nc_tool_memory_recall(void *mem_ctx);
nc_tool nc_tool_get_time(void);
nc_tool nc_tool_sys_info(void);
nc_tool nc_tool_hw_gpio(void);
nc_tool nc_tool_hw_i2c(void);
nc_tool nc_tool_calc(void);
nc_tool nc_tool_http_fetch(void);
nc_tool nc_tool_list_dir(const nc_config *cfg);
nc_tool nc_tool_env_get(void);
nc_tool nc_tool_base64(void);
nc_tool nc_tool_hash(const nc_config *cfg);
nc_tool nc_tool_core_memory_append(const nc_config *cfg);
nc_tool nc_tool_core_memory_replace(const nc_config *cfg);
nc_tool nc_tool_acp_delegate(void);
nc_tool nc_tool_reasoning(void);
nc_tool nc_tool_tavily_search(const char *api_key);
nc_tool nc_tool_wikipedia_search(void);
nc_tool nc_tool_i2c(void);
nc_tool nc_tool_guardian_memory(void *mem_ctx);

#define NC_MAX_TOOLS 128

/* MCP extensions */
int nc_mcp_register_all(const nc_config *cfg, nc_tool *tools, int start_idx);
void nc_mcp_cleanup(void);

nc_memory nc_memory_noop(void);
nc_memory nc_memory_guardian(const char *path);

/* Guardian memory entity struct (shared between memory.c and mcp_builtin.c) */
#define GM_MAX_OBS 16
#define GM_FIELD_LEN 512

typedef struct gm_entity {
    char name[GM_FIELD_LEN];
    char type[GM_FIELD_LEN];
    char observations[GM_MAX_OBS][GM_FIELD_LEN];
    int  obs_count;
    long created_at;
} gm_entity;

/* Guardian memory access functions */
int  gm_entity_count(void *ctx);
gm_entity *gm_entity_at(void *ctx, int idx);
bool gm_store_entity(void *ctx, const char *name, const char *type, const char *obs);
int  gm_query(void *ctx, const char *query, char *out, size_t out_cap);
bool gm_forget_entity(void *ctx, const char *name);

int nc_register_default_tools(nc_tool *tools, const nc_config *cfg, nc_memory *mem);

void nc_agent_init(nc_agent *agent, nc_config *cfg, nc_provider *prov,
                   nc_tool *tools, int tool_count, nc_memory *mem);
const char *nc_agent_chat(nc_agent *agent, const char *user_input, nc_stream_cb stream_cb, void *stream_user_data);
int nc_agent_compact_context(nc_agent *agent);
void nc_agent_reset(nc_agent *agent);
void nc_agent_free(nc_agent *agent);

typedef struct nc_gateway {
    nc_config *config;
    nc_agent  *agent;
    char       pairing_code[8];
    char       bearer_token[65];
    bool       paired;
    int        server_fd;
} nc_gateway;

void nc_gateway_init(nc_gateway *gw, nc_config *cfg, nc_agent *agent);
bool nc_gateway_run(nc_gateway *gw);

typedef struct nc_http_response {
    int    status;
    char  *body;
    size_t body_len;
    size_t body_cap;
} nc_http_response;

typedef bool (*nc_http_stream_cb)(void *user_data, const char *data, size_t len);

bool nc_http_post(const char *url, const char *body, size_t body_len,
                  const char **headers, int header_count,
                  nc_http_response *resp);
bool nc_http_post_stream(const char *url, const char *body, size_t body_len,
                         const char **headers, int header_count,
                         nc_http_stream_cb on_chunk, void *user_data,
                         nc_http_response *resp);
bool nc_http_get(const char *url, const char **headers, int header_count,
                 nc_http_response *resp);
void nc_http_response_free(nc_http_response *resp);

int nc_cmd_agent(int argc, char **argv);
int nc_cmd_gateway(int argc, char **argv);
int nc_cmd_status(int argc, char **argv);
int nc_cmd_onboard(int argc, char **argv);
int nc_cmd_doctor(int argc, char **argv);

bool nc_commands_execute(nc_agent *agent, const char *cmd, long chat_id, nc_channel *chan);

size_t nc_strlcpy(char *dst, const char *src, size_t dstsize);
const char *nc_home_dir(void);
char *nc_path_join(char *buf, size_t bufsz, const char *a, const char *b);
char *nc_path_join3(char *buf, size_t bufsz, const char *a, const char *b, const char *c);
char *nc_read_file(const char *path, size_t *out_len);
bool  nc_write_file(const char *path, const char *data, size_t len);
bool  nc_mkdir_p(const char *path);
bool  nc_file_exists(const char *path);
void nc_random_hex(char *out, size_t len);
void nc_detect_hardware(char *out, size_t cap);

/* Hardware Abstraction (Luckfox RV1103 / Dev Mock) */
void nc_hardware_init(void);
bool nc_hardware_is_luckfox(void);
bool nc_gpio_set_alias(const char *name, const char *pin_str);
bool nc_gpio_remove_alias(const char *name);

/* GPIO */
bool nc_gpio_export(int pin);
bool nc_gpio_unexport(int pin);
bool nc_gpio_set_dir(int pin, const char *dir); /* "in" or "out" */
bool nc_gpio_write(int pin, int val);
int  nc_gpio_read(int pin);

/* I2C */
bool nc_i2c_set_alias(const char *name, int bus, int addr);
bool nc_i2c_remove_alias(const char *name);
bool nc_i2c_resolve_alias(const char *name, int *bus, int *addr);
int  nc_i2c_open(int bus, int addr);
void nc_i2c_close(int fd);
int  nc_i2c_write(int fd, const unsigned char *data, size_t len);
int  nc_i2c_read(int fd, unsigned char *data, size_t len);

#ifdef NC_TEST
extern int nc_test_pass;
extern int nc_test_fail;

#define NC_ASSERT(condition, name) do { \
    if (condition) { \
        nc_test_pass++; \
        printf("  PASS: %s\n", name); \
    } else { \
        nc_test_fail++; \
        printf("  FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
    } \
} while (0)

void nc_test_arena(void);
void nc_test_str(void);
void nc_test_json(void);
void nc_test_jwriter(void);
void nc_test_config(void);
void nc_test_memory(void);
void nc_test_http(void);
void nc_test_builtin_tools(void);
void nc_test_agent_context(void);
#endif

#endif
