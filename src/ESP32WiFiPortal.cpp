#include "ESP32WiFiPortal.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_netif.h>

namespace {
constexpr uint32_t kConnectAttemptSeconds = 30;
constexpr const char* kJsonNetworkKey = "network";
constexpr const char* kJsonPasswordKey = "password";
constexpr const char* kJsonModeKey = "mode";
constexpr const char* kModeClient = "client";
constexpr const char* kModeRepeater = "repeater";
constexpr const char* kRepeaterApSuffix = "-extender";

uint32_t ipToU32(const IPAddress& ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}
}  // namespace

ESP32WiFiPortal::ESP32WiFiPortal(uint16_t port)
    : _server(port),
      _ledPin(-1),
      _mode(WiFiPortalMode::AccessPoint),
      _fsReady(false),
      _pagesRegistered(false),
      _htmlPath("/WiFiSetup.html"),
      _configPath("/wifi.json") {}

void ESP32WiFiPortal::begin(const String& accessPointName,
                            const String& accessPointPassword) {
    _apName = accessPointName;
    _apPassword = accessPointPassword;

    if (!ensureFilesystem()) {
        return;
    }

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
void ESP32WiFiPortal::setHtmlPath(const String& path) { _htmlPath = path; }
void ESP32WiFiPortal::setConfigPath(const String& path) { _configPath = path; }

bool ESP32WiFiPortal::ensureFilesystem() {
    if (_fsReady) return true;
    if (!LittleFS.begin(true)) {
        Serial.println("[ESP32WiFiPortal] Failed to mount LittleFS");
        return false;
    }
    _fsReady = true;
    Serial.println("[ESP32WiFiPortal] LittleFS mounted");
    return true;
}

bool ESP32WiFiPortal::loadAndConnect() {
    if (!LittleFS.exists(_configPath)) {
        Serial.println("[ESP32WiFiPortal] No saved credentials");
        return false;
    }
    File file = LittleFS.open(_configPath, FILE_READ);
    if (!file) {
        Serial.println("[ESP32WiFiPortal] Failed to open config file");
        return false;
    }
    String content = file.readString();
    file.close();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) {
        Serial.print("[ESP32WiFiPortal] Config parse error: ");
        Serial.println(err.c_str());
        return false;
    }

    String network = doc[kJsonNetworkKey].as<String>();
    String password = doc[kJsonPasswordKey].as<String>();
    String mode = doc[kJsonModeKey].as<String>();
    if (mode != kModeRepeater) mode = kModeClient;
    return tryConnect(network, password, mode);
}

bool ESP32WiFiPortal::tryConnect(const String& network, const String& password,
                                 const String& mode) {
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
    Serial.println("[ESP32WiFiPortal] Starting Access Point");
    WiFi.mode(WIFI_AP);
    if (_apPassword.length() > 0) {
        WiFi.softAP(_apName.c_str(), _apPassword.c_str());
    } else {
        WiFi.softAP(_apName.c_str());
    }
    Serial.print("[ESP32WiFiPortal] AP IP ");
    Serial.println(WiFi.softAPIP());

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

void ESP32WiFiPortal::startRepeaterAccessPoint(const String& upstreamSsid,
                                               const String& upstreamPassword) {
    _repeaterApIp = pickRepeaterSubnet();
    _repeaterApSsid = upstreamSsid + kRepeaterApSuffix;
    IPAddress mask(255, 255, 255, 0);

    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAPConfig(_repeaterApIp, _repeaterApIp, mask)) {
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

    // Toggle NAPT on STA up/down so a brief upstream outage doesn't strand
    // the AP with stale forwarding state.
    Network.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
        (void)info;
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                WiFi.AP.enableNAPT(true);
                Serial.println("[ESP32WiFiPortal] NAPT enabled");
                break;
            case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                WiFi.AP.enableNAPT(false);
                Serial.println("[ESP32WiFiPortal] NAPT disabled");
                break;
            default:
                break;
        }
    });
    WiFi.AP.enableNAPT(true);

    _mode = WiFiPortalMode::Repeating;
}

void ESP32WiFiPortal::persistCredentials(const String& network,
                                         const String& password,
                                         const String& mode) {
    if (!_fsReady) return;
    LittleFS.remove(_configPath);
    File file = LittleFS.open(_configPath, FILE_WRITE);
    if (!file) {
        Serial.println("[ESP32WiFiPortal] Failed to open config for writing");
        return;
    }
    JsonDocument doc;
    doc[kJsonNetworkKey] = network;
    doc[kJsonPasswordKey] = password;
    doc[kJsonModeKey] = (mode == kModeRepeater) ? kModeRepeater : kModeClient;
    String payload;
    serializeJson(doc, payload);
    if (!file.print(payload)) {
        Serial.println("[ESP32WiFiPortal] Config write failed");
    }
    file.close();
}

void ESP32WiFiPortal::handleConfigPage() {
    if (_server.hasArg(kJsonNetworkKey)) {
        String submittedMode = _server.arg(kJsonModeKey);
        if (submittedMode != kModeRepeater) submittedMode = kModeClient;
        tryConnect(_server.arg(kJsonNetworkKey), _server.arg(kJsonPasswordKey),
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

    File file = LittleFS.open(_htmlPath, FILE_READ);
    if (!file) {
        _server.send(500, "text/plain",
                     "Config HTML missing. Upload " + _htmlPath + " to LittleFS.");
        return;
    }
    String html = file.readString();
    file.close();

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
