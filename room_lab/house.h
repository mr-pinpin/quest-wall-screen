// House file client (flag HOUSE; house/README.md, docs/house-onboarding.md section 4). The Mini serves house.json on
// :57800; the headset keeps the last good copy in files/house.json and works offline from it.
//  - Reads: GET /house?schema=1, the body checked against X-House-SHA256, the rev from X-House-Rev.
//  - Writes: whole objects by id ("edits"), queued in files/house-pending.json so a restart loses none, and sent as one
//    PUT of the whole house with If-Match: <rev it was based on>. On 409 the server returns the current house; the
//    queued edits are re-applied to it and the PUT is retried. Nothing is ever overwritten blind.
//  - Every file write is temp + fsync + rename, so a crash leaves either the old or the new file, never half.
// GL-free and blocking: room_lab runs sync() on a worker thread. bench/house_client_test.cpp drives it against
// house/server.py on the VPS.
#pragma once
#include "json.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace house {

// ---- SHA-256 (FIPS 180-4), for X-House-SHA256.
inline std::string sha256(const std::string& msg) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = msg; uint64_t bits = (uint64_t)msg.size() * 8;
    m += (char)0x80; while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)((bits >> (i * 8)) & 0xff);
    auto rr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)(uint8_t)m[off + 4 * i] << 24 | (uint32_t)(uint8_t)m[off + 4 * i + 1] << 16 | (uint32_t)(uint8_t)m[off + 4 * i + 2] << 8 | (uint8_t)m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65]; for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

// ---- JSON writer for json::Value (keys in sorted order; integers without a decimal point).
inline void write(std::string& o, const json::Value& v) {
    switch (v.type) {
        case json::Value::Null: o += "null"; break;
        case json::Value::Bool: o += v.b ? "true" : "false"; break;
        case json::Value::Num: {
            char buf[32];
            if (std::isfinite(v.n) && v.n == std::floor(v.n) && std::fabs(v.n) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)v.n);
            else if (std::isfinite(v.n)) snprintf(buf, sizeof buf, "%.9g", v.n);
            else snprintf(buf, sizeof buf, "null");
            o += buf; break;
        }
        case json::Value::Str: {
            o += '"';
            for (unsigned char c : v.s) {
                if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
                else if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else if (c == '\r') o += "\\r";
                else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
            }
            o += '"'; break;
        }
        case json::Value::Arr: { o += '['; for (size_t i = 0; i < v.a.size(); i++) { if (i) o += ','; write(o, v.a[i]); } o += ']'; break; }
        case json::Value::Obj: {
            o += '{'; bool first = true;
            for (auto& kv : v.o) { if (!first) o += ','; first = false; json::Value k; k.type = json::Value::Str; k.s = kv.first; write(o, k); o += ':'; write(o, kv.second); }
            o += '}'; break;
        }
    }
}
inline std::string dump(const json::Value& v) { std::string o; write(o, v); return o; }
inline json::Value num(double n) { json::Value v; v.type = json::Value::Num; v.n = n; return v; }
inline json::Value str(const std::string& s) { json::Value v; v.type = json::Value::Str; v.s = s; return v; }
inline json::Value obj() { json::Value v; v.type = json::Value::Obj; return v; }
inline json::Value arr() { json::Value v; v.type = json::Value::Arr; return v; }

// ---- Atomic file write: temp, fsync, rename.
inline bool writeAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) { ssize_t n = ::write(fd, data.data() + done, data.size() - done); if (n <= 0) { close(fd); return false; } done += (size_t)n; }
    bool ok = fsync(fd) == 0; close(fd);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}
inline bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
}

// ---- Minimal HTTP/1.0 client (LAN only, blocking, 5 s timeouts).
struct Response { int status = 0; std::string body, sha, rev, error; };
inline Response http(const std::string& host, int port, const std::string& method, const std::string& path,
                     const std::string& body = "", const std::vector<std::string>& headers = {}) {
    Response r;
    addrinfo hints{}, *ai = nullptr; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ai) != 0 || !ai) { r.error = "resolve"; return r; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { freeaddrinfo(ai); if (fd >= 0) close(fd); r.error = "connect"; return r; }
    freeaddrinfo(ai);
    std::string req = method + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    for (auto& h : headers) req += h + "\r\n";
    req += "\r\n"; req += body;
    for (size_t off = 0; off < req.size();) { ssize_t n = send(fd, req.data() + off, req.size() - off, 0); if (n <= 0) { close(fd); r.error = "send"; return r; } off += (size_t)n; }
    std::string resp; char buf[16384];
    for (;;) { ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) break; resp.append(buf, (size_t)n); }
    close(fd);
    size_t he = resp.find("\r\n\r\n"); if (he == std::string::npos) { r.error = "no response"; return r; }
    std::string head = resp.substr(0, he); r.body = resp.substr(he + 4);
    sscanf(head.c_str(), "HTTP/%*s %d", &r.status);
    std::istringstream hs(head); std::string line; size_t clen = std::string::npos;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':'); if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1); while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        for (auto& ch : k) ch = (char)tolower(ch);
        if (k == "x-house-sha256") r.sha = v; else if (k == "x-house-rev") r.rev = v; else if (k == "content-length") clen = strtoul(v.c_str(), nullptr, 10);
    }
    if (clen != std::string::npos && r.body.size() != clen) { r.error = "truncated"; r.status = 0; }
    return r;
}

// ---- Edits: one whole object by id. section "screens"/"walls"/"shapes"/"rooms" (arrays, by "id"), or "menu"/"settings"
// (objects, replaced whole; id ignored). obj Null deletes from an array.
struct Edit { std::string section, id; json::Value obj; };
inline std::string idOf(const json::Value& v) {
    const json::Value& i = v["id"];
    if (i.type == json::Value::Str) return i.s;
    if (i.type == json::Value::Num) return std::to_string((long long)i.n);
    return "";
}
inline void apply(json::Value& doc, const Edit& e) {
    if (doc.type != json::Value::Obj) return;
    if (e.section == "menu" || e.section == "settings") { doc.o[e.section] = e.obj; return; }
    json::Value& a = doc.o[e.section];
    if (a.type != json::Value::Arr) a = arr();
    for (size_t i = 0; i < a.a.size(); i++) if (idOf(a.a[i]) == e.id) {
        if (e.obj.type == json::Value::Null) a.a.erase(a.a.begin() + i); else a.a[i] = e.obj;
        return;
    }
    if (e.obj.type != json::Value::Null) a.a.push_back(e.obj);
}

struct Store {
    std::string host = "192.168.0.50"; int port = 57800;
    std::string cachePath, pendingPath;   // files/house.json, files/house-pending.json
    json::Value doc; long rev = -1;      // rev of doc as the server last confirmed it (-1: none)
    std::vector<Edit> pending;
    std::string lastError; long gets = 0, puts = 0, conflicts = 0, rejects = 0;

    explicit Store(const std::string& dir = ".") : cachePath(dir + "/house.json"), pendingPath(dir + "/house-pending.json") {}

    // The cached copy and queued edits from the last run. True if a valid cached house was found.
    bool loadCache() {
        std::string s; bool ok = false;
        if (readFile(cachePath, s)) {
            json::Value v = json::parse(s, &ok);
            if (ok && v["schema"].integer() == 1) { doc = v; rev = v["rev"].integer(-1); } else ok = false;
        }
        pending.clear();
        std::string p; bool pok = false;
        if (readFile(pendingPath, p)) {
            json::Value v = json::parse(p, &pok);
            if (pok) for (auto& e : v.a) pending.push_back({e["section"].str(), e["id"].str(), e["obj"]});
        }
        if (ok) for (auto& e : pending) apply(doc, e);  // the local copy shows the queued edits, as before the restart
        return ok;
    }
    bool savePending() const {
        json::Value a = arr();
        for (auto& e : pending) { json::Value o = obj(); o.o["section"] = str(e.section); o.o["id"] = str(e.id); o.o["obj"] = e.obj; a.a.push_back(o); }
        return writeAtomic(pendingPath, dump(a));
    }
    // Queue an edit and apply it to the local copy at once (the app reads doc; the server catches up on sync()).
    void edit(const Edit& e) {
        for (size_t i = 0; i < pending.size(); i++) if (pending[i].section == e.section && pending[i].id == e.id) { pending.erase(pending.begin() + i); break; }
        pending.push_back(e); apply(doc, e); savePending();
    }
    // GET the house; on a good one, keep it (with the local edits re-applied on top) and cache it.
    bool fetch() {
        Response r = http(host, port, "GET", "/house?schema=1"); gets++;
        if (r.status != 200) { lastError = r.error.empty() ? "GET " + std::to_string(r.status) : r.error; return false; }
        return adopt(r.body, r.sha);
    }
    bool adopt(const std::string& body, const std::string& sha) {
        if (!sha.empty() && sha256(body) != sha) { lastError = "sha mismatch"; return false; }
        bool ok = false; json::Value v = json::parse(body, &ok);
        if (!ok || v["schema"].integer() != 1) { lastError = "invalid house"; return false; }
        rev = v["rev"].integer(-1); doc = v;
        for (auto& e : pending) apply(doc, e);
        writeAtomic(cachePath, dump(doc));
        return true;
    }
    // What house/schema.py would refuse in our own edits, fixed before every PUT (2026-11-04: v0.66's first push was
    // refused, 400, and stayed pending: a depth wall's scene held its w-d id, and nextScreenId was not above the new
    // screens). Walls' scene[] keeps only 32-hex Meta uuids; nextScreenId is above every screen id.
    static bool hex32(const std::string& u) { if (u.size() != 32) return false; for (char c : u) if (!isxdigit((unsigned char)c)) return false; return true; }
    static void sanitize(json::Value& d) {
        if (d["walls"].type == json::Value::Arr) for (auto& w : d.o["walls"].a) if (w["scene"].type == json::Value::Arr) {
            json::Value keep = arr(); for (auto& u : w["scene"].a) if (hex32(u.str())) keep.a.push_back(u);
            w.o["scene"] = keep;
        }
        long top = 0; for (auto& s : d["screens"].a) top = std::max(top, (long)s["id"].integer());
        if (d["nextScreenId"].integer(0) <= top) d.o["nextScreenId"] = num((double)(top + 1));
    }
    // Send the queued edits. Returns true when nothing is left to send.
    bool push(const std::string& note = "headset") {
        if (pending.empty()) return true;
        if (rev < 0 && !fetch()) return false;
        for (int attempt = 0; attempt < 3; attempt++) {
            json::Value d = doc; d.o["by"] = str("headset"); sanitize(d);
            std::string body = dump(d);
            Response r = http(host, port, "PUT", "/house", body,
                              {"If-Match: " + std::to_string(rev), "X-House-SHA256: " + sha256(body), "X-House-Note: " + note,
                               "Content-Type: application/json"});
            puts++;
            if (r.status == 200) {
                bool ok = false; json::Value v = json::parse(r.body, &ok);
                rev = ok ? v["rev"].integer(rev + 1) : rev + 1; doc.o["rev"] = num((double)rev); doc.o["by"] = str("headset");
                pending.clear(); savePending(); writeAtomic(cachePath, dump(doc));
                return true;
            }
            if (r.status == 409) { conflicts++; if (!adopt(r.body, r.sha)) return false; continue; }  // re-applied on the current house
            if (r.status == 400 || r.status == 428) { rejects++; lastError = "PUT " + std::to_string(r.status) + ": " + r.body.substr(0, 200); return false; }
            lastError = r.error.empty() ? "PUT " + std::to_string(r.status) : r.error; return false;
        }
        lastError = "conflicts kept coming"; return false;
    }
    // One sync round: fetch when there's nothing to send (picks up edits from the VPS), push otherwise.
    bool sync() { return pending.empty() ? fetch() : push(); }
};

}  // namespace house
