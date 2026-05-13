/*
  DnsService - single DNS-on-port-53 owner for ESP32WiFiPortal.

  This is the only thing in the library that binds UDP port 53. The
  bind happens lazily on the first setMode(non-Off) call (from either
  startAccessPoint or startRepeaterAccessPoint, both of which run
  WiFi.mode() first so the lwIP TCP/IP task is up). Once bound, the
  socket stays bound for the rest of the device's lifetime; mode
  changes only flip an internal dispatch flag and never rebind.
  That's a deliberate choice — Arduino-ESP32's DNSServer leaves a
  stale lwIP socket on port 53 past stop() in current pioarduino
  builds, and a second component trying to bind port 53 (even on a
  specific local IP) hits EADDRINUSE. Holding the port ourselves
  permanently dodges the whole timing problem.

  Modes:
    Off            — drop every query received on port 53.
    CaptivePortal  — answer every A query with `addr` (the AP IP). This
                     is the standard "lie to clients so they hit the
                     portal" pattern that triggers iOS/Android/macOS
                     captive-network detection.
    Forwarder      — proxy queries to `upstream` (typically whichever
                     resolver the STA last learned from DHCP) and route
                     responses back to the originating client. Used in
                     repeater mode so AP-side clients get a stable DNS
                     pointer (the AP IP) regardless of upstream DNS
                     churn.

  In Forwarder mode a second UDP socket (`_uplink`) is bound to ANY:0
  for outgoing queries — a socket bound to a specific local IP can't
  route out a different netif (lwIP refuses), so the uplink stays on
  INADDR_ANY and lwIP picks the right source IP per destination.
  In-flight queries are tracked in a small fixed-size table keyed by
  DNS transaction ID; entries time out after 5 s.
*/

#ifndef ESP32_WIFI_PORTAL_DNS_SERVICE_H
#define ESP32_WIFI_PORTAL_DNS_SERVICE_H

#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiUdp.h>

class DnsService {
public:
    enum class Mode { Off, CaptivePortal, Forwarder };

    DnsService();

    // Bind the listener on ANY:53. Safe to call repeatedly; returns
    // true if already bound. After this, set the mode to control
    // behavior — the listener socket itself is never rebound.
    // setMode() will call this lazily on the first non-Off mode, so
    // callers don't have to invoke it explicitly unless they want
    // early-bind diagnostics. Calling it before the host's first
    // WiFi.mode() trips an xQueueSemaphoreTake assert because the
    // lwIP task hasn't been created yet.
    bool begin();

    // Switch behavior. For CaptivePortal: `addr` is what every A query
    // gets as its answer. For Forwarder: `addr` identifies the AP
    // subnet (clients on that /24 are accepted; anyone else is ignored)
    // and `upstream` is where queries are proxied. For Off: arguments
    // ignored.
    void setMode(Mode mode, IPAddress addr = IPAddress(),
                 IPAddress upstream = IPAddress());

    // Stop both sockets. Normally not called — the service holds port
    // 53 for the device's lifetime by design — but provided for
    // symmetry and for shutdown paths.
    void end();

    Mode mode() const { return _mode; }
    bool isRunning() const { return _listenerUp; }

    // Pump UDP. Call from the host's loop() unconditionally; Off-mode
    // calls are cheap (one parsePacket each).
    void loop();

private:
    static constexpr uint16_t kDnsPort = 53;
    static constexpr size_t kMaxInflight = 16;
    static constexpr uint32_t kQueryTimeoutMs = 5000;
    static constexpr size_t kMaxPacket = 512;

    struct InflightQuery {
        uint16_t id;
        IPAddress clientIp;
        uint16_t clientPort;
        uint32_t startedMs;
        bool active;
    };

    WiFiUDP _listener;
    WiFiUDP _uplink;
    Mode _mode;
    IPAddress _captiveAddr;
    IPAddress _apSubnetAnchor;
    IPAddress _upstream;
    bool _listenerUp;
    bool _uplinkUp;
    InflightQuery _inflight[kMaxInflight];

    void drainListener();
    void drainUplink();
    void respondCaptivePortal(const IPAddress& src, uint16_t srcPort,
                              const uint8_t* query, size_t len);
    void proxyToUpstream(const IPAddress& src, uint16_t srcPort,
                         const uint8_t* query, size_t len);
    void routeUpstreamResponse(const uint8_t* data, size_t len);
    void recordInflight(uint16_t id, const IPAddress& client, uint16_t port);
    bool consumeInflight(uint16_t id, IPAddress& outClient, uint16_t& outPort);
    void cleanupExpired();
    bool isLocalApClient(const IPAddress& ip) const;
    bool ensureUplink();
    void stopUplink();
};

#endif  // ESP32_WIFI_PORTAL_DNS_SERVICE_H
