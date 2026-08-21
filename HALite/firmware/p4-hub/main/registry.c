#include "registry.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "halite_ipc.h"

static const char *TAG = "registry";

static halite_entity_t s_ents[HALITE_MAX_ENTITIES];
static registry_changed_cb_t s_cb;
static void *s_cb_ctx;
static bool s_wifi, s_mqtt, s_cloud, s_permit, s_ipc_ok;
static int8_t s_rssi;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void notify(const halite_entity_t *e)
{
    if (s_cb) {
        s_cb(e, s_cb_ctx);
    }
}

static halite_entity_t *alloc_ent(void)
{
    for (int i = 0; i < HALITE_MAX_ENTITIES; i++) {
        if (!s_ents[i].in_use) {
            memset(&s_ents[i], 0, sizeof(s_ents[i]));
            s_ents[i].in_use = true;
            return &s_ents[i];
        }
    }
    return NULL;
}

static halite_entity_t *find_id(const char *id)
{
    for (int i = 0; i < HALITE_MAX_ENTITIES; i++) {
        if (s_ents[i].in_use && strcmp(s_ents[i].entity_id, id) == 0) {
            return &s_ents[i];
        }
    }
    return NULL;
}

static void ieee_hex(uint64_t ieee, char *out, size_t out_len)
{
    snprintf(out, out_len, "%016llx", (unsigned long long)ieee);
}

static halite_entity_t *upsert(const char *entity_id, const char *name, const char *device_id,
                               halite_transport_t tr, halite_domain_t dom, uint32_t cap, uint64_t ieee)
{
    halite_entity_t *e = find_id(entity_id);
    if (!e) {
        e = alloc_ent();
        if (!e) {
            ESP_LOGW(TAG, "registry full");
            return NULL;
        }
        strncpy(e->entity_id, entity_id, sizeof(e->entity_id) - 1);
    }
    strncpy(e->name, name, sizeof(e->name) - 1);
    strncpy(e->device_id, device_id, sizeof(e->device_id) - 1);
    e->transport = tr;
    e->domain = dom;
    e->capabilities = cap;
    e->ieee = ieee;
    e->online = true;
    e->last_seen_ms = now_ms();
    notify(e);
    return e;
}

void registry_init(void)
{
    memset(s_ents, 0, sizeof(s_ents));
}

void registry_set_changed_cb(registry_changed_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}

void registry_on_device_joined(const ipc_device_joined_t *msg)
{
    if (!msg) {
        return;
    }
    char hex[20];
    char devid[32];
    char label[48];
    ieee_hex(msg->ieee, hex, sizeof(hex));
    snprintf(devid, sizeof(devid), "zb_%s", hex);
    if (msg->model[0]) {
        snprintf(label, sizeof(label), "%s", msg->model);
    } else if (msg->manufacturer[0]) {
        snprintf(label, sizeof(label), "%s", msg->manufacturer);
    } else {
        snprintf(label, sizeof(label), "Zigbee %s", hex + 8);
    }

    char eid[64];
    if (msg->capabilities & HALITE_CAP_ON_OFF) {
        snprintf(eid, sizeof(eid), "switch.zb_%s", hex);
        upsert(eid, label, devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_SWITCH, HALITE_CAP_ON_OFF, msg->ieee);
    }
    if (msg->capabilities & HALITE_CAP_TEMPERATURE) {
        snprintf(eid, sizeof(eid), "sensor.zb_%s_temperature", hex);
        upsert(eid, "Temperature", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_SENSOR, HALITE_CAP_TEMPERATURE,
               msg->ieee);
        halite_entity_t *e = find_id(eid);
        if (e) {
            strncpy(e->unit, "°C", sizeof(e->unit) - 1);
        }
    }
    if (msg->capabilities & HALITE_CAP_HUMIDITY) {
        snprintf(eid, sizeof(eid), "sensor.zb_%s_humidity", hex);
        upsert(eid, "Humidity", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_SENSOR, HALITE_CAP_HUMIDITY, msg->ieee);
        halite_entity_t *e = find_id(eid);
        if (e) {
            strncpy(e->unit, "%", sizeof(e->unit) - 1);
        }
    }
    if (msg->capabilities & HALITE_CAP_POWER) {
        snprintf(eid, sizeof(eid), "sensor.zb_%s_power", hex);
        upsert(eid, "Power", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_SENSOR, HALITE_CAP_POWER, msg->ieee);
        halite_entity_t *e = find_id(eid);
        if (e) {
            strncpy(e->unit, "W", sizeof(e->unit) - 1);
        }
    }
    if (msg->capabilities & HALITE_CAP_ENERGY) {
        snprintf(eid, sizeof(eid), "sensor.zb_%s_energy", hex);
        upsert(eid, "Energy", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_SENSOR, HALITE_CAP_ENERGY, msg->ieee);
        halite_entity_t *e = find_id(eid);
        if (e) {
            strncpy(e->unit, "kWh", sizeof(e->unit) - 1);
        }
    }
    if (msg->capabilities & HALITE_CAP_CONTACT) {
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_contact", hex);
        upsert(eid, "Contact", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_BINARY, HALITE_CAP_CONTACT, msg->ieee);
    }
    if (msg->capabilities & HALITE_CAP_OCCUPANCY) {
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_occupancy", hex);
        upsert(eid, "Occupancy", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_BINARY, HALITE_CAP_OCCUPANCY, msg->ieee);
    }
    if (msg->capabilities & HALITE_CAP_SMOKE) {
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_smoke", hex);
        upsert(eid, "Smoke", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_BINARY, HALITE_CAP_SMOKE, msg->ieee);
    }
    if (msg->capabilities & HALITE_CAP_BATTERY) {
        snprintf(eid, sizeof(eid), "sensor.zb_%s_battery", hex);
        upsert(eid, "Battery", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_SENSOR, HALITE_CAP_BATTERY, msg->ieee);
        halite_entity_t *e = find_id(eid);
        if (e) {
            strncpy(e->unit, "%", sizeof(e->unit) - 1);
        }
    }
    if (msg->capabilities & HALITE_CAP_BATTERY_LOW) {
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_battery_low", hex);
        upsert(eid, "Battery low", devid, HALITE_TRANSPORT_ZIGBEE, HALITE_DOMAIN_BINARY, HALITE_CAP_BATTERY_LOW,
               msg->ieee);
    }
    ESP_LOGI(TAG, "joined ieee=%s caps=0x%lx", hex, (unsigned long)msg->capabilities);
}

void registry_on_device_left(uint64_t ieee)
{
    for (int i = 0; i < HALITE_MAX_ENTITIES; i++) {
        if (s_ents[i].in_use && s_ents[i].ieee == ieee && s_ents[i].transport == HALITE_TRANSPORT_ZIGBEE) {
            s_ents[i].in_use = false;
            notify(&s_ents[i]);
        }
    }
}

void registry_on_attr_report(const ipc_attr_report_t *msg)
{
    if (!msg) {
        return;
    }
    char hex[20];
    char eid[64];
    ieee_hex(msg->ieee, hex, sizeof(hex));
    switch (msg->attr_id) {
    case HALITE_IPC_ATTR_ON_OFF:
        snprintf(eid, sizeof(eid), "switch.zb_%s", hex);
        break;
    case HALITE_IPC_ATTR_TEMP_C_X100:
        snprintf(eid, sizeof(eid), "sensor.zb_%s_temperature", hex);
        break;
    case HALITE_IPC_ATTR_HUMIDITY_X100:
        snprintf(eid, sizeof(eid), "sensor.zb_%s_humidity", hex);
        break;
    case HALITE_IPC_ATTR_POWER_W_X10:
        snprintf(eid, sizeof(eid), "sensor.zb_%s_power", hex);
        break;
    case HALITE_IPC_ATTR_ENERGY_WH:
        snprintf(eid, sizeof(eid), "sensor.zb_%s_energy", hex);
        break;
    case HALITE_IPC_ATTR_BATTERY_PCT:
        snprintf(eid, sizeof(eid), "sensor.zb_%s_battery", hex);
        break;
    case HALITE_IPC_ATTR_CONTACT:
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_contact", hex);
        break;
    case HALITE_IPC_ATTR_OCCUPANCY:
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_occupancy", hex);
        break;
    case HALITE_IPC_ATTR_SMOKE:
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_smoke", hex);
        break;
    case HALITE_IPC_ATTR_BATTERY_LOW:
        snprintf(eid, sizeof(eid), "binary_sensor.zb_%s_battery_low", hex);
        break;
    default:
        return;
    }

    halite_entity_t *e = find_id(eid);
    if (!e) {
        /* Late report before DEVICE_JOINED — synthesize a minimal join. */
        ipc_device_joined_t fake = {.ieee = msg->ieee, .capabilities = 0};
        if (msg->attr_id == HALITE_IPC_ATTR_ON_OFF) {
            fake.capabilities = HALITE_CAP_ON_OFF;
        }
        registry_on_device_joined(&fake);
        e = find_id(eid);
        if (!e) {
            return;
        }
    }
    e->last_seen_ms = now_ms();
    e->online = true;
    if (e->domain == HALITE_DOMAIN_SENSOR) {
        if (msg->attr_id == HALITE_IPC_ATTR_POWER_W_X10) {
            e->value_x100 = (int32_t)msg->value_bits * 10; /* deci-W → x100 W */
        } else if (msg->attr_id == HALITE_IPC_ATTR_ENERGY_WH) {
            e->value_x100 = (int32_t)((int32_t)msg->value_bits); /* store Wh; UI divides */
        } else {
            e->value_x100 = (int32_t)msg->value_bits;
        }
    } else {
        e->on = (msg->value_bits & 1u) != 0;
    }
    notify(e);
}

void registry_on_esphome_entity(const ipc_esphome_entity_t *msg)
{
    if (!msg || !msg->entity_id[0]) {
        return;
    }
    char devid[48];
    snprintf(devid, sizeof(devid), "esphome_%s", msg->node_name[0] ? msg->node_name : "node");
    halite_domain_t dom = HALITE_DOMAIN_SWITCH;
    if (msg->domain == 2) {
        dom = HALITE_DOMAIN_BINARY;
    } else if (msg->domain == 3) {
        dom = HALITE_DOMAIN_SENSOR;
    }
    upsert(msg->entity_id, msg->entity_id, devid, HALITE_TRANSPORT_ESPHOME, dom, HALITE_CAP_ON_OFF, 0);
}

void registry_on_esphome_state(const ipc_esphome_state_t *msg)
{
    if (!msg) {
        return;
    }
    halite_entity_t *e = find_id(msg->entity_id);
    if (!e) {
        ipc_esphome_entity_t fake = {0};
        strncpy(fake.entity_id, msg->entity_id, sizeof(fake.entity_id) - 1);
        fake.domain = 1;
        registry_on_esphome_entity(&fake);
        e = find_id(msg->entity_id);
        if (!e) {
            return;
        }
    }
    e->last_seen_ms = now_ms();
    e->online = true;
    if (msg->value_type == 0) {
        e->on = (msg->value_bits & 1u) != 0;
    } else {
        e->value_x100 = (int32_t)msg->value_bits;
    }
    notify(e);
}

int registry_count(void)
{
    int n = 0;
    for (int i = 0; i < HALITE_MAX_ENTITIES; i++) {
        if (s_ents[i].in_use) {
            n++;
        }
    }
    return n;
}

const halite_entity_t *registry_get(int index)
{
    int n = 0;
    for (int i = 0; i < HALITE_MAX_ENTITIES; i++) {
        if (!s_ents[i].in_use) {
            continue;
        }
        if (n == index) {
            return &s_ents[i];
        }
        n++;
    }
    return NULL;
}

const halite_entity_t *registry_find(const char *entity_id)
{
    return find_id(entity_id);
}

void registry_set_net(bool wifi_up, bool mqtt_up, bool cloud_up, int8_t rssi)
{
    s_wifi = wifi_up;
    s_mqtt = mqtt_up;
    s_cloud = cloud_up;
    s_rssi = rssi;
}

void registry_get_net(bool *wifi_up, bool *mqtt_up, bool *cloud_up, int8_t *rssi)
{
    if (wifi_up) {
        *wifi_up = s_wifi;
    }
    if (mqtt_up) {
        *mqtt_up = s_mqtt;
    }
    if (cloud_up) {
        *cloud_up = s_cloud;
    }
    if (rssi) {
        *rssi = s_rssi;
    }
}

void registry_set_permit_join(bool enabled)
{
    s_permit = enabled;
}

bool registry_permit_join(void)
{
    return s_permit;
}

void registry_set_ipc_ok(bool ok)
{
    s_ipc_ok = ok;
}

bool registry_ipc_ok(void)
{
    return s_ipc_ok;
}
