/*
  BasicConfigPortal - minimal use of ESP32WiFiPortal.

  On first boot the device will start an open Access Point named
  "ESP32-Portal-Setup". Connect to it from a phone or laptop, browse to
  http://192.168.4.1/, pick a network, and submit credentials. The device
  saves them to NVS and on subsequent boots joins that network
  automatically. If the saved network is unreachable it falls back to AP
  mode again.

  Once joined, the device is also reachable at http://esp32-portal.local/
  (mDNS). The name can be changed from the config form's Device name field.

  OTA: when built with a non-empty OTA_PASSWORD (PlatformIO reads it from
  the git-ignored secrets.ini; see secrets.ini.example), the device also
  accepts firmware over WiFi: pio run -e esp32dev_ota -t upload

  Connected devices: http://<device>/devices lists the phones/laptops
  joined to this board's own WiFi (the setup AP or the repeater's -ext
  network) with MAC, IP, signal and WiFi standard; join/leave events are
  also printed over serial. It shows how to add your own page with
  portal.on().

  Status: http://<device>/api/status reports mode, uptime, RSSI and
  (when built with -DESP32WIFIPORTAL_BATTERY) the supply battery.

  No filesystem upload step is required: the config-form HTML is
  compiled into the firmware from the library's data/ directory.
*/

#include <ESP32WiFiPortal.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_ap_get_sta_list.h>

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

ESP32WiFiPortal portal;

String macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

const char* signalLabel(int rssi) {
    if (rssi >= -60) return "Strong";
    if (rssi >= -72) return "Good";
    return "Weak";
}

const char* standardLabel(const wifi_sta_info_t& sta) {
    if (sta.phy_11n) return "802.11n";
    if (sta.phy_11g) return "802.11g";
    if (sta.phy_11b) return "802.11b";
    return "?";
}

void handleDevices() {
    String html =
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"10\"><title>Connected devices</title>"
        "<style>:root{--bg:#eef1f6;--fg:#141a26;--muted:#5b6578;--line:#d6dce7;--surface:#fff}"
        "@media(prefers-color-scheme:dark){:root{--bg:#0e121a;--fg:#e7ebf3;--muted:#9aa4b8;"
        "--line:#2a3344;--surface:#171d29;color-scheme:dark}}"
        "body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}"
        "main{max-width:640px;margin:0 auto;padding:20px 16px}h1{font-size:1.25rem;margin:0 0 4px}"
        "p{margin:0 0 16px;color:var(--muted);font-size:.85rem}"
        ".card{background:var(--surface);border:1px solid var(--line);border-radius:12px;overflow-x:auto}"
        "table{width:100%;border-collapse:collapse;font-size:.875rem}"
        "th,td{text-align:left;padding:10px 12px;border-bottom:1px solid var(--line);white-space:nowrap}"
        "th{color:var(--muted);font-weight:600;font-size:.75rem}tr:last-child td{border-bottom:0}"
        "td.mono{font-family:ui-monospace,Menlo,Consolas,monospace}"
        ".empty{padding:16px;color:var(--muted)}a{color:inherit}</style></head><body><main>"
        "<h1>Connected devices</h1>";

    wifi_sta_list_t stations = {};
    wifi_sta_mac_ip_list_t addresses = {};
    // Fails with ESP_ERR_WIFI_MODE when the board is a plain client and
    // isn't running an access point at all.
    bool apRunning = esp_wifi_ap_get_sta_list(&stations) == ESP_OK;
    if (apRunning) {
        esp_wifi_ap_get_sta_list_with_ip(&stations, &addresses);
        html += "<p>On <b>" + WiFi.softAPSSID() + "</b> (" +
                WiFi.softAPIP().toString() + ") &middot; " +
                String(stations.num) + " connected &middot; refreshes every 10 s</p>";
    } else {
        html += "<p>This board is running as a WiFi client only, so no devices "
                "can connect to it. Choose Repeater mode on the <a href=\"/\">setup "
                "page</a> to share the connection.</p>";
    }

    html += "<div class=\"card\">";
    if (apRunning && stations.num > 0) {
        html += "<table><tr><th>MAC address</th><th>IP address</th>"
                "<th>Signal</th><th>Standard</th></tr>";
        for (int i = 0; i < stations.num; ++i) {
            const wifi_sta_info_t& sta = stations.sta[i];
            // No address yet means the device joined but hasn't finished DHCP.
            IPAddress ip(addresses.sta[i].ip.addr);
            html += "<tr><td class=\"mono\">" + macToString(sta.mac) + "</td><td class=\"mono\">" +
                    ((uint32_t)ip != 0 ? ip.toString() : String("(waiting for DHCP)")) +
                    "</td><td>" + signalLabel(sta.rssi) + " (" + String(sta.rssi) +
                    " dBm)</td><td>" + standardLabel(sta) + "</td></tr>";
        }
        html += "</table>";
    } else if (apRunning) {
        html += "<div class=\"empty\">No devices connected.</div>";
    }
    html += "</div></main></body></html>";
    portal.server().send(200, "text/html", html);
}

void logStationEvents() {
    Network.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
                Serial.printf("[Devices] %s joined\n",
                              macToString(info.wifi_ap_staconnected.mac).c_str());
                break;
            case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
                Serial.printf("[Devices] %s got %s\n",
                              macToString(info.wifi_ap_staipassigned.mac).c_str(),
                              IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
                Serial.printf("[Devices] %s left\n",
                              macToString(info.wifi_ap_stadisconnected.mac).c_str());
                break;
            default:
                break;
        }
    });
}

void setup() {
    Serial.begin(115200);
    delay(200);
    portal.setHostname("esp32-portal");
    portal.enableOta(OTA_PASSWORD);
    portal.enableBootLog();
    // Solid = online, fast blink = no internet, slow blink = setup AP.
    // External LED: GPIO23 -> 220-330 ohm resistor -> LED -> GND.
    portal.setLedPin(23);
#ifdef ESP32WIFIPORTAL_BATTERY
    // Battery + -> 100k -> GPIO34 -> 15k -> GND, 0.1 uF from GPIO34 to GND.
    BatteryConfig battery;
    battery.pin = 34;
    battery.dividerRatio = (100.0f + 15.0f) / 15.0f;
    battery.cells = 4;
    battery.curve = kBatteryCurveLiIon;
    portal.setBattery(battery);
#endif
    portal.on("/devices", handleDevices);
    logStationEvents();
    portal.begin("ESP32-Portal-Setup");
}

void loop() {
    portal.loop();
}
