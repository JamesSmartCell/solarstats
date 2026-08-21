#pragma once

#include "esp_err.h"

esp_err_t hub_command(const char *entity_id, const char *action);
