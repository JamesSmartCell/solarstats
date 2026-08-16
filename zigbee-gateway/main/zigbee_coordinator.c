#include "zigbee_coordinator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_io.h"
#include "config.h"
#include "device_registry.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ha_discovery.h"
#include "mqtt_bridge.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "zb_coord";

static bool s_network_ready;
static bool s_permit_join_pending;
static esp_timer_handle_t s_commission_timer;
static esp_timer_handle_t s_ias_enroll_timer;
static uint16_t s_ias_enroll_short;
static uint8_t s_pending_commission_mode;

#define ZBGW_DEV_ANNCE_BIT BIT0
static EventGroupHandle_t s_annce_events;
static uint64_t s_annce_ieee;

typedef enum {
    MATCH_TEMP = 0,
    MATCH_HUMIDITY,
    MATCH_OCCUPANCY,
    MATCH_IAS_ZONE,
    MATCH_ON_OFF,
    MATCH_ELECTRICAL,
    MATCH_METERING,
    MATCH_POWER_CONFIG,
    MATCH_COUNT,
} match_kind_t;

static uint8_t s_next_ias_zone_id = 1;

/* Scaling for electrical / metering (defaults = raw watts / Wh→kWh). */
typedef struct {
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t electrical_ep;
    uint8_t metering_ep;
    uint16_t ac_power_mult;
    uint16_t ac_power_div;
    uint16_t ac_voltage_mult;
    uint16_t ac_voltage_div;
    uint16_t ac_current_mult;
    uint16_t ac_current_div;
    uint16_t last_rms_voltage; /* raw attr */
    uint16_t last_rms_current; /* raw attr */
    int16_t last_active_power_raw;
    bool saw_nonzero_active_power;
    uint32_t meter_mult;
    uint32_t meter_div;
    double last_energy_kwh; /* monotonic guard — ignore spurious drops */
    bool has_last_energy;
    bool has_electrical;
    bool switch_known; /* true once we have seen On/Off state */
    bool switch_is_on; /* when known+off, force published power to 0 W */
    bool in_use;
} power_scale_t;

static power_scale_t s_power_scale[ZBGW_MAX_DEVICES];
static esp_timer_handle_t s_power_poll_timer;
static uint16_t s_interview_queue[ZBGW_MAX_DEVICES];
static uint8_t s_interview_q_len;
static const uint8_t s_power_probe_eps[] = {1, 2, 11};

static power_scale_t *power_scale_get_ieee(uint64_t ieee, uint16_t short_addr, bool create)
{
    power_scale_t *empty = NULL;
    for (int i = 0; i < ZBGW_MAX_DEVICES; ++i) {
        if (s_power_scale[i].in_use && s_power_scale[i].ieee == ieee) {
            if (short_addr) {
                s_power_scale[i].short_addr = short_addr;
            }
            return &s_power_scale[i];
        }
        if (!empty && !s_power_scale[i].in_use) {
            empty = &s_power_scale[i];
        }
    }
    if (!create || !empty || !ieee) {
        return NULL;
    }
    memset(empty, 0, sizeof(*empty));
    empty->in_use = true;
    empty->ieee = ieee;
    empty->short_addr = short_addr;
    empty->ac_power_mult = 1;
    empty->ac_power_div = 1;
    empty->ac_voltage_mult = 1;
    empty->ac_voltage_div = 10; /* common: raw/10 = volts */
    empty->ac_current_mult = 1;
    empty->ac_current_div = 1000; /* common: raw/1000 = amps */
    empty->meter_mult = 1;
    empty->meter_div = 1000; /* common: summation in Wh → kWh */
    return empty;
}

static void publish_power_watts(zbgw_device_t *dev, power_scale_t *scale, float watts, const char *source)
{
    char buf[32];
    if (watts < 0) {
        watts = 0;
    }
    /* Plugs often keep reporting last ActivePower after relay off — zero until ON again. */
    if (scale && scale->switch_known && !scale->switch_is_on) {
        watts = 0;
        source = "SwitchOff";
    }
    /* One decimal so sub-watt standby is not rounded to 0. */
    snprintf(buf, sizeof(buf), "%.1f", watts);
    ESP_LOGI(TAG, "Power ieee=%016llx via %s -> %s W", (unsigned long long)dev->ieee, source, buf);
    ha_discovery_publish_sensor_state(dev, "power", buf);
    device_registry_add_capability(dev, ZBGW_CAP_POWER);
}

static power_scale_t *power_scale_for_dev(zbgw_device_t *dev, bool create)
{
    if (!dev) {
        return NULL;
    }
    return power_scale_get_ieee(dev->ieee, dev->short_addr, create);
}

static uint32_t read_u24_le(const void *value)
{
    const uint8_t *p = (const uint8_t *)value;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

typedef struct {
    uint16_t short_addr;
    uint8_t endpoint;
    match_kind_t kind;
} interview_ctx_t;

static esp_timer_handle_t s_interview_timer;
static uint16_t s_interview_short;
static bool s_interview_busy;
static uint8_t s_interview_eps[16];
static uint8_t s_interview_ep_count;
static uint8_t s_interview_ep_idx;

static void interview_timer_cb(void *arg);
static void interview_request_active_eps(void);
static void interview_request_next_simple_desc(void);
static void read_on_off_attr(uint16_t short_addr, uint8_t endpoint);
static void read_ias_zone_status(uint16_t short_addr, uint8_t endpoint);
static void read_battery_percentage(uint16_t short_addr, uint8_t endpoint);
static void ias_write_cie_address(uint16_t short_addr, uint8_t endpoint);
static void ias_send_enroll_response(uint16_t short_addr, uint8_t endpoint, uint8_t zone_id);
static void ias_apply_zone_type(zbgw_device_t *dev, uint16_t zone_type);
static void ias_publish_status(zbgw_device_t *dev, uint16_t status);
static void schedule_ias_enroll_retry(uint16_t short_addr, uint64_t delay_us);

static uint16_t cluster_for_kind(match_kind_t kind)
{
    switch (kind) {
    case MATCH_TEMP:
        return EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT;
    case MATCH_HUMIDITY:
        return EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT;
    case MATCH_OCCUPANCY:
        return EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING;
    case MATCH_IAS_ZONE:
        return EZB_ZCL_CLUSTER_ID_IAS_ZONE;
    case MATCH_ON_OFF:
        return EZB_ZCL_CLUSTER_ID_ON_OFF;
    case MATCH_ELECTRICAL:
        return EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT;
    case MATCH_METERING:
        return EZB_ZCL_CLUSTER_ID_METERING;
    case MATCH_POWER_CONFIG:
        return EZB_ZCL_CLUSTER_ID_POWER_CONFIG;
    default:
        return 0;
    }
}

static uint32_t capability_for_kind(match_kind_t kind)
{
    switch (kind) {
    case MATCH_TEMP:
        return ZBGW_CAP_TEMPERATURE;
    case MATCH_HUMIDITY:
        return ZBGW_CAP_HUMIDITY;
    case MATCH_OCCUPANCY:
        return ZBGW_CAP_OCCUPANCY;
    case MATCH_IAS_ZONE:
        /* Resolved from ZoneType after read/enroll (smoke / contact / occupancy). */
        return 0;
    case MATCH_ON_OFF:
        return ZBGW_CAP_ON_OFF;
    case MATCH_ELECTRICAL:
        return ZBGW_CAP_POWER;
    case MATCH_METERING:
        return ZBGW_CAP_ENERGY | ZBGW_CAP_POWER;
    case MATCH_POWER_CONFIG:
        return ZBGW_CAP_BATTERY;
    default:
        return 0;
    }
}

static const char *kind_name(match_kind_t kind)
{
    switch (kind) {
    case MATCH_TEMP:
        return "temp";
    case MATCH_HUMIDITY:
        return "humidity";
    case MATCH_OCCUPANCY:
        return "occupancy";
    case MATCH_IAS_ZONE:
        return "ias_zone";
    case MATCH_ON_OFF:
        return "on_off";
    case MATCH_ELECTRICAL:
        return "electrical";
    case MATCH_METERING:
        return "metering";
    case MATCH_POWER_CONFIG:
        return "power_config";
    default:
        return "?";
    }
}

static uint64_t read_u48_le(const void *value)
{
    const uint8_t *p = (const uint8_t *)value;
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v |= ((uint64_t)p[i]) << (8 * i);
    }
    return v;
}

static void read_electrical_attrs(uint16_t short_addr, uint8_t endpoint);
static void read_metering_attrs(uint16_t short_addr, uint8_t endpoint);
static void find_clusters_on_device(uint16_t short_addr);
static void interview_drain_queue(void);

static float zb_s16_to_centi(int16_t value)
{
    return 1.0f * value / 100.0f;
}

static float zb_u16_to_centi(uint16_t value)
{
    return 1.0f * value / 100.0f;
}

static uint64_t ieee_from_extended(const ezb_extaddr_t *addr)
{
    return addr ? addr->u64 : 0;
}

static void ensure_discovery(zbgw_device_t *dev)
{
    if (!dev || dev->discovery_published || !dev->capabilities) {
        return;
    }
    /* Avoid discovery storms before MQTT is up (also blocks permit_join). */
    if (!mqtt_bridge_is_connected()) {
        return;
    }
    if (ha_discovery_publish_device(dev) == ESP_OK) {
        dev->discovery_published = true;
        device_registry_save();
    }
}

static void commission_timer_cb(void *arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(s_pending_commission_mode);
    esp_zigbee_lock_release();
}

static void schedule_commissioning(uint8_t mode, uint32_t delay_ms)
{
    s_pending_commission_mode = mode;
    if (!s_commission_timer) {
        const esp_timer_create_args_t args = {
            .callback = &commission_timer_cb,
            .name = "zb_comm",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_commission_timer));
    }
    esp_timer_stop(s_commission_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_commission_timer, (uint64_t)delay_ms * 1000ULL));
}

static void publish_bridge_info(void)
{
    char json[256];
    ezb_extpanid_t ext;
    ezb_nwk_get_extended_panid(&ext);
    snprintf(json, sizeof(json),
             "{\"pan_id\":\"0x%04x\",\"channel\":%d,\"short_addr\":\"0x%04x\",\"ext_pan\":\"0x%llx\"}",
             ezb_nwk_get_panid(), ezb_nwk_get_current_channel(), ezb_nwk_get_short_address(),
             (unsigned long long)ext.u64);
    mqtt_bridge_publish_info(json);
}

static void read_occupancy_attr(uint16_t short_addr, uint8_t endpoint);
static void read_ias_zone_status(uint16_t short_addr, uint8_t endpoint);

static ezb_err_t config_reporting(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id,
                                  uint8_t attr_type, int16_t change_s16, uint16_t change_u16, bool signed_change)
{
    ezb_zcl_config_report_record_t record = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id = attr_id,
        .client =
            {
                .attr_type = attr_type,
                .min_interval = 1,
                .max_interval = 300,
            },
    };
    if (signed_change) {
        record.client.reportable_change.s16 = change_s16;
    } else {
        record.client.reportable_change.u16 = change_u16;
    }

    ezb_zcl_config_report_cmd_t cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = cluster_id,
            },
        .payload.record_number = 1,
        .payload.record_field = &record,
    };
    ESP_LOGI(TAG, "Config report short=0x%04x ep=%u cluster=0x%04x attr=0x%04x", short_addr, endpoint, cluster_id,
             attr_id);
    return ezb_zcl_config_report_cmd_req(&cmd);
}

static void bind_result_cb(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    interview_ctx_t *ctx = user_ctx;
    if (!result || !ctx) {
        free(ctx);
        return;
    }

    if (result->error == EZB_ERR_NONE && result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Bind ok short=0x%04x ep=%u kind=%d", ctx->short_addr, ctx->endpoint, (int)ctx->kind);
        if (ctx->kind == MATCH_TEMP) {
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                             EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_ATTR_TYPE_INT16, 50, 0,
                             true);
        } else if (ctx->kind == MATCH_HUMIDITY) {
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                             EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_ATTR_TYPE_UINT16, 0, 100,
                             false);
        } else if (ctx->kind == MATCH_OCCUPANCY) {
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                             EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, EZB_ZCL_ATTR_TYPE_MAP8, 0, 1, false);
            /* Force a fresh read so HA is not stuck on a retained MQTT ON. */
            read_occupancy_attr(ctx->short_addr, ctx->endpoint);
        } else if (ctx->kind == MATCH_IAS_ZONE) {
            /* CIE write triggers Zone Enroll Request on most IAS devices (Heiman smoke, etc.).
             * May fail until TC auth completes (APS 0x13); DEVICE_AUTHORIZED retries sooner. */
            ias_write_cie_address(ctx->short_addr, ctx->endpoint);
            read_ias_zone_status(ctx->short_addr, ctx->endpoint);
            schedule_ias_enroll_retry(ctx->short_addr, 20000 * 1000ULL);
        } else if (ctx->kind == MATCH_ON_OFF) {
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                             EZB_ZCL_ATTR_TYPE_BOOL, 0, 1, false);
            read_on_off_attr(ctx->short_addr, ctx->endpoint);
        } else if (ctx->kind == MATCH_ELECTRICAL) {
            zbgw_device_t *dev = device_registry_find_short(ctx->short_addr);
            power_scale_t *scale = power_scale_for_dev(dev, true);
            if (scale) {
                scale->has_electrical = true;
                scale->electrical_ep = ctx->endpoint;
            }
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
                             EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID, EZB_ZCL_ATTR_TYPE_INT16, 5, 0, true);
            read_electrical_attrs(ctx->short_addr, ctx->endpoint);
        } else if (ctx->kind == MATCH_METERING) {
            zbgw_device_t *dev = device_registry_find_short(ctx->short_addr);
            power_scale_t *scale = power_scale_for_dev(dev, true);
            if (scale) {
                scale->metering_ep = ctx->endpoint;
            }
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                             EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, EZB_ZCL_ATTR_TYPE_UINT48, 0, 1,
                             false);
            /* InstantaneousDemand is often 0/unsupported on Tuya; ActivePower is preferred. */
            read_metering_attrs(ctx->short_addr, ctx->endpoint);
        } else if (ctx->kind == MATCH_POWER_CONFIG) {
            config_reporting(ctx->short_addr, ctx->endpoint, EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                             EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, EZB_ZCL_ATTR_TYPE_UINT8, 0, 2,
                             false);
            read_battery_percentage(ctx->short_addr, ctx->endpoint);
        }
    } else {
        ESP_LOGW(TAG, "Bind failed kind=%d err=0x%04x", (int)ctx->kind, result->error);
    }
    free(ctx);
}

static ezb_err_t bind_cluster(uint16_t short_addr, uint8_t endpoint, match_kind_t kind)
{
    uint16_t cluster_id = cluster_for_kind(kind);
    interview_ctx_t *local_ctx = calloc(1, sizeof(*local_ctx));
    interview_ctx_t *remote_ctx = calloc(1, sizeof(*remote_ctx));
    if (!local_ctx || !remote_ctx) {
        free(local_ctx);
        free(remote_ctx);
        return EZB_ERR_FAIL;
    }
    local_ctx->short_addr = short_addr;
    local_ctx->endpoint = endpoint;
    local_ctx->kind = kind;
    *remote_ctx = *local_ctx;

    ezb_zdo_bind_req_t *bind_local = malloc(sizeof(*bind_local));
    ezb_zdo_bind_req_t *bind_remote = malloc(sizeof(*bind_remote));
    if (!bind_local || !bind_remote) {
        free(bind_local);
        free(bind_remote);
        free(local_ctx);
        free(remote_ctx);
        return EZB_ERR_FAIL;
    }

    bind_local->dst_nwk_addr = ezb_nwk_get_short_address();
    bind_local->field.src_ep = ZBGW_HA_GATEWAY_EP_ID;
    bind_local->field.cluster_id = cluster_id;
    bind_local->field.dst_addr_mode = EZB_ADDR_MODE_EXT;
    bind_local->field.dst_ep = endpoint;
    bind_local->cb = bind_result_cb;
    bind_local->user_ctx = local_ctx;
    ezb_nwk_get_extended_address(&bind_local->field.src_addr);
    if (ezb_address_extended_by_short(short_addr, &bind_local->field.dst_addr.extended_addr) != EZB_ERR_NONE) {
        free(bind_local);
        free(bind_remote);
        free(local_ctx);
        free(remote_ctx);
        return EZB_ERR_FAIL;
    }
    ezb_zdo_bind_req(bind_local);

    bind_remote->dst_nwk_addr = short_addr;
    bind_remote->field.src_ep = endpoint;
    bind_remote->field.cluster_id = cluster_id;
    bind_remote->field.dst_addr_mode = EZB_ADDR_MODE_EXT;
    bind_remote->field.dst_ep = ZBGW_HA_GATEWAY_EP_ID;
    bind_remote->cb = bind_result_cb;
    bind_remote->user_ctx = remote_ctx;
    ezb_nwk_get_extended_address(&bind_remote->field.dst_addr.extended_addr);
    if (ezb_address_extended_by_short(short_addr, &bind_remote->field.src_addr) != EZB_ERR_NONE) {
        free(bind_remote);
        free(remote_ctx);
        return EZB_ERR_FAIL;
    }
    return ezb_zdo_bind_req(bind_remote);
}

static void read_basic_identity(uint16_t short_addr, uint8_t endpoint)
{
    uint16_t attr_field[] = {EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID};
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_BASIC,
            },
        .payload.attr_number = sizeof(attr_field) / sizeof(attr_field[0]),
        .payload.attr_field = attr_field,
    };
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static void read_occupancy_attr(uint16_t short_addr, uint8_t endpoint)
{
    static uint16_t attr_field[] = {EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID};
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
            },
        .payload.attr_number = 1,
        .payload.attr_field = attr_field,
    };
    ESP_LOGI(TAG, "Reading occupancy short=0x%04x ep=%u", short_addr, endpoint);
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static void read_ias_zone_status(uint16_t short_addr, uint8_t endpoint)
{
    static uint16_t attr_field[] = {
        EZB_ZCL_ATTR_IAS_ZONE_ZONE_TYPE_ID,
        EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID,
        EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATE_ID,
    };
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_IAS_ZONE,
            },
        .payload.attr_number = 3,
        .payload.attr_field = attr_field,
    };
    ESP_LOGI(TAG, "Reading IAS zone type/status/state short=0x%04x ep=%u", short_addr, endpoint);
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static void read_battery_percentage(uint16_t short_addr, uint8_t endpoint)
{
    static uint16_t attr_field[] = {EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID};
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            },
        .payload.attr_number = 1,
        .payload.attr_field = attr_field,
    };
    ESP_LOGI(TAG, "Reading battery %% short=0x%04x ep=%u", short_addr, endpoint);
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static void ias_write_cie_address(uint16_t short_addr, uint8_t endpoint)
{
    ezb_extaddr_t local = {0};
    ezb_nwk_get_extended_address(&local);
    static uint64_t cie_ieee;
    cie_ieee = local.u64;

    ezb_zcl_attribute_t attr = {
        .id = EZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID,
        .data =
            {
                .type = EZB_ZCL_ATTR_TYPE_EUI64,
                .size = sizeof(cie_ieee),
                .value = &cie_ieee,
            },
    };
    ezb_zcl_write_attr_cmd_t cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_IAS_ZONE,
            },
        .payload.attr_number = 1,
        .payload.attr_field = &attr,
    };
    ESP_LOGI(TAG, "Writing IAS CIE ieee=0x%llx -> short=0x%04x ep=%u", (unsigned long long)cie_ieee, short_addr,
             endpoint);
    ezb_zcl_write_attr_cmd_req(&cmd);
}

static void ias_send_enroll_response(uint16_t short_addr, uint8_t endpoint, uint8_t zone_id)
{
    ezb_zcl_ias_zone_enroll_rsp_cmd_t rsp = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .dis_default_rsp = false,
            },
        .payload =
            {
                .enroll_rsp_code = EZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS,
                .zone_id = zone_id,
            },
    };
    ESP_LOGI(TAG, "IAS enroll response short=0x%04x ep=%u zone_id=%u", short_addr, endpoint, zone_id);
    ezb_zcl_ias_zone_enroll_cmd_resp(&rsp);
}

static uint32_t ias_cap_for_zone_type(uint16_t zone_type)
{
    switch (zone_type) {
    case EZB_ZCL_IAS_ZONE_ZONE_TYPE_FIRE_SENSOR:
    case EZB_ZCL_IAS_ZONE_ZONE_TYPE_CARBON_MONOXIDE_SENSOR:
        return ZBGW_CAP_SMOKE;
    case EZB_ZCL_IAS_ZONE_ZONE_TYPE_CONTACT_SWITCH:
    case EZB_ZCL_IAS_ZONE_ZONE_TYPE_DOOR_WINDOW_HANDLE:
        return ZBGW_CAP_CONTACT;
    case EZB_ZCL_IAS_ZONE_ZONE_TYPE_MOTION_SENSOR:
        return ZBGW_CAP_OCCUPANCY;
    default:
        return ZBGW_CAP_OCCUPANCY;
    }
}

static const char *ias_alarm_suffix(uint32_t cap)
{
    if (cap & ZBGW_CAP_SMOKE) {
        return "smoke";
    }
    if (cap & ZBGW_CAP_CONTACT) {
        return "contact";
    }
    return "occupancy";
}

static void ias_apply_zone_type(zbgw_device_t *dev, uint16_t zone_type)
{
    if (!dev) {
        return;
    }
    if (dev->ias_zone_type == zone_type &&
        (dev->capabilities & (ZBGW_CAP_SMOKE | ZBGW_CAP_CONTACT | ZBGW_CAP_OCCUPANCY))) {
        return;
    }
    dev->ias_zone_type = zone_type;
    uint32_t alarm_cap = ias_cap_for_zone_type(zone_type);
    /* Drop the other alarm-style caps so HA does not keep a wrong entity. */
    uint32_t clear = ZBGW_CAP_SMOKE | ZBGW_CAP_CONTACT | ZBGW_CAP_OCCUPANCY;
    if ((dev->capabilities & clear) != alarm_cap) {
        dev->capabilities = (dev->capabilities & ~clear) | alarm_cap;
        dev->discovery_published = false;
        device_registry_save();
    } else {
        device_registry_add_capability(dev, alarm_cap);
    }
    ESP_LOGI(TAG, "IAS zone type=0x%04x -> %s ieee=%016llx", zone_type, ias_alarm_suffix(alarm_cap),
             (unsigned long long)dev->ieee);
    ensure_discovery(dev);
}

static void ias_publish_status(zbgw_device_t *dev, uint16_t status)
{
    if (!dev || !dev->ias_ep) {
        return;
    }
    uint32_t alarm_cap = ias_cap_for_zone_type(dev->ias_zone_type);
    if (!(dev->capabilities & (ZBGW_CAP_SMOKE | ZBGW_CAP_CONTACT | ZBGW_CAP_OCCUPANCY))) {
        device_registry_add_capability(dev, alarm_cap);
    }
    bool alarm = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1) != 0;
    bool tamper = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_TAMPER) != 0;
    bool batt_low = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_BATTERY) != 0;
    bool test = (status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_TEST) != 0;

    const char *suffix = ias_alarm_suffix(dev->capabilities);
    ESP_LOGI(TAG, "IAS status=0x%04x %s=%s tamper=%d batt_low=%d test=%d", status, suffix, alarm ? "ON" : "OFF",
             (int)tamper, (int)batt_low, (int)test);
    ha_discovery_publish_binary_state(dev, suffix, alarm);

    device_registry_add_capability(dev, ZBGW_CAP_TAMPER | ZBGW_CAP_BATTERY_LOW | ZBGW_CAP_SMOKE_TEST);
    ha_discovery_publish_binary_state(dev, "tamper", tamper);
    ha_discovery_publish_binary_state(dev, "battery_low", batt_low);
    ha_discovery_publish_binary_state(dev, "test", test);
    ensure_discovery(dev);
}

static void ias_enroll_retry_timer_cb(void *arg)
{
    (void)arg;
    uint16_t short_addr = s_ias_enroll_short;
    zbgw_device_t *dev = device_registry_find_short(short_addr);
    if (!dev || !dev->in_use) {
        ESP_LOGW(TAG, "IAS enroll retry: no device short=0x%04x", short_addr);
        return;
    }
    if (!dev->ias_ep) {
        return;
    }
    uint8_t ep = dev->ias_ep;
    if (!dev->ias_zone_id) {
        dev->ias_zone_id = s_next_ias_zone_id++;
        if (s_next_ias_zone_id == 0 || s_next_ias_zone_id == 0xff) {
            s_next_ias_zone_id = 1;
        }
        device_registry_save();
    }
    ESP_LOGI(TAG, "Post-auth IAS enroll retry short=0x%04x ep=%u zone_id=%u", short_addr, ep, dev->ias_zone_id);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ias_write_cie_address(short_addr, ep);
    /* Proactive enroll response: Heiman often never re-sends the enroll request. */
    ias_send_enroll_response(short_addr, ep, dev->ias_zone_id);
    read_ias_zone_status(short_addr, ep);
    read_battery_percentage(short_addr, ep);
    esp_zigbee_lock_release();
}

static void schedule_ias_enroll_retry(uint16_t short_addr, uint64_t delay_us)
{
    s_ias_enroll_short = short_addr;
    if (!s_ias_enroll_timer) {
        const esp_timer_create_args_t args = {
            .callback = &ias_enroll_retry_timer_cb,
            .name = "zb_ias_enroll",
        };
        if (esp_timer_create(&args, &s_ias_enroll_timer) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create IAS enroll timer");
            return;
        }
    }
    esp_timer_stop(s_ias_enroll_timer);
    ESP_LOGI(TAG, "IAS enroll retry in %llu ms short=0x%04x", (unsigned long long)(delay_us / 1000ULL), short_addr);
    esp_timer_start_once(s_ias_enroll_timer, delay_us);
}

static void read_on_off_attr(uint16_t short_addr, uint8_t endpoint)
{
    static uint16_t attr_field[] = {EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID};
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_ON_OFF,
            },
        .payload.attr_number = 1,
        .payload.attr_field = attr_field,
    };
    ESP_LOGI(TAG, "Reading on/off short=0x%04x ep=%u", short_addr, endpoint);
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static void read_electrical_attrs(uint16_t short_addr, uint8_t endpoint)
{
    static uint16_t attr_field[] = {
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_MULTIPLIER_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_DIVISOR_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_MULTIPLIER_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_DIVISOR_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_MULTIPLIER_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_DIVISOR_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_MULTIPLIER_ID,
        EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_DIVISOR_ID,
    };
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT,
            },
        .payload.attr_number = sizeof(attr_field) / sizeof(attr_field[0]),
        .payload.attr_field = attr_field,
    };
    ESP_LOGI(TAG, "Reading electrical measurement short=0x%04x ep=%u", short_addr, endpoint);
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static void read_metering_attrs(uint16_t short_addr, uint8_t endpoint)
{
    static uint16_t attr_field[] = {
        EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        EZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
        EZB_ZCL_ATTR_METERING_DIVISOR_ID,
        EZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID,
    };
    ezb_zcl_read_attr_cmd_t read_attr_cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = short_addr,
                .dst_ep = endpoint,
                .cluster_id = EZB_ZCL_CLUSTER_ID_METERING,
            },
        .payload.attr_number = sizeof(attr_field) / sizeof(attr_field[0]),
        .payload.attr_field = attr_field,
    };
    ESP_LOGI(TAG, "Reading metering short=0x%04x ep=%u", short_addr, endpoint);
    ezb_zcl_read_attr_cmd_req(&read_attr_cmd);
}

static match_kind_t kind_for_cluster(uint16_t cluster_id)
{
    for (match_kind_t kind = MATCH_TEMP; kind < MATCH_COUNT; ++kind) {
        if (cluster_for_kind(kind) == cluster_id) {
            return kind;
        }
    }
    return MATCH_COUNT;
}

static void apply_cluster_on_endpoint(uint16_t short_addr, uint8_t ep, uint16_t cluster_id)
{
    match_kind_t kind = kind_for_cluster(cluster_id);
    if (kind >= MATCH_COUNT) {
        ESP_LOGI(TAG, "EP %u cluster 0x%04x (ignored)", ep, cluster_id);
        return;
    }

    ezb_extaddr_t ieee_addr = {0};
    if (ezb_address_extended_by_short(short_addr, &ieee_addr) != EZB_ERR_NONE) {
        return;
    }
    uint64_t ieee = ieee_from_extended(&ieee_addr);
    /* Keep On/Off as primary endpoint; don't let metering ep overwrite switch ep. */
    uint8_t upsert_ep = (kind == MATCH_ON_OFF) ? ep : 0;
    zbgw_device_t *dev = device_registry_upsert(ieee, short_addr, upsert_ep);
    if (!dev) {
        return;
    }
    device_registry_add_capability(dev, capability_for_kind(kind));
    if (kind == MATCH_ON_OFF) {
        device_registry_add_on_off_ep(dev, ep);
    }
    if (kind == MATCH_IAS_ZONE) {
        dev->ias_ep = ep;
        if (!dev->ias_zone_id) {
            dev->ias_zone_id = s_next_ias_zone_id++;
            if (s_next_ias_zone_id == 0 || s_next_ias_zone_id == 0xff) {
                s_next_ias_zone_id = 1;
            }
            device_registry_save();
        }
    }
    power_scale_t *scale = power_scale_for_dev(dev, true);
    if (scale) {
        if (kind == MATCH_ELECTRICAL) {
            scale->has_electrical = true;
            scale->electrical_ep = ep;
        } else if (kind == MATCH_METERING) {
            scale->metering_ep = ep;
        }
    }
    read_basic_identity(short_addr, ep);
    bind_cluster(short_addr, ep, kind);
    ensure_discovery(dev);
    ESP_LOGI(TAG, "Matched %s ieee=%016llx short=0x%04x ep=%u", kind_name(kind), (unsigned long long)ieee, short_addr,
             ep);
}

static void interview_drain_queue(void)
{
    if (s_interview_busy || s_interview_q_len == 0) {
        return;
    }
    uint16_t next = s_interview_queue[0];
    memmove(&s_interview_queue[0], &s_interview_queue[1],
            (s_interview_q_len - 1) * sizeof(s_interview_queue[0]));
    s_interview_q_len--;
    find_clusters_on_device(next);
}

static void interview_finish(void)
{
    ESP_LOGI(TAG, "Interview complete short=0x%04x", s_interview_short);
    s_interview_busy = false;
    interview_drain_queue();
}

static void interview_advance_ep(void)
{
    s_interview_ep_idx++;
    if (s_interview_ep_idx >= s_interview_ep_count) {
        interview_finish();
        return;
    }
    if (!s_interview_timer) {
        const esp_timer_create_args_t args = {
            .callback = &interview_timer_cb,
            .name = "zb_interview",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_interview_timer));
    }
    esp_timer_stop(s_interview_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_interview_timer, 200 * 1000ULL));
}

static void simple_desc_result_cb(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx)
{
    uint8_t ep = (uint8_t)(uintptr_t)user_ctx;
    uint16_t short_addr = s_interview_short;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp || result->rsp->status != EZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Simple desc failed ep=%u short=0x%04x err=%d", ep, short_addr,
                 result ? (int)result->error : -1);
        interview_advance_ep();
        return;
    }

    const ezb_af_simple_desc_t *desc = &result->rsp->desc;
    ESP_LOGI(TAG, "Simple desc ep=%u profile=0x%04x device=0x%04x in=%u out=%u", desc->ep_id, desc->app_profile_id,
             desc->app_device_id, desc->app_input_cluster_count, desc->app_output_cluster_count);
    if (desc->app_cluster_list) {
        for (uint8_t i = 0; i < desc->app_input_cluster_count; ++i) {
            apply_cluster_on_endpoint(short_addr, ep ? ep : desc->ep_id, desc->app_cluster_list[i]);
        }
    }
    interview_advance_ep();
}

static void interview_request_next_simple_desc(void)
{
    if (s_interview_ep_idx >= s_interview_ep_count) {
        interview_finish();
        return;
    }
    uint8_t ep = s_interview_eps[s_interview_ep_idx];
    ESP_LOGI(TAG, "Interview simple_desc short=0x%04x ep=%u", s_interview_short, ep);
    ezb_zdo_simple_desc_req_t req = {
        .dst_nwk_addr = s_interview_short,
        .field =
            {
                .nwk_addr_of_interest = s_interview_short,
                .endpoint = ep,
            },
        .cb = simple_desc_result_cb,
        .user_ctx = (void *)(uintptr_t)ep,
    };
    if (ezb_zdo_simple_desc_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "simple_desc_req failed ep=%u", ep);
        interview_advance_ep();
    }
}

static void active_ep_result_cb(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp || result->rsp->status != EZB_ZDP_STATUS_SUCCESS ||
        result->rsp->active_ep_count == 0 || !result->rsp->active_ep_list) {
        ESP_LOGW(TAG, "Active EP failed short=0x%04x err=%d - fallback ep=1", s_interview_short,
                 result ? (int)result->error : -1);
        s_interview_eps[0] = 1;
        s_interview_ep_count = 1;
        s_interview_ep_idx = 0;
        interview_request_next_simple_desc();
        return;
    }

    s_interview_ep_count = result->rsp->active_ep_count;
    if (s_interview_ep_count > sizeof(s_interview_eps)) {
        s_interview_ep_count = sizeof(s_interview_eps);
    }
    memcpy(s_interview_eps, result->rsp->active_ep_list, s_interview_ep_count);
    s_interview_ep_idx = 0;
    ESP_LOGI(TAG, "Active EPs short=0x%04x count=%u", s_interview_short, s_interview_ep_count);
    for (uint8_t i = 0; i < s_interview_ep_count; ++i) {
        ESP_LOGI(TAG, "  ep[%u]=%u", i, s_interview_eps[i]);
    }
    interview_request_next_simple_desc();
}

static void interview_request_active_eps(void)
{
    ESP_LOGI(TAG, "Interview active_ep short=0x%04x", s_interview_short);
    ezb_zdo_active_ep_req_t req = {
        .dst_nwk_addr = s_interview_short,
        .field =
            {
                .nwk_addr_of_interest = s_interview_short,
            },
        .cb = active_ep_result_cb,
        .user_ctx = NULL,
    };
    if (ezb_zdo_active_ep_req(&req) != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "active_ep_req failed - fallback ep=1");
        s_interview_eps[0] = 1;
        s_interview_ep_count = 1;
        s_interview_ep_idx = 0;
        interview_request_next_simple_desc();
    }
}

static void interview_timer_cb(void *arg)
{
    (void)arg;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (s_interview_ep_count == 0) {
        interview_request_active_eps();
    } else {
        interview_request_next_simple_desc();
    }
    esp_zigbee_lock_release();
}

static void find_clusters_on_device(uint16_t short_addr)
{
    if (s_interview_busy) {
        if (s_interview_q_len < ZBGW_MAX_DEVICES) {
            for (uint8_t i = 0; i < s_interview_q_len; ++i) {
                if (s_interview_queue[i] == short_addr) {
                    return;
                }
            }
            s_interview_queue[s_interview_q_len++] = short_addr;
            ESP_LOGI(TAG, "Interview queued short=0x%04x (q=%u)", short_addr, s_interview_q_len);
        } else {
            ESP_LOGW(TAG, "Interview queue full - dropping short=0x%04x", short_addr);
        }
        return;
    }
    s_interview_busy = true;
    s_interview_short = short_addr;
    s_interview_ep_count = 0;
    s_interview_ep_idx = 0;
    if (!s_interview_timer) {
        const esp_timer_create_args_t args = {
            .callback = &interview_timer_cb,
            .name = "zb_interview",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_interview_timer));
    }
    esp_timer_stop(s_interview_timer);
    /* Sleepy IAS devices (smoke) often need longer before APS keys are ready. */
    ESP_LOGI(TAG, "Interview starting in 4s short=0x%04x (active_ep path)", short_addr);
    ESP_ERROR_CHECK(esp_timer_start_once(s_interview_timer, 4000 * 1000ULL));
}

static void power_poll_one(zbgw_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev || !dev->in_use || !dev->short_addr) {
        return;
    }
    if (!(dev->capabilities & (ZBGW_CAP_POWER | ZBGW_CAP_ENERGY))) {
        return;
    }
    power_scale_t *scale = power_scale_for_dev(dev, true);
    if (dev->capabilities & ZBGW_CAP_POWER) {
        if (scale && scale->electrical_ep) {
            read_electrical_attrs(dev->short_addr, scale->electrical_ep);
        } else {
            /* Probe common Tuya endpoints until interview records the real one. */
            for (size_t i = 0; i < sizeof(s_power_probe_eps); ++i) {
                read_electrical_attrs(dev->short_addr, s_power_probe_eps[i]);
            }
        }
    }
    if (dev->capabilities & ZBGW_CAP_ENERGY) {
        if (scale && scale->metering_ep) {
            read_metering_attrs(dev->short_addr, scale->metering_ep);
        } else {
            for (size_t i = 0; i < sizeof(s_power_probe_eps); ++i) {
                read_metering_attrs(dev->short_addr, s_power_probe_eps[i]);
            }
        }
    }
}

static size_t s_power_rr;

static void power_poll_count(zbgw_device_t *dev, void *ctx)
{
    size_t *n = (size_t *)ctx;
    if (dev && dev->in_use && (dev->capabilities & (ZBGW_CAP_POWER | ZBGW_CAP_ENERGY))) {
        (*n)++;
    }
}

static void power_poll_nth(zbgw_device_t *dev, void *ctx)
{
    size_t *state = (size_t *)ctx; /* [0]=target index, [1]=running index */
    if (!dev || !dev->in_use || !(dev->capabilities & (ZBGW_CAP_POWER | ZBGW_CAP_ENERGY))) {
        return;
    }
    if (state[1]++ != state[0]) {
        return;
    }
    power_poll_one(dev, NULL);
}

static void power_poll_timer_cb(void *arg)
{
    (void)arg;
    /* One device per tick — polling all plugs at once spikes RF/current → brownout on USB. */
    size_t n = 0;
    device_registry_foreach(power_poll_count, &n);
    if (!n) {
        return;
    }
    size_t state[2] = {s_power_rr % n, 0};
    s_power_rr++;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    device_registry_foreach(power_poll_nth, state);
    esp_zigbee_lock_release();
}

static void start_power_poll_timer(void)
{
    if (s_power_poll_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &power_poll_timer_cb,
        .name = "zb_power_poll",
    };
    if (esp_timer_create(&args, &s_power_poll_timer) != ESP_OK) {
        return;
    }
    /* 5s tick, round-robin → ~15s per plug with 3 metering devices (same cadence, less peak). */
    esp_timer_start_periodic(s_power_poll_timer, 5 * 1000 * 1000ULL);
    ESP_LOGI(TAG, "Power/energy poll round-robin every 5s");
}

static void reinterview_known_device(zbgw_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev || !dev->in_use || dev->short_addr == 0 || dev->short_addr == 0xffff) {
        return;
    }
    ESP_LOGI(TAG, "Re-interview known device ieee=%016llx short=0x%04x", (unsigned long long)dev->ieee,
             dev->short_addr);
    find_clusters_on_device(dev->short_addr);
}

static void reinterview_known_devices(void)
{
    device_registry_foreach(reinterview_known_device, NULL);
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Device started%s factory-new", ezb_bdb_is_factory_new() ? "" : " non-");
            if (ezb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting Zigbee network formation on channel %d...", CONFIG_ZBGW_PRIMARY_CHANNEL);
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                s_network_ready = true;
                board_io_set_led(true);
                publish_bridge_info();
                /* Match Espressif HA_on_off_switch: always open join after reboot. */
                s_permit_join_pending = false;
                ezb_bdb_open_network(CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
                mqtt_bridge_publish_permit_state(true);
                ESP_LOGI(TAG, "Coordinator reboot - network restored CH=%d PAN=0x%04x, join open %us",
                         ezb_nwk_get_current_channel(), ezb_nwk_get_panid(), CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
                start_power_poll_timer();
                reinterview_known_devices();
            }
        } else {
            ESP_LOGW(TAG, "%s failed status=0x%02x", ezb_app_signal_to_string(signal_type), status);
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Formed network PAN=0x%04x EXT=0x%llx CH=%d", ezb_nwk_get_panid(),
                     (unsigned long long)extended_pan_id.u64, ezb_nwk_get_current_channel());
            s_network_ready = true;
            board_io_set_led(true);
            publish_bridge_info();
            /* Formation success -> steer (opens join), same as Espressif example. */
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            s_permit_join_pending = false;
            ezb_bdb_open_network(CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
            mqtt_bridge_publish_permit_state(true);
            ESP_LOGI(TAG, "Network formed - join open %us (put plug in pairing mode NOW)",
                     CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
            start_power_poll_timer();
        } else {
            ESP_LOGW(TAG, "Network formation failed 0x%02x", status);
            schedule_commissioning(EZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Network steering completed (permit join active)");
            mqtt_bridge_publish_permit_state(true);
        } else {
            ESP_LOGW(TAG, "Steering failed 0x%02x", status);
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *annce = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device announce short=0x%04x ieee=0x%llx", annce->short_addr,
                 (unsigned long long)annce->device_addr.u64);
        s_annce_ieee = annce->device_addr.u64;
        if (!s_annce_events) {
            s_annce_events = xEventGroupCreate();
        }
        if (s_annce_events) {
            xEventGroupSetBits(s_annce_events, ZBGW_DEV_ANNCE_BIT);
        }
        device_registry_upsert(annce->device_addr.u64, annce->short_addr, 0);
        board_io_blink_led(2, 80, 80);
        board_io_set_led(true);
        find_clusters_on_device(annce->short_addr);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device leaving short=0x%04x", leave->short_addr);
        ha_discovery_unpublish_device(leave->device_addr.u64);
        device_registry_remove_ieee(leave->device_addr.u64);
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *auth = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device authorized short=0x%04x status=%u type=%u", auth->short_addr, auth->status, auth->type);
        if (auth->status != EZB_ZDO_AUTH_STATUS_SUCCESS) {
            break;
        }
        zbgw_device_t *dev = device_registry_find_short(auth->short_addr);
        if (!dev) {
            device_registry_upsert(auth->device_addr.u64, auth->short_addr, 0);
            dev = device_registry_find_short(auth->short_addr);
        }
        /* CIE/bind often race APS key install; retry IAS enroll after auth settles. */
        if (dev && dev->ias_ep) {
            schedule_ias_enroll_retry(auth->short_addr, 2500 * 1000ULL);
        }
    } break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        mqtt_bridge_publish_permit_state(duration != 0);
        if (duration) {
            ESP_LOGI(TAG, "Permit join open for %u s", duration);
        } else {
            ESP_LOGI(TAG, "Permit join closed");
        }
    } break;
    default:
        ESP_LOGI(TAG, "Signal %s (0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static void handle_basic_read(ezb_zcl_cmd_read_attr_rsp_message_t *message)
{
    uint16_t short_addr = message->in.header->src_addr.u.short_addr;
    zbgw_device_t *dev = device_registry_find_short(short_addr);
    if (!dev) {
        return;
    }

    char manufacturer[33] = {0};
    char model[33] = {0};
    ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables;
    while (var) {
        if (var->status == EZB_ZCL_STATUS_SUCCESS && var->attr_value) {
            uint8_t len = *(uint8_t *)var->attr_value;
            const char *str = (const char *)var->attr_value + 1;
            if (var->attr_id == EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID) {
                snprintf(manufacturer, sizeof(manufacturer), "%.*s", len, str);
            } else if (var->attr_id == EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) {
                snprintf(model, sizeof(model), "%.*s", len, str);
            }
        }
        var = var->next;
    }
    if (manufacturer[0] || model[0]) {
        device_registry_set_identity(dev, manufacturer[0] ? manufacturer : NULL, model[0] ? model : NULL);
        ensure_discovery(dev);
    }
}

static void publish_from_report(uint16_t short_addr, uint16_t cluster_id, uint16_t attr_id, const void *value)
{
    zbgw_device_t *dev = device_registry_find_short(short_addr);
    if (!dev || !value) {
        return;
    }

    char buf[32];
    if (cluster_id == EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT &&
        attr_id == EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID) {
        snprintf(buf, sizeof(buf), "%.2f", zb_s16_to_centi(*(const int16_t *)value));
        ha_discovery_publish_sensor_state(dev, "temperature", buf);
        device_registry_add_capability(dev, ZBGW_CAP_TEMPERATURE);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT &&
               attr_id == EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID) {
        snprintf(buf, sizeof(buf), "%.2f", zb_u16_to_centi(*(const uint16_t *)value));
        ha_discovery_publish_sensor_state(dev, "humidity", buf);
        device_registry_add_capability(dev, ZBGW_CAP_HUMIDITY);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING &&
               attr_id == EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID) {
        bool occupied = ((*(const uint8_t *)value) & 0x01) != 0;
        ESP_LOGI(TAG, "Occupancy short=0x%04x -> %s", short_addr, occupied ? "ON" : "OFF");
        ha_discovery_publish_binary_state(dev, "occupancy", occupied);
        device_registry_add_capability(dev, ZBGW_CAP_OCCUPANCY);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_IAS_ZONE && attr_id == EZB_ZCL_ATTR_IAS_ZONE_ZONE_TYPE_ID) {
        ias_apply_zone_type(dev, *(const uint16_t *)value);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_IAS_ZONE && attr_id == EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID) {
        ias_publish_status(dev, *(const uint16_t *)value);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_IAS_ZONE && attr_id == EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATE_ID) {
        uint8_t state = *(const uint8_t *)value;
        ESP_LOGI(TAG, "IAS zone state short=0x%04x -> %s", short_addr,
                 state == EZB_ZCL_IAS_ZONE_ZONE_STATE_ENROLLED ? "enrolled" : "not_enrolled");
        if (state != EZB_ZCL_IAS_ZONE_ZONE_STATE_ENROLLED && dev->ias_ep) {
            ias_write_cie_address(short_addr, dev->ias_ep ? dev->ias_ep : 1);
        }
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_POWER_CONFIG &&
               attr_id == EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID) {
        /* Spec: 0–200 in 0.5% units. */
        uint8_t raw = *(const uint8_t *)value;
        unsigned pct = (unsigned)raw / 2U;
        if (pct > 100) {
            pct = 100;
        }
        snprintf(buf, sizeof(buf), "%u", pct);
        ESP_LOGI(TAG, "Battery short=0x%04x raw=%u -> %s%%", short_addr, raw, buf);
        ha_discovery_publish_sensor_state(dev, "battery", buf);
        device_registry_add_capability(dev, ZBGW_CAP_BATTERY);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF && attr_id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        bool on = (*(const uint8_t *)value) != 0;
        ESP_LOGI(TAG, "On/Off short=0x%04x -> %s", short_addr, on ? "ON" : "OFF");
        ha_discovery_publish_sensor_state(dev, "switch", on ? "ON" : "OFF");
        device_registry_add_capability(dev, ZBGW_CAP_ON_OFF);
        power_scale_t *scale = power_scale_for_dev(dev, true);
        if (scale) {
            scale->switch_known = true;
            scale->switch_is_on = on;
            if (!on) {
                publish_power_watts(dev, scale, 0.0f, "SwitchOff");
            }
        }
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT) {
        power_scale_t *scale = power_scale_for_dev(dev, true);
        scale->has_electrical = true;
        if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_MULTIPLIER_ID ||
            attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_MULTIPLIER_ID) {
            uint16_t mult = *(const uint16_t *)value;
            if (mult) {
                scale->ac_power_mult = mult;
            }
            ESP_LOGI(TAG, "AC power multiplier short=0x%04x -> %u", short_addr, scale->ac_power_mult);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_POWER_DIVISOR_ID ||
                   attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_POWER_DIVISOR_ID) {
            uint16_t div = *(const uint16_t *)value;
            if (div) {
                scale->ac_power_div = div;
            }
            ESP_LOGI(TAG, "AC power divisor short=0x%04x -> %u", short_addr, scale->ac_power_div);
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_MULTIPLIER_ID) {
            uint16_t mult = *(const uint16_t *)value;
            if (mult) {
                scale->ac_voltage_mult = mult;
            }
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_VOLTAGE_DIVISOR_ID) {
            uint16_t div = *(const uint16_t *)value;
            if (div) {
                scale->ac_voltage_div = div;
            }
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_MULTIPLIER_ID) {
            uint16_t mult = *(const uint16_t *)value;
            if (mult) {
                scale->ac_current_mult = mult;
            }
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_AC_CURRENT_DIVISOR_ID) {
            uint16_t div = *(const uint16_t *)value;
            if (div) {
                scale->ac_current_div = div;
            }
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_VOLTAGE_ID) {
            uint16_t raw = *(const uint16_t *)value;
            if (raw != 0xffff) {
                scale->last_rms_voltage = raw;
                float volts = (float)raw * (float)scale->ac_voltage_mult /
                              (float)(scale->ac_voltage_div ? scale->ac_voltage_div : 1);
                ESP_LOGI(TAG, "RMS voltage short=0x%04x raw=%u -> %.1f V", short_addr, raw, volts);
                /* Many Tuya plugs leave ActivePower at 0; derive W from V*I. */
                if (!scale->saw_nonzero_active_power && scale->last_rms_current) {
                    float amps = (float)scale->last_rms_current * (float)scale->ac_current_mult /
                                 (float)(scale->ac_current_div ? scale->ac_current_div : 1);
                    publish_power_watts(dev, scale, volts * amps, "V*I");
                }
            }
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_RMS_CURRENT_ID) {
            uint16_t raw = *(const uint16_t *)value;
            if (raw != 0xffff) {
                scale->last_rms_current = raw;
                float amps = (float)raw * (float)scale->ac_current_mult /
                             (float)(scale->ac_current_div ? scale->ac_current_div : 1);
                ESP_LOGI(TAG, "RMS current short=0x%04x raw=%u -> %.3f A", short_addr, raw, amps);
                if (!scale->saw_nonzero_active_power && scale->last_rms_voltage) {
                    float volts = (float)scale->last_rms_voltage * (float)scale->ac_voltage_mult /
                                  (float)(scale->ac_voltage_div ? scale->ac_voltage_div : 1);
                    publish_power_watts(dev, scale, volts * amps, "V*I");
                }
            }
        } else if (attr_id == EZB_ZCL_ATTR_ELECTRICAL_MEASUREMENT_ACTIVE_POWER_ID) {
            int16_t raw = *(const int16_t *)value;
            scale->last_active_power_raw = raw;
            /* 0x8000 = ZCL invalid / unsupported reading */
            if (raw == (int16_t)0x8000) {
                ESP_LOGW(TAG, "Active power short=0x%04x invalid (0x8000)", short_addr);
            } else if (raw != 0) {
                scale->saw_nonzero_active_power = true;
                float watts =
                    (float)raw * (float)scale->ac_power_mult / (float)(scale->ac_power_div ? scale->ac_power_div : 1);
                ESP_LOGI(TAG, "Active power short=0x%04x raw=%d mult=%u div=%u", short_addr, (int)raw,
                         scale->ac_power_mult, scale->ac_power_div);
                publish_power_watts(dev, scale, watts, "ActivePower");
            } else {
                ESP_LOGI(TAG, "Active power short=0x%04x raw=0 (will use V*I / InstantaneousDemand fallback)",
                         short_addr);
            }
        }
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_METERING) {
        power_scale_t *scale = power_scale_for_dev(dev, true);
        if (attr_id == EZB_ZCL_ATTR_METERING_MULTIPLIER_ID) {
            /* ZCL type is uint24 — do not read as uint32 (extra byte corrupts scale → ~0 kWh). */
            uint32_t mult = read_u24_le(value);
            if (mult) {
                scale->meter_mult = mult;
            }
            ESP_LOGI(TAG, "Meter multiplier short=0x%04x -> %lu", short_addr, (unsigned long)scale->meter_mult);
        } else if (attr_id == EZB_ZCL_ATTR_METERING_DIVISOR_ID) {
            uint32_t div = read_u24_le(value);
            if (div && div < 100000000UL) {
                scale->meter_div = div;
            } else {
                ESP_LOGW(TAG, "Ignoring implausible meter divisor short=0x%04x raw=%lu", short_addr,
                         (unsigned long)div);
            }
            ESP_LOGI(TAG, "Meter divisor short=0x%04x -> %lu", short_addr, (unsigned long)scale->meter_div);
        } else if (attr_id == EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID) {
            uint64_t raw = read_u48_le(value);
            double kwh = (double)raw * (double)scale->meter_mult / (double)(scale->meter_div ? scale->meter_div : 1);
            /* Drop decreases (reboot/glitch zeros) so HA total_increasing does not reset. */
            if (scale->has_last_energy && kwh + 0.0005 < scale->last_energy_kwh) {
                ESP_LOGW(TAG,
                         "Energy drop ignored short=0x%04x raw=%llu -> %.3f kWh (kept %.3f)", short_addr,
                         (unsigned long long)raw, kwh, scale->last_energy_kwh);
            } else {
                scale->last_energy_kwh = kwh;
                scale->has_last_energy = true;
                snprintf(buf, sizeof(buf), "%.3f", kwh);
                ESP_LOGI(TAG, "Energy short=0x%04x raw=%llu mult=%lu div=%lu -> %s kWh", short_addr,
                         (unsigned long long)raw, (unsigned long)scale->meter_mult, (unsigned long)scale->meter_div,
                         buf);
                ha_discovery_publish_sensor_state(dev, "energy", buf);
                device_registry_add_capability(dev, ZBGW_CAP_ENERGY);
            }
        } else if (attr_id == EZB_ZCL_ATTR_METERING_INSTANTANEOUS_DEMAND_ID) {
            const uint8_t *p = (const uint8_t *)value;
            int32_t raw = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            if (raw & 0x800000) {
                raw |= ~0xFFFFFF;
            }
            ESP_LOGI(TAG, "Instant demand short=0x%04x raw=%ld", short_addr, (long)raw);
            /* Use when ActivePower is stuck at 0 (common on Tuya). */
            if (!scale->saw_nonzero_active_power && raw != 0) {
                publish_power_watts(dev, scale, (float)(raw < 0 ? 0 : raw), "InstantaneousDemand");
            }
        }
    }
    ensure_discovery(dev);
}

static void zcl_read_attr_rsp(ezb_zcl_cmd_read_attr_rsp_message_t *message)
{
    if (!message) {
        return;
    }
    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_BASIC) {
        handle_basic_read(message);
        return;
    }

    ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables;
    while (var) {
        if (var->status == EZB_ZCL_STATUS_SUCCESS) {
            publish_from_report(message->in.header->src_addr.u.short_addr, message->info.cluster_id, var->attr_id,
                                var->attr_value);
        }
        var = var->next;
    }
}

static void zcl_report_attr(ezb_zcl_cmd_report_attr_message_t *message)
{
    if (!message) {
        return;
    }
    ezb_zcl_report_attr_variable_t *var = message->in.variables;
    while (var) {
        publish_from_report(message->in.header->src_addr.u.short_addr, message->info.cluster_id, var->attr_id,
                            var->attr_value);
        var = var->next;
    }
}

static void zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID:
        zcl_read_attr_rsp(message);
        break;
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID:
        zcl_report_attr(message);
        break;
    case EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID:
        ESP_LOGI(TAG, "Config report response");
        break;
    case EZB_ZCL_CORE_IAS_ZONE_STATUS_CHANGE_NOTIF_CB_ID: {
        ezb_zcl_ias_zone_status_change_notif_message_t *ias = message;
        if (!ias || !ias->in.header) {
            break;
        }
        uint16_t short_addr = ias->in.header->src_addr.u.short_addr;
        uint8_t src_ep = ias->in.header->src_ep;
        zbgw_device_t *dev = device_registry_find_short(short_addr);
        if (!dev) {
            ezb_extaddr_t ieee_addr = {0};
            if (ezb_address_extended_by_short(short_addr, &ieee_addr) == EZB_ERR_NONE) {
                dev = device_registry_upsert(ieee_from_extended(&ieee_addr), short_addr, src_ep);
            }
        }
        if (dev) {
            if (!dev->ias_ep) {
                dev->ias_ep = src_ep;
            }
            ias_publish_status(dev, ias->in.payload.zone_status);
        }
    } break;
    case EZB_ZCL_CORE_IAS_ZONE_ENROLL_CB_ID: {
        ezb_zcl_ias_zone_enroll_req_message_t *req = message;
        if (!req || !req->in.header) {
            break;
        }
        uint16_t short_addr = req->in.header->src_addr.u.short_addr;
        uint8_t src_ep = req->in.header->src_ep;
        zbgw_device_t *dev = device_registry_find_short(short_addr);
        if (!dev) {
            ezb_extaddr_t ieee_addr = {0};
            if (ezb_address_extended_by_short(short_addr, &ieee_addr) == EZB_ERR_NONE) {
                dev = device_registry_upsert(ieee_from_extended(&ieee_addr), short_addr, src_ep);
            }
        }
        uint8_t zone_id = 1;
        if (dev) {
            if (!dev->ias_ep) {
                dev->ias_ep = src_ep;
            }
            if (!dev->ias_zone_id) {
                dev->ias_zone_id = s_next_ias_zone_id++;
                if (s_next_ias_zone_id == 0 || s_next_ias_zone_id == 0xff) {
                    s_next_ias_zone_id = 1;
                }
            }
            zone_id = dev->ias_zone_id;
            ias_apply_zone_type(dev, req->in.payload.zone_type);
            device_registry_save();
        }
        ESP_LOGI(TAG, "IAS enroll request short=0x%04x ep=%u zone_type=0x%04x manuf=0x%04x", short_addr, src_ep,
                 req->in.payload.zone_type, req->in.payload.manuf_code);
        ias_send_enroll_response(short_addr, src_ep, zone_id);
        req->out.result = EZB_ZCL_STATUS_SUCCESS;
        if (dev) {
            read_ias_zone_status(short_addr, src_ep);
        }
    } break;
    default:
        break;
    }
}

static esp_err_t create_gateway_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_thermostat_config_t thermostat_cfg = EZB_ZHA_THERMOSTAT_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_thermostat(ZBGW_HA_GATEWAY_EP_ID, &thermostat_cfg);

    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                        (void *)ZBGW_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                        (void *)ZBGW_MODEL_IDENTIFIER);

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep_desc, ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep_desc, ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep_desc, ezb_zcl_occupancy_sensing_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(
        ezb_af_endpoint_add_cluster_desc(ep_desc, ezb_zcl_ias_zone_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(
        ezb_af_endpoint_add_cluster_desc(ep_desc, ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep_desc, ezb_zcl_electrical_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(
        ezb_af_endpoint_add_cluster_desc(ep_desc, ezb_zcl_metering_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep_desc, ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(zcl_action_handler);
    return ESP_OK;
}

static void zigbee_main_task(void *pvParameters)
{
    (void)pvParameters;
    esp_zigbee_config_t config = ZBGW_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ZBGW_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ZBGW_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    ESP_ERROR_CHECK(create_gateway_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    /* Main loop must run immediately — formation/steering signals are processed here. */
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zigbee_coordinator_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init_partition(ZBGW_STORAGE_PARTITION_NAME));
    BaseType_t ok = xTaskCreate(zigbee_main_task, "Zigbee_main", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t zigbee_coordinator_permit_join(bool enable)
{
    if (!s_network_ready) {
        s_permit_join_pending = enable;
        mqtt_bridge_publish_permit_state(enable);
        ESP_LOGW(TAG, "Zigbee network not ready yet - permit join %s queued", enable ? "ON" : "OFF");
        return ESP_ERR_INVALID_STATE;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (enable) {
        ezb_bdb_open_network(CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        board_io_blink_led(3, 60, 60);
        board_io_set_led(true);
    } else {
        ezb_bdb_open_network(0);
    }
    esp_zigbee_lock_release();
    mqtt_bridge_publish_permit_state(enable);
    return ESP_OK;
}

bool zigbee_coordinator_network_ready(void)
{
    return s_network_ready;
}

esp_err_t zigbee_coordinator_set_on_off(uint64_t ieee, bool on)
{
    zbgw_device_t *dev = device_registry_find_ieee(ieee);
    if (!dev || !(dev->capabilities & ZBGW_CAP_ON_OFF)) {
        ESP_LOGW(TAG, "set_on_off: unknown/unsupported ieee=%016llx", (unsigned long long)ieee);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t ep = dev->endpoint ? dev->endpoint : 1;
    ezb_zcl_on_off_cmd_t cmd = {
        .cmd_ctrl =
            {
                .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .dst_addr.u.short_addr = dev->short_addr,
                .dst_ep = ep,
            },
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = on ? ezb_zcl_on_off_on_cmd_req(&cmd) : ezb_zcl_on_off_off_cmd_req(&cmd);
    esp_zigbee_lock_release();
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "On/Off cmd failed err=0x%x short=0x%04x ep=%u", (unsigned)err, dev->short_addr, ep);
        return ESP_FAIL;
    }
    ha_discovery_publish_sensor_state(dev, "switch", on ? "ON" : "OFF");
    ESP_LOGI(TAG, "On/Off cmd %s short=0x%04x ep=%u", on ? "ON" : "OFF", dev->short_addr, ep);

    power_scale_t *scale = power_scale_for_dev(dev, true);
    if (scale) {
        scale->switch_known = true;
        scale->switch_is_on = on;
    }

    /* If we never learned power/energy clusters, interview now (no re-pair needed). */
    if (!(dev->capabilities & (ZBGW_CAP_POWER | ZBGW_CAP_ENERGY))) {
        ESP_LOGI(TAG, "Missing power/energy caps - starting interview short=0x%04x", dev->short_addr);
        find_clusters_on_device(dev->short_addr);
    } else if (!on) {
        /* Final 0 W so HA / pie charts drop immediately (poll can lag with stale ActivePower). */
        publish_power_watts(dev, scale, 0.0f, "SwitchOffCmd");
    } else {
        /* Nudge a fresh power read on turn-on (helps when reporting is quiet). */
        power_poll_one(dev, NULL);
    }
    return ESP_OK;
}

esp_err_t zigbee_coordinator_remove_device(uint64_t ieee)
{
    ha_discovery_unpublish_device(ieee);
    device_registry_remove_ieee(ieee);
    return ESP_OK;
}

esp_err_t zigbee_coordinator_remove_all_switches(void)
{
    return ESP_OK;
}

static void rediscover_one(zbgw_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev || !dev->in_use || !dev->capabilities) {
        return;
    }
    if (ha_discovery_publish_device(dev) == ESP_OK) {
        dev->discovery_published = true;
    }
}

esp_err_t zigbee_coordinator_rediscover(void)
{
    ha_discovery_publish_bridge();
    device_registry_foreach(rediscover_one, NULL);
    /* Also re-interview so new clusters (power/energy) can appear without re-pairing. */
    reinterview_known_devices();
    return ESP_OK;
}

void zigbee_coordinator_dev_test_reset(void)
{
    s_annce_ieee = 0;
    if (!s_annce_events) {
        s_annce_events = xEventGroupCreate();
    }
    if (s_annce_events) {
        xEventGroupClearBits(s_annce_events, ZBGW_DEV_ANNCE_BIT);
    }
}

bool zigbee_coordinator_wait_on_off_join(uint32_t timeout_ms, uint64_t *ieee_out)
{
    (void)timeout_ms;
    (void)ieee_out;
    return false;
}

bool zigbee_coordinator_wait_device_announce(uint32_t timeout_ms, uint64_t *ieee_out)
{
    if (!s_annce_events) {
        s_annce_events = xEventGroupCreate();
        if (!s_annce_events) {
            return false;
        }
    }
    EventBits_t bits = xEventGroupWaitBits(s_annce_events, ZBGW_DEV_ANNCE_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & ZBGW_DEV_ANNCE_BIT) || !s_annce_ieee) {
        return false;
    }
    if (ieee_out) {
        *ieee_out = s_annce_ieee;
    }
    return true;
}
