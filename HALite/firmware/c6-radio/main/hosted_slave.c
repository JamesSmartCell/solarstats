#include "hosted_slave.h"

#if !CONFIG_HALITE_ESP_HOSTED
esp_err_t hosted_slave_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#else

#include "esp_hosted.h"
#include "esp_log.h"

static const char *TAG = "hosted_slave";

esp_err_t hosted_slave_start(void)
{
    /* Slave app_main is disabled (CONFIG_ESP_HOSTED_COPROCESSOR_APP_MAIN=n).
     * Hosted still owns Wi-Fi and SDIO; Zigbee and UART IPC stay in our app. */
    esp_err_t err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Hosted slave up (SDIO). Zigbee IPC still on UART until custom-data transport.");
    return ESP_OK;
}

#endif
