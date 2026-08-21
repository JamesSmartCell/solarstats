#include "http_api.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "hub_cmd.h"
#include "hub_json.h"
#include "ipc_link.h"
#include "sdkconfig.h"
#include "ui_html.h"

static const char *TAG = "http_api";

#define MAX_WS 4

static httpd_handle_t s_server;
static int s_ws_fds[MAX_WS];

static void add_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
}

static esp_err_t send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    add_cors(req);
    return httpd_resp_send_err(req, code, msg);
}

static bool check_auth(httpd_req_t *req)
{
    const char *tok = CONFIG_HALITE_API_TOKEN;
    if (!tok[0]) {
        return true;
    }
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        if (strncmp(hdr, "Bearer ", 7) == 0 && strcmp(hdr + 7, tok) == 0) {
            return true;
        }
    }
    send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    return false;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    add_cors(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static esp_err_t cors_options(httpd_req_t *req)
{
    add_cors(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t health_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    bool wifi = false, mqtt = false, cloud = false;
    int8_t rssi = 0;
    registry_get_net(&wifi, &mqtt, &cloud, &rssi);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "ipc_c6", registry_ipc_ok());
    cJSON_AddBoolToObject(o, "wifi", wifi);
    cJSON_AddBoolToObject(o, "mqtt", mqtt);
    cJSON_AddBoolToObject(o, "cloud", cloud);
    cJSON_AddNumberToObject(o, "wifi_rssi", rssi);
    return send_json(req, o);
}

static esp_err_t entities_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "entities", hub_json_entities());
    return send_json(req, o);
}

static bool path_entity_id(httpd_req_t *req, char *out, size_t out_len)
{
    const char *uri = req->uri;
    const char *p = strstr(uri, "/api/entities/");
    if (!p) {
        return false;
    }
    p += strlen("/api/entities/");
    char tmp[96];
    size_t n = 0;
    while (*p && *p != '/' && *p != '?' && n + 1 < sizeof(tmp)) {
        tmp[n++] = *p++;
    }
    tmp[n] = '\0';
    /* decode %2E etc. — at least + and % */
    size_t o = 0;
    for (size_t i = 0; tmp[i] && o + 1 < out_len; i++) {
        if (tmp[i] == '%' && tmp[i + 1] && tmp[i + 2]) {
            char hex[3] = {tmp[i + 1], tmp[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = tmp[i];
        }
    }
    out[o] = '\0';
    return out[0] != '\0';
}

static esp_err_t entity_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char id[64];
    if (!path_entity_id(req, id, sizeof(id))) {
        return send_err(req, HTTPD_404_NOT_FOUND, "not_found");
    }
    const halite_entity_t *e = registry_find(id);
    if (!e) {
        return send_err(req, HTTPD_404_NOT_FOUND, "not_found");
    }
    return send_json(req, hub_json_entity(e));
}

static esp_err_t entity_cmd(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char id[64];
    if (!path_entity_id(req, id, sizeof(id))) {
        return send_err(req, HTTPD_404_NOT_FOUND, "not_found");
    }
    char body[128];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "bad");
    }
    body[n] = '\0';
    cJSON *j = cJSON_Parse(body);
    const cJSON *act = j ? cJSON_GetObjectItem(j, "action") : NULL;
    const char *action = cJSON_IsString(act) ? act->valuestring : "toggle";
    esp_err_t err = hub_command(id, action);
    cJSON_Delete(j);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_err(req, HTTPD_404_NOT_FOUND, "not_found");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "failed");
    }
    const halite_entity_t *e = registry_find(id);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    if (e) {
        cJSON_AddItemToObject(o, "entity", hub_json_entity(e));
    }
    return send_json(req, o);
}

static esp_err_t permit_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", registry_permit_join());
    return send_json(req, o);
}

static esp_err_t permit_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char body[96];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    body[n < 0 ? 0 : n] = '\0';
    cJSON *j = cJSON_Parse(body);
    const cJSON *en = j ? cJSON_GetObjectItem(j, "enabled") : NULL;
    bool enabled = cJSON_IsTrue(en);
    int seconds = 0;
    const cJSON *sec = j ? cJSON_GetObjectItem(j, "seconds") : NULL;
    if (cJSON_IsNumber(sec)) {
        seconds = sec->valueint;
    }
    cJSON_Delete(j);
    esp_err_t err = ipc_link_permit_join(enabled, (uint8_t)seconds);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(o, "enabled", enabled);
    return send_json(req, o);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, UI_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static void ws_add(int fd)
{
    for (int i = 0; i < MAX_WS; i++) {
        if (s_ws_fds[i] == 0) {
            s_ws_fds[i] = fd;
            return;
        }
    }
}

static char s_ws_payload[1536];

static void ws_send_all(const char *msg)
{
    if (!s_server || !msg) {
        return;
    }
    strncpy(s_ws_payload, msg, sizeof(s_ws_payload) - 1);
    s_ws_payload[sizeof(s_ws_payload) - 1] = '\0';
    httpd_ws_frame_t fr = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s_ws_payload,
        .len = strlen(s_ws_payload),
    };
    for (int i = 0; i < MAX_WS; i++) {
        if (s_ws_fds[i]) {
            if (httpd_ws_send_frame_async(s_server, s_ws_fds[i], &fr) != ESP_OK) {
                s_ws_fds[i] = 0;
            }
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ws_add(httpd_req_to_sockfd(req));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "hello");
        cJSON_AddItemToObject(o, "entities", hub_json_entities());
        char *s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (s) {
            httpd_ws_frame_t fr = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)s, .len = strlen(s)};
            httpd_ws_send_frame(req, &fr);
            free(s);
        }
        return ESP_OK;
    }
    httpd_ws_frame_t fr = {0};
    httpd_ws_recv_frame(req, &fr, 0);
    return ESP_OK;
}

void http_api_broadcast_entity(const halite_entity_t *e)
{
    (void)e;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "state");
    if (e && e->in_use) {
        cJSON_AddItemToObject(o, "entity", hub_json_entity(e));
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        ws_send_all(s);
        free(s);
    }
}

esp_err_t http_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/health", .method = HTTP_GET, .handler = health_get},
        {.uri = "/api/entities", .method = HTTP_GET, .handler = entities_get},
        {.uri = "/api/entities/*", .method = HTTP_GET, .handler = entity_get},
        {.uri = "/api/entities/*/command", .method = HTTP_POST, .handler = entity_cmd},
        {.uri = "/api/zigbee/permit_join", .method = HTTP_GET, .handler = permit_get},
        {.uri = "/api/zigbee/permit_join", .method = HTTP_POST, .handler = permit_post},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
        {.uri = "/*", .method = HTTP_OPTIONS, .handler = cors_options},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    ESP_LOGI(TAG, "HTTP UI on port %d", cfg.server_port);
    return ESP_OK;
}
