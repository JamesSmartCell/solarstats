#include "ipc_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_bridge.h"
#include "sdkconfig.h"
#include "zigbee_coordinator.h"

static const char *TAG = "ipc_host";

#define IPC_UART_NUM ((uart_port_t)CONFIG_HALITE_IPC_UART_NUM)
#define RX_BUF_SZ    1024

static SemaphoreHandle_t s_tx_mu;
static uint16_t s_seq;
static bool s_up;
static vprintf_like_t s_prev_vprintf;
static uint8_t s_rx[RX_BUF_SZ];
static size_t s_rx_len;

static int ipc_log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0 && s_up && s_tx_mu) {
        size_t w = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        if (xSemaphoreTake(s_tx_mu, pdMS_TO_TICKS(20)) == pdTRUE) {
            uart_write_bytes(IPC_UART_NUM, buf, w);
            xSemaphoreGive(s_tx_mu);
        }
    }
    if (s_prev_vprintf) {
        return s_prev_vprintf(fmt, args);
    }
    return n;
}

static uint16_t next_seq(void)
{
    s_seq++;
    if (s_seq == 0) {
        s_seq = 1;
    }
    return s_seq;
}

esp_err_t ipc_host_send(uint8_t type, uint8_t flags, const void *payload, uint16_t len)
{
    if (!s_tx_mu) {
        return ESP_ERR_INVALID_STATE;
    }
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

esp_err_t ipc_host_send_ack(uint16_t seq, uint8_t type)
{
    uint8_t frame[HALITE_IPC_MAX_FRAME];
    size_t n = ipc_encode_frame(frame, sizeof(frame), type, HALITE_IPC_FLAG_IS_ACK, seq, NULL, 0);
    if (!n) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    int w = uart_write_bytes(IPC_UART_NUM, frame, n);
    xSemaphoreGive(s_tx_mu);
    return w == (int)n ? ESP_OK : ESP_FAIL;
}

esp_err_t ipc_host_send_cmd_result(uint16_t req_seq, int32_t status)
{
    ipc_cmd_result_t r = {.req_seq = req_seq, .status = status};
    return ipc_host_send(HALITE_IPC_TYPE_CMD_RESULT, 0, &r, sizeof(r));
}

esp_err_t ipc_host_device_joined(const ipc_device_joined_t *msg)
{
    return ipc_host_send(HALITE_IPC_TYPE_DEVICE_JOINED, 0, msg, sizeof(*msg));
}

esp_err_t ipc_host_device_left(uint64_t ieee)
{
    ipc_device_left_t m = {.ieee = ieee};
    return ipc_host_send(HALITE_IPC_TYPE_DEVICE_LEFT, 0, &m, sizeof(m));
}

esp_err_t ipc_host_attr_report(uint64_t ieee, uint8_t attr_id, uint8_t ep, uint8_t value_type, uint32_t value_bits)
{
    ipc_attr_report_t m = {
        .ieee = ieee,
        .attr_id = attr_id,
        .ep = ep,
        .value_type = value_type,
        .reserved = 0,
        .value_bits = value_bits,
    };
    return ipc_host_send(HALITE_IPC_TYPE_ATTR_REPORT, 0, &m, sizeof(m));
}

esp_err_t ipc_host_permit_join_state(bool enabled)
{
    ipc_permit_join_state_t m = {.enabled = enabled ? 1 : 0};
    return ipc_host_send(HALITE_IPC_TYPE_PERMIT_JOIN_STATE, 0, &m, sizeof(m));
}

esp_err_t ipc_host_esphome_entity(const ipc_esphome_entity_t *msg)
{
    return ipc_host_send(HALITE_IPC_TYPE_ESPHOME_ENTITY, 0, msg, sizeof(*msg));
}

esp_err_t ipc_host_esphome_state(const char *entity_id, uint8_t value_type, uint32_t value_bits)
{
    ipc_esphome_state_t m = {0};
    strncpy(m.entity_id, entity_id ? entity_id : "", sizeof(m.entity_id) - 1);
    m.value_type = value_type;
    m.value_bits = value_bits;
    return ipc_host_send(HALITE_IPC_TYPE_ESPHOME_STATE, 0, &m, sizeof(m));
}

esp_err_t ipc_host_net_status(bool wifi_up, bool mqtt_up, int8_t rssi)
{
    ipc_net_status_t m = {
        .wifi_up = wifi_up ? 1 : 0,
        .mqtt_up = mqtt_up ? 1 : 0,
        .cloud_up = 0,
        .wifi_rssi = rssi,
    };
    return ipc_host_send(HALITE_IPC_TYPE_NET_STATUS, 0, &m, sizeof(m));
}

static void handle_frame(const ipc_frame_view_t *f)
{
    if (f->flags & HALITE_IPC_FLAG_IS_ACK) {
        return;
    }
    if (f->flags & HALITE_IPC_FLAG_NEEDS_ACK) {
        (void)ipc_host_send_ack(f->seq, f->type);
    }

    switch (f->type) {
    case HALITE_IPC_TYPE_PING: {
        uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        (void)ipc_host_send(HALITE_IPC_TYPE_PONG, 0, &uptime_ms, sizeof(uptime_ms));
        break;
    }
    case HALITE_IPC_TYPE_PERMIT_JOIN: {
        if (f->len < sizeof(ipc_permit_join_t)) {
            (void)ipc_host_send_cmd_result(f->seq, ESP_ERR_INVALID_SIZE);
            break;
        }
        const ipc_permit_join_t *p = (const ipc_permit_join_t *)f->payload;
        esp_err_t err = zigbee_coordinator_permit_join(p->enable != 0);
        (void)ipc_host_send_cmd_result(f->seq, (int32_t)err);
        break;
    }
    case HALITE_IPC_TYPE_CMD_SET_ON_OFF: {
        if (f->len < sizeof(ipc_cmd_set_on_off_t)) {
            (void)ipc_host_send_cmd_result(f->seq, ESP_ERR_INVALID_SIZE);
            break;
        }
        const ipc_cmd_set_on_off_t *p = (const ipc_cmd_set_on_off_t *)f->payload;
        esp_err_t err = zigbee_coordinator_set_on_off(p->ieee, p->on != 0);
        (void)ipc_host_send_cmd_result(f->seq, (int32_t)err);
        break;
    }
    case HALITE_IPC_TYPE_CMD_REMOVE_DEVICE: {
        if (f->len < sizeof(ipc_cmd_remove_t)) {
            (void)ipc_host_send_cmd_result(f->seq, ESP_ERR_INVALID_SIZE);
            break;
        }
        const ipc_cmd_remove_t *p = (const ipc_cmd_remove_t *)f->payload;
        esp_err_t err = zigbee_coordinator_remove_device(p->ieee);
        (void)ipc_host_send_cmd_result(f->seq, (int32_t)err);
        break;
    }
    case HALITE_IPC_TYPE_CMD_REDISCOVER: {
        esp_err_t err = zigbee_coordinator_rediscover();
        (void)ipc_host_send_cmd_result(f->seq, (int32_t)err);
        break;
    }
    case HALITE_IPC_TYPE_CMD_ESPHOME_SET: {
        if (f->len < sizeof(ipc_cmd_esphome_set_t)) {
            (void)ipc_host_send_cmd_result(f->seq, ESP_ERR_INVALID_SIZE);
            break;
        }
        const ipc_cmd_esphome_set_t *p = (const ipc_cmd_esphome_set_t *)f->payload;
        char id[49];
        memcpy(id, p->entity_id, 48);
        id[48] = '\0';
        esp_err_t err = mqtt_bridge_esphome_set(id, p->on != 0);
        (void)ipc_host_send_cmd_result(f->seq, (int32_t)err);
        break;
    }
    default:
        ESP_LOGW(TAG, "unsupported type 0x%02x", f->type);
        (void)ipc_host_send(HALITE_IPC_TYPE_UNSUPPORTED, 0, &f->type, 1);
        break;
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
                ESP_LOGW(TAG, "RX overflow, reset buffer");
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
                memmove(s_rx, s_rx + skip, s_rx_len - skip);
                s_rx_len -= skip;
                continue;
            }
            break;
        }
    }
}

esp_err_t ipc_host_start(void)
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

    BaseType_t ok = xTaskCreate(rx_task, "ipc_rx", 4096, NULL, 6, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_up = true;
    s_prev_vprintf = esp_log_set_vprintf(ipc_log_vprintf);
    ESP_LOGI(TAG, "IPC UART%d TX=%d RX=%d baud=%d (logs mirrored to P4)", CONFIG_HALITE_IPC_UART_NUM,
             CONFIG_HALITE_IPC_TX_GPIO, CONFIG_HALITE_IPC_RX_GPIO, CONFIG_HALITE_IPC_BAUD);
    return ESP_OK;
}

bool ipc_host_is_up(void)
{
    return s_up;
}
