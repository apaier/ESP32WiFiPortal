#include "ESP32WiFiPortal.h"

#include <WiFi.h>
#include <esp_netif.h>

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
constexpr const char* kModeClient = "client";
constexpr const char* kModeRepeater = "repeater";
constexpr const char* kRepeaterApSuffix = "-extender";
constexpr const char* kHtmlAssetName = "WiFiSetup.html";

uint32_t ipToU32(const IPAddress& ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}
}  // namespace

ESP32WiFiPortal::ESP32WiFiPortal(uint16_t port)
    : _server(port),
      _ledPin(-1),
      _mode(WiFiPortalMode::AccessPoint),
      _prefsReady(false),
      _pagesRegistered(false),
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

    if (!ensurePreferences()) {
        return;
    }

    registerWiFiEvents();

    if (!loadAndConnect()) {
        startAccessPoint();
    }

    registerPages();
    _server.begin();
}

void ESP32WiFiPortal::loop() {
    if (_mode == WiFiPortalMode::AccessPoint) {
        _dns.processNextRequest();
    }

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

void ESP32WiFiPortal::setLedPin(int pin) { _ledPin = pin; }

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
                _disconnectReason = info.wifi_sta_disconnected.reason;
                _disconnectEventPending = true;
                if (_mode == WiFiPortalMode::Repeating) {
                    WiFi.AP.enableNAPT(false);
                }
                break;
            case ARDUINO_EVENT_WIFI_STA_LOST_IP:
                if (_mode == WiFiPortalMode::Repeating) {
                    WiFi.AP.enableNAPT(false);
                }
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                _softFailEventCount = 0;
                if (_mode == WiFiPortalMode::Repeating) {
                    // Force-reset NAPT: an enabled-but-stuck NAT table
                    // (e.g. left over from a reconnect where the STA
                    // briefly landed on a different subnet) won't be
                    // recovered by a second enable(true), so we toggle
                    // off then on to make lwIP rebuild the tables
                    // against the current STA IP/gateway.
                    WiFi.AP.enableNAPT(false);
                    WiFi.AP.enableNAPT(true);
                    refreshRepeaterDhcpDns();
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
    // Re-arm chip-level auto-reconnect so transient drops after a
    // successful association recover on their own (we may have left it
    // off after a prior AP fallback).
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(network.c_str(), password.c_str());
    Serial.print("[ESP32WiFiPortal] Connecting to ");
    Serial.print(network);

    uint32_t waited = 0;
    while (WiFi.status() != WL_CONNECTED && waited < kConnectAttemptSeconds) {
        Serial.print('.');
        delay(1000);
        waited++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ESP32WiFiPortal] Connection failed");
        return false;
    }

    _dns.stop();
    _mode = WiFiPortalMode::Connected;
    if (_ledPin >= 0) digitalWrite(_ledPin, HIGH);
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
    Serial.println("[ESP32WiFiPortal] Starting Access Point");

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

    _dns.stop();
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", WiFi.softAPIP());

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

void ESP32WiFiPortal::refreshRepeaterDhcpDns() {
    // The AP's DHCP server caches whatever DNS we passed to softAPConfig
    // at first setup. If the STA later (re)associates and learns a
    // different upstream DNS — or, in the failure case from the field,
    // the STA briefly landed on a different subnet during reconnect and
    // we captured a DNS that no longer routes — clients keep getting
    // the stale value until they rejoin. Pull the current STA-side DNS
    // and re-stamp the AP's DHCP DNS option without restarting the AP.
    IPAddress dns = WiFi.dnsIP(0);
    if ((uint32_t)dns == 0) dns = IPAddress(8, 8, 4, 4);
    esp_netif_t* apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (apNetif == nullptr) return;

    esp_netif_dns_info_t info = {};
    info.ip.type = ESP_IPADDR_TYPE_V4;
    info.ip.u_addr.ip4.addr = static_cast<uint32_t>(dns);
    esp_netif_dhcps_stop(apNetif);
    esp_err_t err = esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &info);
    esp_netif_dhcps_start(apNetif);
    if (err != ESP_OK) {
        Serial.printf("[ESP32WiFiPortal] DHCP DNS refresh failed: 0x%x\n", err);
    } else {
        Serial.printf("[ESP32WiFiPortal] DHCP DNS set to %s\n",
                      dns.toString().c_str());
    }
}

void ESP32WiFiPortal::startRepeaterAccessPoint(const String& upstreamSsid,
                                               const String& upstreamPassword) {
    _repeaterApIp = pickRepeaterSubnet();
    _repeaterApSsid = upstreamSsid + kRepeaterApSuffix;
    IPAddress mask(255, 255, 255, 0);
    IPAddress leaseStart(_repeaterApIp[0], _repeaterApIp[1], _repeaterApIp[2], 2);
    // Hand clients whatever DNS upstream gave us. Without this the DHCP
    // server advertises the AP IP itself, which black-holes lookups because
    // we don't run a resolver. Fall back to 8.8.4.4 if upstream didn't
    // supply one (unlikely on a working STA association).
    IPAddress dns = WiFi.dnsIP(0);
    if ((uint32_t)dns == 0) dns = IPAddress(8, 8, 4, 4);

    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAPConfig(_repeaterApIp, _repeaterApIp, mask, leaseStart, dns)) {
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

void ESP32WiFiPortal::registerPages() {
    if (_pagesRegistered) return;
    auto handler = [this]() { this->handleConfigPage(); };
    _server.on("/", handler);
    _server.on("/wifi", handler);
    _server.onNotFound([this]() { this->handleCaptiveRedirect(); });
    _pagesRegistered = true;
}
