#pragma once

#include "device_registry.h"
#include "esp_err.h"

esp_err_t ha_discovery_publish_device(const zbgw_device_t *dev);
esp_err_t ha_discovery_unpublish_device(uint64_t ieee);
esp_err_t ha_discovery_publish_bridge(void);
esp_err_t ha_discovery_publish_sensor_state(const zbgw_device_t *dev, const char *suffix, const char *value);
esp_err_t ha_discovery_publish_binary_state(const zbgw_device_t *dev, const char *suffix, bool on);
