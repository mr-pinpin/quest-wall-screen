// setmode <displayID> [width height]  — list modes, or switch to a 1x mode of that size.
import CoreGraphics
import Foundation
let a = CommandLine.arguments
guard a.count >= 2, let id = UInt32(a[1]) else { print("usage: setmode id [w h]"); exit(2) }
let opts = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
let modes = (CGDisplayCopyAllDisplayModes(id, opts) as? [CGDisplayMode]) ?? []
if a.count < 4 {
  for m in modes { print("\(m.width)x\(m.height) px \(m.pixelWidth)x\(m.pixelHeight) \(m.refreshRate)Hz") }
  if let c = CGDisplayCopyDisplayMode(id) { print("current \(c.width)x\(c.height) px \(c.pixelWidth)x\(c.pixelHeight)") }
  exit(0)
}
let w = Int(a[2])!, h = Int(a[3])!
guard let m = modes.first(where: { $0.width == w && $0.height == h && $0.pixelWidth == w }) else { print("no 1x \(w)x\(h) mode"); exit(1) }
var cfg: CGDisplayConfigRef?
CGBeginDisplayConfiguration(&cfg)
CGConfigureDisplayWithDisplayMode(cfg, id, m, nil)
let r = CGCompleteDisplayConfiguration(cfg, .permanently)
print("set \(id) -> \(w)x\(h) 1x: \(r.rawValue)")
exit(r == .success ? 0 : 1)
