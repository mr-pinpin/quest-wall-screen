// pinpin-mini-screen: token-protected HTTP screen tools for the MINI wall screen (K20).
// The MINI screen's Claude brain runs on the VPS (no claude on the Mini). The Mini only exposes
// screenshot + input of ONE display — the "PinPin Atlas Owned" display (Sunshine output 1975) — so the
// brain can see and act on it over the LAN. Runs as a PinPin Host child, inheriting Screen Recording +
// Accessibility (no re-prompt on rebuild). It NEVER touches Sunshine, never changes displays, and only ever
// drives the one named display.
//
//   GET  /shot                 -> image/png of the target display (ScreenCaptureKit)
//   POST /input {op,...}        -> move|click|type|scroll|key. u,v are 0..10000 across the target display.
//                                  A click WARPS the cursor into the display first (libvirtualhid drops a
//                                  click when the cursor is outside 1975) then clicks at the current cursor.
//   GET  /health                -> {ok, display, name, bounds}
// Auth: Authorization: Bearer <token file ~/.local/share/pinpin-mini-screen/token>. Source restricted to
// loopback / 192.168 / 100.x (Tailscale). Target resolved BY NAME at each use (display ids change on re-create).
import Foundation
import ScreenCaptureKit
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import AppKit

let env = ProcessInfo.processInfo.environment
let TARGET_NAME = env["PINPIN_MINI_DISPLAY_NAME"] ?? "PinPin Atlas Owned"
let PORT = UInt16(env["PINPIN_MINI_SCREEN_PORT"] ?? "57860") ?? 57860
let TOKEN: String = {
    let p = (env["PINPIN_MINI_SCREEN_TOKEN_FILE"] as NSString?)?.expandingTildeInPath
        ?? ("~/.local/share/pinpin-mini-screen/token" as NSString).expandingTildeInPath
    return ((try? String(contentsOfFile: p, encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
}()
let eventSrc = CGEventSource(stateID: .combinedSessionState)

// ---- resolve the target display BY NAME (ids change when displays are re-created) ----
func target() -> (did: CGDirectDisplayID, bounds: CGRect)? {
    for s in NSScreen.screens where s.localizedName == TARGET_NAME {
        let n = (s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? 0
        if n != 0 { return (CGDirectDisplayID(n), CGDisplayBounds(CGDirectDisplayID(n))) }
    }
    return nil
}

func uvToGlobal(_ u: Double, _ v: Double, _ b: CGRect) -> CGPoint {
    CGPoint(x: b.minX + max(0, min(10000, u)) / 10000.0 * b.width,
            y: b.minY + max(0, min(10000, v)) / 10000.0 * b.height)
}

func postMouse(_ type: CGEventType, _ p: CGPoint, click: Int) {
    if let e = CGEvent(mouseEventSource: eventSrc, mouseType: type, mouseCursorPosition: p, mouseButton: .left) {
        if click > 0 { e.setIntegerValueField(.mouseEventClickState, value: Int64(click)) }
        e.post(tap: .cghidEventTap)
    }
}

// ---- input ops (warp INTO the target display first, per libvirtualhid) ----
func doMove(_ u: Double, _ v: Double, _ b: CGRect) {
    let p = uvToGlobal(u, v, b); CGWarpMouseCursorPosition(p); postMouse(.mouseMoved, p, click: 0)
}
func doClick(_ u: Double, _ v: Double, _ b: CGRect) {
    let p = uvToGlobal(u, v, b)
    CGWarpMouseCursorPosition(p)                 // into 1975: a click with the cursor outside it is dropped
    postMouse(.mouseMoved, p, click: 0)          // hover first: some apps ignore a click with no prior pointer
    usleep(40_000)
    postMouse(.leftMouseDown, p, click: 1); usleep(60_000); postMouse(.leftMouseUp, p, click: 1)
}
func doScroll(_ u: Double, _ v: Double, _ dy: Int, _ dx: Int, _ b: CGRect) {
    let p = uvToGlobal(u, v, b); CGWarpMouseCursorPosition(p); postMouse(.mouseMoved, p, click: 0)
    if let e = CGEvent(scrollWheelEvent2Source: eventSrc, units: .pixel, wheelCount: 2,
                       wheel1: Int32(dy), wheel2: Int32(dx), wheel3: 0) { e.post(tap: .cghidEventTap) }
}
func doType(_ s: String) {
    for ch in s {
        for phase in [true, false] {
            if let e = CGEvent(keyboardEventSource: eventSrc, virtualKey: 0, keyDown: phase) {
                var u16 = Array(String(ch).utf16)
                e.keyboardSetUnicodeString(stringLength: u16.count, unicodeString: &u16)
                e.post(tap: .cghidEventTap)
            }
        }
        usleep(4_000)
    }
}
let KEYMAP: [String: CGKeyCode] = ["return": 36, "enter": 36, "tab": 48, "space": 49, "delete": 51,
    "escape": 53, "esc": 53, "left": 123, "right": 124, "down": 125, "up": 126, "l": 37, "t": 17, "r": 15, "w": 13]
func doKey(_ combo: String) {
    let parts = combo.lowercased().split(separator: "+").map { String($0) }
    var flags: CGEventFlags = []
    var key: CGKeyCode? = nil
    for p in parts {
        switch p {
        case "cmd", "command": flags.insert(.maskCommand)
        case "shift": flags.insert(.maskShift)
        case "alt", "option": flags.insert(.maskAlternate)
        case "ctrl", "control": flags.insert(.maskControl)
        default: key = KEYMAP[p]
        }
    }
    guard let k = key else { return }
    for phase in [true, false] {
        if let e = CGEvent(keyboardEventSource: eventSrc, virtualKey: k, keyDown: phase) {
            e.flags = flags; e.post(tap: .cghidEventTap)
        }
    }
}

// ---- screenshot (ScreenCaptureKit; long edge ~900 px, no cursor) ----
func capture(_ did: CGDirectDisplayID) -> Data? {
    let sem = DispatchSemaphore(value: 0); var out: Data? = nil
    Task {
        defer { sem.signal() }
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            guard let disp = content.displays.first(where: { $0.displayID == did }) else { return }
            let cfg = SCStreamConfiguration()
            let scale = min(1.0, 900.0 / Double(disp.width))
            cfg.width = Int(Double(disp.width) * scale); cfg.height = Int(Double(disp.height) * scale)
            cfg.showsCursor = false
            let img = try await SCScreenshotManager.captureImage(
                contentFilter: SCContentFilter(display: disp, excludingWindows: []), configuration: cfg)
            let d = NSMutableData()
            if let dst = CGImageDestinationCreateWithData(d, UTType.png.identifier as CFString, 1, nil) {
                CGImageDestinationAddImage(dst, img, nil)
                if CGImageDestinationFinalize(dst) { out = d as Data }
            }
        } catch { }
    }
    sem.wait(); return out
}

// ---- tiny HTTP/1.1 server (one request at a time; low traffic) ----
func send(_ fd: Int32, _ status: String, _ ctype: String, _ body: Data) {
    let head = "HTTP/1.1 \(status)\r\nContent-Type: \(ctype)\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n"
    var d = head.data(using: .utf8)!; d.append(body)
    d.withUnsafeBytes { _ = write(fd, $0.baseAddress, d.count) }
}
func sendJSON(_ fd: Int32, _ status: String, _ obj: [String: Any]) {
    let b = (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8); send(fd, status, "application/json", b)
}

func serve(_ fd: Int32) {
    defer { close(fd) }
    var buf = [UInt8](repeating: 0, count: 65536); var req = Data()
    while true {   // read headers (and small body)
        let n = read(fd, &buf, buf.count); if n <= 0 { break }
        req.append(contentsOf: buf[0..<n])
        if let r = req.range(of: Data("\r\n\r\n".utf8)) {
            let header = String(data: req.subdata(in: 0..<r.lowerBound), encoding: .utf8) ?? ""
            var clen = 0
            for line in header.split(separator: "\r\n") where line.lowercased().hasPrefix("content-length:") {
                clen = Int(line.split(separator: ":")[1].trimmingCharacters(in: .whitespaces)) ?? 0
            }
            let bodyStart = r.upperBound
            while req.count - bodyStart < clen { let m = read(fd, &buf, buf.count); if m <= 0 { break }; req.append(contentsOf: buf[0..<m]) }
            let body = req.subdata(in: bodyStart..<min(req.count, bodyStart + clen))
            handle(fd, header, body); return
        }
        if req.count > 1_000_000 { break }
    }
}

func handle(_ fd: Int32, _ header: String, _ body: Data) {
    let reqLine = header.split(separator: "\r\n").first.map(String.init) ?? ""
    let parts = reqLine.split(separator: " "); guard parts.count >= 2 else { return }
    let method = String(parts[0]), path = String(parts[1])
    if method == "GET" && path == "/health" {
        if let t = target() {
            return sendJSON(fd, "200 OK", ["ok": true, "display": Int(t.did), "name": TARGET_NAME,
                "bounds": ["x": t.bounds.minX, "y": t.bounds.minY, "w": t.bounds.width, "h": t.bounds.height]])
        }
        return sendJSON(fd, "200 OK", ["ok": false, "error": "display '\(TARGET_NAME)' not found"])
    }
    // auth for everything else
    let auth = header.split(separator: "\r\n").first { $0.lowercased().hasPrefix("authorization:") }
        .map { String($0.split(separator: ":")[1].trimmingCharacters(in: .whitespaces)) } ?? ""
    let tok = auth.hasPrefix("Bearer ") ? String(auth.dropFirst(7)) : auth
    if TOKEN.isEmpty || tok != TOKEN { return sendJSON(fd, "401 Unauthorized", ["error": "unauthorized"]) }
    guard let t = target() else { return sendJSON(fd, "503 Service Unavailable", ["error": "display not found"]) }
    if method == "GET" && path == "/shot" {
        if let png = capture(t.did) { return send(fd, "200 OK", "image/png", png) }
        return sendJSON(fd, "500 Internal Server Error", ["error": "capture failed"])
    }
    if method == "POST" && path == "/input" {
        let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] ?? [:]
        let op = j["op"] as? String ?? ""
        func d(_ k: String) -> Double { (j[k] as? NSNumber)?.doubleValue ?? 0 }
        switch op {
        case "move": doMove(d("u"), d("v"), t.bounds)
        case "click": doClick(d("u"), d("v"), t.bounds)
        case "scroll": doScroll(d("u"), d("v"), Int(d("dy")), Int(d("dx")), t.bounds)
        case "type": doType(j["text"] as? String ?? "")
        case "key": doKey(j["combo"] as? String ?? "")
        default: return sendJSON(fd, "400 Bad Request", ["error": "unknown op"])
        }
        return sendJSON(fd, "200 OK", ["ok": true, "op": op])
    }
    sendJSON(fd, "404 Not Found", ["error": "not found"])
}

// ---- main: listen (source restricted to loopback / LAN / Tailscale) ----
guard CGPreflightScreenCaptureAccess() else { FileHandle.standardError.write(Data("no Screen Recording\n".utf8)); exit(3) }
let s = socket(AF_INET, SOCK_STREAM, 0); var one: Int32 = 1
setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
var addr = sockaddr_in(); addr.sin_family = sa_family_t(AF_INET); addr.sin_port = PORT.bigEndian; addr.sin_addr.s_addr = INADDR_ANY
let bound = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
if bound != 0 || listen(s, 8) != 0 { FileHandle.standardError.write(Data("bind failed\n".utf8)); exit(1) }
FileHandle.standardError.write(Data("pinpin-mini-screen on :\(PORT), target '\(TARGET_NAME)'\n".utf8))
let q = DispatchQueue(label: "mini-screen", qos: .userInitiated)
q.async {
    while true {
        var peer = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let c = withUnsafeMutablePointer(to: &peer) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(s, $0, &len) } }
        if c < 0 { continue }
        let ip = UInt32(bigEndian: peer.sin_addr.s_addr)
        let a = (ip >> 24) & 0xff, b = (ip >> 16) & 0xff            // loopback / 192.168 / 100.x only
        if !(a == 127 || (a == 192 && b == 168) || a == 100) { close(c); continue }
        serve(c)
    }
}
RunLoop.main.run()
