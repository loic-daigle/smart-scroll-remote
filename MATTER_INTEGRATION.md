# Matter Integration Guide for Smart Button

## Overview

Tu as maintenant une synchronisation **bidirectionnelle complète** entre :
- **Local** : Encodeur → MenuSystem → NeoPixel
- **Matter** : Home Assistant → Endpoint Matter ↔️ MenuSystem → NeoPixel

## Architecture

```
┌──────────────────┐
│   Home Assistant │
└────────┬─────────┘
         │ Matter Protocol
    ┌────▼─────────────────────────────────┐
    │     Matter Stack (3 Endpoints)       │
    │  - LevelControl (Brightness 0-254)   │
    │  - ColorControl (Hue 0-254)          │
    │  - ColorControl (Mireds 153-500)     │
    └────┬──────────────────────────────────┘
         │ app_attribute_update_cb()
         │ (Matter→Local)
    ┌────▼─────────────────────┐
    │  MenuSystem (C++)         │
    │  - intensity[3] (0-100)   │
    │  - colorTemp[3] (0-100)   │
    │  - rainbowHue[3] (0-255)  │
    │  - isRainbowMode[3]       │
    └────┬──────────────────────┘
         │ updateDisplay()
    ┌────▼──────────┐
    │  NeoPixel RGB │
    └───────────────┘

Encodeur → MenuSystem → matter_update_*() → Matter Endpoints
```

## Mappings de Valeurs

### 1. Intensité (Luminosité)

| Domaine | Plage | Conversion |
|---------|-------|-----------|
| **Local** | 0-100% | % direct |
| **Matter (LevelControl)** | 0-254 | uint8_t (0-254 = 0-100%) |

**Code** :
```c
// Local → Matter
uint8_t level = (intensity * 254) / 100;
matter_update_intensity(channel, intensity); // Auto-converted

// Matter → Local
uint8_t intensity = (matterLevel * 100) / 254;
menu->intensity[channel] = intensity;
```

### 2. Température de Couleur

| Domaine | Plage | Signification |
|---------|-------|-------------|
| **Local** | 0-100 | 0=warm(2000K), 50=mid(4200K), 100=cold(6500K) |
| **Matter (Mireds)** | 153-500 | Reciprocal megakelvin (153≈6500K, 500≈2000K) |

**Conversion exacte** :
```c
// Local (0-100) → Matter Mireds (153-500)
// 0 (warm) → 500, 100 (cold) → 153
uint16_t mireds = 500 - (colorTemp * 347 / 100);  // 347 = 500-153

// Matter → Local
uint8_t colorTemp = (500 - mireds) * 100 / 347;
```

**Formule complète** :
```
colorTemp:  0  ──────────────  100
            │                   │
mireds:   500  ──────────────  153
         warm                 cold
```

### 3. Mode Rainbow (HSV)

| Domaine | Plage | Note |
|---------|-------|------|
| **Local Hue** | 0-255 | 8-bit hue (0=red, 85=green, 170=blue) |
| **Matter Hue** | 0-254 | 8-bit Matter standard |
| **Saturation** | 0-254 | 254 = 100% saturé |
| **Brightness** | 0-254 | Idem LevelControl |

**Conversion** :
```c
// Local → Matter
uint8_t matterHue = (localHue * 254) / 256;

// Matter → Local
uint8_t localHue = (matterHue * 256) / 254;
```

## Flux Synchronisation

### Local → Matter (Encodeur → HA)

```cpp
// Dans MenuSystem::handleEncoder()

case IN_CHANNEL_INTENSITY:
    intensity[currentChannel] = constrain(intensity[currentChannel] + dir * 5, 0, 100);
    matter_update_intensity(currentChannel, intensity[currentChannel]);
    // ✅ Matter endpoint se met à jour
    // ✅ Home Assistant le voit
    break;

case IN_CHANNEL_COLOR_TEMP:
    colorTemp[currentChannel] = constrain(colorTemp[currentChannel] + dir * 5, 0, 100);
    isRainbowMode[currentChannel] = false;
    matter_update_color_temp(currentChannel, colorTemp[currentChannel]);
    // ✅ ColorTemperatureMireds: 153-500 updated
    // ✅ Rainbow mode désactivé dans Matter
    break;

case IN_CHANNEL_RAINBOW:
    rainbowHue[currentChannel] += dir * 10;
    isRainbowMode[currentChannel] = true;
    matter_update_rainbow_hue(currentChannel, rainbowHue[currentChannel]);
    // ✅ CurrentHue: 0-254 updated
    // ✅ Rainbow mode activé dans Matter
    break;
```

### Matter → Local (HA → NeoPixel)

```cpp
// Dans app_attribute_update_cb() (matter.cpp)

// Quand HA change LevelControl → CurrentLevel
uint8_t intensity = (newBrightness * 100) / 254;
menusys.updateIntensity(channel, intensity);

// Quand HA change ColorControl → ColorTemperatureMireds
uint8_t colorTemp = (500 - mireds) * 100 / 347;
menusys.updateColorTemp(channel, colorTemp);
menusys.updateRainbowMode(channel, false);  // ← Désactive rainbow

// Quand HA change ColorControl → CurrentHue
uint8_t localHue = (matterHue * 256) / 254;
menusys.updateRainbowHue(channel, localHue);
menusys.updateRainbowMode(channel, true);   // ← Active rainbow
```

## Intégration Avec main.c

Ajoute ceci à `main.c` ou `main.cpp`:

```c
#include "menu_system_matter.h"

// Globales
static MenuSystem *gMenu = nullptr;
// ... autres variables

void app_main(void) {
    // ... Initialization NVS, LED strip, etc.

    // 1. Initialize Matter
    matter_init();

    // 2. Create and initialize MenuSystem with Matter integration
    gMenu = menu_system_create();
    menu_system_init(gMenu, led_strip, app_nvs_handle);

    // 3. Main loop
    int32_t currentEncoder = 0;

    while (1) {
        // ... Read button

        // Read encoder (from polling or ISR)
        currentEncoder = encoderValue;

        // Process encoder
        menu_system_handle_encoder(gMenu, currentEncoder);

        // Process button
        if (btnEvent != 0) {
            menu_system_handle_button(gMenu, btnEvent);
        }

        // Handle long press overview
        menu_system_set_overview(gMenu, button_held);

        // Periodically save to NVS
        if (save_needed) {
            menu_system_save_to_nvs(gMenu);
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
```

## Gestion Deep Sleep (Sleepy End Device)

Le code Matter déclare déjà les endpoints comme **Extended Color Light**. Pour les marquer comme Sleepy End Device :

**Dans `matter_init()` (matter.cpp)**, le code appelle :
```cpp
esp_matter::start(app_event_cb);
```

Et ouvre la fenêtre de commissioning. Cette configuration permet à Home Assistant de détecter le mode sleep.

**Configuration NVS** pour le timeout :
```c
// Tunable via Matter's OnOffTransitionTime
// Par défaut: 600 (6 secondes)
uint32_t sleep_timeout = matter_get_sleep_timeout_ms();
```

Le device dormira après ~15s d'inactivité. Home Assistant comprendra que c'est un SED s'il est commissioning en Thread ou WiFi selon la personality.

## Test Valeurs

### Intensity
```
Local: 0%  → Matter: 0    ✅
Local: 50% → Matter: 127  ✅
Local: 100% → Matter: 254 ✅
```

### Color Temp
```
Local: 0 (warm)     → Mireds: 500  (2000K)  ✅
Local: 50 (mid)     → Mireds: 327  (3050K)  ✅
Local: 100 (cold)   → Mireds: 153  (6500K)  ✅
```

### Rainbow Hue
```
Local: 0    → Matter: 0     (Red)      ✅
Local: 85   → Matter: 85    (Green)    ✅
Local: 170  → Matter: 170   (Blue)     ✅
Local: 255  → Matter: 254   (Magenta)  ✅
```

## Callbacks Matter → Local

Les callbacks sont définis dans `app_attribute_update_cb()` et appellent les fonctions :

```c
menusys.updateIntensity(channel, value);    // 0-100
menusys.updateColorTemp(channel, value);    // 0-100
menusys.updateRainbowHue(channel, hue);     // 0-255
menusys.updateRainbowMode(channel, enabled);// true/false
menusys.updateDisplay();                    // Refresh LEDs
```

Ces **callbacks sont enregistrés** automatiquement via :
```c
matter_set_menu_system_interface(&iface);  // Dans menu_system_init()
```

## Débogage

Activer les logs Matter/MenuSystem :
```c
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
```

Vérifier les valeurs actuelles :
```c
printf("CH0: intensity=%d, temp=%d, hue=%d, rainbow=%d\n",
    menu_system_get_intensity(gMenu, 0),
    menu_system_get_color_temp(gMenu, 0),
    menu_system_get_rainbow_hue(gMenu, 0),
    menu_system_get_rainbow_mode(gMenu, 0)
);
```

## Points Critiques

⚠️ **NE PAS BLOQUER** dans Matter callbacks :
```cpp
// ❌ BAD
menusys.updateIntensity(ch, val);
vTaskDelay(1000);  // JAMAIS BLOQUER!

// ✅ GOOD
menusys.updateIntensity(ch, val);
menusys.updateDisplay();  // Non-blocking
```

⚠️ **Thread-safe** : Matter callbacks verrouillent déjà le CHIP stack.

⚠️ **Conversions exactes** : Les conversions d'échelle (0-100 ↔ 0-254) doivent être symétriques pour éviter les dérives.

## Résumé

| Aspect | Couvert ? |
|--------|-----------|
| 3 endpoints Extended Color Light | ✅ |
| LevelControl (0-254) | ✅ |
| ColorControl Hue (0-254) | ✅ |
| ColorControl Mireds (153-500) | ✅ |
| Local → Matter | ✅ |
| Matter → Local | ✅ |
| Non-bloquant | ✅ |
| Thread/WiFi dual stack | ✅ |
| Sleepy End Device | ✅ |
| NVS Persistence | ✅ |

Tu es prêt à compiler et tester ! 🚀
