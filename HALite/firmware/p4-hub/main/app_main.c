#include "console_cmd.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "http_api.h"
#include "ipc_link.h"
#include "nvs_flash.h"
#include "registry.h"
#include "sdkconfig.h"
#include "usb_ncm.h"
#if CONFIG_HALITE_ESP_HOSTED
#include "hosted_wifi.h"
#endif

#if CONFIG_HALITE_P4_ETHERNET
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#endif

static const char *TAG = "halite-p4";

static void on_registry_changed(const halite_entity_t *e, void *ctx)
{
    (void)ctx;
    http_api_broadcast_entity(e);
}

#if CONFIG_HALITE_P4_ETHERNET
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet up %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
                 mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet down");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "ETH IP " IPSTR, IP2STR(&event->ip_info.ip));
}

static esp_err_t start_ethernet(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_HALITE_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_HALITE_ETH_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_HALITE_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_HALITE_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "Ethernet MAC/PHY alloc failed");
        return ESP_FAIL;
    }

    esp_eth_handle_t eth = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    return ESP_OK;
}
#endif

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "halite>";
    repl_config.max_cmdline_length = 256;

    ESP_ERROR_CHECK(esp_console_register_help_command());
    console_cmd_register();

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
#else
    ESP_LOGW(TAG, "No console channel — skip REPL");
    return;
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    ESP_LOGI(TAG, "HALite P4 hub starting (registry + UART IPC to C6)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    registry_init();
    registry_set_changed_cb(on_registry_changed, NULL);
    ESP_ERROR_CHECK(ipc_link_start());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_HALITE_P4_ETHERNET
    if (start_ethernet() != ESP_OK) {
        ESP_LOGW(TAG, "Ethernet failed — UI only via console");
    }
#endif
#if CONFIG_HALITE_ESP_HOSTED
    if (hosted_wifi_start() != ESP_OK) {
        ESP_LOGW(TAG, "Hosted Wi-Fi failed — LAN API not up");
    }
#endif
#if CONFIG_HALITE_USB_NCM
    if (usb_ncm_start() != ESP_OK) {
        ESP_LOGW(TAG, "USB NCM failed — HTTP API has no interface");
    }
#endif

    ESP_ERROR_CHECK(http_api_start());
    start_console();

    ESP_LOGI(TAG, "Ready. USB console: entities / permit / toggle / ping");
}
