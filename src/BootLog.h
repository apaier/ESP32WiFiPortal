/*
  BootLog - persistent record of recent boots for ESP32WiFiPortal.

  Diagnoses resets that happen when nobody is watching the serial port
  (e.g. a board that only misbehaves on battery). Each boot appends an
  entry to a small ring in NVS holding the reset reason that started
  the boot and the furthest startup stage it reached. Stages are only
  written when they advance, so a boot costs at most four NVS writes.

  Reading an entry: the reset reason says why *this* boot started (so
  it describes how the previous boot ended), and the stage says how far
  this boot got before the next reset. A run of "brownout" entries that
  never get past RadioStarting is the classic signature of a supply
  that sags when WiFi transmits.

  On classic ESP32, a press of the EN button reports as power-on.
*/

#ifndef ESP32_WIFI_PORTAL_BOOT_LOG_H
#define ESP32_WIFI_PORTAL_BOOT_LOG_H

#include <Arduino.h>
#include <Preferences.h>

class BootLog {
public:
    enum class Stage : uint8_t {
        Booted = 0,         // begin() ran
        RadioStarting = 1,  // about to bring up WiFi (current spike)
        NetworkUp = 2,      // AP up or STA joined
        Stable = 3,         // survived kStableAfterMs of uptime
    };

    static constexpr uint32_t kStableAfterMs = 60UL * 1000UL;

    BootLog();

    // Open NVS and record this boot. Returns false if NVS is unavailable,
    // in which case every other call is a no-op.
    bool begin();
    bool active() const;

    // Advance this boot's stage. Ignored unless it moves forward.
    void mark(Stage stage);

    // Human-readable history, newest first.
    String format() const;
    void clear();

private:
    struct Entry {
        uint32_t boot;
        uint8_t reason;
        uint8_t stage;
        uint8_t reserved[2];
    };
    static constexpr size_t kEntries = 8;

    Preferences _prefs;
    Entry _entries[kEntries];
    size_t _count;
    bool _active;

    void save();
};

#endif  // ESP32_WIFI_PORTAL_BOOT_LOG_H
