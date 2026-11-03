// PinPin Host: the one Mac app that holds every permission PinPin needs, and runs every tool as a plugin.
//
// Why: macOS ties a privacy grant (Screen Recording, Accessibility, ...) to the code signature of the app that is
// "responsible" for a process. Ad-hoc signed tools get a new identity on every rebuild, so every rebuild needed
// new clicks. This app is signed with one permanent identity ("Mr PinPin Code Signing"), so its grants survive
// rebuilds. Every tool runs as its child process, and macOS charges children to the app that launched them, so
// tools can be rebuilt and swapped at will without ever asking again.
//
// Plugins: one JSON file per plugin in ~/Library/Application Support/PinPin Host/plugins/<name>.json:
//   {"exec": "~/.local/share/pinpin-atlas/pinpin-atlas", "argsFile": "~/.local/share/pinpin-atlas/args",
//    "args": [], "env": {}, "cwd": "~", "log": "~/.local/share/pinpin-atlas/atlas.log", "truncateLog": true,
//    "enabled": true, "restartDelay": 1.5}
// The host starts every enabled plugin, restarts it after any exit (always from the binary on disk, so
// "replace the file, pkill the process" is an upgrade), and re-reads the folder every 3 s (new, changed and
// removed files take effect on their own).
//
// Host restarts leave plugins running: a new host adopts the copy the last one started (same pid file, same
// binary, same spec) instead of restarting it, so rebuilding the host never interrupts a stream, a benchmark or
// the extra displays. launchd's SIGTERM (kickstart -k, bootout) therefore stops only the host; SIGINT or
// POST /stopall stops the plugins too. A changed spec still restarts that plugin.
//
// Control (localhost only, for ssh): http://127.0.0.1:57790
//   GET /status          permissions and plugins as JSON
//   POST /start|/stop|/restart?name=<plugin>   (stop holds until /start or a host restart; use "enabled": false to keep it off)
//   POST /stopall        stop every plugin (held until a host restart)
//   POST /request?perm=<screen|audio|accessibility|input|mic|camera|localnet>   show that permission's prompt
import AppKit
import AVFoundation
import ApplicationServices
import CoreAudio
import CoreGraphics
import IOKit.hid
import Network

let home = FileManager.default.homeDirectoryForCurrentUser.path
let support = home + "/Library/Application Support/PinPin Host"
let pluginDir = support + "/plugins", runDir = support + "/run", logDir = support + "/logs"
// Supervision and the control server run on this queue, never on the main thread: AppKit can block the main
// thread on WindowServer (seen 2026-11-03: a display-timing query hung for minutes after a replayd crash), and
// that must never stop plugins from being restarted.
let work = DispatchQueue(label: "io.mr-pinpin.host.supervisor")
func expand(_ p: String) -> String { p.hasPrefix("~") ? home + p.dropFirst() : p }
func hostLog(_ s: String) {
    let line = "\(ISO8601DateFormatter().string(from: Date())) \(s)\n"
    FileHandle.standardError.write(line.data(using: .utf8)!)
}

// MARK: - Permissions

enum Perm: String, CaseIterable {
    case screen, audio, accessibility, input, mic, camera, localnet
    var title: String {
        switch self {
        case .screen: return "Screen & System Audio Recording"
        case .audio: return "System audio capture"
        case .accessibility: return "Accessibility (mouse and keyboard)"
        case .input: return "Input Monitoring"
        case .mic: return "Microphone"
        case .camera: return "Camera"
        case .localnet: return "Local Network"
        }
    }
}

// TCC's own preflight, for the services with no public check (system audio capture). nil = unavailable.
typealias TCCPreflight = @convention(c) (CFString, CFDictionary?) -> Int32
let tccPreflight: TCCPreflight? = {
    guard let h = dlopen("/System/Library/PrivateFrameworks/TCC.framework/TCC", RTLD_NOW), let s = dlsym(h, "TCCAccessPreflight") else { return nil }
    return unsafeBitCast(s, to: TCCPreflight.self)
}()

final class Permissions {
    var localNetDenied: Bool? = nil
    private var probe: NWConnection?

    func granted(_ p: Perm) -> Bool? {
        switch p {
        case .screen: return CGPreflightScreenCaptureAccess()
        case .audio: return tccPreflight.map { $0("kTCCServiceAudioCapture" as CFString, nil) == 0 }
        case .accessibility: return AXIsProcessTrusted()
        case .input: return IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted
        case .mic: return AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
        case .camera: return AVCaptureDevice.authorizationStatus(for: .video) == .authorized
        case .localnet: return localNetDenied.map { !$0 }
        }
    }

    func request(_ p: Perm) {
        switch p {
        case .screen:
            if !CGRequestScreenCaptureAccess() { openSettings("Privacy_ScreenCapture") }
        case .audio: AudioTapPrompt.run()
        case .accessibility:
            let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
            if !AXIsProcessTrustedWithOptions(opts) { openSettings("Privacy_Accessibility") }
        case .input:
            if !IOHIDRequestAccess(kIOHIDRequestTypeListenEvent) { openSettings("Privacy_ListenEvent") }
        case .mic: AVCaptureDevice.requestAccess(for: .audio) { _ in }
        case .camera: AVCaptureDevice.requestAccess(for: .video) { _ in }
        case .localnet: probeLocalNetwork()
        }
    }

    func openSettings(_ pane: String) {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)")!)
    }

    // Local Network has no check API: connect to the router and read why the path is (un)satisfied.
    // The first attempt also shows the prompt.
    func probeLocalNetwork() {
        probe?.cancel()
        let c = NWConnection(host: "192.168.0.1", port: 80, using: .tcp)
        c.pathUpdateHandler = { [weak self] path in
            if #available(macOS 14.0, *) {
                if path.unsatisfiedReason == .localNetworkDenied { self?.localNetDenied = true }
                else if path.status == .satisfied { self?.localNetDenied = false }
            }
        }
        c.stateUpdateHandler = { [weak self] st in
            if case .ready = st { self?.localNetDenied = false; c.cancel() }
        }
        c.start(queue: work); probe = c
        work.asyncAfter(deadline: .now() + 5) { c.cancel() }
    }

    var json: [String: Any] {
        var d: [String: Any] = [:]
        for p in Perm.allCases { d[p.rawValue] = granted(p).map { $0 as Any } ?? NSNull() }
        return d
    }
}

// System audio capture asks for permission the first time audio flows from a process tap. Run a tap for a
// moment so the prompt appears (and so the grant is recorded for this app, and through it for every plugin).
enum AudioTapPrompt {
    static func run() {
        guard #available(macOS 14.2, *) else { return }
        let desc = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        desc.name = "PinPin Host permission check"; desc.isPrivate = true
        var tap = AudioObjectID(kAudioObjectUnknown)
        guard AudioHardwareCreateProcessTap(desc, &tap) == noErr else { hostLog("audio tap: create failed"); return }
        let agg: [String: Any] = [kAudioAggregateDeviceNameKey: "PinPin Host tap", kAudioAggregateDeviceUIDKey: "io.mr-pinpin.host.tap.\(UUID().uuidString)",
                                  kAudioAggregateDeviceIsPrivateKey: true, kAudioAggregateDeviceTapAutoStartKey: true,
                                  kAudioAggregateDeviceTapListKey: [[kAudioSubTapUIDKey: desc.uuid.uuidString]]]
        var dev = AudioObjectID(kAudioObjectUnknown)
        guard AudioHardwareCreateAggregateDevice(agg as CFDictionary, &dev) == noErr else { AudioHardwareDestroyProcessTap(tap); return }
        var proc: AudioDeviceIOProcID?
        AudioDeviceCreateIOProcIDWithBlock(&proc, dev, nil) { _, _, _, _, _ in }
        if let proc = proc { AudioDeviceStart(dev, proc) }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            if let proc = proc { AudioDeviceStop(dev, proc); AudioDeviceDestroyIOProcID(dev, proc) }
            AudioHardwareDestroyAggregateDevice(dev); AudioHardwareDestroyProcessTap(tap)
        }
    }
}

// MARK: - Plugins

struct PluginSpec: Codable, Equatable {
    var exec: String
    var args: [String]?
    var argsFile: String?
    var env: [String: String]?
    var cwd: String?
    var log: String?
    var truncateLog: Bool?
    var enabled: Bool?
    var restartDelay: Double?
    // Identifies the spec a running copy was started with (stored in its pid file), so only a matching copy is adopted.
    var key: String {
        let e = JSONEncoder(); e.outputFormatting = [.sortedKeys]
        return (try? e.encode(self)).flatMap { String(data: $0, encoding: .utf8) } ?? ""
    }
}

final class Plugin {
    let name: String
    var spec: PluginSpec
    var process: Process?
    var adopted: Int32?, exitWatch: DispatchSourceProcess?  // a copy started by an earlier host instance
    var running: Bool { process != nil || adopted != nil }
    var pid: Int32 { process?.processIdentifier ?? adopted ?? 0 }
    var restarts = 0, quickFails = 0, held = false, stopping = false
    var startedAt: Date?, lastExit: String = ""
    init(name: String, spec: PluginSpec) { self.name = name; self.spec = spec }

    var pidFile: String { runDir + "/\(name).pid" }

    func start() {
        guard !running, spec.enabled ?? true, !held else { return }
        if adopt() { return }
        killStale()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: expand(spec.exec))
        var args = spec.args ?? []
        if let f = spec.argsFile, let s = try? String(contentsOfFile: expand(f), encoding: .utf8) {
            args = s.split(whereSeparator: { $0.isWhitespace }).map(String.init) + args
        }
        p.arguments = args.map(expand)  // "~/..." works in args too
        if let env = spec.env { p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 } }
        p.currentDirectoryURL = URL(fileURLWithPath: expand(spec.cwd ?? "~"))
        let logPath = expand(spec.log ?? logDir + "/\(name).log")
        if spec.truncateLog ?? true || !FileManager.default.fileExists(atPath: logPath) {
            FileManager.default.createFile(atPath: logPath, contents: nil)
        }
        if let h = FileHandle(forWritingAtPath: logPath) { h.seekToEndOfFile(); p.standardOutput = h; p.standardError = h }
        p.standardInput = FileHandle.nullDevice
        p.terminationHandler = { [weak self] proc in work.async { self?.exited(proc) } }
        do {
            try p.run()
            process = p; startedAt = Date()
            try? "\(p.processIdentifier)\n\(spec.key)".write(toFile: pidFile, atomically: true, encoding: .utf8)
            hostLog("\(name): started pid \(p.processIdentifier)")
        } catch {
            lastExit = "launch failed: \(error.localizedDescription)"; hostLog("\(name): \(lastExit)")
            scheduleRestart()
        }
    }

    // The copy a previous host instance left running, if its pid file names a live process of this binary.
    func previous() -> (pid: Int32, key: String)? {
        guard let s = try? String(contentsOfFile: pidFile, encoding: .utf8) else { return nil }
        let lines = s.split(separator: "\n", maxSplits: 1).map(String.init)
        guard let pid = Int32(lines.first?.trimmingCharacters(in: .whitespaces) ?? ""), pid > 1, kill(pid, 0) == 0 else { return nil }
        var buf = [CChar](repeating: 0, count: 4096)
        guard proc_pidpath(pid, &buf, UInt32(buf.count)) > 0, String(cString: buf) == expand(spec.exec) else { return nil }
        return (pid, lines.count > 1 ? lines[1] : "")
    }

    // Keep that copy if it was started with the current spec: watch it for exit instead of restarting it.
    func adopt() -> Bool {
        guard let prev = previous(), prev.key == spec.key else { return false }
        let src = DispatchSource.makeProcessSource(identifier: prev.pid, eventMask: .exit, queue: work)
        src.setEventHandler { [weak self] in self?.adoptedExited(prev.pid) }
        src.resume()
        adopted = prev.pid; exitWatch = src; startedAt = Date()
        hostLog("\(name): adopted running pid \(prev.pid)")
        if kill(prev.pid, 0) != 0 { adoptedExited(prev.pid) }  // died between the check and the watch
        return true
    }

    func adoptedExited(_ pid: Int32) {
        guard adopted == pid else { return }
        exitWatch?.cancel(); exitWatch = nil; adopted = nil
        lastExit = "adopted pid \(pid) exited at \(ISO8601DateFormatter().string(from: Date()))"
        hostLog("\(name): \(lastExit)")
        quickFails = 0
        try? FileManager.default.removeItem(atPath: pidFile)
        if stopping { stopping = false; return }
        scheduleRestart()
    }

    // A copy started with another spec (or by an older host) is replaced, so there is one copy, matching the spec.
    func killStale() {
        guard let pid = previous()?.pid else { return }
        kill(pid, SIGTERM); usleep(500_000); if kill(pid, 0) == 0 { kill(pid, SIGKILL) }
        hostLog("\(name): replaced stale pid \(pid)")
    }

    func exited(_ p: Process) {
        guard p === process else { return }
        lastExit = "exit \(p.terminationStatus) (\(p.terminationReason == .uncaughtSignal ? "signal" : "status")) at \(ISO8601DateFormatter().string(from: Date()))"
        hostLog("\(name): \(lastExit)")
        // Dying within 10 s of starting (e.g. exit 1: another copy holds the port) backs off exponentially.
        quickFails = (startedAt.map { Date().timeIntervalSince($0) < 10 } ?? true) ? quickFails + 1 : 0
        process = nil; try? FileManager.default.removeItem(atPath: pidFile)
        if stopping { stopping = false; return }
        scheduleRestart()
    }

    func scheduleRestart() {
        guard spec.enabled ?? true, !held else { return }
        restarts += 1
        let delay = min(60, (spec.restartDelay ?? 1.5) * pow(2, Double(max(0, quickFails - 1))))
        work.asyncAfter(deadline: .now() + delay) { [weak self] in self?.start() }
    }

    func stop(hold: Bool) {
        held = hold
        guard running else { return }
        let pid = self.pid
        stopping = true
        if let p = process { p.terminate() } else { kill(pid, SIGTERM) }
        work.asyncAfter(deadline: .now() + 3) { if kill(pid, 0) == 0 { kill(pid, SIGKILL) } }
    }

    var json: [String: Any] {
        ["name": name, "running": running, "pid": pid, "adopted": adopted != nil, "restarts": restarts,
         "held": held, "enabled": spec.enabled ?? true, "exec": spec.exec, "lastExit": lastExit,
         "startedAt": startedAt.map { ISO8601DateFormatter().string(from: $0) } ?? ""]
    }
}

final class Supervisor {
    var plugins: [String: Plugin] = [:]

    func scan() {
        let files = (try? FileManager.default.contentsOfDirectory(atPath: pluginDir)) ?? []
        var seen = Set<String>()
        for f in files where f.hasSuffix(".json") {
            let name = String(f.dropLast(5)); seen.insert(name)
            guard let data = FileManager.default.contents(atPath: pluginDir + "/" + f),
                  let spec = try? JSONDecoder().decode(PluginSpec.self, from: data) else { continue }
            if let p = plugins[name] {
                if p.spec != spec {
                    hostLog("\(name): spec changed"); p.stop(hold: false); p.spec = spec
                    work.asyncAfter(deadline: .now() + 1) { p.start() }
                }
            } else {
                let p = Plugin(name: name, spec: spec); plugins[name] = p; p.start()
            }
        }
        for (name, p) in plugins where !seen.contains(name) { hostLog("\(name): removed"); p.stop(hold: true); plugins[name] = nil }
    }

    func stopAll() { plugins.values.forEach { $0.stop(hold: true) } }
}

// MARK: - Control server (127.0.0.1:57790)

final class Control {
    let sup: Supervisor, perms: Permissions
    var listener: NWListener?
    init(sup: Supervisor, perms: Permissions) { self.sup = sup; self.perms = perms }

    func start() {
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: 57790)
        guard let l = try? NWListener(using: params) else { hostLog("control: listen failed"); return }
        l.newConnectionHandler = { [weak self] c in self?.serve(c) }
        l.start(queue: work); listener = l
    }

    func serve(_ c: NWConnection) {
        c.start(queue: work)
        c.receive(minimumIncompleteLength: 1, maximumLength: 16384) { [weak self] data, _, _, _ in
            guard let self = self, let data = data, let req = String(data: data, encoding: .utf8) else { c.cancel(); return }
            let first = req.split(separator: "\r\n").first?.split(separator: " ") ?? []
            let method = first.count > 0 ? String(first[0]) : "", target = first.count > 1 ? String(first[1]) : "/"
            let comps = URLComponents(string: target)
            let q = Dictionary((comps?.queryItems ?? []).map { ($0.name, $0.value ?? "") }, uniquingKeysWith: { $1 })
            let (code, body) = self.handle(method, comps?.path ?? "/", q)
            let out = "HTTP/1.1 \(code)\r\nContent-Type: application/json\r\nContent-Length: \(body.utf8.count)\r\nConnection: close\r\n\r\n\(body)"
            c.send(content: out.data(using: .utf8), completion: .contentProcessed { _ in c.cancel() })
        }
    }

    func handle(_ method: String, _ path: String, _ q: [String: String]) -> (String, String) {
        func js(_ o: Any) -> String { String(data: (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys])) ?? Data(), encoding: .utf8) ?? "{}" }
        switch (method, path) {
        case ("GET", "/status"):
            let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
            return ("200 OK", js(["version": version, "pid": getpid(), "permissions": perms.json,
                                  "plugins": sup.plugins.values.sorted { $0.name < $1.name }.map { $0.json }]))
        case ("POST", "/start"), ("POST", "/stop"), ("POST", "/restart"):
            guard let p = sup.plugins[q["name"] ?? ""] else { return ("404 Not Found", js(["error": "no such plugin"])) }
            if path == "/stop" { p.stop(hold: true) }
            else if path == "/start" { p.held = false; p.start() }
            else { p.held = false; p.stop(hold: false); work.asyncAfter(deadline: .now() + 1) { p.start() } }
            return ("200 OK", js(p.json))
        case ("POST", "/stopall"):
            sup.stopAll()
            return ("200 OK", js(["stopped": sup.plugins.keys.sorted()]))
        case ("POST", "/request"):
            guard let perm = Perm(rawValue: q["perm"] ?? "") else { return ("400 Bad Request", js(["error": "unknown perm"])) }
            DispatchQueue.main.async { self.perms.request(perm) }  // prompts are UI: main thread
            return ("200 OK", js(["requested": perm.rawValue]))
        default:
            return ("404 Not Found", js(["error": "GET /status, POST /start|/stop|/restart?name=, POST /stopall, POST /request?perm="]))
        }
    }
}

// MARK: - UI: a menu-bar item, and a one-page checklist while anything is missing

// The core keeps plugins alive. It starts before AppKit and never needs the main thread: on 2026-11-03 the very
// first AppKit call to WindowServer hung for minutes, and plugins must keep running (and restarting) through that.
final class Core {
    let perms = Permissions(), sup = Supervisor()
    lazy var control = Control(sup: sup, perms: perms)
    var signalSources: [DispatchSourceSignal] = []
    var scanTimer: DispatchSourceTimer?
    func start() {
        for d in [pluginDir, runDir, logDir] { try? FileManager.default.createDirectory(atPath: d, withIntermediateDirectories: true) }
        hostLog("PinPin Host \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] ?? "") starting")
        work.async { [self] in
            perms.probeLocalNetwork()  // no UI needed; re-run by the menu when it is up
            control.start(); sup.scan()
            let t = DispatchSource.makeTimerSource(queue: work)
            t.schedule(deadline: .now() + 3, repeating: 3); t.setEventHandler { [weak self] in self?.sup.scan() }; t.resume(); scanTimer = t
        }
        // launchd's SIGTERM (kickstart -k, bootout) stops only the host: the next one adopts the running plugins.
        // SIGINT takes the plugins down too.
        for sig in [SIGTERM, SIGINT] {
            signal(sig, SIG_IGN)
            let s = DispatchSource.makeSignalSource(signal: sig, queue: work)
            s.setEventHandler { [weak self] in
                if sig == SIGINT { self?.sup.stopAll(); work.asyncAfter(deadline: .now() + 1.5) { exit(0) } } else { exit(0) }
            }
            s.resume(); signalSources.append(s)
        }
    }
}
let core = Core()

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    var perms: Permissions { core.perms }
    var sup: Supervisor { core.sup }
    var item: NSStatusItem!
    var window: NSWindow?
    var rows: [Perm: (NSTextField, NSButton)] = [:]

    func applicationDidFinishLaunching(_ n: Notification) {
        hostLog("menu bar UI up")
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "PinPin"
        let menu = NSMenu(); menu.delegate = self; item.menu = menu
        perms.probeLocalNetwork()
        Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.refresh() }
        if Perm.allCases.contains(where: { perms.granted($0) != true }) { showChecklist() }
    }

    func showChecklist() {
        if let w = window { w.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true); return }
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 120 + 44 * Perm.allCases.count),
                         styleMask: [.titled, .closable], backing: .buffered, defer: false)
        w.title = "PinPin Host: permissions"; w.isReleasedWhenClosed = false
        let v = NSView(frame: w.contentRect(forFrameRect: w.frame))
        let intro = NSTextField(wrappingLabelWithString: "PinPin runs every tool through this one app. Grant each permission once; rebuilt and new tools inherit them, so you won't be asked again. Some take effect after the app restarts (it restarts itself).")
        intro.frame = NSRect(x: 20, y: v.bounds.height - 80, width: 480, height: 60); v.addSubview(intro)
        for (i, p) in Perm.allCases.enumerated() {
            let y = v.bounds.height - 120 - CGFloat(i) * 44
            let label = NSTextField(labelWithString: "")
            label.frame = NSRect(x: 20, y: y, width: 330, height: 24); v.addSubview(label)
            let b = NSButton(title: "Grant", target: self, action: #selector(grant(_:)))
            b.tag = i; b.bezelStyle = .rounded; b.frame = NSRect(x: 380, y: y - 4, width: 120, height: 32); v.addSubview(b)
            rows[p] = (label, b)
        }
        w.contentView = v; w.center(); window = w
        refresh(); w.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
    }

    @objc func grant(_ b: NSButton) { perms.request(Perm.allCases[b.tag]) }

    func mark(_ g: Bool?) -> String { g == true ? "✅" : g == false ? "⬜️" : "❔" }

    func refresh() {
        for (p, (label, button)) in rows {
            let g = perms.granted(p)
            label.stringValue = "\(mark(g))  \(p.title)"
            button.isEnabled = g != true; button.title = g == true ? "Granted" : "Grant"
        }
        let missing = Perm.allCases.filter { perms.granted($0) == false }.count
        item.button?.title = missing == 0 ? "PinPin" : "PinPin ⚠︎\(missing)"
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        menu.addItem(withTitle: "Permissions", action: nil, keyEquivalent: "")
        for p in Perm.allCases {
            let g = perms.granted(p)
            let mi = NSMenuItem(title: "\(mark(g))  \(p.title)", action: g == true ? nil : #selector(menuGrant(_:)), keyEquivalent: "")
            mi.target = self; mi.representedObject = p.rawValue; menu.addItem(mi)
        }
        menu.addItem(withTitle: "Show permission checklist…", action: #selector(openChecklist), keyEquivalent: "").target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Plugins", action: nil, keyEquivalent: "")
        let lines: [String] = work.sync {
            sup.plugins.values.sorted(by: { $0.name < $1.name }).map { p in
                let state = p.running ? "● running (pid \(p.pid))" : p.held ? "○ stopped" : "○ waiting"
                return "\(p.name)  \(state)  restarts \(p.restarts)"
            }
        }
        for l in lines { menu.addItem(withTitle: l, action: nil, keyEquivalent: "") }
        if lines.isEmpty { menu.addItem(withTitle: "(none)", action: nil, keyEquivalent: "") }
        menu.addItem(.separator())
        menu.addItem(withTitle: "Open plugins folder", action: #selector(openPlugins), keyEquivalent: "").target = self
        menu.addItem(withTitle: "Quit (restarts by itself)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    }

    @objc func menuGrant(_ mi: NSMenuItem) { if let r = mi.representedObject as? String, let p = Perm(rawValue: r) { perms.request(p) } }
    @objc func openChecklist() { showChecklist() }
    @objc func openPlugins() { NSWorkspace.shared.open(URL(fileURLWithPath: pluginDir)) }
}

core.start()  // plugins first; the UI below may block on WindowServer without affecting them
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
