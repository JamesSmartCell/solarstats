#include "setup_ap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_io.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <unistd.h>
#include "nvs_creds.h"
#include "sdkconfig.h"

static const char *TAG = "setup_ap";

#define SETUP_AP_IP "192.168.4.1"
#define DNS_PORT    53

static const char s_captive_uri[] = "http://" SETUP_AP_IP;

static volatile bool s_dns_run;
static httpd_handle_t s_httpd;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
    size_t di = 0;
    for (size_t i = 0; i < src_len && di + 1 < dst_sz; ++i) {
        if (src[i] == '+' ) {
            dst[di++] = ' ';
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_nibble(src[i + 1]);
            int lo = hex_nibble(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            dst[di++] = src[i];
        }
    }
    dst[di] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if ((p == body || p[-1] == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            size_t n = end ? (size_t)(end - val) : strlen(val);
            url_decode(out, out_sz, val, n);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    if (out_sz) {
        out[0] = '\0';
    }
    return false;
}

static void html_escape(char *dst, size_t dst_sz, const char *src)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dst_sz; ++i) {
        const char *rep = NULL;
        switch (src[i]) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        default:
            dst[di++] = src[i];
            continue;
        }
        size_t rl = strlen(rep);
        if (di + rl >= dst_sz) {
            break;
        }
        memcpy(dst + di, rep, rl);
        di += rl;
    }
    dst[di] = '\0';
}

static const char *PAGE_FMT =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Zigbee gateway setup</title>"
    "<style>body{font-family:sans-serif;max-width:22rem;margin:1.5rem auto;padding:0 1rem}"
    "h1{font-size:1.2rem}label{display:block;margin:.8rem 0 .2rem}"
    "input{width:100%;box-sizing:border-box;padding:.45rem;font-size:1rem}"
    "label.check{display:flex;align-items:flex-start;gap:.55rem;margin:1.1rem 0 .2rem}"
    "label.check input{width:auto;margin-top:.2rem;flex:0 0 auto}"
    "button{margin-top:1.2rem;width:100%;padding:.7rem;font-size:1rem}"
    "p{color:#444;font-size:.9rem}</style></head><body>"
    "<h1>Zigbee gateway setup</h1>"
    "<p>Wi-Fi and MQTT only. Port 1883 and Zigbee settings stay as flashed.</p>"
    "<form method=post action=/save>"
    "<label>Wi-Fi name (SSID)</label><input name=ssid required value=\"%s\">"
    "<label>Wi-Fi password</label><input name=wpass type=password value=\"%s\">"
    "<label>MQTT host (IP or name)</label><input name=mhost required value=\"%s\">"
    "<label>MQTT username</label><input name=muser value=\"%s\">"
    "<label>MQTT password</label><input name=mpass type=password value=\"%s\">"
    "<label class=check><input type=checkbox name=diag value=1%s>"
    "<span>Allow anonymous diagnostics. Sends failures and an hourly health ping to homesolar.percolate.one. No Wi-Fi or MQTT passwords are included.</span></label>"
    "<button type=submit>Save and reboot</button></form>"
    "<p>If this page did not open itself, go to http://" SETUP_AP_IP "</p>"
    "</body></html>";

static esp_err_t send_form(httpd_req_t *req)
{
    zbgw_creds_t creds;
    if (nvs_creds_get(&creds) != ESP_OK || !creds.wifi_ssid[0]) {
        nvs_creds_kconfig_defaults(&creds);
        if (strcmp(creds.wifi_ssid, "YOUR_WIFI_SSID") == 0) {
            creds.wifi_ssid[0] = '\0';
        }
        if (strcmp(creds.wifi_pass, "YOUR_WIFI_PASSWORD") == 0) {
            creds.wifi_pass[0] = '\0';
        }
    }

    char ssid[96], wpass[160], host[160], user[96], pass[160];
    html_escape(ssid, sizeof(ssid), creds.wifi_ssid);
    html_escape(wpass, sizeof(wpass), creds.wifi_pass);
    html_escape(host, sizeof(host), creds.mqtt_host);
    html_escape(user, sizeof(user), creds.mqtt_user);
    html_escape(pass, sizeof(pass), creds.mqtt_pass);
    const char *diag_checked = creds.diag_opt_in ? " checked" : "";

    char *page = malloc(4096);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    snprintf(page, 4096, PAGE_FMT, ssid, wpass, host, user, pass, diag_checked);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Link", "<http://" SETUP_AP_IP "/>; rel=\"captive-portal\"");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

static esp_err_t get_root(httpd_req_t *req)
{
    return send_form(req);
}

/* Android / iOS / Windows probe these. Return the form (200 HTML), not 204,
 * so the OS treats us as a captive portal and opens the sheet. */
static esp_err_t get_captive_probe(httpd_req_t *req)
{
    return send_form(req);
}

/* RFC 8908 — Android 11+ / some desktops prefer this over generate_204. */
static esp_err_t get_captive_api(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/captive+json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Link", "<http://" SETUP_AP_IP "/>; rel=\"captive-portal\"");
    httpd_resp_sendstr(req, "{\"captive\":true,\"user-portal-url\":\"http://" SETUP_AP_IP "/\"}");
    return ESP_OK;
}

/* Unknown paths: 303 to / with a body. iOS needs content, not a bare redirect. */
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void dhcp_set_captiveportal_url(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!netif) {
        ESP_LOGW(TAG, "No WIFI_AP_DEF netif for DHCP captive portal");
        return;
    }
    (void)esp_netif_dhcps_stop(netif);
    esp_err_t err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           (void *)s_captive_uri, strlen(s_captive_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP option 114 failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DHCP captive portal URI %s", s_captive_uri);
    }
    (void)esp_netif_dhcps_start(netif);
}

static void restart_later(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t post_save(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 768) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    char *body = calloc(1, (size_t)len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        return ESP_FAIL;
    }
    body[got] = '\0';

    zbgw_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    form_get(body, "ssid", creds.wifi_ssid, sizeof(creds.wifi_ssid));
    form_get(body, "wpass", creds.wifi_pass, sizeof(creds.wifi_pass));
    form_get(body, "mhost", creds.mqtt_host, sizeof(creds.mqtt_host));
    form_get(body, "muser", creds.mqtt_user, sizeof(creds.mqtt_user));
    form_get(body, "mpass", creds.mqtt_pass, sizeof(creds.mqtt_pass));
    char diag[8] = {0};
    creds.diag_opt_in = form_get(body, "diag", diag, sizeof(diag));
    free(body);

    if (!creds.wifi_ssid[0] || !creds.mqtt_host[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID and MQTT host are required");
        return ESP_FAIL;
    }

    esp_err_t err = nvs_creds_set(&creds);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
                    "<!DOCTYPE html><html><body><p>Saved. Rebooting onto your Wi-Fi…</p></body></html>",
                    HTTPD_RESP_USE_STRLEN);
    if (xTaskCreate(restart_later, "setup_rst", 2048, NULL, 5, NULL) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    return ESP_OK;
}

/* Reply to any DNS A query with 192.168.4.1 so phones open the captive page. */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[256];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 12) {
            continue;
        }

        uint8_t tx[512];
        if ((size_t)n + 16 > sizeof(tx)) {
            continue;
        }
        memcpy(tx, rx, (size_t)n);
        tx[2] = 0x81; /* response, recursion available */
        tx[3] = 0x80;
        int qend = 12;
        while (qend < n && rx[qend] != 0) {
            qend += 1 + rx[qend];
        }
        qend += 5; /* null + type + class */
        if (qend > n || qend < 16) {
            continue;
        }
        uint16_t qtype = ((uint16_t)rx[qend - 4] << 8) | rx[qend - 3];
        /* AAAA / HTTPS / other: NOERROR + no answer so the phone falls back to A. */
        if (qtype != 1) {
            tx[6] = 0;
            tx[7] = 0;
            (void)sendto(sock, tx, (size_t)n, 0, (struct sockaddr *)&from, fromlen);
            continue;
        }
        tx[6] = 0;
        tx[7] = 1;
        int o = n;
        tx[o++] = 0xc0;
        tx[o++] = 0x0c;
        tx[o++] = 0x00;
        tx[o++] = 0x01; /* A */
        tx[o++] = 0x00;
        tx[o++] = 0x01; /* IN */
        tx[o++] = 0x00;
        tx[o++] = 0x00;
        tx[o++] = 0x00;
        tx[o++] = 0x1e; /* TTL 30s */
        tx[o++] = 0x00;
        tx[o++] = 0x04;
        tx[o++] = 192;
        tx[o++] = 168;
        tx[o++] = 4;
        tx[o++] = 1;
        (void)sendto(sock, tx, (size_t)o, 0, (struct sockaddr *)&from, fromlen);
    }
    close(sock);
    vTaskDelete(NULL);
}

static void blink_task(void *arg)
{
    (void)arg;
    while (true) {
        board_io_set_led(true);
        vTaskDelay(pdMS_TO_TICKS(250));
        board_io_set_led(false);
        vTaskDelay(pdMS_TO_TICKS(750));
    }
}

void setup_ap_run(void)
{
    ESP_LOGW(TAG, "Starting setup AP \"%s\" — connect and open http://%s", CONFIG_ZBGW_SETUP_AP_SSID, SETUP_AP_IP);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", CONFIG_ZBGW_SETUP_AP_SSID);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* After start: stay on 11b/g/n so older phones still run captive detection. */
    (void)esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    dhcp_set_captiveportal_url();

    s_dns_run = true;
    (void)xTaskCreate(dns_task, "setup_dns", 3072, NULL, 3, NULL);
    (void)xTaskCreate(blink_task, "setup_led", 2048, NULL, 2, NULL);

    /* Captive probes hit many hosts/paths; keep httpd quiet. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_uri_handlers = 16;
    http.lru_purge_enable = true;
    http.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &http));

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = get_root};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = post_save};
    const httpd_uri_t rfc8908 = {.uri = "/.well-known/captive-portal", .method = HTTP_GET, .handler = get_captive_api};
    const httpd_uri_t cap[] = {
        {.uri = "/generate_204", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/canonical.html", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/success.txt", .method = HTTP_GET, .handler = get_captive_probe},
        {.uri = "/*", .method = HTTP_GET, .handler = get_captive_probe},
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &rfc8908));
    for (size_t i = 0; i < sizeof(cap) / sizeof(cap[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &cap[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler));

    ESP_LOGI(TAG, "Setup portal ready on http://%s (captive)", SETUP_AP_IP);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
