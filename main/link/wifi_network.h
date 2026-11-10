#pragma once
#include "esp_http_server.h"
#include <stdbool.h>
typedef struct {
    const char *ap_prefix;
    const char *ap_ip;
    bool station_enabled;
    void (*on_activity)(void);
} wifi_network_options_t;
typedef struct {
    char name[24], ip[16], ssid[33];
    bool connected;
} wifi_network_status_t;
esp_err_t wifi_network_start(const wifi_network_options_t *options);
void wifi_network_status(wifi_network_status_t *out);
esp_err_t wifi_network_register(httpd_handle_t server);
/* A button was pressed: if the LAN is down, search it now instead of waiting. */
void wifi_network_kick(void);
