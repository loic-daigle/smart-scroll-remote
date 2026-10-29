                #include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_pm.h"

#include "driver/gpio.h"
#include "led_strip.h"

#include "matter.h"
#include "button_menu.h"
#include "esp_wifi.h"

// ------------------ Debug Configuration ------------------
// Set to 0 to disable most debug logging for faster performance
#define DEBUG_LOGGING 0

#if DEBUG_LOGGING
#define DEBUG_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define DEBUG_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#else
#define DEBUG_LOGI(tag, fmt, ...) do {} while(0)
#define DEBUG_LOGW(tag, fmt, ...) do {} while(0)
#endif

// ------------------ Hardware Configuration ------------------
// New hardware: Encoder + 3 LEDs
#define PIN_BUTTON 0         // Button input
#define PIN_ENC_A 2          // Encoder A channel
#define PIN_ENC_B 3          // Encoder B channel
#define PIN_VBUS -1          // USB detection (unused)
#define PIN_NEO 6            // NeoPixel data line
#define PIN_NEO_EN 7         // NeoPixel power enable

#define LED_COUNT 3          // 3 NeoPixels

// ------------------ Timing Constants ------------------
#define DEBOUNCE_MS             50
#define LONG_PRESS_MS           400
#define MULTI_CLICK_TIMEOUT_MS  200
#define BATTERY_READ_INTERVAL_MS 60000
#define LIGHT_SLEEP_IDLE_MS     5000
#define SYNC_MIN_GAP_MS         300   // Min spacing between Matter publishes; dial turns in between coalesce into one

// ------------------ Battery ADC Configuration ------------------
#define PIN_BAT_ADC_GPIO 1
// GPIO 1 is ADC1 Channel 1 on ESP32-C6
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_1
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_MV_FULL 4200
#define BATTERY_MV_EMPTY 3000

// ------------------ Globals ------------------
static const char *TAG = "smart_button";

led_strip_handle_t led_strip;
uint8_t led_brightness = 50;

nvs_handle_t app_nvs_handle;

uint32_t cSingle, cDouble, cTriple, cQuad, cQuint, cLong, cError;
bool useLed = true;
uint32_t waitBeforeOffMs = 300000;

// ADC handles for battery reading
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_cali_valid = false;

// Forward declarations
static void update_battery_percent(void);

// Encoder state tracking
static volatile int encoderValue = 0; // Matches Arduino exactly
static uint8_t enc_old_AB = 0;

// Power management: held while the UI is active; released when idle so the PM can
// light-sleep between Thread polls / Wi-Fi DTIM beacons without dropping the network.
static esp_pm_lock_handle_t ui_awake_lock = NULL;
static TaskHandle_t button_task_handle = NULL;
static TaskHandle_t matter_sync_task_handle = NULL;
static volatile int idle_wake_pin = -1;


// Button and menu
static smart_button_t button;
static menu_system_t menu;
static bool is_plugged = false; // Detection via high battery voltage (charging)
static uint32_t ha_update_pulse_time = 0; // Timer for visual feedback from HA
static uint32_t ui_feedback_enable_time_ms = 0; // Suppress HA visual pulses during early boot

static volatile uint32_t lastNvsWriteTime = 0;
static volatile bool nvsWritePending = false;

// Local user changes must be published even if activity debounce/jitter delays the normal sync window.
static volatile bool pending_intensity_sync[3] = {false, false, false};
static volatile bool pending_color_temp_sync[3] = {false, false, false};
static volatile bool pending_rainbow_hue_sync[3] = {false, false, false};
static volatile bool pending_rainbow_mode_sync[3] = {false, false, false};
static volatile bool suppress_next_single_click = false;
static uint32_t suppress_single_click_until_ms = 0;
static bool pending_button_wake_action = false;

SemaphoreHandle_t led_mutex = NULL;
static SemaphoreHandle_t menu_mutex = NULL;

// ==================== STATE RETENTION ====================
RTC_DATA_ATTR static menu_state_t rtc_currentState = MENU_SELECT_CHANNEL;
RTC_DATA_ATTR static uint8_t rtc_currentChannel = 0;

// ==================== ENCODER ISR ====================
// True hardware ISR identical to Arduino attachInterrupt(..., CHANGE)
static void IRAM_ATTR encoder_isr_handler(void* arg) {
    static const int8_t states[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    enc_old_AB <<= 2;
    enc_old_AB |= ((gpio_get_level(PIN_ENC_B) << 1) | gpio_get_level(PIN_ENC_A));
    encoderValue += states[(enc_old_AB & 0x0F)];
}

// While idle, inputs are level-triggered wake sources. Mask them on first hit so the
// level doesn't re-fire, and hand control back to button_task.
static void IRAM_ATTR input_wake_isr_handler(void* arg) {
    gpio_intr_disable(PIN_BUTTON);
    gpio_intr_disable(PIN_ENC_A);
    gpio_intr_disable(PIN_ENC_B);
    if (idle_wake_pin < 0) {
        idle_wake_pin = (int)(intptr_t)arg;
    }
    BaseType_t higher_prio_woken = pdFALSE;
    vTaskNotifyGiveFromISR(button_task_handle, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

// ------------------ Time helpers ------------------
static inline uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline void delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

static inline void request_ha_pulse(void)
{
    uint32_t now = millis();
    if (now >= ui_feedback_enable_time_ms) {
        ha_update_pulse_time = now;
    }
}

bool menu_lock(uint32_t timeout_ms)
{
    return menu_mutex && xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void menu_unlock(void)
{
    xSemaphoreGive(menu_mutex);
}

void menu_request_nvs_save(void)
{
    nvsWritePending = true;
    lastNvsWriteTime = millis();
}

static inline void request_matter_sync(void)
{
    if (matter_sync_task_handle) {
        xTaskNotifyGive(matter_sync_task_handle);
    }
}

static void toggle_channel_from_button_wake(uint8_t channel)
{
    if (channel >= 3) {
        return;
    }

    uint32_t now = millis();
    uint8_t next_intensity = (menu.intensity[channel] == 0) ? 50 : 0;

    menu.currentState = IN_CHANNEL_INTENSITY;
    menu.showOverview = false;
    menu.lastActivityTime = now;
    menu.intensity[channel] = next_intensity;
    pending_intensity_sync[channel] = true;
    nvsWritePending = true;
    lastNvsWriteTime = now;
    request_matter_sync();

    menu_system_notify_interaction(channel);
    menu_system_update_display(&menu);
}

static inline void arm_single_click_suppression(uint32_t now_ms)
{
    suppress_next_single_click = true;
    suppress_single_click_until_ms = now_ms + 700;
}

// ------------------ NVS helpers ------------------
static uint32_t nvs_get_u32_default(const char *key, uint32_t def)
{
    uint32_t tmp;
    return (nvs_get_u32(app_nvs_handle, key, &tmp) == ESP_OK) ? tmp : def;
}

// ------------------ LED helpers ------------------
static void led_set_color(uint32_t color)
{
    if (!useLed)
    {
        return;
    }

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // Brightness scaling
    r = (uint8_t)((uint16_t)r * led_brightness / 255U);
    g = (uint8_t)((uint16_t)g * led_brightness / 255U);
    b = (uint8_t)((uint16_t)b * led_brightness / 255U);

    for (int i = 0; i < LED_COUNT; i++)
    {
        led_strip_set_pixel(led_strip, i, r, g, b);
    }
    
    // Refresh only if non-zero color or if we want to clear
    led_strip_refresh(led_strip);
}

static void showColor(uint32_t color, int duration_ms)
{
    if (!useLed)
    {
        return;
    }

    if (color == 0)
    {
        led_strip_clear(led_strip);
        // Let display_task handle power off to avoid state conflicts
        return;
    }

    // Power is kept active (LOW) globally now.

    led_set_color(color);

    if (duration_ms > 0)
    {
        delay_ms(duration_ms);
        led_strip_clear(led_strip);
        // Let display_task handle power off so we don't break its state
    }
}

// ------------------ NVS init and load ------------------
static void nvs_init_and_load(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash full or new version - ERASING...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nvs_open("user_cfg", NVS_READWRITE, &app_nvs_handle));

    cSingle = nvs_get_u32_default("c1", 0x00FF00);
    cDouble = nvs_get_u32_default("c2", 0x0000FF);
    cTriple = nvs_get_u32_default("c3", 0xFFFF00);
    cQuad   = nvs_get_u32_default("c4", 0x00FFFF);
    cQuint  = nvs_get_u32_default("c5", 0xFFFFFF);
    cLong   = nvs_get_u32_default("cL", 0xFF00FF);
    cError  = nvs_get_u32_default("ce", 0xFF0000);
    waitBeforeOffMs = nvs_get_u32_default("delay", 300000);

    uint8_t led_en = 1;
    size_t len = sizeof(led_en);
    if (nvs_get_blob(app_nvs_handle, "led_en", &led_en, &len) != ESP_OK)
    {
        led_en = 1;
    }
    useLed = (led_en != 0);
}

void app_nvs_set_u32(const char *key, uint32_t value)
{
    esp_err_t err = nvs_set_u32(app_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u32 failed for key '%s': %s", key, esp_err_to_name(err));
        return;
    }

    err = nvs_commit(app_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed for key '%s': %s", key, esp_err_to_name(err));
    }
}


// ------------------ Setup ------------------
static void gpio_init_all(void)
{
    gpio_config_t io_conf = {0};

    // 1. Neopixel Enable Pin (GPIO 7) - Power control for LEDs
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_NEO_EN);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(PIN_NEO_EN, 0); // Active LOW: Start with LEDs ON!

    // 2. Button input (GPIO 0, Active-Low with Pull-Up)
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;  // ISR handled in button_task
    io_conf.pin_bit_mask = (1ULL << PIN_BUTTON);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    // 3. Encoder pins (GPIO 2, 3) - Inputs with ISR (ANYEDGE)
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    // Install GPIO ISR service (ESP_ERR_INVALID_STATE is harmless if already installed)
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    // Attach the ISR handler
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ENC_A, encoder_isr_handler, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ENC_B, encoder_isr_handler, NULL));

    if (PIN_VBUS != -1) {
        io_conf.pin_bit_mask = (1ULL << (PIN_VBUS > 0 ? PIN_VBUS : 0)); // Dummy if -1 to avoid warning
        if (PIN_VBUS < 0) io_conf.pin_bit_mask = 0;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }
}

static void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_NEO,
        .max_leds = LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

// Initialize ADC for battery voltage reading
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BATTERY_ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_config));

    // Try to create calibration handle (ESP32-C6 uses curve fitting)
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle) == ESP_OK) {
        adc_cali_valid = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration failed - using raw estimates");
    }
}

// Read battery voltage and update Matter attribute
static void update_battery_percent(void)
{
    if (!adc_handle) return;

    uint32_t sum = 0;
    int raw = 0;
    const int samples = 20;
    int ok_samples = 0;

    for (int i = 0; i < samples; i++) {
        if (adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            ok_samples++;
        }
        esp_rom_delay_us(2000);
    }

    if (ok_samples == 0) {
        ESP_LOGW(TAG, "Battery ADC read failed for all samples");
        return;
    }

    float raw_average = (float)sum / ok_samples;
    int voltage_mv = 0;
    int raw_avg_int = (int)(raw_average + 0.5f);

    if (adc_cali_valid && adc_cali_handle) {
        adc_cali_raw_to_voltage(adc_cali_handle, raw_avg_int, &voltage_mv);
    } else {
        voltage_mv = (int)((raw_average / 4095.0f) * 3300.0f);
    }

    int battery_mv = voltage_mv * 2;
    int percent = 0;

    if (battery_mv >= BATTERY_MV_FULL) {
        percent = 100;
    } else if (battery_mv <= BATTERY_MV_EMPTY) {
        percent = 0;
    } else {
        percent = (battery_mv - BATTERY_MV_EMPTY) * 100 / (BATTERY_MV_FULL - BATTERY_MV_EMPTY);
    }

    matter_set_battery_percent((uint8_t)percent);
    
    // Simple "Plugged" detection: if battery voltage is >= 4200mV (charging range)
    is_plugged = (battery_mv >= 4200); 

    DEBUG_LOGI(TAG, "Battery: %d mV, %d%%, Plugged: %s", battery_mv, percent, is_plugged ? "YES" : "NO");
}

// Button and encoder task - handles user input and menu navigation
// ==================== WAKEUP HANDLER ====================

static void configure_ext1_wakeup_for_inputs(void)
{
    // Half-step trap fix: wake on opposite level of current encoder state.
    esp_sleep_disable_ext1_wakeup_io(0);

    uint64_t low_mask = (1ULL << PIN_BUTTON);
    uint64_t high_mask = 0;

    if (gpio_get_level(PIN_ENC_A) == 1) {
        low_mask |= (1ULL << PIN_ENC_A);
    } else {
        high_mask |= (1ULL << PIN_ENC_A);
    }

    if (gpio_get_level(PIN_ENC_B) == 1) {
        low_mask |= (1ULL << PIN_ENC_B);
    } else {
        high_mask |= (1ULL << PIN_ENC_B);
    }

    if (low_mask > 0) {
        esp_sleep_enable_ext1_wakeup_io(low_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    if (high_mask > 0) {
        esp_sleep_enable_ext1_wakeup_io(high_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    }
}

static void arm_input_wakeup(void)
{
    gpio_intr_disable(PIN_ENC_A);
    gpio_intr_disable(PIN_ENC_B);
    idle_wake_pin = -1;

    gpio_isr_handler_add(PIN_BUTTON, input_wake_isr_handler, (void *)PIN_BUTTON);
    gpio_isr_handler_add(PIN_ENC_A, input_wake_isr_handler, (void *)PIN_ENC_A);
    gpio_isr_handler_add(PIN_ENC_B, input_wake_isr_handler, (void *)PIN_ENC_B);

    // Half-step trap fix: wake on opposite level of current encoder state.
    // If a pin flips between the read and the enable, the ISR fires immediately - no missed wake.
    gpio_wakeup_enable(PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(PIN_ENC_A, gpio_get_level(PIN_ENC_A) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable(PIN_ENC_B, gpio_get_level(PIN_ENC_B) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    gpio_intr_enable(PIN_BUTTON);
    gpio_intr_enable(PIN_ENC_A);
    gpio_intr_enable(PIN_ENC_B);
}

static void disarm_input_wakeup(void)
{
    gpio_intr_disable(PIN_BUTTON);
    gpio_intr_disable(PIN_ENC_A);
    gpio_intr_disable(PIN_ENC_B);

    gpio_wakeup_disable(PIN_BUTTON);
    gpio_wakeup_disable(PIN_ENC_A);
    gpio_wakeup_disable(PIN_ENC_B);

    // Back to polled button + edge-driven encoder
    gpio_isr_handler_remove(PIN_BUTTON);
    gpio_set_intr_type(PIN_BUTTON, GPIO_INTR_DISABLE);
    gpio_set_intr_type(PIN_ENC_A, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(PIN_ENC_B, GPIO_INTR_ANYEDGE);

    // Edges during sleep were not decoded; reseed so the first new edge doesn't count as a phantom step.
    enc_old_AB = (uint8_t)((gpio_get_level(PIN_ENC_B) << 1) | gpio_get_level(PIN_ENC_A));

    gpio_isr_handler_add(PIN_ENC_A, encoder_isr_handler, NULL);
    gpio_isr_handler_add(PIN_ENC_B, encoder_isr_handler, NULL);
    gpio_intr_enable(PIN_ENC_A);
    gpio_intr_enable(PIN_ENC_B);
}

// Idle: block until an input wakes us. The PM light-sleeps the chip whenever nothing is
// scheduled, but Thread polling / Wi-Fi beacons keep running, so the network link stays up.
static void enter_smart_sleep(void) {
    // Save state for potential deep-sleep reboot path.
    rtc_currentState = menu.currentState;
    rtc_currentChannel = menu.currentChannel;

    // Turn off NeoPixels before sleeping.
    gpio_set_level(PIN_NEO_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Idle: automatic light sleep enabled (network stays connected)");
    ulTaskNotifyTake(pdTRUE, 0); // Drop any stale wake notification
    arm_input_wakeup();
    esp_pm_lock_release(ui_awake_lock);

    // Deep sleep delay comes from the controller's "Deep sleep after" select (0 = never)
    uint32_t deep_sleep_ms = matter_get_deep_sleep_ms();
    uint32_t woke = ulTaskNotifyTake(pdTRUE, deep_sleep_ms ? pdMS_TO_TICKS(deep_sleep_ms) : portMAX_DELAY);

    esp_pm_lock_acquire(ui_awake_lock);
    disarm_input_wakeup();

    if (!woke) {
        ESP_LOGI(TAG, "%" PRIu32 " min inactivity reached: escalating to deep sleep", deep_sleep_ms / 60000);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        configure_ext1_wakeup_for_inputs();
        esp_deep_sleep_start();
    }

    gpio_set_level(PIN_NEO_EN, 0);

    if (xSemaphoreTake(menu_mutex, portMAX_DELAY) == pdTRUE) {
        menu.lastActivityTime = millis();

        if (idle_wake_pin == PIN_ENC_A || idle_wake_pin == PIN_ENC_B) {
            menu.currentState = IN_CHANNEL_INTENSITY;
            menu_system_update_display(&menu);
            ESP_LOGI(TAG, "Idle wake: encoder trigger (A/B), channel %d", menu.currentChannel);
        } else if (idle_wake_pin == PIN_BUTTON) {
            ESP_LOGI(TAG, "Idle wake: button trigger");
            arm_single_click_suppression(millis());
            toggle_channel_from_button_wake(menu.currentChannel);
        }
        xSemaphoreGive(menu_mutex);
    }
}

// ==================== MULTI-CLICK LED FEEDBACK ====================
// Confirms a multi-click was sent: 2 = sides, 3 = all, 4 = all then middle, 5 = all then sides.
static TaskHandle_t click_feedback_task_handle = NULL;

#define FEEDBACK_MIDDLE 0x2 // LED 1
#define FEEDBACK_SIDES  0x5 // LEDs 0 and 2
#define FEEDBACK_ALL    0x7

static void click_feedback_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        uint32_t count = 0;
        xTaskNotifyWait(0, UINT32_MAX, &count, portMAX_DELAY);
        if (!useLed || count < 2 || count > 5) {
            continue;
        }

        uint8_t groups[2] = {0, 0};
        switch (count) {
            case 2: groups[0] = FEEDBACK_SIDES; break;
            case 3: groups[0] = FEEDBACK_ALL; break;
            case 4: groups[0] = FEEDBACK_ALL; groups[1] = FEEDBACK_MIDDLE; break;
            case 5: groups[0] = FEEDBACK_ALL; groups[1] = FEEDBACK_SIDES; break;
        }

        // Hold the LED mutex for the whole pattern so menu redraws don't interleave with it
        if (xSemaphoreTake(led_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        for (int g = 0; g < 2 && groups[g]; g++) {
            for (int i = 0; i < LED_COUNT; i++) {
                uint8_t v = (groups[g] & (1 << i)) ? 80 : 0;
                led_strip_set_pixel(led_strip, i, v, v, v);
            }
            led_strip_refresh(led_strip);
            delay_ms(180);
            led_strip_clear(led_strip);
            delay_ms(150);
        }
        xSemaphoreGive(led_mutex);

        // Restore the normal menu display
        if (menu_lock(200)) {
            menu.lastActivityTime = millis();
            menu_system_update_display(&menu);
            menu_unlock();
        }
    }
}

static void button_task(void *pvParameters)
{
    (void)pvParameters;

    // Initialize button and menu
    smart_button_init(&button, PIN_BUTTON, DEBOUNCE_MS, MULTI_CLICK_TIMEOUT_MS, LONG_PRESS_MS);
    if (xSemaphoreTake(menu_mutex, portMAX_DELAY) == pdTRUE) {
        menu_system_init(&menu);
        xSemaphoreGive(menu_mutex);
    }

    if (xSemaphoreTake(menu_mutex, portMAX_DELAY) == pdTRUE) {
        if (pending_button_wake_action) {
            arm_single_click_suppression(millis());
            toggle_channel_from_button_wake(menu.currentChannel);
            pending_button_wake_action = false;
        }
        xSemaphoreGive(menu_mutex);
    }

    static int lastRawEncoder = 0;

    while (1) {
        uint32_t now = millis();

        // Get button event
        int btnEvent = smart_button_update(&button, now);

        int currentEncoder = encoderValue;

        if (xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // --- LOGIQUE DU MENU ---
        int pre_state = menu.currentState;
        bool pre_overview = menu.showOverview;
        int pre_channel = menu.currentChannel;
        uint8_t pre_intensity[3];
        uint8_t pre_colorTemp[3];
        uint8_t pre_rainbowHue[3];
        bool pre_rainbowMode[3];
        for (int ch = 0; ch < 3; ch++) {
            pre_intensity[ch] = menu.intensity[ch];
            pre_colorTemp[ch] = menu.colorTemp[ch];
            pre_rainbowHue[ch] = menu.rainbowHue[ch];
            pre_rainbowMode[ch] = menu.isRainbowMode[ch];
        }

        // --- VISUAL PULSE FROM HA ---
        // Only pulse if we are in the main menu and the user hasn't touched the device recently.
        // This prevents HA updates from stealing focus while the user is actively turning the dial.
        bool userActiveNow = (btnEvent != 0 || currentEncoder != lastRawEncoder);
        if (userActiveNow) {
            ha_update_pulse_time = 0; // Cancel pulse on any physical interaction
        }
        lastRawEncoder = currentEncoder;

        bool ha_pulse_active = (menu.currentState == MENU_SELECT_CHANNEL) &&
                               (ha_update_pulse_time > 0) &&
                               (now - ha_update_pulse_time < 5000);
        if (ha_pulse_active) {
            menu.lastActivityTime = now; // Prevent sleep while pulsed
        }
        if (ha_update_pulse_time > 0 && ((now - ha_update_pulse_time > 6000) || menu.currentState != MENU_SELECT_CHANNEL)) {
            ha_update_pulse_time = 0;
        }

        // Overview is active only while long-press is physically held (or while a short HA pulse is active).
        menu_system_set_overview(&menu, smart_button_is_held(&button) || ha_pulse_active);

        // --- ENCODER STEERING ---
        // Modified: EC21A1520401 is 15 pulses / 30 detents. Use /2 for 1 detent = 1 step.
        int steps = currentEncoder / 2;
        static int lastSteps = 0;
        int encoder_dir = 0;
        if (steps != lastSteps) {
            encoder_dir = (steps > lastSteps) ? 1 : -1;
            ha_update_pulse_time = 0; // Physical touch wins
            menu.showOverview = false; // Stop the override so encoder works
            lastSteps = steps;
        }

        menu_system_handle_encoder(&menu, currentEncoder);
        if (menu.currentChannel != pre_channel) {
            // Persist the selection so any reboot (not only deep sleep) comes back on the same channel
            nvsWritePending = true;
            lastNvsWriteTime = now;
        }

        if (suppress_next_single_click && now > suppress_single_click_until_ms) {
            suppress_next_single_click = false;
        }

        if (suppress_next_single_click && btnEvent == 1) {
            btnEvent = 0;
            suppress_next_single_click = false;
            suppress_single_click_until_ms = 0;
        }

        // Double/triple/... click inside a channel menu: report it to the controller for that channel.
        // Sent after the menu mutex is released (the Matter task may be waiting on it).
        int multi_press_count = 0;
        uint8_t multi_press_channel = menu.currentChannel;
        if (btnEvent >= 2 && menu.currentState != MENU_SELECT_CHANNEL) {
            multi_press_count = btnEvent;
        }

        if (btnEvent != 0) {
            menu_system_handle_button(&menu, btnEvent);

            // Mark NVS as needing save
            if (menu.currentState == IN_CHANNEL_INTENSITY ||
                menu.currentState == IN_CHANNEL_COLOR_TEMP ||
                menu.currentState == IN_CHANNEL_RAINBOW) {
                nvsWritePending = true;
                lastNvsWriteTime = now;
            }
        }

        bool sync_requested = false;
        for (int ch = 0; ch < 3; ch++) {
            if (pre_intensity[ch] != menu.intensity[ch]) {
                pending_intensity_sync[ch] = true;
                sync_requested = true;
                nvsWritePending = true;
                lastNvsWriteTime = now;
            }
            if (pre_colorTemp[ch] != menu.colorTemp[ch]) {
                pending_color_temp_sync[ch] = true;
                sync_requested = true;
                nvsWritePending = true;
                lastNvsWriteTime = now;
            }
            if (pre_rainbowHue[ch] != menu.rainbowHue[ch]) {
                pending_rainbow_hue_sync[ch] = true;
                sync_requested = true;
                nvsWritePending = true;
                lastNvsWriteTime = now;
            }
            if (pre_rainbowMode[ch] != menu.isRainbowMode[ch]) {
                pending_rainbow_mode_sync[ch] = true;
                sync_requested = true;
                nvsWritePending = true;
                lastNvsWriteTime = now;
            }
        }

        // If user keeps rotating down while already at 0, force an OFF re-publish.
        // This recovers from rare missed OFF publishes without requiring an up-then-down cycle.
        if (encoder_dir < 0 && menu.currentState == IN_CHANNEL_INTENSITY) {
            uint8_t ch = menu.currentChannel;
            if (ch < 3 && menu.intensity[ch] == 0) {
                pending_intensity_sync[ch] = true;
                sync_requested = true;
                nvsWritePending = true;
                lastNvsWriteTime = now;
            }
        }

        if (sync_requested) {
            request_matter_sync();
        }

        // If we update on every single raw encoder tick, we spam the RMT hardware,
        // which causes it to silently drop packets, missing the real detent updates.
        bool menuDataChanged = (pre_state != menu.currentState) ||
                               (pre_overview != menu.showOverview) ||
                               (pre_channel != menu.currentChannel) ||
                               (pre_intensity[0] != menu.intensity[0]) ||
                               (pre_intensity[1] != menu.intensity[1]) ||
                               (pre_intensity[2] != menu.intensity[2]) ||
                               (pre_colorTemp[0] != menu.colorTemp[0]) ||
                               (pre_colorTemp[1] != menu.colorTemp[1]) ||
                               (pre_colorTemp[2] != menu.colorTemp[2]) ||
                               (pre_rainbowHue[0] != menu.rainbowHue[0]) ||
                               (pre_rainbowHue[1] != menu.rainbowHue[1]) ||
                               (pre_rainbowHue[2] != menu.rainbowHue[2]) ||
                               (pre_rainbowMode[0] != menu.isRainbowMode[0]) ||
                               (pre_rainbowMode[1] != menu.isRainbowMode[1]) ||
                               (pre_rainbowMode[2] != menu.isRainbowMode[2]);

        // --- ACTIVITY DETECTION ---
        // Only trigger activity on REAL changes (clicks/button), ignoring continuous +/- 1 resting jitter
        // Continuous resting jitter prevents the 400ms Matter sync debounce from ever executing!
        if (btnEvent != 0 || smart_button_is_held(&button) || menuDataChanged) {
            menu.lastActivityTime = now;
        }

        // --- VÉRIFICATION DU TIMEOUT ---
        // Two-tier sleep: short inactivity -> automatic light sleep, 1h idle -> Deep Sleep.
        // Stay fully awake while unpaired so BLE commissioning isn't disturbed.
        // is_plugged is NOT used here: it is guessed from battery voltage (>= 4.2 V), and a nearly full
        // LiPo hovers around that threshold, so the remote randomly refused to sleep with LEDs on.
        // On a PC, CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION keeps it awake; on a charger, sleeping is fine.
        bool should_sleep = (matter_is_commissioned() &&
                             (now - menu.lastActivityTime > LIGHT_SLEEP_IDLE_MS));
        if (should_sleep) {
            xSemaphoreGive(menu_mutex);
            enter_smart_sleep();
            // Skip remaining loop work for this cycle after wake.
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (btnEvent != 0 || menuDataChanged) {
            menu_system_update_display(&menu);
        }

        xSemaphoreGive(menu_mutex);

        if (multi_press_count > 0) {
            ESP_LOGI(TAG, "CH%d multi-press x%d -> Matter", multi_press_channel, multi_press_count);
            if (click_feedback_task_handle) {
                xTaskNotify(click_feedback_task_handle, (uint32_t)multi_press_count, eSetValueWithOverwrite);
            }
            matter_send_channel_multi_press(multi_press_channel, (uint8_t)multi_press_count);
        }

        // NO BLOCKING OPERATIONS IN BUTTON_TASK!
        // Minimal yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1);  // 2ms is enough - button_task must stay responsive
    }
}

// ==================== NVS SAVE TASK (handles deferred NVS writes) ====================
static void nvs_save_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        uint32_t now = millis();

        // Only save if needed and 1+ second has passed since last change
        if (nvsWritePending && (now - lastNvsWriteTime > 1000)) {
            if (xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                menu_system_save_to_nvs(&menu);  // Blocking NVS operation
                nvsWritePending = false;
                xSemaphoreGive(menu_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));  // Low priority - long delay lets the chip light-sleep
    }
}

// ==================== WIFI REAPER TASK (Force-kills Wi-Fi if Matter restarts it) ====================
static void wifi_reaper_task(void *pvParameters)
{
    (void)pvParameters;
    // Wait for system to settle
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    while (1) {
        // ONLY REAP IF PERSONALITY IS THREAD (Value 2)
        // This ensures the suppression is reversible if switching back to Wi-Fi transport
        if (matter_get_personality_int() == 2) {
            wifi_mode_t mode;
            if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_NULL) {
                ESP_LOGW("REAPER", "Thread Personality active: Killing Wi-Fi for mesh stability...");
                esp_err_t err = matter_shutdown_wifi_radio();
                if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
                    ESP_LOGW("REAPER", "esp_wifi_deinit failed: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
    }
}

// ==================== MATTER ATTRIBUTE SYNC TASK (syncs state to HA) ====================
static void matter_sync_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t last_send_time = 0;
    bool was_transport_ready = false;
    uint32_t transport_ready_since = 0;

    while (1) {
        uint32_t now = millis();

        bool transport_ready = matter_is_ready() && matter_is_commissioned() && matter_is_transport_connected();

        // No full republish on reconnect: it overwrote changes made from Home Assistant while the remote was
        // away. Local changes made while offline stay pending (they are only cleared once sent) and go out now.
        if (transport_ready && !was_transport_ready) {
            transport_ready_since = now;
        } else if (!transport_ready) {
            transport_ready_since = 0;
        }
        was_transport_ready = transport_ready;

        // Give the transport a short settle time after reconnect before publishing.
        bool publish_ready = transport_ready && (transport_ready_since > 0) && (now - transport_ready_since > 1200);

        bool has_pending = false;

        // Rate-limited: the first change goes out immediately, further dial ticks within
        // SYNC_MIN_GAP_MS are coalesced into a single publish of the latest value.
        // Matter's reliable messaging (MRP) already retransmits, so each value is sent once.
        if (publish_ready && (now - last_send_time >= SYNC_MIN_GAP_MS) &&
            xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            enum { SYNC_NONE = 0, SYNC_INTENSITY, SYNC_COLOR_TEMP, SYNC_RAINBOW_HUE } action = SYNC_NONE;
            uint8_t action_channel = 0;
            uint8_t value = 0;

            for (int ch = 0; ch < 3 && action == SYNC_NONE; ch++) {
                bool color_dirty = pending_color_temp_sync[ch] || pending_rainbow_hue_sync[ch] || pending_rainbow_mode_sync[ch];

                if (pending_intensity_sync[ch]) {
                    action = SYNC_INTENSITY;
                    value = menu.intensity[ch];
                    pending_intensity_sync[ch] = false;
                } else if (color_dirty) {
                    if (menu.isRainbowMode[ch]) {
                        action = SYNC_RAINBOW_HUE;
                        value = menu.rainbowHue[ch];
                    } else {
                        action = SYNC_COLOR_TEMP;
                        value = menu.colorTemp[ch];
                    }
                    pending_color_temp_sync[ch] = false;
                    pending_rainbow_hue_sync[ch] = false;
                    pending_rainbow_mode_sync[ch] = false;
                }
                action_channel = (uint8_t)ch;
            }

            for (int ch = 0; ch < 3; ch++) {
                if (pending_intensity_sync[ch] || pending_color_temp_sync[ch] ||
                    pending_rainbow_hue_sync[ch] || pending_rainbow_mode_sync[ch]) {
                    has_pending = true;
                }
            }
            xSemaphoreGive(menu_mutex);

            // Publish outside the menu mutex so the dial stays responsive.
            if (action == SYNC_INTENSITY) {
                ESP_LOGI(TAG, "SYNC CH%d INTENSITY: %d", action_channel, value);
                matter_update_intensity(action_channel, value);
            } else if (action == SYNC_COLOR_TEMP) {
                ESP_LOGI(TAG, "SYNC CH%d COLOR_TEMP: %d", action_channel, value);
                matter_update_color_temp(action_channel, value);
            } else if (action == SYNC_RAINBOW_HUE) {
                ESP_LOGI(TAG, "SYNC CH%d RAINBOW_HUE: %d", action_channel, value);
                matter_update_rainbow_hue(action_channel, value);
            }
            if (action != SYNC_NONE) {
                last_send_time = now;
            }
        } else {
            // Inside the rate-limit window: re-check soon so coalesced changes go out on time.
            has_pending = publish_ready;
        }

        // Block long when there is nothing to send so the chip can light-sleep;
        // button_task notifies us on every local change.
        bool busy = has_pending || (transport_ready && !publish_ready);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? 50 : 2000));
    }
}

// ==================== BATTERY MONITOR TASK (reads battery periodically) ====================
static void battery_task(void *pvParameters)
{
    (void)pvParameters;
    static uint32_t lastBatteryCheck = 0;

    while (1) {
        uint32_t now = millis();

        if (now - lastBatteryCheck > BATTERY_READ_INTERVAL_MS) {
            update_battery_percent();  // Blocking ADC read
            lastBatteryCheck = now;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // Low priority - long delay lets the chip light-sleep
    }
}

static void matter_ui_update_intensity(uint8_t ch, uint8_t val) { 
    if (ch < 3 && xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        menu.intensity[ch] = val; 
        request_ha_pulse();
        xSemaphoreGive(menu_mutex);
    }
}
static void matter_ui_update_colortemp(uint8_t ch, uint8_t val) { 
    if (ch < 3 && xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        menu.colorTemp[ch] = val; 
        request_ha_pulse();
        xSemaphoreGive(menu_mutex);
    }
}
static void matter_ui_update_rainbowhue(uint8_t ch, uint8_t hue) { 
    if (ch < 3 && xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        menu.rainbowHue[ch] = hue; 
        request_ha_pulse();
        xSemaphoreGive(menu_mutex);
    }
}
static void matter_ui_update_rainbowmode(uint8_t ch, bool enabled) { 
    if (ch < 3 && xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        menu.isRainbowMode[ch] = enabled; 
        request_ha_pulse();
        xSemaphoreGive(menu_mutex);
    }
}
static void matter_ui_update_display(void) {
    if (xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        menu_system_update_display(&menu);
        xSemaphoreGive(menu_mutex);
    }
}

void app_main(void)
{
    ui_feedback_enable_time_ms = millis() + 8000;

    // 1. Initialize hardware COMPLETELY before any tasks start
    gpio_init_all();
    led_init();

    // Create LED mutex for task synchronization
    led_mutex = xSemaphoreCreateMutex();
    if (!led_mutex) {
        ESP_LOGE(TAG, "Failed to create LED mutex");
    }

    menu_mutex = xSemaphoreCreateMutex();
    if (!menu_mutex) {
        ESP_LOGE(TAG, "Failed to create menu mutex");
        return;
    }

    // Automatic light sleep: the radio drivers (Thread poll / Wi-Fi DTIM) schedule their own
    // wake-ups, so the link survives sleep. ui_awake_lock keeps the CPU up while the user is active.
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(pm_err));
    }
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui_awake", &ui_awake_lock));
    esp_pm_lock_acquire(ui_awake_lock);

    // 2. Ready storage
    nvs_init_and_load();

    // 3. Initialize remaining peripherals before task creation
    adc_init();

    // Link Matter attribute updates to our physical menu system
    MenuSystemInterface menu_iface = {
        .updateIntensity = matter_ui_update_intensity,
        .updateColorTemp = matter_ui_update_colortemp,
        .updateRainbowHue = matter_ui_update_rainbowhue,
        .updateRainbowMode = matter_ui_update_rainbowmode,
        .updateDisplay = matter_ui_update_display
    };
    matter_set_menu_system_interface(&menu_iface);

    // 3.5 Initialize menu struct BEFORE matter_init to prevent race conditions during HA interview
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    uint64_t wakeup_status = 0;
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        wakeup_status = esp_sleep_get_ext1_wakeup_status();
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        wakeup_status = esp_sleep_get_gpio_wakeup_status();
    }

    // Any boot after deep sleep is a wake-up, even if the wake cause/status comes back unclear: otherwise the
    // remote booted like a power-on (main screen, channel 1) and the first dial step down wrapped to channel 3.
    bool woke_from_deep_sleep = (esp_reset_reason() == ESP_RST_DEEPSLEEP) ||
                                wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 || wakeup_reason == ESP_SLEEP_WAKEUP_GPIO;
    if (woke_from_deep_sleep) {
        // Dial (pin A or B), or an unclear status: open the channel without toggling it
        if ((wakeup_status & ((1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B))) || !(wakeup_status & (1ULL << PIN_BUTTON))) {
            menu.currentState = IN_CHANNEL_INTENSITY; // Dial wakeup -> Direct to Dimming
            menu.currentChannel = rtc_currentChannel;
            ESP_LOGI(TAG, "Wakeup from DIAL: jumping to channel %d intensity", menu.currentChannel);
        } else {
            menu.currentState = IN_CHANNEL_INTENSITY;
            menu.currentChannel = rtc_currentChannel;
            pending_button_wake_action = true;
            ESP_LOGI(TAG, "Wakeup from BUTTON: toggle channel %d and open intensity", menu.currentChannel);
        }
    } else {
        // Channel is restored from NVS in menu_system_init(); 0 is only the first-boot default
        menu.currentState = MENU_SELECT_CHANNEL;
        menu.currentChannel = 0;
        ESP_LOGI(TAG, "Normal boot: starting on channel select");
    }

    menu.lastEncoderVal = 0;
    menu.showOverview = false;
    menu.lastActivityTime = millis();  // Set to current time so display_task knows device is active
    for (int i = 0; i < 3; i++) {
        menu.intensity[i] = 50;
        menu.colorTemp[i] = 50;
        menu.rainbowHue[i] = 0;
        menu.isRainbowMode[i] = false;
    }

    matter_init();
    
    // Bridge the C menu system to the Matter C++ callbacks
    void menu_system_bridge_init(menu_system_t *actualMenu);
    void matter_init_menu_system(void);
    menu_system_bridge_init(&menu);
    matter_init_menu_system();

    // 4. NOW create tasks (after all initialization is complete)
    xTaskCreate(matter_sync_task, "matter_sync", 3072, NULL, 3, &matter_sync_task_handle); // MED priority - matter sync
    xTaskCreate(click_feedback_task, "click_fb", 2048, NULL, 4, &click_feedback_task_handle); // multi-click LED blink
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, &button_task_handle);           // HIGH priority - input
    xTaskCreate(nvs_save_task, "nvs_task", 2048, NULL, 2, NULL);       // LOW priority - NVS save
    xTaskCreate(battery_task, "battery_task", 2048, NULL, 2, NULL);    // LOW priority - battery

    // Encoder is now purely interrupt-driven!

    ESP_LOGI(TAG, "=== ESP32-C6 MATTER SMART DIAL (Encoder + 3 LEDs) ===");

    // Perform immediate battery reading
    update_battery_percent();
    
    if (matter_get_personality_int() == 2) {
        // In Thread personality, disable Wi-Fi to reduce radio contention.
        ESP_LOGI(TAG, "Thread personality detected: de-initializing Wi-Fi radio");

        int retries = 5;
        while (retries-- > 0) {
            esp_err_t err = matter_shutdown_wifi_radio();
            if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGI(TAG, "Wi-Fi de-initialized.");
                break;
            }

            ESP_LOGE(TAG, "Wi-Fi deinit failed (error 0x%x), retrying...", err);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // Keep a guard task in case Wi-Fi is re-enabled while in Thread personality.
        xTaskCreate(wifi_reaper_task, "wifi_reaper", 2048, NULL, 1, NULL);
    } else {
        ESP_LOGI(TAG, "Wi-Fi personality detected: leaving Wi-Fi stack managed by Matter");
    }

    if (!matter_is_commissioned())
    {
        ESP_LOGW(TAG, "Not commissioned - waiting for pairing");
        if (useLed)
            showColor(0x000032, 500);
    }
    else
    {
        ESP_LOGI(TAG, "Device commissioned");
    }

    // Initial menu display after boot flashes
    if (xSemaphoreTake(menu_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        menu.lastActivityTime = millis();
        menu_system_update_display(&menu);
        xSemaphoreGive(menu_mutex);
    }
}
