# Backlog

Tracked here until the repo is published on GitHub, then migrated to Issues.

## Functional features

### 1. Reconfigure WiFi from UI while already connected
The current form is most useful in AP mode. Once connected, hitting `/` should still let the user pick a different network and trigger a new connection attempt without manually wiping credentials.

### 2. UI control for "reconnect every X minutes"
A numeric field on the config page that sets a reconnect interval, persisted to LittleFS. `0` disables.

### 3. Periodic reconnect from AP mode when STA credentials exist
When the device falls back to AP mode but `/wifi.json` exists, retry the saved network on the configured interval (#2). On success, switch to Station mode.

When this lands, also stop the captive-portal `DNSServer` on the AP→STA transition (and re-start it on any STA→AP fallback). Today it's started once in `startAccessPoint()` and never stopped, which is correct only because the mode is fixed at boot.

### 4. Configurable AP credentials via UI
Today the AP is open and its name is whatever the sketch passes to `begin()`. Add fields for AP name + password and persist them.

### 6. mDNS / configurable hostname
Optional `setHostname()` and mDNS announcement so the device is reachable as `mydevice.local` on the network.

### 7. Move sensitive data to a secure storage approach
WiFi credentials currently sit in plaintext at `/wifi.json` on LittleFS. Move at minimum to NVS with encryption enabled, or use the ESP32 flash encryption / secure-boot story. Decide whether the same applies to AP password, API keys, or other consumer-supplied secrets.

## Library / packaging

### 8. WiFiRepeater example
Port [ESP32-Wifi-Repeater](https://github.com/apaier/ESP32-Wifi-Repeater) (`WiFiExtender.ino`) to use this library as its config-portal layer, with the sketch supplying the AP+STA + NAPT bits the library doesn't own. Likely depends on a "library doesn't manage WiFi.mode()" hook or option.

### 9. Arduino Library Manager submission
Once the API has settled, submit to the Arduino IDE Library Manager index so users can install via the IDE's library browser.

### 10. PlatformIO Registry submission
Same idea for `pio pkg publish`.

### 11. CI build of examples
GitHub Action that compiles every example sketch against current `espressif32` to catch breakage on platform updates.

### 12. Migration note for WiFiSetup.cpp consumers
The Temperature Device project has a hand-ported `WiFiSetup.cpp` that this library is derived from. Document the API differences and the recommended switch.
