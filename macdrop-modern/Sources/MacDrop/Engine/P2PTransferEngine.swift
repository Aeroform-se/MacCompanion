import Foundation
import Network
import Combine

/// Direct P2P File Transfer Engine for vintage Macintosh 68k clients.
/// Streams MacBinary II data at 100% full unconstrained wire speed over raw TCP
/// directly to port 6803 on the target Mac.
public final class P2PTransferEngine: ObservableObject {
    public static let shared = P2PTransferEngine()

    @Published public var currentTransfer: TransferProgress?
    @Published public var isTransferring: Bool = false
    @Published public var lastDeliveredFile: String?

    private var activeConnection: NWConnection?
    private var isCancelled: Bool = false
    private let queue = DispatchQueue(label: "se.aeroform.macdrop.p2p", qos: .userInitiated)

    public init() {}

    /// Cancels any currently active transfer.
    public func cancelCurrentTransfer() {
        queue.async { [weak self] in
            guard let self = self else { return }
            self.isCancelled = true
            self.activeConnection?.cancel()
            self.activeConnection = nil

            DispatchQueue.main.async {
                if var xfer = self.currentTransfer {
                    xfer.status = .cancelled
                    xfer.errorMessage = "Cancelled by user"
                    self.currentTransfer = xfer
                }
                self.isTransferring = false
            }
        }
    }

    /// Sends a file directly to a vintage Mac's port 6803 over TCP.
    public func send(fileURL: URL, to target: DiscoveredMac) {
        let resolvedURL = (fileURL as NSURL).filePathURL?.standardizedFileURL ?? fileURL.resolvingSymlinksInPath().standardizedFileURL
        let fileName = resolvedURL.lastPathComponent

        DebugLogger.log("[P2P] send called for '\(fileName)' to '\(target.name)' (ip=\(target.ip), id=\(target.id))")

        // Immediately mark transfer state on the main thread so dismissal checks & HUD respond instantly
        let setupBlock = {
            var xfer = TransferProgress()
            xfer.status = .preparing
            xfer.fileName = fileName
            xfer.targetName = target.name
            xfer.targetIP = target.ip
            xfer.startedAt = Date()
            self.currentTransfer = xfer
            self.isTransferring = true
        }

        if Thread.isMainThread {
            setupBlock()
        } else {
            DispatchQueue.main.sync(execute: setupBlock)
        }

        queue.async { [weak self] in
            guard let self = self else { return }
            self.isCancelled = false

            do {
                // 1. Encode file to standard MacBinary II
                DebugLogger.log("[P2P] Encoding MacBinary for '\(resolvedURL.path)'...")
                let encoded = try MacBinaryEncoder.encode(fileURL: resolvedURL)
                DebugLogger.log("[P2P] MacBinary encoded successfully (\(encoded.totalStreamSize) bytes)")

                DispatchQueue.main.async {
                    if var xfer = self.currentTransfer {
                        xfer.status = .connecting
                        xfer.fileSize = encoded.totalStreamSize
                        self.currentTransfer = xfer
                    }
                }

                // Check if target is connected via inbound reverse channel (e.g. Basilisk II SLIRP NAT)
                let hasChannel = InboundChannelServer.shared.hasActiveChannel(for: target.id, ip: target.ip)
                DebugLogger.log("[P2P] Checking reverse channel for \(target.id) (ip=\(target.ip)): \(hasChannel)")

                if hasChannel {
                    let startTime = Date()
                    DispatchQueue.main.async {
                        if var xfer = self.currentTransfer {
                            xfer.status = .streaming
                            self.currentTransfer = xfer
                        }
                    }

                    DebugLogger.log("[P2P] Streaming down active channel to \(target.name)...")
                    InboundChannelServer.shared.streamToDevice(
                        id: target.id,
                        ip: target.ip,
                        fileName: fileName,
                        fileData: encoded.fullMacBinaryData,
                        onProgress: { [weak self] bytesSent, speedBps in
                            DispatchQueue.main.async {
                                if var xfer = self?.currentTransfer {
                                    xfer.bytesSent = bytesSent
                                    xfer.speedBps = speedBps
                                    self?.currentTransfer = xfer
                                }
                            }
                        },
                        completion: { [weak self] error in
                            guard let self = self else { return }
                            if let error = error {
                                DebugLogger.log("[P2P] Reverse channel stream failed: \(error.localizedDescription)")
                                self.handleFailure(message: error.localizedDescription)
                            } else {
                                DebugLogger.log("[P2P] Reverse channel stream completed successfully!")
                                self.markCompleted(totalBytes: encoded.totalStreamSize, startTime: startTime)
                            }
                        }
                    )
                    return
                }

                if target.ip.hasPrefix("10.0.2.") {
                    let msg = "No active connection from \(target.name). Make sure MacDrop is running inside Basilisk II."
                    DebugLogger.log("[P2P] SLIRP target has no active channel: \(msg)")
                    self.handleFailure(message: msg)
                    return
                }

                // 2. Setup TCP connection to vintage Mac port 6803
                DebugLogger.log("[P2P] Setting up direct TCP connection to \(target.ip):\(target.port)...")
                let host = NWEndpoint.Host(target.ip)
                let port = NWEndpoint.Port(integerLiteral: UInt16(target.port))

                let tcpOptions = NWProtocolTCP.Options()
                tcpOptions.noDelay = true // Disable Nagle's algorithm for instant streaming
                tcpOptions.enableKeepalive = true
                tcpOptions.keepaliveIdle = 3

                let params = NWParameters(tls: nil, tcp: tcpOptions)
                let connection = NWConnection(host: host, port: port, using: params)
                self.activeConnection = connection

                let streamData = encoded.fullMacBinaryData
                self.streamData(connection: connection, data: streamData, target: target)

            } catch {
                DebugLogger.log("[P2P] Exception in send: \(error)")
                DispatchQueue.main.async {
                    if var xfer = self.currentTransfer {
                        xfer.status = .failed
                        xfer.errorMessage = "Failed to encode MacBinary: \(error.localizedDescription)"
                        self.currentTransfer = xfer
                    }
                    self.isTransferring = false
                }
            }
        }
    }

    private func streamData(connection: NWConnection, data: Data, target: DiscoveredMac) {
        let startTime = Date()
        let chunkSize = 32768 // 32 KB chunk streaming matching 68k Mac buffer size
        let totalBytes = Int64(data.count)

        connection.stateUpdateHandler = { [weak self] state in
            guard let self = self else { return }
            switch state {
            case .ready:
                DispatchQueue.main.async {
                    if var xfer = self.currentTransfer {
                        xfer.status = .streaming
                        self.currentTransfer = xfer
                    }
                }
                self.sendNextChunk(
                    connection: connection,
                    data: data,
                    offset: 0,
                    chunkSize: chunkSize,
                    totalBytes: totalBytes,
                    startTime: startTime,
                    target: target
                )

            case .failed(let err):
                self.handleFailure(message: "Connection failed: \(err.localizedDescription)")

            case .cancelled:
                break

            default:
                break
            }
        }

        connection.start(queue: queue)
    }

    private func sendNextChunk(
        connection: NWConnection,
        data: Data,
        offset: Int,
        chunkSize: Int,
        totalBytes: Int64,
        startTime: Date,
        target: DiscoveredMac
    ) {
        if isCancelled { return }

        if offset >= data.count {
            // All chunks transmitted! Now wait for Mac to flush disk and finish
            self.finalizeTransfer(connection: connection, totalBytes: totalBytes, startTime: startTime)
            return
        }

        let chunkEnd = min(offset + chunkSize, data.count)
        let chunkData = data.subdata(in: offset..<chunkEnd)
        let nextOffset = chunkEnd
        let currentOffset = Int64(nextOffset)

        let isLastChunk = (nextOffset >= data.count)

        connection.send(content: chunkData, completion: .contentProcessed { [weak self] error in
            guard let self = self else { return }
            if let error = error {
                self.handleFailure(message: "Socket write error: \(error.localizedDescription)")
                return
            }

            // Update live progress
            let elapsed = max(0.1, Date().timeIntervalSince(startTime))
            let currentBps = Double(currentOffset) / elapsed

            DispatchQueue.main.async {
                if var xfer = self.currentTransfer {
                    xfer.bytesSent = currentOffset
                    xfer.speedBps = currentBps
                    self.currentTransfer = xfer
                }
            }

            // Send next chunk immediately (unthrottled wire speed)
            if !isLastChunk {
                self.queue.async {
                    self.sendNextChunk(
                        connection: connection,
                        data: data,
                        offset: nextOffset,
                        chunkSize: chunkSize,
                        totalBytes: totalBytes,
                        startTime: startTime,
                        target: target
                    )
                }
            } else {
                self.finalizeTransfer(connection: connection, totalBytes: totalBytes, startTime: startTime)
            }
        })
    }

    private func finalizeTransfer(connection: NWConnection, totalBytes: Int64, startTime: Date) {
        DispatchQueue.main.async {
            if var xfer = self.currentTransfer {
                xfer.status = .finalizing
                xfer.bytesSent = totalBytes
                self.currentTransfer = xfer
            }
        }

        // Send TCP FIN to signal end of stream
        connection.send(content: nil, contentContext: .finalMessage, isComplete: true, completion: .idempotent)

        // Wait up to 3 seconds for Mac to close its socket after flushing to disk
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1024) { [weak self] (_, _, isComplete, _) in
            guard let self = self else { return }
            self.markCompleted(totalBytes: totalBytes, startTime: startTime)
            connection.cancel()
            self.activeConnection = nil
        }

        // Fallback safety timer if client closes silently
        queue.asyncAfter(deadline: .now() + 2.5) { [weak self] in
            guard let self = self, self.isTransferring else { return }
            self.markCompleted(totalBytes: totalBytes, startTime: startTime)
            connection.cancel()
            self.activeConnection = nil
        }
    }

    private func markCompleted(totalBytes: Int64, startTime: Date) {
        let duration = max(0.1, Date().timeIntervalSince(startTime))
        let avgSpeed = Double(totalBytes) / duration

        DispatchQueue.main.async {
            if var xfer = self.currentTransfer, xfer.status != .completed {
                xfer.status = .completed
                xfer.bytesSent = totalBytes
                xfer.completedAt = Date()
                xfer.speedBps = avgSpeed
                self.currentTransfer = xfer
                self.lastDeliveredFile = xfer.fileName
            }
            self.isTransferring = false
        }
    }

    private func handleFailure(message: String) {
        DispatchQueue.main.async {
            if var xfer = self.currentTransfer {
                xfer.status = .failed
                xfer.errorMessage = message
                self.currentTransfer = xfer
            }
            self.isTransferring = false
        }
        self.activeConnection?.cancel()
        self.activeConnection = nil
    }
}
