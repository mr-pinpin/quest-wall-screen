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
    var port: UInt16 = 57720, fps = 30, bitrate = 40_000_000, cellW = 1920, cellH = 1200, maxW = 3840, maxH = 2160
    var url = "http://127.0.0.1:57710/displays", test = ""
    init() {
        var it = CommandLine.arguments.dropFirst().makeIterator()
        while let a = it.next() {
            guard let v = it.next() else { break }
            switch a {
            case "--port": port = UInt16(v) ?? port
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

private let compressionOutput: VTCompressionOutputCallback = { context, _, status, _, sample in
    guard status == noErr, let context = context, let sample = sample else { return }
    Unmanaged<Atlas>.fromOpaque(context).takeUnretainedValue().encoded(sample)
}

final class Peer { let c: NWConnection; var pending = 0; var needsKey = true; init(_ c: NWConnection) { self.c = c } }

/// Captures one display into its cell of the atlas buffer.
final class CellCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    let cell: Cell; weak var atlas: Atlas?; var stream: SCStream?
    init(_ cell: Cell, _ atlas: Atlas) { self.cell = cell; self.atlas = atlas }
    func start(_ display: SCDisplay, fps: Int, queue: DispatchQueue) async throws {
        let c = SCStreamConfiguration()
        c.width = cell.w; c.height = cell.h; c.pixelFormat = kCVPixelFormatType_32BGRA; c.showsCursor = true
        c.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps)); c.queueDepth = 3
        let s = SCStream(filter: SCContentFilter(display: display, excludingWindows: []), configuration: c, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        try await s.startCapture(); stream = s
    }
    func stop() { if let s = stream { stream = nil; Task { try? await s.stopCapture() } } }
    func stream(_ stream: SCStream, didStopWithError error: Error) { log("screen \(cell.entry.screen) capture stopped: \(error.localizedDescription)"); atlas?.layoutKey = "" }
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let pb = CMSampleBufferGetImageBuffer(sb), let a = atlas else { return }
        CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self) else { return }
        let stride = CVPixelBufferGetBytesPerRow(pb), w = min(cell.w, CVPixelBufferGetWidth(pb)), h = min(cell.h, CVPixelBufferGetHeight(pb))
        a.lock.lock(); defer { a.lock.unlock() }
        guard a.pixels.count == a.W * a.H * 4 else { return }
        a.pixels.withUnsafeMutableBufferPointer { dst in
            for y in 0..<h { memcpy(dst.baseAddress! + ((cell.y + y) * a.W + cell.x) * 4, base + y * stride, w * 4) }
        }
        a.dirty = true
    }
}

final class Atlas {
    let o: Options
    let lock = NSLock()
    var W = 0, H = 0, pixels: [UInt8] = [], dirty = false
    var cells: [Cell] = [], captures: [CellCapture] = []
    var layoutKey = "", layoutJSON = ""
    var encoder: VTCompressionSession?
    var peers: [UUID: Peer] = [:]
    var forceKey = true, codecBytes: [UInt8] = [0x64, 0, 0x33], frame: Int64 = 0, lastEncode: UInt64 = 0
    let net = DispatchQueue(label: "atlas.net"), capQ = DispatchQueue(label: "atlas.capture"), work = DispatchQueue(label: "atlas.work")
    init(_ o: Options) { self.o = o }

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

    // Row packing: each display fits a cellW x cellH box keeping its aspect; rows wrap at maxW.
    func layout(_ list: [Entry]) -> (Int, Int, [Cell]) {
        var cells: [Cell] = [], x = 0, y = 0, rowH = 0, W = 0
        for e in list {
            let s = min(Double(o.cellW) / Double(e.w), Double(o.cellH) / Double(e.h), 1.0)
            let w = max(16, Int(Double(e.w) * s) / 2 * 2), h = max(16, Int(Double(e.h) * s) / 2 * 2)
            if x + w > o.maxW { x = 0; y += rowH; rowH = 0 }
            if y + h > o.maxH { log("atlas full: screen \(e.screen) skipped"); continue }
            cells.append(Cell(entry: e, x: x, y: y, w: w, h: h)); x += w; rowH = max(rowH, h); W = max(W, x)
        }
        return (max(16, (W + 15) / 16 * 16), max(16, (y + rowH + 15) / 16 * 16), cells)
    }

    func refresh() async {
        let list = entries()
        let key = list.map { "\($0.screen):\($0.display):\($0.w)x\($0.h)" }.joined(separator: ",")
        if key == layoutKey { return }
        layoutKey = key
        for c in captures { c.stop() }; captures = []
        let (w, h, cells) = layout(list)
        lock.withLock { W = w; H = h; pixels = [UInt8](repeating: 0, count: w * h * 4); self.cells = cells; dirty = true }
        let cj = cells.map { "{\"screen\":\($0.entry.screen),\"x\":\($0.x),\"y\":\($0.y),\"w\":\($0.w),\"h\":\($0.h)}" }.joined(separator: ",")
        layoutJSON = "{\"atlas\":[\(w),\(h)],\"cells\":[\(cj)]}"
        work.sync { makeEncoder() }
        guard let content = try? await SCShareableContent.current else { log("no shareable content"); layoutKey = ""; return }
        for c in cells {
            guard let d = content.displays.first(where: { $0.displayID == c.entry.display }) else { log("display \(c.entry.display) not found"); continue }
            let cap = CellCapture(c, self)
            do { try await cap.start(d, fps: o.fps, queue: capQ); captures.append(cap) } catch { log("capture \(c.entry.screen): \(error.localizedDescription)") }
        }
        log("layout \(layoutJSON)")
        forceKey = true
    }

    // ---- encoding
    func makeEncoder() {
        if let e = encoder { VTCompressionSessionInvalidate(e); encoder = nil }
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
    func tick() {
        guard let e = encoder, !peers.isEmpty, let pool = VTCompressionSessionGetPixelBufferPool(e) else { return }
        lock.lock(); let changed = dirty; dirty = false; lock.unlock()
        let key = forceKey
        // A still display still gets a frame every 200 ms: decoders hold the first frame until the next arrives.
        let now = DispatchTime.now().uptimeNanoseconds
        if !changed && !key && now - lastEncode < 200_000_000 { return }
        lastEncode = now
        forceKey = false
        var out: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess, let pb = out else { return }
        CVPixelBufferLockBaseAddress(pb, [])
        if let base = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self) {
            let stride = CVPixelBufferGetBytesPerRow(pb)
            lock.lock()
            pixels.withUnsafeBufferPointer { src in for y in 0..<H { memcpy(base + y * stride, src.baseAddress! + y * W * 4, W * 4) } }
            lock.unlock()
        }
        CVPixelBufferUnlockBaseAddress(pb, [])
        frame += 1
        let props: CFDictionary? = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(e, imageBuffer: pb, presentationTimeStamp: CMTime(value: frame, timescale: CMTimeScale(o.fps)),
                                        duration: CMTime(value: 1, timescale: CMTimeScale(o.fps)), frameProperties: props, sourceFrameRefcon: nil, infoFlagsOut: nil)
    }
    func encoded(_ sample: CMSampleBuffer) {
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
        var p = Data([key ? 1 : 0]); var be = pts.bigEndian; Swift.withUnsafeBytes(of: &be) { p.append(contentsOf: $0) }
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
    func broadcast(_ packet: Data, key: Bool, meta: Bool) {
        net.async {
            var framed = Data(); var n = UInt32(packet.count).bigEndian; Swift.withUnsafeBytes(of: &n) { framed.append(contentsOf: $0) }; framed.append(packet)
            for peer in self.peers.values {
                if !meta {
                    if peer.pending >= 3 { peer.needsKey = true; self.forceKey = true; continue }
                    if peer.needsKey && !key { continue }
                    if key { peer.needsKey = false }
                }
                peer.pending += 1
                peer.c.send(content: framed, completion: .contentProcessed { e in peer.pending = max(0, peer.pending - 1); if e != nil { peer.c.cancel() } })
            }
        }
    }
}

let atlas = Atlas(Options())
atlas.listen()
Task { while true { await atlas.refresh(); try? await Task.sleep(nanoseconds: 2_000_000_000) } }
let timer = DispatchSource.makeTimerSource(queue: atlas.work)
timer.schedule(deadline: .now(), repeating: .milliseconds(1000 / atlas.o.fps))
timer.setEventHandler { atlas.tick() }
timer.resume()
dispatchMain()
