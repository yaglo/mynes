import Foundation
import Combine

enum ConnectionState: Sendable, Equatable {
    case disconnected
    case connecting
    case connected
    case error(String)

    var label: String {
        switch self {
        case .disconnected: return "Disconnected"
        case .connecting:   return "Connecting..."
        case .connected:    return "Connected"
        case .error(let msg): return "Error: \(msg)"
        }
    }
}

enum ConnectionError: Error, LocalizedError {
    case socketCreationFailed
    case connectFailed(String)
    case sendFailed
    case notConnected

    var errorDescription: String? {
        switch self {
        case .socketCreationFailed: return "Failed to create socket"
        case .connectFailed(let msg): return "Connection failed: \(msg)"
        case .sendFailed: return "Send failed"
        case .notConnected: return "Not connected"
        }
    }
}

@MainActor
final class EmulatorConnection: ObservableObject {
    @Published private(set) var state: ConnectionState = .disconnected
    @Published private(set) var snapshot: ChainSnapshot = .empty
    @Published private(set) var tapWaveform: WaveformData? = nil

    @Published private(set) var framesPerSecond: Double = 0
    private var rateSample: (time: TimeInterval, frame: UInt32)?

    @Published private(set) var presets = PresetCatalog()
    @Published private(set) var presetBusy = false
    @Published var presetError: String?

    @Published private(set) var controls: [PhysicalControl] = []

    let socketPath: String

    private var fd: Int32 = -1
    private var listenTask: Task<Void, Never>?
    private var reconnectTask: Task<Void, Never>?

    init(socketPath: String? = nil) {
        self.socketPath = socketPath ?? ProcessInfo.processInfo.environment["MYNES_DEBUG_SOCKET"] ?? "/tmp/mynes_gpu_debug.sock"
    }

    deinit {
        listenTask?.cancel()
        reconnectTask?.cancel()
        if fd >= 0 { close(fd) }
    }

    // MARK: - Public API

    func connect() {
        guard state != .connecting, state != .connected else { return }
        reconnectTask?.cancel()
        state = .connecting
        listenTask?.cancel()
        listenTask = Task { await connectAndListen() }
    }

    func disconnect() {
        listenTask?.cancel()
        reconnectTask?.cancel()
        closeSocket()
        state = .disconnected
    }

    func requestTap(stageIndex: Int, chainType: ChainType) {
        // msg_type=3 (tap_request), payload = stage_index(4) — matches server DebugTapRequest
        var buf = Data(capacity: 8 + 4)
        appendUInt32(&buf, 3)            // msg_type: tap_request
        appendUInt32(&buf, 4)            // payload_size
        appendUInt32(&buf, UInt32(stageIndex))
        sendRaw(buf)
    }

    func updateControl(_ id: Int, value: Double) {
        guard state == .connected, let control = controls.first(where: { $0.id == id }),
              value.isFinite, value >= control.minimum, value <= control.maximum else { return }
        var message = Data()
        appendUInt32(&message, 6)
        appendUInt32(&message, 8)
        appendUInt32(&message, UInt32(id))
        appendUInt32(&message, Float(value).bitPattern)
        sendRaw(message)
    }

    func presetCommand(_ operation: UInt32, id: Int? = nil, name: String = "") {
        guard state == .connected, !presetBusy, name.utf8.count <= 100 else { return }
        var message = Data()
        appendUInt32(&message, 8)
        appendUInt32(&message, 140)
        appendUInt32(&message, operation)
        appendUInt32(&message, UInt32(bitPattern: Int32(id ?? presets.activeID)))
        appendUInt32(&message, presets.revision)
        let encoded = Data(name.utf8)
        message.append(encoded)
        message.append(Data(repeating: 0, count: 128 - encoded.count))
        presetBusy = true
        sendRaw(message)
    }

    // MARK: - Connection loop

    private func connectAndListen() async {
        let connected = await connectSocket()
        guard !Task.isCancelled else { return }

        if !connected {
            state = .error("Could not connect to \(socketPath)")
            scheduleReconnect()
            return
        }

        state = .connected

        // Read loop
        await readLoop()

        // If we get here, connection was lost
        guard !Task.isCancelled else { return }
        closeSocket()
        state = .disconnected
        scheduleReconnect()
    }

    private func connectSocket() async -> Bool {
        let path = socketPath
        let result: Int32 = await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .utility).async {
                let sockFD = socket(AF_UNIX, SOCK_STREAM, 0)
                guard sockFD >= 0 else {
                    continuation.resume(returning: -1)
                    return
                }

                var noSignal: Int32 = 1
                setsockopt(sockFD, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, socklen_t(MemoryLayout<Int32>.size))
                var addr = sockaddr_un()
                addr.sun_family = sa_family_t(AF_UNIX)
                let pathBytes = path.utf8CString
                guard pathBytes.count <= MemoryLayout.size(ofValue: addr.sun_path) else {
                    close(sockFD)
                    continuation.resume(returning: -1)
                    return
                }
                withUnsafeMutableBytes(of: &addr.sun_path) { dst in
                    pathBytes.withUnsafeBufferPointer { src in
                        dst.copyBytes(from: UnsafeRawBufferPointer(src))
                    }
                }

                let len = socklen_t(MemoryLayout<sa_family_t>.size + pathBytes.count)
                let connectResult = withUnsafePointer(to: &addr) { ptr in
                    ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                        Foundation.connect(sockFD, sa, len)
                    }
                }

                if connectResult != 0 {
                    close(sockFD)
                    continuation.resume(returning: -1)
                } else {
                    continuation.resume(returning: sockFD)
                }
            }
        }

        if result < 0 {
            return false
        }
        fd = result
        return true
    }

    private static let msgTypeSnapshot: UInt32 = 0
    private static let msgTypeTapData: UInt32 = 4

    private func readLoop() async {
        let socketFD = fd
        guard socketFD >= 0 else { return }

        while !Task.isCancelled {
            let messageData: Data? = await withCheckedContinuation { continuation in
                DispatchQueue.global(qos: .utility).async {
                    guard let data = Self.readMessage(fd: socketFD) else {
                        continuation.resume(returning: nil)
                        return
                    }
                    continuation.resume(returning: data)
                }
            }

            guard let data = messageData, data.count >= 8 else { break }

            // Peek at message type from the header
            let msgType = data.withUnsafeBytes { ptr in
                UInt32(littleEndian: ptr.loadUnaligned(fromByteOffset: 0, as: UInt32.self))
            }

            switch msgType {
            case Self.msgTypeSnapshot:
                if let decoded = ChainSnapshot.decode(from: data) {
                    let now = ProcessInfo.processInfo.systemUptime
                    if let previous = rateSample, now - previous.time >= 0.75 {
                        framesPerSecond = decoded.frameNumber >= previous.frame
                            ? Double(decoded.frameNumber - previous.frame) / (now - previous.time) : 0
                        rateSample = (now, decoded.frameNumber)
                    } else if rateSample == nil { rateSample = (now, decoded.frameNumber) }
                    self.snapshot = decoded
                }
            case 7:
                if let catalog = PresetCatalog.decode(data) { presets = catalog }
            case 9:
                if data.count == 144 {
                    let ok = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 12, as: UInt32.self) }
                    presetBusy = false
                    if ok == 0 { presetError = String(decoding: data[16..<144].prefix { $0 != 0 }, as: UTF8.self) }
                }
            case 5:
                if let controls = PhysicalControl.decode(data) { self.controls = controls }
            case Self.msgTypeTapData:
                if let wf = WaveformData.decode(from: data) {
                    self.tapWaveform = wf
                }
            default:
                break // ignore unknown message types
            }
        }
    }

    /// Read one framed message: 4-byte msg_type + 4-byte payload_size + payload.
    private nonisolated static func readMessage(fd: Int32) -> Data? {
        // Read 8-byte header
        var header = Data(count: 8)
        let headerRead = header.withUnsafeMutableBytes { ptr in
            readFully(fd: fd, buf: ptr.baseAddress!, count: 8)
        }
        guard headerRead else { return nil }

        let payloadSize = header.withUnsafeBytes { ptr in
            UInt32(littleEndian: ptr.loadUnaligned(fromByteOffset: 4, as: UInt32.self))
        }
        guard payloadSize < 10_000_000 else { return nil } // sanity limit

        var message = header
        if payloadSize > 0 {
            var payload = Data(count: Int(payloadSize))
            let payloadRead = payload.withUnsafeMutableBytes { ptr in
                readFully(fd: fd, buf: ptr.baseAddress!, count: Int(payloadSize))
            }
            guard payloadRead else { return nil }
            message.append(payload)
        }

        return message
    }

    private nonisolated static func readFully(fd: Int32, buf: UnsafeMutableRawPointer, count: Int) -> Bool {
        var totalRead = 0
        while totalRead < count {
            let n = read(fd, buf.advanced(by: totalRead), count - totalRead)
            if n <= 0 { return false }
            totalRead += n
        }
        return true
    }

    private func closeSocket() {
        presetBusy = false
        rateSample = nil
        framesPerSecond = 0
        if fd >= 0 {
            shutdown(fd, SHUT_RDWR)
            close(fd)
            fd = -1
        }
    }

    private func sendRaw(_ data: Data) {
        guard fd >= 0 else { return }
        data.withUnsafeBytes { ptr in
            var sent = 0
            while sent < ptr.count {
                let result = write(fd, ptr.baseAddress!.advanced(by: sent), ptr.count - sent)
                if result < 0 && errno == EINTR { continue }
                if result <= 0 { break }
                sent += result
            }
        }
    }

    private func scheduleReconnect() {
        reconnectTask?.cancel()
        reconnectTask = Task {
            try? await Task.sleep(for: .seconds(2))
            guard !Task.isCancelled else { return }
            connect()
        }
    }

    // MARK: - Encoding helpers

    private func appendUInt32(_ data: inout Data, _ value: UInt32) {
        var le = value.littleEndian
        withUnsafeBytes(of: &le) { data.append(contentsOf: $0) }
    }

    private func appendUInt16(_ data: inout Data, _ value: UInt16) {
        var le = value.littleEndian
        withUnsafeBytes(of: &le) { data.append(contentsOf: $0) }
    }
}
