# Smart Scroll Remote: a DIY Matter scroll-wheel remote (Thread + Wi-Fi)

A battery-powered **Matter** remote built on the **ESP32-C6**, in the spirit of the IKEA BILRESA scroll remote. One rotary encoder, one push button and three RGB LEDs control **three independent channels**. Each channel can be a dimmer, a colour light, an on/off switch, or a free "dial" for media volume, blinds, scenes and similar.

**Full source code, firmware, Home Assistant blueprints and documentation:**
👉 **https://github.com/loic-daigle/smart-scroll-remote**

---

## Highlights

- **Matter over Thread _or_ Wi-Fi, in one firmware.** The commissioner chooses the network when you pair the remote. On Thread, the remote is a real **sleepy end device**: it sleeps between uses but stays on the network, so the first turn of the dial is answered in about a second.
- **3 channels**, each with a type set from Home Assistant:
  - **On/Off**: a click or a turn switches the light on or off
  - **Dimmable**: brightness only
  - **Dimmable + white**: brightness and colour temperature
  - **Dimmable + colour**: brightness, colour temperature and colour
  - **Dial events**: no light; the dial and the clicks are reported as events, like the BILRESA wheel
- **Two-way sync.** The remote's dial and LEDs follow the real light, even when the light is changed from an app, by voice or by an automation.
- **Multi-click events** (2 to 5 clicks) on every channel, confirmed by an LED pattern.
- **Matter bindings**: a channel can also control Matter lights or a Matter group directly, without a hub.
- **Low power.** Light sleep 5 s after the last touch. Deep sleep after an adjustable idle time (10 min to 12 h, or never). The battery level is reported to the controller, and the LEDs blink red when it is low.
- **Home Assistant ready.** The blueprints sync each channel with any light, light group, switch or plug, including Tuya, and add click and dial actions without writing YAML by hand.

---

## How it works

| Input | Main screen | Inside a channel |
|---|---|---|
| Rotate | Choose channel 1 / 2 / 3 | Change the setting (brightness, colour temperature, colour) |
| Click | Open the selected channel | Next page (brightness → colour temperature → colour) |
| Long press | Preview all three channels | Back to the main screen |
| 2–5 clicks | — | Multi-press event for that channel |

When the remote is asleep, turning the dial wakes it directly on the brightness page of the current channel. Pressing the button wakes it and toggles the channel.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32-C6 module | Espressif ESP32-C6-MINI-class module, 4 MB flash; built-in USB Serial/JTAG for flashing |
| Rotary encoder | EC21A1520401 (15 pulses / 30 detents) or a similar quadrature encoder |
| Push button | Active low |
| 3 × WS2812 (NeoPixel) | On a switchable supply, so they draw nothing while asleep |
| Li-ion / LiPo cell | Voltage measured through a 1:2 divider |

### Pinout

| Function | GPIO | Notes |
|---|---|---|
| Button | 0 | Active low, internal pull-up, wake source |
| Battery voltage | 1 | ADC, via 1:2 divider (3.0 V = 0 %, 4.2 V = 100 %) |
| Encoder A | 2 | Internal pull-up, wake source |
| Encoder B | 3 | Internal pull-up, wake source |
| NeoPixel data | 6 | |
| NeoPixel power enable | 7 | Active low (LOW = LEDs powered) |
| BOOT / download mode | 9 | Hold LOW at power-up to force flashing mode |

### Design notes

> **Keep the antenna clear.** The module's PCB antenna needs about **15 mm of clearance** from metal, ground planes and the battery. A battery or a metal part on top of or in front of the antenna costs a lot of range. In the logs it shows up as many radio retries (`Frame tx attempt … failed, error:NoAck`). Keep the antenna end of the module free, or use a module with an external antenna connector.

- Power the LEDs through the enable pin (GPIO 7), so they can be switched off completely during sleep.
- Bring **GPIO 9** and **GND** out to a pad or test point. Holding GPIO 9 low at power-up is the recovery path if the chip ever stops answering on USB.

---

## Firmware

The firmware is built with **ESP-IDF v5.4.1** and **esp-matter**. The tested versions, the exact build configuration and the step-by-step build instructions (including building under Windows + WSL) are in the GitHub repository:

👉 [Building and flashing](https://github.com/loic-daigle/smart-scroll-remote#building)

In short:

```bash
git clone https://github.com/loic-daigle/smart-scroll-remote.git
cd smart-scroll-remote/firmware
idf.py build
idf.py -p <PORT> flash
```

---

## Pairing

The firmware uses the public **Matter test credentials**, which is fine for a personal device:

| | |
|---|---|
| Manual pairing code | **3497-011-2332** |
| Vendor / Product ID | 0xFFF1 / 0x8001 (test IDs) |

When it is not paired, the remote advertises over Bluetooth at power-up. Most controllers warn that the device is uncertified; accept the warning.

**Home Assistant users:** the current Matter Server rejects test certificates by default. Pairing goes through Bluetooth and Thread, then fails at the very end with "error code 1". Turn on **Enable test-net DCL** in the Matter Server's configuration and restart it, then pair again. The [pairing guide](https://github.com/loic-daigle/smart-scroll-remote#pairing-with-home-assistant) has the full procedure and the factory-reset command.

---

## Home Assistant

Two blueprints are included (one-click import buttons are in the repository):

- **Light Sync** keeps a channel and a real light, group or on/off device in sync, in both directions, with as little lag as possible. It also scales colour temperature to the bulb's range, offers a transition setting, and handles Tuya quirks.
- **Dial actions** runs your own actions when a "Dial events" channel is turned up or down (with the number of notches) or clicked 1 to 5 times.

👉 [Home Assistant setup](https://github.com/loic-daigle/smart-scroll-remote#home-assistant)

---

## License

- **Hardware** (this PCB and the enclosure): [CERN-OHL-S v2](https://ohwr.org/cern_ohl_s_v2.txt)
- **Firmware and Home Assistant files**: [MIT](https://github.com/loic-daigle/smart-scroll-remote/blob/main/LICENSE)

Questions, issues and improvements are welcome on GitHub:
**https://github.com/loic-daigle/smart-scroll-remote/issues**
