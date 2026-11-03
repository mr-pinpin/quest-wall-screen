// Finding the laptop on the house network without a hard-coded address (it moved from .102 to .104 when DHCP
// re-leased after a reboot on 2026-11-03, and every stream pointed at a dead address).
//
// Order: files/hosts.json override {"LAPTOP": "192.168.0.104"} -> mDNS A query for <name>.local (macOS answers
// for its own LocalHostName; the name comes from hosts.json "laptopName", default below) -> the last address that
// resolved (files/hosts-last.json) -> the built-in default. The mDNS query is a one-shot "legacy unicast" query
// (RFC 6762 6.7): sent to 224.0.0.251:5353 from an ordinary port, so the responder answers by unicast to that
// port and no Android multicast lock is needed. Packet building and parsing are plain functions, unit-tested on
// a desktop (bench/hosts_test.cpp).
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hosts {

// A standard query for one A record. id != 0 marks legacy unicast; the QU bit asks for a unicast reply too.
inline std::vector<uint8_t> buildQuery(const std::string& name, uint16_t id) {
    std::vector<uint8_t> q = {uint8_t(id >> 8), uint8_t(id), 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start); if (dot == std::string::npos) dot = name.size();
        size_t len = dot - start;
        if (len == 0 || len > 63) return {};
        q.push_back(uint8_t(len)); q.insert(q.end(), name.begin() + start, name.begin() + dot);
        start = dot + 1;
        if (dot == name.size()) break;
    }
    q.push_back(0);
    q.insert(q.end(), {0, 1, 0x80, 1});  // type A, class IN with the unicast-response bit
    return q;
}

// Reads a (possibly compressed) name at p; returns the offset after it in the original stream, or 0 on error.
inline size_t readName(const uint8_t* m, size_t n, size_t p, std::string& out) {
    out.clear(); size_t end = 0; int jumps = 0;
    while (p < n) {
        uint8_t l = m[p];
        if (l == 0) { if (!end) end = p + 1; return end; }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= n || ++jumps > 16) return 0;
            if (!end) end = p + 2;
            p = ((l & 0x3F) << 8) | m[p + 1]; continue;
        }
        if (l > 63 || p + 1 + l > n) return 0;
        if (!out.empty()) out += '.';
        out.append((const char*)m + p + 1, l); p += 1 + l;
    }
    return 0;
}

inline bool sameName(std::string a, std::string b) {
    if (!a.empty() && a.back() == '.') a.pop_back();
    if (!b.empty() && b.back() == '.') b.pop_back();
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// The first A record for `name` in a response (answers or additionals), as dotted IPv4; "" if none.
inline std::string parseA(const uint8_t* m, size_t n, const std::string& name) {
    if (n < 12 || !(m[2] & 0x80)) return "";  // must be a response
    size_t qd = (m[4] << 8) | m[5], rr = ((m[6] << 8) | m[7]) + ((m[8] << 8) | m[9]) + ((m[10] << 8) | m[11]);
    size_t p = 12; std::string nm;
    for (size_t i = 0; i < qd; i++) { p = readName(m, n, p, nm); if (!p || p + 4 > n) return ""; p += 4; }
    for (size_t i = 0; i < rr; i++) {
        p = readName(m, n, p, nm); if (!p || p + 10 > n) return "";
        uint16_t type = (m[p] << 8) | m[p + 1], len = (m[p + 8] << 8) | m[p + 9];
        p += 10; if (p + len > n) return "";
        if (type == 1 && len == 4 && sameName(nm, name)) {
            char b[16]; snprintf(b, sizeof b, "%d.%d.%d.%d", m[p], m[p + 1], m[p + 2], m[p + 3]); return b;
        }
        p += len;
    }
    return "";
}

// One query, waiting up to timeoutMs for the answer. Blocking: call off the render thread. ifAddr: the local
// address of the interface to send on (multi-homed hosts; the Mini's default multicast route is its VPN).
inline std::string resolve(const std::string& name, int timeoutMs = 1000, const char* ifAddr = nullptr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return "";
    uint16_t id = uint16_t(0x5000 + (getpid() & 0x0fff));
    std::vector<uint8_t> q = buildQuery(name, id);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(5353); inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    unsigned char ttl = 255; setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    if (ifAddr) { in_addr ia{}; inet_pton(AF_INET, ifAddr, &ia); setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof ia); }
    std::string ip;
    for (int attempt = 0; attempt < 2 && ip.empty(); attempt++) {
        if (q.empty() || sendto(fd, q.data(), q.size(), 0, (sockaddr*)&to, sizeof to) < 0) break;
        timeval tv{timeoutMs / 2000, (timeoutMs / 2 % 1000) * 1000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t buf[1500];
        for (int k = 0; k < 8 && ip.empty(); k++) {
            ssize_t r = recv(fd, buf, sizeof buf, 0); if (r <= 0) break;
            ip = parseA(buf, (size_t)r, name);
        }
    }
    close(fd);
    return ip;
}

inline bool isIPv4(const std::string& s) { in_addr a; return inet_pton(AF_INET, s.c_str(), &a) == 1; }

// "key": "value" from a small flat JSON file; "" when missing.
inline std::string field(const std::string& j, const std::string& key) {
    size_t k = j.find("\"" + key + "\""); if (k == std::string::npos) return "";
    size_t a = j.find('"', j.find(':', k) + 1); if (a == std::string::npos) return "";
    size_t b = j.find('"', a + 1); return b == std::string::npos ? "" : j.substr(a + 1, b - a - 1);
}

struct Result { std::string ip, via; };

// The laptop's address, by the order above. useMdns: the HOSTRES flag.
inline Result laptop(const std::string& dir, bool useMdns, const std::string& fallback, std::string defaultName = "Miguels-MacBook-Air") {
    std::stringstream ss; { std::ifstream f(dir + "/hosts.json"); ss << f.rdbuf(); }
    std::string cfg = ss.str(), over = field(cfg, "LAPTOP"), name = field(cfg, "laptopName");
    if (name.empty()) name = defaultName;
    if (isIPv4(over)) return {over, "hosts.json"};
    if (useMdns) {
        std::string ip = resolve(name + ".local");
        if (isIPv4(ip)) { std::ofstream(dir + "/hosts-last.json") << "{\"LAPTOP\": \"" << ip << "\"}"; return {ip, "mdns"}; }
    }
    std::stringstream ls; { std::ifstream f(dir + "/hosts-last.json"); ls << f.rdbuf(); }
    std::string last = field(ls.str(), "LAPTOP");
    if (isIPv4(last)) return {last, "last known"};
    return {fallback, "default"};
}

}  // namespace hosts
