#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define WIFI_NET_CONNECTED_BIT BIT0
#define WIFI_NET_FAIL_BIT      BIT1

esp_err_t wifi_net_start(void);
bool wifi_net_is_connected(void);
esp_err_t wifi_net_wait_connected(TickType_t ticks_to_wait);
/* Network-byte-order IPv4 + netmask for the STA interface. */
bool wifi_net_get_sta_ipv4(uint32_t *ip_addr, uint32_t *netmask);

/* Pause Wi‑Fi so Zigbee can use the RF path during pairing. */
esp_err_t wifi_net_pause_for_zigbee(void);
esp_err_t wifi_net_resume(void);
bool wifi_net_is_paused(void);
/* No modem sleep while MQTT is down; TX follows RSSI. Relax PS after MQTT is up. */
void wifi_net_on_mqtt_up(void);
void wifi_net_on_mqtt_down(void);
/* Drop modem sleep so 802.15.4 can TX; restore MIN_MODEM after hold_ms. */
void wifi_net_zigbee_tx_hold(uint32_t hold_ms);
