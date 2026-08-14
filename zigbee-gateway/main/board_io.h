#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*board_io_button_cb_t)(void);

esp_err_t board_io_init(board_io_button_cb_t boot_cb);
void board_io_set_led(bool on);
void board_io_blink_led(unsigned count, unsigned on_ms, unsigned off_ms);
