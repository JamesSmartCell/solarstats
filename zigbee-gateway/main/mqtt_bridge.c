#include "mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "device_registry.h"
#include "esp_log.h"
#include "ha_discovery.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_bridge";

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static mqtt_bridge_permit_join_cb_t s_permit_cb;
static mqtt_bridge_switch_cb_t s_switch_cb;
static mqtt_bridge_remove_cb_t s_remove_cb;
static mqtt_bridge_rediscover_cb_t s_rediscover_cb;

static void rediscover_device_cb(zbgw_device_t *dev, void *ctx)
{
    (void)ctx;
    if (dev) {
        ha_discovery_publish_device(dev);
        dev->discovery_published = true;
    }
}

static bool payload_to_bool(const char *data, int len, bool *out)
{
    if (!data || len <= 0 || !out) {
        return false;
    }

    char buf[16] = {0};
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);

    if (strncmp(buf, "ON", 2) == 0 || strncmp(buf, "on", 2) == 0 || strncmp(buf, "1", 1) == 0 ||
        strncmp(buf, "true", 4) == 0 || strncmp(buf, "True", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(buf, "OFF", 3) == 0 || strncmp(buf, "off", 3) == 0 || strncmp(buf, "0", 1) == 0 ||
        strncmp(buf, "false", 5) == 0 || strncmp(buf, "False", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_ieee_payload(const char *data, int len, uint64_t *ieee)
{
    if (!data || len <= 0 || !ieee) {
        return false;
    }
    char buf[24] = {0};
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    /* Trim whitespace/newlines */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    const char *p = buf;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    return device_registry_ieee_from_str(p, ieee);
}

static void handle_permit_join_payload(const char *data, int len)
{
    if (!s_permit_cb) {
        return;
    }
    bool enable = false;
    if (!payload_to_bool(data, len, &enable)) {
        ESP_LOGW(TAG, "Unknown permit_join payload: %.*s", len, data);
        return;
    }
    s_permit_cb(enable);
}

static void handle_switch_set_topic(const char *topic, int topic_len, const char *data, int data_len)
{
    if (!s_switch_cb || !topic || topic_len <= 0) {
        return;
    }

    /* Expected: <prefix>/<16-hex-ieee>/switch/set */
    const char *prefix = ZBGW_TOPIC_PREFIX;
    size_t prefix_len = strlen(prefix);
    if ((size_t)topic_len < prefix_len + 1 + 16 + strlen("/switch/set")) {
        return;
    }
    if (strncmp(topic, prefix, prefix_len) != 0 || topic[prefix_len] != '/') {
        return;
    }

    const char *ieee_start = topic + prefix_len + 1;
    if (strncmp(ieee_start + 16, "/switch/set", 11) != 0) {
        return;
    }

    char ieee_str[17] = {0};
    memcpy(ieee_str, ieee_start, 16);
    uint64_t ieee = 0;
    if (!device_registry_ieee_from_str(ieee_str, &ieee)) {
        ESP_LOGW(TAG, "Bad IEEE in switch topic: %s", ieee_str);
        return;
    }

    bool on = false;
    if (!payload_to_bool(data, data_len, &on)) {
        ESP_LOGW(TAG, "Unknown switch payload: %.*s", data_len, data);
        return;
    }
    s_switch_cb(ieee, on);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected - publishing HA discovery");
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_PERMIT_JOIN, 1);
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_SWITCH_SET_WILDCARD, 1);
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_REMOVE, 1);
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_REDISCOVER, 1);
        mqtt_bridge_publish_status("online");
        mqtt_bridge_publish_permit_state(false);
        /* Announce bridge immediately so HA shows the device even before Zigbee is ready */
        ha_discovery_publish_bridge();
        device_registry_foreach(rediscover_device_cb, NULL);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(ZBGW_TOPIC_PERMIT_JOIN) &&
            strncmp(event->topic, ZBGW_TOPIC_PERMIT_JOIN, event->topic_len) == 0) {
            handle_permit_join_payload(event->data, event->data_len);
        } else if (event->topic_len == (int)strlen(ZBGW_TOPIC_REMOVE) &&
                   strncmp(event->topic, ZBGW_TOPIC_REMOVE, event->topic_len) == 0) {
            if (!s_remove_cb) {
                break;
            }
            /* Payloads: "<16-hex-ieee>" | "all" | "switches" */
            char buf[24] = {0};
            int n = event->data_len < (int)sizeof(buf) - 1 ? event->data_len : (int)sizeof(buf) - 1;
            if (n > 0) {
                memcpy(buf, event->data, n);
            }
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
                buf[--n] = '\0';
            }
            if (strcmp(buf, "all") == 0 || strcmp(buf, "ALL") == 0 || strcmp(buf, "switches") == 0 ||
                strcmp(buf, "SWITCHES") == 0) {
                s_remove_cb(0, true);
            } else {
                uint64_t ieee = 0;
                if (!parse_ieee_payload(event->data, event->data_len, &ieee)) {
                    ESP_LOGW(TAG, "Bad remove payload: %.*s (use IEEE hex or 'switches')", event->data_len,
                             event->data);
                } else {
                    s_remove_cb(ieee, false);
                }
            }
        } else if (event->topic_len == (int)strlen(ZBGW_TOPIC_REDISCOVER) &&
                   strncmp(event->topic, ZBGW_TOPIC_REDISCOVER, event->topic_len) == 0) {
            if (s_rediscover_cb) {
                s_rediscover_cb();
            }
        } else {
            handle_switch_set_topic(event->topic, event->topic_len, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

void mqtt_bridge_suspend(void)
{
    if (!s_client) {
        return;
    }
    ESP_LOGW(TAG, "Suspending MQTT during Zigbee pairing");
    s_connected = false;
    (void)esp_mqtt_client_stop(s_client);
}

void mqtt_bridge_resume(void)
{
    if (!s_client) {
        return;
    }
    ESP_LOGI(TAG, "Resuming MQTT after Zigbee pairing");
    (void)esp_mqtt_client_start(s_client);
}

esp_err_t mqtt_bridge_start(mqtt_bridge_permit_join_cb_t permit_cb, mqtt_bridge_switch_cb_t switch_cb,
                            mqtt_bridge_remove_cb_t remove_cb, mqtt_bridge_rediscover_cb_t rediscover_cb)
{
    s_permit_cb = permit_cb;
    s_switch_cb = switch_cb;
    s_remove_cb = remove_cb;
    s_rediscover_cb = rediscover_cb;

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", CONFIG_ZBGW_MQTT_HOST, CONFIG_ZBGW_MQTT_PORT);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = CONFIG_ZBGW_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_ZBGW_MQTT_PASSWORD,
        .session.last_will =
            {
                .topic = ZBGW_TOPIC_STATUS,
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = true,
            },
        .session.keepalive = 30,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    ESP_LOGI(TAG, "MQTT client started -> %s", uri);
    return ESP_OK;
}

bool mqtt_bridge_is_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_bridge_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !topic || !payload) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_bridge_publish_status(const char *status)
{
    return mqtt_bridge_publish(ZBGW_TOPIC_STATUS, status, 1, true);
}

esp_err_t mqtt_bridge_publish_permit_state(bool open)
{
    return mqtt_bridge_publish(ZBGW_TOPIC_PERMIT_STATE, open ? "ON" : "OFF", 1, true);
}

esp_err_t mqtt_bridge_publish_info(const char *json)
{
    return mqtt_bridge_publish(ZBGW_TOPIC_INFO, json, 1, true);
}
