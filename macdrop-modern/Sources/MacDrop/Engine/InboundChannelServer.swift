import Foundation
import Darwin
import AppKit
import UserNotifications

/// Lightweight background companion server handling:
/// 1. UDP Port 6801 Discovery Responder: Answers `MACCOMPANION_DISCOVER` from 68k Macs
/// 2. TCP Port 6800 Reverse Channel & Peer Registry: Accepts 68k client channels (crucial for Basilisk II SLIRP NAT)
/// 3. Direct Streaming down open reverse channels at full wire speed
public final class InboundChannelServer {
    public static let shared = InboundChannelServer()

    private let queue = DispatchQueue(label: "se.aeroform.macdrop.channelserver", qos: .userInitiated)
    private var udpSource: DispatchSourceRead?
    private var tcpSource: DispatchSourceRead?
    private var udpFd: Int32 = -1
    private var tcpFd: Int32 = -1

    private var activeSessions: [String: ChannelSession] = [:]
    private let sessionsLock = NSLock()

    public init() {}

    public func start() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        queue.async { [weak self] in
            self?.startUDPDiscovery()
            self?.startTCPChannelServer()
        }
    }

    public func stop() {
        queue.async { [weak self] in
            guard let self = self else { return }
            self.udpSource?.cancel()
            self.tcpSource?.cancel()
            if self.udpFd >= 0 { close(self.udpFd); self.udpFd = -1 }
            if self.tcpFd >= 0 { close(self.tcpFd); self.tcpFd = -1 }

            self.sessionsLock.lock()
            for (_, session) in self.activeSessions {
                session.close()
            }
            self.activeSessions.removeAll()
            self.sessionsLock.unlock()
        }
    }

    // MARK: - UDP Port 6801 Discovery Responder

    private func startUDPDiscovery() {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else {
            print("[ChannelServer] Failed to create UDP socket")
            return
        }

        var opt: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = UInt16(6801).bigEndian
        addr.sin_addr.s_addr = INADDR_ANY.bigEndian

        let bindRes = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        guard bindRes == 0 else {
            print("[ChannelServer] UDP 6801 bind failed (another MacDrop or server may be running)")
            close(fd)
            return
        }

        self.udpFd = fd
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in
            self?.handleIncomingUDP()
        }
        source.setCancelHandler {
            close(fd)
        }
        source.resume()
        self.udpSource = source
        print("[ChannelServer] UDP Discovery Responder active on port 6801")
    }

    private func handleIncomingUDP() {
        guard udpFd >= 0 else { return }

        var buffer = [UInt8](repeating: 0, count: 1024)
        var clientAddr = sockaddr_in()
        var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        let bytesRead = withUnsafeMutablePointer(to: &clientAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                recvfrom(udpFd, &buffer, buffer.count, 0, $0, &addrLen)
            }
        }

        guard bytesRead > 0 else { return }
        let msg = String(decoding: buffer.prefix(bytesRead), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)

        guard msg.hasPrefix("MACCOMPANION_DISCOVER") || msg == "PING" else { return }

        // Determine sender IP
        var clientIPBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &clientAddr.sin_addr, &clientIPBuf, socklen_t(INET_ADDRSTRLEN))
        let senderIP = String(cString: clientIPBuf)

        // Resolve host IP to return:
        // - If Basilisk II SLIRP NAT (10.0.2.x), host gateway is 10.0.2.2
        // - If localhost loopback, use 127.0.0.1
        // - Otherwise, primary LAN IP
        let replyHost: String
        if senderIP.hasPrefix("10.0.2.") {
            replyHost = "10.0.2.2"
        } else if senderIP == "127.0.0.1" {
            replyHost = "127.0.0.1"
        } else {
            replyHost = Self.getLocalIPAddress()
        }

        let replyPayload = "MACCOMPANION_SERVER|\(replyHost)|6800|macdrop\n"
        guard let replyData = replyPayload.data(using: .ascii) else { return }

        _ = replyData.withUnsafeBytes { rawPtr in
            withUnsafePointer(to: &clientAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(udpFd, rawPtr.baseAddress, replyData.count, 0, $0, addrLen)
                }
            }
        }

        print("[ChannelServer] Replied to discovery from \(senderIP) -> \(replyPayload.trimmingCharacters(in: .whitespacesAndNewlines))")
    }

    // MARK: - TCP Port 6800 Channel Server

    private func startTCPChannelServer() {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else {
            print("[ChannelServer] Failed to create TCP socket")
            return
        }

        var opt: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = UInt16(6800).bigEndian
        addr.sin_addr.s_addr = INADDR_ANY.bigEndian

        let bindRes = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        guard bindRes == 0 else {
            print("[ChannelServer] TCP Port 6800 already in use (Node.js server or another app active, running in direct client mode)")
            close(fd)
            return
        }

        guard listen(fd, 10) == 0 else {
            print("[ChannelServer] TCP listen failed on 6800")
            close(fd)
            return
        }

        self.tcpFd = fd
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        source.setEventHandler { [weak self] in
            self?.acceptTCPConnection()
        }
        source.setCancelHandler {
            close(fd)
        }
        source.resume()
        self.tcpSource = source
        print("[ChannelServer] TCP Reverse Channel Server active on port 6800")
    }

    private func acceptTCPConnection() {
        guard tcpFd >= 0 else { return }

        var clientAddr = sockaddr_in()
        var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        let clientFd = withUnsafeMutablePointer(to: &clientAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                accept(tcpFd, $0, &addrLen)
            }
        }

        guard clientFd >= 0 else { return }

        // Optimize TCP socket performance & safety
        var nosigpipe: Int32 = 1
        setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, socklen_t(MemoryLayout<Int32>.size))
        var rcvBuf: Int32 = 262144 // 256 KB buffer for high wire speed
        setsockopt(clientFd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, socklen_t(MemoryLayout<Int32>.size))
        var noDelay: Int32 = 1
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &noDelay, socklen_t(MemoryLayout<Int32>.size))

        var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &clientAddr.sin_addr, &ipBuf, socklen_t(INET_ADDRSTRLEN))
        let remoteIP = String(cString: ipBuf)

        // Read initial HTTP request
        queue.async { [weak self] in
            self?.handleClientConnection(clientFd: clientFd, remoteIP: remoteIP)
        }
    }

    private func handleClientConnection(clientFd: Int32, remoteIP: String) {
        var buf = [UInt8](repeating: 0, count: 2048)
        let bytesRead = recv(clientFd, &buf, buf.count, 0)
        guard bytesRead > 0 else {
            close(clientFd)
            return
        }

        let requestStr = String(decoding: buf.prefix(bytesRead), as: UTF8.self)
        guard let firstLine = requestStr.components(separatedBy: "\r\n").first else {
            close(clientFd)
            return
        }

        let parts = firstLine.components(separatedBy: " ")
        guard parts.count >= 2 else {
            close(clientFd)
            return
        }

        let _ = parts[0]
        let path = parts[1]

        // 1. Reverse Push Channel: GET /api/macdrop/channel?id=...
        if path.hasPrefix("/api/macdrop/channel") {
            handleChannelRegistration(clientFd: clientFd, remoteIP: remoteIP, urlString: path)
            return
        }

        // 2. Peer List Request: GET /api/macdrop/peers
        if path.hasPrefix("/api/macdrop/peers") {
            let localIP = Self.getLocalIPAddress()
            let devName = DeviceSettingsManager.shared.effectiveDeviceName
            let machine = DeviceSettingsManager.shared.machineName
            let peerPayload = "PEER|modern_mac|\(devName)|\(machine)|\(localIP)|6800\n"

            // Encode payload using macOSRoman with lossy fallback so System 7 displays Swedish accents/umlauts natively
            let payloadData = peerPayload.data(using: .macOSRoman, allowLossyConversion: true) ?? Data(peerPayload.utf8)
            let header = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: \(payloadData.count)\r\nConnection: close\r\n\r\n"
            var fullResponse = header.data(using: .ascii) ?? Data()
            fullResponse.append(payloadData)

            fullResponse.withUnsafeBytes { raw in
                if let ptr = raw.baseAddress {
                    _ = send(clientFd, ptr, fullResponse.count, 0)
                }
            }
            close(clientFd)
            return
        }

        // 3. Status/Ping: GET /api/macdrop/status
        if path.hasPrefix("/api/macdrop/status") {
            let devName = DeviceSettingsManager.shared.effectiveDeviceName.replacingOccurrences(of: "\"", with: "\\\"")
            let json = "{\"status\":\"ok\",\"service\":\"MacDrop Modern Companion\",\"name\":\"\(devName)\"}\n"
            let resp = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: \(json.utf8.count)\r\nConnection: close\r\n\r\n\(json)"
            if let d = resp.data(using: .utf8) ?? resp.data(using: .ascii) {
                _ = d.withUnsafeBytes { send(clientFd, $0.baseAddress, d.count, 0) }
            }
            close(clientFd)
            return
        }

        // 4. Inbound Upload from 68k Mac: POST /api/macdrop/inbound/upload
        if path.hasPrefix("/api/macdrop/inbound/upload") || path.hasPrefix("/api/macdrop/upload") {
            handleInboundUpload(clientFd: clientFd, initialBuffer: buf, initialBytesRead: bytesRead, remoteIP: remoteIP)
            return
        }

        // Default 404
        let notFound = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
        if let d = notFound.data(using: .ascii) {
            _ = d.withUnsafeBytes { send(clientFd, $0.baseAddress, d.count, 0) }
        }
        close(clientFd)
    }

    private func handleChannelRegistration(clientFd: Int32, remoteIP: String, urlString: String) {
        guard let urlComponents = URLComponents(string: urlString) else {
            close(clientFd)
            return
        }

        let items = urlComponents.queryItems ?? []
        let rawId = items.first(where: { $0.name == "id" })?.value ?? ""
        let rawName = items.first(where: { $0.name == "name" })?.value ?? ""
        let rawMachine = items.first(where: { $0.name == "machine" })?.value ?? ""
        let rawVer = items.first(where: { $0.name == "ver" })?.value ?? "0.2.16"
        let reportedIP = items.first(where: { $0.name == "ip" })?.value ?? remoteIP

        let deviceId = rawId.isEmpty ? "mac_\(remoteIP.replacingOccurrences(of: ".", with: "_"))" : rawId
        let deviceName = rawName.isEmpty ? "Macintosh (\(remoteIP))" : rawName
        let machineName = rawMachine.isEmpty ? "Macintosh 68k" : rawMachine

        // Close any old session for this device
        sessionsLock.lock()
        if let old = activeSessions[deviceId] {
            old.close()
        }
        let session = ChannelSession(clientFd: clientFd, deviceId: deviceId, remoteIP: remoteIP, reportedIP: reportedIP)
        activeSessions[deviceId] = session
        sessionsLock.unlock()

        print("[ChannelServer] Vintage Mac Connected: \(deviceName) (\(machineName)) at \(remoteIP)")
        DebugLogger.log("[ChannelServer] Vintage Mac Connected: \(deviceName) (\(machineName)) at \(remoteIP), id=\(deviceId)")

        // Register device in DeviceDiscoveryManager
        DeviceDiscoveryManager.shared.registerChannelDevice(
            id: deviceId,
            name: deviceName,
            machine: machineName,
            ver: rawVer,
            ip: remoteIP.hasPrefix("10.0.2.") ? "10.0.2.15" : (reportedIP.isEmpty ? remoteIP : reportedIP)
        )

        // Install keepalive poll timer:
        // If no file transfer begins within 30 seconds, reply with HTTP 204 No Content
        // so the 68k Mac client closes and immediately reconnects, keeping the channel live.
        session.scheduleKeepalivePoll { [weak self, weak session] in
            guard let self = self, let s = session, s.isOpen, !s.isStreaming else { return }
            let pollResp = "HTTP/1.0 204 No Content\r\nX-MacDrop-Status: POLL\r\nConnection: close\r\n\r\n"
            if let d = pollResp.data(using: .ascii) {
                _ = d.withUnsafeBytes { send(s.clientFd, $0.baseAddress, d.count, 0) }
            }
            self.sessionsLock.lock()
            if self.activeSessions[s.deviceId]?.clientFd == s.clientFd {
                self.activeSessions.removeValue(forKey: s.deviceId)
            }
            self.sessionsLock.unlock()
            s.close()
        }
    }

    // MARK: - Streaming down Active Channel

    public func hasActiveChannel(for deviceId: String, ip: String? = nil) -> Bool {
        sessionsLock.lock()
        defer { sessionsLock.unlock() }
        if let s = activeSessions[deviceId], s.isOpen {
            return true
        }
        if activeSessions.values.contains(where: { $0.isOpen && ($0.deviceId == deviceId || (ip != nil && ($0.remoteIP == ip || $0.reportedIP == ip))) }) {
            return true
        }
        // Fallback: if there is any open channel session
        if activeSessions.values.contains(where: { $0.isOpen }) {
            return true
        }
        return false
    }

    public func streamToDevice(
        id: String,
        ip: String? = nil,
        fileName: String,
        fileData: Data,
        onProgress: @escaping (Int64, Double) -> Void,
        completion: @escaping (Error?) -> Void
    ) {
        sessionsLock.lock()
        var targetSession: ChannelSession? = nil

        if let s = activeSessions[id], s.isOpen {
            targetSession = s
        } else if let s = activeSessions.values.first(where: { $0.isOpen && ($0.deviceId == id || (ip != nil && ($0.remoteIP == ip || $0.reportedIP == ip))) }) {
            targetSession = s
        } else if let s = activeSessions.values.first(where: { $0.isOpen }) {
            targetSession = s
        }

        guard let session = targetSession, session.isOpen else {
            let activeKeys = activeSessions.keys.joined(separator: ", ")
            sessionsLock.unlock()
            let err = "No active channel for device id=\(id), ip=\(ip ?? "nil") (known active: [\(activeKeys)])"
            DebugLogger.log("[ChannelServer] \(err)")
            completion(NSError(domain: "MacDrop", code: -1, userInfo: [NSLocalizedDescriptionKey: err]))
            return
        }

        session.isStreaming = true
        session.cancelKeepalive()
        sessionsLock.unlock()

        DebugLogger.log("[ChannelServer] Found channel session for \(session.deviceId) (clientFd=\(session.clientFd), remoteIP=\(session.remoteIP), reportedIP=\(session.reportedIP))")

        queue.async {
            let clientFd = session.clientFd
            let transferId = "xfer_\(UUID().uuidString.prefix(8).lowercased())"

            // 1. Send HTTP Header (using UTF-8 with fallback so Swedish accents never fail)
            let cleanName = fileName.replacingOccurrences(of: "\"", with: "")
            let headerStr = "HTTP/1.0 200 OK\r\n" +
                            "Content-Type: application/octet-stream\r\n" +
                            "X-Transfer-Id: \(transferId)\r\n" +
                            "X-MacDrop-File: \(cleanName)\r\n" +
                            "Connection: close\r\n\r\n"

            guard let headerData = headerStr.data(using: .utf8) ?? headerStr.data(using: .ascii) else {
                DebugLogger.log("[ChannelServer] Header encoding error for \(cleanName)")
                completion(NSError(domain: "MacDrop", code: -2, userInfo: [NSLocalizedDescriptionKey: "Header encoding error"]))
                return
            }

            DebugLogger.log("[ChannelServer] Sending HTTP header (\(headerData.count) bytes) down clientFd \(clientFd)...")
            let headSent = headerData.withUnsafeBytes { send(clientFd, $0.baseAddress, headerData.count, 0) }
            guard headSent > 0 else {
                let errStr = "Failed to write header to channel (send returned \(headSent), errno=\(errno))"
                DebugLogger.log("[ChannelServer] \(errStr)")
                completion(NSError(domain: "MacDrop", code: -3, userInfo: [NSLocalizedDescriptionKey: errStr]))
                return
            }

            // 2. Stream Data in 32 KB Chunks
            let chunkSize = 32768
            let total = fileData.count
            var offset = 0
            let startTime = Date()

            DebugLogger.log("[ChannelServer] Streaming \(total) bytes in 32KB chunks...")
            while offset < total {
                let chunkEnd = min(offset + chunkSize, total)
                let chunk = fileData.subdata(in: offset..<chunkEnd)

                let written = chunk.withUnsafeBytes { send(clientFd, $0.baseAddress, chunk.count, 0) }
                if written <= 0 {
                    let errStr = "Socket error streaming chunk at offset \(offset)/\(total) (errno=\(errno))"
                    DebugLogger.log("[ChannelServer] \(errStr)")
                    completion(NSError(domain: "MacDrop", code: -4, userInfo: [NSLocalizedDescriptionKey: errStr]))
                    return
                }

                offset += written

                let elapsed = max(0.05, Date().timeIntervalSince(startTime))
                let speedBps = Double(offset) / elapsed
                onProgress(Int64(offset), speedBps)
            }

            DebugLogger.log("[ChannelServer] All \(total) bytes written! Shutting down write side...")
            // 3. Finalize: Shutdown write side to signal EOF to 68k Mac
            shutdown(clientFd, SHUT_WR)

            // Allow 68k Mac time to read EOF and flush to disk
            usleep(250_000)

            self.sessionsLock.lock()
            self.activeSessions.removeValue(forKey: session.deviceId)
            self.sessionsLock.unlock()

            session.close()
            DebugLogger.log("[ChannelServer] Stream completed successfully!")
            completion(nil)
        }
    }

    // MARK: - Inbound File Upload (68k Mac -> Modern Mac)

    private func handleInboundUpload(clientFd: Int32, initialBuffer: [UInt8], initialBytesRead: Int, remoteIP: String) {
        var headerData = Data(initialBuffer.prefix(initialBytesRead))
        var bodyData = Data()
        var contentLength: Int? = nil
        var fallbackFileName = "VintageMacFile"
        var clientName = "Macintosh"

        let delim = "\r\n\r\n".data(using: .ascii)!
        let delim2 = "\n\n".data(using: .ascii)!

        while true {
            if let range = headerData.range(of: delim) {
                let headerEnd = range.upperBound
                let headerBytes = headerData.subdata(in: 0..<range.lowerBound)
                if headerEnd < headerData.count {
                    bodyData = headerData.subdata(in: headerEnd..<headerData.count)
                }
                parseUploadHeaders(data: headerBytes, contentLength: &contentLength, fileName: &fallbackFileName, clientName: &clientName)
                break
            } else if let range = headerData.range(of: delim2) {
                let headerEnd = range.upperBound
                let headerBytes = headerData.subdata(in: 0..<range.lowerBound)
                if headerEnd < headerData.count {
                    bodyData = headerData.subdata(in: headerEnd..<headerData.count)
                }
                parseUploadHeaders(data: headerBytes, contentLength: &contentLength, fileName: &fallbackFileName, clientName: &clientName)
                break
            }

            var chunk = [UInt8](repeating: 0, count: 4096)
            let n = recv(clientFd, &chunk, chunk.count, 0)
            guard n > 0 else { break }
            headerData.append(contentsOf: chunk.prefix(n))
        }

        // Read remaining body until contentLength is reached or connection closed
        let targetLength = contentLength ?? Int.max
        let expectedTotal = contentLength.map { Int64($0) } ?? 0
        let startTime = Date()

        if let expected = contentLength, expected > 0 {
            bodyData.reserveCapacity(expected)
        }

        // 1. Notify user that a file is being received
        Self.postNotification(
            title: "MacDrop",
            subtitle: "Receiving file from \(clientName)...",
            body: "Downloading \"\(fallbackFileName)\""
        )

        // 2. Initialize UI progress HUD and menu bar progress ring
        DispatchQueue.main.async {
            var prog = TransferProgress()
            prog.fileName = fallbackFileName
            prog.targetName = clientName
            prog.fileSize = expectedTotal
            prog.bytesSent = Int64(bodyData.count)
            prog.status = .streaming
            prog.isReceiving = true
            prog.startedAt = startTime
            P2PTransferEngine.shared.currentTransfer = prog
            P2PTransferEngine.shared.isTransferring = true
            InvisibleNetPanelController.shared.showDropZone(animated: true)
            InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
        }

        var lastUpdate = Date()
        while bodyData.count < targetLength {
            var chunk = [UInt8](repeating: 0, count: 32768)
            let n = recv(clientFd, &chunk, chunk.count, 0)
            if n <= 0 { break }
            bodyData.append(contentsOf: chunk.prefix(n))

            let now = Date()
            if now.timeIntervalSince(lastUpdate) >= 0.20 || bodyData.count >= targetLength {
                lastUpdate = now
                let currentBytes = Int64(bodyData.count)
                let elapsed = max(0.05, now.timeIntervalSince(startTime))
                let speedBps = Double(currentBytes) / elapsed
                DispatchQueue.main.async {
                    P2PTransferEngine.shared.currentTransfer?.bytesSent = currentBytes
                    P2PTransferEngine.shared.currentTransfer?.speedBps = speedBps
                }
            }
        }

        DispatchQueue.main.async {
            P2PTransferEngine.shared.currentTransfer?.status = .finalizing
        }

        // Unpack MacBinary or save file into ~/Downloads
        let downloadsFolder = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSHomeDirectory() + "/Downloads")
        let savedName = Self.unpackMacBinary(data: bodyData, destFolder: downloadsFolder, fallbackName: fallbackFileName)

        // Send HTTP 200 response to 68k client
        let responseBody = "{\"status\":\"ok\",\"delivered\":true}\n"
        let response = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: \(responseBody.utf8.count)\r\nConnection: close\r\n\r\n\(responseBody)"
        if let d = response.data(using: .ascii) {
            _ = d.withUnsafeBytes { send(clientFd, $0.baseAddress, d.count, 0) }
        }
        close(clientFd)

        print("[ChannelServer] Successfully received upload from \(clientName): \(savedName) (\(bodyData.count) bytes)")

        // Notify user and update UI (notification sound is played by UNNotification, avoiding duplicate sounds)
        DispatchQueue.main.async {
            P2PTransferEngine.shared.currentTransfer?.status = .completed
            P2PTransferEngine.shared.currentTransfer?.bytesSent = Int64(bodyData.count)
            P2PTransferEngine.shared.currentTransfer?.completedAt = Date()
            P2PTransferEngine.shared.lastDeliveredFile = "Received \"\(savedName)\""
            InvisibleNetPanelController.shared.scheduleAutoDismissWatcher()
        }

        Self.postNotification(
            title: "MacDrop",
            subtitle: "File received from \(clientName)",
            body: "Saved \"\(savedName)\" to Downloads",
            sound: .default
        )
    }

    /// Helper to post system notifications using standard macOS banner layout
    public static func postNotification(title: String, subtitle: String, body: String, sound: UNNotificationSound? = nil) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.subtitle = subtitle
        content.body = body
        if let sound = sound {
            content.sound = sound
        }

        let req = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(req) { error in
            if let error = error {
                print("[ChannelServer] Notification post error: \(error)")
            }
        }
    }

    private func parseUploadHeaders(data: Data, contentLength: inout Int?, fileName: inout String, clientName: inout String) {
        guard let headerStr = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .ascii) else { return }
        for line in headerStr.components(separatedBy: "\r\n") {
            let lower = line.lowercased()
            if lower.hasPrefix("content-length:") {
                let val = line.components(separatedBy: ":").dropFirst().joined(separator: ":").trimmingCharacters(in: .whitespaces)
                contentLength = Int(val)
            } else if lower.hasPrefix("x-filename:") {
                let val = line.components(separatedBy: ":").dropFirst().joined(separator: ":").trimmingCharacters(in: .whitespaces)
                if !val.isEmpty { fileName = val }
            } else if lower.hasPrefix("x-client-name:") {
                let val = line.components(separatedBy: ":").dropFirst().joined(separator: ":").trimmingCharacters(in: .whitespaces)
                if !val.isEmpty { clientName = val }
            }
        }
    }

    private static func unpackMacBinary(data: Data, destFolder: URL, fallbackName: String) -> String {
        guard data.count >= 128 && data[0] == 0 else {
            let safeName = sanitizeFilename(fallbackName.isEmpty ? "MacDrop_File" : fallbackName)
            let destURL = uniqueFileURL(folder: destFolder, fileName: safeName)
            try? data.write(to: destURL)
            return destURL.lastPathComponent
        }

        let nameLen = Int(data[1])
        guard nameLen >= 1 && nameLen <= 63 else {
            let safeName = sanitizeFilename(fallbackName)
            let destURL = uniqueFileURL(folder: destFolder, fileName: safeName)
            try? data.write(to: destURL)
            return destURL.lastPathComponent
        }

        let nameBytes = data.subdata(in: 2..<(2 + nameLen))
        var fileName = String(data: nameBytes, encoding: .macOSRoman) ?? String(data: nameBytes, encoding: .ascii) ?? fallbackName
        fileName = sanitizeFilename(fileName)

        let typeBytes = data.subdata(in: 65..<69)
        let creatorBytes = data.subdata(in: 69..<73)
        let flagsHigh = data[73]
        let flagsLow = data[101]

        let dataLen = (Int(data[83]) << 24) | (Int(data[84]) << 16) | (Int(data[85]) << 8) | Int(data[86])
        let rsrcLen = (Int(data[87]) << 24) | (Int(data[88]) << 16) | (Int(data[89]) << 8) | Int(data[90])

        let dataOffset = 128
        let dataPadded = dataLen == 0 ? 0 : ((dataLen + 127) / 128) * 128
        let rsrcOffset = dataOffset + dataPadded

        guard data.count >= dataOffset + dataLen else {
            let destURL = uniqueFileURL(folder: destFolder, fileName: fileName)
            try? data.write(to: destURL)
            return destURL.lastPathComponent
        }

        let dataFork = data.subdata(in: dataOffset..<(dataOffset + dataLen))
        let destURL = uniqueFileURL(folder: destFolder, fileName: fileName)

        do {
            try dataFork.write(to: destURL)

            // Write Resource Fork if present
            if rsrcLen > 0 && data.count >= rsrcOffset + rsrcLen {
                let rsrcFork = data.subdata(in: rsrcOffset..<(rsrcOffset + rsrcLen))
                _ = rsrcFork.withUnsafeBytes { raw in
                    setxattr(destURL.path, "com.apple.ResourceFork", raw.baseAddress, rsrcFork.count, 0, 0)
                }
            }

            // Write FinderInfo (Type, Creator, Flags)
            var finderInfo = Data(count: 32)
            finderInfo.replaceSubrange(0..<4, with: typeBytes)
            finderInfo.replaceSubrange(4..<8, with: creatorBytes)
            finderInfo[8] = flagsHigh
            finderInfo[9] = flagsLow
            _ = finderInfo.withUnsafeBytes { raw in
                setxattr(destURL.path, "com.apple.FinderInfo", raw.baseAddress, finderInfo.count, 0, 0)
            }

            return destURL.lastPathComponent
        } catch {
            return fileName
        }
    }

    private static func uniqueFileURL(folder: URL, fileName: String) -> URL {
        var dest = folder.appendingPathComponent(fileName)
        guard FileManager.default.fileExists(atPath: dest.path) else { return dest }

        let baseName = dest.deletingPathExtension().lastPathComponent
        let ext = dest.pathExtension
        var counter = 2
        while FileManager.default.fileExists(atPath: dest.path) {
            let nextName = ext.isEmpty ? "\(baseName) \(counter)" : "\(baseName) \(counter).\(ext)"
            dest = folder.appendingPathComponent(nextName)
            counter += 1
        }
        return dest
    }

    private static func sanitizeFilename(_ name: String) -> String {
        return name
            .replacingOccurrences(of: ":", with: "_")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // MARK: - Helpers

    public static func getLocalIPAddress() -> String {
        var address: String = "127.0.0.1"
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let firstAddr = ifaddr else { return address }
        defer { freeifaddrs(ifaddr) }

        for ptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let flags = Int32(ptr.pointee.ifa_flags)
            let addr = ptr.pointee.ifa_addr.pointee
            if (flags & (IFF_UP | IFF_RUNNING)) != 0 && (flags & IFF_LOOPBACK) == 0 {
                if addr.sa_family == UInt8(AF_INET) {
                    var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                    if getnameinfo(ptr.pointee.ifa_addr, socklen_t(addr.sa_len), &hostname, socklen_t(hostname.count), nil, 0, NI_NUMERICHOST) == 0 {
                        let ip = String(cString: hostname)
                        if !ip.hasPrefix("169.254.") && !ip.hasPrefix("172.17.") && !ip.hasPrefix("192.168.97.") {
                            address = ip
                            break
                        }
                    }
                }
            }
        }
        return address
    }
}

/// Represents an active reverse channel connection from a vintage Mac
final class ChannelSession {
    let clientFd: Int32
    let deviceId: String
    let remoteIP: String
    let reportedIP: String
    var isOpen: Bool = true
    var isStreaming: Bool = false
    private var pollTimer: Timer?

    init(clientFd: Int32, deviceId: String, remoteIP: String, reportedIP: String = "") {
        self.clientFd = clientFd
        self.deviceId = deviceId
        self.remoteIP = remoteIP
        self.reportedIP = reportedIP

        // Disable SIGPIPE on this socket so broken connections return EPIPE rather than killing the app
        var yes: Int32 = 1
        setsockopt(clientFd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
    }

    func scheduleKeepalivePoll(action: @escaping () -> Void) {
        DispatchQueue.main.async { [weak self] in
            self?.pollTimer?.invalidate()
            self?.pollTimer = Timer.scheduledTimer(withTimeInterval: 30.0, repeats: false) { _ in
                action()
            }
        }
    }

    func cancelKeepalive() {
        DispatchQueue.main.async { [weak self] in
            self?.pollTimer?.invalidate()
            self?.pollTimer = nil
        }
    }

    func close() {
        cancelKeepalive()
        if isOpen {
            isOpen = false
            Darwin.close(clientFd)
        }
    }
}
