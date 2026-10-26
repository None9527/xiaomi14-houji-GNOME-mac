import Foundation
import Network

/// Message-framed TCP connection to xscreend (through `adb forward`).
/// All callbacks run on `queue`.
final class Connection {
    var onMessage: ((XS.Message, Data) -> Void)?
    var onClosed: ((String) -> Void)?

    let queue = DispatchQueue(label: "xscreen.connection", qos: .userInteractive)
    private let connection: NWConnection

    init(port: UInt16) {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        connection = NWConnection(host: "127.0.0.1",
                                  port: NWEndpoint.Port(rawValue: port)!,
                                  using: NWParameters(tls: nil, tcp: tcp))
    }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                self?.readHeader()
            case .failed(let error):
                self?.onClosed?("connection failed: \(error)")
            case .cancelled:
                self?.onClosed?("connection closed")
            default:
                break
            }
        }
        connection.start(queue: queue)
    }

    func send(_ data: Data, completion: (() -> Void)? = nil) {
        connection.send(content: data, completion: .contentProcessed { _ in completion?() })
    }

    func cancel() {
        connection.cancel()
    }

    private func readHeader() {
        connection.receive(minimumIncompleteLength: XS.headerSize,
                           maximumLength: XS.headerSize) { [weak self] data, _, done, error in
            guard let self else { return }
            guard let data, data.count == XS.headerSize, error == nil else {
                self.connection.cancel()
                return
            }
            var r = ByteReader(data)
            let magic = r.u32()
            let typeAndFlags = r.u32()
            let length = Int(r.u32())
            guard magic == XS.magic, length < 64 << 20 else {
                self.onClosed?("protocol error")
                self.connection.cancel()
                return
            }
            let type = UInt16(truncatingIfNeeded: typeAndFlags)
            if length == 0 {
                self.deliver(type, Data())
                self.readHeader()
            } else {
                self.readPayload(type, length)
            }
            _ = done
        }
    }

    private func readPayload(_ type: UInt16, _ length: Int) {
        connection.receive(minimumIncompleteLength: length,
                           maximumLength: length) { [weak self] data, _, _, error in
            guard let self else { return }
            guard let data, data.count == length, error == nil else {
                self.connection.cancel()
                return
            }
            self.deliver(type, data)
            self.readHeader()
        }
    }

    private func deliver(_ type: UInt16, _ payload: Data) {
        if let message = XS.Message(rawValue: type) {
            onMessage?(message, payload)
        }
    }
}
