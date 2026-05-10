# ESP32WiFiPortal

A reusable WiFi configuration portal for ESP32 sketches. Drop it into a project and you get a working WiFi setup web UI on first boot, with no boilerplate.

## What it does

- On boot, mounts LittleFS and reads `/wifi.json` for saved credentials.
- If credentials exist, attempts to join the network (30s timeout).
- If no credentials exist, or the join fails, starts an open Access Point and serves a configuration page on `/` and `/wifi`.
- Submitting the form retries connection. On success, saves credentials to LittleFS and switches the device into Station mode.
- Exposes the underlying `WebServer` so consumers can register their own routes (status pages, REST endpoints, etc).

## Requirements

This library targets **Arduino-ESP32 3.x** (ESP-IDF v5.1+) so it can use `ESP_NETIF_CAPTIVEPORTAL_URI` (RFC 7710 / DHCP option 114) for reliable captive-portal detection on iOS, Android, and macOS. The official `platformio/platform-espressif32` still ships Arduino-ESP32 2.0.17, so use the [`pioarduino/platform-espressif32`](https://github.com/pioarduino/platform-espressif32) community fork until that catches up.

## Install

### PlatformIO

In your project's `platformio.ini`:

```ini
[env:esp32dev]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip
board = esp32doit-devkit-v1
framework = arduino
board_build.filesystem = littlefs
lib_deps =
    https://github.com/apaier/ESP32WiFiPortal.git
    bblanchon/ArduinoJson@^7.0.0
```

During local development before publishing, point at a checkout instead:

```ini
lib_extra_dirs = /Users/you/Documents/Arduino/ESP32
lib_deps = ESP32WiFiPortal
```

### Arduino IDE

Clone this repository into `~/Documents/Arduino/libraries/ESP32WiFiPortal/`, then restart the IDE. ArduinoJson must be installed separately via Library Manager.

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

Then upload `data/WiFiSetup.html` to LittleFS:

- PlatformIO: `pio run -t uploadfs`
- Arduino IDE: use the LittleFS upload plugin (`arduino-esp32fs-plugin` for ESP32).

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

WiFiPortalMode getMode() const;       // AccessPoint | Connected
bool           isConnected() const;
IPAddress      getIP() const;

WebServer&     server();
void           on(const char* uri, WebServer::THandlerFunction handler);

void setLedPin(int pin);              // optional connection indicator
void setHtmlPath(const String& path); // default "/WiFiSetup.html"
void setConfigPath(const String& path);// default "/wifi.json"
```

## Customising the HTML

The library reads `/WiFiSetup.html` from LittleFS at request time and substitutes two placeholders:

- `***options***` — `<option>` tags for each scanned network.
- `***Connection Status***` — current connection state.

You can replace the file with any markup you want as long as those two placeholders and a form posting `network` + `password` to `/wifi` are present.

## Examples

- **BasicConfigPortal** — minimal sketch showing the default behaviour.
- **WiFiRepeater** — *(planned)* full AP+STA repeater with NAT, ported from the [ESP32-Wifi-Repeater](https://github.com/apaier/ESP32-Wifi-Repeater) project. Pending the AP+STA support listed in [BACKLOG.md](BACKLOG.md).

## Roadmap

See [BACKLOG.md](BACKLOG.md) for upcoming features. These will move to GitHub Issues once the repo is published.

## License

GPL-3.0. See [LICENSE](LICENSE).
