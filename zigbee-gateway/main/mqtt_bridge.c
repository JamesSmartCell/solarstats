#include "mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "device_registry.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_discovery.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_bridge";

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static mqtt_bridge_permit_join_cb_t s_permit_cb;
static mqtt_bridge_switch_cb_t s_switch_cb;
static mqtt_bridge_remove_cb_t s_remove_cb;
static mqtt_bridge_rediscover_cb_t s_rediscover_cb;
static TaskHandle_t s_discovery_task;
static uint32_t s_discovery_gen;
static bool s_discovery_pending;
/* Resolved once at start so reconnects do not re-hit flaky .local DNS. */
static char s_broker_host[64];

typedef struct {
    uint32_t gen;
} discovery_pace_ctx_t;

static bool host_looks_like_ipv4(const char *s)
{
    int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    return s && sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 && a >= 0 && a <= 255 && b >= 0 &&
           b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static esp_err_t resolve_broker_host(void)
{
    const char *cfg = CONFIG_ZBGW_MQTT_HOST;
    snprintf(s_broker_host, sizeof(s_broker_host), "%s", cfg);
    if (host_looks_like_ipv4(cfg)) {
        ESP_LOGI(TAG, "MQTT broker IP %s", s_broker_host);
        return ESP_OK;
    }

    size_t len = strlen(cfg);
    if (len > 6 && strcmp(cfg + len - 6, ".local") == 0) {
        char name[64];
        size_t nlen = len - 6;
        if (nlen >= sizeof(name)) {
            nlen = sizeof(name) - 1;
        }
        memcpy(name, cfg, nlen);
        name[nlen] = '\0';

        esp_err_t mdns_err = mdns_init();
        if (mdns_err != ESP_OK && mdns_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(mdns_err));
        } else {
            for (int i = 0; i < 12; ++i) {
                esp_ip4_addr_t addr = {0};
                if (mdns_query_a(name, 2500, &addr) == ESP_OK) {
                    snprintf(s_broker_host, sizeof(s_broker_host), IPSTR, IP2STR(&addr));
                    ESP_LOGI(TAG, "mDNS %s -> %s", cfg, s_broker_host);
                    return ESP_OK;
                }
                ESP_LOGW(TAG, "mDNS %s failed, retry %d/12", name, i + 1);
                vTaskDelay(pdMS_TO_TICKS(750));
            }
        }
    }

    for (int i = 0; i < 8; ++i) {
        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int gerr = getaddrinfo(cfg, NULL, &hints, &res);
        if (gerr == 0 && res && res->ai_addr) {
            struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
            inet_ntoa_r(sa->sin_addr, s_broker_host, sizeof(s_broker_host));
            freeaddrinfo(res);
            ESP_LOGI(TAG, "DNS %s -> %s", cfg, s_broker_host);
            return ESP_OK;
        }
        if (res) {
            freeaddrinfo(res);
        }
        ESP_LOGW(TAG, "DNS %s failed (%d), retry %d/8", cfg, gerr, i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "Keeping unresolved hostname %s (MQTT may fail until DNS works)", cfg);
    return ESP_ERR_NOT_FOUND;
}

static void rediscover_device_paced(zbgw_device_t *dev, void *ctx)
{
    discovery_pace_ctx_t *pace = ctx;
    if (!dev || !pace || !s_connected || pace->gen != s_discovery_gen) {
        return;
    }
    (void)ha_discovery_publish_device(dev);
    dev->discovery_published = true;
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void discovery_task(void *arg)
{
    (void)arg;
    do {
        s_discovery_pending = false;
        uint32_t gen = s_discovery_gen;
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!s_connected || gen != s_discovery_gen) {
            continue;
        }

        ESP_LOGI(TAG, "Publishing HA discovery (paced)");
        (void)ha_discovery_publish_bridge();
        vTaskDelay(pdMS_TO_TICKS(100));

        discovery_pace_ctx_t pace = {.gen = gen};
        device_registry_foreach(rediscover_device_paced, &pace);

        if (s_connected && gen == s_discovery_gen) {
            ESP_LOGI(TAG, "HA discovery publish complete");
        }
    } while (s_discovery_pending && s_connected);

    s_discovery_task = NULL;
    vTaskDelete(NULL);
}

static void schedule_discovery(void)
{
    s_discovery_gen++;
    s_discovery_pending = true;
    if (s_discovery_task) {
        return;
    }
    if (xTaskCreate(discovery_task, "mqtt_disc", 4096, NULL, 5, &s_discovery_task) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start discovery task");
        s_discovery_task = NULL;
        s_discovery_pending = false;
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
        ESP_LOGI(TAG, "MQTT connected");
        s_connected = true;
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_PERMIT_JOIN, 1);
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_SWITCH_SET_WILDCARD, 1);
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_REMOVE, 1);
        esp_mqtt_client_subscribe(s_client, ZBGW_TOPIC_REDISCOVER, 1);
        mqtt_bridge_publish_status("online");
        mqtt_bridge_publish_permit_state(false);
        schedule_discovery();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        s_discovery_gen++;
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
    s_discovery_gen++;
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

    /* Let DHCP/DNS settle; Zigbee RF also contends for the radio right after boot. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    (void)resolve_broker_host();

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_broker_host, CONFIG_ZBGW_MQTT_PORT);

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
        .session.keepalive = 60,
        .network.timeout_ms = 20000,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
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
    if (!s_connected) {
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
    return mqtt_bridge_publish(ZBGW_TOPIC_INFO, json, 0, true);
}
