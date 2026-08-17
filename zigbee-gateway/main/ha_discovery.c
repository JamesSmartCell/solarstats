#include "ha_discovery.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_log.h"
#include "mqtt_bridge.h"
#include "sdkconfig.h"

static const char *TAG = "ha_disc";

static void add_device_object(cJSON *root, const zbgw_device_t *dev)
{
    char ieee[20];
    device_registry_ieee_to_str(dev->ieee, ieee, sizeof(ieee));

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON *ids = cJSON_AddArrayToObject(device, "identifiers");
    char id[48];
    snprintf(id, sizeof(id), "zbgw_%s", ieee);
    cJSON_AddItemToArray(ids, cJSON_CreateString(id));
    const char *fallback_name = (dev->capabilities & ZBGW_CAP_ON_OFF) ? "Zigbee Switch" : "Zigbee Sensor";
    cJSON_AddStringToObject(device, "name", dev->model[0] ? dev->model : fallback_name);
    cJSON_AddStringToObject(device, "manufacturer", dev->manufacturer[0] ? dev->manufacturer : "Zigbee");
    cJSON_AddStringToObject(device, "model", dev->model[0] ? dev->model : "device");
    cJSON_AddStringToObject(device, "via_device", "zbgw_bridge");
}

static esp_err_t publish_config(const char *component, const char *object_id, cJSON *root)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/%s/config", ZBGW_HA_DISCOVERY_PREFIX, component, object_id);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = mqtt_bridge_publish(topic, payload, 0, true);
    cJSON_free(payload);
    return err;
}

static esp_err_t publish_sensor(const zbgw_device_t *dev, const char *suffix, const char *name, const char *device_class,
                                const char *unit, const char *state_class)
{
    char ieee[20];
    device_registry_ieee_to_str(dev->ieee, ieee, sizeof(ieee));

    char object_id[64];
    snprintf(object_id, sizeof(object_id), "zbgw_%s_%s", ieee, suffix);

    char state_topic[96];
    snprintf(state_topic, sizeof(state_topic), "%s/%s/%s", ZBGW_TOPIC_PREFIX, ieee, suffix);

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "zbgw_%s_%s", ieee, suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    if (device_class) {
        cJSON_AddStringToObject(root, "device_class", device_class);
    }
    if (unit) {
        cJSON_AddStringToObject(root, "unit_of_measurement", unit);
    }
    if (state_class) {
        cJSON_AddStringToObject(root, "state_class", state_class);
    }
    cJSON_AddStringToObject(root, "availability_topic", ZBGW_TOPIC_STATUS);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    add_device_object(root, dev);
    return publish_config("sensor", object_id, root);
}

static esp_err_t publish_binary(const zbgw_device_t *dev, const char *suffix, const char *name, const char *device_class)
{
    char ieee[20];
    device_registry_ieee_to_str(dev->ieee, ieee, sizeof(ieee));

    char object_id[64];
    snprintf(object_id, sizeof(object_id), "zbgw_%s_%s", ieee, suffix);

    char state_topic[96];
    snprintf(state_topic, sizeof(state_topic), "%s/%s/%s", ZBGW_TOPIC_PREFIX, ieee, suffix);

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "zbgw_%s_%s", ieee, suffix);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    if (device_class) {
        cJSON_AddStringToObject(root, "device_class", device_class);
    }
    cJSON_AddStringToObject(root, "availability_topic", ZBGW_TOPIC_STATUS);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    add_device_object(root, dev);
    return publish_config("binary_sensor", object_id, root);
}

static esp_err_t publish_switch(const zbgw_device_t *dev)
{
    char ieee[20];
    device_registry_ieee_to_str(dev->ieee, ieee, sizeof(ieee));

    char object_id[64];
    snprintf(object_id, sizeof(object_id), "zbgw_%s_switch", ieee);

    char state_topic[96];
    snprintf(state_topic, sizeof(state_topic), "%s/%s/switch", ZBGW_TOPIC_PREFIX, ieee);

    char command_topic[96];
    snprintf(command_topic, sizeof(command_topic), "%s/%s/switch/set", ZBGW_TOPIC_PREFIX, ieee);

    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "zbgw_%s_switch", ieee);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Switch");
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "state_on", "ON");
    cJSON_AddStringToObject(root, "state_off", "OFF");
    cJSON_AddBoolToObject(root, "optimistic", false);
    cJSON_AddStringToObject(root, "availability_topic", ZBGW_TOPIC_STATUS);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    add_device_object(root, dev);
    return publish_config("switch", object_id, root);
}

static esp_err_t unpublish_config(const char *component, const char *object_id)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s/%s/config", ZBGW_HA_DISCOVERY_PREFIX, component, object_id);
    /* Empty retained payload removes the entity from Home Assistant. */
    return mqtt_bridge_publish(topic, "", 1, true);
}

esp_err_t ha_discovery_publish_device(const zbgw_device_t *dev)
{
    if (!dev || !dev->in_use) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    if (dev->capabilities & ZBGW_CAP_TEMPERATURE) {
        err |= publish_sensor(dev, "temperature", "Temperature", "temperature", "°C", "measurement");
    }
    if (dev->capabilities & ZBGW_CAP_HUMIDITY) {
        err |= publish_sensor(dev, "humidity", "Humidity", "humidity", "%", "measurement");
    }
    if (dev->capabilities & ZBGW_CAP_CONTACT) {
        err |= publish_binary(dev, "contact", "Contact", "door");
        /* Door sensors: only Contact + Battery low. Drop leftover retained entities. */
        char ieee[20];
        char object_id[64];
        device_registry_ieee_to_str(dev->ieee, ieee, sizeof(ieee));
        snprintf(object_id, sizeof(object_id), "zbgw_%s_tamper", ieee);
        err |= unpublish_config("binary_sensor", object_id);
        snprintf(object_id, sizeof(object_id), "zbgw_%s_test", ieee);
        err |= unpublish_config("binary_sensor", object_id);
        snprintf(object_id, sizeof(object_id), "zbgw_%s_battery", ieee);
        err |= unpublish_config("sensor", object_id);
        err |= ha_discovery_publish_binary_state(dev, "battery_low", false);
    }
    if (dev->capabilities & ZBGW_CAP_OCCUPANCY) {
        err |= publish_binary(dev, "occupancy", "Occupancy", "occupancy");
    }
    if (dev->capabilities & ZBGW_CAP_SMOKE) {
        err |= publish_binary(dev, "smoke", "Smoke", "smoke");
    }
    if (dev->capabilities & ZBGW_CAP_TAMPER) {
        err |= publish_binary(dev, "tamper", "Tamper", "tamper");
    }
    if (dev->capabilities & ZBGW_CAP_SMOKE_TEST) {
        err |= publish_binary(dev, "test", "Self-test", NULL);
    }
    if (dev->capabilities & ZBGW_CAP_BATTERY_LOW) {
        err |= publish_binary(dev, "battery_low", "Battery low", "battery");
    }
    if (dev->capabilities & ZBGW_CAP_BATTERY) {
        err |= publish_sensor(dev, "battery", "Battery", "battery", "%", "measurement");
    }
    if (dev->capabilities & ZBGW_CAP_ON_OFF) {
        err |= publish_switch(dev);
    }
    if (dev->capabilities & ZBGW_CAP_POWER) {
        err |= publish_sensor(dev, "power", "Power", "power", "W", "measurement");
    }
    if (dev->capabilities & ZBGW_CAP_ENERGY) {
        err |= publish_sensor(dev, "energy", "Energy", "energy", "kWh", "total_increasing");
    }
    return err;
}

esp_err_t ha_discovery_unpublish_device(uint64_t ieee)
{
    char ieee_str[20];
    device_registry_ieee_to_str(ieee, ieee_str, sizeof(ieee_str));

    char object_id[64];
    char state_topic[96];
    esp_err_t err = ESP_OK;

    snprintf(object_id, sizeof(object_id), "zbgw_%s_temperature", ieee_str);
    err |= unpublish_config("sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_humidity", ieee_str);
    err |= unpublish_config("sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_contact", ieee_str);
    err |= unpublish_config("binary_sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_occupancy", ieee_str);
    err |= unpublish_config("binary_sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_smoke", ieee_str);
    err |= unpublish_config("binary_sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_tamper", ieee_str);
    err |= unpublish_config("binary_sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_test", ieee_str);
    err |= unpublish_config("binary_sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_battery_low", ieee_str);
    err |= unpublish_config("binary_sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_battery", ieee_str);
    err |= unpublish_config("sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_switch", ieee_str);
    err |= unpublish_config("switch", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_power", ieee_str);
    err |= unpublish_config("sensor", object_id);
    snprintf(object_id, sizeof(object_id), "zbgw_%s_energy", ieee_str);
    err |= unpublish_config("sensor", object_id);

    /* Clear retained state topics so HA does not revive stale values. */
    static const char *suffixes[] = {"temperature", "humidity", "contact", "occupancy", "smoke", "tamper",
                                     "test",        "battery_low", "battery", "switch",  "power", "energy"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        snprintf(state_topic, sizeof(state_topic), "%s/%s/%s", ZBGW_TOPIC_PREFIX, ieee_str, suffixes[i]);
        err |= mqtt_bridge_publish(state_topic, "", 1, true);
    }

    ESP_LOGI(TAG, "Unpublished discovery+state for ieee=%s", ieee_str);
    return err;
}

esp_err_t ha_discovery_publish_bridge(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Permit join");
    cJSON_AddStringToObject(root, "unique_id", "zbgw_bridge_permit_join");
    cJSON_AddStringToObject(root, "command_topic", ZBGW_TOPIC_PERMIT_JOIN);
    cJSON_AddStringToObject(root, "state_topic", ZBGW_TOPIC_PERMIT_STATE);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddBoolToObject(root, "optimistic", false);
    cJSON_AddStringToObject(root, "availability_topic", ZBGW_TOPIC_STATUS);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON_AddStringToObject(root, "icon", "mdi:zigbee");

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON *ids = cJSON_AddArrayToObject(device, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString("zbgw_bridge"));
    cJSON_AddStringToObject(device, "name", "ESP32-C6 Zigbee Gateway");
    cJSON_AddStringToObject(device, "manufacturer", "DFRobot / Espressif");
    cJSON_AddStringToObject(device, "model", "FireBeetle 2 ESP32-C6");
    cJSON_AddStringToObject(device, "sw_version", "1.0.0");

    return publish_config("switch", "zbgw_bridge_permit_join", root);
}

esp_err_t ha_discovery_publish_sensor_state(const zbgw_device_t *dev, const char *suffix, const char *value)
{
    if (!dev || !suffix || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    char ieee[20];
    device_registry_ieee_to_str(dev->ieee, ieee, sizeof(ieee));
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s/%s", ZBGW_TOPIC_PREFIX, ieee, suffix);
    return mqtt_bridge_publish(topic, value, 1, true);
}

esp_err_t ha_discovery_publish_binary_state(const zbgw_device_t *dev, const char *suffix, bool on)
{
    return ha_discovery_publish_sensor_state(dev, suffix, on ? "ON" : "OFF");
}
