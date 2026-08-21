#pragma once

#include "esp_err.h"

/** ESP-Hosted host: C6 is the radio, P4 gets a LAN STA IP and serves httpd. */
esp_err_t hosted_wifi_start(void);
