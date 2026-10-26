#include "button_menu.h"
#include "esp_log.h"
#include "matter.h"
#include "nvs.h"
#include <math.h>
#include "esp_timer.h"

static const char *TAG = "menu_system_matter";

// ==================== C BRIDGE TO ACTUAL MENU ====================
static menu_system_t *g_actual_menu = NULL;
static uint32_t last_interaction_times[3] = {0, 0, 0};

extern "C" void menu_system_bridge_init(menu_system_t *actualMenu) {
    g_actual_menu = actualMenu;
    ESP_LOGI(TAG, "Menu bridge initialized with actual menu at %p", actualMenu);
}

extern "C" void menu_system_notify_interaction(uint8_t channel) {
    if (channel < 3) {
        last_interaction_times[channel] = (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
}

static bool is_locked_out(uint8_t channel) {
    if (channel < 3) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        return (now - last_interaction_times[channel] < 1500);
    }
    return false;
}

// ==================== MATTER CALLBACKS ====================
// Values from the controller are always stored (and saved to NVS), so the dial continues from the real
// light state. The LEDs only follow while inside that channel's menu: no updates on the home screen.
static inline bool should_refresh_display(uint8_t channel) {
    return g_actual_menu->currentState != MENU_SELECT_CHANNEL && g_actual_menu->currentChannel == channel;
}

static void matter_cb_update_intensity(uint8_t channel, uint8_t value) {
    if (!g_actual_menu || channel >= 3 || is_locked_out(channel)) return;
    if (!menu_lock(200)) return;
    g_actual_menu->intensity[channel] = value;
    menu_request_nvs_save();
    if (should_refresh_display(channel)) {
        menu_system_update_display(g_actual_menu);
    }
    menu_unlock();
}

static void matter_cb_update_color_temp(uint8_t channel, uint8_t value) {
    if (!g_actual_menu || channel >= 3 || is_locked_out(channel)) return;
    if (!menu_lock(200)) return;
    g_actual_menu->colorTemp[channel] = value;
    g_actual_menu->isRainbowMode[channel] = false;
    menu_request_nvs_save();
    if (should_refresh_display(channel)) {
        menu_system_update_display(g_actual_menu);
    }
    menu_unlock();
}

// hue is already on the internal 0-255 scale (converted in matter.cpp)
static void matter_cb_update_rainbow_hue(uint8_t channel, uint8_t hue) {
    if (!g_actual_menu || channel >= 3 || is_locked_out(channel)) return;
    if (!menu_lock(200)) return;
    g_actual_menu->rainbowHue[channel] = hue;
    menu_request_nvs_save();
    if (should_refresh_display(channel)) {
        menu_system_update_display(g_actual_menu);
    }
    menu_unlock();
}

static void matter_cb_update_rainbow_mode(uint8_t channel, bool enabled) {
    if (!g_actual_menu || channel >= 3 || is_locked_out(channel)) return;
    if (!menu_lock(200)) return;
    g_actual_menu->isRainbowMode[channel] = enabled;
    menu_request_nvs_save();
    if (should_refresh_display(channel)) {
        menu_system_update_display(g_actual_menu);
    }
    menu_unlock();
}

static void matter_cb_update_display(void) {
    if (!g_actual_menu || !menu_lock(200)) return;
    // Only update display if we are NOT on the main selection menu
    if (g_actual_menu->currentState != MENU_SELECT_CHANNEL) {
        menu_system_update_display(g_actual_menu);
    }
    menu_unlock();
}

static bool matter_cb_is_locked_out(uint8_t channel) {
    return is_locked_out(channel);
}

extern "C" void matter_init_menu_system(void) {
    static const MenuSystemInterface iface = {
        .updateIntensity = matter_cb_update_intensity,
        .updateColorTemp = matter_cb_update_color_temp,
        .updateRainbowHue = matter_cb_update_rainbow_hue,
        .updateRainbowMode = matter_cb_update_rainbow_mode,
        .updateDisplay = matter_cb_update_display,
        .isLockedOut = matter_cb_is_locked_out,
    };
    matter_set_menu_system_interface(&iface);
}
