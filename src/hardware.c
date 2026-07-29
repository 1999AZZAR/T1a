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

void nc_hardware_init(void) {
    char hw[128];
    nc_detect_hardware(hw, sizeof(hw));
    if (strstr(hw, "RV1103") != NULL || strstr(hw, "Luckfox") != NULL) {
        s_is_luckfox = true;
    } else {
        s_is_luckfox = false;
    }
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
