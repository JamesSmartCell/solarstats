#include "device_registry.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dev_reg";
static const char *NVS_NS = "zbgw_devs";
static const char *NVS_KEY = "table";

static zbgw_device_t s_devices[ZBGW_MAX_DEVICES];

esp_err_t device_registry_init(void)
{
    memset(s_devices, 0, sizeof(s_devices));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved device table yet");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");

    size_t size = sizeof(s_devices);
    err = nvs_get_blob(handle, NVS_KEY, s_devices, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Device table layout changed - clearing NVS table");
        memset(s_devices, 0, sizeof(s_devices));
        return device_registry_save();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_get_blob failed");

    int count = 0;
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use) {
            s_devices[i].discovery_published = false;
            count++;
        }
    }
    ESP_LOGI(TAG, "Loaded %d devices from NVS", count);
    return ESP_OK;
}

esp_err_t device_registry_save(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &handle), TAG, "nvs_open failed");
    esp_err_t err = nvs_set_blob(handle, NVS_KEY, s_devices, sizeof(s_devices));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static zbgw_device_t *find_free_slot(void)
{
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (!s_devices[i].in_use) {
            return &s_devices[i];
        }
    }
    return NULL;
}

zbgw_device_t *device_registry_find_ieee(uint64_t ieee)
{
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use && s_devices[i].ieee == ieee) {
            return &s_devices[i];
        }
    }
    return NULL;
}

zbgw_device_t *device_registry_find_short(uint16_t short_addr)
{
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use && s_devices[i].short_addr == short_addr) {
            return &s_devices[i];
        }
    }
    return NULL;
}

zbgw_device_t *device_registry_upsert(uint64_t ieee, uint16_t short_addr, uint8_t endpoint)
{
    zbgw_device_t *dev = device_registry_find_ieee(ieee);
    if (!dev) {
        dev = find_free_slot();
        if (!dev) {
            ESP_LOGE(TAG, "Device table full");
            return NULL;
        }
        memset(dev, 0, sizeof(*dev));
        dev->ieee = ieee;
        dev->in_use = true;
        strncpy(dev->manufacturer, "Unknown", sizeof(dev->manufacturer) - 1);
        strncpy(dev->model, "ZigbeeSensor", sizeof(dev->model) - 1);
    }
    dev->short_addr = short_addr;
    if (endpoint) {
        dev->endpoint = endpoint;
    }
    device_registry_save();
    return dev;
}

void device_registry_remove_ieee(uint64_t ieee)
{
    zbgw_device_t *dev = device_registry_find_ieee(ieee);
    if (dev) {
        memset(dev, 0, sizeof(*dev));
        device_registry_save();
    }
}

void device_registry_set_identity(zbgw_device_t *dev, const char *manufacturer, const char *model)
{
    if (!dev) {
        return;
    }
    if (manufacturer && manufacturer[0]) {
        strncpy(dev->manufacturer, manufacturer, sizeof(dev->manufacturer) - 1);
        dev->manufacturer[sizeof(dev->manufacturer) - 1] = '\0';
    }
    if (model && model[0]) {
        strncpy(dev->model, model, sizeof(dev->model) - 1);
        dev->model[sizeof(dev->model) - 1] = '\0';
    }
    device_registry_save();
}

void device_registry_add_capability(zbgw_device_t *dev, uint8_t cap)
{
    if (!dev) {
        return;
    }
    uint8_t before = dev->capabilities;
    dev->capabilities |= cap;
    if (dev->capabilities != before) {
        dev->discovery_published = false;
        device_registry_save();
    }
}

void device_registry_add_on_off_ep(zbgw_device_t *dev, uint8_t ep)
{
    if (!dev || ep < 1 || ep > 32) {
        return;
    }
    uint32_t bit = 1u << (ep - 1);
    uint32_t before = dev->on_off_eps;
    dev->on_off_eps |= bit;

    /* Prefer endpoint 1 when present; otherwise keep the lowest On/Off ep. */
    if (!dev->endpoint || ep == 1 || (dev->endpoint != 1 && ep < dev->endpoint)) {
        dev->endpoint = ep;
    }

    if (dev->on_off_eps != before) {
        dev->discovery_published = false;
        device_registry_save();
        ESP_LOGI(TAG, "On/Off eps mask=0x%08lx primary_ep=%u ieee=%016llx", (unsigned long)dev->on_off_eps,
                 dev->endpoint, (unsigned long long)dev->ieee);
    }
}

void device_registry_foreach(device_registry_iter_cb_t cb, void *ctx)
{
    if (!cb) {
        return;
    }
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use) {
            cb(&s_devices[i], ctx);
        }
    }
}

void device_registry_ieee_to_str(uint64_t ieee, char *out, size_t out_len)
{
    if (!out || out_len < 17) {
        return;
    }
    snprintf(out, out_len, "%02x%02x%02x%02x%02x%02x%02x%02x", (unsigned)((ieee >> 56) & 0xff),
             (unsigned)((ieee >> 48) & 0xff), (unsigned)((ieee >> 40) & 0xff), (unsigned)((ieee >> 32) & 0xff),
             (unsigned)((ieee >> 24) & 0xff), (unsigned)((ieee >> 16) & 0xff), (unsigned)((ieee >> 8) & 0xff),
             (unsigned)(ieee & 0xff));
}

bool device_registry_ieee_from_str(const char *str, uint64_t *ieee)
{
    if (!str || !ieee || strlen(str) < 16) {
        return false;
    }
    uint64_t value = 0;
    for (int i = 0; i < 16; ++i) {
        char c = str[i];
        uint8_t nibble;
        if (c >= '0' && c <= '9') {
            nibble = (uint8_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = (uint8_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nibble = (uint8_t)(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | nibble;
    }
    *ieee = value;
    return true;
}
