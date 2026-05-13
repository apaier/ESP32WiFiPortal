#include "DnsService.h"

#include <string.h>

namespace {
constexpr uint16_t kDnsPort = 53;

uint16_t readBe16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}
}  // namespace

DnsService::DnsService()
    : _mode(Mode::Off), _listenerUp(false), _uplinkUp(false) {
    for (auto& slot : _inflight) slot.active = false;
}

bool DnsService::begin() {
    if (_listenerUp) return true;
    if (!_listener.begin(kDnsPort)) {
        Serial.println("[DnsService] Listener bind on ANY:53 failed");
        return false;
    }
    _listenerUp = true;
    Serial.println("[DnsService] Listening on ANY:53");
    return true;
}

void DnsService::setMode(Mode mode, IPAddress addr, IPAddress upstream) {
    // Lazy-bind on first non-Off mode. Calling begin() any earlier
    // (e.g. before the first WiFi.mode() in the host) hits an
    // xQueueSemaphoreTake assert — the lwIP task and its semaphores
    // don't exist until the WiFi stack has been initialized.
    if (mode != Mode::Off && !_listenerUp) {
        if (!begin()) {
            Serial.println("[DnsService] setMode aborted: listener bind failed");
            return;
        }
    }
    _mode = mode;
    switch (mode) {
        case Mode::Off:
            stopUplink();
            Serial.println("[DnsService] mode=Off");
            break;
        case Mode::CaptivePortal:
            _captiveAddr = addr;
            stopUplink();
            Serial.printf("[DnsService] mode=CaptivePortal, answering with %s\n",
                          addr.toString().c_str());
            break;
        case Mode::Forwarder:
            _apSubnetAnchor = addr;
            _upstream = upstream;
            ensureUplink();
            Serial.printf("[DnsService] mode=Forwarder, AP subnet anchor %s, upstream %s\n",
                          addr.toString().c_str(),
                          upstream.toString().c_str());
            break;
    }
}

void DnsService::end() {
    stopUplink();
    if (_listenerUp) {
        _listener.stop();
        _listenerUp = false;
    }
    _mode = Mode::Off;
    for (auto& slot : _inflight) slot.active = false;
}

bool DnsService::ensureUplink() {
    if (_uplinkUp) return true;
    if (!_uplink.begin(0)) {
        Serial.println("[DnsService] Uplink bind on ephemeral port failed");
        return false;
    }
    _uplinkUp = true;
    return true;
}

void DnsService::stopUplink() {
    if (!_uplinkUp) return;
    _uplink.stop();
    _uplinkUp = false;
    for (auto& slot : _inflight) slot.active = false;
}

void DnsService::loop() {
    if (!_listenerUp) return;
    cleanupExpired();
    drainListener();
    if (_uplinkUp) drainUplink();
}

void DnsService::drainListener() {
    int sz = _listener.parsePacket();
    if (sz <= 0) return;
    if (static_cast<size_t>(sz) > kMaxPacket) {
        uint8_t scratch[64];
        while (_listener.available() > 0) {
            _listener.read(scratch, sizeof(scratch));
        }
        return;
    }
    uint8_t buf[kMaxPacket];
    int read = _listener.read(buf, sizeof(buf));
    if (read < 12) return;
    IPAddress src = _listener.remoteIP();
    uint16_t srcPort = _listener.remotePort();

    switch (_mode) {
        case Mode::Off:
            // Drop. Listener stays bound so we hold port 53, but we
            // don't act on queries while the device isn't expected to
            // serve DNS (i.e. plain Connected/client mode).
            break;
        case Mode::CaptivePortal:
            respondCaptivePortal(src, srcPort, buf, static_cast<size_t>(read));
            break;
        case Mode::Forwarder:
            if (isLocalApClient(src)) {
                proxyToUpstream(src, srcPort, buf, static_cast<size_t>(read));
            }
            // Reject queries from outside the AP subnet so we don't act
            // as an open resolver to whatever's on the STA-side LAN.
            break;
    }
}

void DnsService::drainUplink() {
    int sz = _uplink.parsePacket();
    if (sz <= 0) return;
    if (static_cast<size_t>(sz) > kMaxPacket) {
        uint8_t scratch[64];
        while (_uplink.available() > 0) {
            _uplink.read(scratch, sizeof(scratch));
        }
        return;
    }
    uint8_t buf[kMaxPacket];
    int read = _uplink.read(buf, sizeof(buf));
    if (read < 12) return;
    IPAddress src = _uplink.remoteIP();
    uint16_t srcPort = _uplink.remotePort();
    if ((uint32_t)src != (uint32_t)_upstream || srcPort != kDnsPort) {
        // Stray packet (old upstream, spoof, etc.) — ignore.
        return;
    }
    routeUpstreamResponse(buf, static_cast<size_t>(read));
}

void DnsService::respondCaptivePortal(const IPAddress& src, uint16_t srcPort,
                                       const uint8_t* query, size_t len) {
    // Build a minimal A-record response in a local buffer: copy the
    // header + question section from the query, flip the QR bit, set
    // ANCOUNT=1, append a single answer pointing at _captiveAddr with
    // a compressed name pointer back to the question.
    if (len + 16 > kMaxPacket) return;
    uint8_t resp[kMaxPacket];
    memcpy(resp, query, len);
    resp[2] |= 0x80;  // QR = 1 (response)
    resp[3] = 0x80;   // RA = 1, RCODE = 0
    // ANCOUNT = 1
    resp[6] = 0x00;
    resp[7] = 0x01;
    // NSCOUNT, ARCOUNT = 0
    resp[8] = 0x00;
    resp[9] = 0x00;
    resp[10] = 0x00;
    resp[11] = 0x00;

    size_t pos = len;
    resp[pos++] = 0xC0;  // name pointer ...
    resp[pos++] = 0x0C;  // ... to offset 12 (start of question name)
    resp[pos++] = 0x00;
    resp[pos++] = 0x01;  // TYPE A
    resp[pos++] = 0x00;
    resp[pos++] = 0x01;  // CLASS IN
    resp[pos++] = 0x00;
    resp[pos++] = 0x00;
    resp[pos++] = 0x00;
    resp[pos++] = 0x3C;  // TTL = 60s
    resp[pos++] = 0x00;
    resp[pos++] = 0x04;  // RDLENGTH = 4
    resp[pos++] = _captiveAddr[0];
    resp[pos++] = _captiveAddr[1];
    resp[pos++] = _captiveAddr[2];
    resp[pos++] = _captiveAddr[3];

    _listener.beginPacket(src, srcPort);
    _listener.write(resp, pos);
    _listener.endPacket();
}

void DnsService::proxyToUpstream(const IPAddress& src, uint16_t srcPort,
                                  const uint8_t* query, size_t len) {
    if (!_uplinkUp) return;
    if ((uint32_t)_upstream == 0) return;
    uint16_t id = readBe16(query);
    recordInflight(id, src, srcPort);
    _uplink.beginPacket(_upstream, kDnsPort);
    _uplink.write(query, len);
    _uplink.endPacket();
}

void DnsService::routeUpstreamResponse(const uint8_t* data, size_t len) {
    uint16_t id = readBe16(data);
    IPAddress client;
    uint16_t port = 0;
    if (!consumeInflight(id, client, port)) return;
    _listener.beginPacket(client, port);
    _listener.write(data, len);
    _listener.endPacket();
}

void DnsService::recordInflight(uint16_t id, const IPAddress& client,
                                 uint16_t port) {
    int target = -1;
    uint32_t oldestMs = UINT32_MAX;
    for (size_t i = 0; i < kMaxInflight; ++i) {
        if (!_inflight[i].active) {
            target = static_cast<int>(i);
            break;
        }
        if (_inflight[i].startedMs < oldestMs) {
            oldestMs = _inflight[i].startedMs;
            target = static_cast<int>(i);
        }
    }
    _inflight[target].id = id;
    _inflight[target].clientIp = client;
    _inflight[target].clientPort = port;
    _inflight[target].startedMs = millis();
    _inflight[target].active = true;
}

bool DnsService::consumeInflight(uint16_t id, IPAddress& outClient,
                                  uint16_t& outPort) {
    for (auto& slot : _inflight) {
        if (slot.active && slot.id == id) {
            outClient = slot.clientIp;
            outPort = slot.clientPort;
            slot.active = false;
            return true;
        }
    }
    return false;
}

void DnsService::cleanupExpired() {
    uint32_t now = millis();
    for (auto& slot : _inflight) {
        if (slot.active && (now - slot.startedMs) >= kQueryTimeoutMs) {
            slot.active = false;
        }
    }
}

bool DnsService::isLocalApClient(const IPAddress& ip) const {
    return ip[0] == _apSubnetAnchor[0] && ip[1] == _apSubnetAnchor[1] &&
           ip[2] == _apSubnetAnchor[2];
}
