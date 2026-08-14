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
#include "freertos/task.h"
#include "ha_discovery.h"
#include "mqtt_bridge.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "zb_coord";

static bool s_network_ready;
static bool s_permit_join_pending;
static esp_timer_handle_t s_commission_timer;
static uint8_t s_pending_commission_mode;

typedef enum {
    MATCH_TEMP = 0,
    MATCH_HUMIDITY,
    MATCH_OCCUPANCY,
    MATCH_IAS_ZONE,
    MATCH_COUNT,
} match_kind_t;

typedef struct {
    uint16_t short_addr;
    uint8_t endpoint;
    match_kind_t kind;
} interview_ctx_t;

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
    default:
        return 0;
    }
}

static uint8_t capability_for_kind(match_kind_t kind)
{
    switch (kind) {
    case MATCH_TEMP:
        return ZBGW_CAP_TEMPERATURE;
    case MATCH_HUMIDITY:
        return ZBGW_CAP_HUMIDITY;
    case MATCH_OCCUPANCY:
        return ZBGW_CAP_OCCUPANCY;
    case MATCH_IAS_ZONE:
        return ZBGW_CAP_CONTACT;
    default:
        return 0;
    }
}

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

static ezb_err_t config_reporting(uint16_t cluster_id, uint16_t attr_id, uint8_t attr_type, int16_t change_s16,
                                  uint16_t change_u16, bool signed_change)
{
    ezb_zcl_config_report_record_t record = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id = attr_id,
        .client =
            {
                .attr_type = attr_type,
                .min_interval = 10,
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
                .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
                .src_ep = ZBGW_HA_GATEWAY_EP_ID,
                .cluster_id = cluster_id,
            },
        .payload.record_number = 1,
        .payload.record_field = &record,
    };
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
            config_reporting(EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                             EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_ATTR_TYPE_INT16, 50, 0,
                             true);
        } else if (ctx->kind == MATCH_HUMIDITY) {
            config_reporting(EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                             EZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MEASURED_VALUE_ID, EZB_ZCL_ATTR_TYPE_UINT16, 0, 100,
                             false);
        } else if (ctx->kind == MATCH_OCCUPANCY) {
            config_reporting(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, EZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                             EZB_ZCL_ATTR_TYPE_MAP8, 0, 1, false);
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

static void match_result_cb(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    match_kind_t kind = (match_kind_t)(uintptr_t)user_ctx;
    if (!result || result->error != EZB_ERR_NONE || !result->rsp || result->rsp->status != EZB_ZDP_STATUS_SUCCESS ||
        result->rsp->match_length == 0 || !result->rsp->match_list) {
        return;
    }

    uint16_t short_addr = result->rsp->nwk_addr_of_interest;
    for (size_t i = 0; i < result->rsp->match_length; ++i) {
        uint8_t ep = result->rsp->match_list[i];
        ezb_extaddr_t ieee_addr = {0};
        if (ezb_address_extended_by_short(short_addr, &ieee_addr) != EZB_ERR_NONE) {
            continue;
        }
        uint64_t ieee = ieee_from_extended(&ieee_addr);
        zbgw_device_t *dev = device_registry_upsert(ieee, short_addr, ep);
        if (!dev) {
            continue;
        }
        device_registry_add_capability(dev, capability_for_kind(kind));
        read_basic_identity(short_addr, ep);
        bind_cluster(short_addr, ep, kind);
        ensure_discovery(dev);
        ESP_LOGI(TAG, "Matched kind=%d ieee=%016llx short=0x%04x ep=%u", (int)kind, (unsigned long long)ieee,
                 short_addr, ep);
    }
}

static void find_clusters_on_device(uint16_t short_addr)
{
    for (match_kind_t kind = MATCH_TEMP; kind < MATCH_COUNT; ++kind) {
        uint16_t cluster_list[1] = {cluster_for_kind(kind)};
        ezb_zdo_match_desc_req_t req = {
            .dst_nwk_addr = short_addr,
            .field =
                {
                    .nwk_addr_of_interest = short_addr,
                    .profile_id = EZB_AF_HA_PROFILE_ID,
                    .num_in_clusters = 1,
                    .num_out_clusters = 0,
                    .cluster_list = cluster_list,
                },
            .cb = match_result_cb,
            .user_ctx = (void *)(uintptr_t)kind,
        };
        ezb_zdo_match_desc_req(&req);
    }
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
                if (s_permit_join_pending) {
                    s_permit_join_pending = false;
                    ezb_bdb_open_network(CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
                    mqtt_bridge_publish_permit_state(true);
                    ESP_LOGI(TAG, "Coordinator reboot - network restored, applying queued permit join");
                } else {
                    mqtt_bridge_publish_permit_state(false);
                    ESP_LOGI(TAG, "Coordinator reboot - network restored");
                }
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
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            if (s_permit_join_pending) {
                s_permit_join_pending = false;
                ESP_LOGI(TAG, "Applying queued permit join request");
                ezb_bdb_open_network(CONFIG_ZBGW_PERMIT_JOIN_SECONDS);
            }
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
        device_registry_upsert(annce->device_addr.u64, annce->short_addr, 0);
        board_io_blink_led(2, 80, 80);
        board_io_set_led(true);
        find_clusters_on_device(annce->short_addr);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *leave = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Device leaving short=0x%04x", leave->short_addr);
        device_registry_remove_ieee(leave->device_addr.u64);
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
        ha_discovery_publish_binary_state(dev, "occupancy", occupied);
        device_registry_add_capability(dev, ZBGW_CAP_OCCUPANCY);
    } else if (cluster_id == EZB_ZCL_CLUSTER_ID_IAS_ZONE && attr_id == EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID) {
        uint16_t status = *(const uint16_t *)value;
        ha_discovery_publish_binary_state(dev, "contact", (status & 0x0001) != 0);
        device_registry_add_capability(dev, ZBGW_CAP_CONTACT);
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
            device_registry_add_capability(dev, ZBGW_CAP_CONTACT);
            ha_discovery_publish_binary_state(dev, "contact",
                                              (ias->in.payload.zone_status & EZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1) != 0);
            ensure_discovery(dev);
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
