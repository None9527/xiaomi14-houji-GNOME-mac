import AppKit
import AVFoundation

/// Shows the GNOME virtual monitor and turns local input into protocol
/// events.
///
/// Pointer, as in scrcpy: hovering over the picture moves GNOME's pointer
/// along (macOS draws GNOME's cursor sprite). A click captures the mouse:
/// the Mac cursor is frozen and hidden, motion goes to GNOME as relative
/// movement, so the pointer can go on to GNOME's other screens (the
/// phone's), and GNOME's cursor is drawn where GNOME reports it. Tapping
/// Option alone, or leaving the window, releases it.
final class StreamView: NSView {
    let displayLayer = CALayer()
    /// Encoded input messages, ready to send.
    var onInput: ((Data) -> Void)?
    /// Files dropped on the picture, to send to GNOME's Downloads.
    var onDropFiles: (([URL]) -> Void)?

    /// Size of the remote picture in pixels.
    var streamSize = CGSize(width: 16, height: 10) {
        didSet { window?.invalidateCursorRects(for: self) }
    }
    /// Called when the mouse is captured (true) or released.
    var onCaptureChanged: ((Bool) -> Void)?

    private enum PointerMode {
        case absolute
        case captured
    }

    private var mode = PointerMode.absolute
    private var remoteCursor: NSCursor = .arrow
    private var cursorHidden = false
    /// GNOME's cursor while captured: sprite, hotspot (points) and the last
    /// reported position (stream pixels).
    private let cursorView = NSImageView()
    private var cursorHotspot = CGPoint.zero
    private var remotePosition: CGPoint?
    /// Option went down with nothing else: its release toggles capture off.
    private var optionTap = false
    private var trackingArea: NSTrackingArea?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        displayLayer.contentsGravity = .resizeAspect
        displayLayer.minificationFilter = .linear
        displayLayer.backgroundColor = NSColor.black.cgColor
        registerForDraggedTypes([.fileURL])
        cursorView.imageScaling = .scaleNone
        cursorView.isHidden = true
        addSubview(cursorView)
    }

    // MARK: - Dropped files

    private func droppedFiles(_ info: NSDraggingInfo) -> [URL] {
        info.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                            options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        droppedFiles(sender).isEmpty ? [] : .copy
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let urls = droppedFiles(sender)
        guard !urls.isEmpty else { return false }
        onDropFiles?(urls)
        return true
    }

    required init?(coder: NSCoder) { fatalError() }

    override func makeBackingLayer() -> CALayer { displayLayer }
    override var acceptsFirstResponder: Bool { true }
    override var isFlipped: Bool { true }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        displayLayer.contentsScale = window?.backingScaleFactor ?? 2
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea { removeTrackingArea(trackingArea) }
        let area = NSTrackingArea(rect: bounds,
                                  options: [.mouseMoved, .mouseEnteredAndExited,
                                            .activeInKeyWindow, .cursorUpdate, .inVisibleRect],
                                  owner: self)
        addTrackingArea(area)
        trackingArea = area
    }

    /// Where the picture is drawn inside the view (aspect fit).
    var videoRect: CGRect {
        AVMakeRect(aspectRatio: streamSize, insideRect: bounds)
    }

    /// Stream pixels per view point.
    private var pixelsPerPoint: CGFloat {
        streamSize.width / max(videoRect.width, 1)
    }

    // MARK: - Cursor

    /// Updates the cursor from GNOME: position in stream pixels, sprite as
    /// premultiplied RGBA (nil keeps the current sprite).
    func updateRemoteCursor(x: Int, y: Int, rgba: Data?, width: Int, height: Int,
                            hotspotX: Int, hotspotY: Int, visible: Bool) {
        if let rgba, width > 0, height > 0,
           let provider = CGDataProvider(data: rgba as CFData),
           let image = CGImage(width: width, height: height, bitsPerComponent: 8,
                               bitsPerPixel: 32, bytesPerRow: width * 4,
                               space: CGColorSpace(name: CGColorSpace.sRGB)!,
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: true,
                               intent: .defaultIntent) {
            let scale = pixelsPerPoint
            let size = NSSize(width: CGFloat(width) / scale, height: CGFloat(height) / scale)
            let sprite = NSImage(cgImage: image, size: size)
            cursorHotspot = CGPoint(x: CGFloat(hotspotX) / scale, y: CGFloat(hotspotY) / scale)
            remoteCursor = NSCursor(image: sprite, hotSpot: NSPoint(x: cursorHotspot.x, y: cursorHotspot.y))
            cursorView.image = sprite
            cursorView.frame.size = size
        }
        cursorHidden = !visible
        window?.invalidateCursorRects(for: self)
        // Off this monitor (on the phone's screen, say): nothing to draw here.
        let inside = visible && x >= 0 && y >= 0 &&
            CGFloat(x) < streamSize.width && CGFloat(y) < streamSize.height
        remotePosition = inside ? CGPoint(x: x, y: y) : nil

        if case .captured = mode {
            placeCursorView()
        } else if isMouseInside {
            applyCursor()
        }
    }

    private func placeCursorView() {
        guard case .captured = mode, let p = remotePosition else {
            cursorView.isHidden = true
            return
        }
        let r = videoRect
        cursorView.frame.origin = CGPoint(x: r.minX + p.x / pixelsPerPoint - cursorHotspot.x,
                                          y: r.minY + p.y / pixelsPerPoint - cursorHotspot.y)
        cursorView.isHidden = false
    }

    private var isMouseInside: Bool {
        guard let window else { return false }
        let p = convert(window.mouseLocationOutsideOfEventStream, from: nil)
        return bounds.contains(p)
    }

    private static let invisibleCursor: NSCursor = {
        let image = NSImage(size: NSSize(width: 1, height: 1))
        return NSCursor(image: image, hotSpot: .zero)
    }()

    private func applyCursor() {
        (cursorHidden ? Self.invisibleCursor : remoteCursor).set()
    }

    override func cursorUpdate(with event: NSEvent) { applyCursor() }
    override func resetCursorRects() {
        addCursorRect(videoRect, cursor: cursorHidden ? Self.invisibleCursor : remoteCursor)
    }

    // MARK: - Pointer ownership

    private func streamPoint(_ viewPoint: CGPoint) -> CGPoint {
        let r = videoRect
        let x = min(max((viewPoint.x - r.minX) / r.width, 0), 1) * streamSize.width
        let y = min(max((viewPoint.y - r.minY) / r.height, 0), 1) * streamSize.height
        return CGPoint(x: x, y: y)
    }

    private func capture() {
        guard case .absolute = mode else { return }
        mode = .captured
        CGAssociateMouseAndMouseCursorPosition(0)
        NSCursor.hide()
        placeCursorView()
        onCaptureChanged?(true)
    }

    /// Hands the pointer back to macOS, at GNOME's pointer when that is on
    /// this monitor.
    func release() {
        guard case .captured = mode else { return }
        mode = .absolute
        if let p = remotePosition, let window {
            let r = videoRect
            let viewPoint = CGPoint(x: r.minX + p.x / pixelsPerPoint, y: r.minY + p.y / pixelsPerPoint)
            let screenPoint = window.convertPoint(toScreen: convert(viewPoint, to: nil))
            // CoreGraphics global coordinates have their origin at the top
            // left of the main display.
            let mainHeight = NSScreen.screens.first?.frame.height ?? 0
            CGWarpMouseCursorPosition(CGPoint(x: screenPoint.x, y: mainHeight - screenPoint.y))
        }
        CGAssociateMouseAndMouseCursorPosition(1)
        NSCursor.unhide()
        cursorView.isHidden = true
        applyCursor()
        onCaptureChanged?(false)
    }

    private func handleMotion(_ event: NSEvent) {
        optionTap = false
        if case .captured = mode {
            let scale = pixelsPerPoint
            onInput?(Message.input(.pointerRelative,
                                   x: Float(event.deltaX * scale), y: Float(event.deltaY * scale)))
            return
        }
        let p = streamPoint(convert(event.locationInWindow, from: nil))
        onInput?(Message.input(.pointerAbsolute, x: Float(p.x), y: Float(p.y)))
    }

    // evdev BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA
    private static func button(_ number: Int) -> UInt32 {
        switch number {
        case 0: return 0x110
        case 1: return 0x111
        case 2: return 0x112
        case 3: return 0x113
        default: return 0x114
        }
    }

    private func sendButton(_ event: NSEvent, down: Bool) {
        optionTap = false
        if case .absolute = mode {
            handleMotion(event)
            if down { capture() }
        }
        onInput?(Message.input(.button, code: Self.button(event.buttonNumber), value: down ? 1 : 0))
    }

    override func mouseMoved(with event: NSEvent) { handleMotion(event) }
    override func mouseDragged(with event: NSEvent) { handleMotion(event) }
    override func rightMouseDragged(with event: NSEvent) { handleMotion(event) }
    override func otherMouseDragged(with event: NSEvent) { handleMotion(event) }
    override func mouseDown(with event: NSEvent) { sendButton(event, down: true) }
    override func mouseUp(with event: NSEvent) { sendButton(event, down: false) }
    override func rightMouseDown(with event: NSEvent) { sendButton(event, down: true) }
    override func rightMouseUp(with event: NSEvent) { sendButton(event, down: false) }
    override func otherMouseDown(with event: NSEvent) { sendButton(event, down: true) }
    override func otherMouseUp(with event: NSEvent) { sendButton(event, down: false) }

    override func scrollWheel(with event: NSEvent) {
        if event.hasPreciseScrollingDeltas {
            // Trackpad: smooth deltas in points; GNOME expects the opposite
            // sign of macOS's natural scrolling.
            let scale = pixelsPerPoint
            onInput?(Message.input(.scroll,
                                   x: Float(-event.scrollingDeltaX * scale),
                                   y: Float(-event.scrollingDeltaY * scale)))
        } else {
            if event.scrollingDeltaY != 0 {
                onInput?(Message.input(.scrollDiscrete, code: 0,
                                       value: event.scrollingDeltaY > 0 ? -1 : 1))
            }
            if event.scrollingDeltaX != 0 {
                onInput?(Message.input(.scrollDiscrete, code: 1,
                                       value: event.scrollingDeltaX > 0 ? -1 : 1))
            }
        }
    }

    // MARK: - Keyboard

    override func keyDown(with event: NSEvent) {
        optionTap = false
        guard !event.isARepeat, let code = KeyMap.table[event.keyCode] else { return }
        onInput?(Message.input(.key, code: code, value: 1))
    }

    override func keyUp(with event: NSEvent) {
        guard let code = KeyMap.table[event.keyCode] else { return }
        onInput?(Message.input(.key, code: code, value: 0))
    }

    override func flagsChanged(with event: NSEvent) {
        // A tap of Option alone releases the mouse (scrcpy's way).
        let mods = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        if event.keyCode == 0x3A || event.keyCode == 0x3D {
            if mods == .option {
                optionTap = true
            } else if mods.isEmpty, optionTap {
                optionTap = false
                release()
            }
        } else {
            optionTap = false
        }
        guard let code = KeyMap.table[event.keyCode],
              let flag = KeyMap.modifierFlag[event.keyCode] else { return }
        let down = event.modifierFlags.rawValue & flag != 0
        if event.keyCode == 0x39 {
            // Caps Lock reports state, not key travel: tap it.
            onInput?(Message.input(.key, code: code, value: 1))
            onInput?(Message.input(.key, code: code, value: 0))
        } else {
            onInput?(Message.input(.key, code: code, value: down ? 1 : 0))
        }
    }

    /// Keep Cmd-key shortcuts for the remote desktop instead of the menu.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard window?.firstResponder === self else { return false }
        if event.type == .keyDown, event.modifierFlags.contains(.command),
           event.charactersIgnoringModifiers == "q" {
            return false // Cmd-Q still quits this app
        }
        // macOS sends no keyUp for keys pressed while Command is held, so a
        // shortcut is a complete tap.
        if let code = KeyMap.table[event.keyCode], event.type == .keyDown, !event.isARepeat {
            onInput?(Message.input(.key, code: code, value: 1))
            onInput?(Message.input(.key, code: code, value: 0))
        }
        return true
    }
}
