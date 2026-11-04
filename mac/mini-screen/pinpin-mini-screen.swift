// pinpin-mini-screen: token-protected HTTP screen tools for the MINI wall screen (K20).
// The MINI screen's Claude brain runs on the VPS (no claude on the Mini). The Mini only exposes
// screenshot + input of ONE display — the virtual display named "PinPin Atlas Owned" (the one Sunshine
// streams to the headset). The brain sees + acts on it over the LAN. This helper NEVER touches Sunshine,
// never changes displays, drives only that one named display, and refuses to act while the USER is pointing
// (the cursor is shared with the user's live Moonlight pointer).  Contract: docs/mini-screen-tools.md.
//
//   GET  /health                 -> {ok, display, name, bounds}   (no auth; the apps /status row polls it)
//   GET  /shot[?max=<px>]         -> image/png of the target display (SCK); X-Display-Id/X-Display-Bounds headers
//   POST /input {op,...}          -> move|click|type|scroll|key. u,v are 0..10000 across the display.
// Auth: Authorization: Bearer <token ~/.local/share/pinpin-mini-screen/token>. Source: loopback/192.168/100.x.
import Foundation
import ScreenCaptureKit
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import AppKit

let env = ProcessInfo.processInfo.environment
let TARGET_NAME = env["PINPIN_MINI_DISPLAY_NAME"] ?? "PinPin Atlas Owned"
let PORT = UInt16(env["PINPIN_MINI_SCREEN_PORT"] ?? "57860") ?? 57860
let IDLE_WIN = Double(env["PINPIN_MINI_IDLE_S"] ?? "3") ?? 3        // refuse if the user acted within this
let TOKEN: String = {
    let p = (env["PINPIN_MINI_SCREEN_TOKEN_FILE"] as NSString?)?.expandingTildeInPath
        ?? ("~/.local/share/pinpin-mini-screen/token" as NSString).expandingTildeInPath
    return ((try? String(contentsOfFile: p, encoding: .utf8)) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
}()
let AUDIT = ("~/.local/share/pinpin-mini-screen/input.log" as NSString).expandingTildeInPath
let eventSrc = CGEventSource(stateID: .combinedSessionState)
let inputLock = NSLock()               // (2) serialise input so two callers can't interleave move/click
var myLastPost = Date.distantPast      // (1) track MY posted events so they don't count as "user active"

func markPost() { myLastPost = Date() }

// (1) was there REAL user input (not my own posts) within `win` seconds? Returns (active, sinceAnyHID).
func userActive(_ win: Double) -> (Bool, Double) {
    let anyType = CGEventType(rawValue: ~UInt32(0)) ?? .mouseMoved        // kCGAnyInputEventType
    let since = CGEventSource.secondsSinceLastEventType(.hidSystemState, eventType: anyType)
    let myIdle = Date().timeIntervalSince(myLastPost)
    // a HID event newer than my last post (by >0.2s) must be the user's
    return (since < win && since < myIdle - 0.2, since)
}

func audit(_ op: String, _ u: Double, _ v: Double, _ ip: String) {
    let line = "\(ISO8601DateFormatter().string(from: Date())) \(op) u=\(Int(u)) v=\(Int(v)) from=\(ip)\n"
    if let a = try? FileManager.default.attributesOfItem(atPath: AUDIT), (a[.size] as? Int ?? 0) > 512_000 {
        try? FileManager.default.removeItem(atPath: AUDIT)
    }
    if let h = FileHandle(forWritingAtPath: AUDIT) { h.seekToEndOfFile(); h.write(Data(line.utf8)); try? h.close() }
    else { try? line.write(toFile: AUDIT, atomically: true, encoding: .utf8) }
}

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

func postMouse(_ type: CGEventType, _ p: CGPoint, button: CGMouseButton, click: Int) {
    if let e = CGEvent(mouseEventSource: eventSrc, mouseType: type, mouseCursorPosition: p, mouseButton: button) {
        if click > 0 { e.setIntegerValueField(.mouseEventClickState, value: Int64(click)) }
        e.post(tap: .cghidEventTap); markPost()
    }
}

// ---- input ops (warp INTO the target display first, per libvirtualhid) ----
func doMove(_ p: CGPoint) { CGWarpMouseCursorPosition(p); markPost(); postMouse(.mouseMoved, p, button: .left, click: 0) }

func doClick(_ p: CGPoint, button: String, count: Int) {
    let (down, up, btn): (CGEventType, CGEventType, CGMouseButton) = {
        switch button { case "right": return (.rightMouseDown, .rightMouseUp, .right)
                        case "middle": return (.otherMouseDown, .otherMouseUp, .center)
                        default: return (.leftMouseDown, .leftMouseUp, .left) }
    }()
    CGWarpMouseCursorPosition(p); markPost()
    postMouse(.mouseMoved, p, button: .left, click: 0); usleep(40_000)   // hover first
    for i in 1...max(1, count) { postMouse(down, p, button: btn, click: i); usleep(60_000); postMouse(up, p, button: btn, click: i) }
}
func doScroll(_ p: CGPoint, _ dy: Int, _ dx: Int) {
    CGWarpMouseCursorPosition(p); markPost(); postMouse(.mouseMoved, p, button: .left, click: 0)
    if let e = CGEvent(scrollWheelEvent2Source: eventSrc, units: .pixel, wheelCount: 2,
                       wheel1: Int32(dy), wheel2: Int32(dx), wheel3: 0) { e.post(tap: .cghidEventTap); markPost() }
}
func doType(_ s: String) {
    for ch in s.prefix(5000) {                                  // (3) cap length
        for phase in [true, false] {
            if let e = CGEvent(keyboardEventSource: eventSrc, virtualKey: 0, keyDown: phase) {
                var u16 = Array(String(ch).utf16)
                e.keyboardSetUnicodeString(stringLength: u16.count, unicodeString: &u16)
                e.post(tap: .cghidEventTap)
            }
        }
        usleep(4_000)
    }
    markPost()
}
let KEYMAP: [String: CGKeyCode] = ["return": 36, "enter": 36, "tab": 48, "space": 49, "delete": 51,
    "escape": 53, "esc": 53, "left": 123, "right": 124, "down": 125, "up": 126, "l": 37, "t": 17, "r": 15, "w": 13]
func doKey(_ combo: String) {
    var flags: CGEventFlags = []; var key: CGKeyCode? = nil
    for p in combo.lowercased().split(separator: "+").map(String.init) {
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
        if let e = CGEvent(keyboardEventSource: eventSrc, virtualKey: k, keyDown: phase) { e.flags = flags; e.post(tap: .cghidEventTap) }
    }
    markPost()
}

// ---- screenshot (ScreenCaptureKit; long edge capped, no cursor) ----
func capture(_ did: CGDirectDisplayID, _ maxPx: Double) -> Data? {
    let sem = DispatchSemaphore(value: 0); var out: Data? = nil
    Task {
        defer { sem.signal() }
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            guard let disp = content.displays.first(where: { $0.displayID == did }) else { return }
            let cfg = SCStreamConfiguration()
            let scale = min(1.0, maxPx / Double(disp.width))     // uniform scale -> u,v map 1:1 onto the image
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

// ---- tiny HTTP/1.1 server ----
func send(_ fd: Int32, _ status: String, _ ctype: String, _ body: Data, _ extra: [String: String] = [:]) {
    var head = "HTTP/1.1 \(status)\r\nContent-Type: \(ctype)\r\nContent-Length: \(body.count)\r\nConnection: close\r\n"
    for (k, v) in extra { head += "\(k): \(v)\r\n" }
    head += "\r\n"
    var d = Data(head.utf8); d.append(body)
    d.withUnsafeBytes { _ = write(fd, $0.baseAddress, d.count) }
}
func sendJSON(_ fd: Int32, _ status: String, _ obj: [String: Any]) {
    send(fd, status, "application/json", (try? JSONSerialization.data(withJSONObject: obj)) ?? Data("{}".utf8))
}

func serve(_ fd: Int32, _ ip: String) {
    defer { close(fd) }
    var buf = [UInt8](repeating: 0, count: 65536); var req = Data()
    while true {
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
            handle(fd, header, req.subdata(in: bodyStart..<min(req.count, bodyStart + clen)), ip); return
        }
        if req.count > 2_000_000 { break }
    }
}

func handle(_ fd: Int32, _ header: String, _ body: Data, _ ip: String) {
    let lines = header.split(separator: "\r\n")
    let parts = (lines.first.map(String.init) ?? "").split(separator: " "); guard parts.count >= 2 else { return }
    let method = String(parts[0]), full = String(parts[1])
    let path = String(full.split(separator: "?").first ?? ""); let query = full.contains("?") ? String(full.split(separator: "?", maxSplits: 1)[1]) : ""
    if method == "GET" && path == "/health" {
        if let t = target() {
            return sendJSON(fd, "200 OK", ["ok": true, "display": Int(t.did), "name": TARGET_NAME,
                "bounds": ["x": t.bounds.minX, "y": t.bounds.minY, "w": t.bounds.width, "h": t.bounds.height]])
        }
        return sendJSON(fd, "200 OK", ["ok": false, "error": "display '\(TARGET_NAME)' not found"])
    }
    let auth = lines.first { $0.lowercased().hasPrefix("authorization:") }
        .map { String($0.drop(while: { $0 != ":" }).dropFirst()).trimmingCharacters(in: .whitespaces) } ?? ""
    let tok = auth.hasPrefix("Bearer ") ? String(auth.dropFirst(7)) : auth
    if TOKEN.isEmpty || tok != TOKEN { return sendJSON(fd, "401 Unauthorized", ["error": "unauthorized"]) }
    guard let t = target() else { return sendJSON(fd, "503 Service Unavailable", ["error": "display not found"]) }  // (5)
    let dispHdr = ["X-Display-Id": "\(t.did)",
                   "X-Display-Bounds": "\(Int(t.bounds.minX)),\(Int(t.bounds.minY)),\(Int(t.bounds.width)),\(Int(t.bounds.height))"]
    if method == "GET" && path == "/shot" {
        var maxPx = 900.0
        for kv in query.split(separator: "&") where kv.hasPrefix("max=") { maxPx = Double(kv.dropFirst(4)) ?? 900 }
        if let png = capture(t.did, maxPx) { return send(fd, "200 OK", "image/png", png, dispHdr) }    // (4)
        return sendJSON(fd, "500 Internal Server Error", ["error": "capture failed"])
    }
    if method == "POST" && path == "/input" {
        let j = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] ?? [:]
        let op = j["op"] as? String ?? ""
        func d(_ k: String) -> Double { (j[k] as? NSNumber)?.doubleValue ?? 0 }
        let force = (j["force"] as? Bool) ?? false
        inputLock.lock(); defer { inputLock.unlock() }                                   // (2) serialise
        if !force {                                                                      // (1) don't fight the user
            let (active, idle) = userActive(IDLE_WIN)
            if active { return sendJSON(fd, "409 Conflict", ["error": "user_active", "idle_s": idle]) }
        }
        let p = uvToGlobal(d("u"), d("v"), t.bounds)
        switch op {
        case "move": doMove(p)
        case "click": doClick(p, button: (j["button"] as? String) ?? "left", count: Int(d("count")) == 0 ? 1 : Int(d("count")))
        case "scroll": doScroll(p, Int(d("dy")), Int(d("dx")))
        case "type": doType(j["text"] as? String ?? "")
        case "key": doKey(j["combo"] as? String ?? "")
        default: return sendJSON(fd, "400 Bad Request", ["error": "unknown op"])
        }
        audit(op, d("u"), d("v"), ip)                                                    // (6)
        return sendJSON(fd, "200 OK", ["ok": true, "op": op, "x": Int(p.x), "y": Int(p.y), "display": Int(t.did)])  // (3)
    }
    sendJSON(fd, "404 Not Found", ["error": "not found"])
}

// ---- main ----
guard CGPreflightScreenCaptureAccess() else { FileHandle.standardError.write(Data("no Screen Recording\n".utf8)); exit(3) }
let s = socket(AF_INET, SOCK_STREAM, 0); var one: Int32 = 1
setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
var addr = sockaddr_in(); addr.sin_family = sa_family_t(AF_INET); addr.sin_port = PORT.bigEndian; addr.sin_addr.s_addr = INADDR_ANY
let bound = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
if bound != 0 || listen(s, 8) != 0 { FileHandle.standardError.write(Data("bind failed\n".utf8)); exit(1) }
FileHandle.standardError.write(Data("pinpin-mini-screen on :\(PORT), target '\(TARGET_NAME)'\n".utf8))
DispatchQueue(label: "mini-screen", qos: .userInitiated).async {
    while true {
        var peer = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let c = withUnsafeMutablePointer(to: &peer) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(s, $0, &len) } }
        if c < 0 { continue }
        let raw = UInt32(bigEndian: peer.sin_addr.s_addr)
        let a = (raw >> 24) & 0xff, b = (raw >> 16) & 0xff
        if !(a == 127 || (a == 192 && b == 168) || a == 100) { close(c); continue }     // loopback/192.168/100.x
        let ip = "\(a).\(b).\((raw >> 8) & 0xff).\(raw & 0xff)"
        serve(c, ip)
    }
}
RunLoop.main.run()
