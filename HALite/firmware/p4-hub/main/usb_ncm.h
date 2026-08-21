#pragma once

#include "esp_err.h"

/** USB-NCM gadget + DHCP. P4 is 192.168.4.1. Plug the P4 USB-OTG port, not the CH340. */
esp_err_t usb_ncm_start(void);
