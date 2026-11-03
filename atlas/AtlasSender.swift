// Atlas sender for the headset's extra screens (docs/multi-screen-atlas.md).
// Captures each extra screen's macOS display with ScreenCaptureKit, packs them side by side into one
// "atlas" frame, encodes it with the hardware H.264 encoder and serves it to the headset over TCP, using
// MacScreenSender's packet format: u32 length (big endian), then [flags][pts u64][w u16][h u16][3 codec
// bytes][Annex B]. Packets starting with 0x80 carry text; the layout goes out that way before every
// keyframe: {"atlas":[W,H],"cells":[{"screen":id,"x":..,"y":..,"w":..,"h":..}]} (pixels, origin top left).
// Displays come from the virtual-display helper (GET /displays) or, for testing, --test "2:main,3:main".
// Started through Terminal (start-atlas.command) so it inherits Terminal's Screen Recording grant.
import CoreMedia
import CoreVideo
import Foundation
import Network
import ScreenCaptureKit
import VideoToolbox

func log(_ s: String) { FileHandle.standardError.write(("pinpin-atlas: " + s + "\n").data(using: .utf8)!) }

struct Options {
    var port: UInt16 = 57720, demandPort: UInt16 = 57721, fps = 60, bitrate = 40_000_000, cellW = 1920, cellH = 1200, maxW = 3840, maxH = 2160
    var url = "http://127.0.0.1:57710/displays", test = ""
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
            default: break
            }
        }
    }
}

struct Entry: Equatable { let screen: Int; let display: CGDirectDisplayID; let w: Int; let h: Int }
struct Cell { let entry: Entry; let x: Int, y: Int, w: Int, h: Int }

private let compressionOutput: VTCompressionOutputCallback = { context, refcon, status, _, sample in
    guard status == noErr, let context = context, let sample = sample else { return }
    Unmanaged<Atlas>.fromOpaque(context).takeUnretainedValue().encoded(sample, repeated: refcon != nil)
}

final class Peer { let c: NWConnection; var pending = 0; var needsKey = true; init(_ c: NWConnection) { self.c = c } }

/// Captures one display into its cell of the atlas buffer. A capture lives as long as its display stays put: layout
/// changes only move its cell (`cell`, guarded by atlas.lock) and resize its output with updateConfiguration, because
/// restarting every stream on each resize froze the atlas for seconds in the 10-screen stress test.
final class CellCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    var cell: Cell; let origin: CGPoint; weak var atlas: Atlas?; var stream: SCStream?; var dead = false
    var latest: CVPixelBuffer?  // under atlas.lock: the last frame, re-copied when the cell moves without resizing
    init(_ cell: Cell, _ atlas: Atlas) { self.cell = cell; self.atlas = atlas; origin = CGDisplayBounds(cell.entry.display).origin }
    func config(_ w: Int, _ h: Int, fps: Int) -> SCStreamConfiguration {
        let c = SCStreamConfiguration()
        c.width = w; c.height = h; c.pixelFormat = kCVPixelFormatType_32BGRA; c.showsCursor = true
        c.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps)); c.queueDepth = 5
        return c
    }
    func start(_ display: SCDisplay, fps: Int, queue: DispatchQueue) async throws {
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: config(cell.w, cell.h, fps: fps), delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture(); stream = s
    }
    func resize(_ w: Int, _ h: Int, fps: Int) async {
        do { try await stream?.updateConfiguration(config(w, h, fps: fps)) } catch { log("resize screen \(cell.entry.screen): \(error.localizedDescription)"); dead = true }
    }
    func stop() { atlas = nil; if let s = stream { stream = nil; Task { try? await s.stopCapture() } } }  // late frames go nowhere
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("screen \(cell.entry.screen) capture stopped: \(error.localizedDescription)"); dead = true; atlas?.layoutKey = ""
    }
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let pb = CMSampleBufferGetImageBuffer(sb), let a = atlas else { return }
        a.lock.lock(); defer { a.lock.unlock() }
        let screen = cell.entry.screen
        latest = pb
        if a.visible(screen) { a.stash[screen] = nil; a.blit(pb, cell) } else { a.stash[screen] = pb }
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
    struct Want { var vis = true, px = 1920, pri = 1.0 }
    let dlock = NSLock()
    var busy = false, busyTicks = 0, heldTicks = 0
    var want: [Int: Want] = [:], wantAt = Date.distantPast, budgetKbps = 0, appliedKbps = 0
    var tier: [Int: Int] = [:], tierWant: [Int: (w: Int, since: Date)] = [:]
    static let tiers = [480, 640, 960, 1280, 1920]
    let net = DispatchQueue(label: "atlas.net"), capQ = DispatchQueue(label: "atlas.capture"), work = DispatchQueue(label: "atlas.work")
    init(_ o: Options) { self.o = o }

    // Copies one captured frame into its cell; caller holds `lock`.
    func blit(_ pb: CVPixelBuffer, _ cell: Cell) {
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self), pixels.count == W * H * 4 else { return }
        let stride = CVPixelBufferGetBytesPerRow(pb), w = min(cell.w, CVPixelBufferGetWidth(pb)), h = min(cell.h, CVPixelBufferGetHeight(pb))
        guard cell.x >= 0, cell.y >= 0, cell.x + w <= W, cell.y + h <= H else { return }  // a stale cell from before a relayout
        pixels.withUnsafeMutableBufferPointer { dst in
            for y in 0..<h { memcpy(dst.baseAddress! + ((cell.y + y) * W + cell.x) * 4, base + y * stride, w * 4) }
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
            next[Int(id)] = Want(vis: (num(e["vis"]) ?? 1) != 0, px: Int(num(e["px"]) ?? 1920), pri: num(e["pri"]) ?? 1)
        }
        let now = Date(); var reveal: [Int] = []
        dlock.withLock {
            if now.timeIntervalSince(wantAt) > 10 { log("demand from headset") }
            let stale = now.timeIntervalSince(wantAt) > 2
            for (id, w) in next where id != 1 {
                if w.vis && !stale && !(want[id]?.vis ?? true) { reveal.append(id) }
                guard w.vis else { continue }  // unseen screens keep their size
                let t = Atlas.tiers.first(where: { $0 >= w.px }) ?? o.cellW, cur = tier[id] ?? o.cellW
                if min(t, o.cellW) == cur { tierWant[id] = nil } else if tierWant[id]?.w != min(t, o.cellW) { tierWant[id] = (min(t, o.cellW), now) }
            }
            want = next; wantAt = now; budgetKbps = Int(num(j["budgetKbps"]) ?? 0)
        }
        if !reveal.isEmpty {
            lock.withLock {
                for id in reveal { if let pb = stash.removeValue(forKey: id), let c = cells.first(where: { $0.entry.screen == id }) { blit(pb, c) } }
            }
        }
        work.async { self.applyBudget() }
    }
    // The extras' share of the headset's budget, by gaze priority; on the work queue.
    func applyBudget() {
        guard let e = encoder else { return }
        var kbps = o.bitrate / 1000
        dlock.withLock {
            guard budgetKbps > 0, Date().timeIntervalSince(wantAt) <= 2 else { return }
            let all = want.values.reduce(0.0) { $0 + ($1.vis ? $1.pri : 0) }
            let extras = want.reduce(0.0) { $0 + ($1.key != 1 && $1.value.vis ? $1.value.pri : 0) }
            kbps = all > 0 ? Int(Double(budgetKbps) * extras / all) : 0
        }
        kbps = min(o.bitrate / 1000, max(4000, kbps))
        if appliedKbps == 0 || abs(kbps - appliedKbps) * 100 > appliedKbps * 15 {
            appliedKbps = kbps
            VTSessionSetProperty(e, key: kVTCompressionPropertyKey_AverageBitRate, value: (kbps * 1000) as CFNumber)
            log("bitrate \(kbps) kbps")
        }
    }

    // ---- display list
    func entries() -> [Entry] {
        if !o.test.isEmpty {
            return o.test.split(separator: ",").compactMap { pair in
                let p = pair.split(separator: ":"); guard p.count == 2, let s = Int(p[0]) else { return nil }
                let d = p[1] == "main" ? CGMainDisplayID() : CGDirectDisplayID(p[1]) ?? 0
                let mode = CGDisplayCopyDisplayMode(d)
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
    func layout(_ list: [Entry], _ tiers: [Int: Int]) -> (Int, Int, [Cell]) {
        var f = 1.0
        while true {
            var cells: [Cell] = [], x = 0, y = 0, rowH = 0, W = 0, fits = true
            for e in list {
                let cw = Double(tiers[e.screen] ?? o.cellW) * f, ch = cw * Double(o.cellH) / Double(o.cellW)
                let s = min(cw / Double(e.w), ch / Double(e.h), 1.0)
                let w = max(16, Int(Double(e.w) * s) / 2 * 2), h = max(16, Int(Double(e.h) * s) / 2 * 2)
                if x + w > o.maxW { x = 0; y += rowH; rowH = 0 }
                if y + h > o.maxH { fits = false; break }
                cells.append(Cell(entry: e, x: x, y: y, w: w, h: h)); x += w; rowH = max(rowH, h); W = max(W, x)
            }
            if fits || f < 0.05 {
                if f < 1 { log("atlas: \(list.count) screens fit at \(Int(f * 100))% size") }
                return (max(16, (W + 15) / 16 * 16), max(16, (y + rowH + 15) / 16 * 16), cells)
            }
            f *= 0.9
        }
    }

    func refresh() async {
        let list = entries()
        // Size tiers settle before they apply (growing after 2 s, shrinking after 5 s): each change costs a keyframe.
        let now = Date()
        let tiers: [Int: Int] = dlock.withLock {
            if now.timeIntervalSince(wantAt) > 2 { tier = [:]; tierWant = [:] }  // no headset: back to full size
            // At most one size refit per 5 s (each is a keyframe); between refits only the bitrate follows gaze.
            if now.timeIntervalSince(lastRefit) >= 5 {
                for (id, t) in tierWant where now.timeIntervalSince(t.since) > (t.w > (tier[id] ?? o.cellW) ? 2 : 5) { tier[id] = t.w; tierWant[id] = nil; lastRefit = now }
            }
            return tier
        }
        // Display positions are part of the key: adding a virtual display can make macOS slide the others along
        // (screen 3 at x=1440 pushed screen 2 to x=3360), and captures started mid-move showed only the wallpaper.
        let key = list.map { e -> String in let r = CGDisplayBounds(e.display); return "\(e.screen):\(e.display):\(e.w)x\(e.h)@\(Int(r.minX)),\(Int(r.minY))/\(tiers[e.screen] ?? o.cellW)" }.joined(separator: ",")
        if key == layoutKey && !captures.values.contains(where: { $0.dead }) && captures.count == list.count { return }
        let (w, h, cells) = layout(list, tiers)
        let resized = w != W || h != H
        let cj = cells.map { "{\"screen\":\($0.entry.screen),\"x\":\($0.x),\"y\":\($0.y),\"w\":\($0.w),\"h\":\($0.h)}" }.joined(separator: ",")
        let json = "{\"atlas\":[\(w),\(h)],\"cells\":[\(cj)]}"
        // Stop captures whose display is gone, died, or moved (a capture started mid-move shows only the wallpaper).
        for (d, c) in captures where c.dead || !cells.contains(where: { $0.entry.display == d }) || CGDisplayBounds(d).origin != c.origin {
            c.stop(); captures[d] = nil
        }
        var resizes: [(CellCapture, Cell)] = []
        lock.withLock {
            stash = [:]; self.cells = cells; dirty = true
            if resized { W = w; H = h; pixels = [UInt8](repeating: 0, count: w * h * 4) }
            for c in cells {
                guard let cap = captures[c.entry.display] else { continue }
                let old = cap.cell; cap.cell = c
                if old.w != c.w || old.h != c.h { resizes.append((cap, c)) }
                else if let pb = cap.latest { blit(pb, c) }  // same size, new place: copy the last frame now
            }
        }
        for (cap, c) in resizes { await cap.resize(c.w, c.h, fps: o.fps) }
        let changed = json != layoutJSON || resized
        if changed {
            layoutJSON = json
            work.sync { if resized || encoder == nil { lastBuffer = nil; makeEncoder() } }
        }
        let missing = cells.filter { captures[$0.entry.display] == nil }
        if !missing.isEmpty {
            if let content = try? await SCShareableContent.current {
                for c in missing {
                    // A display created moments ago may not be capturable yet; it is retried on the next pass.
                    guard let d = content.displays.first(where: { $0.displayID == c.entry.display }) else { log("display \(c.entry.display) not found yet"); continue }
                    let cap = CellCapture(c, self)
                    do { try await cap.start(d, fps: o.fps, queue: capQ); captures[c.entry.display] = cap } catch { log("capture \(c.entry.screen): \(error.localizedDescription)") }
                }
            } else { log("no shareable content") }
        }
        layoutKey = captures.count == cells.count ? key : ""  // anything missing: try again in 2 s
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
        VTSessionSetProperty(e, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: (o.fps * 2) as CFNumber)
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
        // Pacing: while the network is still sending earlier frames, encode nothing (the pending change stays dirty
        // and goes out as soon as the socket drains). Dropping P-frames instead broke the reference chain, and the
        // keyframe that repaired it was bigger still: 118 keyframes a minute at ~39 Mbit/s in the stress suite.
        if dlock.withLock({ busy }) {
            busyTicks += 1; heldTicks += 1
            // Held back for over half a second: a peer is stuck, not just slow. Cut it loose (it skips to the next
            // keyframe, the old behaviour) so it can't freeze the stream for everyone else.
            if heldTicks > o.fps / 2 {
                heldTicks = 0
                net.async { for p in self.peers.values where p.pending >= 2 { p.needsKey = true }; self.forceKey = true; self.updateBusy() }
                log("peer stuck for 0.5 s: skipping it to a keyframe")
            }
            return
        }
        heldTicks = 0
        lock.lock(); let changed = dirty; dirty = false; lock.unlock()
        let key = forceKey; forceKey = false
        // Three flush repeats, then one per second as a heartbeat (the headset drops a link silent for 10 s).
        if changed { flushes = 0 } else if !key && lastBuffer != nil { flushes += 1; if flushes > 3 && flushes % o.fps != 0 { return } }
        var pb = lastBuffer
        if changed || pb == nil {
            var out: CVPixelBuffer?
            guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess, let fresh = out else { return }
            CVPixelBufferLockBaseAddress(fresh, [])
            if let base = CVPixelBufferGetBaseAddress(fresh)?.assumingMemoryBound(to: UInt8.self) {
                let stride = CVPixelBufferGetBytesPerRow(fresh), bw = CVPixelBufferGetWidth(fresh), bh = CVPixelBufferGetHeight(fresh)
                lock.lock()
                // The atlas can change size (refresh) before the encoder and its pool are rebuilt; copying the new
                // size into the old pool's buffer overran it (stress test crashes). Skip that frame instead.
                let fits = bw == W && bh == H && pixels.count == W * H * 4
                if fits { pixels.withUnsafeBufferPointer { src in for y in 0..<H { memcpy(base + y * stride, src.baseAddress! + y * W * 4, W * 4) } } }
                else { dirty = true }  // retry on the next tick, with the new encoder
                lock.unlock()
                if !fits { CVPixelBufferUnlockBaseAddress(fresh, []); return }
            }
            CVPixelBufferUnlockBaseAddress(fresh, [])
            pb = fresh; lastBuffer = fresh
        }
        frame += 1
        let repeatTag: UnsafeMutableRawPointer? = (changed || key) ? nil : UnsafeMutableRawPointer(bitPattern: 1)
        let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(e, imageBuffer: pb!, presentationTimeStamp: CMTime(value: frame, timescale: CMTimeScale(o.fps)),
                                        duration: CMTime(value: 1, timescale: CMTimeScale(o.fps)), frameProperties: props, sourceFrameRefcon: repeatTag, infoFlagsOut: nil)
    }
    func encoded(_ sample: CMSampleBuffer, repeated: Bool) {
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
            let l = try NWListener(using: .tcp, on: NWEndpoint.Port(rawValue: o.port)!)
            l.newConnectionHandler = { [weak self] c in self?.accept(c) }
            // A busy port only shows up here, asynchronously: exit so a supervisor never keeps a deaf second copy.
            l.stateUpdateHandler = { if case .failed(let e) = $0 { log("listen failed: \(e)"); exit(1) } }
            l.start(queue: net); log("listening on \(o.port)")
        } catch { log("listen failed: \(error)"); exit(1) }
    }
    func accept(_ c: NWConnection) {
        let id = UUID(), peer = Peer(c); peers[id] = peer; forceKey = true
        c.stateUpdateHandler = { [weak self] st in
            switch st { case .failed, .cancelled: self?.net.async { self?.peers.removeValue(forKey: id) }; default: break }
        }
        c.start(queue: net)
        log("headset connected")
    }
    // On the net queue: whether any peer still has 2+ frames queued in its socket. Read by tick() under dlock.
    func updateBusy() { let b = peers.values.contains { $0.pending >= 2 && !$0.needsKey }; dlock.withLock { busy = b } }
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
Task { while true { await atlas.refresh(); try? await Task.sleep(nanoseconds: 2_000_000_000) } }
let timer = DispatchSource.makeTimerSource(queue: atlas.work)
timer.schedule(deadline: .now(), repeating: .milliseconds(1000 / atlas.o.fps))
timer.setEventHandler { atlas.tick() }
timer.resume()
dispatchMain()
