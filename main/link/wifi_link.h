#pragma once

#include "esp_http_server.h"
#include "wearable_protocol.h"

typedef void (*wifi_ack_cb_t)(wearable_action_t action,
                              const wearable_ack_packet_t *ack, void *ctx);
typedef void (*wifi_action_failed_cb_t)(wearable_action_t action, uint32_t sequence, void *ctx);
esp_err_t wifi_link_start(wifi_ack_cb_t cb, wifi_action_failed_cb_t failed_cb, void *ctx);
esp_err_t wifi_link_register(httpd_handle_t server);
bool wifi_link_send_action(wearable_action_t action);
bool wifi_link_is_connected(void);

/*
 * Push transport (UDP, docs/wifi.md): the wearable sends each command as soon as
 * it is pressed and repeats it until the controller answers. The HTTP routes
 * above stay for the browser page and the USB receiver; one controller at a
 * time holds the wearable, whichever transport it uses.
 */
#define WIFI_LINK_PUSH_PORT 37021
#define WIFI_LINK_PUSH_VERSION 1u
#define WIFI_LINK_PUSH_HEADER 20u
#define WIFI_LINK_PUSH_MAX (WIFI_LINK_PUSH_HEADER + WEARABLE_ACTION_PACKET_SIZE)
/* The datagram to send now (0 = nothing). to_everyone: no push controller yet, broadcast it. */
size_t wifi_link_push_output(uint8_t *out, size_t capacity, const uint8_t device[6],
                             uint16_t battery_mv, bool *to_everyone);
/* A datagram arrived. true = its sender is the controller (remember its address). */
bool wifi_link_push_input(const uint8_t *in, size_t len, bool same_peer);
esp_err_t udp_link_start(void);
