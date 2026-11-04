// pinpin-screencap <displayID>  -> writes a PNG of exactly that display to stdout.
// Used by the assistant's screen MCP server to let the model see one virtual display. Needs Screen Recording,
// which it inherits as a child of PinPin Host. One display only: it never captures the whole desktop.
// Uses ScreenCaptureKit (CGDisplayCreateImage was obsoleted in macOS 15).
import Foundation
import ScreenCaptureKit
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count >= 2, let did = UInt32(args[1]) else {
    FileHandle.standardError.write("usage: pinpin-screencap <displayID>\n".data(using: .utf8)!)
    exit(2)
}
guard CGPreflightScreenCaptureAccess() else {
    FileHandle.standardError.write("no Screen Recording permission\n".data(using: .utf8)!)
    exit(3)
}

func err(_ s: String) -> Never {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(4)
}

let sem = DispatchSemaphore(value: 0)
var image: CGImage?
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let disp = content.displays.first(where: { $0.displayID == CGDirectDisplayID(did) }) else {
            err("no SCDisplay for display \(did)")
        }
        let filter = SCContentFilter(display: disp, excludingWindows: [])
        let cfg = SCStreamConfiguration()
        cfg.width = disp.width        // points; a 1x grab is plenty for the model to read layout
        cfg.height = disp.height
        cfg.showsCursor = false
        image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg)
    } catch {
        err("capture error: \(error)")
    }
    sem.signal()
}
sem.wait()
guard let img = image else { err("no image") }
let data = NSMutableData()
guard let dst = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else { exit(5) }
CGImageDestinationAddImage(dst, img, nil)
guard CGImageDestinationFinalize(dst) else { exit(6) }
FileHandle.standardOutput.write(data as Data)
