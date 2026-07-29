#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#ifdef __linux__
#include <linux/i2c-dev.h>
#else
#define I2C_SLAVE 0x0703
#endif

static bool s_is_luckfox = false;

typedef struct {
    char name[32];
    int pin;
} gpio_alias_t;

static gpio_alias_t s_gpio_aliases[64];
static int s_gpio_alias_count = 0;

static int parse_raw_gpio_pin(const char *str) {
    if (strncmp(str, "GPIO", 4) == 0 || strncmp(str, "gpio", 4) == 0) {
        int bank = str[4] - '0';
        if (bank < 0 || bank > 4) return -1;
        if (str[5] != '_') return -1;
        char group_char = str[6];
        int group = -1;
        if (group_char >= 'A' && group_char <= 'D') group = group_char - 'A';
        else if (group_char >= 'a' && group_char <= 'd') group = group_char - 'a';
        else return -1;
        int pin = str[7] - '0';
        if (pin < 0 || pin > 7) return -1;
        return (bank * 32) + (group * 8) + pin;
    }
    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (*endptr == '\0' && val >= 0) return (int)val;
    return -1;
}

void nc_hardware_init(void) {
    char hw[128];
    nc_detect_hardware(hw, sizeof(hw));
    if (strstr(hw, "RV1103") != NULL || strstr(hw, "Luckfox") != NULL) {
        s_is_luckfox = true;
    } else {
        s_is_luckfox = false;
    }

    FILE *f = fopen("gpio_aliases.txt", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f) && s_gpio_alias_count < 64) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *name = line;
                char *val = eq + 1;
                while (*name == ' ' || *name == '\t') name++;
                char *end = name + strlen(name) - 1;
                while (end >= name && (*end == ' ' || *end == '\t')) *end-- = '\0';

                while (*val == ' ' || *val == '\t') val++;
                end = val + strlen(val) - 1;
                while (end >= val && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) *end-- = '\0';

                if (name[0] != '\0' && val[0] != '\0') {
                    nc_strlcpy(s_gpio_aliases[s_gpio_alias_count].name, name, 32);
                    s_gpio_aliases[s_gpio_alias_count].pin = parse_raw_gpio_pin(val);
                    if (s_gpio_aliases[s_gpio_alias_count].pin >= 0) {
                        s_gpio_alias_count++;
                    }
                }
            }
        }
        fclose(f);
    }
}

bool nc_gpio_set_alias(const char *name, const char *pin_str) {
    int pin = parse_raw_gpio_pin(pin_str);
    if (pin < 0) return false;

    bool found = false;
    for (int i = 0; i < s_gpio_alias_count; i++) {
        if (strcasecmp(s_gpio_aliases[i].name, name) == 0) {
            s_gpio_aliases[i].pin = pin;
            found = true;
            break;
        }
    }

    if (!found) {
        if (s_gpio_alias_count >= 64) return false;
        nc_strlcpy(s_gpio_aliases[s_gpio_alias_count].name, name, 32);
        s_gpio_aliases[s_gpio_alias_count].pin = pin;
        s_gpio_alias_count++;
    }

    FILE *f = fopen("gpio_aliases.txt", "w");
    if (!f) return false;
    for (int i = 0; i < s_gpio_alias_count; i++) {
        fprintf(f, "%s=%d\n", s_gpio_aliases[i].name, s_gpio_aliases[i].pin);
    }
    fclose(f);
    return true;
}

bool nc_gpio_remove_alias(const char *name) {
    bool found = false;
    for (int i = 0; i < s_gpio_alias_count; i++) {
        if (strcasecmp(s_gpio_aliases[i].name, name) == 0) {
            for (int j = i; j < s_gpio_alias_count - 1; j++) {
                s_gpio_aliases[j] = s_gpio_aliases[j + 1];
            }
            s_gpio_alias_count--;
            found = true;
            break;
        }
    }

    if (!found) return false;

    FILE *f = fopen("gpio_aliases.txt", "w");
    if (!f) return false;
    for (int i = 0; i < s_gpio_alias_count; i++) {
        fprintf(f, "%s=%d\n", s_gpio_aliases[i].name, s_gpio_aliases[i].pin);
    }
    fclose(f);
    return true;
}

bool nc_hardware_is_luckfox(void) {
    return s_is_luckfox;
}

/* ── GPIO ───────────────────────────────────────────────────────── */

static bool sysfs_write(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%s", val);
    fclose(f);
    return true;
}

bool nc_gpio_export(int pin) {
    if (!s_is_luckfox) return true; // Mock success
    char val[16];
    snprintf(val, sizeof(val), "%d", pin);
    return sysfs_write("/sys/class/gpio/export", val);
}

bool nc_gpio_unexport(int pin) {
    if (!s_is_luckfox) return true;
    char val[16];
    snprintf(val, sizeof(val), "%d", pin);
    return sysfs_write("/sys/class/gpio/unexport", val);
}

bool nc_gpio_set_dir(int pin, const char *dir) {
    if (!s_is_luckfox) return true;
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    return sysfs_write(path, dir);
}

bool nc_gpio_write(int pin, int val) {
    if (!s_is_luckfox) return true;
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    char v[16];
    snprintf(v, sizeof(v), "%d", val);
    return sysfs_write(path, v);
}

int nc_gpio_read(int pin) {
    if (!s_is_luckfox) return 0; // Mock read
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int val = -1;
    if (fscanf(f, "%d", &val) != 1) val = -1;
    fclose(f);
    return val;
}

/* ── I2C ────────────────────────────────────────────────────────── */

int nc_i2c_open(int bus, int addr) {
    if (!s_is_luckfox) return 9999; // Mock file descriptor
    char path[64];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void nc_i2c_close(int fd) {
    if (!s_is_luckfox || fd == 9999) return;
    close(fd);
}

int nc_i2c_write(int fd, const unsigned char *data, size_t len) {
    if (!s_is_luckfox || fd == 9999) return (int)len; // Mock success
    return (int)write(fd, data, len);
}

int nc_i2c_read(int fd, unsigned char *data, size_t len) {
    if (!s_is_luckfox || fd == 9999) {
        memset(data, 0, len); // Mock empty read
        return (int)len;
    }
    return (int)read(fd, data, len);
}

/* ── Tool Integrations ──────────────────────────────────────────── */

static int parse_gpio_pin(const char *str) {
    for (int i = 0; i < s_gpio_alias_count; i++) {
        if (strcasecmp(str, s_gpio_aliases[i].name) == 0) {
            return s_gpio_aliases[i].pin;
        }
    }
    return parse_raw_gpio_pin(str);
}

static bool hw_gpio_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char action[32] = {0}, pin_str[32] = {0}, dir_str[32] = {0}, val_str[32] = {0};

    // Naive JSON extraction
    const char *keys[] = {"action", "pin", "dir", "val"};
    char *outs[] = {action, pin_str, dir_str, val_str};
    const size_t caps[] = {sizeof(action), sizeof(pin_str), sizeof(dir_str), sizeof(val_str)};

    for (int i = 0; i < 4; i++) {
        char search_key[32];
        snprintf(search_key, sizeof(search_key), "\"%s\"", keys[i]);
        char *p = strstr(args_json, search_key);
        if (p) {
            p += strlen(search_key);
            while (*p == ' ' || *p == ':') p++;
            if (*p == '"') {
                p++;
                char *end = strchr(p, '"');
                if (end) {
                    size_t len = end - p;
                    if (len >= caps[i]) len = caps[i] - 1;
                    strncpy(outs[i], p, len);
                    outs[i][len] = '\0';
                }
            } else if (*p >= '0' && *p <= '9') {
                char *end = p;
                while (*end >= '0' && *end <= '9') end++;
                size_t len = end - p;
                if (len >= caps[i]) len = caps[i] - 1;
                strncpy(outs[i], p, len);
                outs[i][len] = '\0';
            }
        }
    }

    if (action[0] == '\0' || pin_str[0] == '\0') {
        snprintf(out, out_cap, "error: action and pin are required");
        return true;
    }

    int pin = parse_gpio_pin(pin_str);
    if (pin < 0) {
        snprintf(out, out_cap, "error: invalid pin format (use GPIO1_C7 or integer)");
        return true;
    }

    if (strcmp(action, "export") == 0) {
        bool ok = nc_gpio_export(pin);
        snprintf(out, out_cap, ok ? "success: exported GPIO %d" : "error: failed to export GPIO %d", pin);
    } else if (strcmp(action, "unexport") == 0) {
        bool ok = nc_gpio_unexport(pin);
        snprintf(out, out_cap, ok ? "success: unexported GPIO %d" : "error: failed to unexport GPIO %d", pin);
    } else if (strcmp(action, "set_dir") == 0) {
        if (dir_str[0] == '\0') {
            snprintf(out, out_cap, "error: dir is required for set_dir");
            return true;
        }
        bool ok = nc_gpio_set_dir(pin, dir_str);
        if (ok) snprintf(out, out_cap, "success: set dir %s for GPIO %d", dir_str, pin);
        else snprintf(out, out_cap, "error: failed to set dir for GPIO %d", pin);
    } else if (strcmp(action, "write") == 0) {
        if (val_str[0] == '\0') {
            snprintf(out, out_cap, "error: val is required for write");
            return true;
        }
        int val = atoi(val_str);
        bool ok = nc_gpio_write(pin, val);
        snprintf(out, out_cap, ok ? "success: wrote %d to GPIO %d" : "error: failed to write to GPIO %d", val, pin);
    } else if (strcmp(action, "read") == 0) {
        int val = nc_gpio_read(pin);
        if (val < 0) snprintf(out, out_cap, "error: failed to read GPIO %d", pin);
        else snprintf(out, out_cap, "{\"pin\":%d,\"value\":%d}", pin, val);
    } else {
        snprintf(out, out_cap, "error: unknown action");
    }
    return true;
}

nc_tool nc_tool_hw_gpio(void) {
    return (nc_tool){
        .def = {
            .name = "hw_gpio",
            .description = "Control hardware GPIO pins. Actions: export, unexport, set_dir, write, read. Pin format: 'LED', 'GPIO1_C7', or '55'. Map aliases in gpio_aliases.txt",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"pin\":{\"type\":\"string\"},\"dir\":{\"type\":\"string\"},\"val\":{\"type\":\"string\"}},\"required\":[\"action\",\"pin\"]}",
        },
        .execute = hw_gpio_execute,
    };
}

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static bool hw_i2c_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char action[32] = {0}, bus_str[16] = {0}, addr_str[16] = {0}, data_hex[256] = {0}, read_len_str[16] = {0};

    const char *keys[] = {"action", "bus", "addr", "data_hex", "read_len"};
    char *outs[] = {action, bus_str, addr_str, data_hex, read_len_str};
    const size_t caps[] = {sizeof(action), sizeof(bus_str), sizeof(addr_str), sizeof(data_hex), sizeof(read_len_str)};

    for (int i = 0; i < 5; i++) {
        char search_key[32];
        snprintf(search_key, sizeof(search_key), "\"%s\"", keys[i]);
        char *p = strstr(args_json, search_key);
        if (p) {
            p += strlen(search_key);
            while (*p == ' ' || *p == ':') p++;
            if (*p == '"') {
                p++;
                char *end = strchr(p, '"');
                if (end) {
                    size_t len = end - p;
                    if (len >= caps[i]) len = caps[i] - 1;
                    strncpy(outs[i], p, len);
                    outs[i][len] = '\0';
                }
            } else if (*p >= '0' && *p <= '9') {
                char *end = p;
                while (*end >= '0' && *end <= '9') end++;
                size_t len = end - p;
                if (len >= caps[i]) len = caps[i] - 1;
                strncpy(outs[i], p, len);
                outs[i][len] = '\0';
            }
        }
    }

    if (action[0] == '\0' || bus_str[0] == '\0') {
        snprintf(out, out_cap, "error: action and bus are required");
        return true;
    }

    int bus = atoi(bus_str);
    int addr = addr_str[0] ? atoi(addr_str) : 0;

    if (strcmp(action, "scan") == 0) {
        char scan_out[256];
        scan_out[0] = '\0';
        int found = 0;
        char path[64];
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        int fd = s_is_luckfox ? open(path, O_RDWR) : 9999;
        if (fd < 0) {
            snprintf(out, out_cap, "error: failed to open I2C bus %d for scanning", bus);
            return true;
        }

        for (int a = 0x03; a < 0x78; a++) {
            if (s_is_luckfox) {
                if (ioctl(fd, I2C_SLAVE, a) >= 0) {
                    unsigned char buf;
                    if (read(fd, &buf, 1) >= 0) {
                        char addr_hex[16];
                        snprintf(addr_hex, sizeof(addr_hex), "\"0x%02X\",", a);
                        strcat(scan_out, addr_hex);
                        found++;
                    }
                }
            } else {
                if (a == 0x3C || a == 0x68) { // mock devices
                    char addr_hex[16];
                    snprintf(addr_hex, sizeof(addr_hex), "\"0x%02X\",", a);
                    strcat(scan_out, addr_hex);
                    found++;
                }
            }
        }
        if (s_is_luckfox) close(fd);
        if (found > 0) {
            scan_out[strlen(scan_out)-1] = '\0'; // remove last comma
            snprintf(out, out_cap, "{\"found\":%d,\"addresses\":[%s]}", found, scan_out);
        } else {
            snprintf(out, out_cap, "{\"found\":0,\"addresses\":[]}");
        }
        return true;
    }

    if (addr_str[0] == '\0') {
        snprintf(out, out_cap, "error: addr is required for read/write");
        return true;
    }

    int fd = nc_i2c_open(bus, addr);
    if (fd < 0) {
        snprintf(out, out_cap, "error: failed to open I2C bus %d at addr 0x%02X", bus, addr);
        return true;
    }

    if (strcmp(action, "write") == 0) {
        unsigned char raw[128];
        size_t raw_len = 0;
        size_t hex_len = strlen(data_hex);
        for (size_t i = 0; i < hex_len && i < 256 && raw_len < 128; i += 2) {
            raw[raw_len++] = (hex_char_to_int(data_hex[i]) << 4) | hex_char_to_int(data_hex[i+1]);
        }
        int w = nc_i2c_write(fd, raw, raw_len);
        nc_i2c_close(fd);
        snprintf(out, out_cap, w >= 0 ? "success: wrote %d bytes" : "error: write failed", w);
    } else if (strcmp(action, "read") == 0) {
        int rlen = atoi(read_len_str);
        if (rlen <= 0 || rlen > 128) rlen = 1;
        unsigned char raw[128];
        int r = nc_i2c_read(fd, raw, rlen);
        nc_i2c_close(fd);
        if (r < 0) {
            snprintf(out, out_cap, "error: read failed");
        } else {
            char hex_out[257];
            for (int i = 0; i < r; i++) {
                sprintf(hex_out + (i * 2), "%02X", raw[i]);
            }
            snprintf(out, out_cap, "{\"read_bytes\":%d,\"data_hex\":\"%s\"}", r, hex_out);
        }
    } else {
        nc_i2c_close(fd);
        snprintf(out, out_cap, "error: unknown action");
    }
    return true;
}

nc_tool nc_tool_hw_i2c(void) {
    return (nc_tool){
        .def = {
            .name = "hw_i2c",
            .description = "Control hardware I2C bus. Actions: scan, write, read. Provide bus and addr as integers. For write, provide data_hex. For read, provide read_len. For scan, only bus is required.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"bus\":{\"type\":\"integer\"},\"addr\":{\"type\":\"integer\"},\"data_hex\":{\"type\":\"string\"},\"read_len\":{\"type\":\"integer\"}},\"required\":[\"action\",\"bus\"]}",
        },
        .execute = hw_i2c_execute,
    };
}
