#include "hosted_wifi.h"

#if !CONFIG_HALITE_ESP_HOSTED
esp_err_t hosted_wifi_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#else

#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "hosted_wifi";

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *event = data;
    ESP_LOGI(TAG, "P4 LAN IP " IPSTR " — poll http://" IPSTR "/api/health", IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.ip));
}

esp_err_t hosted_wifi_start(void)
{
    /* Pulses CONFIG reset (GPIO54 on this board). C6 must already be hosted+Zigbee. */
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_hosted_connect_to_slave();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_HALITE_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_HALITE_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Hosted STA connecting to '%s'", CONFIG_HALITE_WIFI_SSID);
    return ESP_OK;
}

#endif
