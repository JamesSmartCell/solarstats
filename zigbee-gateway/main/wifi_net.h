#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define WIFI_NET_CONNECTED_BIT BIT0
#define WIFI_NET_FAIL_BIT      BIT1

esp_err_t wifi_net_start(void);
bool wifi_net_is_connected(void);
esp_err_t wifi_net_wait_connected(TickType_t ticks_to_wait);

/* Pause Wi‑Fi so Zigbee can use the RF path during pairing. */
esp_err_t wifi_net_pause_for_zigbee(void);
esp_err_t wifi_net_resume(void);
bool wifi_net_is_paused(void);
