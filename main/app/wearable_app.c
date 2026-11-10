#include "wearable_app.h"

#include "wifi_link.h"
#include "wifi_network.h"
#include "board_io.h"
#include "button_manager.h"
#include "config_store.h"
#include "config_runtime.h"
#include "config_ap.h"
#include "gesture_engine.h"
#include "power_manager.h"
#include "sound_manager.h"
#include "sleep_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wearable_app";

static wearable_config_t s_config;
static wearable_match_state_t s_match_state;

static void on_config_changed(const wearable_config_t *config, void *ctx)
{
    (void)ctx;

    if (config == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "runtime config updated mappings=%u",
             (unsigned)config->mapping_count);
    gesture_engine_update_config(config);
    button_manager_update_config(config);
    sound_manager_update_config(config);
}

static void on_ack(wearable_action_t action,
                   const wearable_ack_packet_t *ack,
                   void *ctx)
{
    (void)ctx;

    if (ack->status != WEARABLE_ACK_OK) {
        ESP_LOGW(TAG,
                 "action rejected seq=%lu status=%u",
                 (unsigned long)ack->sequence,
                 (unsigned)ack->status);
        sound_manager_play_system(WEARABLE_SYS_ERROR);
        return;
    }

    s_match_state = ack->state;

    ESP_LOGD(TAG,
             "state rev=%u points=%u-%u games=%u-%u sets=%u-%u flags=0x%02x",
             (unsigned)s_match_state.revision,
             (unsigned)s_match_state.points_a,
             (unsigned)s_match_state.points_b,
             (unsigned)s_match_state.games_a,
             (unsigned)s_match_state.games_b,
             (unsigned)s_match_state.sets_a,
             (unsigned)s_match_state.sets_b,
             (unsigned)ack->transition_flags);

    /*
     * A single queued audio pattern guarantees ordering: confirmation first,
     * then GAME / SET / MATCH. The score transition always comes from the
     * authoritative ACK, never from local padel rules.
     */
    sound_manager_play_ack_feedback(action,
                                    ack->transition_flags);
}

static void start_pairing(void)
{
    sleep_manager_note_button_activity();
    sound_manager_play_system(WEARABLE_SYS_PAIRING_STARTED);
    ESP_LOGI(TAG, "Wi-Fi setup: connect to PLAYMAKER_xxxx and open http://192.168.4.1/settings");
}

static void on_action(wearable_action_t action,
                      void *ctx)
{
    (void)ctx;

    ESP_LOGD(TAG, "gesture action callback=%u", (unsigned)action);

    if (action == WEARABLE_ACTION_ENTER_PAIRING) {
        start_pairing();
        return;
    }

    if (!wifi_link_send_action(action)) {
        sound_manager_play_system(
            WEARABLE_SYS_ERROR);
    }
}

static void on_primitive(wearable_token_t token, void *ctx)
{
    (void)ctx;
    ESP_LOGD(TAG,
             "primitive token=0x%04x mask=0x%02x type=%u",
             (unsigned)token,
             (unsigned)wearable_token_button_mask(token),
             (unsigned)wearable_token_primitive(token));

    /*
     * Pairing must stay recoverable even if a saved configuration remaps or
     * removes the gesture table entry. A long press on the physical UNDO
     * button is therefore a direct, configuration-independent reset.
     */
    if (wearable_token_button_mask(token) ==
            (uint8_t)(1u << WEARABLE_BUTTON_UNDO) &&
        wearable_token_primitive(token) == WEARABLE_PRIMITIVE_LONG) {
        start_pairing();
        return;
    }

    wifi_network_kick();
    gesture_engine_feed(token);
}

static void on_action_failed(wearable_action_t action, uint32_t sequence, void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "Wi-Fi action=%u seq=%lu expired without ACK; press again to retry",
             (unsigned)action, (unsigned long)sequence);
    sound_manager_play_system(WEARABLE_SYS_ERROR);
}

static void on_power(power_event_t event, void *ctx)
{
    (void)ctx;

    if (event == POWER_EVENT_LOW_BATTERY) {
        sound_manager_play_system(
            WEARABLE_SYS_LOW_BATTERY);
    }
}

static void gesture_tick_task(void *arg)
{
    (void)arg;

    for (;;) {
        gesture_engine_tick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t wearable_app_start(void)
{
    esp_err_t err = board_io_init();
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Start inactivity tracking immediately after board IO is restored. On a
     * Deep-sleep wake this also prints the wake banner before the rest of the
     * application starts.
     */
    err = sleep_manager_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Deep-sleep manager unavailable: %s",
                 esp_err_to_name(err));
    }

    err = config_store_init();
    if (err != ESP_OK) {
        return err;
    }

    if (!config_store_load(&s_config)) {
        if (!config_store_save(&s_config)) {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "default configuration created");
    }

    err = config_runtime_init(&s_config,
                              on_config_changed,
                              NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = sound_manager_start(&s_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gesture_engine_init(&s_config,
                              on_action,
                              NULL);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Power monitoring is useful but not required for scoring. If the ADC
     * peripheral cannot start, keep the wearable operational and surface the
     * failure in logs instead of reboot-looping the product.
     */
    err = power_manager_start(on_power, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "power manager unavailable: %s",
                 esp_err_to_name(err));
    }

    /*
     * Bring the Wi-Fi action queue up before exposing physical input. This
     * prevents a button held during boot from reaching an uninitialized link
     * manager.
     */
    err = wifi_link_start(on_ack, on_action_failed, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* Serve setup and scoring on the AP and on the router LAN address. */
    err = config_ap_start();
    if (err != ESP_OK) {
        return err;
    }

    err = button_manager_start(&s_config,
                               on_primitive,
                               NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(gesture_tick_task,
                    "gesture_tick",
                    2048,
                    NULL,
                    5,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
 
    ESP_LOGI(TAG, "wearable started");
    return ESP_OK;
}
