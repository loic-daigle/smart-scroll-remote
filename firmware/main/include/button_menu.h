#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_timer.h>

// Menu states - exactly matching Arduino
typedef enum {
    MENU_SELECT_CHANNEL,
    IN_CHANNEL_INTENSITY,
    IN_CHANNEL_COLOR_TEMP,
    IN_CHANNEL_RAINBOW
} menu_state_t;

// Button event codes
#define BTN_NO_EVENT 0
#define BTN_LONG_PRESS -1
// 1-5 for click counts

// SmartButton state
typedef struct {
    uint8_t pin;
    bool lastReading;
    bool buttonState;
    uint32_t lastDebounceTime;
    uint32_t pressTime;
    uint32_t lastClickTime;

    uint32_t debounceDelay;
    uint32_t clickTimeout;
    uint32_t longPressTime;

    uint8_t clickCount;
    bool longPressHandled;
} smart_button_t;

// MenuSystem state
typedef struct {
    menu_state_t currentState;
    uint8_t currentChannel;
    int lastEncoderVal;

    uint8_t intensity[3];
    uint8_t colorTemp[3];
    uint8_t rainbowHue[3];
    bool isRainbowMode[3];
    int16_t dialPending[3];   // "Dial events" channels: notches turned, not sent yet (+ up, - down)

    bool showOverview;

    uint32_t lastActivityTime;
} menu_system_t;

#ifdef __cplusplus
extern "C" {
#endif

// SmartButton functions
void smart_button_init(smart_button_t *btn, uint8_t pin, uint32_t debounce, uint32_t timeout, uint32_t longPress);
int smart_button_update(smart_button_t *btn, uint32_t now);
bool smart_button_is_held(smart_button_t *btn);

// MenuSystem functions
void menu_system_init(menu_system_t *menu);
void menu_system_set_overview(menu_system_t *menu, bool active);
void menu_system_handle_encoder(menu_system_t *menu, int currentEncoderVal);
void menu_system_handle_button(menu_system_t *menu, int event);
uint32_t menu_system_get_channel_color(menu_system_t *menu, uint8_t ch, bool forceVis);
void menu_system_update_display(menu_system_t *menu);
void menu_system_save_to_nvs(menu_system_t *menu);
void menu_system_load_from_nvs(menu_system_t *menu);
void menu_system_notify_interaction(uint8_t channel);

// Implemented in main.c: guard the menu state shared with the Matter task, and schedule a deferred NVS save
bool menu_lock(uint32_t timeout_ms);
void menu_unlock(void);
void menu_request_nvs_save(void);

#ifdef __cplusplus
}
#endif
