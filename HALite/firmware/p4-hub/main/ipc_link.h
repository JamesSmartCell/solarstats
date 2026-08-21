#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ipc_link_start(void);
bool ipc_link_is_up(void);

esp_err_t ipc_link_ping(void);
esp_err_t ipc_link_permit_join(bool enable, uint8_t seconds);
esp_err_t ipc_link_set_on_off(uint64_t ieee, bool on);
esp_err_t ipc_link_esphome_set(const char *entity_id, bool on);
esp_err_t ipc_link_rediscover(void);
