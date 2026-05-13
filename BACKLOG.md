# Backlog

Tracked here until the repo is published on GitHub, then migrated to Issues.

## Functional features

### 1. Reconfigure WiFi from UI while already connected — done
`handleConfigPage` processes form submissions in any mode, and `tryConnect` is mode-agnostic. Hitting `/` from a connected client triggers a fresh association attempt without manually wiping credentials. The remaining gap is discoverability — finding the device's IP without inspecting the router — which is covered by #6 (mDNS).

### 2. UI control for "reconnect every X minutes" — superseded
The reconnect path was reworked to be event-driven (soft-fail disconnect count) on the STA side and scan-cadenced on the AP side, so there's no single "interval" knob a user would tune. The tunable constants that survive (`kScanCadenceMs`, `kSoftFailEventsBeforeFallback`, `kBlindRetryFloorMs`) live in code rather than the UI. See [docs/wifi-recovery.md](docs/wifi-recovery.md).

### 3. Periodic reconnect from AP mode when STA credentials exist — done
Scan-and-pounce in AP mode: every `kScanCadenceMs` (30s) we run an async scan for the saved SSID; on a match, `attemptReconnect` runs and switches to Station mode on success. A 15-minute blind-retry floor handles hidden SSIDs and DFS-channel APs that don't appear in passive scans. The captive-portal `DNSServer` is explicitly stopped on AP→STA in `tryConnect` and re-started on STA→AP in `startAccessPoint`. See [docs/wifi-recovery.md](docs/wifi-recovery.md).

### 4. Configurable AP credentials via UI
Today the AP is open and its name is whatever the sketch passes to `begin()`. Add fields for AP name + password and persist them.

### 6. mDNS / configurable hostname
Optional `setHostname()` and mDNS announcement so the device is reachable as `mydevice.local` on the network. Also covers the discoverability gap from #1 — once connected, users need a stable name to reach the config form without hunting through the router's DHCP table.

### 7. Move sensitive data to a secure storage approach
WiFi credentials currently sit in plaintext at `/wifi.json` on LittleFS. Move at minimum to NVS with encryption enabled, or use the ESP32 flash encryption / secure-boot story. Decide whether the same applies to AP password, API keys, or other consumer-supplied secrets.

## Library / packaging

### 8. WiFiRepeater example — done (0.2.0)
Implemented in-library rather than as a separate example: the config form has a Client / Repeater radio button, and selecting Repeater brings up an AP on a non-overlapping `/24` (SSID `<upstream>-extender`, same password as upstream) with NAPT forwarding via `WiFi.AP.enableNAPT`. STA disconnect/reconnect toggles NAPT through a `Network.onEvent` handler.

### 9. Arduino Library Manager submission
Once the API has settled, submit to the Arduino IDE Library Manager index so users can install via the IDE's library browser.

### 10. PlatformIO Registry submission
Same idea for `pio pkg publish`.

### 11. CI build of examples
GitHub Action that compiles every example sketch against current `espressif32` to catch breakage on platform updates.

### 12. Migration note for WiFiSetup.cpp consumers
The Temperature Device project has a hand-ported `WiFiSetup.cpp` that this library is derived from. Document the API differences and the recommended switch.
