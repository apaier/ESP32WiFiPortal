/*
  BasicConfigPortal - minimal use of ESP32WiFiPortal.

  On first boot the device will start an open Access Point named
  "ESP32-Portal-Setup". Connect to it from a phone or laptop, browse to
  http://192.168.4.1/, pick a network, and submit credentials. The device
  saves them to NVS and on subsequent boots joins that network
  automatically. If the saved network is unreachable it falls back to AP
  mode again.

  No filesystem upload step is required: the config-form HTML is
  compiled into the firmware from the library's data/ directory.
*/

#include <ESP32WiFiPortal.h>

ESP32WiFiPortal portal;

void setup() {
    Serial.begin(115200);
    delay(200);
    portal.begin("ESP32-Portal-Setup");
}

void loop() {
    portal.loop();
}
