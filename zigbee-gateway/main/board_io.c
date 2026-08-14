#include "board_io.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "board_io";

static board_io_button_cb_t s_boot_cb;
static QueueHandle_t s_btn_q;
static int64_t s_last_press_us;

static void IRAM_ATTR boot_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_last_press_us < 300000) {
        return;
    }
    s_last_press_us = now;
    BaseType_t hp = pdFALSE;
    uint8_t evt = 1;
    xQueueSendFromISR(s_btn_q, &evt, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    (void)arg;
    uint8_t evt;
    while (true) {
        if (xQueueReceive(s_btn_q, &evt, portMAX_DELAY) == pdTRUE) {
            if (s_boot_cb) {
                s_boot_cb();
            }
        }
    }
}

esp_err_t board_io_init(board_io_button_cb_t boot_cb)
{
    s_boot_cb = boot_cb;
    s_btn_q = xQueueCreate(4, sizeof(uint8_t));
    if (!s_btn_q) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << CONFIG_ZBGW_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(CONFIG_ZBGW_LED_GPIO, 0);

    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << CONFIG_ZBGW_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_ZBGW_BOOT_GPIO, boot_isr, NULL));
    xTaskCreate(button_task, "boot_btn", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "LED GPIO%d, BOOT GPIO%d", CONFIG_ZBGW_LED_GPIO, CONFIG_ZBGW_BOOT_GPIO);
    return ESP_OK;
}

void board_io_set_led(bool on)
{
    gpio_set_level(CONFIG_ZBGW_LED_GPIO, on ? 1 : 0);
}

void board_io_blink_led(unsigned count, unsigned on_ms, unsigned off_ms)
{
    for (unsigned i = 0; i < count; ++i) {
        board_io_set_led(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        board_io_set_led(false);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}
