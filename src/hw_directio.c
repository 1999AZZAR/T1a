#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Direct IO Abstraction for F1-F5 pins
 */

static bool hw_directio_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    char pin[16] = {0};
    int state = -1;

    char *pin_str = strstr(args_json, "\"pin\":\"");
    if (pin_str) {
        pin_str += 7;
        char *end = strchr(pin_str, '"');
        if (end) {
            int len = end - pin_str;
            if (len >= (int)sizeof(pin)) len = sizeof(pin) - 1;
            memcpy(pin, pin_str, len);
        }
    }

    char *state_str = strstr(args_json, "\"state\":");
    if (state_str) {
        state = atoi(state_str + 8);
    }

    if (strlen(pin) == 0) {
        nc_strlcpy(out, "{\"error\": \"Must provide 'pin' (F1, F2, F3, F4, F5)\"}", out_cap);
        return true;
    }

    /* We map F1-F5 to their underlying GPIO aliases using the existing GPIO system */
    if (state >= 0) {
        /* Write */
        char args_fwd[128];
        snprintf(args_fwd, sizeof(args_fwd), "{\"action\":\"write\", \"alias\":\"%s\", \"value\":%d}", pin, state);
        /* We can just call nc_tool_hw_gpio().execute directly */
        nc_tool gpio_tool = nc_tool_hw_gpio();
        bool res = gpio_tool.execute(&gpio_tool, args_fwd, out, out_cap);
        if (gpio_tool.free) gpio_tool.free(&gpio_tool);
        return res;
    } else {
        /* Read */
        char args_fwd[128];
        snprintf(args_fwd, sizeof(args_fwd), "{\"action\":\"read\", \"alias\":\"%s\"}", pin);
        nc_tool gpio_tool = nc_tool_hw_gpio();
        bool res = gpio_tool.execute(&gpio_tool, args_fwd, out, out_cap);
        if (gpio_tool.free) gpio_tool.free(&gpio_tool);
        return res;
    }
}

nc_tool nc_tool_hw_directio(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_directio",
            .description = "Reads or writes to the free direct IO pins (F1, F2, F3, F4, F5). Omit state to read.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"string\",\"description\":\"F1, F2, F3, F4, or F5\"},\"state\":{\"type\":\"integer\",\"description\":\"0 or 1 to write. Omit to read.\"}},\"required\":[\"pin\"]}"
        },
        .execute = hw_directio_execute,
        .free = NULL
    };
    return tool;
}
