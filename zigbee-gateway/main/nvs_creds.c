#include "nvs_creds.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "nvs_creds";
static const char *NS = "zbgw";

static bool s_configured;

static bool is_placeholder_pass(const char *pass)
{
    return !pass || strcmp(pass, "YOUR_WIFI_PASSWORD") == 0;
}

void nvs_creds_kconfig_defaults(zbgw_creds_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%s", CONFIG_ZBGW_WIFI_SSID);
    if (!is_placeholder_pass(CONFIG_ZBGW_WIFI_PASSWORD)) {
        snprintf(out->wifi_pass, sizeof(out->wifi_pass), "%s", CONFIG_ZBGW_WIFI_PASSWORD);
    }
    snprintf(out->mqtt_host, sizeof(out->mqtt_host), "%s", CONFIG_ZBGW_MQTT_HOST);
    snprintf(out->mqtt_user, sizeof(out->mqtt_user), "%s", CONFIG_ZBGW_MQTT_USERNAME);
    snprintf(out->mqtt_pass, sizeof(out->mqtt_pass), "%s", CONFIG_ZBGW_MQTT_PASSWORD);
    out->diag_opt_in = true;
}

static esp_err_t read_str(nvs_handle_t h, const char *key, char *out, size_t out_sz)
{
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static bool nvs_has_ssid(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char ssid[NVS_CREDS_SSID_MAX + 1] = {0};
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(h, "ssid", ssid, &len);
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

esp_err_t nvs_creds_get(zbgw_creds_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = read_str(h, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    if (err == ESP_OK) {
        err = read_str(h, "wpass", out->wifi_pass, sizeof(out->wifi_pass));
    }
    if (err == ESP_OK) {
        err = read_str(h, "mhost", out->mqtt_host, sizeof(out->mqtt_host));
    }
    if (err == ESP_OK) {
        err = read_str(h, "muser", out->mqtt_user, sizeof(out->mqtt_user));
    }
    if (err == ESP_OK) {
        err = read_str(h, "mpass", out->mqtt_pass, sizeof(out->mqtt_pass));
    }
    uint8_t diag = 1;
    if (nvs_get_u8(h, "diag", &diag) == ESP_OK) {
        out->diag_opt_in = diag != 0;
    } else {
        out->diag_opt_in = true;
    }
    nvs_close(h);
    return err;
}

esp_err_t nvs_creds_set(const zbgw_creds_t *in)
{
    if (!in || !in->wifi_ssid[0] || !in->mqtt_host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", in->wifi_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wpass", in->wifi_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "mhost", in->mqtt_host);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "muser", in->mqtt_user);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "mpass", in->mqtt_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "diag", in->diag_opt_in ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "user", 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_configured = true;
        ESP_LOGI(TAG, "Saved credentials SSID=%s MQTT=%s user=%s", in->wifi_ssid, in->mqtt_host, in->mqtt_user);
    }
    return err;
}

esp_err_t nvs_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        s_configured = false;
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    (void)nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    s_configured = false;
    ESP_LOGW(TAG, "Cleared stored credentials");
    return err;
}

static bool nvs_user_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t user = 0;
    esp_err_t err = nvs_get_u8(h, "user", &user);
    nvs_close(h);
    return err == ESP_OK && user != 0;
}

static bool nvs_take_force_setup(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t force = 0;
    bool asked = nvs_get_u8(h, "force", &force) == ESP_OK && force != 0;
    if (asked) {
        (void)nvs_erase_key(h, "force");
        (void)nvs_commit(h);
    }
    nvs_close(h);
    return asked;
}

esp_err_t nvs_creds_request_setup(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "force", 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_configured = false;
        ESP_LOGW(TAG, "Setup portal requested for next boot");
    }
    return err;
}

/* Bump to force one more Wi-Fi/MQTT wipe + setup portal. Leave at 1 after this flash. */
#define ZBGW_CREDS_WIPE_ONCE 2

static void wipe_creds_once(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t done = 0;
    (void)nvs_get_u8(h, "wipe", &done);
    nvs_close(h);
    if (done == ZBGW_CREDS_WIPE_ONCE) {
        return;
    }
    ESP_LOGW(TAG, "One-shot Wi-Fi/MQTT reset (wipe=%u)", ZBGW_CREDS_WIPE_ONCE);
    (void)nvs_creds_clear();
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, "wipe", ZBGW_CREDS_WIPE_ONCE);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t nvs_creds_init(void)
{
    s_configured = false;
    wipe_creds_once();
    bool force = nvs_take_force_setup();

    if (nvs_has_ssid() && !nvs_user_saved()) {
        zbgw_creds_t leftover;
        if (nvs_creds_get(&leftover) == ESP_OK && leftover.wifi_ssid[0]) {
            ESP_LOGW(TAG, "Ignoring leftover NVS SSID=%s (not saved via setup portal)", leftover.wifi_ssid);
        }
        (void)nvs_creds_clear();
    }

    if (!force && nvs_has_ssid() && nvs_user_saved()) {
        zbgw_creds_t creds;
        if (nvs_creds_get(&creds) == ESP_OK && creds.wifi_ssid[0] && creds.mqtt_host[0]) {
            s_configured = true;
            ESP_LOGI(TAG, "NVS credentials SSID=%s MQTT=%s", creds.wifi_ssid, creds.mqtt_host);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "No portal-saved Wi-Fi/MQTT credentials - setup portal required");
    return ESP_OK;
}

bool nvs_creds_is_configured(void)
{
    return s_configured;
}

bool nvs_creds_diag_opt_in(void)
{
    zbgw_creds_t creds;
    if (nvs_creds_get(&creds) != ESP_OK) {
        return true;
    }
    return creds.diag_opt_in;
}
