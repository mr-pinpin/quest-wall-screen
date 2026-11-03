// PinPin Stress Pattern: fills the given displays (CGDirectDisplayIDs as arguments) with moving content that
// encodes like real use: sliding bars (motion), scrolling text (detail) and a noise block (video-like entropy).
// Launched by bench/stress.py through `open`, so its windows live in the GUI session; quit with pkill.
import AppKit

final class PatternView: NSView {
    let label: String; let t0 = CACurrentMediaTime(); var frameNo = 0
    init(frame: NSRect, label: String) { self.label = label; super.init(frame: frame) }
    required init?(coder: NSCoder) { fatalError() }
    override func draw(_ dirty: NSRect) {
        let t = CACurrentMediaTime() - t0, w = bounds.width, h = bounds.height
        frameNo += 1
        NSColor(hue: CGFloat((t * 0.03).truncatingRemainder(dividingBy: 1)), saturation: 0.4, brightness: 0.3, alpha: 1).setFill()
        bounds.fill()
        for i in 0..<10 {  // bars sliding across: global motion
            let x = (CGFloat(t) * 300 + CGFloat(i) * w / 10).truncatingRemainder(dividingBy: w)
            NSColor(white: i % 2 == 0 ? 0.9 : 0.1, alpha: 0.7).setFill()
            NSRect(x: x, y: 0, width: 36, height: h).fill()
        }
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedSystemFont(ofSize: 22, weight: .regular), .foregroundColor: NSColor.white]
        let scroll = CGFloat(t * 90).truncatingRemainder(dividingBy: 30)
        for row in 0..<Int(h / 30) {  // scrolling text: fine detail that must stay legible
            let line = "\(label)  line \(row + Int(t * 3))  frame \(frameNo)  the quick brown fox jumps over the lazy dog 0123456789"
            (line as NSString).draw(at: NSPoint(x: 60, y: CGFloat(row) * 30 + scroll), withAttributes: attrs)
        }
        let bx = w * 0.55, by = h * 0.25, cell: CGFloat = 10  // noise block: video-like entropy
        for y in 0..<32 { for x in 0..<48 {
            NSColor(white: CGFloat.random(in: 0...1), alpha: 1).setFill()
            NSRect(x: bx + CGFloat(x) * cell, y: by + CGFloat(y) * cell, width: cell, height: cell).fill()
        } }
        ("\(label)" as NSString).draw(at: NSPoint(x: 60, y: h - 120), withAttributes: [.font: NSFont.boldSystemFont(ofSize: 90), .foregroundColor: NSColor.systemYellow])
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let ids = CommandLine.arguments.dropFirst().compactMap { UInt32($0) }
var windows: [NSWindow] = []
for s in NSScreen.screens {
    guard let n = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber, ids.contains(n.uint32Value) else { continue }
    let win = NSWindow(contentRect: s.frame, styleMask: .borderless, backing: .buffered, defer: false)
    win.setFrame(s.frame, display: true)
    win.contentView = PatternView(frame: NSRect(origin: .zero, size: s.frame.size), label: "STRESS \(n.uint32Value)")
    win.orderFrontRegardless(); windows.append(win)
}
FileHandle.standardError.write("stresspattern: \(windows.count) of \(ids.count) displays\n".data(using: .utf8)!)
Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { _ in windows.forEach { $0.contentView?.needsDisplay = true } }
app.run()
