#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Helper to write to sysfs */
static void write_sysfs(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", val);
        fclose(f);
    }
}

static bool hw_buzzer_beep_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    int freq = 0;
    int duration_ms = 0;

    /* Naive JSON parsing for freq and duration */
    char *freq_str = strstr(args_json, "\"freq\":");
    if (freq_str) freq = atoi(freq_str + 7);

    char *dur_str = strstr(args_json, "\"duration_ms\":");
    if (dur_str) duration_ms = atoi(dur_str + 14);

    if (freq <= 0 || duration_ms <= 0) {
        nc_strlcpy(out, "{\"error\": \"Invalid frequency or duration\"}", out_cap);
        return true;
    }

    /* Convert frequency (Hz) to period in nanoseconds */
    long long period_ns = 1000000000LL / freq;
    long long duty_ns = period_ns / 2; /* 50% duty cycle */

    char buf[64];

    /* Ensure PWM is exported. Depending on board, this might be pwmchip0, pwm0 */
    /* Write period */
    snprintf(buf, sizeof(buf), "%lld", period_ns);
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/period", buf);

    /* Write duty cycle */
    snprintf(buf, sizeof(buf), "%lld", duty_ns);
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/duty_cycle", buf);

    /* Enable */
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/enable", "1");

    /* Sleep for duration */
    usleep(duration_ms * 1000);

    /* Disable */
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/enable", "0");

    snprintf(out, out_cap, "{\"status\": \"success\", \"freq\": %d, \"duration_ms\": %d}", freq, duration_ms);
    return true;
}

nc_tool nc_tool_hw_buzzer(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_buzzer_beep",
            .description = "Plays a tone on a passive buzzer using Hardware PWM.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"freq\":{\"type\":\"integer\",\"description\":\"Frequency in Hz\"},\"duration_ms\":{\"type\":\"integer\",\"description\":\"Duration in milliseconds\"}},\"required\":[\"freq\",\"duration_ms\"]}"
        },
        .execute = hw_buzzer_beep_execute,
        .free = NULL
    };
    return tool;
}
