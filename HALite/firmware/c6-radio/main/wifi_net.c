#include "wifi_net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_net";

static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static bool s_connected;
static bool s_paused;
static bool s_started;

/* esp_wifi_set_max_tx_power unit is 0.25 dBm (8=2 dBm … 84=20 dBm). */
#define WIFI_TX_LOW_QDBM  16 /* ~4 dBm — next to the AP */
#define WIFI_TX_HIGH_QDBM 84 /* ~20 dBm — weaker link */
#define WIFI_RSSI_LOW_TX  (-55)

static void apply_tx_power_from_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGW(TAG, "No AP info yet - leaving TX at high power");
        return;
    }

    int8_t qdbm = (ap.rssi >= WIFI_RSSI_LOW_TX) ? WIFI_TX_LOW_QDBM : WIFI_TX_HIGH_QDBM;
    const char *mode = (qdbm == WIFI_TX_LOW_QDBM) ? "low" : "high";
    float dbm = (float)qdbm * 0.25f;
    esp_err_t err = esp_wifi_set_max_tx_power(qdbm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_max_tx_power failed: %s (RSSI %d)", esp_err_to_name(err), (int)ap.rssi);
        return;
    }
    ESP_LOGI(TAG, "WiFi RSSI %d dBm -> %s TX ~%.0f dBm", (int)ap.rssi, mode, dbm);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_paused) {
            /* Intentional stop for Zigbee pairing — do not reconnect. */
            return;
        }
        s_retry_count++;
        if (s_retry_count > 7) {
            ESP_LOGE(TAG, "WiFi failed %d times - restarting", s_retry_count);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        ESP_LOGW(TAG, "Retry WiFi connect (%d/7)", s_retry_count);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_events, WIFI_NET_CONNECTED_BIT);
        apply_tx_power_from_rssi();
    }
}

esp_err_t wifi_net_start(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_ZBGW_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_ZBGW_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    /* Prefer modem sleep so IEEE 802.15.4 can share the RF path */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Associate at high power, then drop to low if RSSI is strong (see GOT_IP). */
    (void)esp_wifi_set_max_tx_power(WIFI_TX_HIGH_QDBM);
    s_started = true;

    ESP_LOGI(TAG, "Connecting to SSID:%s", CONFIG_ZBGW_WIFI_SSID);
    return ESP_OK;
}

bool wifi_net_is_connected(void)
{
    return s_connected && !s_paused;
}

esp_err_t wifi_net_wait_connected(TickType_t ticks_to_wait)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_NET_CONNECTED_BIT | WIFI_NET_FAIL_BIT, pdFALSE, pdFALSE,
                                           ticks_to_wait);
    if (bits & WIFI_NET_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t wifi_net_pause_for_zigbee(void)
{
    if (!s_started || s_paused) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Pausing WiFi for Zigbee pairing (RF exclusive)");
    s_paused = true;
    s_connected = false;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_net_resume(void)
{
    if (!s_started || !s_paused) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Resuming WiFi after Zigbee pairing");
    s_paused = false;
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_NET_CONNECTED_BIT | WIFI_NET_FAIL_BIT);
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        s_retry_count++;
        if (s_retry_count > 7) {
            ESP_LOGE(TAG, "WiFi resume failed %d times - restarting", s_retry_count);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
    return err;
}

bool wifi_net_is_paused(void)
{
    return s_paused;
}

int8_t wifi_net_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}
