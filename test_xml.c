#include <stdio.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    char name[64];
    char id[64];
    char arguments[2048];
} nc_tool_call;

typedef struct {
    nc_tool_call tool_calls[16];
    int tool_call_count;
    bool has_tool_calls;
    char content[8192];
} nc_chat_response;

static void parse_xml_tool_calls(nc_chat_response *resp) {
    if (resp->has_tool_calls || !resp->content[0]) return;

    char *tc = strstr(resp->content, "<tool_call>");
    if (!tc) return;

    char *invoke = tc;
    while ((invoke = strstr(invoke, "<tool_call>")) != NULL) {
        if (resp->tool_call_count >= 16) break;

        invoke += 11; /* length of "<tool_call>" */
        while (*invoke == ' ' || *invoke == '\t') invoke++;

        char *name_end = invoke;
        while (*name_end && *name_end != '\r' && *name_end != '\n' && *name_end != '<') name_end++;

        if (name_end == invoke) break;

        nc_tool_call *out = &resp->tool_calls[resp->tool_call_count];
        int name_len = name_end - invoke;
        if (name_len >= (int)sizeof(out->name)) name_len = sizeof(out->name) - 1;
        memcpy(out->name, invoke, name_len);
        out->name[name_len] = '\0';

        snprintf(out->id, sizeof(out->id), "xml_%d", resp->tool_call_count);

        char args[2048] = "{";
        int args_len = 1;

        char *end_invoke = strstr(invoke, "</tool_call>");
        if (!end_invoke) break;

        char *param = invoke;
        bool first = true;
        while ((param = strstr(param, "<arg_key>")) != NULL && param < end_invoke) {
            param += 9;
            char *key_end = strstr(param, "</arg_key>");
            if (!key_end) break;

            char *val_start = strstr(key_end, "<arg_value>");
            if (!val_start || val_start > end_invoke) break;
            val_start += 11;

            char *val_end = strstr(val_start, "</arg_value>");
            if (!val_end || val_end > end_invoke) break;

            if (!first) { args[args_len++] = ','; }
            first = false;

            args[args_len++] = '"';
            int k_len = key_end - param;
            memcpy(args + args_len, param, k_len);
            args_len += k_len;
            args[args_len++] = '"';
            args[args_len++] = ':';
            args[args_len++] = '"';

            int v_len = val_end - val_start;
            for (int i=0; i<v_len && args_len < (int)sizeof(args)-5; i++) {
                if (val_start[i] == '"' || val_start[i] == '\\') args[args_len++] = '\\';
                if (val_start[i] == '\n' || val_start[i] == '\r') continue;
                args[args_len++] = val_start[i];
            }
            args[args_len++] = '"';

            param = val_end;
        }

        args[args_len++] = '}';
        args[args_len] = '\0';

        //nc_strlcpy(out->arguments, args, sizeof(out->arguments));
        strcpy(out->arguments, args);
        resp->tool_call_count++;

        invoke = end_invoke;
    }

    if (resp->tool_call_count > 0) {
        resp->has_tool_calls = true;
        *tc = '\0'; /* Hide XML block from user output */
    }
}

int main() {
    nc_chat_response r = {0};
    strcpy(r.content, "<tool_call>tavily_search\n<arg_key>query</arg_key>\n<arg_value>orca killer whale facts</arg_value>\n</tool_call>");
    parse_xml_tool_calls(&r);
    printf("has_tools: %d\nname: %s\nargs: %s\ncontent: '%s'\n", r.has_tool_calls, r.tool_calls[0].name, r.tool_calls[0].arguments, r.content);
    return 0;
}
