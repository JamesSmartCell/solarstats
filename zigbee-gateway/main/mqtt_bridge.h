#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef void (*mqtt_bridge_permit_join_cb_t)(bool enable);
typedef void (*mqtt_bridge_switch_cb_t)(uint64_t ieee, bool on);
typedef void (*mqtt_bridge_remove_cb_t)(uint64_t ieee, bool all_switches);
typedef void (*mqtt_bridge_rediscover_cb_t)(void);

esp_err_t mqtt_bridge_start(mqtt_bridge_permit_join_cb_t permit_cb, mqtt_bridge_switch_cb_t switch_cb,
                            mqtt_bridge_remove_cb_t remove_cb, mqtt_bridge_rediscover_cb_t rediscover_cb);
void mqtt_bridge_suspend(void);
void mqtt_bridge_resume(void);
bool mqtt_bridge_is_connected(void);

esp_err_t mqtt_bridge_publish(const char *topic, const char *payload, int qos, bool retain);
esp_err_t mqtt_bridge_publish_status(const char *status);
esp_err_t mqtt_bridge_publish_permit_state(bool open);
esp_err_t mqtt_bridge_publish_info(const char *json);
