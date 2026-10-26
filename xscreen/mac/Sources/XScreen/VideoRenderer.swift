import CoreImage
import CoreMedia
import ImageIO
import QuartzCore
import VideoToolbox

/// Decodes the HEVC Annex-B access units from the phone on the media engine
/// (VideoToolbox) and shows each picture as soon as it is decoded, as the
/// contents of an ordinary layer (its IOSurface, no copy).
///
/// Composited like any other window content, the picture keeps up on every
/// display; AVSampleBufferDisplayLayer's video path stalled on scaled
/// external displays.
final class VideoRenderer {
    let layer: CALayer
    /// Asked for when decoding cannot continue without a new keyframe.
    var onNeedKeyframe: (() -> Void)?

    private var format: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private var waitingForKeyframe = true
    private let lock = NSLock()
    private var shown: CVPixelBuffer?
    private(set) var framesDecoded = 0
    private(set) var keyframes = 0
    private var decodeErrors = 0

    init(layer: CALayer) {
        self.layer = layer
    }

    /// Connection queue.
    func reset() {
        format = nil
        waitingForKeyframe = true
        invalidateSession()
    }

    private func invalidateSession() {
        if let session {
            VTDecompressionSessionWaitForAsynchronousFrames(session)
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
    }

    private func makeSession(_ format: CMVideoFormatDescription) -> VTDecompressionSession? {
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
            kCVPixelBufferMetalCompatibilityKey: true,
        ]
        let specification: [CFString: Any] = [
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: true,
        ]
        var session: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: format,
            decoderSpecification: specification as CFDictionary,
            imageBufferAttributes: attributes as CFDictionary,
            outputCallback: nil, decompressionSessionOut: &session)
        guard status == noErr, let session else { return nil }
        VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        return session
    }

    /// Called on the connection queue for every access unit; `ptsUs` is its
    /// capture time on the phone.
    func enqueue(accessUnit: Data, keyframe: Bool, ptsUs: Int64) {
        let nals = Self.splitAnnexB(accessUnit)

        if keyframe, let newFormat = Self.makeFormat(nals) {
            if format == nil || !CMFormatDescriptionEqual(newFormat, otherFormatDescription: format!) {
                format = newFormat
                if let session, VTDecompressionSessionCanAcceptFormatDescription(session, formatDescription: newFormat) {
                    // Same decoder, new parameter sets.
                } else {
                    invalidateSession()
                }
            }
            waitingForKeyframe = false
        }
        guard let format, !waitingForKeyframe else {
            onNeedKeyframe?()
            return
        }
        if session == nil { session = makeSession(format) }
        guard let session else {
            waitingForKeyframe = true
            onNeedKeyframe?()
            return
        }

        // AVCC framing: 4-byte big-endian length per NAL, parameter sets
        // stay in the format description.
        var sample = Data(capacity: accessUnit.count + nals.count * 4)
        for nal in nals where !Self.isParameterSet(nal) {
            var length = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: &length) { sample.append(contentsOf: $0) }
            sample.append(nal)
        }
        guard let buffer = Self.makeSampleBuffer(sample, format: format, ptsUs: ptsUs) else { return }
        if keyframe { keyframes += 1 }

        let status = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: buffer,
            flags: [._EnableAsynchronousDecompression, ._1xRealTimePlayback],
            infoFlagsOut: nil) { [weak self] status, _, image, _, _ in
                guard let self else { return }
                guard status == noErr, let image else {
                    self.decodeFailed()
                    return
                }
                self.show(image)
            }
        if status != noErr { decodeFailed() }
    }

    private func decodeFailed() {
        lock.lock()
        decodeErrors += 1
        lock.unlock()
        // The next keyframe restarts decoding; reference frames are lost.
        DispatchQueue.main.async { self.onNeedKeyframe?() }
    }

    /// Decoder thread: frames come out in order.
    private func show(_ image: CVPixelBuffer) {
        guard let surface = CVPixelBufferGetIOSurface(image)?.takeUnretainedValue() else { return }
        lock.lock()
        framesDecoded += 1
        lock.unlock()
        DispatchQueue.main.async {
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            self.layer.contents = surface
            CATransaction.commit()
            self.lock.lock()
            self.shown = image   // keeps the buffer out of the pool while shown
            self.lock.unlock()
        }
    }

    var statusDescription: String {
        lock.lock()
        defer { lock.unlock() }
        var size = "nothing"
        if let shown {
            size = "\(CVPixelBufferGetWidth(shown))x\(CVPixelBufferGetHeight(shown))"
        }
        return "\(framesDecoded) frames decoded (\(keyframes) key, \(decodeErrors) errors), showing \(size)"
    }

    /// Debug: writes the picture currently on screen to a PNG.
    func snapshot(to url: URL) -> Bool {
        lock.lock()
        let pixels = shown
        lock.unlock()
        guard let pixels else { return false }
        let image = CIImage(cvPixelBuffer: pixels)
        let context = CIContext()
        guard let cg = context.createCGImage(image, from: image.extent),
              let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else {
            return false
        }
        CGImageDestinationAddImage(dest, cg, nil)
        return CGImageDestinationFinalize(dest)
    }

    // MARK: - HEVC helpers

    private static func nalType(_ nal: Data) -> UInt8 {
        (nal[nal.startIndex] >> 1) & 0x3f
    }

    private static func isParameterSet(_ nal: Data) -> Bool {
        (32...34).contains(nalType(nal))
    }

    static func splitAnnexB(_ data: Data) -> [Data] {
        var nals: [Data] = []
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let p = raw.bindMemory(to: UInt8.self)
            let n = p.count
            var i = 0
            var start = -1
            while i + 2 < n {
                if p[i] == 0, p[i + 1] == 0, p[i + 2] == 1 {
                    if start >= 0 {
                        var end = i
                        if end > start, p[end - 1] == 0 { end -= 1 } // 4-byte start code
                        nals.append(Data(bytes: p.baseAddress! + start, count: end - start))
                    }
                    i += 3
                    start = i
                } else {
                    i += 1
                }
            }
            if start >= 0, start < n {
                nals.append(Data(bytes: p.baseAddress! + start, count: n - start))
            }
        }
        return nals.filter { !$0.isEmpty }
    }

    private static func makeFormat(_ nals: [Data]) -> CMVideoFormatDescription? {
        guard let vps = nals.first(where: { nalType($0) == 32 }),
              let sps = nals.first(where: { nalType($0) == 33 }),
              let pps = nals.first(where: { nalType($0) == 34 }) else {
            return nil
        }
        let sets = [vps, sps, pps]
        var format: CMVideoFormatDescription?
        let status = sets[0].withUnsafeBytes { v in
            sets[1].withUnsafeBytes { s in
                sets[2].withUnsafeBytes { pp in
                    let pointers = [v, s, pp].map { $0.bindMemory(to: UInt8.self).baseAddress! }
                    let sizes = sets.map { $0.count }
                    return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: 3,
                        parameterSetPointers: pointers,
                        parameterSetSizes: sizes,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &format)
                }
            }
        }
        return status == noErr ? format : nil
    }

    private static func makeSampleBuffer(_ sample: Data, format: CMVideoFormatDescription,
                                         ptsUs: Int64) -> CMSampleBuffer? {
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: sample.count,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
            dataLength: sample.count, flags: kCMBlockBufferAssureMemoryNowFlag,
            blockBufferOut: &block) == noErr, let block else {
            return nil
        }
        sample.withUnsafeBytes { raw in
            _ = CMBlockBufferReplaceDataBytes(with: raw.baseAddress!, blockBuffer: block,
                                              offsetIntoDestination: 0, dataLength: sample.count)
        }

        var buffer: CMSampleBuffer?
        var size = sample.count
        var timing = CMSampleTimingInfo(duration: .invalid,
                                        presentationTimeStamp: CMTime(value: ptsUs, timescale: 1_000_000),
                                        decodeTimeStamp: .invalid)
        guard CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
            sampleCount: 1, sampleTimingEntryCount: 1, sampleTimingArray: &timing,
            sampleSizeEntryCount: 1, sampleSizeArray: &size,
            sampleBufferOut: &buffer) == noErr else {
            return nil
        }
        return buffer
    }
}
