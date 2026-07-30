#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper to write to sysfs */
static void write_sysfs(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", val);
        fclose(f);
    }
}

static bool hw_servo_set_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    int servo_id = -1;
    int angle = -1;

    char *id_str = strstr(args_json, "\"servo_id\":");
    if (id_str) servo_id = atoi(id_str + 11);

    char *angle_str = strstr(args_json, "\"angle\":");
    if (angle_str) angle = atoi(angle_str + 8);

    if (servo_id < 0 || servo_id > 1 || angle < 0 || angle > 180) {
        nc_strlcpy(out, "{\"error\": \"servo_id must be 0 or 1. angle must be 0-180\"}", out_cap);
        return true;
    }

    /*
     * Standard SG90 Micro Servo:
     * 50Hz (20,000,000 ns period)
     * 0 degrees = ~500,000 ns pulse
     * 180 degrees = ~2,500,000 ns pulse
     */
    long long period_ns = 20000000;
    long long duty_ns = 500000 + (angle * 2000000LL / 180);

    char path_period[128];
    char path_duty[128];
    char path_enable[128];

    /* Map servo 0 -> pwmchip1, servo 1 -> pwmchip2 */
    int chip_id = servo_id + 1;

    snprintf(path_period, sizeof(path_period), "/sys/class/pwm/pwmchip%d/pwm0/period", chip_id);
    snprintf(path_duty, sizeof(path_duty), "/sys/class/pwm/pwmchip%d/pwm0/duty_cycle", chip_id);
    snprintf(path_enable, sizeof(path_enable), "/sys/class/pwm/pwmchip%d/pwm0/enable", chip_id);

    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", period_ns);
    write_sysfs(path_period, buf);

    snprintf(buf, sizeof(buf), "%lld", duty_ns);
    write_sysfs(path_duty, buf);

    write_sysfs(path_enable, "1");

    snprintf(out, out_cap, "{\"status\": \"success\", \"servo_id\": %d, \"angle\": %d}", servo_id, angle);
    return true;
}

nc_tool nc_tool_hw_servo(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_servo_set",
            .description = "Sets the angle of a standard 180-degree RC servo (Pan/Tilt) via Hardware PWM.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"servo_id\":{\"type\":\"integer\",\"description\":\"0 for Pan, 1 for Tilt\"},\"angle\":{\"type\":\"integer\",\"description\":\"Angle in degrees (0 to 180)\"}},\"required\":[\"servo_id\",\"angle\"]}"
        },
        .execute = hw_servo_set_execute,
        .free = NULL
    };
    return tool;
}
