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

/* Play a single tone */
static void play_tone(int freq, int duration_ms) {
    if (freq <= 0 || duration_ms <= 0) return;
    long long period_ns = 1000000000LL / freq;
    long long duty_ns = period_ns / 2;
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", period_ns);
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/period", buf);
    snprintf(buf, sizeof(buf), "%lld", duty_ns);
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/duty_cycle", buf);
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/enable", "1");
    usleep(duration_ms * 1000);
    write_sysfs("/sys/class/pwm/pwmchip0/pwm0/enable", "0");
    /* Tiny pause between notes */
    usleep(20 * 1000);
}

static bool hw_buzzer_beep_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    int freq = 0;
    int duration_ms = 0;
    char melody[64] = {0};

    char *melody_str = strstr(args_json, "\"melody\":\"");
    if (melody_str) {
        melody_str += 10;
        char *end = strchr(melody_str, '"');
        if (end) {
            int len = end - melody_str;
            if (len >= (int)sizeof(melody)) len = sizeof(melody) - 1;
            memcpy(melody, melody_str, len);
        }
    } else {
        char *freq_str = strstr(args_json, "\"freq\":");
        if (freq_str) freq = atoi(freq_str + 7);
        char *dur_str = strstr(args_json, "\"duration_ms\":");
        if (dur_str) duration_ms = atoi(dur_str + 14);
    }

    if (strlen(melody) > 0) {
        if (strcmp(melody, "do") == 0) play_tone(261, 300);
        else if (strcmp(melody, "re") == 0) play_tone(293, 300);
        else if (strcmp(melody, "mi") == 0) play_tone(329, 300);
        else if (strcmp(melody, "fa") == 0) play_tone(349, 300);
        else if (strcmp(melody, "sol") == 0) play_tone(392, 300);
        else if (strcmp(melody, "la") == 0) play_tone(440, 300);
        else if (strcmp(melody, "si") == 0) play_tone(493, 300);
        else if (strcmp(melody, "r2d2_happy") == 0) {
            play_tone(2000, 100); play_tone(2500, 100);
            play_tone(3000, 100); play_tone(2000, 100);
            play_tone(3500, 200);
        }
        else if (strcmp(melody, "r2d2_sad") == 0) {
            play_tone(1000, 200); play_tone(800, 200); play_tone(600, 400);
        }
        else if (strcmp(melody, "r2d2_confused") == 0) {
            play_tone(2000, 150); play_tone(1000, 150); play_tone(1500, 150);
        }
        snprintf(out, out_cap, "{\"status\": \"success\", \"played_melody\": \"%s\"}", melody);
        return true;
    }

    if (freq > 0 && duration_ms > 0) {
        play_tone(freq, duration_ms);
        snprintf(out, out_cap, "{\"status\": \"success\", \"freq\": %d, \"duration_ms\": %d}", freq, duration_ms);
        return true;
    }

    nc_strlcpy(out, "{\"error\": \"Must provide either 'melody' or 'freq' + 'duration_ms'\"}", out_cap);
    return true;
}

nc_tool nc_tool_hw_buzzer(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_buzzer_beep",
            .description = "Plays a tone or predefined melody on a passive buzzer (e.g. 'do', 're', 'r2d2_happy', 'r2d2_sad').",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"melody\":{\"type\":\"string\",\"description\":\"Optional predefined melody (do, re, mi, fa, sol, la, si, r2d2_happy, r2d2_sad)\"},\"freq\":{\"type\":\"integer\"},\"duration_ms\":{\"type\":\"integer\"}}}"
        },
        .execute = hw_buzzer_beep_execute,
        .free = NULL
    };
    return tool;
}
