/*
 * Push transport: the socket side. What to send and what an answer means is in
 * wifi_link.c (host-tested); here are only the socket and the addresses.
 *
 * The wearable starts the conversation, so the controller needs no address for
 * it: heartbeats go to the broadcast address of every network we are in (our AP
 * and the LAN) until a controller answers, then straight to that controller.
 */
#include "wifi_link.h"

#include <errno.h>
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"

#define RECV_WAIT_US 10000 /* how long a button press can wait for the loop */

static void send_everyone(int sock, const uint8_t *bytes, size_t size)
{
    static const char *const interfaces[] = {"WIFI_AP_DEF", "WIFI_STA_DEF"};
    for (unsigned i = 0; i < sizeof(interfaces) / sizeof(interfaces[0]); i++) {
        esp_netif_ip_info_t info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(interfaces[i]);
        if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || !info.ip.addr) continue;
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(WIFI_LINK_PUSH_PORT),
                                 .sin_addr.s_addr = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr};
        sendto(sock, bytes, size, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void udp_task(void *arg)
{
    (void)arg;
    uint8_t device[6];
    ESP_ERROR_CHECK(esp_read_mac(device, ESP_MAC_BT)); /* same id as /api/network */
    for (;;) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        const int yes = 1;
        const struct timeval wait = {.tv_usec = RECV_WAIT_US};
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        struct sockaddr_in peer = {0};
        for (;;) {
            uint8_t in[64], out[WIFI_LINK_PUSH_MAX];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(sock, in, sizeof(in), 0, (struct sockaddr *)&from, &from_len);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (n > 0 && wifi_link_push_input(in, (size_t)n, peer.sin_port &&
                                              from.sin_addr.s_addr == peer.sin_addr.s_addr &&
                                              from.sin_port == peer.sin_port)) {
                peer = from;
            }
            bool to_everyone = true;
            size_t size = wifi_link_push_output(out, sizeof(out), device, power_manager_battery_mv(), &to_everyone);
            if (!size) continue;
            if (to_everyone || !peer.sin_port) send_everyone(sock, out, size);
            else sendto(sock, out, size, 0, (struct sockaddr *)&peer, sizeof(peer));
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t udp_link_start(void)
{
    return xTaskCreate(udp_task, "udp_link", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
