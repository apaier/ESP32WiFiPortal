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

### Repeater mode

The config form has a Client / Repeater radio button. When Repeater is chosen and the upstream join succeeds, the device also brings up its own AP and NAT-forwards traffic from clients of that AP to the upstream network.

- AP SSID defaults to `<upstream-SSID>-extender`.
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

void setLedPin(int pin);                          // optional connection indicator
void setHtml(const uint8_t* html, size_t length); // override the compiled-in form
```

## Customising the HTML

`data/WiFiSetup.html` is the source of truth for the config form. Edit it like any other file in the repository, then rebuild — PlatformIO regenerates `src/generated/` automatically; Arduino IDE users run `python3 tools/embed_assets.py` first. See [docs/embedded-assets.md](docs/embedded-assets.md) for details on the `data/` → PROGMEM pipeline and on dropping additional assets in.

The library substitutes four placeholders at request time:

- `***options***` — `<option>` tags for each scanned network.
- `***Connection Status***` — current connection state.
- `***client_checked***` / `***repeater_checked***` — `checked` injected into whichever mode radio is currently saved.

You can replace the file with any markup you want as long as those placeholders and a form posting `network`, `password`, and `mode` to `/wifi` are present.

If you'd rather not touch `data/` at all, call `portal.setHtml(myHtmlBytes, myHtmlLen)` before `begin()` to override the compiled-in copy at runtime. The pointer must remain valid for the lifetime of the portal.

## Examples

- **BasicConfigPortal** — minimal sketch showing the default behaviour, including Client/Repeater mode selection on the config form.

## Documentation

- [docs/secure-credentials.md](docs/secure-credentials.md) — where credentials are stored, what NVS does and doesn't protect, how to opt into real encryption.
- [docs/embedded-assets.md](docs/embedded-assets.md) — how the `data/` directory is compiled into the firmware, and how to add or override assets.
- [docs/wifi-recovery.md](docs/wifi-recovery.md) — connection-loss recovery, scan-and-pounce, hard/soft fail classification.

## Roadmap

See [BACKLOG.md](BACKLOG.md) for upcoming features. These will move to GitHub Issues once the repo is published.

## License

GPL-3.0. See [LICENSE](LICENSE).
