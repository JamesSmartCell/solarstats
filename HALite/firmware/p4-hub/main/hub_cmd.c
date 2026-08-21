#include "hub_cmd.h"

#include <string.h>

#include "ipc_link.h"
#include "registry.h"

esp_err_t hub_command(const char *entity_id, const char *action)
{
    const halite_entity_t *e = registry_find(entity_id);
    if (!e || e->domain != HALITE_DOMAIN_SWITCH) {
        return ESP_ERR_NOT_FOUND;
    }
    bool on = e->on;
    if (strcmp(action, "turn_on") == 0) {
        on = true;
    } else if (strcmp(action, "turn_off") == 0) {
        on = false;
    } else if (strcmp(action, "toggle") == 0) {
        on = !e->on;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (e->transport == HALITE_TRANSPORT_ZIGBEE) {
        return ipc_link_set_on_off(e->ieee, on);
    }
    return ipc_link_esphome_set(e->entity_id, on);
}
