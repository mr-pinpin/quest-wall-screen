// PinPin Stress Pattern: fills the given displays (CGDirectDisplayIDs as arguments) with moving content that
// encodes like real use: sliding bars (motion), scrolling text (detail) and a noise block (video-like entropy).
// Everything is Core Animation, so the GPU draws it and the stress test measures the sender, not this app.
// Started by bench/stress.py over ssh (its windows still reach the GUI session); stopped with pkill.
// `--cover ID,ID,...`: those displays get one full-screen window that never changes (a dark desktop with static
// text), hiding macOS's animated aerial wallpaper so idle screens really are idle (no redraws, nothing to capture).
// This avoids setDesktopImageURL, which stopped the pattern from reaching ScreenCaptureKit on that display.
import AppKit
import QuartzCore

func noiseImage(_ w: Int, _ h: Int) -> CGImage {
    var px = [UInt8](repeating: 0, count: w * h)
    for i in px.indices { px[i] = UInt8.random(in: 0...255) }
    let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    return ctx.makeImage()!
}

func patternLayer(size: CGSize, label: String) -> (CALayer, CALayer, CATextLayer) {
    let root = CALayer(); root.frame = CGRect(origin: .zero, size: size)
    let hue = CABasicAnimation(keyPath: "backgroundColor")
    hue.fromValue = NSColor(hue: 0.6, saturation: 0.4, brightness: 0.3, alpha: 1).cgColor
    hue.toValue = NSColor(hue: 0.9, saturation: 0.4, brightness: 0.3, alpha: 1).cgColor
    hue.duration = 7; hue.autoreverses = true; hue.repeatCount = .infinity
    root.add(hue, forKey: "hue")

    let text = CATextLayer()  // scrolling text: fine detail that must stay legible
    let lines = (0..<Int(size.height / 15)).map { "\(label)  line \($0)  the quick brown fox jumps over the lazy dog 0123456789" }
    text.string = lines.joined(separator: "\n"); text.fontSize = 22; text.foregroundColor = NSColor.white.cgColor
    text.font = NSFont.monospacedSystemFont(ofSize: 22, weight: .regular); text.contentsScale = 1
    text.frame = CGRect(x: 60, y: -size.height, width: size.width - 120, height: size.height * 2)
    let scroll = CABasicAnimation(keyPath: "position.y")
    scroll.byValue = size.height / 2; scroll.duration = 12; scroll.repeatCount = .infinity
    text.add(scroll, forKey: "scroll"); root.addSublayer(text)

    for i in 0..<10 {  // bars sliding across: global motion
        let bar = CALayer(); bar.backgroundColor = NSColor(white: i % 2 == 0 ? 0.9 : 0.1, alpha: 0.7).cgColor
        bar.frame = CGRect(x: 0, y: 0, width: 36, height: size.height)
        let slide = CABasicAnimation(keyPath: "position.x")
        slide.fromValue = -40; slide.toValue = size.width + 40; slide.duration = Double(size.width) / 300
        slide.timeOffset = slide.duration * Double(i) / 10; slide.repeatCount = .infinity
        bar.add(slide, forKey: "slide"); root.addSublayer(bar)
    }

    let noise = CALayer()  // noise block: video-like entropy, new random image every frame
    noise.frame = CGRect(x: size.width * 0.55, y: size.height * 0.25, width: 480, height: 320)
    noise.magnificationFilter = .nearest; root.addSublayer(noise)

    let counter = CATextLayer()
    counter.fontSize = 90; counter.foregroundColor = NSColor.systemYellow.cgColor; counter.contentsScale = 1
    counter.frame = CGRect(x: 60, y: size.height - 140, width: size.width - 120, height: 110)
    root.addSublayer(counter)
    return (root, noise, counter)
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
var argv = Array(CommandLine.arguments.dropFirst()), covered: [UInt32] = []
if let i = argv.firstIndex(of: "--cover"), i + 1 < argv.count {
    covered = argv[i + 1].split(separator: ",").compactMap { UInt32($0) }; argv.removeSubrange(i...(i + 1))
}
let ids = argv.compactMap { UInt32($0) }
var windows: [NSWindow] = []
var animated: [(CALayer, CATextLayer, String)] = []
for s in NSScreen.screens {
    guard let n = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber, ids.contains(n.uint32Value) else { continue }
    let win = NSWindow(contentRect: s.frame, styleMask: .borderless, backing: .buffered, defer: false)
    win.setFrame(s.frame, display: true)
    let view = NSView(frame: NSRect(origin: .zero, size: s.frame.size)); view.wantsLayer = true
    let label = "STRESS \(n.uint32Value)"
    let (root, noise, counter) = patternLayer(size: s.frame.size, label: label)
    view.layer = root; win.contentView = view
    win.orderFrontRegardless(); windows.append(win); animated.append((noise, counter, label))
}
var coveredCount = 0
for s in NSScreen.screens {
    guard let n = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber, covered.contains(n.uint32Value) else { continue }
    let win = NSWindow(contentRect: s.frame, styleMask: .borderless, backing: .buffered, defer: false)
    win.setFrame(s.frame, display: true); win.backgroundColor = NSColor(calibratedRed: 0.13, green: 0.13, blue: 0.15, alpha: 1)
    let label = NSTextField(labelWithString: "PinPin screen \(n.uint32Value)\n(idle desktop, static)")
    label.font = NSFont.systemFont(ofSize: 40); label.textColor = NSColor(white: 0.75, alpha: 1)
    label.frame = NSRect(x: 80, y: s.frame.height / 2, width: s.frame.width - 160, height: 120)
    let view = NSView(frame: NSRect(origin: .zero, size: s.frame.size)); view.addSubview(label); win.contentView = view
    win.orderFrontRegardless(); windows.append(win); coveredCount += 1
}
FileHandle.standardError.write("stresspattern: \(windows.count - coveredCount) of \(ids.count) displays, \(coveredCount) of \(covered.count) covered\n".data(using: .utf8)!)
var frameNo = 0
Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { _ in
    frameNo += 1
    CATransaction.begin(); CATransaction.setDisableActions(true)
    for (noise, counter, label) in animated {
        noise.contents = noiseImage(48, 32)
        counter.string = "\(label)  frame \(frameNo)"
    }
    CATransaction.commit()
}
app.run()
