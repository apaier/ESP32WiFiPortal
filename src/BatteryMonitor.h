/*
  BatteryMonitor - optional supply-battery voltage readout for
  ESP32WiFiPortal.

  Compiled only when the build defines ESP32WIFIPORTAL_BATTERY (e.g.
  build_flags = -DESP32WIFIPORTAL_BATTERY in platformio.ini). Without
  the flag this header declares nothing and the library carries no
  battery code.

  Hardware: a resistor divider from battery + to an ADC1 pin (GPIO32-39;
  ADC2 is unusable while WiFi is on), with battery - sharing the ESP32's
  ground. dividerRatio is (R1 + R2) / R2, where R2 is the resistor to
  ground. Keep the pin under ~2.5 V at the highest battery voltage, where
  the ESP32 ADC is most linear.

  Percent is estimated from resting voltage per cell using a curve, so
  it reads low under heavy load and high while charging.
*/

#ifndef ESP32_WIFI_PORTAL_BATTERY_MONITOR_H
#define ESP32_WIFI_PORTAL_BATTERY_MONITOR_H

#ifdef ESP32WIFIPORTAL_BATTERY

#include <Arduino.h>

struct BatteryCurvePoint {
    float cellVolts;
    uint8_t percent;
};

// Points must be ordered from highest to lowest cellVolts.
struct BatteryCurve {
    const BatteryCurvePoint* points;
    size_t count;
};

extern const BatteryCurve kBatteryCurveLiIon;     // NMC / NCA, 4.2 V full
extern const BatteryCurve kBatteryCurveLiFePO4;   // 3.4 V resting full
extern const BatteryCurve kBatteryCurveLeadAcid;  // per 2 V cell (12 V = 6)

struct BatteryConfig {
    int pin = -1;
    float dividerRatio = 1.0f;  // (R1 + R2) / R2
    uint8_t cells = 1;          // in series
    BatteryCurve curve = kBatteryCurveLiIon;
    // Multiplier applied to the computed voltage. Set to
    // (multimeter reading / reported volts) to trim out resistor and
    // ADC error for a specific board.
    float calibration = 1.0f;
};

class BatteryMonitor {
public:
    BatteryMonitor();

    // Apply a configuration; safe to call again at any time to change
    // it. pin < 0 turns monitoring off. Pins that aren't on ADC1 are
    // rejected (ADC2 can't be read while WiFi is running).
    void begin(const BatteryConfig& config);
    bool active() const;
    const BatteryConfig& config() const;
    // Change only the calibration factor, keeping the rest of the config.
    void setCalibration(float calibration);

    // Averaged battery voltage at the battery terminals. Takes ~1 ms.
    float readVolts() const;
    // 0-100 estimate for a given battery voltage.
    int percentFor(float volts) const;
    uint8_t cells() const;

private:
    BatteryConfig _config;
    bool _active;
};

#endif  // ESP32WIFIPORTAL_BATTERY

#endif  // ESP32_WIFI_PORTAL_BATTERY_MONITOR_H
