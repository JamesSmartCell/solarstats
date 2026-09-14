#pragma once

/* STA MAC baked into MQTT topics and HA discovery so one image works on many C6s. */

void zbgw_id_init(void);

const char *zbgw_id_mac(void);
const char *zbgw_id_bridge(void);
const char *zbgw_id_name(void);
const char *zbgw_topic_prefix(void);

const char *zbgw_topic_status(void);
const char *zbgw_topic_permit_join(void);
const char *zbgw_topic_permit_state(void);
const char *zbgw_topic_info(void);
const char *zbgw_topic_remove(void);
const char *zbgw_topic_rediscover(void);
const char *zbgw_topic_ota(void);
const char *zbgw_topic_ota_state(void);
const char *zbgw_topic_switch_set_wildcard(void);
const char *zbgw_topic_power_on_set_wildcard(void);
