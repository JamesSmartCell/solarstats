#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t endpoint;       /* primary On/Off command endpoint (prefer 1) */
    uint8_t ias_ep;         /* IAS Zone endpoint */
    uint8_t ias_zone_id;
    uint16_t ias_zone_type; /* EZB_ZCL_IAS_ZONE_ZONE_TYPE_* */
    uint32_t capabilities;
    uint32_t on_off_eps;    /* bit (ep-1) set for each On/Off server endpoint */
    char manufacturer[33];
    char model[33];
    bool discovery_published;
    bool in_use;
} zbgw_device_t;

esp_err_t device_registry_init(void);
esp_err_t device_registry_save(void);

zbgw_device_t *device_registry_upsert(uint64_t ieee, uint16_t short_addr, uint8_t endpoint);
zbgw_device_t *device_registry_find_ieee(uint64_t ieee);
zbgw_device_t *device_registry_find_short(uint16_t short_addr);
void device_registry_remove_ieee(uint64_t ieee);

void device_registry_set_identity(zbgw_device_t *dev, const char *manufacturer, const char *model);
void device_registry_add_capability(zbgw_device_t *dev, uint32_t cap);
void device_registry_clear_capabilities(zbgw_device_t *dev, uint32_t cap_mask);
void device_registry_add_on_off_ep(zbgw_device_t *dev, uint8_t ep);
/* Strip impossible capability combos left by NVS layout migrations. Returns true if any device changed. */
bool device_registry_sanitize(void);

typedef void (*device_registry_iter_cb_t)(zbgw_device_t *dev, void *ctx);
void device_registry_foreach(device_registry_iter_cb_t cb, void *ctx);

void device_registry_ieee_to_str(uint64_t ieee, char *out, size_t out_len);
bool device_registry_ieee_from_str(const char *str, uint64_t *ieee);
