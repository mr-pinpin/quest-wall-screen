// A small JSON reader for asset manifests (glTF). Values only; no writer. Not for untrusted giant inputs: depth is
// limited to 64 and errors leave a null value.
#pragma once
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<Value> a; std::map<std::string, Value> o;
    const Value& operator[](const std::string& k) const { static const Value none; auto it = o.find(k); return it == o.end() ? none : it->second; }
    const Value& operator[](size_t i) const { static const Value none; return i < a.size() ? a[i] : none; }
    bool has(const std::string& k) const { return o.count(k) > 0; }
    size_t size() const { return type == Arr ? a.size() : type == Obj ? o.size() : 0; }
    double num(double def = 0) const { return type == Num ? n : def; }
    int integer(int def = -1) const { return type == Num ? (int)n : def; }
    const std::string& str() const { return s; }
};

struct Parser {
    const char* p; const char* e; bool ok = true;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool lit(const char* w) { size_t n = strlen(w); if ((size_t)(e - p) >= n && !strncmp(p, w, n)) { p += n; return true; } return false; }
    std::string string() {
        std::string out; p++;  // opening quote
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                    case 'u': {
                        if (e - p < 5) { ok = false; return out; }
                        unsigned c = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); p += 4;
                        if (c < 0x80) out += (char)c;
                        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
                        else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
                        break;
                    }
                    default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= e) ok = false; else p++;
        return out;
    }
    Value value(int depth = 0) {
        Value v; ws();
        if (p >= e || depth > 64) { ok = false; return v; }
        if (*p == '{') {
            v.type = Value::Obj; p++; ws();
            if (p < e && *p == '}') { p++; return v; }
            while (ok) {
                ws(); if (p >= e || *p != '"') { ok = false; break; }
                std::string k = string(); ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++; v.o[k] = value(depth + 1); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false;
            }
        } else if (*p == '[') {
            v.type = Value::Arr; p++; ws();
            if (p < e && *p == ']') { p++; return v; }
            while (ok) {
                v.a.push_back(value(depth + 1)); ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false;
            }
        } else if (*p == '"') { v.type = Value::Str; v.s = string(); }
        else if (lit("true")) { v.type = Value::Bool; v.b = true; }
        else if (lit("false")) { v.type = Value::Bool; }
        else if (lit("null")) {}
        else {
            char* end = nullptr; v.n = strtod(p, &end);
            if (end == p) { ok = false; return v; }
            v.type = Value::Num; p = end;
        }
        return v;
    }
};

inline Value parse(const std::string& text, bool* ok = nullptr) {
    Parser ps{text.data(), text.data() + text.size()};
    Value v = ps.value(); ps.ws();
    if (ps.p != ps.e) ps.ok = false;
    if (ok) *ok = ps.ok;
    return ps.ok ? v : Value{};
}

}  // namespace json
