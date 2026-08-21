#pragma once

#include "esp_err.h"

/** ESP-Hosted slave on SDIO. P4 drives Wi-Fi; Zigbee stays local. */
esp_err_t hosted_slave_start(void);
