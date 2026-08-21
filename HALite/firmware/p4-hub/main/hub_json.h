#pragma once

#include "cJSON.h"
#include "registry.h"

cJSON *hub_json_entity(const halite_entity_t *e);
cJSON *hub_json_entities(void);
cJSON *hub_json_status(void);
void hub_json_print_line(cJSON *obj);
