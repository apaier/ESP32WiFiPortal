# WiFi Recovery and Reconnection

How `ESP32WiFiPortal` decides when to retry a failed WiFi connection and when
to fall back to the configuration AP. Read this when changing anything in
`startAccessPoint`, `tryConnect`, `loop`, or the WiFi event handler.

## Goals

1. If credentials are saved and the network is in range, the device should
   reconnect on its own without rebooting.
2. If the network is out of range or down, the device should keep the
   configuration AP available so a human can intervene.
3. If the saved credentials are wrong (e.g. the user typed the password
   incorrectly), the device should not keep blindly retrying — it should
   fall back to the captive portal immediately and wait for the user.
4. Reconnect attempts should not pointlessly disrupt the AP. Tearing down
   the captive portal for a 30-second `WiFi.begin()` attempt while the
   network is genuinely gone is wasteful.

The previous implementation used wall-clock timers (3-minute blind retry,
60-second disconnect grace). It worked but failed goals 3 and 4. The
current implementation is event-driven on the STA side, scan-driven on
the AP side, with a wall-clock floor only as a safety net for cases scans
can't cover.

## State machine

```
                              +-------------+
                              |  AP mode    |<--+
                  no creds +--|  (AP_STA,   |   | hard-fail disconnect
                           |  | auto-rcn   |   | or N consecutive soft
                           |  |  off)       |   | disconnect events
                           v  +-------------+   |
                    +----------+     |    ^     |
                    |  idle    |     |    |     |
                    +----------+     v    |     |
                                  +-----------------+
                                  |  Connected     |
                                  |  or Repeating  |
                                  |  (auto-rcn on) |
                                  +-----------------+
```

Transitions in plain English:

- **boot → Connected/Repeating** — `loadAndConnect()` succeeded.
- **boot → AP mode** — no creds, parse error, or `tryConnect` timed out.
- **Connected/Repeating → AP mode (hard-fail)** — a disconnect event
  arrived with a reason code that means "the credentials are wrong"
  (AUTH_FAIL, ASSOC_FAIL, HANDSHAKE_TIMEOUT, 4WAY_HANDSHAKE_TIMEOUT).
  Immediate fallback. We also call `WiFi.disconnect(false, true)` to
  erase the chip's auto-reconnect credentials so it stops
  auto-reassociating with bad creds. The library's own Preferences
  entries are preserved so the portal can prefill for editing.
- **Connected/Repeating → AP mode (soft-fail)** — disconnect events with
  transient reasons (BEACON_TIMEOUT, NO_AP_FOUND, etc.) are counted.
  Each event increments `_softFailEventCount`. A successful `STA_GOT_IP`
  resets the count. If the count reaches `kSoftFailEventsBeforeFallback`
  (10) we fall back to AP. This avoids tearing down the AP for a
  one-second blip while still recovering from genuinely-gone networks
  within a minute or so.
- **AP mode → Connected/Repeating** — async scan finds the saved SSID
  in the results; `attemptReconnect()` calls `loadAndConnect()` which
  re-runs `tryConnect`.

## Why we don't scan from the Connected side

Earlier versions scanned right after a soft-fail disconnect to confirm
whether the SSID was still reachable. That fought the chip: once
`STA_DISCONNECTED` fires, the chip immediately enters its own internal
reconnect loop, and `esp_wifi_scan_start()` is rejected as long as the
chip is in that state. The symptom was a stream of "scanNetworks failed
to start" messages and no useful recovery signal.

Solution: don't try to scan while the chip is busy reconnecting. Instead,
count disconnect events. The chip's auto-reconnect timing gives us a
natural cadence (one event every few seconds while it's failing). After
`kSoftFailEventsBeforeFallback` (10) events without a successful
`STA_GOT_IP` in between, the network is gone enough to warrant AP
fallback — and only then do we scan.

## The two reconnect mechanisms in AP mode

### 1. Scan-and-pounce (primary)

When we enter AP mode we call `WiFi.setAutoReconnect(false)` and
`WiFi.disconnect(false, false)` to stop the chip's auto-reassociation —
exactly so scans aren't blocked. Then every `kScanCadenceMs` (30s) we
run an async scan. When the saved SSID appears, we call
`attemptReconnect()`, which reads the credentials back from NVS and
runs `tryConnect`. On success we drop out of AP mode (and `tryConnect`
re-enables auto-reconnect for the new Connected state); on failure we
log and stay in AP mode for the next scan.

This is the path that handles "the router was off, then turned back on"
without any user action.

### 2. Blind-retry floor (safety net)

Some networks won't show up in passive scans even when they're working:

- **Hidden SSIDs** (beacon broadcasts the SSID as empty)
- **5GHz DFS channels** where the AP must wait between transmissions and
  can be missed by a single passive scan pass

For these, scan-and-pounce never fires. To recover, we also run a blind
`attemptReconnect()` every `kBlindRetryFloorMs` (15 minutes) regardless
of scan results.

## Reason-code classification

Read from `info.wifi_sta_disconnected.reason` on
`ARDUINO_EVENT_WIFI_STA_DISCONNECTED`. The values come from ESP-IDF
(`esp_wifi_types.h`).

| Reason                       | Class | Notes                            |
| ---------------------------- | ----- | -------------------------------- |
| `AUTH_FAIL` (202)            | Hard  | Wrong password.                  |
| `ASSOC_FAIL` (203)           | Hard  | AP rejected (MAC filter, etc.).  |
| `HANDSHAKE_TIMEOUT` (204)    | Hard  | Usually wrong password.          |
| `4WAY_HANDSHAKE_TIMEOUT` (15)| Hard  | WPA2 handshake failed.           |
| `BEACON_TIMEOUT` (200)       | Soft  | Router rebooting / out of range. |
| `NO_AP_FOUND` (201)          | Soft  | AP gone or moved channel.        |
| `CONNECTION_FAIL` (205)      | Soft  | Generic transient failure.       |
| everything else              | Soft  | Default to "let count decide".   |

Hard-fail flips `_hardFail = true`, calls `WiFi.disconnect(false, true)`
to clear the chip's auto-reconnect credentials, and immediately enters
AP mode. `_hardFail` is cleared the next time `tryConnect` runs (which
happens on a fresh form submission via the captive portal). The portal's
periodic scan-and-pounce is suppressed while `_hardFail` is set so the
device doesn't burn power retrying creds that can't work.

The library's Preferences entries are **not** wiped on hard-fail. The
portal still has the bad SSID/password available to prefill so the user
can edit instead of retyping from scratch. The chip-internal credentials
(separate from our `wifi_portal` namespace, written by
`esp_wifi_set_config`) are the only thing cleared.

## Auto-reconnect lifecycle

The chip-level auto-reassociation (`WiFi.setAutoReconnect`) is on while
we're in Connected/Repeating and off while we're in AP mode. This is the
single most important invariant in the recovery code:

- **On** during Connected/Repeating: a transient drop (one-second blip)
  gets recovered by the chip in 1-2 seconds without intervention. We
  count those drops via the soft-fail event handler; if they pile up
  past the threshold we fall back to AP.
- **Off** during AP mode: scans aren't blocked by an in-flight chip
  reconnect, so scan-and-pounce can actually run.

The toggle points are `startAccessPoint()` (off) and `tryConnect()` (on
at the top, before `WiFi.begin`).

## Why WIFI_AP_STA in AP mode

`WiFi.scanNetworks()` requires the STA radio to be available. If we kept
the AP in `WIFI_AP` mode, every scan would have to switch to `WIFI_AP_STA`,
scan, and switch back — extra mode transitions every 30s. Instead we run
the AP in `WIFI_AP_STA` from the moment we enter AP mode. The STA radio
sits idle (because auto-reconnect is off) and is available for our
scans.

## Concurrency and the event handler

WiFi events fire on the ESP-IDF WiFi event task, not the main loop. The
handler captures the reason code, sets a flag for `loop()` to drain, and
maintains two pieces of state directly:

```cpp
case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    _disconnectReason = info.wifi_sta_disconnected.reason;
    _disconnectEventPending = true;
    // ... NAPT off if Repeating
case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    _softFailEventCount = 0;
    // ... NAPT on if Repeating
case ARDUINO_EVENT_WIFI_AP_START:
    // ... NAPT on if Repeating and STA is up; covers the race where
    // softAP() returns before the new netif is fully online.
```

The disconnect-pending fields are `volatile` to prevent the compiler
from caching reads in the loop. On Xtensa (ESP32 LX6/LX7), byte-aligned
reads/writes of bool and uint8_t are atomic, so no further locking is
needed for this single-flag producer/consumer pattern.

`_softFailEventCount` is written from both task contexts (loop reads
and resets it in some paths; event task increments it via the handler).
Strictly speaking it should also be `volatile` or accessed under a
critical section, but in practice the worst case is a count that's off
by one for a single iteration — not worth the complexity. If you change
the threshold semantics, revisit this.

NAPT toggling for repeater mode happens directly inside the event handler
because `WiFi.AP.enableNAPT(bool)` is a thin shim and works fine from
event-task context. If you add anything more involved to the handler,
move it to a "drain in loop" path like `handleDisconnectEvent`.

`startRepeaterAccessPoint` sets `_mode = Repeating` *before* its direct
`enableNAPT(true)` call, specifically so the asynchronous `AP_START`
event for the new SSID can re-enable NAPT once the netif is actually
online. `softAP()` returns before that event fires, so the direct call
alone is racy — it works when the netif is already up, but on faster
boot paths (e.g. after the LittleFS→NVS storage change shortened the
window between `STA_GOT_IP` and the NAPT enable) it can land before the
new netif is configured, silently no-op, and leave the repeater AP up
but unbridged. The event-handler path is the safety net.

`STA_GOT_IP` does a **toggle** on NAPT (off then on) rather than a plain
re-enable, and refreshes the DHCP DNS option via `refreshRepeaterDhcpDns`.
Reason: a stale NAT table — left over from a reconnect where the STA
briefly landed on a different subnet — is not cleared by a second
`enableNAPT(true)`. lwIP treats that as a no-op. Calling
`enableNAPT(false)` first frees the table; the immediate `true` after
rebuilds it against the current STA gateway. The DNS refresh covers
the matching case on the DHCP-server side: clients otherwise keep
receiving whatever DNS was captured at the first `softAPConfig` call,
even if it's now unroutable through NAPT. Both happen in the event
handler — synchronous lwIP calls, cheap, run on the WiFi event task.

## Tuning parameters

All in the anonymous namespace at the top of `ESP32WiFiPortal.cpp`:

| Constant                          | Default | What it controls |
| --------------------------------- | ------- | ----------------- |
| `kScanCadenceMs`                  | 30s     | How often to scan in AP mode looking for the saved SSID. Lower = faster recovery, more AP disruption. |
| `kScanTimeoutMs`                  | 15s     | How long to wait before declaring an in-flight scan stuck. |
| `kBlindRetryFloorMs`              | 15min   | Blind reconnect cadence (hidden SSIDs / DFS). |
| `kSoftFailEventsBeforeFallback`   | 10      | Consecutive soft-fail disconnect events before falling back from Connected/Repeating to AP. With chip auto-reconnect on, events arrive every few seconds — so 10 ≈ 30-60 seconds of confirmed failure. |
| `kConnectAttemptSeconds`          | 30      | How long a single `WiFi.begin` attempt waits before declaring failure. Pre-existing, unchanged. |

## Known limitations

- **Hidden SSIDs reconnect slowly.** Scan-and-pounce never fires; only the
  15-minute blind floor catches them. If hidden SSIDs become a primary
  use case, change the scan call to `WiFi.scanNetworks(true, true)` to
  include hidden networks in the results.
- **Single SSID supported.** Only one set of credentials is ever stored.
  Roaming between multiple known networks would need a different config
  schema (a list of credential entries) plus selection logic in
  `loadAndConnect`.
- **The `handleConfigPage` user-facing scan is synchronous.** Hitting the
  captive portal triggers a blocking `WiFi.scanNetworks()`. Because
  auto-reconnect is off in AP mode this works, but if a background async
  scan happens to be in flight the page load will wait.
- **Soft-fail counter is not strictly task-safe.** See concurrency note
  above. Practical impact: off-by-one near the threshold in rare cases.

## Files

- [src/ESP32WiFiPortal.cpp](../src/ESP32WiFiPortal.cpp) — implementation
- [src/ESP32WiFiPortal.h](../src/ESP32WiFiPortal.h) — public API + private
  state declarations
