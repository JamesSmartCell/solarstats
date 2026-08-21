#pragma once

/**
 * HALite IPC v0 constants — mirror protocol/ipc.md
 * Shared by future c6-radio and p4-hub firmware.
 */

#include <stdint.h>

#define HALITE_IPC_MAGIC     0x484Cu
#define HALITE_IPC_VERSION   0x01u
#define HALITE_IPC_MAX_PAYLOAD 512u

#define HALITE_IPC_TYPE_PING              0x01u
#define HALITE_IPC_TYPE_PONG              0x02u
#define HALITE_IPC_TYPE_PERMIT_JOIN       0x10u
#define HALITE_IPC_TYPE_PERMIT_JOIN_STATE 0x11u
#define HALITE_IPC_TYPE_DEVICE_JOINED     0x20u
#define HALITE_IPC_TYPE_DEVICE_LEFT       0x21u
#define HALITE_IPC_TYPE_ATTR_REPORT       0x22u
#define HALITE_IPC_TYPE_ESPHOME_ENTITY    0x28u
#define HALITE_IPC_TYPE_ESPHOME_STATE     0x29u
#define HALITE_IPC_TYPE_CMD_SET_ON_OFF    0x30u
#define HALITE_IPC_TYPE_CMD_REMOVE_DEVICE 0x31u
#define HALITE_IPC_TYPE_CMD_REDISCOVER    0x32u
#define HALITE_IPC_TYPE_CMD_ESPHOME_SET   0x33u
#define HALITE_IPC_TYPE_CMD_RESULT        0x3Fu
#define HALITE_IPC_TYPE_NET_STATUS        0x40u
#define HALITE_IPC_TYPE_UNSUPPORTED       0x7Fu

#define HALITE_IPC_FLAG_NEEDS_ACK  (1u << 0)
#define HALITE_IPC_FLAG_IS_ACK     (1u << 1)
#define HALITE_IPC_FLAG_IS_NACK    (1u << 2)

/** Hosted custom-data msg_id when IPC rides SDIO instead of UART. */
#define HALITE_IPC_HOSTED_MSG_ID  0x484Cu

#define HALITE_IPC_ATTR_ON_OFF            1u
#define HALITE_IPC_ATTR_TEMP_C_X100       2u
#define HALITE_IPC_ATTR_HUMIDITY_X100     3u
#define HALITE_IPC_ATTR_CONTACT           4u
#define HALITE_IPC_ATTR_OCCUPANCY         5u
#define HALITE_IPC_ATTR_POWER_W_X10       6u
#define HALITE_IPC_ATTR_ENERGY_WH         7u
#define HALITE_IPC_ATTR_BATTERY_PCT       8u
#define HALITE_IPC_ATTR_SMOKE             9u
#define HALITE_IPC_ATTR_BATTERY_LOW       10u
