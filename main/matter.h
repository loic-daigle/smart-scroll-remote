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
void matter_send_button_event(uint8_t press_count);
void matter_send_long_press(void);
// Double/triple/... click while inside a channel menu, reported on that channel's switch endpoint
void matter_send_channel_multi_press(uint8_t channel, uint8_t press_count);

// ============ MenuSystem Synchronization ============
// Interface structure for MenuSystem callbacks
typedef struct {
    void (*updateIntensity)(uint8_t channel, uint8_t value);       // 0-100
    void (*updateColorTemp)(uint8_t channel, uint8_t value);       // 0-100
    void (*updateRainbowHue)(uint8_t channel, uint8_t hue);        // 0-255
    void (*updateRainbowMode)(uint8_t channel, bool enabled);
    void (*updateDisplay)(void);
    bool (*isLockedOut)(uint8_t channel);
} MenuSystemInterface;

// Register MenuSystem callback interface
void matter_set_menu_system_interface(const MenuSystemInterface *iface);

// Update Matter endpoints FROM MenuSystem (user interaction)
void matter_update_intensity(uint8_t channel, uint8_t intensity_0_100);
void matter_update_color_temp(uint8_t channel, uint8_t temp_0_100);
void matter_update_rainbow_hue(uint8_t channel, uint8_t hue_0_255);

// ============ LED Color Updates ============
// Get RGB colors from Matter state (3 endpoints only)
void matter_update_colors(uint32_t *ch0_rgb, uint32_t *ch1_rgb, uint32_t *ch2_rgb);

// ============ Battery Management ============
void matter_set_battery_percent(uint8_t percent);

// ============ Sleep & Configuration ============
uint32_t matter_get_sleep_timeout_ms(void);
int matter_get_personality_int(void);
// Stop + deinit the Wi-Fi driver while holding the Matter stack lock (Thread personality only)
esp_err_t matter_shutdown_wifi_radio(void);
void matter_toggle_personality_nvs(void);
void matter_reboot(void);
void matter_toggle_personality(void);

#ifdef __cplusplus
}
#endif
