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

static void parse_xml_tool_calls_v2(nc_chat_response *resp) {
    if (resp->has_tool_calls || !resp->content[0]) return;

    char *tc = strstr(resp->content, "<tool_call>");
    if (!tc) return;

    char *invoke = tc;
    while ((invoke = strstr(invoke, "<function=")) != NULL) {
        if (resp->tool_call_count >= 16) break;

        invoke += 10;
        char *name_end = strchr(invoke, '>');
        if (!name_end) break;

        nc_tool_call *out = &resp->tool_calls[resp->tool_call_count];
        int name_len = name_end - invoke;
        if (name_len >= (int)sizeof(out->name)) name_len = sizeof(out->name) - 1;
        memcpy(out->name, invoke, name_len);
        out->name[name_len] = '\0';

        snprintf(out->id, sizeof(out->id), "xml2_%d", resp->tool_call_count);

        char args[2048] = "{";
        int args_len = 1;
        char *end_invoke = strstr(invoke, "</tool_call>");
        if (!end_invoke) break;

        char *param = invoke;
        bool first = true;
        while ((param = strstr(param, "<parameter=")) != NULL && param < end_invoke) {
            param += 11;
            char *key_end = strchr(param, '>');
            if (!key_end) break;

            char *val_start = key_end + 1;
            while (*val_start == '\n' || *val_start == '\r') val_start++;

            char *val_end = strstr(val_start, "</parameter>");
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
            while (v_len > 0 && (val_start[v_len-1] == '\n' || val_start[v_len-1] == '\r')) {
                v_len--;
            }
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
        strcpy(out->arguments, args);
        resp->tool_call_count++;
        invoke = end_invoke;
    }

    if (resp->tool_call_count > 0) {
        resp->has_tool_calls = true;
        *tc = '\0';
    }
}

int main() {
    nc_chat_response r = {0};
    strcpy(r.content, "<tool_call>\n<function=http_fetch>\n<parameter=url>\nhttps://ip-api.com/json/168.110.213.22\n</parameter>\n</function>\n</tool_call>");
    parse_xml_tool_calls_v2(&r);
    printf("has_tools: %d\nname: %s\nargs: %s\ncontent: '%s'\n", r.has_tool_calls, r.tool_calls[0].name, r.tool_calls[0].arguments, r.content);
    return 0;
}
