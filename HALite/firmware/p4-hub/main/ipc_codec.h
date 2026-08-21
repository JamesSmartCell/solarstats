#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "halite_ipc.h"

#define HALITE_IPC_HDR_LEN 10
#define HALITE_IPC_CRC_LEN 2
#define HALITE_IPC_MAX_FRAME (HALITE_IPC_HDR_LEN + HALITE_IPC_MAX_PAYLOAD + HALITE_IPC_CRC_LEN)

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t len;
    const uint8_t *payload;
} ipc_frame_view_t;

typedef struct __attribute__((packed)) {
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t reserved;
    uint32_t capabilities;
    char manufacturer[32];
    char model[32];
} ipc_device_joined_t;

typedef struct __attribute__((packed)) {
    uint64_t ieee;
} ipc_device_left_t;

typedef struct __attribute__((packed)) {
    uint64_t ieee;
    uint8_t attr_id;
    uint8_t ep;
    uint8_t value_type;
    uint8_t reserved;
    uint32_t value_bits;
} ipc_attr_report_t;

typedef struct __attribute__((packed)) {
    uint64_t ieee;
    uint8_t on;
    uint8_t reserved[3];
} ipc_cmd_set_on_off_t;

typedef struct __attribute__((packed)) {
    uint64_t ieee;
} ipc_cmd_remove_t;

typedef struct __attribute__((packed)) {
    uint8_t enable;
    uint8_t seconds;
} ipc_permit_join_t;

typedef struct __attribute__((packed)) {
    uint8_t enabled;
} ipc_permit_join_state_t;

typedef struct __attribute__((packed)) {
    char entity_id[48];
    char node_name[32];
    uint8_t domain;
    uint8_t reserved[3];
    char state_topic[96];
    char command_topic[96];
} ipc_esphome_entity_t;

typedef struct __attribute__((packed)) {
    char entity_id[48];
    uint8_t value_type;
    uint8_t reserved[3];
    uint32_t value_bits;
} ipc_esphome_state_t;

typedef struct __attribute__((packed)) {
    char entity_id[48];
    uint8_t on;
    uint8_t reserved[3];
} ipc_cmd_esphome_set_t;

typedef struct __attribute__((packed)) {
    uint8_t wifi_up;
    uint8_t mqtt_up;
    uint8_t cloud_up;
    uint8_t reserved;
    int8_t wifi_rssi;
    uint8_t reserved2[3];
} ipc_net_status_t;

typedef struct __attribute__((packed)) {
    uint16_t req_seq;
    int32_t status;
} ipc_cmd_result_t;

size_t ipc_encode_frame(uint8_t *out, size_t out_len, uint8_t type, uint8_t flags, uint16_t seq,
                        const void *payload, uint16_t payload_len);
esp_err_t ipc_decode_frame(const uint8_t *buf, size_t buf_len, ipc_frame_view_t *view);
/* Returns bytes consumed if a complete valid frame starts at buf[0], else 0.
 * If magic found later, *skip is set to that offset. */
size_t ipc_try_parse(const uint8_t *buf, size_t buf_len, ipc_frame_view_t *view, size_t *skip);
