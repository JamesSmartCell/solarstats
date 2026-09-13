#pragma once

#include "esp_err.h"

/* HTTPS OTA from CONFIG_ZBGW_OTA_URL. Safe to call from MQTT / a task. */
esp_err_t ota_update_start(void);
void ota_update_schedule_boot_check(void);
