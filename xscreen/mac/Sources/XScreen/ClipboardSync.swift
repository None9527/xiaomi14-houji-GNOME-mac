import AppKit

/// Keeps the Mac's clipboard and GNOME's in step: text, images and files.
///
/// macOS has no change notification for the pasteboard, so its change count
/// is polled; what this class writes itself is not sent back. Copied files
/// travel as a file transfer (up to `XS.fileClipboardMax`) and are put on
/// the other side's clipboard once they have arrived.
final class ClipboardSync {
    /// Sends a clipboard message (main queue).
    var sendMessage: ((Data) -> Void)?
    /// Sends copied files (main queue).
    var sendFiles: (([URL]) -> Void)?

    private let pasteboard = NSPasteboard.general
    private var seen: Int
    private var timer: Timer?

    init() {
        seen = pasteboard.changeCount
    }

    /// Starts following the clipboard; what is on it already stays local.
    func start() {
        seen = pasteboard.changeCount
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in
            self?.poll()
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func poll() {
        guard pasteboard.changeCount != seen else { return }
        seen = pasteboard.changeCount

        if let urls = pasteboard.readObjects(forClasses: [NSURL.self],
                                             options: [.urlReadingFileURLsOnly: true]) as? [URL],
           !urls.isEmpty {
            if FileSender.measure(urls) <= XS.fileClipboardMax {
                sendFiles?(urls)
            }
            return
        }
        if let png = imagePNG() {
            if png.count <= XS.clipboardMax { sendMessage?(Message.clipboard(.image, png)) }
            return
        }
        if let text = pasteboard.string(forType: .string) {
            let data = Data(text.utf8)
            if data.count <= XS.clipboardMax { sendMessage?(Message.clipboard(.text, data)) }
        }
    }

    /// An image on the pasteboard as PNG (screenshots arrive as TIFF or PNG).
    private func imagePNG() -> Data? {
        if let png = pasteboard.data(forType: .png) { return png }
        guard let tiff = pasteboard.data(forType: .tiff),
              let rep = NSBitmapImageRep(data: tiff) else { return nil }
        return rep.representation(using: .png, properties: [:])
    }

    // MARK: - From GNOME (main queue)

    func apply(format: XS.ClipboardFormat, content: Data) {
        pasteboard.clearContents()
        switch format {
        case .text:
            pasteboard.setString(String(decoding: content, as: UTF8.self), forType: .string)
        case .image:
            pasteboard.declareTypes([.png, .tiff], owner: nil)
            pasteboard.setData(content, forType: .png)
            if let tiff = NSImage(data: content)?.tiffRepresentation {
                pasteboard.setData(tiff, forType: .tiff)
            }
        }
        seen = pasteboard.changeCount
    }

    func apply(files: [URL]) {
        pasteboard.clearContents()
        pasteboard.writeObjects(files as [NSURL])
        seen = pasteboard.changeCount
    }
}
