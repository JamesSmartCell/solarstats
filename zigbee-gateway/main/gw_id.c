#include "gw_id.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "gw_id";

static char s_mac[13];
static char s_bridge[20];
static char s_name[48];
static char s_prefix[40];
static char s_status[56];
static char s_permit_join[64];
static char s_permit_state[80];
static char s_info[56];
static char s_remove[56];
static char s_rediscover[64];
static char s_ota[56];
static char s_ota_state[64];
static char s_switch_wild[56];
static char s_power_on_wild[72];
static bool s_ready;

void zbgw_id_init(void)
{
    if (s_ready) {
        return;
    }

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);

    snprintf(s_bridge, sizeof(s_bridge), "zbgw_%s", s_mac);
    snprintf(s_name, sizeof(s_name), "ESP32-C6 Zigbee Gateway %02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_prefix, sizeof(s_prefix), "%s/%s", CONFIG_ZBGW_MQTT_TOPIC_PREFIX, s_mac);

    snprintf(s_status, sizeof(s_status), "%s/bridge/status", s_prefix);
    snprintf(s_permit_join, sizeof(s_permit_join), "%s/bridge/permit_join", s_prefix);
    snprintf(s_permit_state, sizeof(s_permit_state), "%s/bridge/permit_join/state", s_prefix);
    snprintf(s_info, sizeof(s_info), "%s/bridge/info", s_prefix);
    snprintf(s_remove, sizeof(s_remove), "%s/bridge/remove", s_prefix);
    snprintf(s_rediscover, sizeof(s_rediscover), "%s/bridge/rediscover", s_prefix);
    snprintf(s_ota, sizeof(s_ota), "%s/bridge/ota", s_prefix);
    snprintf(s_ota_state, sizeof(s_ota_state), "%s/bridge/ota/state", s_prefix);
    snprintf(s_switch_wild, sizeof(s_switch_wild), "%s/+/switch/set", s_prefix);
    snprintf(s_power_on_wild, sizeof(s_power_on_wild), "%s/+/power_on_behavior/set", s_prefix);

    s_ready = true;
    ESP_LOGI(TAG, "Gateway id %s topics %s", s_bridge, s_prefix);
}

const char *zbgw_id_mac(void)
{
    return s_mac;
}

const char *zbgw_id_bridge(void)
{
    return s_bridge;
}

const char *zbgw_id_name(void)
{
    return s_name;
}

const char *zbgw_topic_prefix(void)
{
    return s_prefix;
}

const char *zbgw_topic_status(void)
{
    return s_status;
}

const char *zbgw_topic_permit_join(void)
{
    return s_permit_join;
}

const char *zbgw_topic_permit_state(void)
{
    return s_permit_state;
}

const char *zbgw_topic_info(void)
{
    return s_info;
}

const char *zbgw_topic_remove(void)
{
    return s_remove;
}

const char *zbgw_topic_rediscover(void)
{
    return s_rediscover;
}

const char *zbgw_topic_ota(void)
{
    return s_ota;
}

const char *zbgw_topic_ota_state(void)
{
    return s_ota_state;
}

const char *zbgw_topic_switch_set_wildcard(void)
{
    return s_switch_wild;
}

const char *zbgw_topic_power_on_set_wildcard(void)
{
    return s_power_on_wild;
}
