#include "dev_pair_test.h"

#include "config.h"
#include "device_registry.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zigbee_coordinator.h"

static const char *TAG = "dev_pair";

esp_err_t dev_pair_test_run(uint32_t join_timeout_ms)
{
    ESP_LOGW(TAG, "======== DEV PAIR TEST START ========");
    ESP_LOGW(TAG, "Do NOT reset the plug yet — wait for *** PAIR NOW ***");

    if (!zigbee_coordinator_network_ready()) {
        ESP_LOGE(TAG, "Zigbee network not ready");
        return ESP_ERR_INVALID_STATE;
    }

    zigbee_coordinator_dev_test_reset();
    esp_err_t err = zigbee_coordinator_permit_join(true);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "permit_join returned %s - continuing", esp_err_to_name(err));
    }

    /* Give BDB/ZDO permit a moment to take effect, then tell the human. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGE(TAG, "************************************************");
    ESP_LOGE(TAG, "*** PAIR NOW — factory-reset the plug NOW   ***");
    ESP_LOGE(TAG, "*** Keep it blinking next to the FireBeetle ***");
    ESP_LOGE(TAG, "*** Window: %u seconds                        ***", (unsigned)(join_timeout_ms / 1000));
    ESP_LOGE(TAG, "************************************************");

    uint64_t ieee = 0;
    if (!zigbee_coordinator_wait_on_off_join(join_timeout_ms, &ieee)) {
        ESP_LOGE(TAG, "DEV PAIR TEST FAILED: no On/Off device joined in time");
        zigbee_coordinator_permit_join(false);
        ESP_LOGW(TAG, "======== DEV PAIR TEST END (FAIL) ========");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "On/Off device joined ieee=0x%llx - waiting 3s for bind/settle",
             (unsigned long long)ieee);
    vTaskDelay(pdMS_TO_TICKS(3000));

    static const bool sequence[] = {true, false, true, false, true};
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        ESP_LOGW(TAG, "DEV TEST toggle %u/%u -> %s", (unsigned)(i + 1),
                 (unsigned)(sizeof(sequence) / sizeof(sequence[0])), sequence[i] ? "ON" : "OFF");
        err = zigbee_coordinator_set_on_off(ieee, sequence[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_on_off err=%s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    zigbee_coordinator_permit_join(false);
    ESP_LOGW(TAG, "======== DEV PAIR TEST END (joined 0x%llx) ========", (unsigned long long)ieee);
    return ESP_OK;
}
