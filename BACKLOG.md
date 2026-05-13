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

### 7. Move sensitive data to a secure storage approach — done
Credentials moved out of LittleFS plaintext (`/wifi.json`) into NVS via the Arduino `Preferences` API, under the namespace `wifi_portal`. NVS storage on its own is not encrypted — the move is a stepping stone that lets consumers opt into NVS / flash encryption project-side without code changes. The threat model and the encryption opt-in path are documented in [docs/secure-credentials.md](docs/secure-credentials.md). The follow-up "make encryption turn-key" work is tracked as #13.

### 13. Encrypted credential storage out of the box
#7 moves credentials into NVS, which is a stepping stone but is not encrypted by default — bits on flash are still readable by anyone who can dump it. Real protection needs NVS encryption (flash encryption on classic ESP32, or the HMAC-key variant on S2/S3/C3), which today is a consumer-side opt-in via partition table + sdkconfig. Investigate what a library can do to make this turn-key: detect at boot whether encryption is active and log a clear warning if not, ship a recommended partition table fragment / `sdkconfig.defaults` snippet, possibly provide a helper script. Document the threat model and the irreversible fuse-burning tradeoffs honestly.

### 14. STA connectivity watchdog with auto-recovery
Periodically probe whether the STA can actually reach the internet (e.g. a low-frequency TCP connect or HTTP HEAD against a known-good target) and, if probes fail for N consecutive intervals, force a WiFi reset — `WiFi.disconnect(false, true)` plus a fresh `tryConnect` from the saved credentials. Catches the "associated to a working SSID, but the upstream has lost its WAN" class of failure, which `STA_DISCONNECTED` events don't surface. Caveat: this is an STA-side probe, so it does **not** detect "NAPT forwarding is broken for AP clients" — the device itself can reach the internet in that case (its own traffic doesn't NAT). NAPT health needs a separate path, possibly watching the return value of `WiFi.AP.enableNAPT()` and a counter on `ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED` packets that never come back. Decide whether to expose probe target/interval as constants in code or via UI.

### 15. Runtime DNS forwarder for repeater clients
Today repeater-mode AP clients get a static DNS server in their DHCP lease (whatever upstream's `dnsIP(0)` returned at the moment `softAPConfig` ran). If upstream's DNS changes — or the STA briefly lands on a different subnet during reconnect and we capture a DNS that's no longer reachable via NAPT — clients silently fail to resolve until they rejoin the AP. Real repeaters fix this by advertising their own AP IP as DNS and running a minimal forwarder that proxies queries to the current upstream resolver. The library already carries a `DNSServer` instance for the captive-portal redirect; teach it a forwarder mode for repeater operation.

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
