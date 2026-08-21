#include "usb_ncm.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/esp_netif_net_stack.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "usb_ncm";
static esp_netif_t *s_netif;

static void l2_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_netif) {
        return ESP_OK;
    }
    void *copy = malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    return esp_netif_receive(s_netif, copy, len, NULL);
}

esp_err_t usb_ncm_start(void)
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    const tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
    };
    err = tinyusb_net_init(&net_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init: %s", esp_err_to_name(err));
        return err;
    }

    static const esp_netif_ip_info_t ip_info = {
        .ip = {.addr = ESP_IP4TOADDR(192, 168, 4, 1)},
        .gw = {.addr = ESP_IP4TOADDR(192, 168, 4, 1)},
        .netmask = {.addr = ESP_IP4TOADDR(255, 255, 255, 0)},
    };
    uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .if_key = "usb_ncm",
        .if_desc = "USB NCM",
        .route_prio = 10,
    };
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = netif_transmit,
        .driver_free_rx_buffer = l2_free,
    };
    struct esp_netif_netstack_config stack_cfg = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &stack_cfg,
    };

    s_netif = esp_netif_new(&cfg);
    if (!s_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_netif, lwip_mac);
    esp_netif_action_start(s_netif, 0, 0, 0);

    ESP_LOGI(TAG, "USB NCM up — poll http://192.168.4.1/api/health (P4 USB-OTG, not CH340 COM22)");
    return ESP_OK;
}
