# Smart Scroll Remote

A battery-powered **Matter** scroll-wheel remote for **ESP32-C6**, in the spirit of the IKEA BILRESA scroll remote. One rotary encoder, one button and three RGB LEDs control **three independent light channels**: brightness, colour temperature and colour.

The same firmware runs on **Thread _or_ Wi-Fi**, selectable at runtime. On Thread it behaves as a proper **sleepy end device**: it sleeps between interactions but stays connected, so the first turn of the dial is answered within about a second.

This repository contains everything needed to build one: the [firmware](firmware/), the [PCB](hardware/pcb/), the [enclosure](hardware/enclosure/) and a [Home Assistant blueprint](home_assistant/).

---

## Features

- **3 channels**, each exposed to Matter as an Extended Color Light (on/off, brightness, colour temperature, hue/saturation).
- **Bidirectional:** turning the dial updates the light, and changes from Home Assistant, an app or voice update the remote.
- **Per-channel type, set from Home Assistant:** On/Off, Dimmable, Dimmable + white or Dimmable + colour. The remote only shows the pages that type needs.
- **Multi-click events:** double, triple, 4 and 5 clicks per channel are reported as Matter Generic Switch events, so you can trigger any automation. An LED pattern on the remote confirms each one.
- **Dual stack:** Thread and Wi-Fi in one firmware. The commissioner decides, and the choice is remembered.
- **Low power:** automatic light sleep 5 s after the last interaction, with the network kept alive. Deep sleep after 1 h idle by default, adjustable from Home Assistant ("Deep sleep after": 10 min to 12 h, or never).
- **Battery reporting** through the Matter Power Source cluster.
- **Matter bindings:** each channel can also send On/Off, Level and Color commands directly to bound lights.
- **Home Assistant blueprint** that keeps each channel in sync with any light or light group (e.g. Tuya), or with an on/off device (switch, plug, fan), and adds click actions.

## Hardware

- **PCB:** design files, fabrication outputs and BOM in [`hardware/pcb/`](hardware/pcb/)
- **Enclosure:** CAD sources and print-ready files in [`hardware/enclosure/`](hardware/enclosure/)

| Part | Notes |
|---|---|
| ESP32-C6 module | Espressif ESP32-C6-MINI-class module (4 MB flash). Built-in USB Serial/JTAG used for flashing. |
| Rotary encoder | EC21A1520401 (15 pulses / 30 detents) or similar quadrature encoder |
| Push button | Active low |
| 3 × WS2812 (NeoPixel) | With a switchable supply so they draw nothing while asleep |
| Li-ion / LiPo cell | Measured through a 1:2 voltage divider |

### Pinout

| Function | GPIO | Notes |
|---|---|---|
| Button | 0 | Active low, internal pull-up, wake source |
| Battery voltage | 1 | ADC1 channel 1, via 1:2 divider (3.0 V = 0 %, 4.2 V = 100 %) |
| Encoder A | 2 | Internal pull-up, wake source |
| Encoder B | 3 | Internal pull-up, wake source |
| NeoPixel data | 6 | |
| NeoPixel power enable | 7 | Active low (LOW = LEDs powered) |
| BOOT / download mode | 9 | Hold LOW at power-up to force flashing mode (recovery only) |

The pins are defined at the top of [`firmware/main/main.c`](firmware/main/main.c).

> **Antenna placement matters.** The module's PCB antenna needs roughly 15 mm of clearance from metal, ground planes and the battery. A cell or metal part on top of or in front of the antenna costs a lot of range, and the logs show it as many MAC-layer retries (`Frame tx attempt 1/16 failed, error:NoAck`). Keep the antenna end of the module free, or use a module with an external antenna connector.

## Using the remote

| Action | Main screen (choose channel) | Inside a channel |
|---|---|---|
| Rotate | Select channel 1 / 2 / 3 (selected LED shown dim white) | Change the current setting |
| Click | Open the selected channel | Cycle **brightness → colour temperature → colour → brightness** |
| Long press (> 0.4 s) | Hold to preview all three channels' colours | Back to the main screen |
| Double / triple / 4 / 5 clicks | — | Send a multi-press event for this channel (see below) |

Multi-click confirmation blinks: 2 clicks = the 2 side LEDs, 3 = all three, 4 = all then the middle one, 5 = all then the sides.

### Channel type

Each channel has a **Channel N type** select (on the remote's device page in Home Assistant, under Configuration). It takes effect immediately, with no restart:

| Type | Inside the channel |
|---|---|
| On/Off | Click toggles. Turning the dial up switches on, down switches off. Only on/off is sent, so the light keeps its own brightness. |
| Dimmable | Brightness only (click does nothing) |
| Dimmable + white | Click cycles brightness ↔ colour temperature |
| Dimmable + colour | Click cycles brightness → colour temperature → colour (default) |
| Dial events (no light) | The dial and clicks are reported as events (LEDs soft blue), for media, blinds, scenes... See [Dial actions](#dial-actions-bilresa-style). |

Only the remote's behaviour changes: the channel stays an Extended Color Light in Matter, so Home Assistant never needs to re-read the device when you switch types.

When the remote is asleep:
- **Turning the dial** wakes it straight into the current channel's brightness setting.
- **Pressing the button** wakes it and toggles the current channel on or off.

## Building

Tested with:
- **ESP-IDF v5.4.1**
- **esp-matter** `main` at commit `20385fb` (January 2026)

```bash
# one-time: install ESP-IDF v5.4.1 and esp-matter (see their docs), then in each shell:
. ~/esp-idf/export.sh
export ESP_MATTER_PATH=~/esp-matter
. $ESP_MATTER_PATH/export.sh

git clone https://github.com/loic-daigle/smart-scroll-remote.git
cd smart-scroll-remote/firmware
idf.py build
idf.py -p <PORT> flash monitor
```

The committed [`firmware/sdkconfig`](firmware/sdkconfig) is the exact tested configuration (target `esp32c6`). [`firmware/sdkconfig.defaults`](firmware/sdkconfig.defaults) lists the options that matter, with comments explaining each one.

> **Windows + WSL:** the sources can live on the Windows drive (handy for GitHub Desktop and CAD/EDA tools) while building in WSL. Keep the build directory on the WSL filesystem for speed:
> `cd /mnt/c/.../smart-scroll-remote/firmware && idf.py -B ~/builds/smart_scroll_remote build`

### Flashing tips

- While the remote is plugged into a computer, it **does not light-sleep**, so normal `idf.py flash` and serial monitoring work.
- If the chip doesn't answer (e.g. after it slept on battery, when its USB stays dead until reset): hold **GPIO9 LOW** while plugging it in to enter download mode, then flash.
- From Windows (WSL has no direct USB access), the simplest route is the Windows `esptool` on the COM port, run from the build directory with `@flash_args`.

## Pairing (commissioning)

The firmware uses the **Matter test credentials** for development:

| | |
|---|---|
| Manual pairing code | **3497-011-2332** (`34970112332`) |
| Setup PIN | 20202021 |
| Discriminator | 3840 |
| Vendor / Product ID | 0xFFF1 / 0x8001 (test IDs) |

When unpaired, the remote opens a commissioning window over BLE at boot and stays awake. Most controllers warn that the device is uncertified; accept it. Choose **Thread** or **Wi-Fi** in your controller as usual.

> These are public test credentials and test attestation certificates, which is fine for a personal device. A product would need its own VID/PID, certificates and a factory partition.

### Pairing with Home Assistant

1. **Allow test devices in the Matter Server.** Settings → Add-ons (Apps) → **Matter Server** → Configuration → turn on **Enable test-net DCL** (if hidden, show the unused optional options), save, and restart the Matter Server. The current Matter Server (matter.js) rejects test attestation certificates by default: pairing then gets through Bluetooth and Thread, and fails at the very end with "error code 1 / unable to add the device", right after Home Assistant reads the remote's certificate.
2. **Give the phone your Thread credentials.** Home Assistant app → Settings → Devices & services → Thread → ⋮ → **Send credentials to phone**.
3. **Pair from the phone**, close to the remote, with the code above. On Android, the system **"Matter device found" popup** (then choose Home Assistant) has been more reliable than starting from Home Assistant's own "Add device" button. Android first adds the device to Google's Matter fabric, then shares it with Home Assistant.

**Starting over:** if the remote is still paired somewhere (for example in Google Home), it does not open pairing mode on its own. Either share it from that app (*Linked Matter apps & services*), or factory-reset it by erasing its `nvs` partition, which also clears the channel types and other settings:

```
esptool --chip esp32c6 -p <PORT> erase-region 0x9000 0x10000
```

### Serial console commands

Over the UART console (the module's TX/RX pins; the USB port is output-only):

```
matter esp personality thread   # or: wifi — select transport and reboot
matter esp factory_reset        # forget all fabrics and reopen commissioning
```

## Matter data model

| Endpoint | Device type | Purpose |
|---|---|---|
| 0 | Root node | Network commissioning (Thread or Wi-Fi), diagnostics, ICD management |
| 1 | Generic Switch | Power Source (battery %) |
| 2, 3, 4 | Extended Color Light | Channels 1, 2, 3 (+ Binding and client clusters for direct control) |
| 5, 6, 7 | Generic Switch | Multi-press events for channels 1, 2, 3 (`multi_press_2` … `multi_press_5`) |
| 8, 9, 10 | Mode Select | Channel 1, 2, 3 type. On their own endpoints: Home Assistant's Matter server ignores a Mode Select cluster placed on a light endpoint. |
| 11 | Mode Select | Deep sleep after (10 min, 30 min, 1 h, 4 h, 12 h, never). Applies from the next time the remote goes idle. |
| 12 – 17 | Generic Switch | Dial up / dial down for channels 1, 2, 3 (in that order), used by "Dial events" channels. `multi_press_N` = N notches. |

## Home Assistant

[![Open your Home Assistant instance and show the blueprint import dialog.](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Floic-daigle%2Fsmart-scroll-remote%2Fblob%2Fmain%2Fhome_assistant%2Fsmart_remote_light_sync.yaml)

[`home_assistant/smart_remote_light_sync.yaml`](home_assistant/smart_remote_light_sync.yaml) keeps one remote channel and one real light, group or on/off device (switch, plug, fan, input boolean) in sync. Create **one automation per channel**.

- **Remote → light:** sent immediately, no debounce. The light always ends on the latest value.
- **Light → remote:** follows changes made elsewhere, with a short priority window for the remote so cloud lights' delayed echoes don't fight the dial.
- **No ping-pong:** nothing is sent when the other side already matches. Small tolerances absorb rounding differences.
- **No surprise changes on reconnect:** when the remote comes back online, it is updated *from* the light.
- **Colour temperature clamping** to what the receiving light supports.
- **Matter binding mode:** if a channel is bound directly to a Matter light, set **Sync direction** to "Light → remote only". The remote then controls the light itself, and the blueprint only keeps the remote's state up to date.
- **Optional multi-click actions:** pick the channel's event entity, then any action for 2, 3, 4 or 5 clicks.

### Dial actions (BILRESA-style)

[![Open your Home Assistant instance and show the blueprint import dialog.](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Floic-daigle%2Fsmart-scroll-remote%2Fblob%2Fmain%2Fhome_assistant%2Fsmart_remote_dial_actions.yaml)

Set a channel's type to **Dial events (no light)** and the dial no longer mirrors a light. Each turn is reported on that channel's *dial up* / *dial down* event entity as `multi_press_N` (N = notches since the last event, up to 10), and single clicks are reported too. [`home_assistant/smart_remote_dial_actions.yaml`](home_assistant/smart_remote_dial_actions.yaml) runs your own actions for up, down and 1–5 clicks, with `notches` and `steps` (signed) available in templates, e.g. `brightness_step_pct: "{{ steps * 5 }}"` or a media player's volume.

Requires Home Assistant 2024.10 or newer. After adding the per-channel click switches to an already-paired remote, use **Re-interview** on the device page so Home Assistant creates the event entities.

## Power and connectivity notes

These are the non-obvious parts of making a dual-stack sleepy remote work on ESP32-C6, in case they help someone else:

- **Automatic light sleep instead of manual sleep.** The firmware never calls `esp_light_sleep_start()`. Doing that freezes the radio, so the Thread parent drops the child after its timeout and every interaction starts with a rejoin. Instead, the UI holds a PM lock while active, and the power manager sleeps between Thread polls and Wi-Fi beacons.
- **`CONFIG_IEEE802154_SLEEP_ENABLE` requires `CONFIG_ESP_PHY_MAC_BB_PD` on ESP32-C6.** Without it, the radio comes back broken after the first light sleep, while the rest of the firmware keeps running.
- **Thread poll period in dual-stack builds.** On ESP32, the Wi-Fi `ConnectivityManager::_SetPollingInterval` (always `NOT_IMPLEMENTED`) hides the Thread one, so the ICD manager never sets the OpenThread poll period (`Failed to set Fast/Slow Polling Interval: err 2d`). An ICD state observer in [`firmware/main/matter.cpp`](firmware/main/matter.cpp) applies it directly with `otLinkSetPollPeriod`: 200 ms when active, 5 s when idle.
- **Correct network type in controllers.** The ESP32 diagnostics provider reports every network interface as operational, so Home Assistant showed the Thread remote as Wi-Fi. A small provider subclass marks the unused transport's interface as non-operational.
- **Rate-limited reporting.** Dial changes are published at most every 300 ms, always with the latest value. Matter's reliable messaging handles retries.
- **USB while sleeping.** The USB Serial/JTAG port can't work during light sleep. `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` keeps the chip awake whenever a USB host is connected.

## Project layout

```
firmware/                    ESP-IDF project (build from here)
  main/
    main.c                   hardware, input handling, sleep, Matter sync task, LED feedback
    matter.cpp / matter.h    Matter node, endpoints, transport personality, SDK workarounds
    menu_system_matter.cpp   controller → remote callbacks (values from Home Assistant)
    src/button_menu.c        button debouncing, menu state machine, LED rendering, NVS storage
    include/                 menu definitions, CHIP project config (device identity)
  partitions.csv             4 MB layout (single factory app, NVS, factory data)
  sdkconfig                  exact tested configuration
  sdkconfig.defaults         key configuration with explanations
  MATTER_INTEGRATION.md      early development notes (French, partly outdated)
hardware/
  pcb/                       schematic, PCB layout, fabrication files, BOM
  enclosure/                 CAD sources and 3D-print files
home_assistant/
  smart_remote_light_sync.yaml   Home Assistant blueprint
```

## License

© 2026 Loic Daigle

| Part | Licence |
|---|---|
| Firmware ([`firmware/`](firmware/)) and Home Assistant blueprint ([`home_assistant/`](home_assistant/)) | [MIT](LICENSE) |
| Hardware: PCB and enclosure ([`hardware/`](hardware/)) | [CERN-OHL-S v2](hardware/LICENSE) (strongly reciprocal) |

In short: do what you like with the code. If you modify and distribute the hardware, you must share your modified design files under the same CERN-OHL-S licence.
