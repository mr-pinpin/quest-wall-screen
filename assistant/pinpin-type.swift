// pinpin-type           reads UTF-8 text from stdin and types it into the focused window (Unicode, no keymap).
// pinpin-type --key <combo>   presses a combo, e.g. return, tab, esc, cmd+l, cmd+t, cmd+w, cmd+a.
// Keyboard events reach whatever window has focus; the assistant places the target screen's window in focus
// first. Needs Accessibility, inherited as a child of PinPin Host.
import Foundation
import CoreGraphics
import ApplicationServices

func typeUnicode(_ s: String) {
    let src = CGEventSource(stateID: .combinedSessionState)
    for ch in s.unicodeScalars {
        var u = UniChar(ch.value > 0xFFFF ? 0xFFFD : UInt16(ch.value))  // BMP; rare astral -> replacement
        for down in [true, false] {
            let e = CGEvent(keyboardEventSource: src, virtualKey: 0, keyDown: down)
            e?.keyboardSetUnicodeString(stringLength: 1, unicodeString: &u)
            e?.post(tap: .cghidEventTap)
        }
        usleep(1500)
    }
}

// Minimal keycode map for combos we actually use.
let KEY: [String: CGKeyCode] = [
    "return": 36, "enter": 36, "tab": 48, "space": 49, "esc": 53, "escape": 53, "delete": 51,
    "left": 123, "right": 124, "down": 125, "up": 126,
    "a": 0, "c": 8, "v": 9, "l": 37, "t": 17, "w": 13, "r": 15, "n": 45, "f": 3,
]

func pressCombo(_ combo: String) {
    var flags = CGEventFlags()
    var key: CGKeyCode? = nil
    for part in combo.lowercased().split(separator: "+").map({ String($0) }) {
        switch part {
        case "cmd", "command": flags.insert(.maskCommand)
        case "shift": flags.insert(.maskShift)
        case "opt", "option", "alt": flags.insert(.maskAlternate)
        case "ctrl", "control": flags.insert(.maskControl)
        default: key = KEY[part]
        }
    }
    guard let k = key else {
        FileHandle.standardError.write("unknown key in combo \(combo)\n".data(using: .utf8)!); exit(2)
    }
    let src = CGEventSource(stateID: .combinedSessionState)
    for down in [true, false] {
        let e = CGEvent(keyboardEventSource: src, virtualKey: k, keyDown: down)
        e?.flags = flags
        e?.post(tap: .cghidEventTap)
        usleep(2000)
    }
}

guard AXIsProcessTrusted() else {
    FileHandle.standardError.write("no Accessibility permission\n".data(using: .utf8)!); exit(3)
}
let a = CommandLine.arguments
if a.count >= 3 && a[1] == "--key" {
    pressCombo(a[2])
} else {
    let data = FileHandle.standardInput.readDataToEndOfFile()
    typeUnicode(String(data: data, encoding: .utf8) ?? "")
}
