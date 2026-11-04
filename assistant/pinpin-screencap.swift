// pinpin-screencap <displayID>  -> writes a PNG of exactly that display to stdout.
// Used by the assistant's screen MCP server to let the model see one virtual display. Needs Screen Recording,
// which it inherits as a child of PinPin Host. One display only: it never captures the whole desktop.
import Foundation
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
guard let img = CGDisplayCreateImage(CGDirectDisplayID(did)) else {
    FileHandle.standardError.write("CGDisplayCreateImage failed for display \(did)\n".data(using: .utf8)!)
    exit(4)
}
let data = NSMutableData()
guard let dst = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else { exit(5) }
CGImageDestinationAddImage(dst, img, nil)
guard CGImageDestinationFinalize(dst) else { exit(6) }
FileHandle.standardOutput.write(data as Data)
