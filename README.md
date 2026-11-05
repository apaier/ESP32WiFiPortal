# ESP32WiFiPortal

A reusable WiFi configuration portal for ESP32 sketches. Drop it into a project and you get a working WiFi setup web UI on first boot, with no boilerplate.

## What it does

- On boot, opens an NVS namespace via the `Preferences` API and reads saved credentials.
- If credentials exist, attempts to join the network (30s timeout).
- If no credentials exist, or the join fails, starts an open Access Point and serves a configuration page on `/` and `/wifi`.
- Submitting the form retries connection. On success, saves credentials to NVS and switches the device into Station mode — or into Repeater mode, if that was selected on the form (see below).
- Exposes the underlying `WebServer` so consumers can register their own routes (status pages, REST endpoints, etc).

The config-form HTML lives in the library's `data/` directory and is compiled into the firmware at build time as a PROGMEM byte array, so there is no filesystem-upload step. See [docs/embedded-assets.md](docs/embedded-assets.md) for how that works and how to customise the form.

Credentials are stored in NVS rather than on a user-visible filesystem, but **NVS itself is not encrypted by default** — bits on flash are still readable by anyone with a flash dump. See [docs/secure-credentials.md](docs/secure-credentials.md) for the threat model and the consumer-side opt-in for real encryption.

### Hostname and mDNS

The device announces itself over mDNS as `<hostname>.local` (with an `_http._tcp` service record) and sends the same name to the DHCP server, so once it has joined a network you can reach the config form at `http://<hostname>.local/` without looking up its IP.

- The default hostname is the AP name passed to `begin()`, lowercased and reduced to letters, digits and hyphens (`ESP32-Portal-Setup` → `esp32-portal-setup`). Call `setHostname()` before `begin()` to choose a different default.
- The config form has a **Device name** field. A name entered there is saved to NVS (key `hostname`) and overrides the sketch default; clearing the field reverts to the default.
- Names are capped at 31 characters.

### OTA updates

Call `enableOta(password)` before `begin()` to accept firmware over WiFi via ArduinoOTA (port 3232), advertised over mDNS as `<hostname>.local`. OTA always requires a password; an empty one leaves it disabled. While an upload runs, the portal stops serving pages, and the device reboots into the new firmware when it finishes.

In this repo's PlatformIO setup the password lives in a git-ignored `secrets.ini`:

1. `cp secrets.ini.example secrets.ini` and set `ota_password` (letters, digits, `-` and `_`). Set `ota_host` if you changed the hostname.
2. Flash once over USB so the board runs an OTA-enabled build: `pio run -e esp32dev -t upload`.
3. From then on, flash over WiFi: `pio run -e esp32dev_ota -t upload`.

Without `secrets.ini` the project still builds, with OTA disabled. The password is compiled into the firmware, so anyone with the binary or a flash dump can recover it.

### Status LED

`setLedPin(pin)` drives an LED (pass `activeLow = true` for LEDs wired to light on LOW):

| LED | Meaning |
|---|---|
| Solid | Joined the saved network (client or repeater) and the internet is reachable |
| Fast blink | Connecting, or joined but the internet isn't reachable |
| Slow blink | Running as the setup access point |

Internet reachability is checked by a background task that fetches `http://connectivitycheck.gstatic.com/generate_204` (the endpoint Android uses) every 30 s, or every 5 s while it's failing, and immediately after the device gets an IP. The check only runs when an LED pin is set. Some DevKit boards have a blue LED on GPIO2; the ESP32-DevKitC has none, so the example drives an external LED on GPIO23 (GPIO23 → 220–330 Ω resistor → LED → GND).

### Status JSON

`GET /status.json` returns a snapshot of the device:

```json
{"hostname":"esp32-portal","mode":"repeater","uptime_s":86412,"ip":"192.168.1.42",
 "ssid":"HomeNet","rssi":-58,"internet":true,"ap_ssid":"HomeNet-ext","ap_clients":2,
 "battery":{"volts":15.82,"percent":72,"cells":4,"calibration":1.0180}}
```

`mode` is `ap`, `client` or `repeater`. `ssid` / `rssi` are `null` when not joined to a network, `ap_ssid` / `ap_clients` are `null` when no AP is running, and `internet` is `null` unless the status LED (and so the internet probe) is enabled. `battery` is present only in builds with battery monitoring, and is `null` if it isn't configured.

### Battery monitoring (optional, compile-time)

For boards running from a battery through a DC-DC converter, the library can report the battery's voltage and estimated charge on the setup page's status card and in `/status.json`. It is compiled in only when the build defines `ESP32WIFIPORTAL_BATTERY`:

```ini
build_flags = -DESP32WIFIPORTAL_BATTERY
```

Wiring: a divider from battery + to an ADC1 pin (GPIO32–39; ADC2 doesn't work while WiFi is on), with battery − sharing ground with the ESP32. For a battery up to ~20 V, 100 kΩ (top) and 15 kΩ (to ground) keep the pin under 2.6 V. Add 0.1 µF from the pin to ground. Check that your DC-DC converter isn't isolated — if battery − and board GND aren't connected, the reading won't work.

```cpp
BatteryConfig battery;
battery.pin = 34;
battery.dividerRatio = (100.0f + 15.0f) / 15.0f;  // (R1 + R2) / R2
battery.cells = 4;                                // in series
battery.curve = kBatteryCurveLiIon;               // or kBatteryCurveLiFePO4, kBatteryCurveLeadAcid
battery.calibration = 1.0f;                       // multimeter volts / reported volts
portal.setBattery(battery);
```

`setBattery()` and `setBatteryPin()` can be called before or after `begin()` and take effect immediately, so the pin can be changed at runtime; `setBatteryPin(-1)` turns monitoring off. Pins that aren't on ADC1 are rejected with a serial message and monitoring stays off.

To calibrate without reflashing, use the **Battery calibration** card on the setup page (shown only when monitoring is active). With the board running, measure the battery terminals with a multimeter, enter that reading, and press Calibrate; the board saves `measured ÷ uncalibrated reading` to NVS (key `bat_cal`), and that saved value overrides `battery.calibration` from the sketch until you press Reset. Factors outside 0.5–1.5 are rejected, since that points to a wiring or divider-ratio mistake rather than tolerance. The card posts to `/battery` (`measured=<volts>`, or `reset=1`), and the current factor is in `/status.json`. Labeled resistor values are fine for `dividerRatio`: calibration absorbs both resistor tolerance and ADC error.

Chemistry and cell count are configuration, not code: pick a built-in curve or pass your own `BatteryCurve` (an array of `{cellVolts, percent}` points, highest voltage first). Percent is estimated from resting voltage per cell, so it reads low under heavy load and high while charging. Expect about ±1–2% voltage accuracy after setting `calibration` against a multimeter. Low battery is reported only; the library takes no action.

### Boot log

`enableBootLog()` keeps the last 8 boots in NVS (namespace `wp_bootlog`): the reset reason that started each boot and how far startup got (`booted` → `radio starting` → `network up` → `stable` after 60 s). The history is printed over serial at `begin()` and served as plain text at `/bootlog`; `/bootlog?clear=1` wipes it. Use it to diagnose resets that happen away from a serial monitor: run the board on its real power supply, then reconnect over USB and read the history. Repeated `BROWNOUT` entries stuck at `radio starting` mean the supply sags when WiFi transmits.

### Repeater mode

The config form has a Client / Repeater radio button. When Repeater is chosen and the upstream join succeeds, the device also brings up its own AP and NAT-forwards traffic from clients of that AP to the upstream network.

- AP SSID defaults to `<upstream-SSID>-ext`.
- AP password is the same as the upstream password (or open, if upstream is open).
- AP subnet is picked at boot to avoid overlap with the upstream (candidates: `192.168.244.0/24`, `10.244.0.0/24`, `172.31.244.0/24`).
- The captive-portal DNS / redirect / DHCP option 114 only run in the standalone config-AP fallback, never in Repeater mode.
- Reach the config page from any client of the repeater AP, or from the upstream LAN at the device's STA IP, to change mode or upstream network.

## Requirements

This library targets **Arduino-ESP32 3.x** (ESP-IDF v5.1+) so it can use `ESP_NETIF_CAPTIVEPORTAL_URI` (RFC 7710 / DHCP option 114) for reliable captive-portal detection on iOS, Android, and macOS. The official `platformio/platform-espressif32` still ships Arduino-ESP32 2.0.17, so use the [`pioarduino/platform-espressif32`](https://github.com/pioarduino/platform-espressif32) community fork until that catches up.

The build also needs **Python 3** on `PATH` to regenerate `src/generated/` from `data/`. PlatformIO already requires Python for its own toolchain, so this is normally free; Arduino IDE users only need it if they intend to edit files under `data/`.

## Install

### PlatformIO

In your project's `platformio.ini`:

```ini
[env:esp32dev]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip
board = esp32doit-devkit-v1
framework = arduino
lib_deps =
    https://github.com/apaier/ESP32WiFiPortal.git
```

No `board_build.filesystem` line and no `uploadfs` step — the HTML is compiled in. The `tools/embed_assets.py` regenerator runs automatically as part of the library build via the `build.extraScript` hook in `library.json`.

During local development before publishing, point at a checkout instead:

```ini
lib_extra_dirs = /Users/you/Documents/Arduino/ESP32
lib_deps = ESP32WiFiPortal
```

### Arduino IDE

Clone this repository into `~/Documents/Arduino/libraries/ESP32WiFiPortal/`, then restart the IDE. No external library dependencies.

The Arduino IDE has no pre-build hook a library can rely on, so `src/generated/embedded_assets.{h,cpp}` are checked into the repo and built as-is. If you edit anything under `data/`, run the regenerator once before compiling:

```sh
python3 tools/embed_assets.py
```

(PlatformIO users don't need this — the same script runs automatically during the build.)

## Quickstart

```cpp
#include <ESP32WiFiPortal.h>

ESP32WiFiPortal portal;

void setup() {
    Serial.begin(115200);
    portal.begin("MyDevice-Setup");      // SSID used when in AP mode
}

void loop() {
    portal.loop();
}
```

That's it. No filesystem upload.

## Adding your own pages

```cpp
ESP32WiFiPortal portal;

void handleStatus() {
    portal.server().send(200, "application/json", "{\"ok\":true}");
}

void setup() {
    portal.begin("MyDevice-Setup");
    portal.on("/status", handleStatus);     // convenience pass-through
    // or use portal.server() directly for any WebServer feature
}

void loop() { portal.loop(); }
```

The `BasicConfigPortal` example has a fuller one: a `/devices` page listing the devices connected to the board's own WiFi network.

## API

```cpp
ESP32WiFiPortal(uint16_t port = 80);

void begin(const String& accessPointName,
           const String& accessPointPassword = String());
void loop();

WiFiPortalMode getMode() const;       // AccessPoint | Connected | Repeating
bool           isConnected() const;
IPAddress      getIP() const;

WebServer&     server();
void           on(const char* uri, WebServer::THandlerFunction handler);

void setLedPin(int pin, bool activeLow = false); // optional status LED (see below)
void setHostname(const String& hostname);         // DHCP / mDNS name (<name>.local)
String getHostname() const;
void enableOta(const String& password);           // OTA updates; empty = disabled
void enableBootLog();                             // reset history at boot + /bootlog
void setBattery(const BatteryConfig& config);     // needs -DESP32WIFIPORTAL_BATTERY
void setBatteryPin(int pin);                      //   change pin at runtime, -1 = off
BatteryConfig getBattery() const;
void setHtml(const uint8_t* html, size_t length); // override the compiled-in form
```

## Customising the HTML

`data/WiFiSetup.html` is the source of truth for the config form. Edit it like any other file in the repository, then rebuild — PlatformIO regenerates `src/generated/` automatically; Arduino IDE users run `python3 tools/embed_assets.py` first. See [docs/embedded-assets.md](docs/embedded-assets.md) for details on the `data/` → PROGMEM pipeline and on dropping additional assets in.

The library substitutes these placeholders at request time:

- `***options***` — `<option>` tags for each scanned network.
- `***Connection Status***` — current connection state.
- `***client_checked***` / `***repeater_checked***` — `checked` injected into whichever mode radio is currently saved.
- `***hostname***` — the current device hostname (without `.local`).
- `***battery***` — battery voltage and charge (e.g. `15.8 V · 72%`), or empty when battery monitoring is off.
- `***battery_hidden***` — `hidden` when battery monitoring is off, otherwise empty; put it on the calibration form's tag.
- `***battery_volts***` / `***battery_calibration***` — current battery voltage (2 decimals) and calibration factor for the calibration card, or empty when monitoring is off.

You can replace the file with any markup you want as long as those placeholders and a form posting `network`, `password`, and `mode` to `/wifi` are present. The `hostname` field is optional; if a form omits it, the saved hostname is left unchanged.

If you'd rather not touch `data/` at all, call `portal.setHtml(myHtmlBytes, myHtmlLen)` before `begin()` to override the compiled-in copy at runtime. The pointer must remain valid for the lifetime of the portal.

## Examples

- **BasicConfigPortal** — minimal sketch showing the default behaviour, including Client/Repeater mode selection on the config form.

## Documentation

- [docs/secure-credentials.md](docs/secure-credentials.md) — where credentials are stored, what NVS does and doesn't protect, how to opt into real encryption.
- [docs/embedded-assets.md](docs/embedded-assets.md) — how the `data/` directory is compiled into the firmware, and how to add or override assets.
- [docs/wifi-recovery.md](docs/wifi-recovery.md) — connection-loss recovery, scan-and-pounce, hard/soft fail classification.

## Roadmap

Planned features and known gaps are tracked in [GitHub Issues](https://github.com/apaier/ESP32WiFiPortal/issues).

## License

GPL-3.0. See [LICENSE](LICENSE).
