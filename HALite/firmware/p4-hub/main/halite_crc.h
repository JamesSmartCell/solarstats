#pragma once

#include <stddef.h>
#include <stdint.h>

uint8_t halite_crc8(const uint8_t *data, size_t len);
uint16_t halite_crc16_ccitt(const uint8_t *data, size_t len);
