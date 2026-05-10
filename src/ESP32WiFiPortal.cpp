#include "ESP32WiFiPortal.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

namespace {
constexpr uint32_t kConnectAttemptSeconds = 30;
constexpr const char* kJsonNetworkKey = "network";
constexpr const char* kJsonPasswordKey = "password";
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
    _server.handleClient();
}

WiFiPortalMode ESP32WiFiPortal::getMode() const { return _mode; }

bool ESP32WiFiPortal::isConnected() const { return WiFi.status() == WL_CONNECTED; }

IPAddress ESP32WiFiPortal::getIP() const {
    return _mode == WiFiPortalMode::Connected ? WiFi.localIP() : WiFi.softAPIP();
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
    return tryConnect(network, password);
}

bool ESP32WiFiPortal::tryConnect(const String& network, const String& password) {
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
    persistCredentials(network, password);
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
}

void ESP32WiFiPortal::persistCredentials(const String& network, const String& password) {
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
    String payload;
    serializeJson(doc, payload);
    if (!file.print(payload)) {
        Serial.println("[ESP32WiFiPortal] Config write failed");
    }
    file.close();
}

void ESP32WiFiPortal::handleConfigPage() {
    if (_server.args() > 0) {
        tryConnect(_server.arg(kJsonNetworkKey), _server.arg(kJsonPasswordKey));
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
    if (WiFi.status() == WL_CONNECTED) {
        status = "Connected to " + WiFi.SSID() + " (RSSI " + WiFi.RSSI() + ")";
    } else {
        status = "Access Point Mode - IP " + WiFi.softAPIP().toString();
    }

    html.replace("***options***", options);
    html.replace("***Connection Status***", status);
    _server.send(200, "text/html", html);
}

void ESP32WiFiPortal::registerPages() {
    if (_pagesRegistered) return;
    auto handler = [this]() { this->handleConfigPage(); };
    _server.on("/", handler);
    _server.on("/wifi", handler);
    _pagesRegistered = true;
}
