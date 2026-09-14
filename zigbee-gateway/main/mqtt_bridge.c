#include "mqtt_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "device_registry.h"
#include "diag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_id.h"
#include "ha_discovery.h"
#include "nvs_creds.h"
#include "ota_update.h"
#include "wifi_net.h"
#include "zigbee_coordinator.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

/* Watchdog period must be longer than one TCP connect attempt. */
#define MQTT_WD_FAIL_LIMIT 4
#define MQTT_WD_PERIOD_MS  20000
#define MQTT_CONNECT_TIMEOUT_MS 15000
#define MQTT_RECONNECT_MS 4000
#define MQTT_LAN_PROBE_MS 180
/* Full HA discovery on every flap starves Wi‑Fi on the shared C6 radio. */
#define MQTT_DISCOVERY_COOLDOWN_MS (15 * 60 * 1000)

static const char *TAG = "mqtt_bridge";

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_suspended;
static int s_fail_count;
static mqtt_bridge_permit_join_cb_t s_permit_cb;
static mqtt_bridge_switch_cb_t s_switch_cb;
static mqtt_bridge_remove_cb_t s_remove_cb;
static mqtt_bridge_rediscover_cb_t s_rediscover_cb;
static TaskHandle_t s_discovery_task;
static uint32_t s_discovery_gen;
static bool s_discovery_pending;
static bool s_discovery_done;
static int64_t s_last_discovery_ms;
/* Resolved once at start so reconnects do not re-hit flaky .local DNS. */
static char s_broker_host[64];
static char s_mqtt_cfg_host[64];
static char s_mqtt_user[NVS_CREDS_USER_MAX + 1];
static char s_mqtt_pass[NVS_CREDS_PASS_MAX + 1];
static char s_mqtt_uri[128];

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
    const char *cfg = s_mqtt_cfg_host;
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

static bool host_is_auto(const char *s)
{
    return !s || !s[0] || strcmp(s, "auto") == 0 || strcmp(s, "AUTO") == 0;
}

static void apply_broker_host(const char *host)
{
    snprintf(s_broker_host, sizeof(s_broker_host), "%s", host);
    snprintf(s_mqtt_cfg_host, sizeof(s_mqtt_cfg_host), "%s", host);
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%d", host, CONFIG_ZBGW_MQTT_PORT);
}

static void persist_broker_host(const char *host)
{
    zbgw_creds_t creds;
    if (nvs_creds_get(&creds) != ESP_OK) {
        return;
    }
    if (strcmp(creds.mqtt_host, host) == 0) {
        return;
    }
    snprintf(creds.mqtt_host, sizeof(creds.mqtt_host), "%s", host);
    if (nvs_creds_set(&creds) == ESP_OK) {
        ESP_LOGI(TAG, "Saved discovered MQTT host %s", host);
    }
}

static bool mqtt_tcp_open(const char *ip, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)CONFIG_ZBGW_MQTT_PORT);
    if (!inet_aton(ip, &dest.sin_addr)) {
        close(sock);
        return false;
    }

    int rc = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
    if (rc == 0) {
        close(sock);
        return true;
    }
    if (errno != EINPROGRESS) {
        close(sock);
        return false;
    }

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    rc = select(sock + 1, NULL, &wset, NULL, &tv);
    bool open = false;
    if (rc > 0) {
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 && so_err == 0) {
            open = true;
        }
    }
    close(sock);
    return open;
}

static bool scan_subnet_mqtt(char *out, size_t out_sz)
{
    uint32_t ip_nbo = 0;
    uint32_t mask_nbo = 0;
    if (!wifi_net_get_sta_ipv4(&ip_nbo, &mask_nbo)) {
        ESP_LOGW(TAG, "No STA IP yet - cannot scan for MQTT");
        return false;
    }

    uint32_t host = ntohl(ip_nbo);
    uint32_t prefix = host & 0xFFFFFF00u;
    uint32_t self = host & 0xFFu;
    ESP_LOGI(TAG, "Scanning %u.%u.%u.1-254 for MQTT :%d", (unsigned)((prefix >> 24) & 0xff),
             (unsigned)((prefix >> 16) & 0xff), (unsigned)((prefix >> 8) & 0xff), CONFIG_ZBGW_MQTT_PORT);

    for (uint32_t last = 1; last <= 254; last++) {
        if (last == self) {
            continue;
        }
        uint32_t cand = htonl(prefix | last);
        char ip[16];
        struct in_addr addr = {.s_addr = cand};
        inet_ntoa_r(addr, ip, sizeof(ip));
        if (mqtt_tcp_open(ip, MQTT_LAN_PROBE_MS)) {
            snprintf(out, out_sz, "%s", ip);
            ESP_LOGI(TAG, "MQTT listener at %s:%d", ip, CONFIG_ZBGW_MQTT_PORT);
            return true;
        }
        if ((last % 32) == 0) {
            ESP_LOGI(TAG, "MQTT scan ... .%u", (unsigned)last);
        }
    }
    ESP_LOGW(TAG, "No MQTT listener on this /24 :%d", CONFIG_ZBGW_MQTT_PORT);
    return false;
}

static bool retarget_mqtt_client(const char *host)
{
    apply_broker_host(host);
    persist_broker_host(host);
    if (!s_client) {
        return true;
    }
    esp_err_t err = esp_mqtt_client_set_uri(s_client, s_mqtt_uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT set_uri %s failed: %s", s_mqtt_uri, esp_err_to_name(err));
        return false;
    }
    (void)esp_mqtt_client_reconnect(s_client);
    ESP_LOGI(TAG, "MQTT client retargeted -> %s", s_mqtt_uri);
    return true;
}

static esp_err_t ensure_broker_host(void)
{
    if (!host_is_auto(s_mqtt_cfg_host)) {
        (void)resolve_broker_host();
        if (host_looks_like_ipv4(s_broker_host) && mqtt_tcp_open(s_broker_host, MQTT_LAN_PROBE_MS * 3)) {
            ESP_LOGI(TAG, "MQTT broker reachable at %s", s_broker_host);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Configured MQTT %s not open on :%d - scanning LAN", s_broker_host,
                 CONFIG_ZBGW_MQTT_PORT);
    } else {
        ESP_LOGI(TAG, "MQTT host not set - scanning LAN for :%d", CONFIG_ZBGW_MQTT_PORT);
    }

    char found[16];
    if (scan_subnet_mqtt(found, sizeof(found))) {
        apply_broker_host(found);
        persist_broker_host(found);
        return ESP_OK;
    }
    if (host_is_auto(s_mqtt_cfg_host) || !s_broker_host[0]) {
        snprintf(s_broker_host, sizeof(s_broker_host), "%s", "0.0.0.0");
    }
    return ESP_ERR_NOT_FOUND;
}

static void rediscover_device_paced(zbgw_device_t *dev, void *ctx)
{
    discovery_pace_ctx_t *pace = ctx;
    if (!dev || !pace || !s_connected || pace->gen != s_discovery_gen) {
        return;
    }
    ESP_LOGI(TAG, "Discovery ieee=%016llx caps=0x%lx", (unsigned long long)dev->ieee,
             (unsigned long)dev->capabilities);
    (void)ha_discovery_publish_device(dev);
    dev->discovery_published = true;
    vTaskDelay(pdMS_TO_TICKS(350));
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
            s_discovery_done = true;
            s_last_discovery_ms = esp_timer_get_time() / 1000;
            zigbee_coordinator_on_discovery_complete();
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

    const char *prefix = zbgw_topic_prefix();
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

static void handle_power_on_set_topic(const char *topic, int topic_len, const char *data, int data_len)
{
    if (!topic || topic_len <= 0 || !data) {
        return;
    }

    const char *prefix = zbgw_topic_prefix();
    size_t prefix_len = strlen(prefix);
    const char *suffix = "/power_on_behavior/set";
    size_t suffix_len = strlen(suffix);
    if ((size_t)topic_len < prefix_len + 1 + 16 + suffix_len) {
        return;
    }
    if (strncmp(topic, prefix, prefix_len) != 0 || topic[prefix_len] != '/') {
        return;
    }

    const char *ieee_start = topic + prefix_len + 1;
    if (strncmp(ieee_start + 16, suffix, suffix_len) != 0) {
        return;
    }

    char ieee_str[17] = {0};
    memcpy(ieee_str, ieee_start, 16);
    uint64_t ieee = 0;
    if (!device_registry_ieee_from_str(ieee_str, &ieee)) {
        ESP_LOGW(TAG, "Bad IEEE in power_on_behavior topic: %s", ieee_str);
        return;
    }

    char name[24] = {0};
    int n = data_len < (int)sizeof(name) - 1 ? data_len : (int)sizeof(name) - 1;
    if (n > 0) {
        memcpy(name, data, n);
    }
    while (n > 0 && (name[n - 1] == '\n' || name[n - 1] == '\r' || name[n - 1] == ' ' || name[n - 1] == '"')) {
        name[--n] = '\0';
    }
    if (name[0] == '"') {
        memmove(name, name + 1, n);
        n--;
        if (n > 0 && name[n - 1] == '"') {
            name[--n] = '\0';
        }
    }
    if (!name[0]) {
        return;
    }
    ESP_LOGI(TAG, "MQTT power_on_behavior ieee=%s -> %s", ieee_str, name);
    (void)zigbee_coordinator_set_power_on_behavior(ieee, name);
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
        s_fail_count = 0;
        wifi_net_on_mqtt_up();
        esp_mqtt_client_subscribe(s_client, zbgw_topic_permit_join(), 1);
        esp_mqtt_client_subscribe(s_client, zbgw_topic_switch_set_wildcard(), 1);
        esp_mqtt_client_subscribe(s_client, zbgw_topic_power_on_set_wildcard(), 1);
        esp_mqtt_client_subscribe(s_client, zbgw_topic_remove(), 1);
        esp_mqtt_client_subscribe(s_client, zbgw_topic_rediscover(), 1);
        esp_mqtt_client_subscribe(s_client, zbgw_topic_ota(), 1);
        mqtt_bridge_publish_status("online");
        mqtt_bridge_publish_permit_state(false);
        zigbee_coordinator_on_mqtt_connected();
        {
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool cooldown_ok =
                s_discovery_done && (now_ms - s_last_discovery_ms) < MQTT_DISCOVERY_COOLDOWN_MS;
            if (cooldown_ok) {
                ESP_LOGI(TAG, "MQTT reconnected - skipping discovery (cooldown)");
                zigbee_coordinator_on_discovery_complete();
            } else {
                schedule_discovery();
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        s_discovery_gen++;
        wifi_net_on_mqtt_down();
        zigbee_coordinator_on_mqtt_disconnected();
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len == (int)strlen(zbgw_topic_permit_join()) &&
            strncmp(event->topic, zbgw_topic_permit_join(), event->topic_len) == 0) {
            handle_permit_join_payload(event->data, event->data_len);
        } else if (event->topic_len == (int)strlen(zbgw_topic_remove()) &&
                   strncmp(event->topic, zbgw_topic_remove(), event->topic_len) == 0) {
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
        } else if (event->topic_len == (int)strlen(zbgw_topic_rediscover()) &&
                   strncmp(event->topic, zbgw_topic_rediscover(), event->topic_len) == 0) {
            if (s_rediscover_cb) {
                s_rediscover_cb();
            }
        } else if (event->topic_len == (int)strlen(zbgw_topic_ota()) &&
                   strncmp(event->topic, zbgw_topic_ota(), event->topic_len) == 0) {
            ESP_LOGI(TAG, "MQTT OTA trigger");
            (void)ota_update_start();
        } else {
            handle_switch_set_topic(event->topic, event->topic_len, event->data, event->data_len);
            handle_power_on_set_topic(event->topic, event->topic_len, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        diag_report_error("mqtt_error", "MQTT client error");
        break;
    default:
        break;
    }
}

static void mqtt_watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MQTT_WD_PERIOD_MS));
        if (s_suspended || wifi_net_is_paused()) {
            s_fail_count = 0;
            continue;
        }
        if (s_connected) {
            s_fail_count = 0;
            continue;
        }
        if (!s_client || !wifi_net_is_connected()) {
            continue;
        }
        s_fail_count++;
        ESP_LOGW(TAG, "MQTT watchdog: still down, reconnect (%d)", s_fail_count);
        if (s_fail_count % MQTT_WD_FAIL_LIMIT == 0) {
            ESP_LOGE(TAG, "MQTT unreachable at %s — scanning LAN", s_broker_host);
            diag_report_error("mqtt_watchdog", "MQTT broker unreachable — scanning LAN");
            char found[16];
            if (scan_subnet_mqtt(found, sizeof(found)) && strcmp(found, s_broker_host) != 0) {
                (void)retarget_mqtt_client(found);
                continue;
            }
        }
        /* Client often stops retrying after ECONNABORTED — kick it. */
        (void)esp_mqtt_client_reconnect(s_client);
    }
}

void mqtt_bridge_suspend(void)
{
    if (!s_client) {
        return;
    }
    ESP_LOGW(TAG, "Suspending MQTT during Zigbee pairing");
    s_suspended = true;
    s_connected = false;
    s_fail_count = 0;
    s_discovery_gen++;
    (void)esp_mqtt_client_stop(s_client);
}

void mqtt_bridge_resume(void)
{
    if (!s_client) {
        return;
    }
    s_fail_count = 0;
    s_suspended = false;
    if (!wifi_net_is_connected()) {
        ESP_LOGI(TAG, "MQTT resume armed - waiting for WiFi IP");
        return;
    }
    ESP_LOGI(TAG, "Resuming MQTT after Zigbee pairing");
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT start %s - reconnect", esp_err_to_name(err));
        (void)esp_mqtt_client_reconnect(s_client);
    }
}

esp_err_t mqtt_bridge_start(mqtt_bridge_permit_join_cb_t permit_cb, mqtt_bridge_switch_cb_t switch_cb,
                            mqtt_bridge_remove_cb_t remove_cb, mqtt_bridge_rediscover_cb_t rediscover_cb)
{
    zbgw_id_init();
    s_permit_cb = permit_cb;
    s_switch_cb = switch_cb;
    s_remove_cb = remove_cb;
    s_rediscover_cb = rediscover_cb;

    zbgw_creds_t creds;
    ESP_ERROR_CHECK(nvs_creds_get(&creds));
    snprintf(s_mqtt_cfg_host, sizeof(s_mqtt_cfg_host), "%s", creds.mqtt_host);
    snprintf(s_mqtt_user, sizeof(s_mqtt_user), "%s", creds.mqtt_user);
    snprintf(s_mqtt_pass, sizeof(s_mqtt_pass), "%s", creds.mqtt_pass);

    /* Wait for Wi-Fi BA / DHCP to settle before the first TCP connect. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    (void)ensure_broker_host();
    if (!s_mqtt_uri[0]) {
        snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%d", s_broker_host, CONFIG_ZBGW_MQTT_PORT);
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.username = s_mqtt_user[0] ? s_mqtt_user : NULL,
        .credentials.authentication.password = s_mqtt_pass[0] ? s_mqtt_pass : NULL,
        .session.last_will =
            {
                .topic = zbgw_topic_status(),
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = true,
            },
        .session.keepalive = 60,
        .network.timeout_ms = MQTT_CONNECT_TIMEOUT_MS,
        .network.reconnect_timeout_ms = MQTT_RECONNECT_MS,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    if (xTaskCreate(mqtt_watchdog_task, "mqtt_wd", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "MQTT watchdog task failed to start");
    }
    ESP_LOGI(TAG, "MQTT client started -> %s (watchdog %d fails)", s_mqtt_uri, MQTT_WD_FAIL_LIMIT);
    return ESP_OK;
}

bool mqtt_bridge_is_connected(void)
{
    return s_connected;
}

bool mqtt_bridge_discovery_busy(void)
{
    return s_discovery_task != NULL;
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
    return mqtt_bridge_publish(zbgw_topic_status(), status, 1, true);
}

esp_err_t mqtt_bridge_publish_permit_state(bool open)
{
    return mqtt_bridge_publish(zbgw_topic_permit_state(), open ? "ON" : "OFF", 1, true);
}

esp_err_t mqtt_bridge_publish_info(const char *json)
{
    return mqtt_bridge_publish(zbgw_topic_info(), json, 0, true);
}
