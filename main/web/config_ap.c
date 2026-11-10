#include "config_ap.h"
#include <stdio.h>
#include "config_runtime.h"
#include "power_manager.h"
#include "wearable_protocol.h"
#include "wifi_link.h"
#include "wifi_network.h"
#include "esp_http_server.h"
#include "sleep_manager.h"
#include "playmaker_discovery.h"

#define ASSET(name) \
    extern const unsigned char name##_start[] asm("_binary_" #name "_start"); \
    extern const unsigned char name##_end[] asm("_binary_" #name "_end")
ASSET(config_ap_page_html);
ASSET(config_ap_app_js);
ASSET(index_html);
ASSET(app_js);
ASSET(brand_css);

static esp_err_t asset(httpd_req_t *req, const char *type,
                       const unsigned char *start, const unsigned char *end)
{
    size_t len = end - start;
    while (len && !start[len - 1]) len--;
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    sleep_manager_note_button_activity();
    return httpd_resp_send(req, (const char *)start, len);
}
static esp_err_t root(httpd_req_t *req) { return asset(req, "text/html; charset=utf-8", index_html_start, index_html_end); }
static esp_err_t settings(httpd_req_t *req) { return asset(req, "text/html; charset=utf-8", config_ap_page_html_start, config_ap_page_html_end); }
static esp_err_t script(httpd_req_t *req) { return asset(req, "application/javascript; charset=utf-8", app_js_start, app_js_end); }
static esp_err_t settings_script(httpd_req_t *req) { return asset(req, "application/javascript; charset=utf-8", config_ap_app_js_start, config_ap_app_js_end); }
static esp_err_t css(httpd_req_t *req) { return asset(req, "text/css; charset=utf-8", brand_css_start, brand_css_end); }

static esp_err_t config_get(httpd_req_t *req)
{
    wearable_config_t cfg;
    if (!config_runtime_snapshot(&cfg)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configurazione non disponibile");
    uint8_t bytes[WEARABLE_CONFIG_MAX_WIRE_SIZE];
    size_t size = wearable_encode_config(&cfg, bytes, sizeof(bytes));
    if (!size) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Codifica fallita");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (char *)bytes, size);
}

static esp_err_t config_post(httpd_req_t *req)
{
    uint8_t command[32];
    if (req->content_len <= 0 || req->content_len > sizeof(command)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Comando non valido");
    unsigned offset = 0;
    while (offset < req->content_len) {
        int n = httpd_req_recv(req, (char *)command + offset, req->content_len - offset);
        if (n <= 0) return ESP_FAIL;
        offset += n;
    }
    esp_err_t err = config_runtime_apply_command(command, offset);
    if (err == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Configurazione non valida: mantieni almeno una gesture di configurazione");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Salvataggio fallito");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char json[160];
    /* "push": this firmware sends commands by itself (UDP, docs/wifi.md); a controller
     * that understands it must not take the HTTP session, or the push link waits 10 s. */
    snprintf(json, sizeof(json), "{\"ssid\":\"Playmaker Wi-Fi\",\"controllerConnected\":%s,\"batteryMv\":%u,\"protocol\":%u,\"push\":%u}",
             wifi_link_is_connected() ? "true" : "false", power_manager_battery_mv(), WEARABLE_PROTOCOL_VERSION,
             WIFI_LINK_PUSH_VERSION);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t config_ap_start(void)
{
    const wifi_network_options_t network = {
        .ap_prefix = "PLAYMAKER_", .ap_ip = "192.168.4.1", .station_enabled = true,
        .on_activity = sleep_manager_note_button_activity,
    };
    esp_err_t err = wifi_network_start(&network);
    if (err != ESP_OK) return err;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    httpd_handle_t server;
    err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=root},
        {.uri="/settings", .method=HTTP_GET, .handler=settings},
        {.uri="/app.js", .method=HTTP_GET, .handler=script},
        {.uri="/settings.js", .method=HTTP_GET, .handler=settings_script},
        {.uri="/brand.css", .method=HTTP_GET, .handler=css},
        {.uri="/api/config", .method=HTTP_GET, .handler=config_get},
        {.uri="/api/config", .method=HTTP_POST, .handler=config_post},
        {.uri="/api/status", .method=HTTP_GET, .handler=status_get},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    err = wifi_network_register(server);
    if (err != ESP_OK) return err;
    err = wifi_link_register(server);
    if (err != ESP_OK) return err;
    err = playmaker_discovery_start();
    if (err != ESP_OK) return err;
    return udp_link_start();
}
