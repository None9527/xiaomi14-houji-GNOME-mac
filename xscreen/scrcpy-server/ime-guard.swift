// xscreen-ime-guard LAUNCHER_PID
//
// While the scrcpy window is in front, selects a plain keyboard layout, so a
// Mac input method (Sogou, Pinyin...) does not take the keys meant for
// GNOME's own input method; puts the user's input source back whenever
// another app comes to the front, and when the launcher goes away.
import AppKit
import Carbon

guard CommandLine.arguments.count == 2, let launcher = pid_t(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: xscreen-ime-guard LAUNCHER_PID\n".data(using: .utf8)!)
    exit(2)
}

var saved: TISInputSource?

func update(_ app: NSRunningApplication?) {
    if app?.executableURL?.lastPathComponent == "scrcpy" {
        guard saved == nil else { return }
        saved = TISCopyCurrentKeyboardInputSource().takeRetainedValue()
        TISSelectInputSource(TISCopyCurrentASCIICapableKeyboardLayoutInputSource().takeRetainedValue())
    } else if let source = saved {
        TISSelectInputSource(source)
        saved = nil
    }
}

func finish() -> Never {
    update(nil)
    exit(0)
}

NSWorkspace.shared.notificationCenter.addObserver(
    forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
) { note in
    update(note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication)
}

var sources: [DispatchSourceProtocol] = []
for sig in [SIGINT, SIGTERM, SIGHUP] {
    signal(sig, SIG_IGN)
    let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
    source.setEventHandler { finish() }
    source.resume()
    sources.append(source)
}
let exitWatch = DispatchSource.makeProcessSource(identifier: launcher, eventMask: .exit, queue: .main)
exitWatch.setEventHandler { finish() }
exitWatch.resume()
sources.append(exitWatch)
if kill(launcher, 0) != 0 { finish() }

update(NSWorkspace.shared.frontmostApplication)
RunLoop.main.run()
