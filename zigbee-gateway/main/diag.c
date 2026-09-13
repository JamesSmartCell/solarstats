#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_registry.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_bridge.h"
#include "nvs_creds.h"
#include "sdkconfig.h"
#include "wifi_net.h"
#include "zigbee_coordinator.h"

static const char *TAG = "diag";

#define DIAG_POLL_MS       (2 * 60 * 1000)
#define DIAG_OK_MS         (60 * 60 * 1000)
#define DIAG_ERROR_MIN_MS  30000
#define DIAG_QUEUE_LEN     8
#define DIAG_RESP_MAX      512
#define DIAG_CODE_MAX      32
#define DIAG_MSG_MAX       128

typedef struct {
    char kind[8];
    char code[DIAG_CODE_MAX];
    char message[DIAG_MSG_MAX];
} diag_event_t;

static QueueHandle_t s_queue;
static bool s_enabled;
static int64_t s_last_error_ms;
static int64_t s_last_ok_ms;
static bool s_had_error;

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_DEEPSLEEP:
        return "sleep";
    default:
        return "other";
    }
}

static void device_id(char *out, size_t out_sz)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_sz, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

static void count_device(zbgw_device_t *dev, void *ctx)
{
    if (dev && dev->in_use) {
        (*(int *)ctx)++;
    }
}

static int wifi_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (!wifi_net_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return (int)ap.rssi;
}

static bool apply_remote_commands(const char *resp)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        return false;
    }
    cJSON *cmds = cJSON_GetObjectItem(root, "commands");
    bool restart = false;
    if (cJSON_IsArray(cmds)) {
        const cJSON *cmd = NULL;
        cJSON_ArrayForEach(cmd, cmds)
        {
            const cJSON *action = cJSON_GetObjectItem(cmd, "action");
            if (cJSON_IsString(action) && action->valuestring &&
                strcmp(action->valuestring, "restart") == 0) {
                restart = true;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return restart;
}

static bool post_event(const diag_event_t *ev)
{
    const char *url = CONFIG_ZBGW_DIAG_URL;
    if (!url || !url[0]) {
        return false;
    }
    if (!wifi_net_is_connected()) {
        ESP_LOGW(TAG, "Skip %s — Wi-Fi down", ev->kind);
        return false;
    }

    char id[16];
    device_id(id, sizeof(id));
    const esp_app_desc_t *app = esp_app_get_description();
    int devices = 0;
    device_registry_foreach(count_device, &devices);

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return false;
    }
    cJSON_AddStringToObject(body, "device_id", id);
    cJSON_AddStringToObject(body, "fw", app && app->version[0] ? app->version : "?");
    cJSON_AddStringToObject(body, "kind", ev->kind);
    cJSON_AddBoolToObject(body, "ok", strcmp(ev->kind, "error") != 0);
    cJSON_AddNumberToObject(body, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(body, "reset", reset_reason_str());
    cJSON_AddNumberToObject(body, "heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(body, "wifi_rssi", (double)wifi_rssi());
    cJSON_AddBoolToObject(body, "mqtt_ok", mqtt_bridge_is_connected());
    cJSON_AddBoolToObject(body, "zigbee_ok", zigbee_coordinator_network_ready());
    cJSON_AddNumberToObject(body, "devices", (double)devices);
    if (ev->code[0]) {
        cJSON_AddStringToObject(body, "code", ev->code);
    }
    if (ev->message[0]) {
        cJSON_AddStringToObject(body, "message", ev->message);
    } else if (strcmp(ev->kind, "ok") == 0) {
        cJSON_AddStringToObject(body, "message", "Device working correctly");
    }

    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(payload);
        return false;
    }
    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    if (CONFIG_ZBGW_DIAG_TOKEN[0]) {
        (void)esp_http_client_set_header(client, "X-ZBGW-Diag", CONFIG_ZBGW_DIAG_TOKEN);
    }

    bool restart = false;
    bool ok = false;
    int len = (int)strlen(payload);
    esp_err_t err = esp_http_client_open(client, len);
    if (err == ESP_OK) {
        int wrote = esp_http_client_write(client, payload, len);
        if (wrote == len) {
            (void)esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            char resp[DIAG_RESP_MAX];
            int n = esp_http_client_read_response(client, resp, sizeof(resp) - 1);
            if (n < 0) {
                n = 0;
            }
            resp[n] = '\0';
            ok = status >= 200 && status < 300;
            if (ok) {
                restart = apply_remote_commands(resp);
            } else {
                ESP_LOGW(TAG, "POST %s HTTP %d", ev->kind, status);
            }
        }
        esp_http_client_close(client);
    } else {
        ESP_LOGW(TAG, "POST %s open failed: %s", ev->kind, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    free(payload);

    if (ok) {
        ESP_LOGI(TAG, "Sent %s%s%s", ev->kind, ev->code[0] ? " " : "", ev->code);
    }
    if (restart) {
        ESP_LOGW(TAG, "Remote restart");
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    }
    return ok;
}

static void fill_event(diag_event_t *ev, const char *kind, const char *code, const char *message)
{
    memset(ev, 0, sizeof(*ev));
    snprintf(ev->kind, sizeof(ev->kind), "%s", kind ? kind : "ok");
    if (code) {
        snprintf(ev->code, sizeof(ev->code), "%s", code);
    }
    if (message) {
        snprintf(ev->message, sizeof(ev->message), "%s", message);
    }
}

static void ensure_time(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "SNTP not synced yet — HTTPS may fail");
}

static void diag_task(void *arg)
{
    (void)arg;
    if (!wifi_net_is_connected()) {
        (void)wifi_net_wait_connected(pdMS_TO_TICKS(90000));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    ensure_time();

    diag_event_t ev;
    fill_event(&ev, "boot", "boot", "Gateway started");
    (void)post_event(&ev);
    s_last_ok_ms = esp_timer_get_time() / 1000;

    while (true) {
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(DIAG_POLL_MS))) {
            (void)post_event(&ev);
            if (strcmp(ev.kind, "error") == 0) {
                s_had_error = true;
                s_last_ok_ms = esp_timer_get_time() / 1000;
            }
            continue;
        }

        int64_t now = esp_timer_get_time() / 1000;
        if (!s_had_error && (now - s_last_ok_ms) >= DIAG_OK_MS) {
            fill_event(&ev, "ok", NULL, "Device working correctly");
            if (post_event(&ev)) {
                s_last_ok_ms = now;
            }
        } else {
            fill_event(&ev, "poll", NULL, NULL);
            (void)post_event(&ev);
        }
    }
}

static bool enqueue_error(const char *code, const char *detail)
{
    if (!s_enabled || !s_queue) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_error_ms && (now - s_last_error_ms) < DIAG_ERROR_MIN_MS) {
        return false;
    }
    s_last_error_ms = now;
    diag_event_t ev;
    fill_event(&ev, "error", code, detail);
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Error queue full (%s)", code ? code : "?");
        return false;
    }
    return true;
}

void diag_report_error(const char *code, const char *detail)
{
    (void)enqueue_error(code, detail);
}

void diag_report_error_blocking(const char *code, const char *detail)
{
    if (!s_enabled) {
        return;
    }
    diag_event_t ev;
    fill_event(&ev, "error", code, detail);
    (void)post_event(&ev);
}

void diag_start(void)
{
    s_enabled = nvs_creds_diag_opt_in();
    if (!s_enabled) {
        ESP_LOGI(TAG, "Anonymous diagnostics off");
        return;
    }
    if (!CONFIG_ZBGW_DIAG_URL[0]) {
        ESP_LOGW(TAG, "Diagnostics URL empty — disabled");
        s_enabled = false;
        return;
    }
    s_queue = xQueueCreate(DIAG_QUEUE_LEN, sizeof(diag_event_t));
    if (!s_queue) {
        s_enabled = false;
        return;
    }
    if (xTaskCreate(diag_task, "diag", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "diag task failed");
        s_enabled = false;
        return;
    }
    ESP_LOGI(TAG, "Anonymous diagnostics -> %s", CONFIG_ZBGW_DIAG_URL);
}
