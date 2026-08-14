#include "board_io.h"
#include "config.h"
#include "device_registry.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_bridge.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_net.h"
#include "zigbee_coordinator.h"

static const char *TAG = "zbgw";

static void on_boot_button(void)
{
    ESP_LOGI(TAG, "BOOT pressed - opening permit join");
    zigbee_coordinator_permit_join(true);
}

static void on_mqtt_permit_join(bool enable)
{
    ESP_LOGI(TAG, "MQTT permit_join -> %s", enable ? "ON" : "OFF");
    zigbee_coordinator_permit_join(enable);
}

static void on_mqtt_switch(uint64_t ieee, bool on)
{
    ESP_LOGI(TAG, "MQTT switch ieee=0x%llx -> %s", (unsigned long long)ieee, on ? "ON" : "OFF");
    zigbee_coordinator_set_on_off(ieee, on);
}

static void on_mqtt_remove(uint64_t ieee, bool all_switches)
{
    if (all_switches) {
        ESP_LOGI(TAG, "MQTT remove all switches");
        zigbee_coordinator_remove_all_switches();
        return;
    }
    ESP_LOGI(TAG, "MQTT remove ieee=0x%llx", (unsigned long long)ieee);
    zigbee_coordinator_remove_device(ieee);
}

static void on_mqtt_rediscover(void)
{
    ESP_LOGI(TAG, "MQTT rediscover");
    zigbee_coordinator_rediscover();
}

static void wait_for_zigbee_network(void)
{
    const int timeout_ms = 60000;
    const int step_ms = 500;
    int waited = 0;

    ESP_LOGI(TAG, "Waiting for Zigbee network formation (up to %d s)...", timeout_ms / 1000);
    while (!zigbee_coordinator_network_ready() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
        if ((waited % 5000) == 0) {
            ESP_LOGI(TAG, "Still forming Zigbee network... %d s", waited / 1000);
        }
    }

    if (zigbee_coordinator_network_ready()) {
        ESP_LOGI(TAG, "Zigbee network ready");
    } else {
        ESP_LOGW(TAG, "Zigbee network not ready after %d s", timeout_ms / 1000);
    }
}

#if CONFIG_ZBGW_ZIGBEE_ONLY_DIAG
static void zigbee_only_diag(void)
{
    ESP_LOGE(TAG, "======== ZIGBEE-ONLY DIAGNOSTIC (NO WIFI) ========");
    zigbee_coordinator_dev_test_reset();
    (void)zigbee_coordinator_permit_join(true);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGE(TAG, "************************************************");
    ESP_LOGE(TAG, "*** PAIR NOW — put NEW switch in pairing mode ***");
    ESP_LOGE(TAG, "*** Keep it blinking next to the FireBeetle   ***");
    ESP_LOGE(TAG, "*** Waiting indefinitely for Device announce  ***");
    ESP_LOGE(TAG, "************************************************");

    uint32_t seconds = 0;
    while (true) {
        uint64_t ieee = 0;
        if (zigbee_coordinator_wait_device_announce(10000, &ieee)) {
            ESP_LOGE(TAG, "*** SUCCESS Device announce ieee=0x%llx ***", (unsigned long long)ieee);
            /* Keep network up so interview logs can appear. */
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
        seconds += 10;
        if ((seconds % 30) == 0) {
            ESP_LOGW(TAG, "Still no Device announce after %u s — re-opening permit join",
                     (unsigned)seconds);
            (void)zigbee_coordinator_permit_join(true);
        }
    }
}
#endif

void app_main(void)
{
#if CONFIG_ZBGW_ZIGBEE_ONLY_DIAG
    ESP_LOGI(TAG, "ESP32-C6 Zigbee-only join diagnostic starting");
#else
    ESP_LOGI(TAG, "ESP32-C6 Zigbee -> MQTT gateway starting (join-restore)");
#endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(device_registry_init());
    ESP_ERROR_CHECK(board_io_init(on_boot_button));

    ESP_ERROR_CHECK(zigbee_coordinator_start());
    wait_for_zigbee_network();

#if CONFIG_ZBGW_ZIGBEE_ONLY_DIAG
    zigbee_only_diag();
    return;
#endif

    ESP_ERROR_CHECK(wifi_net_start());
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
    ESP_LOGI(TAG, "WiFi + IEEE802.15.4 software coexistence enabled");
#endif

    if (wifi_net_wait_connected(pdMS_TO_TICKS(60000)) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected yet - MQTT will retry");
    }

    ESP_ERROR_CHECK(mqtt_bridge_start(on_mqtt_permit_join, on_mqtt_switch, on_mqtt_remove, on_mqtt_rediscover));
    ESP_LOGI(TAG, "Gateway running. Press BOOT or publish ON to %s to pair devices.", ZBGW_TOPIC_PERMIT_JOIN);
}
