#pragma once

#include "esp_err.h"

/**
 * Dev soak: open permit-join (WiFi paused if up), wait for an On/Off device,
 * then toggle it several times. Intended for bench pairing with a plug left
 * in pairing mode.
 */
esp_err_t dev_pair_test_run(uint32_t join_timeout_ms);
