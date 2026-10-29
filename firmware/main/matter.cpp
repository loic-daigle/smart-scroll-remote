/*
 * ESP-Matter integration for Smart Button with Runtime Transport Selection (Personality)
 */

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <app/server/Server.h>
#include <app/server/Dnssd.h>

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_matter_providers.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ThreadStackManager.h>
#endif
#include <app/clusters/color-control-server/color-control-server.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/ClusterCallbacks.h>
#include <lib/support/TypeTraits.h>
#include <new>
#include <string.h>
#include "CHIPProjectConfig.h"
#include <esp_matter_client.h>
#include <esp_wifi.h>
#include <platform/ESP32/DiagnosticDataProviderImpl.h>
#if CHIP_CONFIG_ENABLE_ICD_SERVER && CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <app/icd/server/ICDStateObserver.h>
#include <app/icd/server/ICDConfigurationData.h>
#include <app/clusters/icd-management-server/ICDManagementCluster.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/link.h>
#endif

#include "matter.h"

static const char *TAG = "matter";

namespace {
using namespace chip::DeviceLayer;

enum matter_personality_t {
    PERSONALITY_UNSET = 0,
    PERSONALITY_WIFI,
    PERSONALITY_THREAD
};

static matter_personality_t current_personality = PERSONALITY_THREAD;
static nvs_handle_t matter_nvs_handle = 0;
static bool matter_stack_ready = false;

static void ensure_nvs_open()
{
    if (matter_nvs_handle == 0) {
        esp_err_t err = nvs_open("matter_cfg", NVS_READWRITE, &matter_nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open NVS namespace 'matter_cfg': %s (0x%x)", esp_err_to_name(err), err);
        }
    }
}

static matter_personality_t get_personality()
{
    ensure_nvs_open();
    if (matter_nvs_handle == 0) {
        ESP_LOGE(TAG, "NVS handle is null in get_personality");
        return PERSONALITY_UNSET;
    }

    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(matter_nvs_handle, "mtr_personality", &val);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Read personality from NVS: %d", (int)val);
        return (matter_personality_t)val;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Personality key not found in NVS");
    } else {
        ESP_LOGE(TAG, "Failed to read personality from NVS: %s", esp_err_to_name(err));
    }
    return PERSONALITY_UNSET;
}

static void set_personality(matter_personality_t personality)
{
    ensure_nvs_open();
    if (matter_nvs_handle == 0) return;

    esp_err_t err = nvs_set_u8(matter_nvs_handle, "mtr_personality", (uint8_t)personality);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set personality in NVS: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_commit(matter_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Personality successfully saved to NVS: %d", (int)personality);
    }
}

static esp_err_t personality_handler(int argc, char **argv)
{
    if (argc < 1) {
        ESP_LOGI(TAG, "Usage: matter esp personality <wifi|thread>");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(argv[0], "wifi") == 0) set_personality(PERSONALITY_WIFI);
    else if (strcmp(argv[0], "thread") == 0) set_personality(PERSONALITY_THREAD);
    else return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Personality set to %s. Rebooting...", argv[0]);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void register_personality_command()
{
    static const esp_matter::console::command_t command = {
        .name = "personality",
        .description = "Switch between wifi and thread transport (requires reboot)",
        .handler = personality_handler,
    };
    esp_matter::console::add_commands(&command, 1);
}

static esp_err_t factory_reset_handler(int argc, char **argv)
{
    ESP_LOGI(TAG, "Performing factory reset of Matter data...");
    chip::Server::GetInstance().GetFabricTable().DeleteAllFabrics();
    ESP_LOGI(TAG, "Factory reset complete. Reopening commissioning window...");
    chip::CommissioningWindowManager & commMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    CHIP_ERROR openErr = commMgr.OpenBasicCommissioningWindow(
        commMgr.MaxCommissioningTimeout(),
        chip::CommissioningWindowAdvertisement::kAllSupported
    );
    if (openErr == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Commissioning window reopened successfully");
    } else {
        ESP_LOGE(TAG, "Failed to reopen commissioning window: %" CHIP_ERROR_FORMAT, openErr.Format());
    }
    return ESP_OK;
}

static void register_factory_reset_command()
{
    static const esp_matter::console::command_t command = {
        .name = "factory_reset",
        .description = "Reset Matter fabric table and reopen commissioning window",
        .handler = factory_reset_handler,
    };
    esp_matter::console::add_commands(&command, 1);
}

static void apply_transport_personality()
{
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    if (current_personality == PERSONALITY_WIFI) {
        ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Enabled);
        ThreadStackMgr().SetThreadEnabled(false);
    } else {
        ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Disabled);
        ThreadStackMgr().SetThreadEnabled(true);
    }
#endif
}

class AppDeviceInstanceInfoProvider : public DeviceInstanceInfoProvider
{
public:
    AppDeviceInstanceInfoProvider() {}
    CHIP_ERROR GetVendorName(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "Loic Daigle"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetVendorId(uint16_t & vendorId) override { vendorId = 0xFFF1; return CHIP_NO_ERROR; }
    CHIP_ERROR GetProductName(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "Smart Scroll Remote"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetProductId(uint16_t & productId) override { productId = 0x8001; return CHIP_NO_ERROR; }
    CHIP_ERROR GetPartNumber(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "SSR-001"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetProductURL(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "https://github.com/loic-daigle/smart-scroll-remote"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetProductLabel(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "Smart Scroll Remote"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetSerialNumber(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "SN12345678"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetManufacturingDate(uint16_t & year, uint8_t & month, uint8_t & day) override { year = 2026; month = 4; day = 6; return CHIP_NO_ERROR; }
    CHIP_ERROR GetHardwareVersion(uint16_t & hardwareVersion) override { hardwareVersion = 1; return CHIP_NO_ERROR; }
    CHIP_ERROR GetHardwareVersionString(char * buf, size_t bufSize) override { chip::Platform::CopyString(buf, bufSize, "v1.0"); return CHIP_NO_ERROR; }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan & uniqueIdSpan) override {
        uint8_t uniqueId[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10 };
        return chip::CopySpanToMutableSpan(chip::ByteSpan(uniqueId), uniqueIdSpan);
    }
};

static AppDeviceInstanceInfoProvider gAppDeviceInstanceInfoProvider;

// The ESP32 provider reports every esp_netif as operational, including the idle transport's interface in
// this dual-stack build. Controllers (Home Assistant takes the first operational entry of
// GeneralDiagnostics.NetworkInterfaces) then show a Thread remote as Wi-Fi. Mark the unused transport's
// interfaces as not operational.
class PersonalityDiagnosticDataProvider : public DiagnosticDataProviderImpl
{
public:
    CHIP_ERROR GetNetworkInterfaces(NetworkInterface ** netifpp) override
    {
        CHIP_ERROR err = DiagnosticDataProviderImpl::GetNetworkInterfaces(netifpp);
        if (err != CHIP_NO_ERROR) return err;

        using chip::app::Clusters::GeneralDiagnostics::InterfaceTypeEnum;
        InterfaceTypeEnum unused = (current_personality == PERSONALITY_WIFI) ? InterfaceTypeEnum::kThread
                                                                             : InterfaceTypeEnum::kWiFi;
        for (NetworkInterface *ifp = *netifpp; ifp != nullptr; ifp = ifp->Next) {
            if (ifp->type == unused) {
                ifp->isOperational = false;
            }
        }
        return CHIP_NO_ERROR;
    }
};

static PersonalityDiagnosticDataProvider gDiagnosticDataProvider;

#if CHIP_CONFIG_ENABLE_ICD_SERVER && CHIP_DEVICE_CONFIG_ENABLE_THREAD
// Dual-stack workaround: the ESP32 ConnectivityManager's Wi-Fi _SetPollingInterval (always NOT_IMPLEMENTED)
// hides the Thread one, so ICDManager never applies the Thread poll period ("Failed to set Fast Polling
// Interval") and OpenThread falls back to a poll period derived from the child timeout (minutes).
// Replies and MRP acks then sit at the parent router. Apply the ICD poll intervals to Thread ourselves.
//
// The ICDManager observer pool (CHIP_CONFIG_ICD_OBSERVERS_POOL_SIZE = 3) is already full with Matter's own
// observers, so this one takes the ICD Management cluster's slot and forwards every event to it.
class ThreadPollingFixup : public chip::app::ICDStateObserver
{
public:
    chip::app::ICDStateObserver *mInner = nullptr;

    void OnEnterActiveMode() override
    {
        Apply(chip::ICDConfigurationData::GetInstance().GetFastPollingInterval());
        if (mInner) mInner->OnEnterActiveMode();
    }
    void OnEnterIdleMode() override
    {
        Apply(chip::ICDConfigurationData::GetInstance().GetSlowPollingInterval());
        if (mInner) mInner->OnEnterIdleMode();
    }
    void OnTransitionToIdle() override { if (mInner) mInner->OnTransitionToIdle(); }
    void OnICDModeChange() override { if (mInner) mInner->OnICDModeChange(); }

    static void Apply(chip::System::Clock::Milliseconds32 interval)
    {
        if (current_personality != PERSONALITY_THREAD) return;
        // Same as GenericThreadStackManagerImpl_OpenThread::_SetPollingInterval (which is private).
        otInstance *instance = esp_openthread_get_instance();
        if (!instance) return;
        esp_openthread_lock_acquire(portMAX_DELAY);
        otError otErr = otLinkSetPollPeriod(instance, interval.count());
        esp_openthread_lock_release();
        if (otErr != OT_ERROR_NONE) {
            ESP_LOGW(TAG, "Thread poll period %" PRIu32 " ms not applied: otError %d", interval.count(), (int)otErr);
        }
    }
};

static ThreadPollingFixup gThreadPollingFixup;
#endif
} // namespace

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t switch_endpoint_id = 0;
static uint16_t light_endpoint_ids[3] = {0};
static uint16_t channel_switch_endpoint_ids[3] = {0};
static uint16_t channel_type_endpoint_ids[3] = {0};
static uint16_t dial_up_endpoint_ids[3] = {0};
static uint16_t dial_down_endpoint_ids[3] = {0};
static uint16_t deep_sleep_endpoint_id = 0;

// Default stub if not provided
static void stub_updateIntensity(uint8_t ch, uint8_t val) {}
static void stub_updateColorTemp(uint8_t ch, uint8_t val) {}
static void stub_updateRainbowHue(uint8_t ch, uint8_t hue) {}
static void stub_updateRainbowMode(uint8_t ch, bool enabled) {}
static void stub_updateDisplay(void) {}

static MenuSystemInterface menusys = {
    .updateIntensity = stub_updateIntensity,
    .updateColorTemp = stub_updateColorTemp,
    .updateRainbowHue = stub_updateRainbowHue,
    .updateRainbowMode = stub_updateRainbowMode,
    .updateDisplay = stub_updateDisplay
};

// 3 channels (0=Channel 1, 1=Channel 2, 2=Channel 3)
struct {
    uint16_t hue;           // 0-254 for Matter (0-255 internally)
    uint8_t saturation;     // 0-254 (254=100%)
    uint8_t brightness;     // 0-254 (Level Control)
    uint8_t colorTemp;      // 0-100 (stored as Mireds internally: 153=6500K cold, 500=2000K warm)
    bool rainbowMode;       // True if in color mode, False if in temp mode
} light_config[3] = {
    {120, 254, 127, 50, false},
    {240, 254, 127, 50, false},
    {60, 254, 127, 50, false},
};

// ============ Settings shown as selects in the controller (Mode Select endpoints) ============
// Channel type: one endpoint per channel, mode values are the channel_type_t values.
static uint8_t channel_types[3] = {CHANNEL_TYPE_COLOR, CHANNEL_TYPE_COLOR, CHANNEL_TYPE_COLOR};
static bool channel_types_loaded = false;
// On/Off channels: the light's brightness to report when switched on from the remote
static uint8_t on_off_level[3] = {254, 254, 254};

static void load_channel_types()
{
    if (channel_types_loaded) return;
    ensure_nvs_open();
    for (int i = 0; i < 3 && matter_nvs_handle != 0; i++) {
        char key[12];
        snprintf(key, sizeof(key), "ch_type%d", i);
        uint8_t v = 0;
        if (nvs_get_u8(matter_nvs_handle, key, &v) == ESP_OK && v < CHANNEL_TYPE_COUNT) {
            channel_types[i] = v;
        }
    }
    channel_types_loaded = true;
}

static void save_channel_type(int i)
{
    if (matter_nvs_handle == 0) return;
    char key[12];
    snprintf(key, sizeof(key), "ch_type%d", i);
    esp_err_t err = nvs_set_u8(matter_nvs_handle, key, channel_types[i]);
    if (err == ESP_OK) err = nvs_commit(matter_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist channel %d type: %s", i + 1, esp_err_to_name(err));
    }
}

extern "C" channel_type_t matter_get_channel_type(uint8_t channel)
{
    load_channel_types();
    return (channel < 3) ? (channel_type_t)channel_types[channel] : CHANNEL_TYPE_COLOR;
}

// Deep sleep: after this much inactivity the remote leaves the network and deep-sleeps (it is then
// unavailable in the controller until the dial or button wakes it). Mode value = index in this table.
static const uint32_t k_deep_sleep_ms[] = {10 * 60000, 30 * 60000, 60 * 60000, 4 * 3600000, 12 * 3600000, 0};
static const char *k_deep_sleep_labels[] = {"10 min", "30 min", "1 h", "4 h", "12 h", "Never"};
static constexpr uint8_t k_deep_sleep_count = sizeof(k_deep_sleep_ms) / sizeof(k_deep_sleep_ms[0]);
static uint8_t deep_sleep_mode = 2;  // 1 h

static void load_deep_sleep_mode()
{
    uint8_t v = 0;
    if (matter_nvs_handle != 0 && nvs_get_u8(matter_nvs_handle, "deep_sleep", &v) == ESP_OK && v < k_deep_sleep_count) {
        deep_sleep_mode = v;
    }
}

static void save_deep_sleep_mode()
{
    if (matter_nvs_handle == 0) return;
    esp_err_t err = nvs_set_u8(matter_nvs_handle, "deep_sleep", deep_sleep_mode);
    if (err == ESP_OK) err = nvs_commit(matter_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist deep sleep setting: %s", esp_err_to_name(err));
    }
}

extern "C" uint32_t matter_get_deep_sleep_ms(void) { return k_deep_sleep_ms[deep_sleep_mode]; }

// Options of every select. The Mode Select server keeps a single manager for all endpoints.
class SettingsModesManager : public ModeSelect::SupportedModesManager
{
    using ModeOption = ModeSelect::Structs::ModeOptionStruct::Type;

public:
    SettingsModesManager()
    {
        static const char *labels[CHANNEL_TYPE_COUNT] = {"On/Off", "Dimmable", "Dimmable + white", "Dimmable + colour",
                                                         "Dial events (no light)"};
        for (uint8_t m = 0; m < CHANNEL_TYPE_COUNT; m++) {
            mChannelType[m].label = chip::CharSpan::fromCharString(labels[m]);
            mChannelType[m].mode = m;
        }
        for (uint8_t m = 0; m < k_deep_sleep_count; m++) {
            mDeepSleep[m].label = chip::CharSpan::fromCharString(k_deep_sleep_labels[m]);
            mDeepSleep[m].mode = m;
        }
    }

    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpointId) const override
    {
        if (endpointId == deep_sleep_endpoint_id) return ModeOptionsProvider(mDeepSleep, mDeepSleep + k_deep_sleep_count);
        return ModeOptionsProvider(mChannelType, mChannelType + CHANNEL_TYPE_COUNT);
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(chip::EndpointId endpointId, uint8_t mode,
                                                                  const ModeOption **dataPtr) const override
    {
        ModeOptionsProvider options = getModeOptionsProvider(endpointId);
        if (mode >= options.end() - options.begin()) return chip::Protocols::InteractionModel::Status::InvalidCommand;
        *dataPtr = options.begin() + mode;
        return chip::Protocols::InteractionModel::Status::Success;
    }

private:
    ModeOption mChannelType[CHANNEL_TYPE_COUNT];
    ModeOption mDeepSleep[k_deep_sleep_count];
};

static SettingsModesManager gSettingsModesManager;

static bool is_commissioned_cached = false;
static bool commissioned_cached_valid = false;
static bool thread_connected = false;
static bool wifi_connected = false;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
        thread_connected = (event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established);
        if (thread_connected) {
            ESP_LOGI(TAG, "Thread Connectivity Established");
            // Auto-save THREAD personality when Thread network connects
            if (current_personality != PERSONALITY_THREAD) {
                ESP_LOGI(TAG, "Auto-saving THREAD personality after successful Thread connection");
                current_personality = PERSONALITY_THREAD;
                set_personality(PERSONALITY_THREAD);
            }
        } else {
            ESP_LOGW(TAG, "Thread Connectivity Lost/Not Ready");
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        wifi_connected = (event->WiFiConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established);
        if (wifi_connected) {
            ESP_LOGI(TAG, "WiFi Connectivity Established");
            // Only persist Wi-Fi personality if Wi-Fi is the selected transport.
            // In Thread personality, a transient Wi-Fi bring-up should not reclassify the device.
            if (current_personality == PERSONALITY_WIFI) {
                ESP_LOGI(TAG, "Auto-saving WIFI personality after successful WiFi connection");
                set_personality(PERSONALITY_WIFI);
            }
        } else {
            ESP_LOGW(TAG, "WiFi Connectivity Lost/Not Ready");
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete: 
        ESP_LOGI(TAG, "Commissioning complete"); 
        is_commissioned_cached = true;
        commissioned_cached_valid = true;
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        // Or if factory reset removes it
        commissioned_cached_valid = false;
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged: 
        ESP_LOGI(TAG, "Network IP Changed"); 
        break;
    default: break;
    }
}

// ============ Controller -> remote attribute handling ============
// Exact round-trip conversions (rounded both ways): an echo of a value the remote published converts back to
// exactly the same %, so it can be recognised as "already applied" by comparing with light_config.
static inline uint8_t pct_to_level(uint8_t pct) { return (uint8_t)(1 + ((uint32_t)pct * 253 + 50) / 100); }
static inline uint8_t level_to_pct(uint8_t level)
{
    uint8_t pct = (uint8_t)(((uint32_t)(level > 0 ? level - 1 : 0) * 100 + 126) / 253);
    return pct > 100 ? 100 : pct;
}
static inline uint16_t pct_to_mireds(uint8_t pct) { return (uint16_t)(153 + ((uint32_t)pct * 347 + 50) / 100); }
static inline uint8_t mireds_to_pct(uint16_t mireds)
{
    if (mireds < 153) mireds = 153;
    if (mireds > 500) mireds = 500;
    return (uint8_t)(((uint32_t)(mireds - 153) * 100 + 173) / 347);
}

static bool catchup_pending[3] = {false, false, false};
static bool catchup_timer_running = false;
static void catchup_timer_cb(chip::System::Layer *layer, void *ctx);

// Applies one light-endpoint attribute to the menu. light_config holds what the remote last published or
// applied, so echoes of our own changes compare equal and are ignored. While the user is touching the channel
// (locked out) a real change is not applied and light_config is left untouched; returns true so that a
// catch-up re-reads the remote's own attributes once the user stops (otherwise the LEDs could stay stale).
static bool apply_light_attribute(int i, uint32_t cluster_id, uint32_t attribute_id, const esp_matter_attr_val_t *val)
{
    bool locked_out = (menusys.isLockedOut && menusys.isLockedOut(i));
    // Values the channel type doesn't use are ignored (the endpoint still accepts them)
    channel_type_t type = (channel_type_t)channel_types[i];
    if (type == CHANNEL_TYPE_EVENTS) return false;  // no light on this channel: nothing to show
    bool has_level = (type != CHANNEL_TYPE_ON_OFF);
    bool has_ct = (type == CHANNEL_TYPE_WHITE || type == CHANNEL_TYPE_COLOR);
    bool has_hue = (type == CHANNEL_TYPE_COLOR);

    if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        uint8_t level = val->val.u8;
        if (!has_level) {
            // Remember the light's brightness (synced from the controller while on) to restore it on "on".
            // Level 1 is skipped: Matter drops the level to the minimum when the controller sends Off.
            if (light_config[i].brightness > 0 && level > 1 && level != 0xFF) on_off_level[i] = level;
            return false;
        }
        // While off (brightness 0) the level is kept and picked up when the channel turns on
        if (light_config[i].brightness == 0 || level == 0 || level == light_config[i].brightness) return false;
        if (locked_out) return true;
        light_config[i].brightness = level;
        menusys.updateIntensity(i, level_to_pct(level));
    } else if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        bool is_on = val->val.b;
        bool current_on = (light_config[i].brightness > 0);
        if (is_on == current_on) return false;
        if (locked_out) return true;
        if (!is_on) {
            light_config[i].brightness = 0;
            menusys.updateIntensity(i, 0);
        } else {
            // Turned on by the controller: use the level stored on the endpoint (it may have been set while off)
            esp_matter_attr_val_t lv = esp_matter_invalid(NULL);
            uint8_t level = 0;
            if (!has_level) {
                level = pct_to_level(100);
            } else if (attribute::get_val(light_endpoint_ids[i], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &lv) == ESP_OK) {
                level = lv.val.u8;
            }
            if (level == 0 || level == 0xFF) level = pct_to_level(50);
            light_config[i].brightness = level;
            menusys.updateIntensity(i, level_to_pct(level));
        }
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentHue::Id) {
        if (!has_hue) return false;
        uint8_t matterHue = val->val.u8;
        if (matterHue == light_config[i].hue && light_config[i].rainbowMode) return false;
        if (locked_out) return true;
        light_config[i].hue = matterHue;
        light_config[i].rainbowMode = true;
        menusys.updateRainbowHue(i, (uint8_t)(((uint16_t)matterHue * 255) / 254));  // Matter 0-254 -> internal 0-255
        menusys.updateRainbowMode(i, true);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
        light_config[i].saturation = val->val.u8;
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
        if (!has_ct) return false;
        uint8_t colorTemp = mireds_to_pct(val->val.u16);
        if (colorTemp == light_config[i].colorTemp && !light_config[i].rainbowMode) return false;
        if (locked_out) return true;
        light_config[i].colorTemp = colorTemp;
        light_config[i].rainbowMode = false;
        menusys.updateColorTemp(i, colorTemp);
        menusys.updateRainbowMode(i, false);
    }
    return false;
}

static void schedule_catchup(int i)
{
    catchup_pending[i] = true;
    if (!catchup_timer_running) {
        catchup_timer_running = true;
        chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(1600), catchup_timer_cb, nullptr);
    }
}

// Runs on the Matter task: re-reads this remote's OWN light attributes (no request to any light) for channels
// whose controller reports were skipped, and applies whatever is still different.
static void catchup_timer_cb(chip::System::Layer *layer, void *ctx)
{
    catchup_timer_running = false;
    for (int i = 0; i < 3; i++) {
        if (!catchup_pending[i] || light_endpoint_ids[i] == 0) continue;
        if (menusys.isLockedOut && menusys.isLockedOut(i)) {
            schedule_catchup(i);  // still in use: try again later
            continue;
        }
        catchup_pending[i] = false;

        esp_matter_attr_val_t v = esp_matter_invalid(NULL);
        if (attribute::get_val(light_endpoint_ids[i], OnOff::Id, OnOff::Attributes::OnOff::Id, &v) == ESP_OK) {
            apply_light_attribute(i, OnOff::Id, OnOff::Attributes::OnOff::Id, &v);
        }
        if (attribute::get_val(light_endpoint_ids[i], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &v) == ESP_OK) {
            apply_light_attribute(i, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, &v);
        }
        // ColorMode: 0 = hue/saturation, 1 = XY, 2 = colour temperature
        uint8_t color_mode = 2;
        if (attribute::get_val(light_endpoint_ids[i], ColorControl::Id, ColorControl::Attributes::ColorMode::Id, &v) == ESP_OK) {
            color_mode = v.val.u8;
        }
        uint32_t color_attr = (color_mode == 2) ? ColorControl::Attributes::ColorTemperatureMireds::Id
                                                : ColorControl::Attributes::CurrentHue::Id;
        if (attribute::get_val(light_endpoint_ids[i], ColorControl::Id, color_attr, &v) == ESP_OK) {
            apply_light_attribute(i, ColorControl::Id, color_attr, &v);
        }
    }
}

static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != POST_UPDATE) return ESP_OK;
    if (!val) return ESP_ERR_INVALID_ARG;

    if (endpoint_id == deep_sleep_endpoint_id && cluster_id == ModeSelect::Id &&
        attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        // Takes effect the next time the remote goes idle
        if (val->val.u8 < k_deep_sleep_count && val->val.u8 != deep_sleep_mode) {
            deep_sleep_mode = val->val.u8;
            save_deep_sleep_mode();
            ESP_LOGI(TAG, "Deep sleep after: %s", k_deep_sleep_labels[deep_sleep_mode]);
        }
        return ESP_OK;
    }

    // Handle light endpoints (3 channels: 1, 2, 3 mapping to indices 0, 1, 2)
    for (int i = 0; i < 3; i++) {
        if (endpoint_id == channel_type_endpoint_ids[i] && cluster_id == ModeSelect::Id &&
            attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
            if (val->val.u8 < CHANNEL_TYPE_COUNT && val->val.u8 != channel_types[i]) {
                channel_types[i] = val->val.u8;
                if (channel_types[i] != CHANNEL_TYPE_COLOR) light_config[i].rainbowMode = false;
                save_channel_type(i);
                ESP_LOGI(TAG, "Channel %d type -> %u", i + 1, (unsigned)channel_types[i]);
                if (menusys.channelTypeChanged) menusys.channelTypeChanged(i);
            }
            break;
        }
        if (endpoint_id == light_endpoint_ids[i]) {
            if (apply_light_attribute(i, cluster_id, attribute_id, val)) {
                schedule_catchup(i);
            }
            break;
        }
    }

    return ESP_OK;
}

// ------------------- MATTER CLIENT CALLBACK HANDLERS -------------------
// These handle the actual sending of commands when BindingManager triggers an update.

// Builds the command payload for a bound light. OptionsMask/OptionsOverride are mandatory fields of the
// Level/Color commands: spec-strict lights reject the command (INVALID_COMMAND) when they are missing.
static const char *build_command_payload(const esp_matter::client::request_handle_t *req, char *buf, size_t len)
{
    uint32_t data = (uint32_t)(uintptr_t)req->request_data;
    if (req->command_path.mClusterId == OnOff::Id) {
        return "{}";
    } else if (req->command_path.mClusterId == LevelControl::Id) {
        snprintf(buf, len, "{\"0:U8\": %u, \"1:U16\": 0, \"2:U8\": 0, \"3:U8\": 0}", (unsigned)(uint8_t)data);
        return buf;
    } else if (req->command_path.mClusterId == ColorControl::Id) {
        if (req->command_path.mCommandId == ColorControl::Commands::MoveToColorTemperature::Id) {
            snprintf(buf, len, "{\"0:U16\": %u, \"1:U16\": 0, \"2:U8\": 0, \"3:U8\": 0}", (unsigned)(uint16_t)data);
        } else {
            snprintf(buf, len, "{\"0:U8\": %u, \"1:U8\": 254, \"2:U16\": 0, \"3:U8\": 0, \"4:U8\": 0}", (unsigned)(uint8_t)data);
        }
        return buf;
    }
    return NULL;
}

static void remote_control_callback(esp_matter::client::peer_device_t *peer_device, esp_matter::client::request_handle_t *req, void *priv) {
    if (req->type != esp_matter::client::INVOKE_CMD) return;

    char command_data_str[96];
    const char *payload = build_command_payload(req, command_data_str, sizeof(command_data_str));
    if (payload) {
        esp_matter::client::interaction::invoke::send_request(NULL, peer_device, req->command_path, payload, NULL, NULL, chip::NullOptional);
    }
}

static void remote_group_callback(uint8_t fabric_index, esp_matter::client::request_handle_t *req, void *priv) {
    if (req->type != esp_matter::client::INVOKE_CMD) return;

    char command_data_str[96];
    const char *payload = build_command_payload(req, command_data_str, sizeof(command_data_str));
    if (payload) {
        esp_matter::client::interaction::invoke::send_group_request(fabric_index, req->command_path, payload);
    }
}

static endpoint_t *create_switch_endpoint(node_t *node)
{
    generic_switch::config_t config;
    config.switch_cluster.number_of_positions = 2;
    config.switch_cluster.feature_flags = chip::to_underlying(Switch::Feature::kMomentarySwitch);
    endpoint_t *endpoint = generic_switch::create(node, &config, ENDPOINT_FLAG_NONE, NULL);
    
    cluster_t *sw_cluster = cluster::get(endpoint, Switch::Id);
    if (sw_cluster) {
        cluster::switch_cluster::feature::action_switch::add(sw_cluster);
        cluster::switch_cluster::feature::momentary_switch_multi_press::config_t msm_config; 
        msm_config.multi_press_max = 5;
        cluster::switch_cluster::feature::momentary_switch_multi_press::add(sw_cluster, &msm_config);
        cluster::switch_cluster::feature::momentary_switch_long_press::add(sw_cluster);
    }

    cluster_t *ps_cluster = cluster::get(endpoint, PowerSource::Id);
    if (!ps_cluster) {
        cluster::power_source::config_t ps_config; 
        ps_config.status = 1; 
        ps_config.feature_flags = chip::to_underlying(PowerSource::Feature::kBattery);
        ps_cluster = cluster::power_source::create(endpoint, &ps_config, CLUSTER_FLAG_SERVER);
    }
    if (ps_cluster) {
        cluster::power_source::feature::battery::config_t bat_config;
        cluster::power_source::feature::battery::add(ps_cluster, &bat_config);
        if (!attribute::get(ps_cluster, PowerSource::Attributes::BatPercentRemaining::Id)) {
            cluster::power_source::attribute::create_bat_percent_remaining(ps_cluster, nullable<uint8_t>(), 0, 200);
        }
    }

    return endpoint;
}

// One Generic Switch per channel so the controller knows which channel a multi-click came from
// (Home Assistant shows each as an event entity with multi_press_2 ... multi_press_5).
static endpoint_t *create_channel_switch_endpoint(node_t *node)
{
    generic_switch::config_t config;
    config.switch_cluster.number_of_positions = 2;
    config.switch_cluster.feature_flags = chip::to_underlying(Switch::Feature::kMomentarySwitch);
    endpoint_t *endpoint = generic_switch::create(node, &config, ENDPOINT_FLAG_NONE, NULL);

    cluster_t *sw_cluster = cluster::get(endpoint, Switch::Id);
    if (sw_cluster) {
        cluster::switch_cluster::feature::action_switch::add(sw_cluster);
        cluster::switch_cluster::feature::momentary_switch_multi_press::config_t msm_config;
        msm_config.multi_press_max = 5;
        cluster::switch_cluster::feature::momentary_switch_multi_press::add(sw_cluster, &msm_config);
    }
    return endpoint;
}

static endpoint_t *create_color_light_endpoint(node_t *node, uint8_t channel_idx)
{
    endpoint::extended_color_light::config_t ext_light_cfg;
    ext_light_cfg.on_off.on_off = (light_config[channel_idx].brightness > 0);
    ext_light_cfg.level_control.current_level = light_config[channel_idx].brightness;

    // Mode de couleur par défaut
    ext_light_cfg.color_control.color_mode = (uint8_t)ColorControl::ColorMode::kColorTemperature;
    ext_light_cfg.color_control.enhanced_color_mode = (uint8_t)ColorControl::ColorMode::kColorTemperature;
ext_light_cfg.color_control_color_temperature.start_up_color_temperature_mireds = nullptr;
    // --- C'EST ICI LA VRAIE CORRECTION ---
    // On définit les limites physiques DIRECTEMENT dans la structure de base.
    // Le SDK créera les attributs proprement, sans faire crasher l'ESP.
    ext_light_cfg.color_control_color_temperature.color_temperature_mireds = 350;
    ext_light_cfg.color_control_color_temperature.color_temp_physical_min_mireds = 153; 
    ext_light_cfg.color_control_color_temperature.color_temp_physical_max_mireds = 500;

    // Création de l'endpoint : tout est instancié proprement avec tes valeurs !
    endpoint_t *endpoint = endpoint::extended_color_light::create(node, &ext_light_cfg, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) return NULL;

    cluster_t *cc = cluster::get(endpoint, ColorControl::Id);
    if (cc) {
        // FeatureMap (Optionnel mais recommandé pour bien forcer le HS et CT)
        esp_matter_attr_val_t fm = esp_matter_uint32(0x1B);
        attribute::update(endpoint::get_id(endpoint), ColorControl::Id, 0xFFFC, &fm);

        // Ajout explicite de Hue/Saturation pour Apple/Google
        cluster::color_control::feature::hue_saturation::config_t hs;
        hs.current_hue = light_config[channel_idx].hue;
        hs.current_saturation = light_config[channel_idx].saturation;
        cluster::color_control::feature::hue_saturation::add(cc, &hs);
    }
    
    // 2. Add the Client Clusters to the SAME endpoint
    // This makes the ESP32 a "Controller" that can send commands OUT
    cluster::on_off::config_t onoff_client_cfg;
    cluster::on_off::create(endpoint, &onoff_client_cfg, CLUSTER_FLAG_CLIENT);
    
    cluster::level_control::config_t level_client_cfg;
    cluster::level_control::create(endpoint, &level_client_cfg, CLUSTER_FLAG_CLIENT);
    
    cluster::color_control::config_t color_client_cfg;
    cluster::color_control::create(endpoint, &color_client_cfg, CLUSTER_FLAG_CLIENT);

    // 3. Add the Binding Cluster (Mandatory for the HA Binding UI to work)
    cluster::binding::config_t binding_cfg;
    cluster::binding::create(endpoint, &binding_cfg, CLUSTER_FLAG_SERVER);

    return endpoint;
}

// Channel type select, on its own Mode Select endpoint: Home Assistant's matter.js server ignores a Mode Select
// cluster placed on a light endpoint (not part of the light device type). Shown as "Channel N type" (Description).
static endpoint_t *create_channel_type_endpoint(node_t *node, uint8_t channel_idx)
{
    endpoint::mode_select::config_t cfg;
    snprintf(cfg.mode_select.description, sizeof(cfg.mode_select.description), "Channel %d type", channel_idx + 1);
    cfg.mode_select.current_mode = channel_types[channel_idx];
    cfg.mode_select.delegate = &gSettingsModesManager;
    return endpoint::mode_select::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
}

// Dial up / dial down for a "Dial events" channel, like the IKEA BILRESA wheel: each report is a multi-press
// whose count is the number of notches (Home Assistant: event entity, multi_press_1 ... multi_press_10).
static endpoint_t *create_dial_endpoint(node_t *node)
{
    generic_switch::config_t config;
    config.switch_cluster.number_of_positions = 2;
    config.switch_cluster.feature_flags = chip::to_underlying(Switch::Feature::kMomentarySwitch);
    endpoint_t *endpoint = generic_switch::create(node, &config, ENDPOINT_FLAG_NONE, NULL);

    cluster_t *sw_cluster = cluster::get(endpoint, Switch::Id);
    if (sw_cluster) {
        cluster::switch_cluster::feature::action_switch::add(sw_cluster);
        cluster::switch_cluster::feature::momentary_switch_multi_press::config_t msm_config;
        msm_config.multi_press_max = MATTER_DIAL_MAX_NOTCHES;
        cluster::switch_cluster::feature::momentary_switch_multi_press::add(sw_cluster, &msm_config);
    }
    return endpoint;
}

static endpoint_t *create_deep_sleep_endpoint(node_t *node)
{
    endpoint::mode_select::config_t cfg;
    snprintf(cfg.mode_select.description, sizeof(cfg.mode_select.description), "Deep sleep after");
    cfg.mode_select.current_mode = deep_sleep_mode;
    cfg.mode_select.delegate = &gSettingsModesManager;
    return endpoint::mode_select::create(node, &cfg, ENDPOINT_FLAG_NONE, NULL);
}


extern "C" void matter_init(void)
{
    esp_err_t err;
    ensure_nvs_open();
    load_channel_types();
    load_deep_sleep_mode();

    current_personality = get_personality();
    if (current_personality == PERSONALITY_UNSET) {
        current_personality = PERSONALITY_THREAD;
        ESP_LOGI(TAG, "No personality set, defaulting to THREAD and saving");
        set_personality(current_personality); // Persist the default so it's not UNSET next boot
    } else {
        ESP_LOGI(TAG, "Loaded personality from NVS: %s", 
                 (current_personality == PERSONALITY_WIFI) ? "WIFI" : "THREAD");
    }

    node::config_t node_config;
    // CRITICAL: Decide which transport to advertise for commissioning
    if (current_personality == PERSONALITY_WIFI) {
        node_config.root_node.network_commissioning.feature_map = chip::to_underlying(chip::app::Clusters::NetworkCommissioning::Feature::kWiFiNetworkInterface);
    } else {
        node_config.root_node.network_commissioning.feature_map = chip::to_underlying(chip::app::Clusters::NetworkCommissioning::Feature::kThreadNetworkInterface);
    }

    // Apply the selected transport before Matter starts advertising so the commissioner
    // only sees the intended network path.
    apply_transport_personality();

    node_t *node = node::create(&node_config, app_attribute_update_cb, nullptr);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }
    
    endpoint_t *sw_ep = create_switch_endpoint(node);
    if (sw_ep) switch_endpoint_id = endpoint::get_id(sw_ep);

#if CONFIG_ENABLE_OTA_REQUESTOR
    // Add OTA Requestor cluster to the root node (endpoint 0)
    cluster::ota_software_update_requestor::config_t ota_config;
    cluster::ota_software_update_requestor::create(endpoint::get(node, 0), &ota_config, CLUSTER_FLAG_SERVER);
#endif

    for (int i = 0; i < 3; i++) {
        endpoint_t *ep = create_color_light_endpoint(node, i);
        if (ep) light_endpoint_ids[i] = endpoint::get_id(ep);
    }

    // Created after the lights so existing endpoint IDs (and the controller's entities) stay the same
    for (int i = 0; i < 3; i++) {
        endpoint_t *ep = create_channel_switch_endpoint(node);
        if (ep) channel_switch_endpoint_ids[i] = endpoint::get_id(ep);
    }

    for (int i = 0; i < 3; i++) {
        endpoint_t *ep = create_channel_type_endpoint(node, i);
        if (ep) channel_type_endpoint_ids[i] = endpoint::get_id(ep);
    }
    endpoint_t *ds_ep = create_deep_sleep_endpoint(node);
    if (ds_ep) deep_sleep_endpoint_id = endpoint::get_id(ds_ep);

    // Last, so all earlier endpoint IDs stay the same: channel 1 up, channel 1 down, channel 2 up...
    for (int i = 0; i < 3; i++) {
        endpoint_t *up = create_dial_endpoint(node);
        if (up) dial_up_endpoint_ids[i] = endpoint::get_id(up);
        endpoint_t *down = create_dial_endpoint(node);
        if (down) dial_down_endpoint_ids[i] = endpoint::get_id(down);
    }

    esp_matter::set_custom_device_instance_info_provider(&gAppDeviceInstanceInfoProvider);
    // Must be set before the stack starts (before any GetDiagnosticDataProvider() call)
    chip::DeviceLayer::SetDiagnosticDataProvider(&gDiagnosticDataProvider);

    // Initialize the Matter Client stack (Binding Manager)
    esp_matter::client::set_request_callback(remote_control_callback, remote_group_callback, NULL);
    esp_matter::client::binding_manager_init();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t ot_config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_NONE,
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = 10,
            .task_queue_size = 10,
        },
    };
    set_openthread_platform_config(&ot_config);
#endif

#include <app/server/CommissioningWindowManager.h>

// ...

    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter Stack: %d", err);
    } else {
        {
            esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);

            uint8_t fabricCount = chip::Server::GetInstance().GetFabricTable().FabricCount();
            ESP_LOGI(TAG, "Fabric count: %d", fabricCount);

#if CHIP_CONFIG_ENABLE_ICD_SERVER && CHIP_DEVICE_CONFIG_ENABLE_THREAD
            {
                chip::app::ICDManager &icdMgr = chip::Server::GetInstance().GetICDManager();
                chip::app::ICDStateObserver *icdMgmt = &chip::app::Clusters::ICDManagementServer::GetInstance();
                icdMgr.ReleaseObserver(icdMgmt);
                gThreadPollingFixup.mInner = icdMgmt;
                if (!icdMgr.RegisterObserver(&gThreadPollingFixup)) {
                    // Should not happen (we just freed a slot); restore the cluster's observer.
                    gThreadPollingFixup.mInner = nullptr;
                    icdMgr.RegisterObserver(icdMgmt);
                    ESP_LOGE(TAG, "ICD observer pool full: Thread poll period will not follow ICD mode");
                }
            }
            // Start fast; the first transition to idle switches to the slow interval.
            ThreadPollingFixup::Apply(chip::ICDConfigurationData::GetInstance().GetFastPollingInterval());
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
            if (fabricCount == 0) {
                // Not commissioned yet - keep BOTH transports available for commissioning
                // The commissioner will decide which network to use
                // Personality only affects which transport is ADVERTISED, not disabled
                ESP_LOGI(TAG, "Not commissioned - both transports available for commissioning");
                // Don't disable anything - let Matter commissioning work with either transport
            } else {
                // Already commissioned - use the saved personality to enable correct transport
                ESP_LOGI(TAG, "Device commissioned - using personality: %s", 
                         (current_personality == PERSONALITY_WIFI) ? "WIFI" : "THREAD");
                apply_transport_personality();
            }
#endif

            // Manually Open Commissioning Window since Auto-Start is disabled
            if (fabricCount == 0) {
                ESP_LOGI(TAG, "Opening Commissioning Window");
                chip::CommissioningWindowManager & commMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
                CHIP_ERROR openErr = commMgr.OpenBasicCommissioningWindow(
                    commMgr.MaxCommissioningTimeout(),
                    chip::CommissioningWindowAdvertisement::kAllSupported
                );
                if (openErr != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT, openErr.Format());
                    ESP_LOGW(TAG, "Attempting factory reset to recover...");
                    // Try deleting all fabrics and reopening
                    chip::Server::GetInstance().GetFabricTable().DeleteAllFabrics();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    openErr = commMgr.OpenBasicCommissioningWindow(
                        commMgr.MaxCommissioningTimeout(),
                        chip::CommissioningWindowAdvertisement::kAllSupported
                    );
                    if (openErr != CHIP_NO_ERROR) {
                        ESP_LOGE(TAG, "Factory reset + reopen failed: %" CHIP_ERROR_FORMAT, openErr.Format());
                    } else {
                        ESP_LOGI(TAG, "Factory reset recovered - commissioning window now open");
                    }
                } else {
                    ESP_LOGI(TAG, "Commissioning window opened successfully");
                }
            } else {
                ESP_LOGI(TAG, "Device already commissioned with %d fabric(s)", fabricCount);
            }
        }
    }

    register_personality_command();
    register_factory_reset_command();
    matter_stack_ready = true;
    ESP_LOGI(TAG, "Matter Stack initialized with personality: %s", (current_personality == PERSONALITY_WIFI) ? "WIFI" : "THREAD");
}

extern "C" bool matter_is_ready(void) { return matter_stack_ready; }
extern "C" bool matter_is_commissioned(void) {
    if (commissioned_cached_valid) {
        return is_commissioned_cached;
    }
    
    if (!matter_stack_ready) return false;

    // Use shorter timeout just to ping it initially
    esp_matter::lock::ScopedChipStackLock lock(pdMS_TO_TICKS(50));
    uint8_t count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    is_commissioned_cached = (count > 0);
    commissioned_cached_valid = true;
    return is_commissioned_cached;
}
extern "C" bool matter_is_commissioning(void) { return !matter_is_commissioned(); }  // Stay awake while unpaired

extern "C" bool matter_is_transport_connected(void)
{
    if (!matter_stack_ready) return false;

    if (current_personality == PERSONALITY_WIFI) {
        return wifi_connected;
    }

    return thread_connected;
}

extern "C" void matter_send_channel_multi_press(uint8_t channel, uint8_t press_count) {
    if (channel >= 3 || channel_switch_endpoint_ids[channel] == 0 || press_count == 0) return;
    chip::DeviceLayer::StackLock lock;
    cluster::switch_cluster::event::send_multi_press_complete(channel_switch_endpoint_ids[channel], 0, press_count);
}

extern "C" int matter_send_dial(uint8_t channel, int steps) {
    if (channel >= 3 || steps == 0) return 0;
    uint16_t ep = (steps > 0) ? dial_up_endpoint_ids[channel] : dial_down_endpoint_ids[channel];
    int count = (steps > 0) ? steps : -steps;
    if (count > MATTER_DIAL_MAX_NOTCHES) count = MATTER_DIAL_MAX_NOTCHES;
    if (ep == 0) return 0;
    {
        chip::DeviceLayer::StackLock lock;
        cluster::switch_cluster::event::send_multi_press_complete(ep, 0, (uint8_t)count);
    }
    return (steps > 0) ? count : -count;
}

// ============ MenuSystem Synchronization Functions ============
// These are called FROM MenuSystem (user side) TO update Matter

extern "C" void matter_set_menu_system_interface(const MenuSystemInterface *iface) {
    if (iface) {
        menusys = *iface;
    }
}

// ============ THREAD-SAFE MATTER UPDATES (via ScheduleWork) ============

struct matter_update_args {
    uint16_t endpoint;
    uint32_t cluster;
    uint32_t attr;
    esp_matter_attr_val_t val;
};

static void matter_attribute_update_task(intptr_t arg) {
    struct matter_update_args *a = (struct matter_update_args *)arg;
    attribute::update(a->endpoint, a->cluster, a->attr, &(a->val));
    free(a);
}

static void schedule_matter_update(uint16_t endpoint, uint32_t cluster, uint32_t attr, esp_matter_attr_val_t val) {
    struct matter_update_args *arg = (struct matter_update_args *)malloc(sizeof(struct matter_update_args));
    if (!arg) {
        ESP_LOGW(TAG, "Out of memory while scheduling Matter attribute update");
        return;
    }

    arg->endpoint = endpoint;
    arg->cluster = cluster;
    arg->attr = attr;
    arg->val = val;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(matter_attribute_update_task, (intptr_t)arg);
}

extern "C" void matter_update_intensity(uint8_t channel, uint8_t intensity_0_100) {
    if (channel >= 3 || light_endpoint_ids[channel] == 0) return;

    // Matter CurrentLevel is 1-254 (0x01-0xFE). 0 is technically Off or "Not at level"
    // We map 0-100% to 1-254 for the 'On' state.
    if (channel_types[channel] == CHANNEL_TYPE_EVENTS) return;  // no light on this channel

    // On/Off channel: only on/off is reported and sent, so the light keeps its own brightness
    bool on_off_only = (channel_types[channel] == CHANNEL_TYPE_ON_OFF);
    if (on_off_only && intensity_0_100 > 0) intensity_0_100 = 100;

    uint8_t level = pct_to_level(intensity_0_100);
    if (intensity_0_100 == 0) level = 0;

    bool should_be_on = (intensity_0_100 > 0);
    
    ESP_LOGI(TAG, "Syncing Intensity: CH %d -> %d%% (Matter Level: %d, On: %s)", 
             channel, intensity_0_100, level, should_be_on ? "YES" : "NO");

    light_config[channel].brightness = level;

    // Order matters: controllers apply each attribute report as it arrives. Reporting On before the new level
    // (or writing level 0 on Off) exposed a transient "on at brightness 0" state, which Home Assistant passed to
    // the real light, which then came on at its last brightness (often 100 %) until the next dial step.
    if (!should_be_on) {
        // Off only: keep the last level, like a real light (the internal state above still tracks 0 = off)
        schedule_matter_update(light_endpoint_ids[channel], OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(false));
    } else if (on_off_only) {
        // Restore the light's own brightness first: after an Off from the controller, Matter left the level at
        // the minimum, and the controller would otherwise switch the light on at 1/255
        schedule_matter_update(light_endpoint_ids[channel], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, esp_matter_nullable_uint8(on_off_level[channel]));
        schedule_matter_update(light_endpoint_ids[channel], OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(true));
    } else {
        // Level first, then On, so the light never appears "on" with a stale level
        schedule_matter_update(light_endpoint_ids[channel], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id, esp_matter_nullable_uint8(level));
        schedule_matter_update(light_endpoint_ids[channel], OnOff::Id, OnOff::Attributes::OnOff::Id, esp_matter_bool(true));
    }

    // B. Fire Direct Command over Binding (one message: MoveToLevelWithOnOff also switches the light on)
    esp_matter::client::request_handle_t req;
    req.type = esp_matter::client::INVOKE_CMD;

    if (should_be_on && !on_off_only) {
        req.command_path.mClusterId = LevelControl::Id;
        req.command_path.mCommandId = LevelControl::Commands::MoveToLevelWithOnOff::Id;
        req.request_data = (void*)(uintptr_t)level;
    } else if (should_be_on) {
        req.command_path.mClusterId = OnOff::Id;
        req.command_path.mCommandId = OnOff::Commands::On::Id;
    } else {
        req.command_path.mClusterId = OnOff::Id;
        req.command_path.mCommandId = OnOff::Commands::Off::Id;
    }
    esp_matter::client::cluster_update(light_endpoint_ids[channel], &req);
}

struct matter_color_temp_args {
    uint16_t endpoint;
    uint16_t mireds;
};

static void matter_color_temp_task(intptr_t arg) {
    matter_color_temp_args *up = (matter_color_temp_args *)arg;
    
    // 1. Préparation de la structure exigée par le SDK CHIP récent
    chip::app::Clusters::ColorControl::Commands::MoveToColorTemperature::DecodableType req;
    req.colorTemperatureMireds = up->mireds;
    req.transitionTime = 0; // Pas de transition lente
    req.optionsMask.SetRaw(0);
    req.optionsOverride.SetRaw(0);

    // 2. Appel de la commande avec les 2 arguments requis
    ColorControlServer::Instance().moveToColorTempCommand(up->endpoint, req);

    delete up;
}

extern "C" void matter_update_color_temp(uint8_t channel, uint8_t temp_0_100) {
    if (channel >= 3 || light_endpoint_ids[channel] == 0) return;
    if (channel_types[channel] != CHANNEL_TYPE_WHITE && channel_types[channel] != CHANNEL_TYPE_COLOR) return;

    light_config[channel].colorTemp = temp_0_100;
    light_config[channel].rainbowMode = false;

    uint16_t mireds = pct_to_mireds(temp_0_100 > 100 ? 100 : temp_0_100);

    ESP_LOGI(TAG, "Syncing ColorTemp: CH %d -> %d%% (%d Mireds)", channel, temp_0_100, mireds);

    matter_color_temp_args *args = new (std::nothrow) matter_color_temp_args{light_endpoint_ids[channel], mireds};
    if (!args) {
        ESP_LOGW(TAG, "Out of memory while scheduling color temperature update");
        return;
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(matter_color_temp_task, (intptr_t)args);

    esp_matter::client::request_handle_t req;
    req.type = esp_matter::client::INVOKE_CMD;
    req.command_path.mClusterId = ColorControl::Id;
    req.command_path.mCommandId = ColorControl::Commands::MoveToColorTemperature::Id;
    req.request_data = (void*)(uintptr_t)mireds;
    esp_matter::client::cluster_update(light_endpoint_ids[channel], &req);
}

struct matter_rainbow_args {
    uint16_t endpoint;
    uint8_t hue;
};

static void matter_rainbow_task(intptr_t arg) {
    matter_rainbow_args *up = (matter_rainbow_args *)arg;

    // Appel avec les 7 arguments strictement typés
    ColorControlServer::Instance().moveToHueAndSaturationCommand(
        up->endpoint, 
        up->hue, 
        254, // Saturation maximale
        0,   // Temps de transition (immédiat)
        chip::BitMask<chip::app::Clusters::ColorControl::OptionsBitmap>(0), // Masque d'options vide
        chip::BitMask<chip::app::Clusters::ColorControl::OptionsBitmap>(0), // Override d'options vide
        false // isEnhanced : faux, car on utilise le standard 0-254 et non 0-65535
    );

    delete up;
}
extern "C" void matter_update_rainbow_hue(uint8_t channel, uint8_t hue_0_255) {
    if (channel >= 3 || light_endpoint_ids[channel] == 0) return;
    if (channel_types[channel] != CHANNEL_TYPE_COLOR) return;

    light_config[channel].rainbowMode = true;

    uint8_t matterHue = ((uint16_t)hue_0_255 * 254) / 255;
    light_config[channel].hue = matterHue;

    ESP_LOGI(TAG, "Syncing Rainbow: CH %d -> Hue %d (Matter Hue: %d)", channel, hue_0_255, matterHue);

    matter_rainbow_args *args = new (std::nothrow) matter_rainbow_args{light_endpoint_ids[channel], matterHue};
    if (!args) {
        ESP_LOGW(TAG, "Out of memory while scheduling hue update");
        return;
    }
    chip::DeviceLayer::PlatformMgr().ScheduleWork(matter_rainbow_task, (intptr_t)args);

    esp_matter::client::request_handle_t req;
    req.type = esp_matter::client::INVOKE_CMD;
    req.command_path.mClusterId = ColorControl::Id;
    req.command_path.mCommandId = ColorControl::Commands::MoveToHueAndSaturation::Id;
    req.request_data = (void*)(uintptr_t)matterHue;
    esp_matter::client::cluster_update(light_endpoint_ids[channel], &req);
}

static void matter_update_battery_task(intptr_t arg) {
    uint8_t percent = (uint8_t)arg;
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(percent * 2);
    attribute::update(switch_endpoint_id, PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id, &val);
}

extern "C" void matter_set_battery_percent(uint8_t percent) {
    if (switch_endpoint_id == 0) return;
    chip::DeviceLayer::PlatformMgr().ScheduleWork(matter_update_battery_task, (intptr_t)percent);
}

extern "C" int matter_get_personality_int(void) { return (int)current_personality; }

extern "C" esp_err_t matter_shutdown_wifi_radio(void)
{
    // Hold the stack lock so the Matter task can't touch the Wi-Fi driver mid-teardown.
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_stop();
    return esp_wifi_deinit();
}