#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t zigbee_coordinator_start(void);
esp_err_t zigbee_coordinator_permit_join(bool enable);
esp_err_t zigbee_coordinator_set_on_off(uint64_t ieee, bool on);
esp_err_t zigbee_coordinator_remove_device(uint64_t ieee);
esp_err_t zigbee_coordinator_remove_all_switches(void);
esp_err_t zigbee_coordinator_rediscover(void);
void zigbee_coordinator_on_mqtt_connected(void);
void zigbee_coordinator_on_discovery_complete(void);
bool zigbee_coordinator_network_ready(void);

/* Dev pair-test helpers */
void zigbee_coordinator_dev_test_reset(void);
bool zigbee_coordinator_wait_on_off_join(uint32_t timeout_ms, uint64_t *ieee_out);
bool zigbee_coordinator_wait_device_announce(uint32_t timeout_ms, uint64_t *ieee_out);
