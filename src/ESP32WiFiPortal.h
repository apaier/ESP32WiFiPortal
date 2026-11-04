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
#include <IPAddress.h>
#include <Preferences.h>
#include <WebServer.h>

#include "BatteryMonitor.h"
#include "BootLog.h"
#include "DnsService.h"

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
    // Status LED, -1 disables (default). Solid when online with working
    // internet, fast blink when connecting or joined without internet,
    // slow blink in AP (config portal) mode. Internet is checked by a
    // background task fetching an HTTP 204 endpoint every 30 s (5 s
    // while it's failing); the check only runs when an LED is set.
    void setLedPin(int pin, bool activeLow = false);

    // Hostname used for DHCP and the mDNS announcement (<name>.local).
    // Defaults to a sanitized copy of the AP name passed to begin(). A
    // name saved from the config form overrides this default. Names are
    // lowercased and reduced to [a-z0-9-], max 31 characters.
    void setHostname(const String& hostname);
    String getHostname() const;

    // Accept firmware over the network (ArduinoOTA on UDP/TCP 3232),
    // advertised over mDNS as <hostname>.local. An empty password leaves
    // OTA disabled; the library refuses to run OTA unauthenticated.
    void enableOta(const String& password);

    // Record each boot's reset reason and how far startup got in NVS,
    // print the history at begin(), and serve it at /bootlog
    // (/bootlog?clear=1 wipes it). For diagnosing resets that happen
    // away from a serial monitor, e.g. brownouts on battery power.
    void enableBootLog();

#ifdef ESP32WIFIPORTAL_BATTERY
    // Report supply-battery voltage and estimated charge on the status
    // card and in /status.json. Only available when the build defines
    // ESP32WIFIPORTAL_BATTERY; see BatteryMonitor.h for wiring. Both
    // setters can be called before or after begin() and take effect
    // immediately. A pin of -1 turns monitoring off.
    void setBattery(const BatteryConfig& config);
    void setBatteryPin(int pin);
    BatteryConfig getBattery() const;
#endif

    // Override the config-form HTML at runtime. Defaults to the build-time
    // embedded copy of data/WiFiSetup.html. The pointer must remain valid
    // for the lifetime of the portal; the bytes are not copied.
    void setHtml(const uint8_t* html, size_t length);

private:
    WebServer _server;
    uint16_t _port;
    DnsService _dns;
    BootLog _bootLog;
#ifdef ESP32WIFIPORTAL_BATTERY
    BatteryMonitor _battery;
#endif
    bool _bootLogEnabled;
    Preferences _prefs;
    int _ledPin;
    bool _ledActiveLow;
    bool _ledOn;
    // Written by the probe task / event handler, read by loop().
    volatile bool _internetOk;
    volatile bool _connecting;
    TaskHandle_t _probeTask;
    WiFiPortalMode _mode;
    bool _prefsReady;
    bool _pagesRegistered;
    String _apName;
    String _apPassword;
    String _defaultHostname;
    String _hostname;
    bool _mdnsStarted;
    String _otaPassword;
    bool _otaStarted;
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
    void refreshRepeaterUpstream();
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
    String loadSavedHostname();
    void applyHostname(const String& hostname);
    void startMdns();
    void startOta();
    void updateLed();
    void startInternetProbe();
    static void probeTaskEntry(void* self);
    void runProbeLoop();
    bool probeInternet();
    bool tryConnect(const String& network, const String& password,
                    const String& mode);
    void persistCredentials(const String& network, const String& password,
                            const String& mode);
    void handleConfigPage();
    void handleCaptiveRedirect();
    void handleBootLog();
    void handleStatusJson();
    String batteryStatusText() const;
    void registerPages();
};

#endif // ESP32_WIFI_PORTAL_H
