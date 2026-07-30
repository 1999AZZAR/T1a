#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>

#define MPU_ADDR 0x68
#define PWR_MGMT_1 0x6B
#define ACCEL_XOUT_H 0x3B

static bool hw_mpu_status_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    int file;
    char *filename = "/dev/i2c-1";

    if ((file = open(filename, O_RDWR)) < 0) {
        nc_strlcpy(out, "{\"error\": \"Failed to open /dev/i2c-1\"}", out_cap);
        return true;
    }

    if (ioctl(file, I2C_SLAVE, MPU_ADDR) < 0) {
        close(file);
        nc_strlcpy(out, "{\"error\": \"Failed to bind to I2C address 0x68\"}", out_cap);
        return true;
    }

    /* Wake up MPU6050 */
    unsigned char config[2] = {PWR_MGMT_1, 0x00};
    if (write(file, config, 2) != 2) {
        close(file);
        nc_strlcpy(out, "{\"error\": \"Failed to write wake-up command\"}", out_cap);
        return true;
    }

    /* Read 14 bytes starting from ACCEL_XOUT_H */
    unsigned char reg = ACCEL_XOUT_H;
    unsigned char data[14];

    if (write(file, &reg, 1) != 1 || read(file, data, 14) != 14) {
        close(file);
        nc_strlcpy(out, "{\"error\": \"Failed to read data registers\"}", out_cap);
        return true;
    }

    close(file);

    int16_t ax = (data[0] << 8) | data[1];
    int16_t ay = (data[2] << 8) | data[3];
    int16_t az = (data[4] << 8) | data[5];
    int16_t t  = (data[6] << 8) | data[7];
    int16_t gx = (data[8] << 8) | data[9];
    int16_t gy = (data[10] << 8) | data[11];
    int16_t gz = (data[12] << 8) | data[13];

    /* Convert to approximate physical units (assuming default +/- 2g and +/- 250deg/s) */
    float accel_x = ax / 16384.0f;
    float accel_y = ay / 16384.0f;
    float accel_z = az / 16384.0f;
    float temp_c = (t / 340.0f) + 36.53f;
    float gyro_x = gx / 131.0f;
    float gyro_y = gy / 131.0f;
    float gyro_z = gz / 131.0f;

    snprintf(out, out_cap,
             "{\"ax\":%.2f, \"ay\":%.2f, \"az\":%.2f, "
             "\"gx\":%.2f, \"gy\":%.2f, \"gz\":%.2f, \"temp_c\":%.2f}",
             accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z, temp_c);

    return true;
}

nc_tool nc_tool_hw_mpu6050(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_mpu_status",
            .description = "Reads 6-axis accelerometer and gyroscope data from the MPU6050 over I2C.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"
        },
        .execute = hw_mpu_status_execute,
        .free = NULL
    };
    return tool;
}
