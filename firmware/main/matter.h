#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============ Core Matter Initialization ============
void matter_init(void);
bool matter_is_commissioned(void);
bool matter_is_ready(void);
bool matter_is_commissioning(void);  // Returns true while pairing/commissioning
bool matter_is_transport_connected(void);  // True when active transport (Thread/Wi-Fi) is established

// ============ Button Events to Matter ============
// Double/triple/... click while inside a channel menu, reported on that channel's switch endpoint
void matter_send_channel_multi_press(uint8_t channel, uint8_t press_count);
// "Dial events" channel: notches turned (+ up, - down), reported on the channel's dial up/down endpoints.
// At most MATTER_DIAL_MAX_NOTCHES per call; returns how many notches were sent (same sign as steps).
#define MATTER_DIAL_MAX_NOTCHES 10
int matter_send_dial(uint8_t channel, int steps);

// ============ Channel type ============
// What each channel controls, chosen from the controller (a "Channel N type" select in Home Assistant).
// Only changes the remote's behaviour: the Matter light endpoints keep the same structure.
typedef enum {
    CHANNEL_TYPE_ON_OFF = 0,    // click or dial toggles
    CHANNEL_TYPE_DIMMABLE = 1,  // intensity page only
    CHANNEL_TYPE_WHITE = 2,     // intensity + colour temperature
    CHANNEL_TYPE_COLOR = 3,     // intensity + colour temperature + colour (default)
    CHANNEL_TYPE_EVENTS = 4,    // no light: dial and clicks are reported as events (dial up/down endpoints)
    CHANNEL_TYPE_COUNT
} channel_type_t;

channel_type_t matter_get_channel_type(uint8_t channel);

// ============ MenuSystem Synchronization ============
// Interface structure for MenuSystem callbacks
typedef struct {
    void (*updateIntensity)(uint8_t channel, uint8_t value);       // 0-100
    void (*updateColorTemp)(uint8_t channel, uint8_t value);       // 0-100
    void (*updateRainbowHue)(uint8_t channel, uint8_t hue);        // 0-255
    void (*updateRainbowMode)(uint8_t channel, bool enabled);
    void (*updateDisplay)(void);
    bool (*isLockedOut)(uint8_t channel);
    void (*channelTypeChanged)(uint8_t channel);
} MenuSystemInterface;

// Register MenuSystem callback interface
void matter_set_menu_system_interface(const MenuSystemInterface *iface);

// Update Matter endpoints FROM MenuSystem (user interaction)
void matter_update_intensity(uint8_t channel, uint8_t intensity_0_100);
void matter_update_color_temp(uint8_t channel, uint8_t temp_0_100);
void matter_update_rainbow_hue(uint8_t channel, uint8_t hue_0_255);

// ============ Battery Management ============
void matter_set_battery_percent(uint8_t percent);

// ============ Sleep & Configuration ============
// Idle time before deep sleep, from the controller's "Deep sleep after" select (0 = never)
uint32_t matter_get_deep_sleep_ms(void);
int matter_get_personality_int(void);
// Stop + deinit the Wi-Fi driver while holding the Matter stack lock (Thread personality only)
esp_err_t matter_shutdown_wifi_radio(void);

#ifdef __cplusplus
}
#endif
