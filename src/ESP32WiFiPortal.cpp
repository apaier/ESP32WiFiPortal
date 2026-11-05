#include "ESP32WiFiPortal.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_timer.h>

#include "DnsService.h"
#include "generated/embedded_assets.h"

namespace {
constexpr uint32_t kConnectAttemptSeconds = 30;
constexpr uint32_t kScanCadenceMs = 30UL * 1000UL;
constexpr uint32_t kScanTimeoutMs = 15UL * 1000UL;
constexpr uint32_t kBlindRetryFloorMs = 15UL * 60UL * 1000UL;
constexpr uint8_t kSoftFailEventsBeforeFallback = 10;
constexpr const char* kPrefsNamespace = "wifi_portal";
constexpr const char* kPrefNetworkKey = "network";
constexpr const char* kPrefPasswordKey = "password";
constexpr const char* kPrefModeKey = "mode";
constexpr const char* kPrefHostnameKey = "hostname";
constexpr const char* kPrefBatteryCalKey = "bat_cal";
// Resistor tolerance plus ADC error lands well inside this; a factor
// outside it means a wiring or divider-ratio mistake, not calibration.
constexpr float kMinBatteryCalibration = 0.5f;
constexpr float kMaxBatteryCalibration = 1.5f;
constexpr const char* kModeClient = "client";
constexpr const char* kModeRepeater = "repeater";
constexpr const char* kRepeaterApSuffix = "-ext";
constexpr const char* kHtmlAssetName = "WiFiSetup.html";
constexpr const char* kFallbackHostname = "esp32-portal";
constexpr uint16_t kOtaPort = 3232;
constexpr uint32_t kLedSlowBlinkMs = 1000;
constexpr uint32_t kLedFastBlinkMs = 125;
// Same check Android uses: a plain-HTTP endpoint that returns 204 only
// when the request really reached the internet (captive portals and
// dead upstreams answer differently or not at all).
constexpr const char* kProbeHost = "connectivitycheck.gstatic.com";
constexpr const char* kProbePath = "/generate_204";
constexpr uint32_t kProbeTimeoutMs = 3000;
constexpr uint32_t kProbeOkIntervalMs = 30UL * 1000UL;
constexpr uint32_t kProbeFailIntervalMs = 5UL * 1000UL;
// Arduino-ESP32's default-hostname buffer is 32 bytes including the NUL.
constexpr size_t kMaxHostnameLen = 31;

String jsonString(const String& s) {
    String out = "\"";
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((uint8_t)c < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

uint32_t ipToU32(const IPAddress& ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

// Reduce to a valid DNS label: lowercase [a-z0-9-], no leading or
// trailing hyphen. Spaces, underscores and dots become hyphens; anything
// else is dropped. May return an empty string.
String sanitizeHostname(const String& name) {
    String out;
    for (size_t i = 0; i < name.length() && out.length() < kMaxHostnameLen; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c == ' ' || c == '_' || c == '.') c = '-';
        bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!valid) continue;
        if (c == '-' && out.length() == 0) continue;
        out += c;
    }
    while (out.endsWith("-")) out.remove(out.length() - 1);
    return out;
}
}  // namespace

ESP32WiFiPortal::ESP32WiFiPortal(uint16_t port)
    : _server(port),
      _port(port),
      _bootLogEnabled(false),
      _ledPin(-1),
      _ledActiveLow(false),
      _ledOn(false),
      _internetOk(false),
      _connecting(false),
      _probeTask(nullptr),
      _mode(WiFiPortalMode::AccessPoint),
      _prefsReady(false),
      _pagesRegistered(false),
      _mdnsStarted(false),
      _otaStarted(false),
      _htmlOverride(nullptr),
      _htmlOverrideLen(0),
      _scanInFlight(false),
      _scanStartedMs(0),
      _lastScanMs(0),
      _softFailEventCount(0),
      _hardFail(false),
      _lastBlindRetryMs(0),
      _eventsRegistered(false),
      _disconnectEventPending(false),
      _disconnectReason(0) {}

void ESP32WiFiPortal::begin(const String& accessPointName,
                            const String& accessPointPassword) {
    _apName = accessPointName;
    _apPassword = accessPointPassword;

    // First, so a reset anywhere later in startup is still recorded.
    if (_bootLogEnabled && _bootLog.begin()) {
        Serial.print(_bootLog.format());
    }

    if (!ensurePreferences()) {
        return;
    }
#ifdef ESP32WIFIPORTAL_BATTERY
    // setBattery() usually runs before NVS is open.
    loadSavedBatteryCalibration();
#endif

    if (_ledPin >= 0) {
        pinMode(_ledPin, OUTPUT);
        digitalWrite(_ledPin, _ledActiveLow ? HIGH : LOW);
        startInternetProbe();
    }

    if (_defaultHostname.length() == 0) {
        _defaultHostname = sanitizeHostname(_apName);
    }
    if (_defaultHostname.length() == 0) _defaultHostname = kFallbackHostname;
    String savedHostname = loadSavedHostname();
    _hostname = savedHostname.length() > 0 ? savedHostname : _defaultHostname;
    // Set before the first WiFi.mode() so the STA/AP netifs are created
    // with this name and DHCP announces it.
    WiFi.setHostname(_hostname.c_str());

    // DnsService binds port 53 lazily on the first setMode(non-Off)
    // call (from startAccessPoint or startRepeaterAccessPoint, both
    // of which run WiFi.mode() first and therefore have an
    // initialized lwIP stack to talk to). Binding earlier than that
    // — e.g. here, before any WiFi call — trips an
    // xQueueSemaphoreTake assert because the network task hasn't been
    // created yet. Once bound, the socket stays bound for the rest of
    // the device's lifetime; only the dispatch mode flips.

    registerWiFiEvents();

    if (!loadAndConnect()) {
        startAccessPoint();
    }

    registerPages();
    _server.begin();
    startOta();
    startMdns();
}

void ESP32WiFiPortal::loop() {
    // Blocks for the duration of an upload, then reboots on success.
    if (_otaStarted) ArduinoOTA.handle();
    if (millis() >= BootLog::kStableAfterMs) _bootLog.mark(BootLog::Stage::Stable);
    _dns.loop();

    if (_scanInFlight) {
        pollScanCompletion();
    }

    if (_disconnectEventPending) {
        _disconnectEventPending = false;
        handleDisconnectEvent();
    }

    bool inApMode = (_mode == WiFiPortalMode::AccessPoint);
    bool haveCreds = haveSavedCredentials();
    if (inApMode && haveCreds && !_hardFail && !_scanInFlight) {
        if ((millis() - _lastScanMs) >= kScanCadenceMs) {
            startScan();
        } else if ((millis() - _lastBlindRetryMs) >= kBlindRetryFloorMs) {
            // Hidden SSIDs and DFS-channel APs may never appear in passive
            // scans, so a slow blind retry covers those.
            _lastBlindRetryMs = millis();
            attemptReconnect();
        }
    }

    _server.handleClient();
    updateLed();
}

WiFiPortalMode ESP32WiFiPortal::getMode() const { return _mode; }

bool ESP32WiFiPortal::isConnected() const { return WiFi.status() == WL_CONNECTED; }

IPAddress ESP32WiFiPortal::getIP() const {
    if (_mode == WiFiPortalMode::Connected || _mode == WiFiPortalMode::Repeating) {
        return WiFi.localIP();
    }
    return WiFi.softAPIP();
}

WebServer& ESP32WiFiPortal::server() { return _server; }

void ESP32WiFiPortal::on(const char* uri, WebServer::THandlerFunction handler) {
    _server.on(uri, handler);
}

void ESP32WiFiPortal::setLedPin(int pin, bool activeLow) {
    _ledPin = pin;
    _ledActiveLow = activeLow;
}

void ESP32WiFiPortal::updateLed() {
    if (_ledPin < 0) return;
    // Blink phase is derived from millis() so no toggle state is needed.
    uint32_t now = millis();
    bool online = (_mode != WiFiPortalMode::AccessPoint) &&
                  WiFi.status() == WL_CONNECTED && _internetOk;
    bool on;
    if (_connecting || (_mode != WiFiPortalMode::AccessPoint && !online)) {
        on = (now / kLedFastBlinkMs) % 2 == 0;
    } else if (_mode == WiFiPortalMode::AccessPoint) {
        on = (now / kLedSlowBlinkMs) % 2 == 0;
    } else {
        on = true;
    }
    if (on == _ledOn) return;
    _ledOn = on;
    digitalWrite(_ledPin, (on != _ledActiveLow) ? HIGH : LOW);
}

void ESP32WiFiPortal::startInternetProbe() {
    if (_probeTask != nullptr) return;
    // A blocking DNS lookup + TCP connect would stall loop() (and the
    // web server) for up to kProbeTimeoutMs, so it gets its own task.
    xTaskCreate(&ESP32WiFiPortal::probeTaskEntry, "wp_probe", 4096, this, 1,
                &_probeTask);
}

void ESP32WiFiPortal::probeTaskEntry(void* self) {
    static_cast<ESP32WiFiPortal*>(self)->runProbeLoop();
}

void ESP32WiFiPortal::runProbeLoop() {
    for (;;) {
        bool ok = WiFi.status() == WL_CONNECTED && probeInternet();
        if (ok != _internetOk) {
            Serial.printf("[ESP32WiFiPortal] Internet %s\n",
                          ok ? "reachable" : "unreachable");
        }
        _internetOk = ok;
        // STA_GOT_IP notifies us to re-check straight away.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? kProbeOkIntervalMs
                                                  : kProbeFailIntervalMs));
    }
}

bool ESP32WiFiPortal::probeInternet() {
    NetworkClient client;
    if (!client.connect(kProbeHost, 80, kProbeTimeoutMs)) return false;
    client.setTimeout(kProbeTimeoutMs);
    client.print(String("GET ") + kProbePath + " HTTP/1.1\r\nHost: " +
                 kProbeHost + "\r\nConnection: close\r\n\r\n");
    String statusLine = client.readStringUntil('\n');
    client.stop();
    // "HTTP/1.1 204 No Content"
    return statusLine.startsWith("HTTP/") && statusLine.indexOf(" 204") > 0;
}

void ESP32WiFiPortal::setHostname(const String& hostname) {
    _defaultHostname = sanitizeHostname(hostname);
}

String ESP32WiFiPortal::getHostname() const { return _hostname; }

void ESP32WiFiPortal::enableOta(const String& password) { _otaPassword = password; }

void ESP32WiFiPortal::enableBootLog() { _bootLogEnabled = true; }

#ifdef ESP32WIFIPORTAL_BATTERY
void ESP32WiFiPortal::setBattery(const BatteryConfig& config) {
    _batteryDefaultCalibration = config.calibration;
    _battery.begin(config);
    loadSavedBatteryCalibration();
}

void ESP32WiFiPortal::setBatteryPin(int pin) {
    BatteryConfig config = _battery.config();
    config.pin = pin;
    _battery.begin(config);
}

BatteryConfig ESP32WiFiPortal::getBattery() const { return _battery.config(); }

void ESP32WiFiPortal::loadSavedBatteryCalibration() {
    if (!_prefsReady || !_prefs.isKey(kPrefBatteryCalKey)) return;
    _battery.setCalibration(_prefs.getFloat(kPrefBatteryCalKey, _batteryDefaultCalibration));
}

// POST /battery with measured=<multimeter volts> saves measured / raw as
// the calibration; reset=1 drops it back to the sketch's value.
void ESP32WiFiPortal::handleBatteryCalibration() {
    if (_server.method() != HTTP_POST) {
        _server.send(405, "text/plain", "Use POST");
        return;
    }
    if (_server.hasArg("reset")) {
        if (_prefsReady) _prefs.remove(kPrefBatteryCalKey);
        _battery.setCalibration(_batteryDefaultCalibration);
        Serial.printf("[ESP32WiFiPortal] Battery calibration reset to %.4f\n",
                      _batteryDefaultCalibration);
    } else {
        float measured = _server.arg("measured").toFloat();
        float raw = _battery.active() ? _battery.readVolts() / _battery.config().calibration : 0.0f;
        float factor = (raw > 0.5f) ? measured / raw : 0.0f;
        if (factor < kMinBatteryCalibration || factor > kMaxBatteryCalibration) {
            _server.send(400, "text/html",
                         "<p>Calibration not saved: the board reads " + String(raw, 2) +
                             " V before calibration, so " + String(measured, 2) +
                             " V would need a factor outside 0.5&ndash;1.5. Check the "
                             "wiring and divider ratio.</p><p><a href=\"/\">Back</a></p>");
            return;
        }
        if (_prefsReady) _prefs.putFloat(kPrefBatteryCalKey, factor);
        _battery.setCalibration(factor);
        Serial.printf("[ESP32WiFiPortal] Battery calibration set to %.4f\n", factor);
    }
    _server.sendHeader("Location", "/", true);
    _server.send(303, "text/plain", "");
}
#endif

String ESP32WiFiPortal::batteryStatusText() const {
#ifdef ESP32WIFIPORTAL_BATTERY
    if (_battery.active()) {
        float volts = _battery.readVolts();
        return String(volts, 1) + " V &middot; " + _battery.percentFor(volts) + "%";
    }
#endif
    return String();
}

void ESP32WiFiPortal::setHtml(const uint8_t* html, size_t length) {
    _htmlOverride = html;
    _htmlOverrideLen = length;
}

bool ESP32WiFiPortal::ensurePreferences() {
    if (_prefsReady) return true;
    if (!_prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
        Serial.println("[ESP32WiFiPortal] Failed to open Preferences namespace");
        return false;
    }
    _prefsReady = true;
    Serial.println("[ESP32WiFiPortal] Preferences opened");
    return true;
}

bool ESP32WiFiPortal::haveSavedCredentials() {
    return _prefsReady && _prefs.isKey(kPrefNetworkKey);
}

bool ESP32WiFiPortal::loadAndConnect() {
    if (!haveSavedCredentials()) {
        Serial.println("[ESP32WiFiPortal] No saved credentials");
        return false;
    }
    String network = _prefs.getString(kPrefNetworkKey, String());
    String password = _prefs.getString(kPrefPasswordKey, String());
    String mode = _prefs.getString(kPrefModeKey, kModeClient);
    if (mode != kModeRepeater) mode = kModeClient;
    if (network.length() == 0) {
        Serial.println("[ESP32WiFiPortal] Saved network key is empty");
        return false;
    }
    return tryConnect(network, password, mode);
}

void ESP32WiFiPortal::registerWiFiEvents() {
    if (_eventsRegistered) return;
    Network.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                _internetOk = false;
                _disconnectReason = info.wifi_sta_disconnected.reason;
                _disconnectEventPending = true;
                if (_mode == WiFiPortalMode::Repeating) {
                    WiFi.AP.enableNAPT(false);
                }
                break;
            case ARDUINO_EVENT_WIFI_STA_LOST_IP:
                _internetOk = false;
                if (_mode == WiFiPortalMode::Repeating) {
                    WiFi.AP.enableNAPT(false);
                }
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                _softFailEventCount = 0;
                if (_probeTask != nullptr) xTaskNotifyGive(_probeTask);
                if (_mode == WiFiPortalMode::Repeating) {
                    // Force-reset NAPT: an enabled-but-stuck NAT table
                    // (e.g. left over from a reconnect where the STA
                    // briefly landed on a different subnet) won't be
                    // recovered by a second enable(true), so we toggle
                    // off then on to make lwIP rebuild the tables
                    // against the current STA IP/gateway.
                    WiFi.AP.enableNAPT(false);
                    WiFi.AP.enableNAPT(true);
                    refreshRepeaterUpstream();
                }
                break;
            case ARDUINO_EVENT_WIFI_AP_START:
                // softAP() returns before the new netif is fully up, so the
                // direct enableNAPT() call in startRepeaterAccessPoint can
                // race with the netif coming online. Re-enable here once
                // we see the AP_START event for the new SSID/IP.
                if (_mode == WiFiPortalMode::Repeating &&
                    WiFi.status() == WL_CONNECTED) {
                    WiFi.AP.enableNAPT(true);
                }
                break;
            default:
                break;
        }
    });
    _eventsRegistered = true;
}

bool ESP32WiFiPortal::isHardFailReason(uint8_t reason) const {
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return true;
        default:
            return false;
    }
}

void ESP32WiFiPortal::handleDisconnectEvent() {
    if (_mode != WiFiPortalMode::Connected &&
        _mode != WiFiPortalMode::Repeating) {
        return;
    }
    uint8_t reason = _disconnectReason;
    if (isHardFailReason(reason)) {
        Serial.printf("[ESP32WiFiPortal] Hard-fail disconnect (reason %u); to AP\n",
                      reason);
        _hardFail = true;
        // Stop the chip from auto-reassociating with bad credentials.
        WiFi.disconnect(false, true);
        startAccessPoint();
        return;
    }
    // Soft fail: the chip's auto-reconnect is now busy retrying. We can't
    // usefully scan while it's in that state (esp_wifi_scan_start is
    // rejected), so we just count consecutive failed attempts. A successful
    // STA_GOT_IP resets the counter from the event handler.
    _softFailEventCount++;
    Serial.printf("[ESP32WiFiPortal] Soft-fail disconnect (reason %u, %u/%u)\n",
                  reason, _softFailEventCount,
                  kSoftFailEventsBeforeFallback);
    if (_softFailEventCount >= kSoftFailEventsBeforeFallback) {
        Serial.println("[ESP32WiFiPortal] Soft-fail count exceeded; to AP");
        startAccessPoint();
    }
}

String ESP32WiFiPortal::loadSavedSsid() {
    if (!_prefsReady) return String();
    return _prefs.getString(kPrefNetworkKey, String());
}

String ESP32WiFiPortal::loadSavedHostname() {
    if (!_prefsReady || !_prefs.isKey(kPrefHostnameKey)) return String();
    return sanitizeHostname(_prefs.getString(kPrefHostnameKey, String()));
}

void ESP32WiFiPortal::applyHostname(const String& hostname) {
    String clean = sanitizeHostname(hostname);
    if (clean.length() == 0) clean = _defaultHostname;
    // Only persist a genuine override, so a later change to the sketch's
    // setHostname() default still takes effect on devices left at default.
    if (_prefsReady) {
        if (clean == _defaultHostname) {
            _prefs.remove(kPrefHostnameKey);
        } else {
            _prefs.putString(kPrefHostnameKey, clean);
        }
    }
    if (clean == _hostname) return;

    _hostname = clean;
    WiFi.setHostname(_hostname.c_str());
    // The netifs already exist, so update them directly. The STA's DHCP
    // client picks the new name up on its next lease request.
    WiFi.STA.setHostname(_hostname.c_str());
    WiFi.AP.setHostname(_hostname.c_str());
    startMdns();
}

void ESP32WiFiPortal::startMdns() {
    // The mDNS responder binds to the STA and AP netifs as they come and
    // go, so one start covers every mode. Restart only on rename.
    if (_mdnsStarted) {
        MDNS.end();
        _mdnsStarted = false;
    }
    if (!MDNS.begin(_hostname.c_str())) {
        Serial.println("[ESP32WiFiPortal] mDNS start failed");
        return;
    }
    MDNS.addService("http", "tcp", _port);
    if (_otaStarted) MDNS.enableArduino(kOtaPort, /*auth=*/true);
    _mdnsStarted = true;
    Serial.printf("[ESP32WiFiPortal] mDNS: http://%s.local/\n", _hostname.c_str());
}

void ESP32WiFiPortal::startOta() {
    if (_otaStarted) return;
    if (_otaPassword.length() == 0) {
        Serial.println("[ESP32WiFiPortal] OTA disabled (no password set)");
        return;
    }
    // ArduinoOTA's own mDNS handling would fight startMdns() on rename
    // (its MDNS.begin/end aren't aware of ours), so we advertise the
    // _arduino._tcp service ourselves.
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.setPort(kOtaPort);
    ArduinoOTA.setHostname(_hostname.c_str());
    ArduinoOTA.setPassword(_otaPassword.c_str());
    ArduinoOTA.onStart([]() { Serial.println("[ESP32WiFiPortal] OTA update started"); });
    ArduinoOTA.onEnd([]() { Serial.println("[ESP32WiFiPortal] OTA update complete; rebooting"); });
    ArduinoOTA.onError([](ota_error_t err) {
        Serial.printf("[ESP32WiFiPortal] OTA error %u\n", (unsigned)err);
    });
    ArduinoOTA.begin();
    _otaStarted = true;
    Serial.printf("[ESP32WiFiPortal] OTA listening on port %u\n", kOtaPort);
}

void ESP32WiFiPortal::startScan() {
    if (_scanInFlight) return;
    int16_t result = WiFi.scanNetworks(true /* async */);
    if (result == WIFI_SCAN_FAILED) {
        Serial.println("[ESP32WiFiPortal] scanNetworks failed to start");
        _lastScanMs = millis();
        return;
    }
    _scanInFlight = true;
    _scanStartedMs = millis();
}

void ESP32WiFiPortal::pollScanCompletion() {
    int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
        if ((millis() - _scanStartedMs) >= kScanTimeoutMs) {
            Serial.println("[ESP32WiFiPortal] Scan stuck; aborting");
            WiFi.scanDelete();
            _scanInFlight = false;
            _lastScanMs = millis();
        }
        return;
    }
    if (result < 0) {
        WiFi.scanDelete();
        _scanInFlight = false;
        _lastScanMs = millis();
        return;
    }
    handleScanResult(result);
    WiFi.scanDelete();
    _scanInFlight = false;
    _lastScanMs = millis();
}

void ESP32WiFiPortal::handleScanResult(int count) {
    if (_mode != WiFiPortalMode::AccessPoint) return;
    String saved = loadSavedSsid();
    if (saved.length() == 0) return;

    bool found = false;
    int32_t bestRssi = -127;
    for (int i = 0; i < count; ++i) {
        if (WiFi.SSID(i) == saved) {
            found = true;
            if (WiFi.RSSI(i) > bestRssi) bestRssi = WiFi.RSSI(i);
        }
    }
    if (found) {
        Serial.printf("[ESP32WiFiPortal] Saved SSID '%s' visible (RSSI %d); reconnecting\n",
                      saved.c_str(), (int)bestRssi);
        attemptReconnect();
    }
}

void ESP32WiFiPortal::attemptReconnect() {
    if (!haveSavedCredentials()) return;
    Serial.println("[ESP32WiFiPortal] Attempting saved network");
    if (!loadAndConnect()) {
        startAccessPoint();
    }
}

bool ESP32WiFiPortal::tryConnect(const String& network, const String& password,
                                 const String& mode) {
    _hardFail = false;
    _softFailEventCount = 0;
    // Switch DNS off while we transition. CaptivePortal-mode lying and
    // Forwarder-mode proxying both stop making sense the moment the AP
    // goes away; we'll re-arm whichever mode is appropriate once the
    // new state is established. The listener socket stays bound.
    _dns.setMode(DnsService::Mode::Off);
    _bootLog.mark(BootLog::Stage::RadioStarting);
    // Re-arm chip-level auto-reconnect so transient drops after a
    // successful association recover on their own (we may have left it
    // off after a prior AP fallback).
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(network.c_str(), password.c_str());
    Serial.print("[ESP32WiFiPortal] Connecting to ");
    Serial.print(network);

    // Poll in short steps so the LED keeps blinking while we wait.
    _connecting = true;
    uint32_t started = millis();
    uint32_t lastDot = started;
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - started) < kConnectAttemptSeconds * 1000UL) {
        if ((millis() - lastDot) >= 1000) {
            Serial.print('.');
            lastDot = millis();
        }
        updateLed();
        delay(50);
    }
    _connecting = false;
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ESP32WiFiPortal] Connection failed");
        return false;
    }

    _mode = WiFiPortalMode::Connected;
    _bootLog.mark(BootLog::Stage::NetworkUp);
    Serial.print("[ESP32WiFiPortal] Connected, IP ");
    Serial.println(WiFi.localIP());
    persistCredentials(network, password, mode);

    if (mode == kModeRepeater) {
        startRepeaterAccessPoint(network, password);
    }
    return true;
}

void ESP32WiFiPortal::startAccessPoint() {
    _mode = WiFiPortalMode::AccessPoint;
    _scanInFlight = false;
    _softFailEventCount = 0;
    // Defer the first scan/retry by their full cadence so the AP has time
    // to settle before we steal radio time again.
    _lastScanMs = millis();
    _lastBlindRetryMs = millis();
    _dns.setMode(DnsService::Mode::Off);
    Serial.println("[ESP32WiFiPortal] Starting Access Point");
    _bootLog.mark(BootLog::Stage::RadioStarting);

    // Halt the chip's auto-reassociation. Otherwise esp_wifi_scan_start()
    // is rejected while the chip is mid-connect-attempt, and our periodic
    // scan-and-pounce never gets a chance to run.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    // Tear down any prior softAP so leftover config from repeater mode
    // (non-default IP, upstream DNS in DHCP) doesn't carry through.
    WiFi.softAPdisconnect(false);
    // AP_STA keeps the STA radio available for periodic scans so the AP
    // doesn't have to be torn down to look for the saved network.
    WiFi.mode(WIFI_AP_STA);

    IPAddress apIp(192, 168, 4, 1);
    IPAddress mask(255, 255, 255, 0);
    IPAddress leaseStart(192, 168, 4, 2);
    WiFi.softAPConfig(apIp, apIp, mask, leaseStart, apIp);

    if (_apPassword.length() > 0) {
        WiFi.softAP(_apName.c_str(), _apPassword.c_str());
    } else {
        WiFi.softAP(_apName.c_str());
    }
    Serial.print("[ESP32WiFiPortal] AP IP ");
    Serial.println(WiFi.softAPIP());
    _bootLog.mark(BootLog::Stage::NetworkUp);

    _dns.setMode(DnsService::Mode::CaptivePortal, WiFi.softAPIP());

    // RFC 7710 / DHCP option 114: tell modern clients (iOS 14+, Android 11+,
    // macOS 11+) directly via DHCP that this is a captive portal and where
    // it lives, bypassing the per-SSID "this network has internet" cache
    // that otherwise suppresses the captive UI on rejoin.
    esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (apNetif != nullptr) {
        char uri[64];
        snprintf(uri, sizeof(uri), "http://%s/", WiFi.softAPIP().toString().c_str());
        esp_netif_dhcps_stop(apNetif);
        esp_err_t err = esp_netif_dhcps_option(
            apNetif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
            uri, strlen(uri));
        esp_netif_dhcps_start(apNetif);
        if (err == ESP_OK) {
            Serial.printf("[ESP32WiFiPortal] DHCP option 114 set: %s\n", uri);
        } else {
            Serial.printf("[ESP32WiFiPortal] DHCP option 114 failed: 0x%x\n", err);
        }
    }
}

IPAddress ESP32WiFiPortal::pickRepeaterSubnet() const {
    // Choose a /24 for the repeater AP that doesn't overlap the upstream
    // subnet. NAPT requires the two interfaces be on different networks.
    uint32_t staIp = ipToU32(WiFi.localIP());
    uint32_t staMask = ipToU32(WiFi.subnetMask());
    uint32_t staNet = staIp & staMask;
    uint32_t staBcast = staNet | ~staMask;

    struct Candidate { uint8_t a, b, c; };
    const Candidate candidates[] = {
        {192, 168, 244},
        {10, 244, 0},
        {172, 31, 244},
    };

    for (const auto& cand : candidates) {
        uint32_t candNet = ((uint32_t)cand.a << 24) |
                           ((uint32_t)cand.b << 16) |
                           ((uint32_t)cand.c << 8);
        uint32_t candBcast = candNet | 0xFFu;
        bool overlap = !(candBcast < staNet || staBcast < candNet);
        if (!overlap) {
            return IPAddress(cand.a, cand.b, cand.c, 1);
        }
    }
    return IPAddress(192, 168, 244, 1);
}

void ESP32WiFiPortal::refreshRepeaterUpstream() {
    // Repeater clients have DHCP DNS pointing at our own AP IP so they
    // hit the local DnsService regardless of upstream churn. Here we
    // just update the service's notion of where to proxy queries to
    // — whatever DHCP last gave the STA, falling back to a public
    // resolver if upstream didn't supply one. setMode with the same
    // mode re-applies the upstream parameter without rebinding sockets.
    IPAddress upstream = WiFi.dnsIP(0);
    if ((uint32_t)upstream == 0) upstream = IPAddress(8, 8, 4, 4);
    _dns.setMode(DnsService::Mode::Forwarder, _repeaterApIp, upstream);
}

void ESP32WiFiPortal::startRepeaterAccessPoint(const String& upstreamSsid,
                                               const String& upstreamPassword) {
    _repeaterApIp = pickRepeaterSubnet();
    _repeaterApSsid = upstreamSsid + kRepeaterApSuffix;
    IPAddress mask(255, 255, 255, 0);
    IPAddress leaseStart(_repeaterApIp[0], _repeaterApIp[1], _repeaterApIp[2], 2);
    // Hand the AP's own IP to clients as DNS. They send queries to us,
    // and the DnsForwarder below proxies them to whatever upstream the
    // STA currently has. This decouples client DNS from upstream-DNS
    // churn — no DHCP renew needed when upstream changes resolver.
    IPAddress upstreamDns = WiFi.dnsIP(0);
    if ((uint32_t)upstreamDns == 0) upstreamDns = IPAddress(8, 8, 4, 4);

    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAPConfig(_repeaterApIp, _repeaterApIp, mask, leaseStart,
                           _repeaterApIp)) {
        Serial.println("[ESP32WiFiPortal] softAPConfig failed; aborting repeater");
        return;
    }
    bool apUp = upstreamPassword.length() > 0
                    ? WiFi.softAP(_repeaterApSsid.c_str(), upstreamPassword.c_str())
                    : WiFi.softAP(_repeaterApSsid.c_str());
    if (!apUp) {
        Serial.println("[ESP32WiFiPortal] softAP failed; aborting repeater");
        return;
    }

    Serial.printf("[ESP32WiFiPortal] Repeater AP \"%s\" on %s\n",
                  _repeaterApSsid.c_str(),
                  _repeaterApIp.toString().c_str());

    _dns.setMode(DnsService::Mode::Forwarder, _repeaterApIp, upstreamDns);

    // Set the mode *before* enabling NAPT so the AP_START event handler
    // (which fires asynchronously after softAP() returns, once the new
    // netif is actually up) sees Repeating and re-enables NAPT cleanly.
    // The direct call below is best-effort: it covers the case where the
    // netif is already up by this point. Subsequent STA up/down is handled
    // by the global event handler in registerWiFiEvents().
    _mode = WiFiPortalMode::Repeating;
    WiFi.AP.enableNAPT(true);
}

void ESP32WiFiPortal::persistCredentials(const String& network,
                                         const String& password,
                                         const String& mode) {
    if (!_prefsReady) return;
    _prefs.putString(kPrefNetworkKey, network);
    _prefs.putString(kPrefPasswordKey, password);
    _prefs.putString(kPrefModeKey,
                     (mode == kModeRepeater) ? kModeRepeater : kModeClient);
}

void ESP32WiFiPortal::handleConfigPage() {
    if (_server.hasArg(kPrefNetworkKey)) {
        // Apply before tryConnect so the new name goes out with DHCP.
        if (_server.hasArg(kPrefHostnameKey)) {
            applyHostname(_server.arg(kPrefHostnameKey));
        }
        String submittedMode = _server.arg(kPrefModeKey);
        if (submittedMode != kModeRepeater) submittedMode = kModeClient;
        tryConnect(_server.arg(kPrefNetworkKey), _server.arg(kPrefPasswordKey),
                   submittedMode);
    }

    int networks = WiFi.scanNetworks();
    String options;
    for (int i = 0; i < networks; ++i) {
        options += "<option value=\"";
        options += WiFi.SSID(i);
        options += "\">";
        options += WiFi.SSID(i);
        options += " - ";
        options += WiFi.RSSI(i);
        options += "</option>";
    }

    const uint8_t* htmlBytes = _htmlOverride;
    size_t htmlLen = _htmlOverrideLen;
    if (htmlBytes == nullptr) {
        htmlBytes = getEmbeddedAsset(kHtmlAssetName, &htmlLen);
    }
    if (htmlBytes == nullptr || htmlLen == 0) {
        _server.send(500, "text/plain",
                     "Config HTML missing. Regenerate src/generated/ via "
                     "tools/embed_assets.py or call setHtml().");
        return;
    }
    String html(reinterpret_cast<const char*>(htmlBytes), htmlLen);

    String status;
    if (_mode == WiFiPortalMode::Repeating) {
        status = "Repeating " + WiFi.SSID() + " as \"" + _repeaterApSsid +
                 "\" (RSSI " + WiFi.RSSI() + ")";
    } else if (WiFi.status() == WL_CONNECTED) {
        status = "Connected to " + WiFi.SSID() + " (RSSI " + WiFi.RSSI() + ")";
    } else {
        status = "Access Point Mode - IP " + WiFi.softAPIP().toString();
    }

    bool isRepeater = (_mode == WiFiPortalMode::Repeating);
    html.replace("***options***", options);
    html.replace("***Connection Status***", status);
    html.replace("***client_checked***", isRepeater ? "" : "checked");
    html.replace("***repeater_checked***", isRepeater ? "checked" : "");
    html.replace("***hostname***", _hostname);
    html.replace("***battery***", batteryStatusText());
    String batteryHidden = "hidden";
    String batteryVolts;
    String batteryCalibration;
#ifdef ESP32WIFIPORTAL_BATTERY
    if (_battery.active()) {
        batteryHidden = "";
        batteryVolts = String(_battery.readVolts(), 2);
        batteryCalibration = String(_battery.config().calibration, 4);
    }
#endif
    html.replace("***battery_hidden***", batteryHidden);
    html.replace("***battery_volts***", batteryVolts);
    html.replace("***battery_calibration***", batteryCalibration);
    _server.send(200, "text/html", html);
}

void ESP32WiFiPortal::handleCaptiveRedirect() {
    if (_mode == WiFiPortalMode::AccessPoint) {
        String apIp = WiFi.softAPIP().toString();
        if (_server.hostHeader() != apIp) {
            _server.sendHeader("Location", String("http://") + apIp + "/", true);
            _server.send(302, "text/plain", "");
            return;
        }
    }
    _server.send(404, "text/plain", "Not Found");
}

void ESP32WiFiPortal::handleBootLog() {
    if (_server.hasArg("clear")) _bootLog.clear();
    _server.send(200, "text/plain", _bootLog.format());
}

void ESP32WiFiPortal::handleStatusJson() {
    bool staUp = (_mode != WiFiPortalMode::AccessPoint) &&
                 WiFi.status() == WL_CONNECTED;
    bool apUp = (_mode != WiFiPortalMode::Connected);
    const char* mode = _mode == WiFiPortalMode::AccessPoint ? "ap"
                       : _mode == WiFiPortalMode::Repeating ? "repeater"
                                                            : "client";

    String json = "{";
    json += "\"hostname\":" + jsonString(_hostname);
    json += ",\"mode\":\"" + String(mode) + "\"";
    json += ",\"uptime_s\":" + String((uint32_t)(esp_timer_get_time() / 1000000LL));
    json += ",\"ip\":" + jsonString(getIP().toString());
    json += ",\"ssid\":" + (staUp ? jsonString(WiFi.SSID()) : String("null"));
    json += ",\"rssi\":" + (staUp ? String(WiFi.RSSI()) : String("null"));
    // Only known when the LED's internet probe is running.
    json += ",\"internet\":" +
            String(_probeTask == nullptr ? "null" : (_internetOk ? "true" : "false"));
    json += ",\"ap_ssid\":" + (apUp ? jsonString(WiFi.softAPSSID()) : String("null"));
    json += ",\"ap_clients\":" + (apUp ? String(WiFi.softAPgetStationNum()) : String("null"));
#ifdef ESP32WIFIPORTAL_BATTERY
    if (_battery.active()) {
        float volts = _battery.readVolts();
        json += ",\"battery\":{\"volts\":" + String(volts, 2) +
                ",\"percent\":" + String(_battery.percentFor(volts)) +
                ",\"cells\":" + String(_battery.cells()) +
                ",\"calibration\":" + String(_battery.config().calibration, 4) + "}";
    } else {
        json += ",\"battery\":null";
    }
#endif
    json += "}";
    _server.send(200, "application/json", json);
}

void ESP32WiFiPortal::registerPages() {
    if (_pagesRegistered) return;
    auto handler = [this]() { this->handleConfigPage(); };
    _server.on("/", handler);
    _server.on("/wifi", handler);
    _server.on("/status.json", [this]() { this->handleStatusJson(); });
    if (_bootLogEnabled) {
        _server.on("/bootlog", [this]() { this->handleBootLog(); });
    }
#ifdef ESP32WIFIPORTAL_BATTERY
    _server.on("/battery", [this]() { this->handleBatteryCalibration(); });
#endif
    _server.onNotFound([this]() { this->handleCaptiveRedirect(); });
    _pagesRegistered = true;
}
