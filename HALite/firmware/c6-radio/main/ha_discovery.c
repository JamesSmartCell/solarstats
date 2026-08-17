#include "ha_discovery.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "halite_ipc.h"
#include "ipc_host.h"

static const char *TAG = "host_notify";

static void copy_str32(char *dst, const char *src)
{
    memset(dst, 0, 32);
    if (src) {
        strncpy(dst, src, 31);
    }
}

esp_err_t ha_discovery_publish_device(const zbgw_device_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    ipc_device_joined_t msg = {
        .ieee = dev->ieee,
        .short_addr = dev->short_addr,
        .endpoint = dev->endpoint ? dev->endpoint : 1,
        .capabilities = dev->capabilities,
    };
    copy_str32(msg.manufacturer, dev->manufacturer);
    copy_str32(msg.model, dev->model);
    ESP_LOGI(TAG, "DEVICE_JOINED ieee=%016llx caps=0x%lx", (unsigned long long)dev->ieee,
             (unsigned long)dev->capabilities);
    return ipc_host_device_joined(&msg);
}

esp_err_t ha_discovery_unpublish_device(uint64_t ieee)
{
    ESP_LOGI(TAG, "DEVICE_LEFT ieee=%016llx", (unsigned long long)ieee);
    return ipc_host_device_left(ieee);
}

esp_err_t ha_discovery_publish_bridge(void)
{
    return ESP_OK;
}

static uint8_t suffix_to_attr(const char *suffix)
{
    if (!suffix) {
        return 0;
    }
    if (strcmp(suffix, "switch") == 0) {
        return HALITE_IPC_ATTR_ON_OFF;
    }
    if (strcmp(suffix, "temperature") == 0) {
        return HALITE_IPC_ATTR_TEMP_C_X100;
    }
    if (strcmp(suffix, "humidity") == 0) {
        return HALITE_IPC_ATTR_HUMIDITY_X100;
    }
    if (strcmp(suffix, "contact") == 0) {
        return HALITE_IPC_ATTR_CONTACT;
    }
    if (strcmp(suffix, "occupancy") == 0) {
        return HALITE_IPC_ATTR_OCCUPANCY;
    }
    if (strcmp(suffix, "power") == 0) {
        return HALITE_IPC_ATTR_POWER_W_X10;
    }
    if (strcmp(suffix, "energy") == 0) {
        return HALITE_IPC_ATTR_ENERGY_WH;
    }
    if (strcmp(suffix, "battery") == 0) {
        return HALITE_IPC_ATTR_BATTERY_PCT;
    }
    if (strcmp(suffix, "smoke") == 0) {
        return HALITE_IPC_ATTR_SMOKE;
    }
    if (strcmp(suffix, "battery_low") == 0) {
        return HALITE_IPC_ATTR_BATTERY_LOW;
    }
    return 0;
}

esp_err_t ha_discovery_publish_sensor_state(const zbgw_device_t *dev, const char *suffix, const char *value)
{
    if (!dev || !suffix || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t attr = suffix_to_attr(suffix);
    if (!attr) {
        return ESP_OK;
    }
    uint8_t vtype = 1;
    uint32_t bits = 0;
    if (attr == HALITE_IPC_ATTR_ON_OFF || attr == HALITE_IPC_ATTR_CONTACT || attr == HALITE_IPC_ATTR_OCCUPANCY ||
        attr == HALITE_IPC_ATTR_SMOKE || attr == HALITE_IPC_ATTR_BATTERY_LOW) {
        vtype = 0;
        bits = (strcmp(value, "ON") == 0 || strcmp(value, "on") == 0 || strcmp(value, "1") == 0) ? 1 : 0;
    } else if (attr == HALITE_IPC_ATTR_TEMP_C_X100 || attr == HALITE_IPC_ATTR_HUMIDITY_X100) {
        bits = (uint32_t)(int32_t)(atof(value) * 100.0);
    } else if (attr == HALITE_IPC_ATTR_POWER_W_X10) {
        bits = (uint32_t)(int32_t)(atof(value) * 10.0);
    } else if (attr == HALITE_IPC_ATTR_ENERGY_WH) {
        bits = (uint32_t)(int32_t)(atof(value) * 1000.0);
    } else {
        bits = (uint32_t)atoi(value);
    }
    uint8_t ep = dev->endpoint ? dev->endpoint : 1;
    return ipc_host_attr_report(dev->ieee, attr, ep, vtype, bits);
}

esp_err_t ha_discovery_publish_binary_state(const zbgw_device_t *dev, const char *suffix, bool on)
{
    return ha_discovery_publish_sensor_state(dev, suffix, on ? "ON" : "OFF");
}
