import Foundation

/// Debug: frame pacing as seen by the Mac. Capture gaps come from the
/// phone's timestamps; arrival gaps and transit jitter (arrival minus capture
/// time, relative to the best frame in the window) show what encoding and
/// the USB link add on top.
final class FrameStats {
    private var lock = NSLock()
    private var samples: [(pts: Int64, arrival: Int64)] = []

    func record(ptsUs: Int64) {
        let now = Int64(DispatchTime.now().uptimeNanoseconds / 1000)
        lock.lock()
        samples.append((ptsUs, now))
        lock.unlock()
    }

    /// Summarizes and clears the window, or nil when no frames arrived.
    func drain() -> String? {
        lock.lock()
        let window = samples
        samples.removeAll(keepingCapacity: true)
        lock.unlock()
        guard window.count >= 3 else { return nil }

        // Gaps over 100 ms are the screen being idle, not pacing.
        let pairs = zip(window, window.dropFirst()).filter { $1.pts - $0.pts <= 100_000 }
        guard !pairs.isEmpty else { return nil }
        let captureGaps = pairs.map { Double($1.pts - $0.pts) / 1000 }
        let arrivalGaps = pairs.map { Double($1.arrival - $0.arrival) / 1000 }
        let transit = window.map { $0.arrival - $0.pts }
        let best = transit.min()!
        let jitter = transit.map { Double($0 - best) / 1000 }
        let seconds = captureGaps.reduce(0, +) / 1000

        func describe(_ values: [Double]) -> String {
            let sorted = values.sorted()
            func p(_ q: Double) -> Double { sorted[min(sorted.count - 1, Int(Double(sorted.count) * q))] }
            return String(format: "p50 %.1f p90 %.1f p99 %.1f max %.1f", p(0.5), p(0.9), p(0.99), sorted.last!)
        }
        return String(format: "%d frames %.0f fps while active | capture gap ms %@ | arrival gap ms %@ | transit jitter ms %@",
                      window.count, Double(pairs.count) / max(seconds, 0.001),
                      describe(captureGaps), describe(arrivalGaps), describe(jitter))
    }
}
