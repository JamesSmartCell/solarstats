#pragma once

#include "esp_err.h"
#include "registry.h"

esp_err_t http_api_start(void);
void http_api_broadcast_entity(const halite_entity_t *e);
