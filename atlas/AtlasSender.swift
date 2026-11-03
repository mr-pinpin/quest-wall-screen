// Atlas sender for the headset's extra screens (docs/multi-screen-atlas.md).
// Captures each extra screen's macOS display with ScreenCaptureKit, packs them side by side into one
// "atlas" frame, encodes it with the hardware H.264 encoder and serves it to the headset over TCP, using
// MacScreenSender's packet format: u32 length (big endian), then [flags][pts u64][w u16][h u16][3 codec
// bytes][Annex B]. Packets starting with 0x80 carry text; the layout goes out that way before every
// keyframe: {"atlas":[W,H],"cells":[{"screen":id,"x":..,"y":..,"w":..,"h":..}]} (pixels, origin top left).
// Flag vt (docs/virtual-texturing.md, protocol v2): with fresh v2 demand, cells also carry "lvl" and "src", a screen
// may span several tiles, hidden screens shrink to mip-tail thumbnails and the layout carries "gen".
// Displays come from the virtual-display helper (GET /displays) or, for testing, --test "2:main,3:main".
// Started through Terminal (start-atlas.command) so it inherits Terminal's Screen Recording grant.
import CoreMedia
import CoreVideo
import Foundation
import IOKit.pwr_mgt
import Network
import ScreenCaptureKit
import VideoToolbox

func log(_ s: String) { FileHandle.standardError.write(("pinpin-atlas: " + s + "\n").data(using: .utf8)!) }

struct Options {
    var port: UInt16 = 57720, demandPort: UInt16 = 57721, fps = 60, bitrate = 40_000_000, cellW = 1920, cellH = 1200, maxW = 3840, maxH = 2160
    var url = "http://127.0.0.1:57710/displays", test = ""
    // Feature flags (--flags "bpcut,..."; off by default, each kept only if the bench proves it):
    //   bpcut  lower the encoder bitrate while the network backs up, recover it gradually
    //   vt     virtual texturing (protocol v2): levels, tiles past the 1920 cell cap, mip-tail thumbnails
    //   nowake  turn OFF the display wake (on by default, a fix): while a headset is connected the sender holds a
    //          PreventUserIdleDisplaySleep assertion, and declares user activity on connect to wake a sleeping display.
    //          With the Air's display asleep, WindowServer stops driving the virtual displays and they drop out of
    //          ScreenCaptureKit ("display not found", 0 fps; work-c4, 2026-11-03 14:37).
    //   capfast  capture interval half a refresh (1/120 s at 60 fps) so frames landing a hair early aren't skipped
    //   nodelay  TCP_NODELAY on the headset connections (Nagle held each frame's last partial segment for an ACK)
    //   pend1  encode only when nothing is still in flight to any peer (default: up to 1 frame queued behind the one
    //          sending), so the next frame always carries the newest content and stale frames never queue
    //   pace   send each frame in 16 KB chunks spread at 1.5x the stream's bitrate, instead of one burst, through one
    //          ordered queue per peer (bursts on a contended Wi-Fi channel cost retries and head-of-line delay)
    //   longgop  periodic keyframes every 10 s instead of 2 s (forced ones unchanged)
    //   gamecap  while a game stream plays ("game":1), the atlas is laid out within 1920x1200 (screens shrink)
    //   gameyield  while the headset's demand says a game stream is playing ("game":1), the atlas gives the laptop's
    //          one media engine to Sunshine's game encode: it encodes on 1 tick in N (N = 2, rising to 4 while its
    //          own encode time says the engine is contended), with one flush repeat instead of three
    //   damage  a frame identical to the screen's previous one is not copied (no re-encode), and a frame's copy
    //          covers only the rows that differ (compared row by row; SCK's dirty rects cover everything)
    //   prifps  per-screen frame-rate by demand priority: pri >= 0.8 full rate, >= 0.3 15 fps, else 10 fps; when no
    //          full-rate screen changed, the atlas encodes at most 15 times a second
    // --synth "S:WxH,..." (or --test "S:synth:WxH"): synthetic screens instead of macOS displays (source type
    // "synth"); --synth-animate "2,3": the ones that change every tick (default all), the rest hold still pictures
    var synth: [(Int, Int, Int)] = [], synthAnimate: Set<Int>? = nil  // nil: all animate; "none": none
    var flags = Set<String>(), statSecs = 30.0, vtMaxW = 0  // --vt-max-w: cap every vt screen's width (tests)
    init() {
        var it = CommandLine.arguments.dropFirst().makeIterator()
        while let a = it.next() {
            guard let v = it.next() else { break }
            switch a {
            case "--port": port = UInt16(v) ?? port
            case "--demand-port": demandPort = UInt16(v) ?? demandPort
            case "--fps": fps = Int(v) ?? fps
            case "--bitrate": bitrate = Int(v) ?? bitrate
            case "--cell": let p = v.split(separator: "x").compactMap { Int($0) }; if p.count == 2 { cellW = p[0]; cellH = p[1] }
            case "--url": url = v
            case "--test": test = v
            case "--stat-secs": statSecs = Double(v) ?? statSecs
            case "--vt-max-w": vtMaxW = Int(v) ?? 0
            case "--synth":  // "2:1920x1200,3:1920x1200,..."
                synth = v.split(separator: ",").compactMap { item in
                    let p = item.split(separator: ":"); guard p.count == 2, let s = Int(p[0]) else { return nil }
                    let wh = p[1].split(separator: "x").compactMap { Int($0) }; return wh.count == 2 ? (s, wh[0], wh[1]) : nil
                }
            case "--synth-animate": synthAnimate = v == "none" ? [] : Set(v.split(separator: ",").compactMap { Int($0) })
            case "--flags": flags = Set(v.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty })
            default: break
            }
        }
    }
}

struct Entry: Equatable { let screen: Int; let display: CGDirectDisplayID; let w: Int; let h: Int }
// A cell shows the part of its screen's capture frame (fw x fh, 0 = the cell's own size) starting at sx, sy.
struct Cell {
    let entry: Entry; let x: Int, y: Int, w: Int, h: Int; var sx = 0, sy = 0, fw = 0, fh = 0
    var frameW: Int { fw > 0 ? fw : w }
    var frameH: Int { fh > 0 ? fh : h }
}

nonisolated(unsafe) var encodeErrors = 0
private let compressionOutput: VTCompressionOutputCallback = { context, refcon, status, _, sample in
    if status != noErr { encodeErrors += 1; if encodeErrors & (encodeErrors - 1) == 0 { log("encode failed: \(status) (\(encodeErrors) so far)") } }
    guard status == noErr, let context = context, let sample = sample else { return }
    Unmanaged<Atlas>.fromOpaque(context).takeUnretainedValue().encoded(sample, repeated: refcon != nil)
}

final class Peer {
    let c: NWConnection; var pending = 0; var needsKey = true
    var chunks: [(data: Data, last: Bool)] = [], pumping = false  // pace: this peer's ordered send queue (net queue)
    init(_ c: NWConnection) { self.c = c }
}

/// Captures one display into its cells of the atlas buffer (one cell, or several tiles under vt). A capture lives as
/// long as its display stays put: layout changes only move its cells (`cells`, guarded by atlas.lock) and resize its
/// output with updateConfiguration, because restarting every stream on each resize froze the atlas for seconds in
/// the 10-screen stress test.
class CellCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    var cells: [Cell]; let screen: Int; let origin: CGPoint; weak var atlas: Atlas?; var stream: SCStream?; var dead = false
    var latest: CVPixelBuffer?  // under atlas.lock: the last frame, re-copied when the cells move without resizing
    var fps = 60                 // capture rate (prifps changes it with updateConfiguration)
    var frames = 0, clean = 0    // under atlas.lock: frames delivered, and those with no dirty rects (damage)
    init(_ cells: [Cell], _ atlas: Atlas) { self.cells = cells; screen = cells[0].entry.screen; self.atlas = atlas; origin = CGDisplayBounds(cells[0].entry.display).origin }
    static var rateFactor = 1  // 2 with --flags capfast
    func config(_ w: Int, _ h: Int, fps: Int) -> SCStreamConfiguration {
        let c = SCStreamConfiguration()
        c.width = w; c.height = h; c.pixelFormat = kCVPixelFormatType_32BGRA; c.showsCursor = true
        // capfast: ask for twice the rate. At exactly one refresh (1/60 s) a frame arriving a hair early is skipped,
        // which may be why every display delivered ~29.4 fps "@60" (2026-11-03).
        c.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps * CellCapture.rateFactor)); c.queueDepth = 5
        return c
    }
    func start(_ display: SCDisplay, fps: Int, queue: DispatchQueue) async throws {
        self.fps = fps
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: config(cells[0].frameW, cells[0].frameH, fps: fps), delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture(); stream = s
    }
    func resize(_ w: Int, _ h: Int, fps: Int) async {
        self.fps = fps
        do { try await stream?.updateConfiguration(config(w, h, fps: fps)) } catch { log("resize screen \(screen): \(error.localizedDescription)"); dead = true }
    }
    func stop() { atlas = nil; if let s = stream { stream = nil; Task { try? await s.stopCapture() } } }  // late frames go nowhere
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("screen \(screen) capture stopped: \(error.localizedDescription)"); dead = true; atlas?.layoutKey = ""
    }
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        deliver(pb)
    }
    // One new frame of this screen, from any source, into its cells (or the stash while hidden).
    func deliver(_ pb: CVPixelBuffer) {
        guard let a = atlas else { return }
        // damage: the rows this frame changed, in frame pixels; nil = all of it. ScreenCaptureKit marks virtual
        // displays dirty on every frame (0% clean measured), so compare with the previous frame instead: exact, and
        // about the cost of the copy it saves.
        var rows: Range<Int>? = nil
        if a.o.flags.contains("damage"), let prev = a.lock.withLock({ latest }), prev !== pb {
            switch Atlas.changedRows(prev, pb) {
            case .some(let r) where r.isEmpty: a.lock.withLock { frames += 1; clean += 1; latest = pb }; return  // identical
            case .some(let r): rows = r
            case .none: break  // size changed: copy all
            }
        }
        let w0 = DispatchTime.now().uptimeNanoseconds
        a.lock.lock(); defer { a.lock.unlock() }
        let b0 = DispatchTime.now().uptimeNanoseconds
        defer { a.prof.blits += 1; a.prof.blitWaitNs += b0 - w0; a.prof.blitNs += DispatchTime.now().uptimeNanoseconds - b0 }
        latest = pb; frames += 1
        if a.visible(screen) {
            a.stash[screen] = nil; for c in cells { a.blit(pb, c, rows: rows) }
            if a.o.flags.contains("prifps") && fps >= a.o.fps { a.fullRateDirty = true }
        } else { a.stash[screen] = pb }
    }
}

/// A synthetic screen (source type "synth"): moving pictures generated in-process at the capture rate, with no
/// ScreenCaptureKit and no WindowServer, so benchmarks measure our own pipeline (packing, VT, encode, send) rather than
/// macOS screen recording (which delivered ~8.6 fps per display with 10 virtual displays, 2026-11-03). Frames cycle
/// through 8 precomputed IOSurface-backed pictures that differ everywhere (like video or a game), so generating them
/// costs almost nothing.
final class SynthSource: CellCapture {
    var ring: [CVPixelBuffer] = [], k = 0, timer: DispatchSourceTimer?, queue: DispatchQueue?
    static func isSynth(_ d: CGDirectDisplayID) -> Bool { d & 0xF000_0000 == 0xF000_0000 }
    static func display(_ screen: Int) -> CGDirectDisplayID { 0xF000_0000 | CGDirectDisplayID(screen & 0xFFFF) }
    var animated = true  // false: one still picture, delivered at start and after each resize (static content)
    func start(fps: Int, queue: DispatchQueue, animated: Bool) {
        self.queue = queue; self.fps = fps; self.animated = animated
        queue.sync { ring = SynthSource.pictures(cells[0].frameW, cells[0].frameH, seed: screen) }
        schedule()
    }
    func schedule() {
        timer?.cancel(); timer = nil
        guard animated else { queue?.async { [weak self] in if let s = self, let pb = s.ring.first { s.deliver(pb) } }; return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .nanoseconds(1_000_000_000 / max(1, fps)))
        t.setEventHandler { [weak self] in guard let self = self, !self.ring.isEmpty else { return }; self.k += 1; self.deliver(self.ring[self.k % self.ring.count]) }
        t.resume(); timer = t
    }
    override func resize(_ w: Int, _ h: Int, fps: Int) async {
        let rateChanged = fps != self.fps; self.fps = fps
        queue?.sync { if ring.first.map({ CVPixelBufferGetWidth($0) != w || CVPixelBufferGetHeight($0) != h }) ?? true { ring = SynthSource.pictures(w, h, seed: screen) } }
        if rateChanged || !animated { schedule() }
    }
    override func stop() { atlas = nil; timer?.cancel(); timer = nil }
    static func pictures(_ w: Int, _ h: Int, seed: Int) -> [CVPixelBuffer] {
        let attrs: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        return (0..<8).compactMap { k in
            var pb: CVPixelBuffer?
            guard CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb) == kCVReturnSuccess, let b = pb else { return nil }
            CVPixelBufferLockBaseAddress(b, []); defer { CVPixelBufferUnlockBaseAddress(b, []) }
            guard let base = CVPixelBufferGetBaseAddress(b)?.assumingMemoryBound(to: UInt32.self) else { return nil }
            let stride = CVPixelBufferGetBytesPerRow(b) / 4, s = UInt32(truncatingIfNeeded: seed * 977 + k * 61)
            var r = UInt32(truncatingIfNeeded: seed &* 2654435761) &+ UInt32(k)
            for y in 0..<h {  // a scrolled gradient (shifts every frame) ...
                let row = base + y * stride, v = UInt32(truncatingIfNeeded: y) &+ s
                for x in 0..<w { row[x] = 0xFF000000 | ((v &+ UInt32(truncatingIfNeeded: x)) & 0xFF) << 16 | (v & 0xFF) << 8 | ((UInt32(truncatingIfNeeded: x) &* 3 &+ s) & 0xFF) }
            }
            let bw = max(1, w / 5), bh = max(1, h / 4), bx = (k * w / 9) % max(1, w - bw), by = (k * h / 11) % max(1, h - bh)
            for y in by..<by + bh { for x in bx..<bx + bw { r = r &* 1664525 &+ 1013904223; base[y * stride + x] = 0xFF000000 | (r >> 8) } }  // ... and a moving noise block
            return b
        }
    }
}

final class Atlas {
    let o: Options
    let lock = NSLock()
    var W = 0, H = 0, pixels: [UInt8] = [], dirty = false
    var cells: [Cell] = [], captures: [CGDirectDisplayID: CellCapture] = [:], lastRefit = Date.distantPast
    var layoutKey = "", layoutJSON = ""
    var encoder: VTCompressionSession?
    var peers: [UUID: Peer] = [:]
    var forceKey = true, codecBytes: [UInt8] = [0x64, 0, 0x33], frame: Int64 = 0, flushes = 0
    var lastBuffer: CVPixelBuffer?
    var stash: [Int: CVPixelBuffer] = [:]  // under lock: the latest frame of each screen the headset can't see

    // ---- demand from the headset (docs/PERFORMANCE.md): UDP datagrams at 5 Hz,
    // {"budgetKbps":60000,"screens":[{"id":1,"vis":1,"px":1400,"pri":1.0},...]}; id 1 is the main panel.
    // Unseen screens cost nothing (their frames wait in `stash`), the bitrate is the extras' share of the budget
    // by priority, and each cell shrinks to the smallest width tier covering its px. Stale demand (> 2 s, e.g. no
    // headset) means: everything visible, full size, full bitrate.
    struct Want { var vis = true, px = 1920, pri = 1.0, lvl: Int? = nil, have = -1 }
    let dlock = NSLock()
    var busy = false, busyTicks = 0, heldTicks = 0
    // Capture only what is seen: a screen hidden for 2 s loses its SCStream (its cell keeps the last frame) and gets
    // one again when it comes into view. 30 screens x 2 senders of always-on streams crashed replayd.
    var hiddenAt: [Int: Date] = [:], wake = false, refreshSince: Date? = nil
    var want: [Int: Want] = [:], wantAt = Date.distantPast, budgetKbps = 0, appliedKbps = 0
    // bpcut flag (work queue): bitrate scale under backpressure. x0.7 at most every 250 ms while held back,
    // x1.2 per calm second back to 1.
    var bpScale = 1.0, bpCutAt = Date.distantPast, bpRiseAt = Date.distantPast
    var statAt = Date()  // refresh: last capture statistics line
    var lastVT = false   // refresh: the previous layout was vt
    // Profile, logged with the capture statistics: how ticks end (work queue), and the time spent copying under `lock`
    // by the tick (whole atlas -> encoder buffer) and by capture callbacks (frame -> atlas), plus time waiting for it.
    struct Prof { var ticks = 0, held = 0, priHeld = 0, yielded = 0, idle = 0, fresh = 0, repeats = 0
                  var copyNs: UInt64 = 0, copyMax: UInt64 = 0, tickWaitNs: UInt64 = 0, blits = 0, blitNs: UInt64 = 0, blitWaitNs: UInt64 = 0 }
    var prof = Prof()  // tick fields on the work queue; blit fields under lock; read and reset under lock
    var fullRateDirty = false, lastEncodeAt = Date.distantPast  // prifps: a full-rate screen changed (under lock); work queue
    var gameOn = false, ticks = 0, yieldN = 2, yieldAt = Date()  // gameyield: demand "game" (dlock); tick state (work queue)
    var encEwma = 0.0  // under elock: smoothed encode ms, the contention signal for gameyield
    // Encode time (submit -> output callback), logged every 30 s: the atlas's share of the media engine.
    let elock = NSLock(); var submitAt: [Int64: UInt64] = [:], encMs: [Double] = [], encLogAt = Date()
    // vt: the last demand was protocol v2; each screen's display width (levels are octaves of it).
    var wantV2 = false, srcW: [Int: Int] = [:], gen = 0, layoutCore = ""
    // vt, under lock: last frame of a paused capture, for its mip tail and, scaled up, as the placeholder in its new
    // cells after a reveal until the restarted capture's first frame lands (no black or foreign pixels meanwhile).
    var resident: [CGDirectDisplayID: CVPixelBuffer] = [:]
    var tier: [Int: Int] = [:], tierWant: [Int: (w: Int, since: Date)] = [:]
    static let tiers = [480, 640, 960, 1280, 1920]
    let net = DispatchQueue(label: "atlas.net"), capQ = DispatchQueue(label: "atlas.capture"), work = DispatchQueue(label: "atlas.work")
    init(_ o: Options) { self.o = o }

    // damage: the range of rows where two same-sized frames differ (empty if identical), nil if they can't be compared.
    static func changedRows(_ a: CVPixelBuffer, _ b: CVPixelBuffer) -> Range<Int>? {
        let w = CVPixelBufferGetWidth(a), h = CVPixelBufferGetHeight(a)
        guard w == CVPixelBufferGetWidth(b), h == CVPixelBufferGetHeight(b) else { return nil }
        CVPixelBufferLockBaseAddress(a, .readOnly); CVPixelBufferLockBaseAddress(b, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(a, .readOnly); CVPixelBufferUnlockBaseAddress(b, .readOnly) }
        guard let pa = CVPixelBufferGetBaseAddress(a), let pb = CVPixelBufferGetBaseAddress(b) else { return nil }
        let sa = CVPixelBufferGetBytesPerRow(a), sb = CVPixelBufferGetBytesPerRow(b), n = w * 4
        var top = 0
        while top < h && memcmp(pa + top * sa, pb + top * sb, n) == 0 { top += 1 }
        if top == h { return 0..<0 }
        var bottom = h - 1
        while bottom > top && memcmp(pa + bottom * sa, pb + bottom * sb, n) == 0 { bottom -= 1 }
        return top..<(bottom + 1)
    }
    // damage: a frame's changed rows as rows of the tile starting at frame row `sy`, `h` rows tall; nil if none.
    static func tileRows(_ rows: Range<Int>?, sy: Int, h: Int) -> Range<Int>? {
        guard let r = rows else { return h > 0 ? 0..<h : nil }
        let lo = max(0, r.lowerBound - sy), hi = min(h, r.upperBound - sy)
        return lo < hi ? lo..<hi : nil
    }
    // Copies one captured frame (its sx, sy part) into a cell; caller holds `lock`.
    func blit(_ pb: CVPixelBuffer, _ cell: Cell, rows: Range<Int>? = nil) {
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self), pixels.count == W * H * 4 else { return }
        let pw = CVPixelBufferGetWidth(pb), ph = CVPixelBufferGetHeight(pb)
        if cell.fw > 0 && (pw != cell.fw || ph != cell.fh) { return }  // a tile needs its exact frame size (mid-resize)
        let stride = CVPixelBufferGetBytesPerRow(pb), w = min(cell.w, pw - cell.sx), h = min(cell.h, ph - cell.sy)
        guard w > 0, h > 0, cell.x >= 0, cell.y >= 0, cell.x + w <= W, cell.y + h <= H else { return }  // a stale cell from before a relayout
        // damage: only the frame rows that changed (frame row = cell.sy + y); none in this tile: nothing to copy.
        // (Clamp before building the Range: rows entirely above or below a tile made lower > upper, a trap.)
        guard let ys = Atlas.tileRows(rows, sy: cell.sy, h: h) else { return }
        pixels.withUnsafeMutableBufferPointer { dst in
            for y in ys { memcpy(dst.baseAddress! + ((cell.y + y) * W + cell.x) * 4, base + (cell.sy + y) * stride + cell.sx * 4, w * 4) }
        }
        dirty = true
    }
    // vt: a whole frame scaled down into a mip-tail cell (box filter over each cell pixel's footprint); holds `lock`.
    func blitScaled(_ pb: CVPixelBuffer, _ cell: Cell) {
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self), pixels.count == W * H * 4,
              cell.x >= 0, cell.y >= 0, cell.x + cell.w <= W, cell.y + cell.h <= H else { return }
        let pw = CVPixelBufferGetWidth(pb), ph = CVPixelBufferGetHeight(pb), stride = CVPixelBufferGetBytesPerRow(pb)
        let fx = max(1, pw / cell.w), fy = max(1, ph / cell.h)
        pixels.withUnsafeMutableBufferPointer { dst in
            for y in 0..<cell.h {
                let y0 = y * ph / cell.h
                for x in 0..<cell.w {
                    let x0 = x * pw / cell.w; var acc = (0, 0, 0, 0)
                    for yy in 0..<fy { let row = base + min(ph - 1, y0 + yy) * stride
                        for xx in 0..<fx { let q = row + min(pw - 1, x0 + xx) * 4; acc.0 += Int(q[0]); acc.1 += Int(q[1]); acc.2 += Int(q[2]); acc.3 += Int(q[3]) } }
                    let n = fx * fy, d = dst.baseAddress! + ((cell.y + y) * W + cell.x + x) * 4
                    d[0] = UInt8(acc.0 / n); d[1] = UInt8(acc.1 / n); d[2] = UInt8(acc.2 / n); d[3] = UInt8(acc.3 / n)
                }
            }
        }
        dirty = true
    }
    func visible(_ screen: Int) -> Bool {
        dlock.lock(); defer { dlock.unlock() }
        return Date().timeIntervalSince(wantAt) > 2 || (want[screen]?.vis ?? true)
    }
    func listenDemand() {
        do {
            let l = try NWListener(using: .udp, on: NWEndpoint.Port(rawValue: o.demandPort)!)
            l.newConnectionHandler = { [weak self] c in guard let self = self else { return }; c.start(queue: self.net); self.receive(c) }
            l.stateUpdateHandler = { if case .failed(let e) = $0 { log("demand listen failed: \(e)"); exit(1) } }
            l.start(queue: net); log("demand on udp \(o.demandPort)")
        } catch { log("demand listen failed: \(error)") }
    }
    func receive(_ c: NWConnection) {
        c.receiveMessage { [weak self] data, _, _, error in
            if let d = data { self?.demand(d) }
            if error == nil { self?.receive(c) }
        }
    }
    func demand(_ data: Data) {
        guard let j = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let list = j["screens"] as? [[String: Any]] else { return }
        func num(_ v: Any?) -> Double? { (v as? NSNumber)?.doubleValue }
        var next: [Int: Want] = [:]
        for e in list {
            guard let id = num(e["id"]) else { continue }
            next[Int(id)] = Want(vis: (num(e["vis"]) ?? 1) != 0, px: Int(num(e["px"]) ?? 1920), pri: num(e["pri"]) ?? 1,
                                 lvl: num(e["lvl"]).map { max(0, Int($0)) }, have: Int(num(e["have"]) ?? -1))
        }
        let now = Date(); var reveal: [Int] = []
        let vt = o.flags.contains("vt") && num(j["v"]) == 2
        dlock.withLock {
            if now.timeIntervalSince(wantAt) > 10 { log("demand from headset") }
            let stale = now.timeIntervalSince(wantAt) > 2
            if vt != wantV2 { tier = [:]; tierWant = [:]; wake = true }  // v1 <-> v2 (VT toggled on the headset): sizes start over, now
            for (id, w) in next where id != 1 {
                if w.vis && !stale && !(want[id]?.vis ?? true) { reveal.append(id) }
                if w.vis { hiddenAt[id] = nil } else if hiddenAt[id] == nil { hiddenAt[id] = now }
                guard w.vis else { continue }  // unseen screens keep their size
                // v1: the smallest width tier covering px. vt: the wanted level of the display's width (or, without
                // lvl, the smallest octave covering px), no 1920 cap: wide screens become several tiles.
                var t = min(Atlas.tiers.first(where: { $0 >= w.px }) ?? o.cellW, o.cellW)
                if vt {
                    let full = srcW[id] ?? o.cellW
                    if let l = w.lvl { t = max(16, full >> min(l, 8)) } else { var l = 0; while l < 6 && (full >> (l + 1)) >= w.px { l += 1 }; t = full >> l }
                }
                let cur = tier[id] ?? o.cellW
                if t == cur { tierWant[id] = nil } else if tierWant[id]?.w != t { tierWant[id] = (t, now) }
            }
            want = next; wantAt = now; wantV2 = vt; budgetKbps = Int(num(j["budgetKbps"]) ?? 0)
            let game = (num(j["game"]) ?? 0) != 0
            if game != gameOn && o.flags.contains("gameyield") { log(game ? "game stream on: atlas yields the encoder" : "game stream off") }
            if game != gameOn && o.flags.contains("gamecap") { wake = true }  // relayout now, not at the next 2 s pass
            gameOn = game
        }
        if !reveal.isEmpty {
            dlock.withLock { wake = true }  // start its capture now, not at the next 2 s pass
            lock.withLock {
                for id in reveal { if let pb = stash.removeValue(forKey: id) { for c in cells where c.entry.screen == id { blit(pb, c) } } }
            }
        }
        work.async { self.applyBudget() }
    }
    // The extras' share of the headset's budget, by gaze priority; on the work queue.
    func applyBudget(force: Bool = false) {
        guard let e = encoder else { return }
        var kbps = o.bitrate / 1000
        dlock.withLock {
            guard budgetKbps > 0, Date().timeIntervalSince(wantAt) <= 2 else { return }
            let all = want.values.reduce(0.0) { $0 + ($1.vis ? $1.pri : 0) }
            let extras = want.reduce(0.0) { $0 + ($1.key != 1 && $1.value.vis ? $1.value.pri : 0) }
            kbps = all > 0 ? Int(Double(budgetKbps) * extras / all) : 0
        }
        kbps = min(o.bitrate / 1000, max(4000, kbps))
        if bpScale < 1 { kbps = max(1500, Int(Double(kbps) * bpScale)) }
        if appliedKbps == 0 || (force && kbps != appliedKbps) || abs(kbps - appliedKbps) * 100 > appliedKbps * 15 {
            appliedKbps = kbps
            VTSessionSetProperty(e, key: kVTCompressionPropertyKey_AverageBitRate, value: (kbps * 1000) as CFNumber)
            let bps = Double(kbps) * 1000 * 1.5; net.async { self.paceBps = max(4_000_000, bps) }
            log("bitrate \(kbps) kbps" + (bpScale < 1 ? String(format: " (backpressure x%.2f)", bpScale) : ""))
        }
    }

    // ---- display list
    func entries() -> [Entry] {
        if !o.synth.isEmpty { return o.synth.map { Entry(screen: $0.0, display: SynthSource.display($0.0), w: $0.1, h: $0.2) } }
        if !o.test.isEmpty {
            return o.test.split(separator: ",").compactMap { pair in
                // "screen:display" or "screen:display:WxH" (the size the displays helper reports)
                let p = pair.split(separator: ":"); guard p.count >= 2, let s = Int(p[0]) else { return nil }
                let d = p[1] == "main" ? CGMainDisplayID() : p[1] == "synth" ? SynthSource.display(s) : CGDirectDisplayID(p[1]) ?? 0
                let mode = CGDisplayCopyDisplayMode(d), size = p.count > 2 ? p[2].split(separator: "x").compactMap { Int($0) } : []
                if size.count == 2 { return Entry(screen: s, display: d, w: size[0], h: size[1]) }
                return Entry(screen: s, display: d, w: mode?.pixelWidth ?? 1920, h: mode?.pixelHeight ?? 1200)
            }
        }
        guard let url = URL(string: o.url), let data = try? Data(contentsOf: url),
              let list = (try? JSONSerialization.jsonObject(with: data)) as? [[String: Any]] else { return [] }
        return list.compactMap { j in
            guard let s = j["screen"] as? Int, let d = j["display"] as? Int else { return nil }
            return Entry(screen: s, display: CGDirectDisplayID(d), w: j["w"] as? Int ?? 1920, h: j["h"] as? Int ?? 1200)
        }
    }

    // Row packing: each display fits its tier's box (cellW x cellH by default) keeping its aspect; rows wrap at maxW.
    // When the screens don't all fit maxW x maxH, every cell shrinks by the same factor until they do, so a screen
    // is never dropped (ten 1920-wide screens become ten ~760-wide cells).
    func layout(_ list: [Entry], _ tiers: [Int: Int], cap: Bool = false) -> (Int, Int, [Cell]) {
        let maxW = cap ? 1920 : o.maxW, maxH = cap ? 1200 : o.maxH
        var f = 1.0
        while true {
            var cells: [Cell] = [], x = 0, y = 0, rowH = 0, W = 0, fits = true
            for e in list {
                let cw = Double(min(tiers[e.screen] ?? o.cellW, o.cellW)) * f, ch = cw * Double(o.cellH) / Double(o.cellW)
                let s = min(cw / Double(e.w), ch / Double(e.h), 1.0)
                let w = max(16, Int(Double(e.w) * s) / 2 * 2), h = max(16, Int(Double(e.h) * s) / 2 * 2)
                if x + w > maxW { x = 0; y += rowH; rowH = 0 }
                if y + h > maxH { fits = false; break }
                cells.append(Cell(entry: e, x: x, y: y, w: w, h: h)); x += w; rowH = max(rowH, h); W = max(W, x)
            }
            if fits || f < 0.05 {
                if f < 1 { log("atlas: \(list.count) screens fit at \(Int(f * 100))% size") }
                return (max(16, (W + 15) / 16 * 16), max(16, (y + rowH + 15) / 16 * 16), cells)
            }
            f *= 0.9
        }
    }

    // vt layout: each screen at its level's width (no 1920 cap), cut into tiles of at most cellW x cellH, packed
    // skyline bottom-left (tallest first) at the atlas width that gives the smallest area, within 3840x2304: H.264
    // level 5.1, which the Quest decodes, capped by the Mac's hardware encoder (4096x2304; at 3840x2400 it produced no
    // frames at all, 2026-11-03). The encoder pays for the whole atlas, empty space included: row shelves across 3840
    // left cinema-10 at 3840x1360 where 2400x1200 holds the same cells. Paused screens are mip tails (<= 240 px wide);
    // a hidden screen still capturing keeps its current size, or starts as a tail. Too big: the screen with the most
    // area per unit of priority shrinks 10%, repeat; a single cinema screen at level 0 keeps the whole atlas while the
    // rest are tails.
    func layoutVT(_ list: [Entry], _ tiers: [Int: Int], _ live: Set<CGDirectDisplayID>, _ pri: [Int: Double], cap: Bool = false,
                  hidden: Set<Int> = [], current: [Int: Int] = [:]) -> (Int, Int, [Cell]) {
        let maxW = cap ? 1920 : 3840, maxH = cap ? 1152 : 2304
        func tail(_ e: Entry) -> Int { var w = e.w; while w > 240 { w /= 2 }; return max(16, w / 2 * 2) }
        func height(_ e: Entry, _ w: Int) -> Int { max(16, w * e.h / max(1, e.w) / 2 * 2) }
        var width: [Int: Int] = [:]
        for e in list {
            if !live.contains(e.display) { width[e.screen] = tail(e) }
            else if hidden.contains(e.screen) { width[e.screen] = current[e.screen] ?? tail(e) }
            else { width[e.screen] = max(tail(e), min(e.w, o.vtMaxW > 0 ? o.vtMaxW : e.w, tiers[e.screen] ?? min(e.w, o.cellW)) / 2 * 2) }
        }
        func pack() -> (Int, Int, [Cell])? {
            var tiles: [Cell] = []
            for e in list {
                let fw = width[e.screen]!, fh = height(e, fw)
                for ty in stride(from: 0, to: fh, by: o.cellH) {
                    for tx in stride(from: 0, to: fw, by: o.cellW) {
                        tiles.append(Cell(entry: e, x: 0, y: 0, w: min(o.cellW, fw - tx), h: min(o.cellH, fh - ty), sx: tx, sy: ty, fw: fw, fh: fh))
                    }
                }
            }
            tiles = tiles.enumerated().sorted { ($0.element.h, $0.element.w, -$0.offset) > ($1.element.h, $1.element.w, -$1.offset) }.map { $0.element }
            let minW = tiles.map { $0.w }.max() ?? 16
            var best: (area: Int, w: Int, h: Int, cells: [Cell])? = nil
            for aw in stride(from: (minW + 15) / 16 * 16, through: maxW, by: 16) {
                guard let (h, cells) = Atlas.skyline(tiles, width: aw, maxH: maxH) else { continue }
                let ah = max(16, (h + 15) / 16 * 16), area = aw * ah
                if best == nil || area < best!.area { best = (area, aw, ah, cells) }
            }
            return best.map { ($0.w, min(maxH, $0.h), $0.cells) }
        }
        while true {
            if let p = pack() { return p }
            func cost(_ e: Entry) -> Double { let w = width[e.screen]!; return Double(w * height(e, w)) / (pri[e.screen] ?? 1) }
            guard let e = list.filter({ width[$0.screen]! > tail($0) }).max(by: { cost($0) < cost($1) }) else {
                log("atlas: \(list.count) mip tails do not fit, using the v1 layout"); return layout(list, [:])
            }
            width[e.screen] = max(tail(e), Int(Double(width[e.screen]!) * 0.9) / 2 * 2)
        }
    }
    // Skyline bottom-left packing of tiles (already sorted) into a strip `width` wide: each tile goes where its top
    // edge is lowest, then leftmost. Returns the used height and the placed cells, or nil past maxH.
    static func skyline(_ tiles: [Cell], width: Int, maxH: Int) -> (Int, [Cell])? {
        var sky: [(x: Int, w: Int, y: Int)] = [(0, width, 0)], out: [Cell] = [], used = 0
        out.reserveCapacity(tiles.count)
        for t in tiles {
            var bx = -1, by = Int.max
            for i in sky.indices {
                let x = sky[i].x
                if x + t.w > width { break }
                var y = 0, j = i, covered = 0
                while covered < t.w { y = max(y, sky[j].y); covered += sky[j].w; j += 1 }
                if y < by { by = y; bx = x }
            }
            if bx < 0 || by + t.h > maxH { return nil }
            out.append(Cell(entry: t.entry, x: bx, y: by, w: t.w, h: t.h, sx: t.sx, sy: t.sy, fw: t.fw, fh: t.fh))
            used = max(used, by + t.h)
            // new skyline: the tile's top replaces whatever it covers
            var next: [(x: Int, w: Int, y: Int)] = []
            for s in sky {
                let e = s.x + s.w
                if e <= bx || s.x >= bx + t.w { next.append(s); continue }
                if s.x < bx { next.append((s.x, bx - s.x, s.y)) }
                if e > bx + t.w { next.append((bx + t.w, e - bx - t.w, s.y)) }
            }
            next.append((bx, t.w, by + t.h)); next.sort { $0.x < $1.x }
            sky = []
            for s in next { if let l = sky.last, l.y == s.y, l.x + l.w == s.x { sky[sky.count - 1].w += s.w } else { sky.append(s) } }
        }
        return (used, out)
    }

    func refresh() async {
        let list = entries()
        // Size tiers settle before they apply (growing after 2 s, shrinking after 5 s): each change costs a keyframe.
        let now = Date()
        let (tiers, vt, pri): ([Int: Int], Bool, [Int: Double]) = dlock.withLock {
            for e in list { srcW[e.screen] = e.w }
            if now.timeIntervalSince(wantAt) > 2 { tier = [:]; tierWant = [:] }  // no headset: back to full size
            let vt = o.flags.contains("vt") && wantV2 && now.timeIntervalSince(wantAt) <= 2
            // At most one size refit per 5 s (each is a keyframe); between refits only the bitrate follows gaze. Under vt
            // the headset already waits 3 s before asking for less, and more detail applies within a second.
            let canRefit = now.timeIntervalSince(lastRefit) >= 5, canGrow = now.timeIntervalSince(lastRefit) >= 1
            var refit = false
            for (id, t) in tierWant {
                let grow = t.w > (tier[id] ?? o.cellW)
                let due = vt ? (grow ? canGrow : canRefit) : canRefit && now.timeIntervalSince(t.since) > (grow ? 2 : 5)
                if due { tier[id] = t.w; tierWant[id] = nil; refit = true }
            }
            if refit { lastRefit = now }
            return (tier, vt, want.mapValues { $0.vis ? max(0.05, $0.pri) : 0.05 })
        }
        // Screens worth a capture stream: all of them without fresh demand, else the visible ones plus those
        // hidden for under 2 s.
        let capturing = Set(captures.filter { !$0.value.dead }.keys)
        let live: Set<CGDirectDisplayID> = dlock.withLock {
            let stale = now.timeIntervalSince(wantAt) > 2
            var l = Set(list.filter { e in stale || (want[e.screen]?.vis ?? true) || now.timeIntervalSince(hiddenAt[e.screen] ?? now) < 2 }.map { $0.display })
            // vt: a reveal goes live at once, but a screen becomes a mip tail only once hidden for 10 s, and demotions
            // wait for the 5 s refit gate and go together: the gaze sweeping a ring of screens relayouted every 2 s
            // pass (29 keyframes a minute in work-c4's replay of the ring-10 trace, bench/demand_replay.py; 14 with
            // this), and a revealed screen still sharpens at once.
            if vt {
                let demote = capturing.subtracting(l).intersection(list.map { $0.display })
                let ready = Set(list.filter { e in demote.contains(e.display) && now.timeIntervalSince(hiddenAt[e.screen] ?? now) >= 10 }.map { $0.display })
                l.formUnion(demote.subtracting(ready))
                if !ready.isEmpty {
                    if now.timeIntervalSince(lastRefit) < 5 { l.formUnion(ready) } else { lastRefit = now }
                }
            }
            return l
        }
        // prifps: each capture's rate follows its screen's priority (full rate without fresh demand)
        if o.flags.contains("prifps") {
            let fresh = dlock.withLock { now.timeIntervalSince(wantAt) <= 2 }
            for cap in captures.values where !cap.dead {
                let p = pri[cap.screen] ?? 1, f = !fresh || p >= 0.8 ? o.fps : p >= 0.3 ? 15 : 10
                if f != cap.fps { await cap.resize(cap.cells[0].frameW, cap.cells[0].frameH, fps: f) }
            }
        }
        // Capture statistics, every --stat-secs: delivered fps per screen and the share with no dirty rects.
        if now.timeIntervalSince(statAt) >= o.statSecs {
            let dt = now.timeIntervalSince(statAt)
            let parts: [String] = lock.withLock {
                captures.values.sorted { $0.screen < $1.screen }.map { c in
                    defer { c.frames = 0; c.clean = 0 }
                    return String(format: "%d %.1f fps%@ @%d", c.screen, Double(c.frames) / dt, c.clean > 0 ? String(format: " (%d%% clean)", 100 * c.clean / max(1, c.frames)) : "", c.fps)
                }
            }
            if !parts.isEmpty { log("capture: " + parts.joined(separator: ", ")) }
            let p: Prof = lock.withLock { defer { prof = Prof() }; return prof }
            if p.ticks > 0 {
                func ms(_ ns: UInt64, _ n: Int) -> Double { n > 0 ? Double(ns) / 1e6 / Double(n) : 0 }
                let enc = p.fresh + p.repeats
                log(String(format: "ticks %.0f/s: fresh %.1f/s, repeats %.1f/s, held (backpressure) %d, prifps %d, gameyield %d, nothing new %d; " +
                           "atlas copy %.1f ms avg / %.1f max, tick waits %.2f ms; frame copies %.1f/s at %.2f ms, waiting %.2f ms",
                           Double(p.ticks) / dt, Double(p.fresh) / dt, Double(p.repeats) / dt, p.held, p.priHeld, p.yielded,
                           p.ticks - p.held - p.priHeld - p.yielded - enc, ms(p.copyNs, p.fresh), Double(p.copyMax) / 1e6, ms(p.tickWaitNs, p.fresh),
                           Double(p.blits) / dt, ms(p.blitNs, p.blits), ms(p.blitWaitNs, p.blits)))
            }
            statAt = now
        }
        // Display positions are part of the key: adding a virtual display can make macOS slide the others along
        // (screen 3 at x=1440 pushed screen 2 to x=3360), and captures started mid-move showed only the wallpaper.
        var key = list.map { e -> String in let r = CGDisplayBounds(e.display); return "\(e.screen):\(e.display):\(e.w)x\(e.h)@\(Int(r.minX)),\(Int(r.minY))/\(tiers[e.screen] ?? o.cellW)" }.joined(separator: ",")
        // vt: paused screens shrink to mip tails and priorities share the atlas, so both are part of the layout.
        // gamecap: while a game stream plays, the whole atlas fits in 1920x1200 (the Air's one media engine can't
        // encode a 3456x2160 atlas next to a 2560x1600 game: the game encode fell to 40 fps, 277 ms, 2026-11-03)
        let capped = o.flags.contains("gamecap") && dlock.withLock { gameOn && now.timeIntervalSince(wantAt) <= 2 }
        if capped { key += "|gamecap" }
        if vt { key += "|vt|" + list.map { e in "\(e.screen)\(live.contains(e.display) ? "" : "t")@\(Int((pri[e.screen] ?? 1) * 20))" }.joined(separator: ",") }
        if key == layoutKey && !captures.values.contains(where: { $0.dead }) && Set(captures.keys) == live { return }
        let hiddenNow: Set<Int> = vt ? dlock.withLock { Set(want.filter { !$0.value.vis }.keys) } : []
        // On the first v2 layout after v1, the captures' sizes are v1's: hidden screens start as tails instead.
        let currentW = vt && !lastVT ? [:] : Dictionary(captures.values.map { ($0.screen, $0.cells[0].frameW) }, uniquingKeysWith: { a, _ in a })
        lastVT = vt
        let (w, h, cells) = vt ? layoutVT(list, tiers, live, pri, cap: capped, hidden: hiddenNow, current: currentW) : layout(list, tiers, cap: capped)
        let resized = w != W || h != H
        let cj = cells.map { c -> String in
            var j = "{\"screen\":\(c.entry.screen),\"x\":\(c.x),\"y\":\(c.y),\"w\":\(c.w),\"h\":\(c.h)"
            if vt {  // level and the part of the display this cell covers, in display pixels: tile edges map per axis
                // and the frame's far edges map to the display's, so a screen's src rects tile it without seams
                func edge(_ v: Int, _ frame: Int, _ disp: Int) -> Int { v >= frame ? disp : Int((Double(v) * Double(disp) / Double(frame)).rounded()) }
                let x0 = edge(c.sx, c.frameW, c.entry.w), x1 = edge(c.sx + c.w, c.frameW, c.entry.w)
                let y0 = edge(c.sy, c.frameH, c.entry.h), y1 = edge(c.sy + c.h, c.frameH, c.entry.h)
                let lvl = max(0, Int(log2(Double(c.entry.w) / Double(c.frameW)).rounded()))
                j += ",\"lvl\":\(lvl),\"src\":[\(x0),\(y0),\(x1 - x0),\(y1 - y0)]"
            }
            return j + "}"
        }.joined(separator: ",")
        let core = "\"atlas\":[\(w),\(h)],\"cells\":[\(cj)]"
        if vt && core != layoutCore { gen += 1 }
        let json = vt ? "{\"atlas\":[\(w),\(h)],\"gen\":\(gen),\"cells\":[\(cj)]}" : "{\(core)}"
        layoutCore = core
        var byDisplay: [CGDirectDisplayID: [Cell]] = [:]
        for c in cells { byDisplay[c.entry.display, default: []].append(c) }
        // Stop captures whose display is gone, died, or moved (a capture started mid-move shows only the wallpaper).
        for (d, c) in captures where c.dead || !live.contains(d) || !cells.contains(where: { $0.entry.display == d }) || CGDisplayBounds(d).origin != c.origin {
            if !live.contains(d) && !c.dead { log("screen \(c.screen) out of view: capture paused") }
            if vt { lock.withLock { if let pb = c.latest { resident[d] = pb } } }
            c.stop(); captures[d] = nil
        }
        var resizes: [(CellCapture, Cell)] = []
        lock.withLock {
            stash = [:]; self.cells = cells; dirty = true
            if resized { W = w; H = h; pixels = [UInt8](repeating: 0, count: w * h * 4) }
            for d in resident.keys where !vt || byDisplay[d] == nil || captures[d]?.latest != nil { resident[d] = nil }
            for (d, cs) in byDisplay {
                // mip tail, or the placeholder of a screen whose capture has not delivered yet
                if vt, captures[d]?.latest == nil, let pb = resident[d] { for c in cs { blitScaled(pb, c) } }
                guard let cap = captures[d] else { continue }
                let old = cap.cells[0]; cap.cells = cs
                if old.frameW != cs[0].frameW || old.frameH != cs[0].frameH { resizes.append((cap, cs[0])) }
                else if let pb = cap.latest { for c in cs { blit(pb, c) } }  // same size, new place: copy the last frame now
            }
        }
        for (cap, c) in resizes { await cap.resize(c.frameW, c.frameH, fps: cap.fps) }
        let changed = json != layoutJSON || resized
        if changed {
            layoutJSON = json
            work.sync { if resized || encoder == nil { lastBuffer = nil; makeEncoder() } }
        }
        var missing = byDisplay.values.compactMap { $0.first }.filter { captures[$0.entry.display] == nil && live.contains($0.entry.display) }
        for c in missing where SynthSource.isSynth(c.entry.display) {
            let cap = SynthSource(byDisplay[c.entry.display]!, self)
            cap.start(fps: o.fps, queue: capQ, animated: o.synthAnimate?.contains(c.entry.screen) ?? true)
            captures[c.entry.display] = cap; log("screen \(c.entry.screen) synthetic source")
        }
        missing.removeAll { SynthSource.isSynth($0.entry.display) }
        if !missing.isEmpty {
            if let content = try? await SCShareableContent.current {
                for c in missing {
                    // A display created moments ago may not be capturable yet; it is retried on the next pass.
                    guard let d = content.displays.first(where: { $0.displayID == c.entry.display }) else { log("display \(c.entry.display) not found yet"); continue }
                    let cap = CellCapture(byDisplay[c.entry.display]!, self)
                    do { try await cap.start(d, fps: o.fps, queue: capQ); captures[c.entry.display] = cap; log("screen \(c.entry.screen) capturing") } catch { log("capture \(c.entry.screen): \(error.localizedDescription)") }
                }
            } else { log("no shareable content") }
        }
        layoutKey = Set(captures.keys) == live.intersection(cells.map { $0.entry.display }) ? key : ""  // anything missing: retry
        if changed { log("layout \(layoutJSON)"); forceKey = true }
    }

    // ---- encoding
    func makeEncoder() {
        if let e = encoder { VTCompressionSessionInvalidate(e); encoder = nil }
        appliedKbps = 0
        guard !cells.isEmpty else { return }
        var s: VTCompressionSession?
        let spec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true,
                                     kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true]
        let r = VTCompressionSessionCreate(allocator: nil, width: Int32(W), height: Int32(H), codecType: kCMVideoCodecType_H264,
                                           encoderSpecification: spec as CFDictionary,
                                           imageBufferAttributes: [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA] as CFDictionary,
                                           compressedDataAllocator: nil, outputCallback: compressionOutput,
                                           refcon: Unmanaged.passUnretained(self).toOpaque(), compressionSessionOut: &s)
        guard r == noErr, let e = s else { log("encoder \(W)x\(H) failed: \(r)"); return }
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_AverageBitRate, value: o.bitrate as CFNumber)
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: o.fps as CFNumber)
        // longgop: a keyframe every 10 s instead of 2 s. Over TCP nothing is lost, and keyframes are already forced
        // when needed (new or stuck peer, layout change); every periodic one is a burst of a whole atlas (cinema-10:
        // 27 a minute, with 130 ms fresh-frame gaps p99 on the Air's Wi-Fi, 2026-11-03).
        let gop = o.flags.contains("longgop") ? 10 : 2
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: (o.fps * gop) as CFNumber)
        if gop > 2 { VTSessionSetProperty(e, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: Double(gop) as CFNumber) }
        VTCompressionSessionPrepareToEncodeFrames(e)
        encoder = e
    }
    // Hardware decoders release a frame only when the next one arrives, so sending only on change made a still
    // screen's updates wait for the next change (~230 ms measured). After each change the previous buffer is
    // re-encoded a few times (skip-coded P-frames) to push it through, then the stream goes quiet. A constant 60 fps
    // did the same job but re-encoded the whole atlas every frame: with two screens (3840x1200) next to Sunshine's
    // stream the laptop's encoder fell to ~40 fps and latency rose to ~125 ms.
    func tick() {
        guard let e = encoder, !peers.isEmpty, let pool = VTCompressionSessionGetPixelBufferPool(e) else { return }
        lock.withLock { prof.ticks += 1 }
        // Pacing: while the network is still sending earlier frames, encode nothing (the pending change stays dirty
        // and goes out as soon as the socket drains). Dropping P-frames instead broke the reference chain, and the
        // keyframe that repaired it was bigger still: 118 keyframes a minute at ~39 Mbit/s in the stress suite.
        if dlock.withLock({ busy }) {
            busyTicks += 1; heldTicks += 1
            if o.flags.contains("bpcut"), Date().timeIntervalSince(bpCutAt) > 0.25 {
                bpScale = max(0.25, bpScale * 0.7); bpCutAt = Date(); applyBudget(force: true)
            }
            // Held back for over half a second: a peer is stuck, not just slow. Cut it loose (it skips to the next
            // keyframe, the old behaviour) so it can't freeze the stream for everyone else.
            if heldTicks > o.fps / 2 {
                heldTicks = 0
                net.async { for p in self.peers.values where p.pending >= 2 { p.needsKey = true }; self.forceKey = true; self.updateBusy() }
                log("peer stuck for 0.5 s: skipping it to a keyframe")
            }
            lock.withLock { prof.held += 1 }
            return
        }
        heldTicks = 0
        if bpScale < 1, Date().timeIntervalSince(bpCutAt) > 1, Date().timeIntervalSince(bpRiseAt) > 1 {
            bpScale = min(1, bpScale * 1.2); bpRiseAt = Date(); applyBudget(force: true)
        }
        // prifps: a change only from reduced-rate screens waits until 1/15 s after the last encode
        if o.flags.contains("prifps"), !forceKey, Date().timeIntervalSince(lastEncodeAt) < 1.0 / 15,
           lock.withLock({ dirty && !fullRateDirty }) { lock.withLock { prof.priHeld += 1 }; return }
        lock.lock(); let changed = dirty; dirty = false; fullRateDirty = false; lock.unlock()
        if changed { lastEncodeAt = Date() }
        let key = forceKey; forceKey = false
        // Three flush repeats, then one per second as a heartbeat (the headset drops a link silent for 10 s).
        ticks += 1
        let yielding = o.flags.contains("gameyield") && dlock.withLock({ gameOn && Date().timeIntervalSince(wantAt) <= 2 })
        if yielding {
            // Once a second: an encode slower than 8 ms (60 fps leaves 16.7 ms per frame for both encoders) means the
            // engine is contended, so yield more; under 4 ms, yield less. Never below every other tick.
            if Date().timeIntervalSince(yieldAt) >= 1 {
                let ms = elock.withLock { encEwma }
                let n = ms > 8 ? min(4, yieldN + 1) : ms < 4 ? max(2, yieldN - 1) : yieldN
                if n != yieldN { log(String(format: "gameyield: encoding 1 tick in %d (atlas encode %.1f ms)", n, ms)) }
                yieldN = n; yieldAt = Date()
            }
            if !key, ticks % yieldN != 0 {
                lock.withLock { if changed { dirty = true }; prof.yielded += 1 }  // goes out on the next encoding tick
                return
            }
        }
        let flushMax = yielding ? 1 : 3
        if changed { flushes = 0 } else if !key && lastBuffer != nil { flushes += 1; if flushes > flushMax && flushes % o.fps != 0 { lock.withLock { prof.idle += 1 }; return } }
        var pb = lastBuffer
        if changed || pb == nil {
            var out: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess, let fresh = out else { return }
            CVPixelBufferLockBaseAddress(fresh, [])
            if let base = CVPixelBufferGetBaseAddress(fresh)?.assumingMemoryBound(to: UInt8.self) {
                let stride = CVPixelBufferGetBytesPerRow(fresh), bw = CVPixelBufferGetWidth(fresh), bh = CVPixelBufferGetHeight(fresh)
                let w0 = DispatchTime.now().uptimeNanoseconds
                lock.lock()
                let c0 = DispatchTime.now().uptimeNanoseconds
                // The atlas can change size (refresh) before the encoder and its pool are rebuilt; copying the new
                // size into the old pool's buffer overran it (stress test crashes). Skip that frame instead.
                let fits = bw == W && bh == H && pixels.count == W * H * 4
                if fits { pixels.withUnsafeBufferPointer { src in for y in 0..<H { memcpy(base + y * stride, src.baseAddress! + y * W * 4, W * 4) } } }
                else { dirty = true }  // retry on the next tick, with the new encoder
                let c1 = DispatchTime.now().uptimeNanoseconds
                prof.tickWaitNs += c0 - w0; prof.copyNs += c1 - c0; prof.copyMax = max(prof.copyMax, c1 - c0)
                lock.unlock()
                if !fits { CVPixelBufferUnlockBaseAddress(fresh, []); return }
            }
            CVPixelBufferUnlockBaseAddress(fresh, [])
            pb = fresh; lastBuffer = fresh
        }
        frame += 1
        lock.withLock { if changed || key { prof.fresh += 1 } else { prof.repeats += 1 } }
        elock.withLock { submitAt[frame] = DispatchTime.now().uptimeNanoseconds; if submitAt.count > 600 { submitAt.removeAll() } }
        let repeatTag: UnsafeMutableRawPointer? = (changed || key) ? nil : UnsafeMutableRawPointer(bitPattern: 1)
        let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(e, imageBuffer: pb!, presentationTimeStamp: CMTime(value: frame, timescale: CMTimeScale(o.fps)),
                                        duration: CMTime(value: 1, timescale: CMTimeScale(o.fps)), frameProperties: props, sourceFrameRefcon: repeatTag, infoFlagsOut: nil)
    }
    func encoded(_ sample: CMSampleBuffer, repeated: Bool) {
        let done = DispatchTime.now().uptimeNanoseconds, pv = CMSampleBufferGetPresentationTimeStamp(sample).value
        elock.withLock {
            if let t = submitAt.removeValue(forKey: pv) { let ms = Double(done - t) / 1e6; encMs.append(ms); encEwma = encEwma * 0.9 + ms * 0.1 }
            if Date().timeIntervalSince(encLogAt) >= o.statSecs, !encMs.isEmpty {
                let v = encMs.sorted()
                log(String(format: "encode ms p50 %.1f p99 %.1f max %.1f (%d frames, %dx%d)", v[v.count / 2], v[min(v.count - 1, v.count * 99 / 100)], v[v.count - 1], v.count, W, H))
                encMs.removeAll(); encLogAt = Date()
            }
        }
        guard let block = CMSampleBufferGetDataBuffer(sample), let format = CMSampleBufferGetFormatDescription(sample) else { return }
        let att = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[String: Any]]
        let key = !(att?.first?[kCMSampleAttachmentKey_NotSync as String] as? Bool ?? false)
        var annex = Data()
        if key {
            for i in 0...1 {
                var p: UnsafePointer<UInt8>?; var len = 0, count = 0; var hl: Int32 = 0
                if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &p, parameterSetSizeOut: &len, parameterSetCountOut: &count, nalUnitHeaderLengthOut: &hl) == noErr, let p = p {
                    if i == 0 && len >= 4 { codecBytes = [p[1], p[2], p[3]] }
                    annex.append(contentsOf: [0, 0, 0, 1]); annex.append(p, count: len)
                }
            }
        }
        var total = 0; var ptr: UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &total, dataPointerOut: &ptr) == noErr, let ptr = ptr else { return }
        let avcc = Data(bytes: ptr, count: total); var off = 0
        while off + 4 <= avcc.count {
            let n = Int(avcc[off]) << 24 | Int(avcc[off + 1]) << 16 | Int(avcc[off + 2]) << 8 | Int(avcc[off + 3]); off += 4
            guard n > 0, off + n <= avcc.count else { return }
            annex.append(contentsOf: [0, 0, 0, 1]); annex.append(avcc[off..<off + n]); off += n
        }
        let pts = UInt64(max(0, CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)) * 1_000_000))
        // Bit 0: keyframe. Bit 1: repeat of the previous image, so the headset leaves it out of its latency figure.
        var p = Data([(key ? 1 : 0) | (repeated ? 2 : 0)]); var be = pts.bigEndian; Swift.withUnsafeBytes(of: &be) { p.append(contentsOf: $0) }
        var w16 = UInt16(W).bigEndian, h16 = UInt16(H).bigEndian
        Swift.withUnsafeBytes(of: &w16) { p.append(contentsOf: $0) }; Swift.withUnsafeBytes(of: &h16) { p.append(contentsOf: $0) }
        p.append(contentsOf: codecBytes); p.append(annex)
        if key { var m = Data([0x80]); m.append(Data(layoutJSON.utf8)); broadcast(m, key: true, meta: true) }
        broadcast(p, key: key, meta: false)
    }

    // ---- network (MacScreenSender's peer logic: drop to the next keyframe when a peer falls behind)
    func listen() {
        do {
            let tcp = NWProtocolTCP.Options(); tcp.noDelay = o.flags.contains("nodelay")
            let l = try NWListener(using: NWParameters(tls: nil, tcp: tcp), on: NWEndpoint.Port(rawValue: o.port)!)
            l.newConnectionHandler = { [weak self] c in self?.accept(c) }
            // A busy port only shows up here, asynchronously: exit so a supervisor never keeps a deaf second copy.
            l.stateUpdateHandler = { if case .failed(let e) = $0 { log("listen failed: \(e)"); exit(1) } }
            l.start(queue: net); log("listening on \(o.port)")
        } catch { log("listen failed: \(error)"); exit(1) }
    }
    // Display wake (see nowake): held while any peer is connected; on the net queue.
    var wakeAssertion: IOPMAssertionID = 0
    func updateWake() {
        guard !o.flags.contains("nowake") else { return }
        if !peers.isEmpty && wakeAssertion == 0 {
            var activity: IOPMAssertionID = 0
            IOPMAssertionDeclareUserActivity("pinpin-atlas: headset connected" as CFString, kIOPMUserActiveLocal, &activity)  // wakes it now
            if IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                           "pinpin-atlas: extra screens streaming" as CFString, &wakeAssertion) == kIOReturnSuccess {
                log("display kept awake while a headset is connected")
            } else { wakeAssertion = 0 }
        } else if peers.isEmpty && wakeAssertion != 0 {
            IOPMAssertionRelease(wakeAssertion); wakeAssertion = 0; log("display may sleep again (no headset)")
        }
    }
    // The headset never sends on this connection; reading anyway is how a close is noticed (EOF), which cancels the
    // connection so the peer and the display-wake assertion go away (otherwise they lingered until a send failed).
    func watchClose(_ c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] _, _, done, error in
            if done || error != nil { c.cancel() } else { self?.watchClose(c) }
        }
    }
    func accept(_ c: NWConnection) {
        let id = UUID(), peer = Peer(c); peers[id] = peer; forceKey = true
        c.stateUpdateHandler = { [weak self] st in
            switch st { case .failed, .cancelled: self?.net.async { self?.peers.removeValue(forKey: id); self?.updateWake() }; default: break }
        }
        c.start(queue: net)
        watchClose(c)
        log("headset connected"); updateWake()
    }
    // On the net queue: whether any peer still has 2+ frames queued in its socket. Read by tick() under dlock.
    func updateBusy() {
        let limit = o.flags.contains("pend1") ? 1 : 2
        let b = peers.values.contains { $0.pending >= limit && !$0.needsKey }; dlock.withLock { busy = b }
    }
    // pace: the send rate in bits/s (1.5x the applied bitrate; set from applyBudget), and the chunk pump (net queue).
    var paceBps = 60_000_000.0
    func pump(_ peer: Peer) {
        guard !peer.pumping, !peer.chunks.isEmpty else { return }
        let (chunk, last) = peer.chunks.removeFirst(); peer.pumping = true
        let gap = Double(chunk.count * 8) / paceBps, sentAt = DispatchTime.now()
        peer.c.send(content: chunk, completion: .contentProcessed { [weak self] e in
            guard let self = self else { return }
            if last { peer.pending = max(0, peer.pending - 1); self.updateBusy() }
            if e != nil { peer.c.cancel(); peer.chunks.removeAll(); peer.pumping = false; return }
            // the next chunk no sooner than this one's share of the pace rate after it was handed over
            self.net.asyncAfter(deadline: sentAt + gap) { peer.pumping = false; self.pump(peer) }
        })
    }
    func broadcast(_ packet: Data, key: Bool, meta: Bool) {
        net.async {
            var framed = Data(); var n = UInt32(packet.count).bigEndian; Swift.withUnsafeBytes(of: &n) { framed.append(contentsOf: $0) }; framed.append(packet)
            for peer in self.peers.values {
                if !meta {
                    // Only a peer that is truly stuck loses frames (and then needs a keyframe); ordinary
                    // backpressure is handled at the source by tick(), which waits instead of encoding.
                    if peer.pending >= 8 { peer.needsKey = true; self.forceKey = true; continue }
                    if peer.needsKey && !key { continue }
                    if key { peer.needsKey = false }
                }
                peer.pending += 1
                if self.o.flags.contains("pace") {
                    var off = 0
                    while off < framed.count {
                        let end = min(framed.count, off + 16_384)
                        peer.chunks.append((framed.subdata(in: off..<end), end == framed.count)); off = end
                    }
                    self.pump(peer)
                    continue
                }
                peer.c.send(content: framed, completion: .contentProcessed { e in
                    peer.pending = max(0, peer.pending - 1); if e != nil { peer.c.cancel() }
                    self.updateBusy()
                })
            }
            self.updateBusy()
        }
    }
}

let atlas = Atlas(Options())
atlas.listen()
atlas.listenDemand()
if !atlas.o.flags.isEmpty { log("flags: " + atlas.o.flags.sorted().joined(separator: ",")) }
if atlas.o.flags.contains("capfast") { CellCapture.rateFactor = 2 }
// One refresh loop (refresh is not reentrant): every 2 s, or at once when demand reveals a screen.
// When replayd (ScreenCaptureKit's daemon) crashes, an await on it can hang forever and the sender silently stops
// (seen 2026-11-03 13:11 under 30 screens). A refresh pass stuck for 20 s exits; the supervisor restarts us clean.
let stuckCheck = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "atlas.stuck"))
stuckCheck.schedule(deadline: .now() + 5, repeating: 5)
stuckCheck.setEventHandler {
    if let t = atlas.dlock.withLock({ atlas.refreshSince }), Date().timeIntervalSince(t) > 20 { log("refresh stuck for 20 s (ScreenCaptureKit not answering): exiting for a clean restart"); exit(3) }
}
stuckCheck.resume()
Task {
    while true {
        atlas.dlock.withLock { atlas.refreshSince = Date() }
        await atlas.refresh()
        atlas.dlock.withLock { atlas.refreshSince = nil }
        for _ in 0..<20 {
            try? await Task.sleep(nanoseconds: 100_000_000)
            if atlas.dlock.withLock({ let w = atlas.wake; atlas.wake = false; return w }) { break }
        }
    }
}
let timer = DispatchSource.makeTimerSource(queue: atlas.work)
timer.schedule(deadline: .now(), repeating: .milliseconds(1000 / atlas.o.fps))
timer.setEventHandler { atlas.tick() }
timer.resume()
dispatchMain()
