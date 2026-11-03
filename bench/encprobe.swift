// Stand-in for Sunshine's game encode (bench): encodes a moving 2560x1600 picture with the hardware HEVC encoder at
// 60 fps, real-time, the way a game stream does, and reports its per-frame encode latency. Run next to the atlas
// sender: how much the atlas slows this down is its cost to gaming on the laptop's one media engine.
//   swiftc -O -o encprobe bench/encprobe.swift && ./encprobe [seconds=20] [width=2560] [height=1600] [hevc|h264]
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

let args = CommandLine.arguments
let secs = args.count > 1 ? Double(args[1]) ?? 20 : 20, W = args.count > 2 ? Int(args[2]) ?? 2560 : 2560, H = args.count > 3 ? Int(args[3]) ?? 1600 : 1600
let codec = args.count > 4 && args[4] == "h264" ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC
let lock = NSLock(); var submitted: [Int64: UInt64] = [:], lat: [Double] = [], errors = 0

var session: VTCompressionSession?
let spec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true]
VTCompressionSessionCreate(allocator: nil, width: Int32(W), height: Int32(H), codecType: codec, encoderSpecification: spec as CFDictionary,
                           imageBufferAttributes: [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA] as CFDictionary,
                           compressedDataAllocator: nil, outputCallback: { _, _, status, _, sample in
    let done = DispatchTime.now().uptimeNanoseconds
    lock.lock(); defer { lock.unlock() }
    guard status == noErr, let sample = sample else { errors += 1; return }
    if let t = submitted.removeValue(forKey: CMSampleBufferGetPresentationTimeStamp(sample).value) { lat.append(Double(done - t) / 1e6) }
}, refcon: nil, compressionSessionOut: &session)
guard let s = session else { print("{\"error\":\"no encoder\"}"); exit(1) }
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: 80_000_000 as CFNumber)
VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: 60 as CFNumber)
VTCompressionSessionPrepareToEncodeFrames(s)
guard let pool = VTCompressionSessionGetPixelBufferPool(s) else { print("{\"error\":\"no pool\"}"); exit(1) }

// 8 precomputed game-like pictures (scrolled gradients plus a noisy block), cycled, so every frame differs everywhere
// and the CPU only copies (filling 2560x1600 per frame in software would load the CPU, not the encoder)
let ring: [[UInt32]] = (0..<8).map { k in
    var px = [UInt32](repeating: 0, count: W * H); var r = UInt32(k + 1) &* 2654435761
    for y in 0..<H { for x in 0..<W { let v = UInt32(truncatingIfNeeded: y + k * 37 + x)
        px[y * W + x] = 0xFF000000 | (v & 0xFF) << 16 | (UInt32(truncatingIfNeeded: y + k * 11) & 0xFF) << 8 | (UInt32(truncatingIfNeeded: x * 7 + k * 29) & 0xFF) } }
    let bx = (k * 211) % (W - 400), by = (k * 97) % (H - 300)
    for y in by..<by + 300 { for x in bx..<bx + 400 { r = r &* 1664525 &+ 1013904223; px[y * W + x] = 0xFF000000 | (r >> 8) } }
    return px
}
var frame: Int64 = 0
let start = Date(), period = 1.0 / 60
while Date().timeIntervalSince(start) < secs {
    let due = start.addingTimeInterval(Double(frame) * period)
    let wait = due.timeIntervalSinceNow; if wait > 0 { usleep(UInt32(wait * 1e6)) }
    var pb: CVPixelBuffer?
    guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb) == kCVReturnSuccess, let buf = pb else { continue }
    CVPixelBufferLockBaseAddress(buf, [])
    if let base = CVPixelBufferGetBaseAddress(buf)?.assumingMemoryBound(to: UInt8.self) {
        let stride = CVPixelBufferGetBytesPerRow(buf), src = ring[Int(frame % 8)]
        src.withUnsafeBytes { s in for y in 0..<H { memcpy(base + y * stride, s.baseAddress! + y * W * 4, W * 4) } }
    }
    CVPixelBufferUnlockBaseAddress(buf, [])
    frame += 1
    lock.lock(); submitted[frame] = DispatchTime.now().uptimeNanoseconds; lock.unlock()
    VTCompressionSessionEncodeFrame(s, imageBuffer: buf, presentationTimeStamp: CMTime(value: frame, timescale: 60), duration: CMTime(value: 1, timescale: 60),
                                    frameProperties: nil, sourceFrameRefcon: nil, infoFlagsOut: nil)
}
VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
lock.lock(); let v = lat.sorted(); lock.unlock()
func pct(_ p: Double) -> Double { v.isEmpty ? -1 : v[min(v.count - 1, Int(Double(v.count) * p))] }
print(String(format: "{\"frames\":%d,\"encoded\":%d,\"fps\":%.1f,\"p50\":%.2f,\"p95\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"errors\":%d}",
             frame, v.count, Double(v.count) / secs, pct(0.5), pct(0.95), pct(0.99), v.last ?? -1, errors))
