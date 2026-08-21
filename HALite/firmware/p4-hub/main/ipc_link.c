#include "ipc_link.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "halite_ipc.h"
#include "ipc_codec.h"
#include "registry.h"
#include "sdkconfig.h"

static const char *TAG = "ipc_link";

#define IPC_UART_NUM ((uart_port_t)CONFIG_HALITE_IPC_UART_NUM)
#define RX_BUF_SZ    1024

static SemaphoreHandle_t s_tx_mu;
static uint16_t s_seq;
static bool s_up;
static bool s_asked_rediscover;
static uint8_t s_rx[RX_BUF_SZ];
static size_t s_rx_len;

static uint16_t next_seq(void)
{
    s_seq++;
    if (s_seq == 0) {
        s_seq = 1;
    }
    return s_seq;
}

static esp_err_t send_raw(uint8_t type, uint8_t flags, const void *payload, uint16_t len)
{
    uint8_t frame[HALITE_IPC_MAX_FRAME];
    size_t n = ipc_encode_frame(frame, sizeof(frame), type, flags, next_seq(), payload, len);
    if (!n) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    int w = uart_write_bytes(IPC_UART_NUM, frame, n);
    xSemaphoreGive(s_tx_mu);
    return w == (int)n ? ESP_OK : ESP_FAIL;
}

esp_err_t ipc_link_ping(void)
{
    return send_raw(HALITE_IPC_TYPE_PING, HALITE_IPC_FLAG_NEEDS_ACK, NULL, 0);
}

esp_err_t ipc_link_permit_join(bool enable, uint8_t seconds)
{
    ipc_permit_join_t p = {.enable = enable ? 1 : 0, .seconds = seconds};
    return send_raw(HALITE_IPC_TYPE_PERMIT_JOIN, HALITE_IPC_FLAG_NEEDS_ACK, &p, sizeof(p));
}

esp_err_t ipc_link_set_on_off(uint64_t ieee, bool on)
{
    ipc_cmd_set_on_off_t p = {.ieee = ieee, .on = on ? 1 : 0};
    return send_raw(HALITE_IPC_TYPE_CMD_SET_ON_OFF, HALITE_IPC_FLAG_NEEDS_ACK, &p, sizeof(p));
}

esp_err_t ipc_link_esphome_set(const char *entity_id, bool on)
{
    ipc_cmd_esphome_set_t p = {0};
    strncpy(p.entity_id, entity_id ? entity_id : "", sizeof(p.entity_id) - 1);
    p.on = on ? 1 : 0;
    return send_raw(HALITE_IPC_TYPE_CMD_ESPHOME_SET, HALITE_IPC_FLAG_NEEDS_ACK, &p, sizeof(p));
}

esp_err_t ipc_link_rediscover(void)
{
    return send_raw(HALITE_IPC_TYPE_CMD_REDISCOVER, HALITE_IPC_FLAG_NEEDS_ACK, NULL, 0);
}

static void handle_frame(const ipc_frame_view_t *f)
{
    if (f->flags & HALITE_IPC_FLAG_IS_ACK) {
        return;
    }

    switch (f->type) {
    case HALITE_IPC_TYPE_PONG:
        registry_set_ipc_ok(true);
        ESP_LOGI(TAG, "PONG from C6");
        if (!s_asked_rediscover) {
            s_asked_rediscover = true;
            (void)send_raw(HALITE_IPC_TYPE_CMD_REDISCOVER, HALITE_IPC_FLAG_NEEDS_ACK, NULL, 0);
        }
        break;
    case HALITE_IPC_TYPE_PERMIT_JOIN_STATE:
        if (f->len >= sizeof(ipc_permit_join_state_t)) {
            const ipc_permit_join_state_t *p = (const ipc_permit_join_state_t *)f->payload;
            registry_set_permit_join(p->enabled != 0);
        }
        break;
    case HALITE_IPC_TYPE_DEVICE_JOINED:
        if (f->len >= sizeof(ipc_device_joined_t)) {
            registry_on_device_joined((const ipc_device_joined_t *)f->payload);
        }
        break;
    case HALITE_IPC_TYPE_DEVICE_LEFT:
        if (f->len >= sizeof(ipc_device_left_t)) {
            registry_on_device_left(((const ipc_device_left_t *)f->payload)->ieee);
        }
        break;
    case HALITE_IPC_TYPE_ATTR_REPORT:
        if (f->len >= sizeof(ipc_attr_report_t)) {
            registry_on_attr_report((const ipc_attr_report_t *)f->payload);
        }
        break;
    case HALITE_IPC_TYPE_ESPHOME_ENTITY:
        if (f->len >= sizeof(ipc_esphome_entity_t)) {
            registry_on_esphome_entity((const ipc_esphome_entity_t *)f->payload);
        }
        break;
    case HALITE_IPC_TYPE_ESPHOME_STATE:
        if (f->len >= sizeof(ipc_esphome_state_t)) {
            registry_on_esphome_state((const ipc_esphome_state_t *)f->payload);
        }
        break;
    case HALITE_IPC_TYPE_NET_STATUS:
        if (f->len >= sizeof(ipc_net_status_t)) {
            const ipc_net_status_t *n = (const ipc_net_status_t *)f->payload;
            registry_set_net(n->wifi_up != 0, n->mqtt_up != 0, n->cloud_up != 0, n->wifi_rssi);
        }
        break;
    case HALITE_IPC_TYPE_CMD_RESULT:
        if (f->len >= sizeof(ipc_cmd_result_t)) {
            const ipc_cmd_result_t *r = (const ipc_cmd_result_t *)f->payload;
            ESP_LOGI(TAG, "CMD_RESULT seq=%u status=%ld", r->req_seq, (long)r->status);
        }
        break;
    case HALITE_IPC_TYPE_PING:
        /* C6 pinged us — reply PONG */
        {
            uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            (void)send_raw(HALITE_IPC_TYPE_PONG, 0, &uptime_ms, sizeof(uptime_ms));
            if (f->flags & HALITE_IPC_FLAG_NEEDS_ACK) {
                uint8_t frame[HALITE_IPC_MAX_FRAME];
                size_t n = ipc_encode_frame(frame, sizeof(frame), HALITE_IPC_TYPE_PING, HALITE_IPC_FLAG_IS_ACK, f->seq,
                                            NULL, 0);
                if (n) {
                    xSemaphoreTake(s_tx_mu, portMAX_DELAY);
                    uart_write_bytes(IPC_UART_NUM, frame, n);
                    xSemaphoreGive(s_tx_mu);
                }
            }
        }
        break;
    default:
        ESP_LOGW(TAG, "unhandled type 0x%02x", f->type);
        break;
    }
}

static void echo_c6_text(const uint8_t *data, size_t len)
{
    static char line[192];
    static size_t line_len;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[line_len] = '\0';
            if (line_len) {
                printf("C6>%s\n", line);
            }
            line_len = 0;
            continue;
        }
        if (c < 0x20 || c > 0x7e) {
            continue;
        }
        if (line_len + 1 < sizeof(line)) {
            line[line_len++] = (char)c;
        }
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t tmp[128];
    while (true) {
        int n = uart_read_bytes(IPC_UART_NUM, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (n > 0) {
            if (s_rx_len + (size_t)n > sizeof(s_rx)) {
                ESP_LOGW(TAG, "RX overflow");
                s_rx_len = 0;
            }
            if (s_rx_len + (size_t)n <= sizeof(s_rx)) {
                memcpy(s_rx + s_rx_len, tmp, (size_t)n);
                s_rx_len += (size_t)n;
            }
        }
        while (s_rx_len >= 2) {
            ipc_frame_view_t view;
            size_t skip = 0;
            size_t used = ipc_try_parse(s_rx, s_rx_len, &view, &skip);
            if (used) {
                handle_frame(&view);
                memmove(s_rx, s_rx + used, s_rx_len - used);
                s_rx_len -= used;
                continue;
            }
            if (skip) {
                echo_c6_text(s_rx, skip);
                memmove(s_rx, s_rx + skip, s_rx_len - skip);
                s_rx_len -= skip;
                continue;
            }
            break;
        }
    }
}

static void ping_once_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    (void)ipc_link_ping();
    vTaskDelete(NULL);
}

esp_err_t ipc_link_start(void)
{
    s_tx_mu = xSemaphoreCreateMutex();
    if (!s_tx_mu) {
        return ESP_ERR_NO_MEM;
    }
    uart_config_t cfg = {
        .baud_rate = CONFIG_HALITE_IPC_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(IPC_UART_NUM, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(IPC_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(IPC_UART_NUM, CONFIG_HALITE_IPC_TX_GPIO, CONFIG_HALITE_IPC_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    if (xTaskCreate(rx_task, "ipc_rx", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(ping_once_task, "ipc_ping", 3072, NULL, 3, NULL);
    s_up = true;
    ESP_LOGI(TAG, "IPC UART%d TX=%d RX=%d baud=%d", CONFIG_HALITE_IPC_UART_NUM, CONFIG_HALITE_IPC_TX_GPIO,
             CONFIG_HALITE_IPC_RX_GPIO, CONFIG_HALITE_IPC_BAUD);
    return ESP_OK;
}

bool ipc_link_is_up(void)
{
    return s_up;
}
