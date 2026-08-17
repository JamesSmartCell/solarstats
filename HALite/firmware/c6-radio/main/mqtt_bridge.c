#include "mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ipc_host.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "esphome_mqtt";

typedef struct {
    char entity_id[48];
    char node_name[32];
    char state_topic[96];
    char command_topic[96];
} esphome_slot_t;

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_started;
static char s_broker_host[64];
static esphome_slot_t s_slots[4];
static int s_slot_count;

static void load_slots(void)
{
    s_slot_count = 0;
#if defined(CONFIG_HALITE_ESPHOME_1_ENTITY_ID)
    if (CONFIG_HALITE_ESPHOME_1_ENTITY_ID[0]) {
        strncpy(s_slots[s_slot_count].entity_id, CONFIG_HALITE_ESPHOME_1_ENTITY_ID, sizeof(s_slots[0].entity_id) - 1);
        strncpy(s_slots[s_slot_count].node_name, CONFIG_HALITE_ESPHOME_1_NODE, sizeof(s_slots[0].node_name) - 1);
        strncpy(s_slots[s_slot_count].state_topic, CONFIG_HALITE_ESPHOME_1_STATE_TOPIC,
                sizeof(s_slots[0].state_topic) - 1);
        strncpy(s_slots[s_slot_count].command_topic, CONFIG_HALITE_ESPHOME_1_CMD_TOPIC,
                sizeof(s_slots[0].command_topic) - 1);
        s_slot_count++;
    }
#endif
#if defined(CONFIG_HALITE_ESPHOME_2_ENTITY_ID)
    if (s_slot_count < 4 && CONFIG_HALITE_ESPHOME_2_ENTITY_ID[0]) {
        strncpy(s_slots[s_slot_count].entity_id, CONFIG_HALITE_ESPHOME_2_ENTITY_ID, sizeof(s_slots[0].entity_id) - 1);
        strncpy(s_slots[s_slot_count].node_name, CONFIG_HALITE_ESPHOME_2_NODE, sizeof(s_slots[0].node_name) - 1);
        strncpy(s_slots[s_slot_count].state_topic, CONFIG_HALITE_ESPHOME_2_STATE_TOPIC,
                sizeof(s_slots[0].state_topic) - 1);
        strncpy(s_slots[s_slot_count].command_topic, CONFIG_HALITE_ESPHOME_2_CMD_TOPIC,
                sizeof(s_slots[0].command_topic) - 1);
        s_slot_count++;
    }
#endif
}

static const esphome_slot_t *find_by_state_topic(const char *topic)
{
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].state_topic[0] && strcmp(s_slots[i].state_topic, topic) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static const esphome_slot_t *find_by_entity(const char *entity_id)
{
    for (int i = 0; i < s_slot_count; i++) {
        if (strcmp(s_slots[i].entity_id, entity_id) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static bool host_looks_like_ipv4(const char *s)
{
    int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    return s && sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 && a >= 0 && a <= 255 && b >= 0 &&
           b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255;
}

static void resolve_broker_host(void)
{
    const char *cfg = CONFIG_ZBGW_MQTT_HOST;
    snprintf(s_broker_host, sizeof(s_broker_host), "%s", cfg);
    if (!cfg[0] || host_looks_like_ipv4(cfg)) {
        return;
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
        if (mdns_err == ESP_OK || mdns_err == ESP_ERR_INVALID_STATE) {
            esp_ip4_addr_t addr = {0};
            if (mdns_query_a(name, 2500, &addr) == ESP_OK) {
                snprintf(s_broker_host, sizeof(s_broker_host), IPSTR, IP2STR(&addr));
                ESP_LOGI(TAG, "mDNS %s -> %s", cfg, s_broker_host);
                return;
            }
        }
    }
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(cfg, NULL, &hints, &res) == 0 && res && res->ai_addr) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(sa->sin_addr, s_broker_host, sizeof(s_broker_host));
        freeaddrinfo(res);
        ESP_LOGI(TAG, "DNS %s -> %s", cfg, s_broker_host);
        return;
    }
    if (res) {
        freeaddrinfo(res);
    }
}

void mqtt_bridge_announce_entities(void)
{
    for (int i = 0; i < s_slot_count; i++) {
        ipc_esphome_entity_t msg = {0};
        strncpy(msg.entity_id, s_slots[i].entity_id, sizeof(msg.entity_id) - 1);
        strncpy(msg.node_name, s_slots[i].node_name, sizeof(msg.node_name) - 1);
        strncpy(msg.state_topic, s_slots[i].state_topic, sizeof(msg.state_topic) - 1);
        strncpy(msg.command_topic, s_slots[i].command_topic, sizeof(msg.command_topic) - 1);
        msg.domain = 1;
        (void)ipc_host_esphome_entity(&msg);
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        for (int i = 0; i < s_slot_count; i++) {
            if (s_slots[i].state_topic[0]) {
                esp_mqtt_client_subscribe(s_client, s_slots[i].state_topic, 1);
            }
        }
        mqtt_bridge_announce_entities();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        char topic[128];
        char payload[64];
        size_t tlen = e->topic_len < sizeof(topic) - 1 ? (size_t)e->topic_len : sizeof(topic) - 1;
        size_t plen = e->data_len < sizeof(payload) - 1 ? (size_t)e->data_len : sizeof(payload) - 1;
        memcpy(topic, e->topic, tlen);
        topic[tlen] = '\0';
        memcpy(payload, e->data, plen);
        payload[plen] = '\0';
        const esphome_slot_t *slot = find_by_state_topic(topic);
        if (!slot) {
            break;
        }
        bool on = (strcmp(payload, "ON") == 0 || strcmp(payload, "on") == 0 || strcmp(payload, "1") == 0);
        (void)ipc_host_esphome_state(slot->entity_id, 0, on ? 1 : 0);
        break;
    }
    default:
        break;
    }
}

esp_err_t mqtt_bridge_start(void)
{
    load_slots();
    if (!CONFIG_ZBGW_MQTT_HOST[0]) {
        ESP_LOGW(TAG, "MQTT host empty — ESPHome ingest disabled");
        s_started = true;
        return ESP_OK;
    }
    resolve_broker_host();

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", s_broker_host, CONFIG_ZBGW_MQTT_PORT);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = CONFIG_ZBGW_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_ZBGW_MQTT_PASSWORD,
        .session.keepalive = 30,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    s_started = true;
    ESP_LOGI(TAG, "ESPHome MQTT client -> %s (%d entities)", uri, s_slot_count);
    return ESP_OK;
}

void mqtt_bridge_suspend(void)
{
    if (s_client) {
        (void)esp_mqtt_client_stop(s_client);
        s_connected = false;
    }
}

void mqtt_bridge_resume(void)
{
    if (s_client) {
        (void)esp_mqtt_client_start(s_client);
    }
}

bool mqtt_bridge_is_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_bridge_publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected || !topic) {
        return ESP_ERR_INVALID_STATE;
    }
    int id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    return id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_bridge_publish_status(const char *status)
{
    (void)status;
    return ESP_OK;
}

esp_err_t mqtt_bridge_publish_permit_state(bool open)
{
    return ipc_host_permit_join_state(open);
}

esp_err_t mqtt_bridge_publish_info(const char *json)
{
    ESP_LOGI(TAG, "zigbee info %s", json ? json : "");
    return ESP_OK;
}

esp_err_t mqtt_bridge_esphome_set(const char *entity_id, bool on)
{
    const esphome_slot_t *slot = find_by_entity(entity_id);
    if (!slot || !slot->command_topic[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    return mqtt_bridge_publish(slot->command_topic, on ? "ON" : "OFF", 1, false);
}
