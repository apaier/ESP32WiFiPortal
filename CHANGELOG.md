# Changelog

## 0.3.0 - Unreleased

- Replaced the Arduino-ESP32 `DNSServer` instance with an in-library `DnsService` (`src/DnsService.{h,cpp}`) that owns port 53 for the device's lifetime and dispatches by mode: `Off`, `CaptivePortal` (lie with the AP IP — drives iOS/Android/macOS captive-network detection), or `Forwarder` (proxy to upstream, match responses by DNS transaction ID). The reason for the rewrite is a real bug seen in the field: `DNSServer::stop()` on current pioarduino builds doesn't synchronously release its lwIP socket, so any subsequent component trying to bind port 53 — even on a specific local IP — got `EADDRINUSE`. Holding port 53 once at `ESP32WiFiPortal::begin()` and never rebinding sidesteps the timing problem.
- Repeater clients now get the device's own AP IP as DNS (advertised via DHCP) instead of upstream's resolver. Queries land on `DnsService` in Forwarder mode, which proxies to whichever upstream the STA last learned from DHCP and is retargeted invisibly on every `STA_GOT_IP` — surviving upstream-DNS changes without forcing clients to rejoin or wait for DHCP lease renewal. See [docs/wifi-recovery.md](docs/wifi-recovery.md).
- Renamed the private `refreshRepeaterDhcpDns` helper to `refreshRepeaterUpstream`; it now just retargets `DnsService` rather than re-stamping a DHCP option.

## 0.2.0 - Unreleased

- **Breaking:** credentials moved from `/wifi.json` on LittleFS to NVS via the Arduino `Preferences` API, under the namespace `wifi_portal` with keys `network`, `password`, `mode`. Existing 0.1.x installs need to re-enter credentials once after the upgrade — there is no on-device migration path, by design (carrying it would have required keeping the LittleFS dependency for one transitional release). NVS storage is not encrypted by default; see [docs/secure-credentials.md](docs/secure-credentials.md) for the threat model and the consumer-side opt-in for real encryption.
- **Breaking:** `data/WiFiSetup.html` (and anything else under `data/`) is now compiled into the firmware at build time as a `PROGMEM` byte array via [tools/embed_assets.py](tools/embed_assets.py). No filesystem-upload step is required (no more `pio run -t uploadfs`). PlatformIO runs the regenerator automatically via `library.json`'s `build.extraScript`; Arduino IDE users must run `python3 tools/embed_assets.py` after editing anything in `data/`. The generated `src/generated/embedded_assets.{h,cpp}` are checked into the repo so out-of-the-box Arduino IDE builds work.
- **Breaking:** removed `setHtmlPath()` and `setConfigPath()`. The HTML is now identified by its name under `data/` (default `WiFiSetup.html`); credentials live in NVS under a fixed namespace.
- Added `setHtml(const uint8_t* html, size_t length)` for callers who want to override the compiled-in form at runtime without editing `data/`.
- Library no longer depends on `ArduinoJson` or `LittleFS`. Net flash usage is smaller in most consumer projects.
- Repeater mode. The config form now has a Client / Repeater radio. When Repeater is selected and the upstream join succeeds, the device brings up an AP on a non-overlapping `/24` (default candidates `192.168.244.0/24`, `10.244.0.0/24`, `172.31.244.0/24`), SSID `<upstream>-extender`, same password as upstream, with NAPT forwarding enabled via `WiFi.AP.enableNAPT`. Falls back to AP config mode unchanged if the upstream join fails.
- A `Network.onEvent` handler toggles NAPT on `STA_GOT_IP` / `STA_DISCONNECTED` / `STA_LOST_IP` so a brief upstream outage doesn't strand the AP with stale forwarding state.
- Added `WiFiPortalMode::Repeating` state. `getIP()` returns the STA IP in this state; the captive-portal DNSServer, redirect, and DHCP option 114 do not run.
- HTML config page gained two new substitution tokens: `***client_checked***` and `***repeater_checked***`. The persisted record gained a `mode` field (`"client"` or `"repeater"`); missing values default to `"client"`.
- Repeater recovery hardening: on `STA_GOT_IP` in Repeating mode, NAPT is now toggled off→on (a plain re-enable is a no-op against a stuck NAT table) and the AP's DHCP DNS option is re-stamped from the current `WiFi.dnsIP(0)` via `esp_netif_set_dns_info`. Catches the "STA briefly landed on the wrong subnet during reconnect, captured a now-unreachable DNS, NAT tables pointed at a dead gateway" failure mode that left clients associated but cut off. The `AP_START` event also re-enables NAPT to cover the synchronous-softAP-vs-async-netif race on initial setup. See [docs/wifi-recovery.md](docs/wifi-recovery.md).

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
