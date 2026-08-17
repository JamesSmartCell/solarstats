#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t mqtt_bridge_start(void);
void mqtt_bridge_suspend(void);
void mqtt_bridge_resume(void);
bool mqtt_bridge_is_connected(void);

esp_err_t mqtt_bridge_publish(const char *topic, const char *payload, int qos, bool retain);
esp_err_t mqtt_bridge_publish_status(const char *status);
esp_err_t mqtt_bridge_publish_permit_state(bool open);
esp_err_t mqtt_bridge_publish_info(const char *json);
esp_err_t mqtt_bridge_esphome_set(const char *entity_id, bool on);
void mqtt_bridge_announce_entities(void);
