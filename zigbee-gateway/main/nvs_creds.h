#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define NVS_CREDS_SSID_MAX 32
#define NVS_CREDS_PASS_MAX 64
#define NVS_CREDS_HOST_MAX 63
#define NVS_CREDS_USER_MAX 31

typedef struct {
    char wifi_ssid[NVS_CREDS_SSID_MAX + 1];
    char wifi_pass[NVS_CREDS_PASS_MAX + 1];
    char mqtt_host[NVS_CREDS_HOST_MAX + 1];
    char mqtt_user[NVS_CREDS_USER_MAX + 1];
    char mqtt_pass[NVS_CREDS_PASS_MAX + 1];
    bool diag_opt_in;
} zbgw_creds_t;

/* Load NVS. Only a portal-saved network counts as configured. */
esp_err_t nvs_creds_init(void);
bool nvs_creds_is_configured(void);
esp_err_t nvs_creds_get(zbgw_creds_t *out);
esp_err_t nvs_creds_set(const zbgw_creds_t *in);
esp_err_t nvs_creds_clear(void);
void nvs_creds_kconfig_defaults(zbgw_creds_t *out);
/* Next boot opens ZIGBEE_SETUP even if NVS still has an SSID. */
esp_err_t nvs_creds_request_setup(void);
/* Missing NVS key counts as opted in (setup checkbox default). */
bool nvs_creds_diag_opt_in(void);
