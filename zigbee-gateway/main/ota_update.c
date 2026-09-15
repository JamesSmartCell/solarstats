#include "ota_update.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "gw_id.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_bridge.h"
#include "sdkconfig.h"
#include "wifi_net.h"

static const char *TAG = "ota";
static volatile bool s_busy;
static esp_timer_handle_t s_period_timer;

#define OTA_PERIOD_US (12ULL * 60 * 60 * 1000000ULL)

/* Compare dotted numeric versions (1.2.10 > 1.2.9). Extra suffix is ignored. */
static int version_cmp(const char *a, const char *b)
{
    int am = 0, an = 0, ap = 0, bm = 0, bn = 0, bp = 0;
    int na = a ? sscanf(a, "%d.%d.%d", &am, &an, &ap) : 0;
    int nb = b ? sscanf(b, "%d.%d.%d", &bm, &bn, &bp) : 0;
    if (na < 2 || nb < 2) {
        return strcmp(a ? a : "", b ? b : "");
    }
    if (am != bm) {
        return am - bm;
    }
    if (an != bn) {
        return an - bn;
    }
    return ap - bp;
}

static void publish_ota(const char *state)
{
    (void)mqtt_bridge_publish(zbgw_topic_ota_state(), state, 1, false);
}

static void ota_task(void *arg)
{
    (void)arg;
    const char *url = CONFIG_ZBGW_OTA_URL;
    if (!url || !url[0]) {
        ESP_LOGW(TAG, "OTA URL is not set — skip");
        publish_ota("no_url");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (wifi_net_is_paused() || !wifi_net_is_connected()) {
        ESP_LOGW(TAG, "OTA skipped — Wi-Fi not ready");
        publish_ota("wifi_down");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA checking %s", url);
    publish_ota("checking");

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = {
        .http_config = &http,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        diag_report_error("ota_begin", esp_err_to_name(err));
        publish_ota("failed");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t new_app = {0};
    err = esp_https_ota_get_img_desc(handle, &new_app);
    const esp_app_desc_t *cur = esp_app_get_description();
    if (err != ESP_OK || !cur || !new_app.version[0]) {
        ESP_LOGE(TAG, "OTA image version unread: %s", esp_err_to_name(err));
        publish_ota("failed");
        (void)esp_https_ota_abort(handle);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    int cmp = version_cmp(new_app.version, cur->version);
    if (cmp <= 0) {
        if (cmp < 0) {
            ESP_LOGW(TAG, "OTA skip downgrade %s -> %s", cur->version, new_app.version);
        } else {
            ESP_LOGI(TAG, "OTA already on %s", cur->version);
        }
        publish_ota("up_to_date");
        (void)esp_https_ota_abort(handle);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OTA %s -> %s", cur->version, new_app.version);

    publish_ota("updating");
    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        diag_report_error("ota_download", esp_err_to_name(err));
        publish_ota("failed");
        (void)esp_https_ota_abort(handle);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        diag_report_error("ota_finish", esp_err_to_name(err));
        publish_ota("failed");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA complete — restarting");
    publish_ota("rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t ota_update_start(void)
{
    if (s_busy) {
        ESP_LOGW(TAG, "OTA already running");
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void boot_check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(90000));
    (void)ota_update_start();
    vTaskDelete(NULL);
}

static void period_check_cb(void *arg)
{
    (void)arg;
    (void)ota_update_start();
}

void ota_update_schedule_boot_check(void)
{
#if CONFIG_ZBGW_OTA_CHECK_ON_BOOT
    if (xTaskCreate(boot_check_task, "ota_boot", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "OTA boot check task failed");
    }
    if (!s_period_timer) {
        const esp_timer_create_args_t args = {
            .callback = &period_check_cb,
            .name = "ota_period",
        };
        if (esp_timer_create(&args, &s_period_timer) == ESP_OK) {
            (void)esp_timer_start_periodic(s_period_timer, OTA_PERIOD_US);
        }
    }
#else
    ESP_LOGI(TAG, "OTA boot check disabled");
#endif
}
