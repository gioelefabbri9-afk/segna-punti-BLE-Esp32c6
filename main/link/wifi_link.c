#include "wifi_link.h"
#include "action_retry.h"

#include <string.h>
#include <stdio.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define QUEUE_SIZE 16
#define LEASE_US (10LL * 1000000)
static SemaphoreHandle_t s_lock;
typedef struct {
    wearable_action_packet_t packet;
    action_retry_t retry;
} queued_action_t;
static queued_action_t s_queue[QUEUE_SIZE];
static unsigned s_head, s_count;
static uint32_t s_session, s_sequence;
static char s_owner[33];
static int64_t s_seen;
static wifi_ack_cb_t s_ack_cb;
static wifi_action_failed_cb_t s_failed_cb;
static void *s_ctx;
static wearable_ack_packet_t s_last_ack;
static bool s_have_ack;

/* Push transport: see wifi_link.h. */
#define PUSH_OWNER "udp"            /* never a valid HTTP client id (32 hex digits) */
#define PUSH_LEASE_US 3000000LL     /* three heartbeats without an answer */
#define PUSH_RESEND_US 100000LL
#define PUSH_BEAT_US 1000000LL
static int64_t s_push_next_us, s_beat_next_us;
/* The controller's token, echoed in every datagram sent straight to it: it proves the
 * sender really receives what the controller sends to this address (a forged source
 * address does not). Never broadcast. */
static uint8_t s_push_token[4];

static void pop_head(void)
{
    s_head = (s_head + 1) % QUEUE_SIZE;
    s_count--;
    s_push_next_us = 0;
    if (s_count) action_retry_start(&s_queue[s_head].retry, esp_timer_get_time());
}

static void expire_head(void)
{
    wearable_action_packet_t failed = {0};
    bool expired = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count && action_retry_expired(&s_queue[s_head].retry, esp_timer_get_time())) {
        failed = s_queue[s_head].packet;
        pop_head();
        expired = true;
    }
    xSemaphoreGive(s_lock);
    if (expired && s_failed_cb) s_failed_cb(failed.action, failed.sequence, s_ctx);
}

static void retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        expire_head();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool push_owner(void)
{
    return !strcmp(s_owner, PUSH_OWNER);
}

static bool active(void)
{
    return s_owner[0] && esp_timer_get_time() - s_seen < (push_owner() ? PUSH_LEASE_US : LEASE_US);
}

bool wifi_link_is_connected(void)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool connected = active();
    xSemaphoreGive(s_lock);
    return connected;
}

static bool client_id(httpd_req_t *req, char out[33])
{
    if (httpd_req_get_hdr_value_len(req, "X-Playmaker-Client") != 32 ||
        httpd_req_get_hdr_value_str(req, "X-Playmaker-Client", out, 33) != ESP_OK) return false;
    for (unsigned i = 0; i < 32; i++) {
        if (!((out[i] >= '0' && out[i] <= '9') || (out[i] >= 'a' && out[i] <= 'f'))) return false;
    }
    return true;
}

static bool authorize(httpd_req_t *req, bool claim)
{
    char id[33];
    if (!client_id(req, id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Client non valido");
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = !strcmp(id, s_owner) || (claim && !active());
    if (ok) {
        /* A new controller must never receive commands from an expired lease. */
        if (strcmp(id, s_owner)) { s_head = s_count = 0; }
        strlcpy(s_owner, id, sizeof(s_owner));
        s_seen = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    if (!ok) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Tabellone gia aperto su un altro dispositivo. Chiudilo e attendi 10 secondi.");
    }
    return ok;
}

static esp_err_t session_post(httpd_req_t *req)
{
    if (req->content_len != 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Corpo non previsto");
    if (!authorize(req, true)) return ESP_OK;
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t action_get(httpd_req_t *req)
{
    if (!authorize(req, false)) return ESP_OK;
    uint8_t packet[WEARABLE_ACTION_PACKET_SIZE];
    size_t size = 0;
    unsigned attempt = 0, remaining_ms = 0;
    char attempts[4], remaining[12];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count && action_retry_ready(&s_queue[s_head].retry, esp_timer_get_time())) {
        size = wearable_encode_action(&s_queue[s_head].packet, packet, sizeof(packet));
        if (size) {
            action_retry_sent(&s_queue[s_head].retry, esp_timer_get_time());
            attempt = s_queue[s_head].retry.attempts;
            int64_t remaining = s_queue[s_head].retry.deadline_us - esp_timer_get_time();
            remaining_ms = remaining > 0 ? (unsigned)(remaining / 1000) : 0;
        }
    }
    xSemaphoreGive(s_lock);
    if (size) {
        snprintf(attempts, sizeof(attempts), "%u", attempt);
        snprintf(remaining, sizeof(remaining), "%u", remaining_ms);
        httpd_resp_set_hdr(req, "X-Playmaker-Attempt", attempts);
        httpd_resp_set_hdr(req, "X-Playmaker-Remaining-Ms", remaining);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/octet-stream");
    if (!size) httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, (const char *)packet, size);
}

/* Shared by both transports. false = that command already expired. */
static bool take_ack(const wearable_ack_packet_t *ack)
{
    bool accepted = false, notify = false;
    wearable_action_t action = WEARABLE_ACTION_POINT_A;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count && !action_retry_expired(&s_queue[s_head].retry, esp_timer_get_time()) &&
        ack->session_id == s_queue[s_head].packet.session_id && ack->sequence == s_queue[s_head].packet.sequence) {
        accepted = true;
        if (ack->status != WEARABLE_ACK_TEMPORARY_ERROR) {
            action = s_queue[s_head].packet.action;
            pop_head();
            s_last_ack = *ack;
            s_have_ack = true;
            notify = true;
        }
    } else if (s_have_ack && ack->session_id == s_last_ack.session_id && ack->sequence == s_last_ack.sequence) {
        accepted = true;
    }
    xSemaphoreGive(s_lock);
    if (notify && s_ack_cb) s_ack_cb(action, ack, s_ctx);
    return accepted;
}

static esp_err_t ack_post(httpd_req_t *req)
{
    if (!authorize(req, false)) return ESP_OK;
    uint8_t bytes[WEARABLE_ACK_PACKET_SIZE];
    if (req->content_len != sizeof(bytes)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ACK invalido");
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        int n = httpd_req_recv(req, (char *)bytes + offset, sizeof(bytes) - offset);
        if (n <= 0) return ESP_FAIL;
        offset += n;
    }
    wearable_ack_packet_t ack;
    if (!wearable_decode_ack(bytes, sizeof(bytes), &ack)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ACK invalido");
    if (!take_ack(&ack)) {
        httpd_resp_set_status(req, "410 Gone");
        return httpd_resp_sendstr(req, "Comando scaduto: nessun ulteriore tentativo");
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t wifi_link_start(wifi_ack_cb_t cb, wifi_action_failed_cb_t failed_cb, void *ctx)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_session = esp_random();
    if (!s_session) s_session = 1;
    s_sequence = 1;
    s_ack_cb = cb;
    s_failed_cb = failed_cb;
    s_ctx = ctx;
    return xTaskCreate(retry_task, "action_retry", 2048, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool wifi_link_send_action(wearable_action_t action)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = active() && s_count < QUEUE_SIZE && action <= WEARABLE_ACTION_STOP;
    if (!active()) s_head = s_count = 0;
    if (ok) {
        s_queue[(s_head + s_count) % QUEUE_SIZE] = (queued_action_t){
            .packet = {.session_id = s_session, .sequence = s_sequence++, .action = action},
        };
        if (!s_count) {
            action_retry_start(&s_queue[s_head].retry, esp_timer_get_time());
            s_push_next_us = 0;
        }
        s_count++;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

esp_err_t wifi_link_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri="/api/session", .method=HTTP_POST, .handler=session_post},
        {.uri="/api/action", .method=HTTP_GET, .handler=action_get},
        {.uri="/api/ack", .method=HTTP_POST, .handler=ack_post},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16); out[3] = (uint8_t)(value >> 24);
}

size_t wifi_link_push_output(uint8_t *out, size_t capacity, const uint8_t device[6],
                             uint16_t battery_mv, bool *to_everyone)
{
    if (capacity < WIFI_LINK_PUSH_MAX) return 0;
    uint8_t type = 0;
    size_t size = WIFI_LINK_PUSH_HEADER;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const bool pushing = active() && push_owner();
    /* The head command goes out at once and again every 100 ms until its ACK
     * (or until it expires, three seconds after reaching the head). */
    if (pushing && s_count && !action_retry_expired(&s_queue[s_head].retry, now) && now >= s_push_next_us) {
        size_t n = wearable_encode_action(&s_queue[s_head].packet, out + WIFI_LINK_PUSH_HEADER,
                                          capacity - WIFI_LINK_PUSH_HEADER);
        if (n) {
            type = 'A';
            size += n;
            s_push_next_us = now + PUSH_RESEND_US;
        }
    }
    /* Otherwise one heartbeat a second: it finds the controller and keeps the lease. */
    if (!type && now >= s_beat_next_us) type = 'H';
    if (type) s_beat_next_us = now + PUSH_BEAT_US;
    const uint32_t session = s_session;
    /* Lets the controller learn that a command it confirmed had already expired here. */
    const uint32_t confirmed = s_have_ack && s_last_ack.session_id == s_session ? s_last_ack.sequence : 0;
    memset(out + 20, 0, 4);
    if (pushing) memcpy(out + 20, s_push_token, 4);
    xSemaphoreGive(s_lock);
    if (!type) return 0;
    out[0] = 'P'; out[1] = 'M'; out[2] = WIFI_LINK_PUSH_VERSION; out[3] = type;
    memcpy(out + 4, device, 6);
    put32(out + 10, session);
    put32(out + 14, confirmed);
    out[18] = (uint8_t)battery_mv; out[19] = (uint8_t)(battery_mv >> 8);
    *to_everyone = !pushing;
    return size;
}

bool wifi_link_push_input(const uint8_t *in, size_t len, bool same_peer)
{
    if (len < WIFI_LINK_PUSH_REPLY || in[0] != 'P' || in[1] != 'M' || in[2] != WIFI_LINK_PUSH_VERSION) return false;
    wearable_ack_packet_t ack;
    bool has_ack = false;
    if (in[3] == 'a') {
        if (!wearable_decode_ack(in + WIFI_LINK_PUSH_REPLY, len - WIFI_LINK_PUSH_REPLY, &ack)) return false;
        has_ack = true;
    } else if (in[3] != 'h' || len != WIFI_LINK_PUSH_REPLY) {
        return false;
    }
    bool ok;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (active()) {
        /* One controller at a time, whichever transport it uses. */
        ok = push_owner() && same_peer;
    } else {
        /* A new controller must never receive commands from an expired lease. */
        s_head = s_count = 0;
        strlcpy(s_owner, PUSH_OWNER, sizeof(s_owner));
        ok = true;
    }
    if (ok) {
        s_seen = esp_timer_get_time();
        memcpy(s_push_token, in + 4, 4);
    }
    xSemaphoreGive(s_lock);
    if (ok && has_ack) take_ack(&ack);
    return ok;
}
