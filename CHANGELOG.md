# Changelog

## 0.2.0 - Unreleased

- Repeater mode. The config form now has a Client / Repeater radio. When Repeater is selected and the upstream join succeeds, the device brings up an AP on a non-overlapping `/24` (default candidates `192.168.244.0/24`, `10.244.0.0/24`, `172.31.244.0/24`), SSID `<upstream>-extender`, same password as upstream, with NAPT forwarding enabled via `WiFi.AP.enableNAPT`. Falls back to AP config mode unchanged if the upstream join fails.
- A `Network.onEvent` handler toggles NAPT on `STA_GOT_IP` / `STA_DISCONNECTED` / `STA_LOST_IP` so a brief upstream outage doesn't strand the AP with stale forwarding state.
- Added `WiFiPortalMode::Repeating` state. `getIP()` returns the STA IP in this state; the captive-portal DNSServer, redirect, and DHCP option 114 do not run.
- HTML config page gained two new substitution tokens: `***client_checked***` and `***repeater_checked***`. `/wifi.json` gained a `mode` field (`"client"` or `"repeater"`); missing values default to `"client"` so existing installs upgrade cleanly.

## 0.1.0 - Unreleased

Initial release.

- Class-based WiFi configuration portal for ESP32.
- LittleFS-backed credential storage at `/wifi.json`.
- HTML config page served from LittleFS at `/WiFiSetup.html`.
- Owns its own `WebServer`; consumers can register additional routes.
- Configurable AP name/password, LED indicator pin, HTML and config paths.
- Captive-portal redirect in AP mode: hijacks DNS to `softAPIP` and 302-redirects unknown hosts to the config page so phones / laptops auto-open the portal on connect.
- Sets DHCP option 114 (`ESP_NETIF_CAPTIVEPORTAL_URI`, RFC 7710) so iOS 14+, Android 11+, and macOS 11+ pop the captive UI without relying on the per-SSID probe cache.
- Targets Arduino-ESP32 3.x via the `pioarduino/platform-espressif32` fork (the official PlatformIO platform still ships Arduino-ESP32 2.0.17 / IDF 4.4.7, which lacks the captive-portal URI option).
- Example: `BasicConfigPortal`.

Derived from `WiFiSetup.cpp` in the [Temperature Device](https://github.com/apaier/Temperature-Device) project, which itself was based on the `WiFiRepeater.ino` prototype.
