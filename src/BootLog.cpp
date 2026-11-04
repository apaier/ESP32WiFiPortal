#include "BootLog.h"

#include <esp_system.h>
#include <string.h>

namespace {
constexpr const char* kNamespace = "wp_bootlog";
constexpr const char* kSeqKey = "seq";
constexpr const char* kRingKey = "ring";

const char* reasonName(uint8_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "crash (panic)";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_PWR_GLITCH: return "power glitch";
        default: return "unknown";
    }
}

const char* stageName(uint8_t stage) {
    switch (stage) {
        case 0: return "booted";
        case 1: return "radio starting";
        case 2: return "network up";
        case 3: return "stable";
        default: return "?";
    }
}
}  // namespace

BootLog::BootLog() : _entries(), _count(0), _active(false) {}

bool BootLog::begin() {
    if (_active) return true;
    if (!_prefs.begin(kNamespace, /*readOnly=*/false)) {
        Serial.println("[ESP32WiFiPortal] Boot log: failed to open NVS");
        return false;
    }
    _active = true;

    size_t stored = _prefs.getBytesLength(kRingKey);
    if (stored > 0 && stored <= sizeof(_entries) && stored % sizeof(Entry) == 0) {
        _prefs.getBytes(kRingKey, _entries, stored);
        _count = stored / sizeof(Entry);
    }

    uint32_t seq = _prefs.getUInt(kSeqKey, 0) + 1;
    _prefs.putUInt(kSeqKey, seq);

    // Newest at index 0; the oldest falls off the end once full.
    size_t keep = (_count < kEntries) ? _count : kEntries - 1;
    memmove(&_entries[1], &_entries[0], keep * sizeof(Entry));
    _entries[0] = Entry{seq, (uint8_t)esp_reset_reason(),
                        (uint8_t)Stage::Booted, {0, 0}};
    _count = keep + 1;
    save();
    return true;
}

bool BootLog::active() const { return _active; }

void BootLog::mark(Stage stage) {
    if (!_active || _count == 0) return;
    if ((uint8_t)stage <= _entries[0].stage) return;
    _entries[0].stage = (uint8_t)stage;
    save();
}

String BootLog::format() const {
    String out;
    if (!_active) return "Boot log not enabled\n";
    out += "Recent boots, newest first. 'started by' is the reset that began\n";
    out += "the boot; 'reached' is how far it got before the next reset.\n";
    for (size_t i = 0; i < _count; ++i) {
        char line[96];
        snprintf(line, sizeof(line), "  boot %5lu: started by %-18s reached %s%s\n",
                 (unsigned long)_entries[i].boot, reasonName(_entries[i].reason),
                 stageName(_entries[i].stage), (i == 0) ? " (this boot)" : "");
        out += line;
    }
    return out;
}

void BootLog::clear() {
    if (!_active) return;
    // Keep the current boot so stage tracking carries on.
    _count = (_count > 0) ? 1 : 0;
    save();
}

void BootLog::save() {
    _prefs.putBytes(kRingKey, _entries, _count * sizeof(Entry));
}
