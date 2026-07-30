#include "nc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool hw_dht_read_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    FILE *ftemp = fopen("/sys/bus/iio/devices/iio:device0/in_temp_input", "r");
    FILE *fhum = fopen("/sys/bus/iio/devices/iio:device0/in_humidityrelative_input", "r");

    if (!ftemp || !fhum) {
        if (ftemp) fclose(ftemp);
        if (fhum) fclose(fhum);
        nc_strlcpy(out, "{\"error\": \"DHT IIO device not found. Is the dht11 kernel module loaded?\"}", out_cap);
        return true;
    }

    int temp_milli = 0;
    int hum_milli = 0;

    if (fscanf(ftemp, "%d", &temp_milli) != 1) temp_milli = 0;
    if (fscanf(fhum, "%d", &hum_milli) != 1) hum_milli = 0;

    fclose(ftemp);
    fclose(fhum);

    float temp_c = temp_milli / 1000.0f;
    float humidity = hum_milli / 1000.0f;

    snprintf(out, out_cap, "{\"temp_c\": %.2f, \"humidity\": %.2f}", temp_c, humidity);
    return true;
}

nc_tool nc_tool_hw_dht(void) {
    nc_tool tool = {
        .def = {
            .name = "hw_dht_read",
            .description = "Reads temperature and humidity from the DHT11/22 sensor via the Linux IIO subsystem.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"
        },
        .execute = hw_dht_read_execute,
        .free = NULL
    };
    return tool;
}
