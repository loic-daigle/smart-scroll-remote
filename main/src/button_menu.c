#include "button_menu.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "led_strip.h"
#include <math.h>

#include <freertos/semphr.h>


extern led_strip_handle_t led_strip;
extern nvs_handle_t app_nvs_handle;
extern bool useLed;
extern SemaphoreHandle_t led_mutex;

static inline uint32_t get_millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ==================== SmartButton ====================
void smart_button_init(smart_button_t *btn, uint8_t pin, uint32_t debounce, uint32_t timeout, uint32_t longPress) {
    btn->pin = pin;
    btn->debounceDelay = debounce;
    btn->clickTimeout = timeout;
    btn->longPressTime = longPress;

    btn->lastReading = 1;  // HIGH (not pressed)
    btn->buttonState = 1;  // HIGH
    btn->lastDebounceTime = 0;
    btn->lastClickTime = 0;
    btn->pressTime = 0;
    btn->clickCount = 0;
    btn->longPressHandled = false;

    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

int smart_button_update(smart_button_t *btn, uint32_t now) {
    int event = 0;
    bool currentReading = gpio_get_level(btn->pin);

    if (currentReading != btn->lastReading) {
        btn->lastDebounceTime = now;
    }

    if ((now - btn->lastDebounceTime) > btn->debounceDelay) {
        if (currentReading != btn->buttonState) {
            btn->buttonState = currentReading;

            if (btn->buttonState == 0) {  // LOW (pressed)
                btn->pressTime = now;
                btn->longPressHandled = false;
            } else {  // HIGH (released)
                if (!btn->longPressHandled) {
                    btn->clickCount++;
                    btn->lastClickTime = now;
                }
            }
        }
    }

    if (btn->buttonState == 0 && !btn->longPressHandled) {
        long pressDuration = now - btn->pressTime;
        if (pressDuration > btn->longPressTime) {
            btn->longPressHandled = true;
            btn->clickCount = 0;
            event = BTN_LONG_PRESS;  // -1
        }
    }

    if (btn->buttonState == 1 && btn->clickCount > 0) {  // HIGH and clicks pending
        long timeSinceClick = now - btn->lastClickTime;
        if (timeSinceClick > btn->clickTimeout) {
            event = btn->clickCount;
            if (event > 5) event = 5;
            btn->clickCount = 0;
        }
    }

    btn->lastReading = currentReading;
    return event;
}

bool smart_button_is_held(smart_button_t *btn) {
    return (btn->buttonState == 0 && btn->longPressHandled);
}

// ==================== MenuSystem ====================
void menu_system_init(menu_system_t *menu) {
    // Note: currentState and currentChannel must be set BEFORE calling init 
    // (e.g. from app_main for RTC restore), or they default here.
    // If they are already set to something other than 0/MENU_SELECT_CHANNEL,
    // we assume they were restored from RTC.
    
    menu->lastEncoderVal = 0;
    menu->showOverview = false;
    menu->lastActivityTime = get_millis();

    for (int i = 0; i < 3; i++) {
        menu->intensity[i] = 50;
        menu->colorTemp[i] = 50;
        menu->rainbowHue[i] = 0;
        menu->isRainbowMode[i] = false;
    }

    menu_system_load_from_nvs(menu);
}

void menu_system_load_from_nvs(menu_system_t *menu) {
    for (int i = 0; i < 3; i++) {
        char key[16];

        snprintf(key, sizeof(key), "ch%d_int", i);
        uint32_t tmp = 50;
        if (nvs_get_u32(app_nvs_handle, key, &tmp) == ESP_OK) {
            menu->intensity[i] = (uint8_t)tmp;
        }

        snprintf(key, sizeof(key), "ch%d_ct", i);
        tmp = 50;
        if (nvs_get_u32(app_nvs_handle, key, &tmp) == ESP_OK) {
            menu->colorTemp[i] = (uint8_t)tmp;
        }

        snprintf(key, sizeof(key), "ch%d_hue", i);
        tmp = 0;
        if (nvs_get_u32(app_nvs_handle, key, &tmp) == ESP_OK) {
            menu->rainbowHue[i] = (uint8_t)tmp;
        }

        snprintf(key, sizeof(key), "ch%d_rb", i);
        tmp = 0;
        if (nvs_get_u32(app_nvs_handle, key, &tmp) == ESP_OK) {
            menu->isRainbowMode[i] = (bool)tmp;
        }
    }
}

void menu_system_save_to_nvs(menu_system_t *menu) {
    esp_err_t err = ESP_OK;

    for (int i = 0; i < 3; i++) {
        char key[16];

        snprintf(key, sizeof(key), "ch%d_int", i);
        err = nvs_set_u32(app_nvs_handle, key, menu->intensity[i]);
        if (err != ESP_OK) break;

        snprintf(key, sizeof(key), "ch%d_ct", i);
        err = nvs_set_u32(app_nvs_handle, key, menu->colorTemp[i]);
        if (err != ESP_OK) break;

        snprintf(key, sizeof(key), "ch%d_hue", i);
        err = nvs_set_u32(app_nvs_handle, key, menu->rainbowHue[i]);
        if (err != ESP_OK) break;

        snprintf(key, sizeof(key), "ch%d_rb", i);
        err = nvs_set_u32(app_nvs_handle, key, menu->isRainbowMode[i] ? 1 : 0);
        if (err != ESP_OK) break;
    }

    if (err == ESP_OK) {
        err = nvs_commit(app_nvs_handle);
    }

    if (err != ESP_OK) {
        ESP_LOGW("menu_system", "Failed to save menu state to NVS: %s", esp_err_to_name(err));
    }
}

static uint32_t hsv_to_rgb(uint16_t hue, uint8_t sat, uint8_t val) {
    hue = (hue * 360) / 256;
    sat = (sat > 254) ? 254 : sat;
    val = (val > 254) ? 254 : val;

    float hf = hue / 360.0f;
    float sf = sat / 255.0f;
    float vf = val / 255.0f;

    float r, g, b;

    if (sf == 0.0f) {
        r = g = b = vf;
    } else {
        float h = hf * 6.0f;
        int i = (int)h;
        float f = h - i;

        float p = vf * (1.0f - sf);
        float q = vf * (1.0f - sf * f);
        float t = vf * (1.0f - sf * (1.0f - f));

        switch (i % 6) {
            case 0: r = vf; g = t; b = p; break;
            case 1: r = q; g = vf; b = p; break;
            case 2: r = p; g = vf; b = t; break;
            case 3: r = p; g = q; b = vf; break;
            case 4: r = t; g = p; b = vf; break;
            case 5: r = vf; g = p; b = q; break;
            default: r = g = b = 0; break;
        }
    }

    uint8_t r8 = (uint8_t)(r * 255);
    uint8_t g8 = (uint8_t)(g * 255);
    uint8_t b8 = (uint8_t)(b * 255);

    return (r8 << 16) | (g8 << 8) | b8;
}

uint32_t menu_system_get_channel_color(menu_system_t *menu, uint8_t ch, bool forceVis) {
    uint8_t r_base, g_base, b_base;

    // PREVIEW LOGIC: If we are actively in a sub-menu for this channel, 
    // prioritize showing the relevant preview (White for CCT, Rainbow for RGB).
    bool showRainbow = menu->isRainbowMode[ch];
    if (menu->currentChannel == ch) {
        if (menu->currentState == IN_CHANNEL_RAINBOW) showRainbow = true;
        if (menu->currentState == IN_CHANNEL_COLOR_TEMP) showRainbow = false;
    }

    if (showRainbow) {
        uint32_t rgb = hsv_to_rgb(menu->rainbowHue[ch], 255, 255);
        r_base = (rgb >> 16) & 0xFF;
        g_base = (rgb >> 8) & 0xFF;
        b_base = rgb & 0xFF;
    } else {
        int val = menu->colorTemp[ch];
        // Refined Curves: 0% (6500K) -> Vibrant Blue-White, 100% (2000K) -> Rich Amber
        r_base = 120 + (val * 135) / 100;   // 120(Cool) -> 255(Warm)
        g_base = 150 - (val * 50) / 100;    // 150(Cool) -> 100(Warm)
        b_base = 255 - (val * 240) / 100;   // 255(Cool) -> 15(Warm)
    }

    float factor;
    if (forceVis) {
        int display_intensity = (menu->intensity[ch] < 20) ? 20 : menu->intensity[ch];
        factor = display_intensity / 100.0f;
    } else {
        factor = menu->intensity[ch] / 100.0f;
    }

    uint8_t r = (uint8_t)(r_base * factor);
    uint8_t g = (uint8_t)(g_base * factor);
    uint8_t b = (uint8_t)(b_base * factor);

    return (r << 16) | (g << 8) | b;
}

void menu_system_set_overview(menu_system_t *menu, bool active) {
    if (menu->currentState == MENU_SELECT_CHANNEL) {
        if (menu->showOverview != active) {
            menu->showOverview = active;
            menu_system_update_display(menu);
        }
    }
}

void menu_system_handle_encoder(menu_system_t *menu, int currentVal) {
    // ENCODER OPTIMIZATION: EC21A1520401 is 15 pulses / 30 detents.
    // Standard quadrature state machines give 4 increments per pulse.
    // Logic: Division by 2 instead of 4 means 1 detent = 1 step.
    int steps = currentVal / 2;
    int lastSteps = menu->lastEncoderVal / 2;

    if (steps != lastSteps) {
        int dir = (steps > lastSteps) ? 1 : -1;


        switch (menu->currentState) {
            case MENU_SELECT_CHANNEL:
                if (!menu->showOverview) {
                    int nextCh = (int)menu->currentChannel + dir;
                    if (nextCh > 2) nextCh = 0;
                    if (nextCh < 0) nextCh = 2;
                    menu->currentChannel = (int8_t)nextCh;
                }
                break;

            case IN_CHANNEL_INTENSITY: {
                int nextVal = (int)menu->intensity[menu->currentChannel] + dir * 5;
                if (nextVal > 100) nextVal = 100;
                if (nextVal < 0) nextVal = 0;
                menu->intensity[menu->currentChannel] = (uint8_t)nextVal;
                break;
            }

            case IN_CHANNEL_COLOR_TEMP: {
                int nextVal = (int)menu->colorTemp[menu->currentChannel] + dir * 5;
                if (nextVal > 100) nextVal = 100;
                if (nextVal < 0) nextVal = 0;
                menu->colorTemp[menu->currentChannel] = (uint8_t)nextVal;
                menu->isRainbowMode[menu->currentChannel] = false;
                break;
            }

            case IN_CHANNEL_RAINBOW:
                // Rainbow Hue SHOULD wrap around 0-255
                menu->rainbowHue[menu->currentChannel] += dir * 10;
                menu->isRainbowMode[menu->currentChannel] = true;
                break;
        }

        menu->lastEncoderVal = currentVal;
        menu->lastActivityTime = get_millis();
        menu_system_notify_interaction(menu->currentChannel);
        // Don't call display update here - let display_task handle it
        // Just update state in button_task
    }
}

static void flash_transition() {
    if (led_strip && useLed) {
        if (xSemaphoreTake(led_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            led_strip_clear(led_strip);
            led_strip_refresh(led_strip);
            xSemaphoreGive(led_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void menu_system_handle_button(menu_system_t *menu, int event) {

    if (event == BTN_LONG_PRESS) {  // -1
        if (menu->currentState != MENU_SELECT_CHANNEL) {
            flash_transition();
            menu->currentState = MENU_SELECT_CHANNEL;
            menu->lastActivityTime = get_millis();
            menu_system_notify_interaction(menu->currentChannel);
        } else {
        }
    } else if (event == 1) {
        if (menu->showOverview) {
            return;
        }

        flash_transition();
        switch (menu->currentState) {
            case MENU_SELECT_CHANNEL:
                menu->currentState = IN_CHANNEL_INTENSITY;
                break;
            case IN_CHANNEL_INTENSITY:
                menu->currentState = IN_CHANNEL_COLOR_TEMP;
                break;
            case IN_CHANNEL_COLOR_TEMP:
                menu->currentState = IN_CHANNEL_RAINBOW;
                break;
            case IN_CHANNEL_RAINBOW:
                menu->currentState = IN_CHANNEL_INTENSITY;
                break;
        }
        menu->lastActivityTime = get_millis();
        menu_system_notify_interaction(menu->currentChannel);
    } else {
    }
    // Don't call display update here - display_task will handle it
}

void menu_system_update_display(menu_system_t *menu) {
    if (!led_strip || !useLed) return;

    // Acquire mutex to prevent conflicts with display_task
    if (xSemaphoreTake(led_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;  // Skip if mutex is busy
    }

    // Avoid led_strip_clear() here: it refreshes an all-off frame, which causes visible flicker.
    for (int i = 0; i < 3; i++) {
        led_strip_set_pixel(led_strip, i, 0, 0, 0);
    }

    if (menu->currentState == MENU_SELECT_CHANNEL) {
        if (menu->showOverview) {
            // Show all channels with their colors
            for (int i = 0; i < 3; i++) {
                uint32_t color = menu_system_get_channel_color(menu, i, false);
                uint8_t r = (color >> 16) & 0xFF;
                uint8_t g = (color >> 8) & 0xFF;
                uint8_t b = color & 0xFF;
                led_strip_set_pixel(led_strip, i, r, g, b);
            }
        } else {
            // Show only selected channel as gray
            led_strip_set_pixel(led_strip, menu->currentChannel, 50, 50, 50);
        }
    } else {
        // In a menu state - show all LEDs with current channel color
        bool forceVis = (menu->currentState != IN_CHANNEL_INTENSITY);
        uint32_t color = menu_system_get_channel_color(menu, menu->currentChannel, forceVis);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;

        for (int i = 0; i < 3; i++) {
            led_strip_set_pixel(led_strip, i, r, g, b);
        }
    }

    led_strip_refresh(led_strip);
    xSemaphoreGive(led_mutex);
}
