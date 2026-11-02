// In-app GameStream control plane: the headset launches/resumes Sunshine sessions itself over HTTPS with its
// paired client certificate (pinned to the server certificate captured at pairing), then writes
// files/moonlight/session.json for the app's session watcher. Lets the user switch Mini <-> Laptop by pinching.
#pragma once
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

struct GameStreamHost {
    const char* name;      // shown on the button
    const char* address;   // as seen from the headset (the Air is reached through the Mini relay)
    int httpsPort;
    const char* serverPem; // pinned certificate from pairing
    int width, height, fps, kbps;
};

static std::string readFile(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string xmlTag(const std::string& x, const char* tag) {
    std::string o = std::string("<") + tag + ">", c = std::string("</") + tag + ">";
    size_t a = x.find(o); if (a == std::string::npos) return ""; a += o.size();
    size_t b = x.find(c, a); return b == std::string::npos ? "" : x.substr(a, b - a);
}
static std::string hex(const uint8_t* d, size_t n) { static const char* h = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; i++) { s += h[d[i] >> 4]; s += h[d[i] & 15]; } return s; }

struct GameStreamLauncher {
    std::string dir;
    std::thread worker;
    std::atomic<bool> busy{false};
    std::mutex m; std::string status = "idle", active;

    void set(const std::string& s) { std::lock_guard<std::mutex> g(m); status = s; }
    std::string get() { std::lock_guard<std::mutex> g(m); return status; }

    // One HTTPS GET with client certificate; returns the body or "" with `err` set.
    std::string https(const GameStreamHost& h, const std::string& path, const std::string& query, std::string& err) {
        psa_crypto_init();
        std::string cert = readFile(dir + "/moonlight/client.pem"), key = readFile(dir + "/moonlight/client.key"), pin = readFile(dir + "/moonlight/" + h.serverPem);
        mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf; mbedtls_x509_crt own, pinned; mbedtls_pk_context pk;
        mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
        mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf); mbedtls_x509_crt_init(&own); mbedtls_x509_crt_init(&pinned);
        mbedtls_pk_init(&pk); mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);
        std::string body; char port[8]; snprintf(port, sizeof port, "%d", h.httpsPort);
        auto fail = [&](const char* what, int r) { char b[96]; snprintf(b, sizeof b, "%s -0x%04x", what, -r); err = b; };
        int r;
        if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, nullptr, 0))) fail("drbg", r);
        else if ((r = mbedtls_x509_crt_parse(&own, (const unsigned char*)cert.c_str(), cert.size() + 1))) fail("client cert", r);
        else if ((r = mbedtls_pk_parse_key(&pk, (const unsigned char*)key.c_str(), key.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg))) fail("client key", r);
        else if ((r = mbedtls_x509_crt_parse(&pinned, (const unsigned char*)pin.c_str(), pin.size() + 1))) fail("server pin", r);
        else if ((r = mbedtls_net_connect(&net, h.address, port, MBEDTLS_NET_PROTO_TCP))) fail("connect", r);
        else {
            mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);  // self-signed: verified below by exact pin
            mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
            mbedtls_ssl_conf_own_cert(&conf, &own, &pk);
            mbedtls_ssl_conf_read_timeout(&conf, 60000);
            mbedtls_ssl_setup(&ssl, &conf);
            mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
            while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {}
            const mbedtls_x509_crt* peer = r ? nullptr : mbedtls_ssl_get_peer_cert(&ssl);
            if (r) fail("handshake", r);
            else if (!peer || peer->raw.len != pinned.raw.len || memcmp(peer->raw.p, pinned.raw.p, peer->raw.len)) err = "server certificate does not match pairing";
            else {
                std::string req = "GET /" + path + "?" + query + " HTTP/1.1\r\nHost: " + h.address + "\r\nConnection: close\r\n\r\n";
                mbedtls_ssl_write(&ssl, (const unsigned char*)req.data(), req.size());
                unsigned char buf[4096];
                while ((r = mbedtls_ssl_read(&ssl, buf, sizeof buf)) > 0) body.append((char*)buf, r);
                size_t hdr = body.find("\r\n\r\n"); body = hdr == std::string::npos ? "" : body.substr(hdr + 4);
                if (body.empty()) err = "empty response";
            }
            mbedtls_ssl_close_notify(&ssl);
        }
        mbedtls_net_free(&net); mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); mbedtls_x509_crt_free(&own); mbedtls_x509_crt_free(&pinned);
        mbedtls_pk_free(&pk); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent);
        return err.empty() ? body : "";
    }

    void launch(GameStreamHost h) {
        if (busy.exchange(true)) return;
        if (worker.joinable()) worker.join();
        worker = std::thread([this, h] {
            set(std::string("connecting ") + h.name);
            std::string err, uid = "0123456789ABCDEF";
            std::string ident = readFile(dir + "/moonlight/uniqueid"); if (!ident.empty()) uid = ident.substr(0, ident.find_first_of("\r\n"));
            auto q = [&](const std::string& extra) { uint8_t u[16]; randomBytes(u, 16); return "uniqueid=" + uid + "&uuid=" + hex(u, 16) + extra; };
            std::string info = https(h, "serverinfo", q(""), err);
            std::string apps = err.empty() ? https(h, "applist", q(""), err) : "";
            std::string appid = xmlTag(apps, "ID"), current = xmlTag(info, "currentgame");
            if (err.empty() && appid.empty()) err = "no apps";
            if (err.empty() && current != "0" && current != appid) https(h, "cancel", q(""), err);
            uint8_t rikey[16]; randomBytes(rikey, 16); uint32_t rid; randomBytes((uint8_t*)&rid, 4); rid &= 0x7fffffff;
            char params[400];
            snprintf(params, sizeof params, "&appid=%s&mode=%dx%dx%d&additionalStates=1&sops=0&rikey=%s&rikeyid=%u&localAudioPlayMode=0"
                     "&surroundAudioInfo=196610&remoteControllersBitmap=0&gcmap=0&gcpersist=0&corever=1",
                     appid.c_str(), h.width, h.height, h.fps, hex(rikey, 16).c_str(), rid);
            std::string resp = err.empty() ? https(h, current == appid ? "resume" : "launch", q(params), err) : "";
            std::string url = xmlTag(resp, "sessionUrl0");
            if (err.empty() && url.empty()) err = "no session url";
            if (err.empty()) {
                size_t s = url.find("://"), c = url.find(':', s + 3);  // stream through the headset-facing address
                std::string port = c == std::string::npos ? "48010" : url.substr(c + 1);
                url = url.substr(0, s + 3) + h.address + ":" + port;
                std::ostringstream j;
                j << "{\"host\": \"" << h.address << "\", \"rtspSessionUrl\": \"" << url << "\", \"appVersion\": \"" << xmlTag(info, "appversion")
                  << "\", \"gfeVersion\": \"" << xmlTag(info, "GfeVersion") << "\", \"serverCodecModeSupport\": " << atoi(xmlTag(info, "ServerCodecModeSupport").c_str())
                  << ", \"width\": " << h.width << ", \"height\": " << h.height << ", \"fps\": " << h.fps << ", \"bitrateKbps\": " << h.kbps
                  << ", \"hevc\": true, \"rikey\": \"" << hex(rikey, 16) << "\", \"rikeyid\": " << rid << ", \"source\": \"" << h.name << "\"}";
                std::string tmp = dir + "/moonlight/.session.json";
                { std::ofstream f(tmp); f << j.str(); }
                rename(tmp.c_str(), (dir + "/moonlight/session.json").c_str());
                { std::lock_guard<std::mutex> g(m); active = h.name; }
                set(std::string("launched ") + h.name);
            } else set(std::string(h.name) + ": " + err);
            busy = false;
        });
    }
    static void randomBytes(uint8_t* p, size_t n) { int fd = open("/dev/urandom", O_RDONLY); if (fd >= 0) { read(fd, p, n); close(fd); } }
    void shutdown() { if (worker.joinable()) worker.join(); }
};
