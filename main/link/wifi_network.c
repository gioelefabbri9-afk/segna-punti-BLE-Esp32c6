#include "wifi_network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct { char ssid[33]; char password[65]; } credentials_t;
static const credentials_t FALLBACK = {"Se", "ciao1234"};
static credentials_t s_saved, s_attempt;
static QueueHandle_t s_requests;
static SemaphoreHandle_t s_radio;
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_name[24], s_id[13], s_ip[16], s_ssid[33], s_message[120] = "Avvio Wi-Fi";
static bool s_connected;
static wifi_network_options_t s_options;
#define CONNECTED BIT0
#define DISCONNECTED BIT1

static void message(const char *text)
{
    portENTER_CRITICAL(&s_state_lock);
    strlcpy(s_message, text, sizeof(s_message));
    portEXIT_CRITICAL(&s_state_lock);
}

static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got->ip_info.ip));
        portENTER_CRITICAL(&s_state_lock);
        s_connected = true;
        strlcpy(s_ip, ip, sizeof(s_ip));
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI("wifi_network", "LAN ready: http://%s/", ip);
        xEventGroupSetBits(s_events, CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_state_lock);
        s_connected = false;
        s_ip[0] = 0;
        portEXIT_CRITICAL(&s_state_lock);
        xEventGroupSetBits(s_events, DISCONNECTED);
    }
}

/* Quote bytes from untrusted SSIDs before including them in JSON. */
static void quote(const char *src, char *out, size_t capacity)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 7 < capacity; p++) {
        if (*p == '"' || *p == '\\' || *p < 32) {
            n += snprintf(out + n, capacity - n, "\\u%04x", *p);
        } else out[n++] = (char)*p;
    }
    out[n] = 0;
}

static bool connect_to(const credentials_t *credentials)
{
    xSemaphoreTake(s_radio, portMAX_DELAY);
    esp_wifi_disconnect();
    /* Let the previous disconnect event drain before waiting for this attempt. */
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(s_events, CONNECTED | DISCONNECTED);
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(cfg.sta.password, credentials->password, strlen(credentials->password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.failure_retry_cnt = 2;
    portENTER_CRITICAL(&s_state_lock);
    strlcpy(s_ssid, credentials->ssid, sizeof(s_ssid));
    s_connected = false;
    s_ip[0] = 0;
    portEXIT_CRITICAL(&s_state_lock);
    message("Connessione alla rete in corso");
    bool ok = esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK && esp_wifi_connect() == ESP_OK;
    if (ok) {
        EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED | DISCONNECTED,
                                              pdFALSE, pdFALSE, pdMS_TO_TICKS(18000));
        ok = (bits & CONNECTED) != 0;
    }
    if (!ok) esp_wifi_disconnect();
    xSemaphoreGive(s_radio);
    return ok;
}

static bool save(const credentials_t *credentials)
{
    nvs_handle_t h;
    if (nvs_open("wifi_lan", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "last_network", credentials, sizeof(*credentials));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

/*
 * One radio serves both the AP and the station. Every background attempt to
 * find a LAN scans all channels, and while it does the AP is off its own
 * channel: a controller that joined the AP directly sees seconds of silence and
 * button commands expire. Measured with no known LAN in range: about 20 s of
 * dropouts (up to 3.3 s each) in every 50 s cycle, 14 of 150 presses lost.
 * So background retries wait while a client is associated to the AP. A network
 * submitted from the settings page is still tried at once.
 */
#define AP_FIRST_MS 5000
/* Survives deep sleep: was the AP in use the last time we were awake? */
static RTC_DATA_ATTR bool s_ap_was_used;

/* Seconds between LAN searches while it is down: at once, then 2, 4, 8, 16 and
 * every 16 from there. A button press restarts the ladder. */
static const uint16_t BACKOFF_MS[] = {0, 2000, 4000, 8000, 16000};
#define BACKOFF_STEPS (sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]))
#define AP_RECHECK_MS 2000
static volatile bool s_kick;

void wifi_network_kick(void)
{
    s_kick = true;
}

static bool lan_up(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool up = s_connected;
    portEXIT_CRITICAL(&s_state_lock);
    return up;
}

static bool ap_in_use(void)
{
    wifi_sta_list_t clients;
    return esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0;
}

static void network_task(void *arg)
{
    (void)arg;
    bool have_request = false;
    unsigned step = 0;
    /*
     * A button woke us and last time the controller was on the AP: let it join
     * before the first LAN search, otherwise the wake-up press meets a deaf AP.
     * Devices used over a LAN (flag clear) start the search at once, as before.
     */
    for (unsigned waited = 0; s_ap_was_used && waited < AP_FIRST_MS && !ap_in_use(); waited += 250) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    for (;;) {
        bool connected;
        portENTER_CRITICAL(&s_state_lock);
        connected = s_connected;
        portEXIT_CRITICAL(&s_state_lock);
        if (have_request) {
            if (connect_to(&s_attempt)) {
                if (save(&s_attempt)) {
                    s_saved = s_attempt;
                    message("Connesso. Rete salvata per i prossimi avvii.");
                } else message("Connesso, ma salvataggio fallito. Riprova a salvare la rete.");
                connected = true;
            } else {
                message("Connessione fallita: verifica password e copertura. Ripristino rete precedente.");
                connected = false;
            }
        }
        s_ap_was_used = ap_in_use();
        if (!connected && !s_ap_was_used) {
            connected = s_saved.ssid[0] && connect_to(&s_saved);
            if (!connected) connected = connect_to(&FALLBACK);
            message(connected ? "Connesso alla LAN" : "Rete non disponibile. Configura il Wi-Fi dall'AP e riprova.");
            if (step < BACKOFF_STEPS - 1) step++;
        }
        if (connected) step = 0;
        /*
         * Connected: nothing to retry, just notice the drop and search again at
         * once. Down: wait the next rung of the ladder (or, while the AP is in
         * use, a short re-check). A submitted network always wakes us at once.
         */
        const unsigned wait_ms = !connected && s_ap_was_used ? AP_RECHECK_MS : BACKOFF_MS[step];
        have_request = false;
        s_kick = false;
        for (unsigned waited = 0; connected || waited < wait_ms; waited += 250) {
            if (xQueueReceive(s_requests, &s_attempt, pdMS_TO_TICKS(250)) == pdTRUE) { have_request = true; break; }
            if (connected ? !lan_up() : s_kick) { step = 0; break; }
        }
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    char ip[16], ssid[33], msg[120]; bool connected;
    portENTER_CRITICAL(&s_state_lock);
    strlcpy(ip, s_ip, sizeof(ip));
    strlcpy(ssid, s_ssid, sizeof(ssid));
    strlcpy(msg, s_message, sizeof(msg));
    connected = s_connected;
    portEXIT_CRITICAL(&s_state_lock);
    char escaped[200], escaped_message[750], json[1200];
    quote(ssid, escaped, sizeof(escaped));
    quote(msg, escaped_message, sizeof(escaped_message));
    snprintf(json, sizeof(json), "{\"name\":\"%s\",\"device_id\":\"%s\",\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"message\":\"%s\",\"fallback\":\"Se\"}",
             s_name, s_id, connected ? "true" : "false", escaped, ip, escaped_message);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (s_options.on_activity) s_options.on_activity();
    return httpd_resp_sendstr(req, json);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (xSemaphoreTake(s_radio, 0) != pdTRUE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Connessione in corso. Riprova tra pochi secondi.");
    }
    wifi_scan_config_t cfg = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    wifi_ap_record_t *records = calloc(24, sizeof(*records));
    uint16_t count = 24;
    if (err == ESP_OK && records) err = esp_wifi_scan_get_ap_records(&count, records);
    else { esp_wifi_clear_ap_list(); count = 0; }
    xSemaphoreGive(s_radio);
    if (err != ESP_OK || !records) {
        free(records);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scansione non riuscita");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (unsigned i = 0; i < count; i++) {
        if (!records[i].ssid[0]) continue;
        char ssid[33], escaped[200], row[300];
        memcpy(ssid, records[i].ssid, 32); ssid[32] = 0;
        quote(ssid, escaped, sizeof(escaped));
        snprintf(row, sizeof(row), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secured\":%s}",
                 first ? "" : ",", escaped, records[i].rssi,
                 records[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        httpd_resp_sendstr_chunk(req, row);
        first = false;
    }
    free(records);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t network_post(httpd_req_t *req)
{
    if (!s_options.station_enabled) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Seleziona prima la modalita LAN");
    }
    /* Fixed binary payload: 33-byte NUL-terminated SSID + 65-byte password. */
    credentials_t candidate;
    if (req->content_len != sizeof(candidate)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rete non valida");
    unsigned offset = 0;
    while (offset < sizeof(candidate)) {
        int n = httpd_req_recv(req, (char *)&candidate + offset, sizeof(candidate) - offset);
        if (n <= 0) return ESP_FAIL;
        offset += n;
    }
    if (!memchr(candidate.ssid, 0, sizeof(candidate.ssid)) ||
        !memchr(candidate.password, 0, sizeof(candidate.password)) || !candidate.ssid[0] ||
        (strlen(candidate.password) > 0 && strlen(candidate.password) < 8)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID massimo 32 byte; password almeno 8 caratteri, vuota per reti aperte");
    }
    if (xQueueSend(s_requests, &candidate, 0) != pdTRUE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Un'altra richiesta e in corso");
    }
    message("Nuova rete in attesa di connessione");
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "Connessione avviata. La rete verra salvata solo dopo il collegamento.");
}

void wifi_network_status(wifi_network_status_t *out)
{
    portENTER_CRITICAL(&s_state_lock);
    strlcpy(out->name, s_name, sizeof(out->name));
    strlcpy(out->ip, s_ip, sizeof(out->ip));
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    out->connected = s_connected;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t wifi_network_start(const wifi_network_options_t *options)
{
    if (!options || !options->ap_prefix || !options->ap_ip) return ESP_ERR_INVALID_ARG;
    s_options = *options;
    s_requests = xQueueCreate(1, sizeof(credentials_t));
    s_radio = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_requests || !s_radio || !s_events) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    if (nvs_open("wifi_lan", NVS_READONLY, &h) == ESP_OK) {
        size_t size = sizeof(s_saved);
        if (nvs_get_blob(h, "last_network", &s_saved, &size) != ESP_OK ||
            size != sizeof(s_saved) || s_saved.ssid[32] || s_saved.password[64]) memset(&s_saved, 0, sizeof(s_saved));
        nvs_close(h);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_FAIL;
    esp_netif_ip_info_t ap_ip = {0};
    if (esp_netif_str_to_ip4(options->ap_ip, &ap_ip.ip) != ESP_OK) return ESP_ERR_INVALID_ARG;
    ap_ip.gw = ap_ip.ip;
    esp_netif_str_to_ip4("255.255.255.0", &ap_ip.netmask);
    esp_err_t dhcp = esp_netif_dhcps_stop(ap_netif);
    if (dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return dhcp;
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    s_sta = esp_netif_create_default_wifi_sta();
    if (!s_sta) return ESP_FAIL;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(s_name, sizeof(s_name), "%s%02X%02X", options->ap_prefix, mac[4], mac[5]);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, NULL));
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_name, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, "playmaker", sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(options->station_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI("wifi_network", "Setup AP %s, URL http://%s/", s_name, options->ap_ip);
    if (!options->station_enabled) { message("Modalita diretta: collega il wearable alla rete del ricevitore"); return ESP_OK; }
    if (xTaskCreate(network_task, "wifi_network", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t wifi_network_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri="/api/network", .method=HTTP_GET, .handler=status_get},
        {.uri="/api/networks", .method=HTTP_GET, .handler=scan_get},
        {.uri="/api/network", .method=HTTP_POST, .handler=network_post},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
