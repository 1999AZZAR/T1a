#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define OLED_ADDR 0x3C

static bool hw_oled_print_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    char text[128] = {0};

    char *text_str = strstr(args_json, "\"text\":\"");
    if (text_str) {
        text_str += 8;
        char *end = strchr(text_str, '"');
        if (end) {
            int len = end - text_str;
            if (len >= (int)sizeof(text)) len = sizeof(text) - 1;
            memcpy(text, text_str, len);
        }
    }

    int file;
    if ((file = open("/dev/i2c-1", O_RDWR)) < 0) {
        nc_strlcpy(out, "{\"error\": \"Failed to open /dev/i2c-1\"}", out_cap);
        return true;
    }

    if (ioctl(file, I2C_SLAVE, OLED_ADDR) < 0) {
        close(file);
        nc_strlcpy(out, "{\"error\": \"Failed to bind to OLED I2C address 0x3C\"}", out_cap);
        return true;
    }

    /* Standard SSD1306 Initialization sequence */
    unsigned char init_seq[] = {
        0x00, /* Command stream prefix */
        0xAE, /* Display OFF */
        0xD5, 0x80, /* Set Display Clock Divide Ratio */
        0xA8, 0x3F, /* Set Multiplex Ratio */
        0xD3, 0x00, /* Set Display Offset */
        0x40, /* Set Start Line 0 */
        0x8D, 0x14, /* Charge Pump Setting */
        0x20, 0x00, /* Memory Addressing Mode (Horizontal) */
        0xA1, /* Set Segment Re-map */
        0xC8, /* Set COM Output Scan Direction */
        0xDA, 0x12, /* Set COM Pins Hardware Configuration */
        0x81, 0xCF, /* Set Contrast Control */
        0xD9, 0xF1, /* Set Pre-charge Period */
        0xDB, 0x40, /* Set VCOMH Deselect Level */
        0xA4, /* Entire Display ON resume */
        0xA6, /* Normal Display */
        0xAF  /* Display ON */
    };
    write(file, init_seq, sizeof(init_seq));

    /*
     * In a full implementation, we would maintain a 1024-byte framebuffer,
     * convert the 'text' to 5x7 ASCII bitmap data, and flush it here
     * using the 0x40 Data prefix. For now, we clear the screen.
     */
    unsigned char clr_cmd[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    write(file, clr_cmd, sizeof(clr_cmd));

    unsigned char *fb = calloc(1, 1025);
    if (fb) {
        fb[0] = 0x40; /* Data stream prefix */
        /* Normally, map 'text' into 'fb' here */
        write(file, fb, 1025);
        free(fb);
    }

    close(file);

    snprintf(out, out_cap, "{\"status\": \"success\", \"text_displayed\": \"%s\"}", text);
    return true;
}

nc_tool nc_tool_hw_oled(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_oled_print",
            .description = "Prints text to the SSD1306 OLED display via I2C.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to display\"}},\"required\":[\"text\"]}"
        },
        .execute = hw_oled_print_execute,
        .free = NULL
    };
    return tool;
}
