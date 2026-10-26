import AppKit

/// Shows the phone's GNOME virtual monitor in a window. GNOME owns the
/// monitor's configuration (GNOME Settings: resolution, scale, position);
/// this app describes the Mac display it runs on and shows the picture at
/// its aspect ratio.
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var view: StreamView!
    private var renderer: VideoRenderer!
    private var connection: Connection?
    private var lastKeyframeRequest = Date.distantPast
    private var frameStats: FrameStats?
    private let audio = AudioPlayer()
    private let clipboard = ClipboardSync()
    private var fileSender: FileSender?
    private let fileReceiver = FileReceiver()
    private lazy var notifier = FileNotifier()
    /// The display GNOME was last told about (its UUID).
    private var describedDisplay: String?
    private var screenChange: DispatchWorkItem?
    /// Keeps macOS from napping or coalescing timers while the desktop is
    /// shown: input and frames must never wait for a background slot.
    private var activity: NSObjectProtocol?
    private var inputsSent = 0   // debug: input events since the last report

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Start on the display last used, else on the largest external one
        // (this is an extra screen), else on the main one.
        let lastUsed = UserDefaults.standard.string(forKey: Self.displayKey)
        let screen = NSScreen.screens.first { Self.displayUUID(Self.displayID(of: $0)) == lastUsed }
            ?? NSScreen.screens.filter { !Self.isBuiltin($0) }
                .max { $0.frame.width * $0.frame.height < $1.frame.width * $1.frame.height }
            ?? NSScreen.main ?? NSScreen.screens[0]
        let visible = screen.visibleFrame
        let size = Self.windowedSize(on: screen)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: size.width, height: size.height),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "GNOME"
        window.collectionBehavior = [.fullScreenPrimary]
        window.delegate = self
        var frame = window.frame
        frame.origin = CGPoint(x: (visible.midX - frame.width / 2).rounded(),
                               y: (visible.midY - frame.height / 2).rounded())
        window.setFrame(frame, display: false)

        view = StreamView(frame: window.contentView!.bounds)
        view.autoresizingMask = [.width, .height]
        window.contentView = view
        renderer = VideoRenderer(layer: view.displayLayer)
        renderer.onNeedKeyframe = { [weak self] in self?.requestKeyframe() }
        view.onInput = { [weak self] data in
            self?.inputsSent += 1
            self?.connection?.send(data)
        }
        view.onDropFiles = { [weak self] urls in self?.fileSender?.send(urls, purpose: .downloads) }
        view.onCaptureChanged = { [weak self] captured in
            guard let self else { return }
            self.window.subtitle = captured ? "按 ⌥ 释放鼠标" : ""
        }
        clipboard.sendMessage = { [weak self] data in self?.connection?.send(data) }
        clipboard.sendFiles = { [weak self] urls in self?.fileSender?.send(urls, purpose: .clipboard) }
        fileReceiver.onReceived = { [weak self] purpose, urls in
            guard let self else { return }
            if purpose == .clipboard {
                self.clipboard.apply(files: urls)
            } else {
                self.notifier.notifyReceived(urls)
            }
        }

        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        NSApp.activate(ignoringOtherApps: true)

        activity = ProcessInfo.processInfo.beginActivity(
            options: [.userInitiated, .latencyCritical],
            reason: "Showing the GNOME desktop")
        connect()

        // Debug: XSCREEN_DEBUG=1 logs decode stats; XSCREEN_SNAPSHOT=path
        // saves the displayed picture every 5 s.
        let env = ProcessInfo.processInfo.environment
        if env["XSCREEN_DEBUG"] != nil || env["XSCREEN_SNAPSHOT"] != nil {
            frameStats = FrameStats()
            Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
                guard let self else { return }
                let key = self.window.isKeyWindow ? "key" : "not key"
                FileHandle.standardError.write(("xscreen: \(self.renderer.statusDescription), " +
                    "\(self.inputsSent) input events sent, window \(key), " +
                    "frontmost \(NSWorkspace.shared.frontmostApplication?.localizedName ?? "?")\n").data(using: .utf8)!)
                self.inputsSent = 0
                if let pacing = self.frameStats?.drain() {
                    FileHandle.standardError.write("xscreen: \(pacing)\n".data(using: .utf8)!)
                }
                if let path = env["XSCREEN_SNAPSHOT"] {
                    _ = self.renderer.snapshot(to: URL(fileURLWithPath: path))
                }
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    // MARK: - Connection

    private static func adbForward() {
        let adb = ["/opt/homebrew/bin/adb", "/usr/local/bin/adb"]
            .first { FileManager.default.isExecutableFile(atPath: $0) }
        guard let adb else { return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: adb)
        process.arguments = ["forward", "tcp:\(XS.defaultPort)", "tcp:\(XS.defaultPort)"]
        try? process.run()
        process.waitUntilExit()
    }

    /// UserDefaults key: UUID of the display last shown on.
    private static let displayKey = "display"

    /// The screen GNOME was told about, while it is attached.
    private var describedScreen: NSScreen? {
        NSScreen.screens.first { Self.displayUUID(Self.displayID(of: $0)) == describedDisplay }
    }

    private static func displayID(of screen: NSScreen) -> CGDirectDisplayID {
        let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber
        return CGDirectDisplayID(number?.uint32Value ?? CGMainDisplayID())
    }

    private static func isBuiltin(_ screen: NSScreen) -> Bool {
        CGDisplayIsBuiltin(displayID(of: screen)) != 0
    }

    /// A window at 80% of the screen with the screen's shape, in points.
    private static func windowedSize(on screen: NSScreen) -> CGSize {
        let visible = screen.visibleFrame
        let aspect = screen.frame.width / screen.frame.height
        var width = (visible.width * 0.8).rounded()
        var height = (width / aspect).rounded()
        if height > visible.height * 0.8 {
            height = (visible.height * 0.8).rounded()
            width = (height * aspect).rounded()
        }
        return CGSize(width: width, height: height)
    }

    private static func displayUUID(_ displayID: CGDirectDisplayID) -> String {
        if let ref = CGDisplayCreateUUIDFromDisplayID(displayID)?.takeRetainedValue(),
           let string = CFUUIDCreateString(nil, ref) as String? {
            return string
        }
        return "display-\(displayID)"
    }

    private var windowDisplayUUID: String? {
        guard let number = window.screen?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")]
                as? NSNumber else { return nil }
        return Self.displayUUID(CGDirectDisplayID(number.uint32Value))
    }

    /// The display the window is on: pixel size of its current mode, refresh
    /// rate, HiDPI scale and a stable identity. An external display becomes
    /// a GNOME monitor of its own (the phone proposes the whole display);
    /// on the built-in one GNOME gets a window's worth. Derived from the
    /// display alone, so any Mac and display is set up the same way.
    private func displayHello() -> Data {
        let screen = window.screen ?? NSScreen.main ?? NSScreen.screens[0]
        let displayID = Self.displayID(of: screen)
        var width = Int(screen.frame.width * screen.backingScaleFactor)
        var height = Int(screen.frame.height * screen.backingScaleFactor)
        if let mode = CGDisplayCopyDisplayMode(displayID) {
            width = mode.pixelWidth
            height = mode.pixelHeight
        }
        let uuid = Self.displayUUID(displayID)
        describedDisplay = uuid
        UserDefaults.standard.set(uuid, forKey: Self.displayKey)
        let windowed = Self.isBuiltin(screen) ? Self.windowedSize(on: screen) : .zero
        return Message.clientHello(width: width, height: height,
                                   maxFps: screen.maximumFramesPerSecond,
                                   scalePercent: Int(screen.backingScaleFactor * 100),
                                   windowWidth: Int(windowed.width * screen.backingScaleFactor),
                                   windowHeight: Int(windowed.height * screen.backingScaleFactor),
                                   displayID: uuid, displayName: screen.localizedName)
    }

    /// Shows the picture 1:1: the mode chosen in GNOME Settings decides the
    /// window size. The whole display's mode means full screen; a mode too
    /// large for a window is scaled down, keeping its shape.
    private func adoptStreamSize(_ size: CGSize) {
        if ProcessInfo.processInfo.environment["XSCREEN_DEBUG"] != nil {
            let screen = window.screen
            FileHandle.standardError.write(("xscreen: stream \(size) on screen \(screen?.localizedName ?? "nil") " +
                "frame \(screen?.frame ?? .zero) scale \(screen?.backingScaleFactor ?? 0) " +
                "window \(window.frame) fullscreen \(window.styleMask.contains(.fullScreen))\n").data(using: .utf8)!)
        }
        view.streamSize = size
        window.contentAspectRatio = size
        // The display GNOME's monitor stands for: the window stays on it.
        guard let screen = describedScreen ?? window.screen ?? NSScreen.main else { return }
        let scale = screen.backingScaleFactor
        let points = CGSize(width: size.width / scale, height: size.height / scale)
        // The phone aligns widths to 8 and heights to 2 pixels, so the
        // display's own mode may come back a few pixels smaller.
        let slack = 8 / scale + 0.5
        // The display's own size, or on an external display a mode of its
        // shape and most of its size (the phone caps it at 4K-class).
        let sameShape = abs(size.width / size.height - screen.frame.width / screen.frame.height) < 0.01
        let fillsScreen = (abs(points.width - screen.frame.width) < slack &&
                           abs(points.height - screen.frame.height) < slack) ||
                          (!Self.isBuiltin(screen) && sameShape && points.width >= screen.frame.width * 0.7)
        let isFullScreen = window.styleMask.contains(.fullScreen)

        if fillsScreen != isFullScreen {
            // The frame is set once the transition ends (windowDidExitFullScreen).
            window.toggleFullScreen(nil)
            return
        }
        if fillsScreen { return }

        let visible = screen.visibleFrame
        let chrome = window.frame.height - window.contentRect(forFrameRect: window.frame).height
        let fit = min(1, visible.width / points.width, (visible.height - chrome) / points.height)
        let content = CGSize(width: (points.width * fit).rounded(), height: (points.height * fit).rounded())
        var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: content))
        frame.origin = CGPoint(x: min(max(window.frame.minX, visible.minX), visible.maxX - frame.width),
                               y: min(max(window.frame.maxY - frame.height, visible.minY),
                                      visible.maxY - frame.height))
        window.setFrame(frame, display: true, animate: false)
    }

    /// Re-creates the adb forward before every attempt: adb drops forwards
    /// whenever the phone reconnects over USB.
    private func connect() {
        DispatchQueue.global(qos: .userInitiated).async {
            Self.adbForward()
            DispatchQueue.main.async { self.startConnection() }
        }
    }

    private func startConnection() {
        let connection = Connection(port: XS.defaultPort)
        self.connection = connection
        window.title = "GNOME — connecting"

        connection.onMessage = { [weak self] type, payload in
            self?.handle(type, payload)
        }
        connection.onClosed = { [weak self] reason in
            self?.fileReceiver.abandon()
            DispatchQueue.main.async {
                guard let self, self.connection === connection else { return }
                self.stopSession()
                self.window.title = "GNOME — \(reason), retrying"
                self.connection = nil
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.connect() }
            }
        }
        connection.start()
    }

    /// Runs on the connection queue.
    private func handle(_ type: XS.Message, _ payload: Data) {
        switch type {
        case .serverHello:
            DispatchQueue.main.async {
                guard let connection = self.connection else { return }
                connection.send(self.displayHello())
                self.fileSender = FileSender { [weak connection] data, done in
                    guard let connection else { return done() }
                    connection.send(data, completion: done)
                }
                self.clipboard.start()
            }

        case .clipboard:
            var r = ByteReader(payload)
            guard payload.count >= 4, let format = XS.ClipboardFormat(rawValue: r.u32()) else { break }
            let content = r.remaining
            DispatchQueue.main.async { self.clipboard.apply(format: format, content: content) }

        case .audio:
            guard payload.count >= 16 else { break }
            var r = ByteReader(payload)
            _ = r.i64()
            let rate = Int(r.u32()), channels = Int(r.u16())
            _ = r.u16()
            audio.push(r.remaining, rate: rate, channels: channels)

        case .fileBegin, .fileData, .fileEnd:
            if !fileReceiver.handle(type, payload) { connection?.cancel() }

        case .videoConfig:
            var r = ByteReader(payload)
            _ = r.u32()
            let width = CGFloat(r.u32()), height = CGFloat(r.u32())
            renderer.reset()
            DispatchQueue.main.async {
                self.adoptStreamSize(CGSize(width: width, height: height))
                self.window.title = "GNOME — \(Int(width))×\(Int(height))"
            }

        case .videoFrame:
            var r = ByteReader(payload)
            let pts = r.i64()
            let flags = r.u32()
            // Keyframes come when the picture is still (or on demand) and are
            // large; their transit time is not motion pacing.
            if flags & XS.frameKeyframe == 0 { frameStats?.record(ptsUs: pts) }
            renderer.enqueue(accessUnit: r.remaining, keyframe: flags & XS.frameKeyframe != 0, ptsUs: pts)

        case .cursor:
            var r = ByteReader(payload)
            let x = Int(r.i32()), y = Int(r.i32())
            let hx = Int(r.i32()), hy = Int(r.i32())
            let visible = r.u32() != 0
            let w = Int(r.u32()), h = Int(r.u32())
            let bitmap = w > 0 && h > 0 ? r.remaining : nil
            DispatchQueue.main.async {
                self.view.updateRemoteCursor(x: x, y: y, rgba: bitmap, width: w, height: h,
                                             hotspotX: hx, hotspotY: hy, visible: visible)
            }

        default:
            break
        }
    }

    private func stopSession() {
        clipboard.stop()
        fileSender?.cancel()
        fileSender = nil
        audio.stop()
    }

    private func requestKeyframe() {
        guard Date().timeIntervalSince(lastKeyframeRequest) > 0.5 else { return }
        lastKeyframeRequest = Date()
        connection?.send(Message.encode(.requestKeyframe))
    }

    // MARK: - Window

    func windowDidResignKey(_ notification: Notification) {
        view.release()
    }

    /// Moved to another Mac display: GNOME's monitor takes on that display
    /// (its identity, sizes and refresh rate), through a new session.
    func windowDidChangeScreen(_ notification: Notification) {
        screenChange?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, let connection = self.connection, let now = self.windowDisplayUUID,
                  self.describedDisplay != nil, now != self.describedDisplay else { return }
            self.connection = nil
            connection.queue.async { self.fileReceiver.abandon() }
            connection.cancel()
            self.stopSession()
            self.describedDisplay = nil
            self.connect()
        }
        screenChange = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5, execute: work)
    }

    func windowDidExitFullScreen(_ notification: Notification) {
        adoptStreamSize(view.streamSize)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
