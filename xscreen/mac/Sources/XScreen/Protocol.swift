// Mirror of protocol/xs-protocol.h. Little-endian, packed.

import Foundation

enum XS {
    static let magic: UInt32 = 0x3143_5358 // "XSC1"
    static let version: UInt32 = 1
    static let defaultPort: UInt16 = 7845
    static let headerSize = 12

    enum Message: UInt16 {
        case serverHello = 1
        case videoConfig = 2
        case videoFrame = 3
        case cursor = 4
        case layout = 5
        case clipboard = 6
        case audio = 7
        case fileBegin = 8
        case fileData = 9
        case fileEnd = 10

        case clientHello = 64
        case requestKeyframe = 65
        case input = 67
        case clientClipboard = 68
        case clientFileBegin = 72
        case clientFileData = 73
        case clientFileEnd = 74
    }

    enum ClipboardFormat: UInt32 {
        case text = 1
        case image = 2 // PNG
    }

    static let clipboardMax = 32 << 20

    enum FilePurpose: UInt32 {
        case downloads = 0
        case clipboard = 1
    }

    static let fileDirectory: UInt32 = 1 << 0
    static let fileChunk = 256 << 10
    static let fileClipboardMax: UInt64 = 512 << 20

    struct Edges: OptionSet {
        let rawValue: UInt32
        static let left = Edges(rawValue: 1 << 0)
        static let right = Edges(rawValue: 1 << 1)
        static let top = Edges(rawValue: 1 << 2)
        static let bottom = Edges(rawValue: 1 << 3)
    }

    static let frameKeyframe: UInt32 = 1 << 0
    static let frameRefine: UInt32 = 1 << 1

    enum Input: UInt32 {
        case pointerAbsolute = 1
        case pointerRelative = 2
        case button = 3
        case scroll = 4
        case scrollDiscrete = 5
        case key = 6
    }
}

struct ByteReader {
    let data: Data
    var offset = 0

    init(_ data: Data) { self.data = data }

    mutating func u32() -> UInt32 {
        let v = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self) }
        offset += 4
        return UInt32(littleEndian: v)
    }

    mutating func i32() -> Int32 { Int32(bitPattern: u32()) }

    mutating func u16() -> UInt16 {
        let v = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt16.self) }
        offset += 2
        return UInt16(littleEndian: v)
    }

    mutating func u64() -> UInt64 { UInt64(bitPattern: i64()) }

    mutating func i64() -> Int64 {
        let v = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt64.self) }
        offset += 8
        return Int64(bitPattern: UInt64(littleEndian: v))
    }

    var remaining: Data { data.subdata(in: (data.startIndex + offset)..<data.endIndex) }
}

struct ByteWriter {
    var data = Data()

    mutating func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func i32(_ v: Int32) { u32(UInt32(bitPattern: v)) }
    mutating func u64(_ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func f32(_ v: Float) { u32(v.bitPattern) }

    /// NUL-padded UTF-8, truncated on a character boundary.
    mutating func fixedString(_ string: String, size: Int) {
        var bytes = [UInt8]()
        for scalar in string.unicodeScalars {
            let encoded = Array(String(scalar).utf8)
            if bytes.count + encoded.count > size - 1 { break }
            bytes += encoded
        }
        bytes += [UInt8](repeating: 0, count: size - bytes.count)
        data.append(contentsOf: bytes)
    }
}

enum Message {
    static func encode(_ type: XS.Message, _ payload: Data = Data()) -> Data {
        var w = ByteWriter()
        w.u32(XS.magic)
        w.u16(type.rawValue)
        w.u16(0)
        w.u32(UInt32(payload.count))
        return w.data + payload
    }

    /// Describes the display the window is on; the phone offers GNOME a
    /// monitor with this identity and modes derived from its size.
    static func clientHello(width: Int, height: Int, maxFps: Int, scalePercent: Int,
                            windowWidth: Int, windowHeight: Int,
                            displayID: String, displayName: String) -> Data {
        var w = ByteWriter()
        w.u32(XS.version)
        w.u32(UInt32(width))
        w.u32(UInt32(height))
        w.u32(UInt32(maxFps))
        w.u32(UInt32(scalePercent))
        w.u32(UInt32(windowWidth))
        w.u32(UInt32(windowHeight))
        w.fixedString(displayID, size: 40)
        w.fixedString(displayName, size: 64)
        return encode(.clientHello, w.data)
    }

    static func clipboard(_ format: XS.ClipboardFormat, _ content: Data) -> Data {
        var w = ByteWriter()
        w.u32(format.rawValue)
        return encode(.clientClipboard, w.data + content)
    }

    static func fileBegin(transfer: UInt32, purpose: XS.FilePurpose, directory: Bool,
                          size: UInt64, path: String) -> Data {
        var w = ByteWriter()
        w.u32(transfer)
        w.u32(purpose.rawValue)
        w.u32(directory ? XS.fileDirectory : 0)
        w.u32(0)
        w.u64(size)
        return encode(.clientFileBegin, w.data + Data(path.utf8))
    }

    static func fileData(transfer: UInt32, _ bytes: Data) -> Data {
        var w = ByteWriter()
        w.u32(transfer)
        return encode(.clientFileData, w.data + bytes)
    }

    static func fileEnd(transfer: UInt32, ok: Bool) -> Data {
        var w = ByteWriter()
        w.u32(transfer)
        w.u32(ok ? 0 : 1)
        return encode(.clientFileEnd, w.data)
    }

    static func input(_ type: XS.Input, code: UInt32 = 0, value: Int32 = 0,
                      x: Float = 0, y: Float = 0) -> Data {
        var w = ByteWriter()
        w.u32(type.rawValue)
        w.u32(code)
        w.i32(value)
        w.f32(x)
        w.f32(y)
        return encode(.input, w.data)
    }
}
