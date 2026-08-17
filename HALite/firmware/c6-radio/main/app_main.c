#include "board_io.h"
#include "config.h"
#include "device_registry.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ipc_host.h"
#include "mqtt_bridge.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_net.h"
#include "zigbee_coordinator.h"

static const char *TAG = "halite-c6";

static void on_boot_button(void)
{
    ESP_LOGI(TAG, "BOOT pressed — permit join");
    zigbee_coordinator_permit_join(true);
}

static void wait_for_zigbee_network(void)
{
    const int timeout_ms = 60000;
    const int step_ms = 500;
    int waited = 0;

    ESP_LOGI(TAG, "Waiting for Zigbee network (up to %d s)...", timeout_ms / 1000);
    while (!zigbee_coordinator_network_ready() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
        if ((waited % 5000) == 0) {
            ESP_LOGI(TAG, "Still forming Zigbee... %d s", waited / 1000);
        }
    }
    if (zigbee_coordinator_network_ready()) {
        ESP_LOGI(TAG, "Zigbee network ready");
    } else {
        ESP_LOGW(TAG, "Zigbee network not ready after %d s", timeout_ms / 1000);
    }
}

static void net_status_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        (void)ipc_host_net_status(wifi_net_is_connected(), mqtt_bridge_is_connected(), wifi_net_rssi());
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "HALite C6 radio starting (Zigbee + Wi-Fi, UART IPC to P4)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(device_registry_init());
    ESP_ERROR_CHECK(board_io_init(on_boot_button));
    ESP_ERROR_CHECK(ipc_host_start());

    ESP_ERROR_CHECK(zigbee_coordinator_start());
    wait_for_zigbee_network();

    ESP_ERROR_CHECK(wifi_net_start());
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
    ESP_LOGI(TAG, "WiFi + IEEE802.15.4 software coexistence enabled");
#endif

    if (wifi_net_wait_connected(pdMS_TO_TICKS(60000)) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected yet — MQTT will retry");
    }

    ESP_ERROR_CHECK(mqtt_bridge_start());
    (void)ipc_host_net_status(wifi_net_is_connected(), mqtt_bridge_is_connected(), wifi_net_rssi());
    xTaskCreate(net_status_task, "net_status", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "C6 radio running. BOOT or IPC PERMIT_JOIN to pair Zigbee.");
}
