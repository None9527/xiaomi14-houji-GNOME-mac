import AppKit
import UserNotifications

/// Sends local files and folders to GNOME, one transfer at a time, a few
/// chunks in flight so input and video keep flowing alongside.
final class FileSender {
    private let queue = DispatchQueue(label: "xscreen.files.send", qos: .utility)
    private var nextTransfer: UInt32 = 0
    private var cancelled = false
    /// Sends one message; `completion` runs once it is on its way.
    private let send: (Data, @escaping () -> Void) -> Void

    init(send: @escaping (Data, @escaping () -> Void) -> Void) {
        self.send = send
    }

    func cancel() {
        queue.async { self.cancelled = true }
    }

    /// Total size of regular files below `urls`, without following links to
    /// folders.
    static func measure(_ urls: [URL]) -> UInt64 {
        var total: UInt64 = 0
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey]
        for url in urls {
            if let values = try? url.resourceValues(forKeys: Set(keys)), values.isRegularFile == true {
                total += UInt64(values.fileSize ?? 0)
                continue
            }
            let walker = FileManager.default.enumerator(at: url, includingPropertiesForKeys: keys)
            while let child = walker?.nextObject() as? URL {
                if let values = try? child.resourceValues(forKeys: Set(keys)), values.isRegularFile == true {
                    total += UInt64(values.fileSize ?? 0)
                }
            }
        }
        return total
    }

    func send(_ urls: [URL], purpose: XS.FilePurpose) {
        queue.async {
            self.cancelled = false
            self.nextTransfer &+= 1
            let transfer = self.nextTransfer
            var ok = true
            for url in urls where ok {
                ok = self.sendEntry(url, relative: url.lastPathComponent, transfer: transfer,
                                    purpose: purpose)
            }
            self.sendAndWait(Message.fileEnd(transfer: transfer, ok: ok))
        }
    }

    private let inFlight = DispatchSemaphore(value: 4)

    private func sendAndWait(_ message: Data) {
        inFlight.wait()
        send(message) { self.inFlight.signal() }
    }

    private func sendEntry(_ url: URL, relative: String, transfer: UInt32,
                           purpose: XS.FilePurpose) -> Bool {
        if cancelled { return false }
        let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey,
                                                       .isSymbolicLinkKey, .fileSizeKey])
        if values?.isDirectory == true {
            sendAndWait(Message.fileBegin(transfer: transfer, purpose: purpose, directory: true,
                                          size: 0, path: relative))
            let children = (try? FileManager.default.contentsOfDirectory(
                at: url, includingPropertiesForKeys: [.isSymbolicLinkKey, .isDirectoryKey])) ?? []
            for child in children.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                // Links to folders could loop.
                let link = (try? child.resourceValues(forKeys: [.isSymbolicLinkKey]))?.isSymbolicLink == true
                if link, (try? child.resolvingSymlinksInPath()
                    .resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true { continue }
                if !sendEntry(child, relative: relative + "/" + child.lastPathComponent,
                              transfer: transfer, purpose: purpose) {
                    return false
                }
            }
            return true
        }
        guard values?.isRegularFile == true || values?.isSymbolicLink == true,
              let handle = try? FileHandle(forReadingFrom: url) else {
            return values == nil ? false : true   // unreadable: fail; special files: skip
        }
        defer { try? handle.close() }
        let size = UInt64((try? url.resolvingSymlinksInPath()
            .resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        sendAndWait(Message.fileBegin(transfer: transfer, purpose: purpose, directory: false,
                                      size: size, path: relative))
        var left = size
        while left > 0 {
            if cancelled { return false }
            guard let chunk = try? handle.read(upToCount: Int(min(left, UInt64(XS.fileChunk)))),
                  !chunk.isEmpty else { return false }
            sendAndWait(Message.fileData(transfer: transfer, chunk))
            left -= UInt64(chunk.count)
        }
        return true
    }
}

/// Writes files sent by GNOME: into Downloads, or into a cache folder for
/// files copied there (then put on the Mac's clipboard).
final class FileReceiver {
    /// Called on the main queue when a transfer completes, with its
    /// top-level entries.
    var onReceived: ((XS.FilePurpose, [URL]) -> Void)?

    private var transfer: UInt32?
    private var purpose = XS.FilePurpose.downloads
    private var failed = false
    private var base: URL?
    private var tops: [String: URL] = [:]
    private var topOrder: [URL] = []
    private var handle: FileHandle?
    private var left: UInt64 = 0

    static var clipboardRoot: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("local.xscreen.viewer/clipboard", isDirectory: true)
    }

    /// Connection queue. Returns false on a protocol error.
    func handle(_ type: XS.Message, _ payload: Data) -> Bool {
        var r = ByteReader(payload)
        switch type {
        case .fileBegin:
            guard payload.count >= 24 else { return false }
            let id = r.u32(), purposeRaw = r.u32(), flags = r.u32()
            _ = r.u32()
            let size = r.u64()
            guard let path = String(data: r.remaining, encoding: .utf8) else { return false }
            if transfer != id { start(id, XS.FilePurpose(rawValue: purposeRaw) ?? .downloads) }
            return begin(path: path, directory: flags & XS.fileDirectory != 0, size: size)
        case .fileData:
            guard payload.count >= 4 else { return false }
            guard r.u32() == transfer else { return true }
            return write(r.remaining)
        case .fileEnd:
            guard payload.count >= 8 else { return false }
            let id = r.u32(), status = r.u32()
            guard id == transfer else { return true }
            finish(ok: status == 0)
            return true
        default:
            return true
        }
    }

    func abandon() {
        finish(ok: false)
    }

    private func start(_ id: UInt32, _ purpose: XS.FilePurpose) {
        if transfer != nil { finish(ok: false) }
        transfer = id
        self.purpose = purpose
        failed = false
        let fm = FileManager.default
        if purpose == .clipboard {
            // Only the latest copy is kept.
            try? fm.removeItem(at: Self.clipboardRoot)
            base = Self.clipboardRoot.appendingPathComponent(UUID().uuidString, isDirectory: true)
        } else {
            base = fm.urls(for: .downloadsDirectory, in: .userDomainMask)[0]
        }
        if let base { try? fm.createDirectory(at: base, withIntermediateDirectories: true) }
    }

    /// The local URL for a sent relative path; nil if it is unsafe.
    private func localURL(_ relative: String) -> URL? {
        let parts = relative.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard let base, let first = parts.first, !relative.hasPrefix("/"),
              !parts.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) else { return nil }
        let top: URL
        if let known = tops[first] {
            top = known
        } else {
            top = purpose == .clipboard ? base.appendingPathComponent(first) : Self.unused(base, first)
            tops[first] = top
            topOrder.append(top)
        }
        return parts.dropFirst().reduce(top) { $0.appendingPathComponent($1) }
    }

    /// "name.ext", then "name 2.ext", like Finder.
    private static func unused(_ base: URL, _ name: String) -> URL {
        let fm = FileManager.default
        var url = base.appendingPathComponent(name)
        let ext = (name as NSString).pathExtension
        let stem = (name as NSString).deletingPathExtension
        var n = 2
        while fm.fileExists(atPath: url.path) {
            url = base.appendingPathComponent(ext.isEmpty ? "\(stem) \(n)" : "\(stem) \(n).\(ext)")
            n += 1
        }
        return url
    }

    private func begin(path: String, directory: Bool, size: UInt64) -> Bool {
        if left > 0 { failed = true }
        try? handle?.close()
        handle = nil
        left = 0
        if failed { return true }
        guard let url = localURL(path) else { return false }
        let fm = FileManager.default
        if directory {
            if (try? fm.createDirectory(at: url, withIntermediateDirectories: true)) == nil { failed = true }
            return true
        }
        try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard fm.createFile(atPath: url.path, contents: nil),
              let file = try? FileHandle(forWritingTo: url) else {
            failed = true
            return true
        }
        if size > 0 {
            handle = file
            left = size
        } else {
            try? file.close()
        }
        return true
    }

    private func write(_ bytes: Data) -> Bool {
        if failed { return true }
        guard let handle, UInt64(bytes.count) <= left else { return false }
        do {
            try handle.write(contentsOf: bytes)
        } catch {
            failed = true
            return true
        }
        left -= UInt64(bytes.count)
        if left == 0 {
            try? handle.close()
            self.handle = nil
        }
        return true
    }

    private func finish(ok: Bool) {
        try? handle?.close()
        handle = nil
        let complete = ok && !failed && left == 0
        left = 0
        let received = topOrder
        if !complete {
            for url in received { try? FileManager.default.removeItem(at: url) }
        } else if !received.isEmpty {
            let purpose = self.purpose
            DispatchQueue.main.async { self.onReceived?(purpose, received) }
        }
        transfer = nil
        tops = [:]
        topOrder = []
        base = nil
        failed = false
    }
}

/// "Received from GNOME" notifications; clicking one shows the files.
final class FileNotifier: NSObject, UNUserNotificationCenterDelegate {
    private let center = UNUserNotificationCenter.current()

    override init() {
        super.init()
        center.delegate = self
        center.requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    func notifyReceived(_ urls: [URL]) {
        let content = UNMutableNotificationContent()
        content.title = urls.count == 1 ? "已从 GNOME 收到文件" : "已从 GNOME 收到 \(urls.count) 个项目"
        content.body = urls.count == 1 ? urls[0].lastPathComponent
                                       : "\(urls[0].lastPathComponent) 等，已保存到“下载”"
        content.userInfo = ["paths": urls.map(\.path)]
        center.add(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                didReceive response: UNNotificationResponse,
                                withCompletionHandler completionHandler: @escaping () -> Void) {
        let paths = response.notification.request.content.userInfo["paths"] as? [String] ?? []
        NSWorkspace.shared.activateFileViewerSelecting(paths.map { URL(fileURLWithPath: $0) })
        completionHandler()
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter,
                                willPresent notification: UNNotification,
                                withCompletionHandler completionHandler:
                                    @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner])
    }
}
