#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ipc_codec.h"

esp_err_t ipc_host_start(void);
bool ipc_host_is_up(void);

esp_err_t ipc_host_send(uint8_t type, uint8_t flags, const void *payload, uint16_t len);
esp_err_t ipc_host_send_ack(uint16_t seq, uint8_t type);
esp_err_t ipc_host_send_cmd_result(uint16_t req_seq, int32_t status);

esp_err_t ipc_host_device_joined(const ipc_device_joined_t *msg);
esp_err_t ipc_host_device_left(uint64_t ieee);
esp_err_t ipc_host_attr_report(uint64_t ieee, uint8_t attr_id, uint8_t ep, uint8_t value_type, uint32_t value_bits);
esp_err_t ipc_host_permit_join_state(bool enabled);
esp_err_t ipc_host_esphome_entity(const ipc_esphome_entity_t *msg);
esp_err_t ipc_host_esphome_state(const char *entity_id, uint8_t value_type, uint32_t value_bits);
esp_err_t ipc_host_net_status(bool wifi_up, bool mqtt_up, int8_t rssi);
