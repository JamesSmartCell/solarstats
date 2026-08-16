/*
 * Shared compile-time configuration for the Zigbee → MQTT gateway.
 */
#pragma once

#include "sdkconfig.h"

#define ZBGW_STORAGE_PARTITION_NAME "zb_storage"

#define ZBGW_HA_GATEWAY_EP_ID 1

#define ZBGW_PRIMARY_CHANNEL_MASK   (1U << CONFIG_ZBGW_PRIMARY_CHANNEL)
#define ZBGW_SECONDARY_CHANNEL_MASK CONFIG_ZBGW_SECONDARY_CHANNEL_MASK

#define ZBGW_MANUFACTURER_NAME "\x0A" "FireBeetle"
#define ZBGW_MODEL_IDENTIFIER  "\x0A" "ZB-Gateway"

#define ZBGW_TOPIC_PREFIX CONFIG_ZBGW_MQTT_TOPIC_PREFIX

#define ZBGW_TOPIC_STATUS       ZBGW_TOPIC_PREFIX "/bridge/status"
#define ZBGW_TOPIC_PERMIT_JOIN  ZBGW_TOPIC_PREFIX "/bridge/permit_join"
#define ZBGW_TOPIC_PERMIT_STATE ZBGW_TOPIC_PREFIX "/bridge/permit_join/state"
#define ZBGW_TOPIC_INFO         ZBGW_TOPIC_PREFIX "/bridge/info"
#define ZBGW_TOPIC_REMOVE       ZBGW_TOPIC_PREFIX "/bridge/remove"
#define ZBGW_TOPIC_REDISCOVER   ZBGW_TOPIC_PREFIX "/bridge/rediscover"

#define ZBGW_HA_DISCOVERY_PREFIX "homeassistant"

#define ZBGW_MAX_DEVICES 32

#define ZBGW_CAP_TEMPERATURE (1U << 0)
#define ZBGW_CAP_HUMIDITY    (1U << 1)
#define ZBGW_CAP_CONTACT     (1U << 2)
#define ZBGW_CAP_OCCUPANCY   (1U << 3)
#define ZBGW_CAP_ON_OFF      (1U << 4)
#define ZBGW_CAP_POWER       (1U << 5)
#define ZBGW_CAP_ENERGY      (1U << 6)
#define ZBGW_CAP_SMOKE       (1U << 7)
#define ZBGW_CAP_BATTERY     (1U << 8)
#define ZBGW_CAP_TAMPER      (1U << 9)
#define ZBGW_CAP_SMOKE_TEST  (1U << 10)
#define ZBGW_CAP_BATTERY_LOW (1U << 11)

#define ZBGW_TOPIC_SWITCH_SET_WILDCARD ZBGW_TOPIC_PREFIX "/+/switch/set"

#define ZBGW_ZC_CONFIG()                                \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR, \
        .install_code_policy = false,                   \
        .zczr_config = {                                \
            .max_children = CONFIG_ZBGW_MAX_CHILDREN,   \
        },                                              \
    }

#if CONFIG_SOC_IEEE802154_SUPPORTED
#define ZBGW_PLATFORM_CONFIG()                                       \
    {                                                                \
        .storage_partition_name = ZBGW_STORAGE_PARTITION_NAME,       \
        .radio_config = {                                            \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,              \
        },                                                           \
    }
#else
#error "ESP32-C6 with native IEEE 802.15.4 is required for this gateway"
#endif

#define ZBGW_ZIGBEE_DEFAULT_CONFIG()               \
    {                                              \
        .device_config = ZBGW_ZC_CONFIG(),         \
        .platform_config = ZBGW_PLATFORM_CONFIG(), \
    }
