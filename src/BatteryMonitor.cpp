#include "BatteryMonitor.h"

#ifdef ESP32WIFIPORTAL_BATTERY

namespace {
constexpr int kSamples = 32;

// Approximate resting voltage per cell vs state of charge.
constexpr BatteryCurvePoint kLiIonPoints[] = {
    {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.90f, 70}, {3.80f, 60},
    {3.75f, 50},  {3.70f, 40}, {3.65f, 30}, {3.60f, 20}, {3.50f, 10},
    {3.30f, 5},   {3.00f, 0},
};
constexpr BatteryCurvePoint kLiFePO4Points[] = {
    {3.40f, 100}, {3.35f, 90}, {3.32f, 80}, {3.30f, 70}, {3.27f, 60},
    {3.26f, 50},  {3.25f, 40}, {3.22f, 30}, {3.20f, 20}, {3.00f, 10},
    {2.50f, 0},
};
constexpr BatteryCurvePoint kLeadAcidPoints[] = {
    {2.117f, 100}, {2.083f, 90}, {2.070f, 80}, {2.053f, 70}, {2.033f, 60},
    {2.010f, 50},  {1.983f, 40}, {1.958f, 30}, {1.930f, 20}, {1.885f, 10},
    {1.750f, 0},
};
}  // namespace

const BatteryCurve kBatteryCurveLiIon = {
    kLiIonPoints, sizeof(kLiIonPoints) / sizeof(kLiIonPoints[0])};
const BatteryCurve kBatteryCurveLiFePO4 = {
    kLiFePO4Points, sizeof(kLiFePO4Points) / sizeof(kLiFePO4Points[0])};
const BatteryCurve kBatteryCurveLeadAcid = {
    kLeadAcidPoints, sizeof(kLeadAcidPoints) / sizeof(kLeadAcidPoints[0])};

BatteryMonitor::BatteryMonitor() : _config(), _active(false) {}

void BatteryMonitor::begin(const BatteryConfig& config) {
    _config = config;
    _active = false;
    if (config.pin < 0) return;
    // Arduino numbers ADC1 channels first, so anything at or past
    // SOC_ADC_MAX_CHANNEL_NUM is on ADC2 (or not an ADC pin at all).
    int8_t channel = digitalPinToAnalogChannel(config.pin);
    if (channel < 0 || channel >= SOC_ADC_MAX_CHANNEL_NUM) {
        Serial.printf("[ESP32WiFiPortal] Battery monitor: GPIO%d is not an ADC1 pin, disabled\n",
                      config.pin);
        return;
    }
    if (config.cells == 0 || config.curve.count == 0 || config.dividerRatio <= 0.0f) {
        Serial.println("[ESP32WiFiPortal] Battery monitor: invalid config, disabled");
        return;
    }
    // 11 dB attenuation gives the widest input range (~0-3.1 V).
    analogSetPinAttenuation(config.pin, ADC_11db);
    _active = true;
    Serial.printf("[ESP32WiFiPortal] Battery monitor on GPIO%d\n", config.pin);
}

bool BatteryMonitor::active() const { return _active; }

const BatteryConfig& BatteryMonitor::config() const { return _config; }

void BatteryMonitor::setCalibration(float calibration) {
    if (calibration > 0.0f) _config.calibration = calibration;
}

float BatteryMonitor::readVolts() const {
    if (!_active) return 0.0f;
    // analogReadMilliVolts applies the chip's factory ADC calibration.
    uint32_t total = 0;
    for (int i = 0; i < kSamples; ++i) {
        total += analogReadMilliVolts(_config.pin);
    }
    float pinVolts = (total / (float)kSamples) / 1000.0f;
    return pinVolts * _config.dividerRatio * _config.calibration;
}

int BatteryMonitor::percentFor(float volts) const {
    if (!_active) return 0;
    const BatteryCurvePoint* pts = _config.curve.points;
    size_t n = _config.curve.count;
    float cellVolts = volts / _config.cells;
    if (cellVolts >= pts[0].cellVolts) return pts[0].percent;
    if (cellVolts <= pts[n - 1].cellVolts) return pts[n - 1].percent;
    for (size_t i = 1; i < n; ++i) {
        if (cellVolts >= pts[i].cellVolts) {
            const BatteryCurvePoint& hi = pts[i - 1];
            const BatteryCurvePoint& lo = pts[i];
            float t = (cellVolts - lo.cellVolts) / (hi.cellVolts - lo.cellVolts);
            return (int)(lo.percent + t * (hi.percent - lo.percent) + 0.5f);
        }
    }
    return pts[n - 1].percent;
}

uint8_t BatteryMonitor::cells() const { return _config.cells; }

#endif  // ESP32WIFIPORTAL_BATTERY
