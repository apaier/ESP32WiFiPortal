/*
  ESP32WiFiPortal - Captive-portal style WiFi configuration for ESP32

  On begin(), reads saved credentials from NVS (via the Preferences API)
  and tries to join the configured network. If no credentials exist or
  the join fails, the device starts an open Access Point and serves a
  configuration page so a user can pick a network and submit credentials.
  The HTML form is compiled into the firmware from data/WiFiSetup.html
  at build time. The underlying WebServer is exposed so consumers can
  add their own handlers.

  Copyright (C) 2026 Andrew Paier

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
*/

#ifndef ESP32_WIFI_PORTAL_H
#define ESP32_WIFI_PORTAL_H

#include <Arduino.h>
#include <DNSServer.h>
#include <IPAddress.h>
#include <Preferences.h>
#include <WebServer.h>

enum class WiFiPortalMode { AccessPoint, Connected, Repeating };

class ESP32WiFiPortal {
public:
    explicit ESP32WiFiPortal(uint16_t port = 80);

    // Open the Preferences namespace, attempt to join the saved network,
    // register routes, and start the WebServer. accessPointName is used
    // as the SSID when the device falls back to AP mode.
    void begin(const String& accessPointName,
               const String& accessPointPassword = String());

    // Pump the WebServer. Call from the sketch's loop().
    void loop();

    // Status accessors
    WiFiPortalMode getMode() const;
    bool isConnected() const;
    IPAddress getIP() const;

    // Extension: hand out the underlying server so consumers can register
    // additional routes, or use on() as a convenience pass-through.
    WebServer& server();
    void on(const char* uri, WebServer::THandlerFunction handler);

    // Optional configuration. Call before begin().
    void setLedPin(int pin);                          // -1 disables (default)

    // Override the config-form HTML at runtime. Defaults to the build-time
    // embedded copy of data/WiFiSetup.html. The pointer must remain valid
    // for the lifetime of the portal; the bytes are not copied.
    void setHtml(const uint8_t* html, size_t length);

private:
    WebServer _server;
    DNSServer _dns;
    Preferences _prefs;
    int _ledPin;
    WiFiPortalMode _mode;
    bool _prefsReady;
    bool _pagesRegistered;
    String _apName;
    String _apPassword;
    const uint8_t* _htmlOverride;
    size_t _htmlOverrideLen;
    String _repeaterApSsid;
    IPAddress _repeaterApIp;

    // Scan-and-pounce reconnect state. Set from loop() except where noted.
    bool _scanInFlight;
    uint32_t _scanStartedMs;
    uint32_t _lastScanMs;
    uint8_t _softFailEventCount;
    bool _hardFail;
    uint32_t _lastBlindRetryMs;
    bool _eventsRegistered;
    // Written by the WiFi event task, read by loop().
    volatile bool _disconnectEventPending;
    volatile uint8_t _disconnectReason;

    bool ensurePreferences();
    bool haveSavedCredentials();
    bool loadAndConnect();
    void registerWiFiEvents();
    void refreshRepeaterDhcpDns();
    void startScan();
    void pollScanCompletion();
    void handleScanResult(int count);
    void handleDisconnectEvent();
    bool isHardFailReason(uint8_t reason) const;
    String loadSavedSsid();
    void attemptReconnect();
    void startAccessPoint();
    void startRepeaterAccessPoint(const String& upstreamSsid,
                                  const String& upstreamPassword);
    IPAddress pickRepeaterSubnet() const;
    bool tryConnect(const String& network, const String& password,
                    const String& mode);
    void persistCredentials(const String& network, const String& password,
                            const String& mode);
    void handleConfigPage();
    void handleCaptiveRedirect();
    void registerPages();
};

#endif // ESP32_WIFI_PORTAL_H
