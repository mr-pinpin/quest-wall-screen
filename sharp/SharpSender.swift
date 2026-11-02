// Build-to-lossless refinement sender for the wall panel (see docs/sharp-text.md).
// Captures the screen at the stream size, tracks 64x64 tiles, sends INVALIDATE the moment a
// tile changes and the exact pixels (RGB, raw deflate) once it has been still for `settle` s.
import Compression
import CoreMedia
import Foundation
import ScreenCaptureKit

struct Options {
    var width = 2560, height = 1600, tile = 64, port: UInt16 = 57700, settle = 0.2, display = 0
    init() {
        var it = CommandLine.arguments.dropFirst().makeIterator()
        while let a = it.next() {
            guard let v = it.next() else { break }
            switch a {
            case "--width": width = Int(v) ?? width
            case "--height": height = Int(v) ?? height
            case "--tile": tile = Int(v) ?? tile
            case "--port": port = UInt16(v) ?? port
            case "--settle": settle = Double(v) ?? settle
            case "--display": display = Int(v) ?? display
            default: break
            }
        }
    }
}

func log(_ s: String) { FileHandle.standardError.write(("pinpin-sharp: " + s + "\n").data(using: .utf8)!) }
func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e9 }

/// One connected headset. Writes happen on a private queue; `backlog` lets the producer
/// hold back tiles (never invalidations) while the socket is behind.
final class Client {
    let fd: Int32
    private let q = DispatchQueue(label: "sharp.client")
    private let lock = NSLock()
    private var queued = 0
    private(set) var alive = true
    init(fd: Int32) { self.fd = fd }
    var backlog: Int { lock.lock(); defer { lock.unlock() }; return queued }
    func send(_ type: UInt8, _ payload: Data) {
        var frame = Data([type])
        var n = UInt32(payload.count).littleEndian
        frame.append(Data(bytes: &n, count: 4)); frame.append(payload)
        lock.lock(); queued += frame.count; lock.unlock()
        q.async { [self] in
            if alive {
                frame.withUnsafeBytes { raw in
                    var off = 0
                    while off < raw.count {
                        let r = Darwin.send(fd, raw.baseAddress! + off, raw.count - off, 0)
                        if r <= 0 { alive = false; break }
                        off += r
                    }
                }
            }
            lock.lock(); queued -= frame.count; lock.unlock()
        }
    }
    func close() { alive = false; q.async { Darwin.close(self.fd) } }
}

final class Refiner: NSObject, SCStreamOutput, SCStreamDelegate {
    var o: Options
    var tx: Int, ty: Int
    var cur: [UInt8]                 // BGRA of the latest captured frame
    var changedAt: [Double]
    var dirty: [Bool]                // changed since last sent
    var sent: [Bool]                 // headset currently shows our lossless pixels
    var busy: [Double]               // decaying change count: video and animations wait longer before refining
    var client: Client?
    let q = DispatchQueue(label: "sharp.capture")
    var stream: SCStream?
    var timer: DispatchSourceTimer?
    var stats = (frames: 0, invalidated: 0, tiles: 0, bytes: 0)
    var attemptAt = now()             // last capture start attempt; stuck starts are restarted from outside

    init(_ o: Options) {
        self.o = o
        tx = 0; ty = 0; cur = []; changedAt = []; dirty = []; sent = []; busy = []
        super.init()
        allocate()
    }
    func allocate() {
        tx = (o.width + o.tile - 1) / o.tile; ty = (o.height + o.tile - 1) / o.tile
        cur = [UInt8](repeating: 0, count: o.width * o.height * 4)
        changedAt = [Double](repeating: 0, count: tx * ty)
        dirty = [Bool](repeating: false, count: tx * ty)
        sent = [Bool](repeating: false, count: tx * ty)
        busy = [Double](repeating: 0, count: tx * ty)
    }
    func configuration(_ w: Int, _ h: Int) -> SCStreamConfiguration {
        let c = SCStreamConfiguration()
        c.width = w; c.height = h
        c.pixelFormat = kCVPixelFormatType_32BGRA
        // No color conversion: Sunshine encodes the display's native colors, and exact tiles must match the video.
        c.showsCursor = true
        c.minimumFrameInterval = CMTime(value: 1, timescale: 60)
        c.queueDepth = 5
        return c
    }
    // The headset asks for the size it decodes (NATIVE switches the stream between 2560x1600 and 2880x1800).
    func resize(_ w: Int, _ h: Int) async {
        guard (64...8192).contains(w), (64...8192).contains(h), w != o.width || h != o.height else { return }
        if let s = stream { try? await s.updateConfiguration(configuration(w, h)) }
        q.sync { o.width = w; o.height = h; allocate() }
        log("capture size now \(w)x\(h), \(tx)x\(ty) tiles")
    }
    // A tile that keeps changing (video, Dock magnification) refines only after a long pause, so moving
    // content never alternates between video and exact pixels.
    func settle(_ idx: Int, _ t: Double) -> Double {
        busy[idx] * exp(-(t - changedAt[idx]) / 2) > 2.5 ? 1.5 : o.settle
    }

    func start() async throws {
        attemptAt = now()
        let content = try await SCShareableContent.current
        guard o.display < content.displays.count else { throw NSError(domain: "sharp", code: 1, userInfo: [NSLocalizedDescriptionKey: "no display \(o.display)"]) }
        let display = content.displays[o.display]
        let filter = SCContentFilter(display: display, excludingWindows: [])
        let s = SCStream(filter: filter, configuration: configuration(o.width, o.height), delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: q)
        try await s.startCapture()
        stream = s
        log("capturing display \(display.displayID) \(display.width)x\(display.height) at \(o.width)x\(o.height), \(tx)x\(ty) tiles")
        if timer == nil {
            let t = DispatchSource.makeTimerSource(queue: q)
            t.schedule(deadline: .now(), repeating: .milliseconds(20))
            t.setEventHandler { [weak self] in self?.flush() }
            t.resume(); timer = t
        }
    }

    // macOS stops capture on display sleep, lock or reconfiguration. Keep serving and restart capture;
    // tiles already shown stay valid until a frame proves otherwise.
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("capture stopped: \(error.localizedDescription); restarting")
        q.async { self.stream = nil }
        restart(after: 2)
    }
    func restart(after seconds: Double) {
        Task {
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
            do { try await self.start() } catch { log("restart failed: \(error.localizedDescription)"); self.restart(after: 5) }
        }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen,
              let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let info = atts.first,
              let raw = info[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
              let pb = CMSampleBufferGetImageBuffer(sb) else { return }
        stats.frames += 1
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self) else { return }
        let stride = CVPixelBufferGetBytesPerRow(pb)
        if CVPixelBufferGetWidth(pb) != o.width || CVPixelBufferGetHeight(pb) != o.height { return }  // mid-resize frame
        let w = min(o.width, CVPixelBufferGetWidth(pb)), h = min(o.height, CVPixelBufferGetHeight(pb))
        let t = now()
        var invalid: [UInt16] = []
        // Every tile is compared against the previous frame: exact and cheap (frames only arrive on change).
        for idx in 0..<(tx * ty) {
            let x0 = (idx % tx) * o.tile, y0 = (idx / tx) * o.tile
            let cw = min(o.tile, w - x0), ch = min(o.tile, h - y0)
            if cw <= 0 || ch <= 0 { continue }
            var changed = false
            cur.withUnsafeMutableBufferPointer { dst in
                for y in y0..<(y0 + ch) {
                    let src = base + y * stride + x0 * 4
                    let d = dst.baseAddress! + (y * o.width + x0) * 4
                    if memcmp(d, src, cw * 4) != 0 { memcpy(d, src, cw * 4); changed = true }
                }
            }
            if changed {
                busy[idx] = busy[idx] * exp(-(t - changedAt[idx]) / 2) + 1
                changedAt[idx] = t; dirty[idx] = true
                if sent[idx] { sent[idx] = false; invalid.append(UInt16(idx)) }
            }
        }
        if !invalid.isEmpty, let c = client, c.alive {
            var p = Data(); var n = UInt16(invalid.count).littleEndian; p.append(Data(bytes: &n, count: 2))
            for var i in invalid { i = i.littleEndian; p.append(Data(bytes: &i, count: 2)) }
            c.send(1, p); stats.invalidated += invalid.count
        }
    }

    // Send tiles that have been still for `settle` seconds, oldest first, within a backlog budget.
    func flush() {
        guard let c = client else { return }
        if !c.alive { client = nil; c.close(); log("client gone"); return }
        let t = now()
        var scratch = [UInt8](repeating: 0, count: o.tile * o.tile * 3)
        var out = [UInt8](repeating: 0, count: o.tile * o.tile * 3 + 1024)
        for idx in 0..<(tx * ty) where dirty[idx] && t - changedAt[idx] >= settle(idx, t) {
            if c.backlog > 4 << 20 { break }
            let x0 = (idx % tx) * o.tile, y0 = (idx / tx) * o.tile
            let cw = min(o.tile, o.width - x0), ch = min(o.tile, o.height - y0)
            cur.withUnsafeBufferPointer { s in
                scratch.withUnsafeMutableBufferPointer { d in
                    for y in 0..<ch {
                        let src = s.baseAddress! + ((y0 + y) * o.width + x0) * 4, dst = d.baseAddress! + y * cw * 3
                        for i in 0..<cw { dst[i * 3] = src[i * 4 + 2]; dst[i * 3 + 1] = src[i * 4 + 1]; dst[i * 3 + 2] = src[i * 4] }
                    }
                }
            }
            let rawLen = cw * ch * 3
            let n = compression_encode_buffer(&out, out.count, scratch, rawLen, nil, COMPRESSION_ZLIB)
            var p = Data(capacity: n + 6)
            var i16 = UInt16(idx).littleEndian; p.append(Data(bytes: &i16, count: 2))
            var r32 = UInt32(rawLen).littleEndian; p.append(Data(bytes: &r32, count: 4))
            if n > 0 { p.append(out, count: n) } else { p.append(scratch, count: rawLen) }  // n == 0: incompressible, sent stored
            c.send(2, p)
            dirty[idx] = false; sent[idx] = true
            stats.tiles += 1; stats.bytes += p.count
        }
    }

    func attach(_ c: Client) {
        q.async { [self] in
            client?.close()
            client = c
            for i in 0..<(tx * ty) { dirty[i] = true; sent[i] = false; changedAt[i] = 0 }
            var hello = Data("PPSH".utf8); hello.append(1)
            for v in [o.width, o.height, o.tile] { var u = UInt16(v).littleEndian; hello.append(Data(bytes: &u, count: 2)) }
            // Hello is raw (no frame header) so the client can validate before parsing frames.
            hello.withUnsafeBytes { _ = Darwin.send(c.fd, $0.baseAddress!, $0.count, 0) }
            log("client attached")
        }
    }
}

func serve(_ r: Refiner, port: UInt16) {
    let s = socket(AF_INET, SOCK_STREAM, 0)
    var one: Int32 = 1
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
    var a = sockaddr_in(); a.sin_family = sa_family_t(AF_INET); a.sin_port = port.bigEndian; a.sin_addr.s_addr = INADDR_ANY
    let bound = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
    guard bound == 0, listen(s, 4) == 0 else { log("cannot listen on \(port)"); exit(1) }
    log("listening on \(port)")
    Thread.detachNewThread {
        while true {
            let fd = accept(s, nil, nil)
            if fd < 0 { continue }
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
            var nosig: Int32 = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, socklen_t(MemoryLayout<Int32>.size))
            // Optional size request "PPSZ" u16 w, u16 h (little endian); older headsets send nothing.
            var tv = timeval(tv_sec: 0, tv_usec: 500_000)
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
            var req = [UInt8](repeating: 0, count: 8)
            let n = recv(fd, &req, 8, MSG_WAITALL)
            if n == 8, req[0] == 0x50, req[1] == 0x50, req[2] == 0x53, req[3] == 0x5A {
                let w = Int(req[4]) | Int(req[5]) << 8, h = Int(req[6]) | Int(req[7]) << 8
                let done = DispatchSemaphore(value: 0)
                Task { await r.resize(w, h); done.signal() }
                done.wait()
            }
            r.attach(Client(fd: fd))
        }
    }
}

let opts = Options()
let refiner = Refiner(opts)
signal(SIGPIPE, SIG_IGN)
serve(refiner, port: opts.port)
let ping = DispatchSource.makeTimerSource(queue: refiner.q)
ping.schedule(deadline: .now() + 2, repeating: 2)
ping.setEventHandler {
    // A capture start that never completes (seen once on macOS 15) is not recoverable in-process:
    // exit so the watchdog LaunchAgent starts a fresh sender.
    if refiner.stream == nil && now() - refiner.attemptAt > 30 { log("capture did not start within 30 s; exiting"); exit(3) }
    if let c = refiner.client, c.alive { c.send(3, Data()) }
    let s = refiner.stats
    if s.frames > 0 { log("frames \(s.frames) invalidated \(s.invalidated) tiles \(s.tiles) bytes \(s.bytes)") }
    refiner.stats = (0, 0, 0, 0)
}
ping.resume()
Task {
    do { try await refiner.start() } catch { log("start failed: \(error.localizedDescription); retrying"); refiner.restart(after: 5) }
}
dispatchMain()
