#include "hub_json.h"

#include <stdio.h>
#include <stdlib.h>

static const char *domain_str(halite_domain_t d)
{
    if (d == HALITE_DOMAIN_SWITCH) {
        return "switch";
    }
    if (d == HALITE_DOMAIN_BINARY) {
        return "binary_sensor";
    }
    return "sensor";
}

static const char *transport_str(halite_transport_t t)
{
    return t == HALITE_TRANSPORT_ESPHOME ? "esphome" : "zigbee";
}

cJSON *hub_json_entity(const halite_entity_t *e)
{
    cJSON *o = cJSON_CreateObject();
    if (!e) {
        return o;
    }
    cJSON_AddStringToObject(o, "entity_id", e->entity_id);
    cJSON_AddStringToObject(o, "domain", domain_str(e->domain));
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "transport", transport_str(e->transport));
    cJSON_AddStringToObject(o, "device_id", e->device_id);
    cJSON_AddNumberToObject(o, "capabilities", e->capabilities);
    cJSON_AddBoolToObject(o, "online", e->online);
    cJSON_AddNumberToObject(o, "last_seen", (double)e->last_seen_ms);
    cJSON_AddBoolToObject(o, "on", e->on);
    if (e->domain == HALITE_DOMAIN_SENSOR) {
        if (e->capabilities & HALITE_CAP_ENERGY) {
            cJSON_AddNumberToObject(o, "state", (double)e->value_x100 / 1000.0);
        } else if (e->capabilities & (HALITE_CAP_TEMPERATURE | HALITE_CAP_HUMIDITY | HALITE_CAP_POWER)) {
            cJSON_AddNumberToObject(o, "state", (double)e->value_x100 / 100.0);
        } else {
            cJSON_AddNumberToObject(o, "state", (double)e->value_x100);
        }
        if (e->unit[0]) {
            cJSON_AddStringToObject(o, "unit", e->unit);
        }
    } else {
        cJSON_AddStringToObject(o, "state", e->on ? "on" : "off");
    }
    return o;
}

cJSON *hub_json_entities(void)
{
    cJSON *arr = cJSON_CreateArray();
    int n = registry_count();
    for (int i = 0; i < n; i++) {
        const halite_entity_t *e = registry_get(i);
        if (e) {
            cJSON_AddItemToArray(arr, hub_json_entity(e));
        }
    }
    return arr;
}

cJSON *hub_json_status(void)
{
    bool wifi = false, mqtt = false, cloud = false;
    int8_t rssi = 0;
    registry_get_net(&wifi, &mqtt, &cloud, &rssi);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "ipc_c6", registry_ipc_ok());
    cJSON_AddBoolToObject(o, "permit_join", registry_permit_join());
    cJSON_AddBoolToObject(o, "wifi", wifi);
    cJSON_AddBoolToObject(o, "mqtt", mqtt);
    cJSON_AddBoolToObject(o, "cloud", cloud);
    cJSON_AddNumberToObject(o, "wifi_rssi", rssi);
    cJSON_AddItemToObject(o, "entities", hub_json_entities());
    return o;
}

void hub_json_print_line(cJSON *obj)
{
    char *s = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (s) {
        printf("HALITE_JSON:%s\n", s);
        free(s);
    }
}
