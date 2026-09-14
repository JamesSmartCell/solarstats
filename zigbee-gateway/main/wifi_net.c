#include "wifi_net.h"
#include "mqtt_bridge.h"
#include "nvs_creds.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
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

/* esp_wifi_set_max_tx_power unit is 0.25 dBm (8=2 dBm … 84=20 dBm).
 * 4 dBm + Zigbee coexistence aborts TCP (ECONNABORTED). Floor at ~10 dBm.
 * Do not stay at 20 dBm after associate: GOT_IP / ARP on 11ax + Zigbee
 * browns out the FireBeetle USB rail (office RSSI -53 still TX at 20 dBm). */
#define WIFI_TX_LOW_QDBM  40 /* ~10 dBm */
#define WIFI_TX_MID_QDBM  56 /* ~14 dBm */
#define WIFI_TX_HIGH_QDBM 84 /* ~20 dBm */
#define WIFI_RSSI_LOW_TX  (-55)
#define WIFI_RSSI_MID_TX  (-70)

static int8_t qdbm_for_rssi(int8_t rssi)
{
    if (rssi >= WIFI_RSSI_LOW_TX) {
        return WIFI_TX_LOW_QDBM;
    }
    if (rssi >= WIFI_RSSI_MID_TX) {
        return WIFI_TX_MID_QDBM;
    }
    return WIFI_TX_HIGH_QDBM;
}

static void apply_tx_power_from_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        /* Unknown RSSI: cap at mid so DHCP/ARP cannot sit at 20 dBm. */
        ESP_LOGW(TAG, "No AP info yet - TX mid ~14 dBm");
        (void)esp_wifi_set_max_tx_power(WIFI_TX_MID_QDBM);
        return;
    }

    int8_t qdbm = qdbm_for_rssi(ap.rssi);
    const char *mode = (qdbm == WIFI_TX_LOW_QDBM) ? "low" : (qdbm == WIFI_TX_MID_QDBM) ? "mid" : "high";
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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        /* Drop TX before DHCP/ARP. Waiting until MQTT is up browns out at GOT_IP. */
        apply_tx_power_from_rssi();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_paused) {
            /* Intentional stop for Zigbee pairing — do not reconnect. */
            return;
        }
        s_retry_count++;
        if (s_retry_count > 7) {
            ESP_LOGE(TAG, "WiFi failed %d times - opening setup portal", s_retry_count);
            (void)nvs_creds_request_setup();
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
        apply_tx_power_from_rssi();
        xEventGroupSetBits(s_wifi_events, WIFI_NET_CONNECTED_BIT);
        mqtt_bridge_resume();
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

    zbgw_creds_t creds;
    ESP_ERROR_CHECK(nvs_creds_get(&creds));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, creds.wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, creds.wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = creds.wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    /* 11ax HE peaks higher than 11n; MQTT does not need it on this USB-powered C6. */
    (void)esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    /* No modem sleep until MQTT is up — MAX_MODEM + Zigbee aborts TCP connect. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Associate at high power; STA_CONNECTED drops TX from RSSI before DHCP. */
    (void)esp_wifi_set_max_tx_power(WIFI_TX_HIGH_QDBM);
    s_started = true;

    ESP_LOGI(TAG, "Connecting to SSID:%s", creds.wifi_ssid);
    return ESP_OK;
}

bool wifi_net_is_connected(void)
{
    return s_connected && !s_paused;
}

bool wifi_net_get_sta_ipv4(uint32_t *ip_addr, uint32_t *netmask)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    if (ip_addr) {
        *ip_addr = info.ip.addr;
    }
    if (netmask) {
        *netmask = info.netmask.addr;
    }
    return true;
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

void wifi_net_on_mqtt_up(void)
{
    if (!s_started || s_paused) {
        return;
    }
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    apply_tx_power_from_rssi();
}

void wifi_net_on_mqtt_down(void)
{
    if (!s_started || s_paused) {
        return;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    apply_tx_power_from_rssi();
    ESP_LOGI(TAG, "MQTT down - WiFi PS off, TX from RSSI");
}
