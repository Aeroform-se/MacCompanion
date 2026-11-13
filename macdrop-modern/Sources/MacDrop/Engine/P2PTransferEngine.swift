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
    private var activeUploadTask: URLSessionUploadTask?
    private var isCancelled: Bool = false
    private let queue = DispatchQueue(label: "se.aeroform.macdrop.p2p", qos: .userInitiated)
    private var clearLastDeliveredTimer: Timer?
    private var dismissHUDTimer: Timer?

    public init() {}

    /// Sets a temporary delivery notice (e.g. "Sent ...", "Received ...") and clears it automatically after 4 seconds.
    public func setDeliveredNotice(_ message: String) {
        DispatchQueue.main.async {
            self.clearLastDeliveredTimer?.invalidate()
            self.lastDeliveredFile = message
            self.clearLastDeliveredTimer = Timer.scheduledTimer(withTimeInterval: 4.0, repeats: false) { [weak self] _ in
                DispatchQueue.main.async {
                    self?.lastDeliveredFile = nil
                    InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
                }
            }
        }
    }

    /// Resets all temporary transfer HUD & notice timers and clears the delivery notice.
    public func resetTransferTimers() {
        dismissHUDTimer?.invalidate()
        dismissHUDTimer = nil
        clearLastDeliveredTimer?.invalidate()
        clearLastDeliveredTimer = nil
        lastDeliveredFile = nil
    }

    /// Marks an inbound transfer completed, sets delivered notice, and schedules the HUD to revert to device list after 4 seconds.
    public func markInboundCompleted(fileName: String, bytes: Int64) {
        if var xfer = self.currentTransfer {
            xfer.status = .completed
            xfer.bytesSent = bytes
            xfer.completedAt = Date()
            self.currentTransfer = xfer
        }
        self.isTransferring = false
        self.setDeliveredNotice("Received \"\(fileName)\"")

        self.dismissHUDTimer?.invalidate()
        self.dismissHUDTimer = Timer.scheduledTimer(withTimeInterval: 4.0, repeats: false) { [weak self] _ in
            DispatchQueue.main.async {
                if self?.currentTransfer?.status == .completed || self?.currentTransfer?.status == .failed {
                    self?.currentTransfer = nil
                    InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
                }
            }
        }
    }

    /// Cancels any currently active transfer.
    public func cancelCurrentTransfer() {
        queue.async { [weak self] in
            guard let self = self else { return }
            self.isCancelled = true
            self.activeConnection?.cancel()
            self.activeConnection = nil
            self.activeUploadTask?.cancel()
            self.activeUploadTask = nil

            DispatchQueue.main.async {
                if var xfer = self.currentTransfer {
                    xfer.status = .cancelled
                    xfer.errorMessage = "Cancelled by user"
                    self.currentTransfer = xfer
                }
                self.isTransferring = false

                self.dismissHUDTimer?.invalidate()
                self.dismissHUDTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: false) { [weak self] _ in
                    DispatchQueue.main.async {
                        if self?.currentTransfer?.status == .cancelled {
                            self?.currentTransfer = nil
                            InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
                        }
                    }
                }
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
            self.resetTransferTimers()
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

                // Check if target is reachable via an active MacCompanion relay server (e.g. 192.168.1.40)
                let relayServer = target.relayServerIP ?? DeviceDiscoveryManager.shared.serverForClient(target.id, ip: target.ip)
                if let serverIP = relayServer {
                    DebugLogger.log("[P2P] Streaming via MacCompanion relay server at \(serverIP) for \(target.name)...")
                    self.streamViaServer(serverIP: serverIP, target: target, fileName: fileName, data: encoded.fullMacBinaryData)
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

    // MARK: - 68k Peer-to-Peer Relay Stream

    public func relayMacBinary(data: Data, fileName: String, senderName: String, to target: DiscoveredMac) {
        queue.async { [weak self] in
            guard let self = self else { return }
            let startTime = Date()
            let totalBytes = Int64(data.count)

            DispatchQueue.main.async {
                self.resetTransferTimers()
                var prog = TransferProgress()
                prog.fileName = fileName
                prog.targetName = "\(senderName) → \(target.name)"
                prog.fileSize = totalBytes
                prog.bytesSent = 0
                prog.status = .streaming
                prog.isReceiving = false
                prog.startedAt = startTime
                self.currentTransfer = prog
                self.isTransferring = true
                InvisibleNetPanelController.shared.showDropZone(animated: true)
                InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
            }

            // Check if target has an active reverse channel
            if InboundChannelServer.shared.hasActiveChannel(for: target.id, ip: target.ip) {
                DebugLogger.log("[P2P] Relaying down active reverse channel to \(target.name)...")
                InboundChannelServer.shared.streamToDevice(
                    id: target.id,
                    ip: target.ip,
                    fileName: fileName,
                    fileData: data,
                    onProgress: { [weak self] sent, speed in
                        DispatchQueue.main.async {
                            self?.currentTransfer?.bytesSent = sent
                            self?.currentTransfer?.speedBps = speed
                        }
                    },
                    completion: { [weak self] err in
                        guard let self = self else { return }
                        if let err = err {
                            DebugLogger.log("[P2P] Peer relay stream failed: \(err)")
                            self.handleFailure(message: "Relay failed: \(err.localizedDescription)")
                        } else {
                            DebugLogger.log("[P2P] Peer relay completed successfully!")
                            self.markCompleted(totalBytes: totalBytes, startTime: startTime)
                        }
                    }
                )
                return
            }

            // Direct TCP 6803 stream if reachable
            DebugLogger.log("[P2P] Relaying via direct TCP to \(target.ip):\(target.port)...")
            let host = NWEndpoint.Host(target.ip)
            let port = NWEndpoint.Port(integerLiteral: UInt16(target.port))
            let tcpOptions = NWProtocolTCP.Options()
            tcpOptions.noDelay = true
            let params = NWParameters(tls: nil, tcp: tcpOptions)
            let connection = NWConnection(host: host, port: port, using: params)
            self.activeConnection = connection
            self.streamData(connection: connection, data: data, target: target)
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
                if let fallbackServer = DeviceDiscoveryManager.shared.serverForClient(target.id, ip: target.ip) {
                    DebugLogger.log("[P2P] Direct TCP failed (\(err)), falling back to relay server at \(fallbackServer)...")
                    self.streamViaServer(serverIP: fallbackServer, target: target, fileName: target.name, data: data)
                    return
                }
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

        var completedOnce = false
        let safeComplete = { [weak self] in
            guard let self = self, !completedOnce else { return }
            completedOnce = true
            self.markCompleted(totalBytes: totalBytes, startTime: startTime)
            connection.cancel()
            self.activeConnection = nil
        }

        // Wait up to 3 seconds for Mac to close its socket after flushing to disk
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1024) { (_, _, _, _) in
            safeComplete()
        }

        // Fallback safety timer if client closes silently
        queue.asyncAfter(deadline: .now() + 2.5) {
            safeComplete()
        }
    }

    private func markCompleted(totalBytes: Int64, startTime: Date) {
        let duration = max(0.1, Date().timeIntervalSince(startTime))
        let avgSpeed = Double(totalBytes) / duration

        DispatchQueue.main.async {
            var fileName = "file"
            var targetName = "Macintosh"
            if var xfer = self.currentTransfer, xfer.status != .completed {
                xfer.status = .completed
                xfer.bytesSent = totalBytes
                xfer.completedAt = Date()
                xfer.speedBps = avgSpeed
                fileName = xfer.fileName
                targetName = xfer.targetName
                self.currentTransfer = xfer
            }
            self.isTransferring = false
            self.setDeliveredNotice("Sent \"\(fileName)\"")

            self.dismissHUDTimer?.invalidate()
            self.dismissHUDTimer = Timer.scheduledTimer(withTimeInterval: 4.0, repeats: false) { [weak self] _ in
                DispatchQueue.main.async {
                    if self?.currentTransfer?.status == .completed || self?.currentTransfer?.status == .failed {
                        self?.currentTransfer = nil
                        InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
                    }
                }
            }

            // Post system notification alert for sent file
            InboundChannelServer.postNotification(
                title: "MacDrop X",
                subtitle: "File delivered to \(targetName)",
                body: "Sent \"\(fileName)\" successfully",
                sound: .default
            )
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

            self.dismissHUDTimer?.invalidate()
            self.dismissHUDTimer = Timer.scheduledTimer(withTimeInterval: 4.0, repeats: false) { [weak self] _ in
                DispatchQueue.main.async {
                    if self?.currentTransfer?.status == .failed {
                        self?.currentTransfer = nil
                        InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
                    }
                }
            }
        }
        self.activeConnection?.cancel()
        self.activeConnection = nil
        self.activeUploadTask?.cancel()
        self.activeUploadTask = nil
    }

    // MARK: - MacCompanion Server Relay Streaming

    private func streamViaServer(serverIP: String, target: DiscoveredMac, fileName: String, data: Data) {
        let startTime = Date()
        DispatchQueue.main.async {
            if var xfer = self.currentTransfer {
                xfer.status = .streaming
                xfer.fileSize = Int64(data.count)
                xfer.bytesSent = 0
                self.currentTransfer = xfer
            }
        }

        guard let encodedName = fileName.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed),
              let url = URL(string: "http://\(serverIP):6800/api/macdrop/upload?target_id=\(target.id)&filename=\(encodedName)") else {
            self.handleFailure(message: "Invalid upload URL for server \(serverIP)")
            return
        }

        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/x-macbinary", forHTTPHeaderField: "Content-Type")
        req.setValue(fileName, forHTTPHeaderField: "X-Filename")
        req.setValue(target.id, forHTTPHeaderField: "X-Target-Id")
        req.timeoutInterval = 300

        let delegate = ServerUploadProgressDelegate { [weak self] bytesSent, totalExpected in
            DispatchQueue.main.async {
                guard let self = self, var xfer = self.currentTransfer, xfer.status == .streaming else { return }
                xfer.bytesSent = bytesSent
                let elapsed = max(0.1, Date().timeIntervalSince(startTime))
                xfer.speedBps = Double(bytesSent) / elapsed
                self.currentTransfer = xfer
            }
        }

        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        let task = session.uploadTask(with: req, from: data) { [weak self] resData, response, error in
            session.finishTasksAndInvalidate()
            guard let self = self else { return }

            if let error = error {
                DebugLogger.log("[P2P] Server upload error: \(error)")
                self.handleFailure(message: "Transfer failed: \(error.localizedDescription)")
                return
            }

            guard let httpRes = response as? HTTPURLResponse, (200...299).contains(httpRes.statusCode) else {
                let status = (response as? HTTPURLResponse)?.statusCode ?? 500
                let errMsg: String
                if let d = resData, let json = try? JSONSerialization.jsonObject(with: d) as? [String: Any], let msg = json["error"] as? String {
                    errMsg = msg
                } else {
                    errMsg = "Server returned HTTP \(status)"
                }
                DebugLogger.log("[P2P] Server upload rejected: \(errMsg)")
                self.handleFailure(message: errMsg)
                return
            }

            DebugLogger.log("[P2P] Server relay completed successfully!")
            self.markCompleted(totalBytes: Int64(data.count), startTime: startTime)
        }
        self.activeUploadTask = task
        task.resume()
    }
}

final class ServerUploadProgressDelegate: NSObject, URLSessionTaskDelegate {
    private let onProgress: (Int64, Int64) -> Void

    init(onProgress: @escaping (Int64, Int64) -> Void) {
        self.onProgress = onProgress
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didSendBodyData bytesSent: Int64, totalBytesSent: Int64, totalBytesExpectedToSend: Int64) {
        onProgress(totalBytesSent, totalBytesExpectedToSend)
    }
}
