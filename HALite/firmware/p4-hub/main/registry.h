#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ipc_codec.h"

#define HALITE_MAX_ENTITIES 64

#define HALITE_CAP_TEMPERATURE (1U << 0)
#define HALITE_CAP_HUMIDITY    (1U << 1)
#define HALITE_CAP_CONTACT     (1U << 2)
#define HALITE_CAP_OCCUPANCY   (1U << 3)
#define HALITE_CAP_ON_OFF      (1U << 4)
#define HALITE_CAP_POWER       (1U << 5)
#define HALITE_CAP_ENERGY      (1U << 6)
#define HALITE_CAP_SMOKE       (1U << 7)
#define HALITE_CAP_BATTERY     (1U << 8)
#define HALITE_CAP_TAMPER      (1U << 9)
#define HALITE_CAP_BATTERY_LOW (1U << 11)

typedef enum {
    HALITE_TRANSPORT_ZIGBEE = 0,
    HALITE_TRANSPORT_ESPHOME = 1,
} halite_transport_t;

typedef enum {
    HALITE_DOMAIN_SWITCH = 1,
    HALITE_DOMAIN_BINARY = 2,
    HALITE_DOMAIN_SENSOR = 3,
} halite_domain_t;

typedef struct {
    char entity_id[64];
    char name[48];
    char device_id[48];
    halite_transport_t transport;
    halite_domain_t domain;
    uint32_t capabilities;
    uint64_t ieee;
    bool online;
    bool on;
    int32_t value_x100; /* sensors */
    char unit[8];
    int64_t last_seen_ms;
    bool in_use;
} halite_entity_t;

typedef void (*registry_changed_cb_t)(const halite_entity_t *e, void *ctx);

void registry_init(void);
void registry_set_changed_cb(registry_changed_cb_t cb, void *ctx);

void registry_on_device_joined(const ipc_device_joined_t *msg);
void registry_on_device_left(uint64_t ieee);
void registry_on_attr_report(const ipc_attr_report_t *msg);
void registry_on_esphome_entity(const ipc_esphome_entity_t *msg);
void registry_on_esphome_state(const ipc_esphome_state_t *msg);

int registry_count(void);
const halite_entity_t *registry_get(int index);
const halite_entity_t *registry_find(const char *entity_id);

void registry_set_net(bool wifi_up, bool mqtt_up, bool cloud_up, int8_t rssi);
void registry_get_net(bool *wifi_up, bool *mqtt_up, bool *cloud_up, int8_t *rssi);
void registry_set_permit_join(bool enabled);
bool registry_permit_join(void);
void registry_set_ipc_ok(bool ok);
bool registry_ipc_ok(void);
