# Changelog

## 0.1.0 - Unreleased

Initial release.

- Class-based WiFi configuration portal for ESP32.
- LittleFS-backed credential storage at `/wifi.json`.
- HTML config page served from LittleFS at `/WiFiSetup.html`.
- Owns its own `WebServer`; consumers can register additional routes.
- Configurable AP name/password, LED indicator pin, HTML and config paths.
- Example: `BasicConfigPortal`.

Derived from `WiFiSetup.cpp` in the [Temperature Device](https://github.com/apaier/Temperature-Device) project, which itself was based on the `WiFiRepeater.ino` prototype.
