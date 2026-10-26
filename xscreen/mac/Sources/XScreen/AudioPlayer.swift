import AVFoundation
import os

/// Plays GNOME's sound: 16-bit PCM packets in, the Mac's current output out.
///
/// A small jitter buffer sits in between. It refills to `target` before
/// playing (at the start and after running dry) and drops the oldest audio
/// when it holds more than `limit`, so latency stays bounded even if the
/// two clocks drift apart. The engine stops when no sound has come for a
/// while and starts again with the next packet.
final class AudioPlayer {
    private static let rate = 48_000.0
    private static let channels = 2
    private static let target = Int(rate * 0.04)   // frames buffered before playing
    private static let limit = Int(rate * 0.12)    // frames kept at most

    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode!
    private var lock = os_unfair_lock()
    private var ring = [Float](repeating: 0, count: Int(rate) * channels)  // 1 s, interleaved
    private var readIndex = 0     // in frames, modulo capacity
    private var buffered = 0      // frames
    private var primed = false
    private var lastPacket = Date.distantPast
    private var idleTimer: Timer?

    private var capacity: Int { ring.count / Self.channels }

    init() {
        let format = AVAudioFormat(standardFormatWithSampleRate: Self.rate,
                                   channels: AVAudioChannelCount(Self.channels))!
        source = AVAudioSourceNode(format: format) { [unowned self] _, _, frameCount, buffers in
            self.render(Int(frameCount), UnsafeMutableAudioBufferListPointer(buffers))
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        // A new default output (headphones plugged in, AirPods) needs a restart.
        NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange,
                                               object: engine, queue: .main) { [weak self] _ in
            guard let self, Date().timeIntervalSince(self.lastPacket) < 1 else { return }
            try? self.engine.start()
        }
    }

    /// Interleaved signed 16-bit PCM at 48 kHz stereo. Any thread.
    func push(_ pcm: Data, rate: Int, channels: Int) {
        guard rate == Int(Self.rate), channels == Self.channels else { return }
        let frames = pcm.count / (2 * channels)
        pcm.withUnsafeBytes { raw in
            let samples = raw.bindMemory(to: Int16.self)
            os_unfair_lock_lock(&lock)
            // Too much queued: the oldest goes.
            let overflow = buffered + frames - Self.limit
            if overflow > 0 {
                readIndex = (readIndex + overflow) % capacity
                buffered -= overflow
            }
            var write = (readIndex + buffered) % capacity
            for i in 0..<frames {
                ring[write * 2] = Float(Int16(littleEndian: samples[i * 2])) / 32768
                ring[write * 2 + 1] = Float(Int16(littleEndian: samples[i * 2 + 1])) / 32768
                write = (write + 1) % capacity
            }
            buffered += frames
            if buffered >= Self.target { primed = true }
            os_unfair_lock_unlock(&lock)
        }
        DispatchQueue.main.async { self.keepRunning() }
    }

    /// Stops playback (the connection went away).
    func stop() {
        DispatchQueue.main.async {
            self.idleTimer?.invalidate()
            self.idleTimer = nil
            self.engine.stop()
            os_unfair_lock_lock(&self.lock)
            self.buffered = 0
            self.primed = false
            os_unfair_lock_unlock(&self.lock)
        }
    }

    private func keepRunning() {
        lastPacket = Date()
        if !engine.isRunning {
            try? engine.start()
        }
        if idleTimer == nil {
            idleTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                guard let self, Date().timeIntervalSince(self.lastPacket) > 2 else { return }
                self.stop()
            }
        }
    }

    /// Realtime audio thread.
    private func render(_ frames: Int, _ buffers: UnsafeMutableAudioBufferListPointer) {
        let left = buffers[0].mData!.assumingMemoryBound(to: Float.self)
        let right = buffers[1].mData!.assumingMemoryBound(to: Float.self)
        os_unfair_lock_lock(&lock)
        let available = primed ? min(frames, buffered) : 0
        for i in 0..<available {
            left[i] = ring[readIndex * 2]
            right[i] = ring[readIndex * 2 + 1]
            readIndex = (readIndex + 1) % capacity
        }
        buffered -= available
        if available < frames { primed = false }   // ran dry: refill first
        os_unfair_lock_unlock(&lock)
        for i in available..<frames {
            left[i] = 0
            right[i] = 0
        }
    }
}
