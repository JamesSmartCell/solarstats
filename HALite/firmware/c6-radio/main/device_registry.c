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
static const char *NVS_VER_KEY = "ver";
/* Bump when zbgw_device_t layout changes. Mismatched blobs must not be reinterpreted. */
static const uint32_t ZBGW_DEV_REG_VERSION = 3;

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

    uint32_t ver = 0;
    (void)nvs_get_u32(handle, NVS_VER_KEY, &ver);

    size_t size = sizeof(s_devices);
    err = nvs_get_blob(handle, NVS_KEY, s_devices, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || size != sizeof(s_devices)) {
        ESP_LOGW(TAG, "Device table layout mismatch (ver=%lu size=%u want=%u) - clearing", (unsigned long)ver,
                 (unsigned)size, (unsigned)sizeof(s_devices));
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
    ESP_LOGI(TAG, "Loaded %d devices from NVS (ver=%lu)", count, (unsigned long)ver);
    (void)device_registry_sanitize();
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (s_devices[i].in_use) {
            ESP_LOGI(TAG, "  ieee=%016llx short=0x%04x caps=0x%lx ias_ep=%u",
                     (unsigned long long)s_devices[i].ieee, s_devices[i].short_addr,
                     (unsigned long)s_devices[i].capabilities, s_devices[i].ias_ep);
        }
    }
    if (ver != ZBGW_DEV_REG_VERSION) {
        (void)device_registry_save();
    }
    return ESP_OK;
}

esp_err_t device_registry_save(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &handle), TAG, "nvs_open failed");
    esp_err_t err = nvs_set_u32(handle, NVS_VER_KEY, ZBGW_DEV_REG_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, NVS_KEY, s_devices, sizeof(s_devices));
    }
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

void device_registry_add_capability(zbgw_device_t *dev, uint32_t cap)
{
    if (!dev || !cap) {
        return;
    }
    uint32_t before = dev->capabilities;
    dev->capabilities |= cap;
    if (dev->capabilities != before) {
        dev->discovery_published = false;
        device_registry_save();
    }
}

void device_registry_clear_capabilities(zbgw_device_t *dev, uint32_t cap_mask)
{
    if (!dev || !cap_mask) {
        return;
    }
    uint32_t before = dev->capabilities;
    dev->capabilities &= ~cap_mask;
    if (dev->capabilities != before) {
        dev->discovery_published = false;
        device_registry_save();
    }
}

bool device_registry_sanitize(void)
{
    bool changed = false;
    const uint32_t plug_bits = ZBGW_CAP_ON_OFF | ZBGW_CAP_POWER | ZBGW_CAP_ENERGY;
    const uint32_t junk_bits = ZBGW_CAP_TEMPERATURE | ZBGW_CAP_HUMIDITY | ZBGW_CAP_CONTACT | ZBGW_CAP_OCCUPANCY |
                               ZBGW_CAP_TAMPER | ZBGW_CAP_SMOKE_TEST | ZBGW_CAP_BATTERY_LOW | ZBGW_CAP_BATTERY;
    /* Zone-type mapping picks one alarm class — never both contact and occupancy. */
    const uint32_t exclusive_alarms = ZBGW_CAP_CONTACT | ZBGW_CAP_OCCUPANCY | ZBGW_CAP_SMOKE;

    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        zbgw_device_t *dev = &s_devices[i];
        if (!dev->in_use) {
            continue;
        }
        uint32_t before_caps = dev->capabilities;
        uint8_t before_ias_ep = dev->ias_ep;

        const bool has_plug_caps = (dev->capabilities & plug_bits) != 0;
        const bool has_on_off_eps = dev->on_off_eps != 0;
        const bool has_smoke = (dev->capabilities & ZBGW_CAP_SMOKE) != 0;
        const uint32_t alarms = dev->capabilities & exclusive_alarms;
        const bool multiple_alarms = alarms && (alarms & (alarms - 1)) != 0;
        const bool orphan_junk = !dev->ias_ep && !has_smoke && (dev->capabilities & junk_bits) != 0;

        if (has_plug_caps || has_on_off_eps || multiple_alarms) {
            uint32_t cleaned = plug_bits;
            /* Reject ASCII/garbage on_off_eps from NVS layout shifts (e.g. 0x5f303031). */
            const bool bad_eps = dev->on_off_eps != 0 && (dev->on_off_eps & ~0xFFFFu) != 0;
            if (cleaned != before_caps || dev->ias_ep || dev->ias_zone_id || dev->ias_zone_type || bad_eps) {
                ESP_LOGW(TAG, "Sanitized plug caps 0x%lx -> 0x%lx (cleared ias_ep=%u on_off_eps=0x%lx) ieee=%016llx",
                         (unsigned long)before_caps, (unsigned long)cleaned, before_ias_ep,
                         (unsigned long)dev->on_off_eps, (unsigned long long)dev->ieee);
                dev->capabilities = cleaned;
                dev->ias_ep = 0;
                dev->ias_zone_id = 0;
                dev->ias_zone_type = 0;
                if (bad_eps) {
                    dev->on_off_eps = 0;
                }
                dev->discovery_published = false;
                changed = true;
            }
        } else if (orphan_junk) {
            uint32_t cleaned = before_caps & ~junk_bits;
            ESP_LOGW(TAG, "Sanitized orphan sensor caps 0x%lx -> 0x%lx ieee=%016llx", (unsigned long)before_caps,
                     (unsigned long)cleaned, (unsigned long long)dev->ieee);
            dev->capabilities = cleaned;
            dev->discovery_published = false;
            changed = true;
        } else if (dev->ias_ep && (dev->capabilities & ZBGW_CAP_CONTACT)) {
            uint32_t cleaned = ZBGW_CAP_CONTACT | ZBGW_CAP_BATTERY_LOW;
            if (cleaned != before_caps) {
                ESP_LOGW(TAG, "Sanitized contact caps 0x%lx -> 0x%lx ieee=%016llx", (unsigned long)before_caps,
                         (unsigned long)cleaned, (unsigned long long)dev->ieee);
                dev->capabilities = cleaned;
                dev->discovery_published = false;
                changed = true;
            }
        } else if (dev->ias_ep && has_smoke) {
            uint32_t cleaned = dev->capabilities & ~plug_bits;
            cleaned &= ~(ZBGW_CAP_TEMPERATURE | ZBGW_CAP_HUMIDITY);
            if (cleaned != before_caps) {
                ESP_LOGW(TAG, "Sanitized IAS caps 0x%lx -> 0x%lx ieee=%016llx", (unsigned long)before_caps,
                         (unsigned long)cleaned, (unsigned long long)dev->ieee);
                dev->capabilities = cleaned;
                dev->discovery_published = false;
                changed = true;
            }
        }
    }
    if (changed) {
        device_registry_save();
    }
    return changed;
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
