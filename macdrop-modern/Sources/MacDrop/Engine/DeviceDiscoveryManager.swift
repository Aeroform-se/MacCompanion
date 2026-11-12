import Foundation
import Network
import Combine
import Darwin

public enum ManualAddResult {
    case success(String)
    case failure(String)
}

/// Discovers vintage Macintosh clients on the local network via:
/// 1. UDP Broadcast prober on Port 6801 (`MACCOMPANION_DISCOVER` -> `MACCOMPANION_SERVER|<ip>|<port>|<modules>`)
/// 2. MacCompanion Server API (`/api/macdrop/status` on port 6800 for discovered & known servers)
/// 3. Direct TCP handshake on Port 6803 (`MACDROP_HELLO` -> `MACDROP_PONG`)
/// 4. Inbound reverse channel connections on local port 6800
public final class DeviceDiscoveryManager: ObservableObject {
    public static let shared = DeviceDiscoveryManager()

    @Published public var devices: [DiscoveredMac] = []
    @Published public var selectedDevice: DiscoveredMac?
    @Published public var isScanning: Bool = false
    @Published public var isManualRefreshing: Bool = false
    @Published public var knownServers: [String] = []
    @Published public var activeServer: String?

    private var scanTimer: Timer?
    private let queue = DispatchQueue(label: "se.aeroform.macdrop.discovery", qos: .utility)
    private let devicesKey = "macdrop_known_devices_v1"
    private let serversKey = "macdrop_known_servers_v1"

    public init() {
        loadPersistedServers()
        loadPersistedDevices()
        startPeriodicScan()
        scanAll(isManual: false)
    }

    public func startPeriodicScan() {
        scanTimer?.invalidate()
        scanTimer = Timer.scheduledTimer(withTimeInterval: 12.0, repeats: true) { [weak self] _ in
            self?.scanAll(isManual: false)
        }
    }

    /// Kicks off all discovery channels in parallel.
    /// `isManual`: true only when user explicitly clicks "Refresh" in the menu.
    public func scanAll(isManual: Bool = false) {
        DispatchQueue.main.async {
            self.isScanning = true
            if isManual {
                self.isManualRefreshing = true
            }
        }

        queue.async { [weak self] in
            guard let self = self else { return }

            let group = DispatchGroup()

            // 1. Discover any MacCompanion servers via UDP 6801 broadcast
            group.enter()
            self.discoverServersViaUDP { discovered in
                DispatchQueue.main.async {
                    for s in discovered {
                        if !self.knownServers.contains(s) {
                            self.knownServers.append(s)
                        }
                    }
                    self.persistServers()
                }
                group.leave()
            }

            // Wait for UDP broadcast response before querying servers
            group.wait()

            let queryGroup = DispatchGroup()

            // 2. Query all known MacCompanion servers (e.g. 192.168.1.40, local, etc.)
            queryGroup.enter()
            self.queryAllServers { queryGroup.leave() }

            // 3. Probe known devices directly on TCP port 6803
            queryGroup.enter()
            self.probeKnownDevices { queryGroup.leave() }

            queryGroup.notify(queue: .main) {
                self.isScanning = false
                self.isManualRefreshing = false
                self.persistDevices()
                if self.selectedDevice == nil, let first = self.devices.first(where: { $0.isOnline }) {
                    self.selectedDevice = first
                }
            }
        }
    }

    // MARK: - UDP 6801 Broadcast Prober for MacCompanion Servers

    private func discoverServersViaUDP(completion: @escaping ([String]) -> Void) {
        let sock = socket(AF_INET, SOCK_DGRAM, 0)
        guard sock >= 0 else {
            completion([])
            return
        }
        defer { close(sock) }

        var opt: Int32 = 1
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, socklen_t(MemoryLayout<Int32>.size))

        var tv = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        let probeStr = "MACCOMPANION_DISCOVER\n"
        let probeData = [UInt8](probeStr.utf8)

        var broadcastAddrs = ["255.255.255.255"]
        if let localIP = Self.getLocalIPv4() {
            let parts = localIP.components(separatedBy: ".")
            if parts.count == 4 {
                broadcastAddrs.append("\(parts[0]).\(parts[1]).\(parts[2]).255")
            }
        }

        for bcast in broadcastAddrs {
            var addr = sockaddr_in()
            addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = in_port_t(6801).bigEndian
            inet_pton(AF_INET, bcast, &addr.sin_addr)

            _ = withUnsafePointer(to: &addr) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                    sendto(sock, probeData, probeData.count, 0, saPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }

        var discovered = Set<String>()
        var buf = [UInt8](repeating: 0, count: 1024)
        var from = sockaddr_in()
        var fromLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        let deadline = Date().addingTimeInterval(1.2)
        while Date() < deadline {
            let n = withUnsafeMutablePointer(to: &from) { ptr in
                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                    recvfrom(sock, &buf, buf.count, 0, saPtr, &fromLen)
                }
            }
            if n <= 0 { break }

            if let text = String(bytes: buf.prefix(n), encoding: .utf8) ?? String(bytes: buf.prefix(n), encoding: .ascii) {
                if text.contains("MACCOMPANION_SERVER") {
                    let parts = text.trimmingCharacters(in: .whitespacesAndNewlines).components(separatedBy: "|")
                    if parts.count > 1 {
                        let serverIP = parts[1].trimmingCharacters(in: .whitespacesAndNewlines)
                        if !serverIP.isEmpty && serverIP != "127.0.0.1" && serverIP != Self.getLocalIPv4() {
                            discovered.insert(serverIP)
                        }
                    }
                }
            }
        }

        completion(Array(discovered))
    }

    // MARK: - MacCompanion Server API Query

    private func queryAllServers(completion: @escaping () -> Void) {
        var serversToQuery = Set(knownServers)
        serversToQuery.insert("192.168.1.40")

        let group = DispatchGroup()

        for srv in serversToQuery {
            // Skip local machine IP for server query if port 6800 is our own InboundChannelServer
            if srv == "127.0.0.1" || srv == Self.getLocalIPv4() {
                continue
            }

            group.enter()
            queryServer(srv) {
                group.leave()
            }
        }

        group.notify(queue: queue) {
            completion()
        }
    }

    public func queryServer(_ serverIP: String, completion: @escaping () -> Void) {
        guard let url = URL(string: "http://\(serverIP):6800/api/macdrop/status") else {
            completion()
            return
        }

        var req = URLRequest(url: url)
        req.timeoutInterval = 1.5

        URLSession.shared.dataTask(with: req) { [weak self] data, response, error in
            guard let self = self, let data = data, error == nil else {
                completion()
                return
            }

            do {
                if let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    DispatchQueue.main.async {
                        self.activeServer = serverIP
                        if !self.knownServers.contains(serverIP) {
                            self.knownServers.append(serverIP)
                            self.persistServers()
                        }
                    }

                    var activeServerIps = Set<String>()

                    // 1. Process active connected clients
                    if let clients = json["clients"] as? [[String: Any]] {
                        for c in clients {
                            let id = c["id"] as? String ?? ""
                            let name = c["name"] as? String ?? "Macintosh"
                            let machine = c["machine"] as? String ?? "Macintosh 68k"
                            let ip = c["ip"] as? String ?? ""
                            let ver = c["version"] as? String ?? ""
                            let online = c["online"] as? Bool ?? true

                            if !ip.isEmpty && ip != "127.0.0.1" && online {
                                activeServerIps.insert(ip)
                                let dev = DiscoveredMac(
                                    id: id.isEmpty ? "mac_\(ip.replacingOccurrences(of: ".", with: "_"))" : id,
                                    name: name,
                                    machine: machine,
                                    ip: ip,
                                    port: 6803,
                                    version: ver,
                                    isOnline: true,
                                    lastSeen: Date(),
                                    source: "server_api",
                                    relayServerIP: serverIP
                                )
                                self.updateOrAddDevice(dev)
                            }
                        }
                    }

                    // 2. Process devices list from server
                    if let srvDevices = json["devices"] as? [[String: Any]] {
                        for d in srvDevices {
                            let online = d["online"] as? Bool ?? false
                            guard online else { continue }
                            let id = d["id"] as? String ?? ""
                            let name = d["name"] as? String ?? "Macintosh"
                            let machine = d["machine"] as? String ?? "Macintosh 68k"
                            let ip = d["ip"] as? String ?? ""
                            let ver = d["version"] as? String ?? ""

                            if !ip.isEmpty && ip != "127.0.0.1" {
                                activeServerIps.insert(ip)
                                let dev = DiscoveredMac(
                                    id: id.isEmpty ? "mac_\(ip.replacingOccurrences(of: ".", with: "_"))" : id,
                                    name: name,
                                    machine: machine,
                                    ip: ip,
                                    port: 6803,
                                    version: ver,
                                    isOnline: true,
                                    lastSeen: Date(),
                                    source: "server_api",
                                    relayServerIP: serverIP
                                )
                                self.updateOrAddDevice(dev)
                            }
                        }
                    }

                    // 3. Purge any stale server_api devices that are no longer reported by this server
                    DispatchQueue.main.async {
                        self.devices.removeAll { dev in
                            dev.source == "server_api" && dev.relayServerIP == serverIP && !activeServerIps.contains(dev.ip)
                        }
                        self.persistDevices()
                    }
                }
            } catch {}
            completion()
        }.resume()
    }

    // MARK: - Direct TCP 6803 Probing

    private func probeKnownDevices(completion: @escaping () -> Void) {
        let currentIps = devices.map { $0.ip }
        var targetIps = Set(currentIps)

        targetIps.insert("192.168.97.1")

        let group = DispatchGroup()
        for ip in targetIps {
            if ip.hasPrefix("10.0.2.") {
                continue
            }
            if let dev = devices.first(where: { $0.ip == ip }),
               InboundChannelServer.shared.hasActiveChannel(for: dev.id) {
                continue
            }

            group.enter()
            probeSingleIP(ip) { [weak self] online, meta in
                guard let self = self else { group.leave(); return }
                if let meta = meta {
                    self.updateOrAddDevice(meta)
                } else if !online {
                    self.markDeviceOffline(ip: ip)
                }
                group.leave()
            }
        }

        group.notify(queue: queue) {
            completion()
        }
    }

    public func probeSingleIP(_ ip: String, completion: @escaping (Bool, DiscoveredMac?) -> Void) {
        let host = NWEndpoint.Host(ip)
        let port = NWEndpoint.Port(integerLiteral: 6803)

        let tcp = NWProtocolTCP.Options()
        tcp.connectionTimeout = 1
        let params = NWParameters(tls: nil, tcp: tcp)
        let conn = NWConnection(host: host, port: port, using: params)

        var finished = false
        let finishOnce = { (online: Bool, mac: DiscoveredMac?) in
            if !finished {
                finished = true
                conn.cancel()
                completion(online, mac)
            }
        }

        conn.stateUpdateHandler = { state in
            switch state {
            case .ready:
                let helloData = Data("MACDROP_HELLO\n".utf8)
                conn.send(content: helloData, completion: .contentProcessed { err in
                    if err != nil {
                        finishOnce(false, nil)
                        return
                    }

                    conn.receive(minimumIncompleteLength: 1, maximumLength: 512) { data, _, _, recvErr in
                        guard let data = data, let text = String(data: data, encoding: .utf8), recvErr == nil else {
                            let mac = DiscoveredMac(name: "Macintosh (\(ip))", machine: "Macintosh 68k", ip: ip, isOnline: true)
                            finishOnce(true, mac)
                            return
                        }

                        if text.contains("MACDROP_PONG") {
                            let parts = text.trimmingCharacters(in: .whitespacesAndNewlines).components(separatedBy: "|")
                            let id = parts.count > 1 ? parts[1] : nil
                            let name = parts.count > 2 ? parts[2] : "Macintosh (\(ip))"
                            let machine = parts.count > 3 ? parts[3] : "Macintosh 68k"
                            let ver = parts.count > 4 ? parts[4] : "0.2.16"

                            let dev = DiscoveredMac(
                                id: id,
                                name: name,
                                machine: machine,
                                ip: ip,
                                port: 6803,
                                version: ver,
                                isOnline: true,
                                lastSeen: Date(),
                                source: "direct_tcp"
                            )
                            finishOnce(true, dev)
                        } else {
                            let dev = DiscoveredMac(name: "Macintosh (\(ip))", machine: "Macintosh 68k", ip: ip, isOnline: true)
                            finishOnce(true, dev)
                        }
                    }
                })

            case .failed, .cancelled:
                finishOnce(false, nil)

            default:
                break
            }
        }

        self.queue.asyncAfter(deadline: .now() + 1.2) {
            finishOnce(false, nil)
        }

        conn.start(queue: self.queue)
    }

    // MARK: - Manual Device Addition & Probing with Full Feedback

    public func addManualDevice(ip: String, name: String = "", completion: ((ManualAddResult) -> Void)? = nil) {
        let cleanIp = ip.trimmingCharacters(in: .whitespacesAndNewlines)
        guard isValidIP(cleanIp) else {
            completion?(.failure("Please enter a valid IPv4 address (e.g. 192.168.1.10)"))
            return
        }

        // 1. First probe direct TCP port 6803 (Vintage Mac)
        probeSingleIP(cleanIp) { [weak self] online, dev in
            guard let self = self else { return }

            if online, let dev = dev {
                var finalDev = dev
                if !name.isEmpty { finalDev.name = name }
                finalDev.source = "manual"
                self.updateOrAddDevice(finalDev)
                completion?(.success("Connected to vintage Mac: \(finalDev.name)!"))
                return
            }

            // 2. If 6803 not responding, probe port 6800 to see if this IP is a MacCompanion Server
            self.probeServerPort(cleanIp) { isServer, clientCount in
                if isServer {
                    DispatchQueue.main.async {
                        self.activeServer = cleanIp
                        if !self.knownServers.contains(cleanIp) {
                            self.knownServers.append(cleanIp)
                            self.persistServers()
                        }
                    }
                    self.queryServer(cleanIp) {
                        DispatchQueue.main.async {
                            completion?(.success("Connected to MacCompanion Server! (\(clientCount) client(s) found)"))
                        }
                    }
                    return
                }

                // 3. Check if cleanIp is known to any of our already connected MacCompanion servers
                if let matched = self.devices.first(where: { $0.ip == cleanIp && $0.source == "server_api" }) {
                    DispatchQueue.main.async {
                        self.selectedDevice = matched
                        completion?(.success("Found \(matched.name) connected via MacCompanion Server!"))
                    }
                    return
                }

                // 4. Otherwise, still add as a manual target so user sees it in list
                let manualDev = DiscoveredMac(
                    id: "manual_\(cleanIp.replacingOccurrences(of: ".", with: "_"))",
                    name: name.isEmpty ? "Macintosh (\(cleanIp))" : name,
                    machine: "Vintage Macintosh",
                    ip: cleanIp,
                    port: 6803,
                    version: "",
                    isOnline: false,
                    lastSeen: Date(),
                    source: "manual"
                )

                DispatchQueue.main.async {
                    if let idx = self.devices.firstIndex(where: { $0.ip == cleanIp }) {
                        self.devices[idx].name = manualDev.name
                        self.devices[idx].source = "manual"
                    } else {
                        self.devices.append(manualDev)
                    }
                    self.selectedDevice = self.devices.first(where: { $0.ip == cleanIp })
                    self.persistDevices()
                    completion?(.failure("Added \(cleanIp). Port 6803 is not responding yet (ensure MacDrop is running on the Mac)."))
                }
            }
        }
    }

    private func probeServerPort(_ ip: String, completion: @escaping (Bool, Int) -> Void) {
        guard let url = URL(string: "http://\(ip):6800/api/macdrop/status") else {
            completion(false, 0)
            return
        }
        var req = URLRequest(url: url)
        req.timeoutInterval = 1.2

        URLSession.shared.dataTask(with: req) { data, response, error in
            guard let data = data, error == nil,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let id = json["id"] as? String, id == "macdrop" else {
                completion(false, 0)
                return
            }
            let clients = json["clients"] as? [[String: Any]] ?? []
            completion(true, clients.count)
        }.resume()
    }

    /// Removes a manually added device.
    public func removeDevice(id: String) {
        DispatchQueue.main.async {
            self.devices.removeAll { $0.id == id }
            if self.selectedDevice?.id == id {
                self.selectedDevice = self.devices.first(where: { $0.isOnline })
            }
            self.persistDevices()
        }
    }

    /// Returns the relay server IP if target is reachable via an active MacCompanion server.
    public func serverForClient(_ id: String, ip: String) -> String? {
        if let dev = devices.first(where: { $0.id == id || $0.ip == ip }), let relay = dev.relayServerIP {
            return relay
        }
        if let active = activeServer {
            return active
        }
        return knownServers.first(where: { $0 != "127.0.0.1" })
    }

    // MARK: - Inbound Reverse Channel Device Registration

    public func registerChannelDevice(id: String, name: String, machine: String, ver: String, ip: String) {
        DispatchQueue.main.async {
            let dev = DiscoveredMac(
                id: id,
                name: name.isEmpty ? "Macintosh (\(ip))" : name,
                machine: machine.isEmpty ? "Macintosh 68k" : machine,
                ip: ip,
                port: 6803,
                version: ver,
                isOnline: true,
                lastSeen: Date(),
                source: "reverse_channel"
            )

            if let idx = self.devices.firstIndex(where: { $0.id == dev.id || $0.ip == ip }) {
                self.devices[idx].id = dev.id
                self.devices[idx].ip = dev.ip
                self.devices[idx].isOnline = true
                self.devices[idx].name = dev.name
                self.devices[idx].machine = dev.machine
                self.devices[idx].version = dev.version
                self.devices[idx].lastSeen = Date()
                self.devices[idx].source = "reverse_channel"
            } else {
                self.devices.append(dev)
            }

            if self.selectedDevice == nil || self.selectedDevice?.id == dev.id {
                self.selectedDevice = self.devices.first(where: { $0.id == dev.id })
            }

            self.persistDevices()
        }
    }

    // MARK: - State Management & Persistence

    private func updateOrAddDevice(_ dev: DiscoveredMac) {
        DispatchQueue.main.async {
            if let idx = self.devices.firstIndex(where: { $0.ip == dev.ip }) {
                if !dev.id.hasPrefix("device_") || self.devices[idx].id.hasPrefix("device_") {
                    self.devices[idx].id = dev.id
                }
                self.devices[idx].name = dev.name
                self.devices[idx].machine = dev.machine
                self.devices[idx].version = dev.version
                self.devices[idx].isOnline = dev.isOnline
                self.devices[idx].lastSeen = Date()
                self.devices[idx].source = dev.source
                if let relay = dev.relayServerIP {
                    self.devices[idx].relayServerIP = relay
                }
            } else {
                self.devices.append(dev)
            }
            if self.selectedDevice?.ip == dev.ip {
                self.selectedDevice = dev
            }
            self.persistDevices()
        }
    }

    private func markDeviceOffline(ip: String) {
        DispatchQueue.main.async {
            if let idx = self.devices.firstIndex(where: { $0.ip == ip }) {
                let dev = self.devices[idx]
                if InboundChannelServer.shared.hasActiveChannel(for: dev.id) {
                    return
                }
                if dev.source == "reverse_channel" && Date().timeIntervalSince(dev.lastSeen) < 45.0 {
                    return
                }
                if dev.source == "server_api" && Date().timeIntervalSince(dev.lastSeen) < 30.0 {
                    return
                }

                self.devices[idx].isOnline = false
                if self.selectedDevice?.ip == ip {
                    self.selectedDevice?.isOnline = false
                }
            }
        }
    }

    private func persistDevices() {
        if let data = try? JSONEncoder().encode(devices) {
            UserDefaults.standard.set(data, forKey: devicesKey)
        }
    }

    private func loadPersistedDevices() {
        if let data = UserDefaults.standard.data(forKey: devicesKey),
           let saved = try? JSONDecoder().decode([DiscoveredMac].self, from: data) {
            var seenIps = Set<String>()
            var unique: [DiscoveredMac] = []
            for d in saved {
                if !seenIps.contains(d.ip) {
                    seenIps.insert(d.ip)
                    unique.append(d)
                }
            }
            self.devices = unique
        }
    }

    private func persistServers() {
        UserDefaults.standard.set(knownServers, forKey: serversKey)
    }

    private func loadPersistedServers() {
        if let saved = UserDefaults.standard.stringArray(forKey: serversKey), !saved.isEmpty {
            self.knownServers = saved
        } else {
            self.knownServers = ["192.168.1.40", "127.0.0.1"]
        }
    }

    private func isValidIP(_ ip: String) -> Bool {
        let parts = ip.components(separatedBy: ".")
        guard parts.count == 4 else { return false }
        return parts.allSatisfy { Int($0) != nil && (0...255).contains(Int($0)!) }
    }

    public static func getLocalIPv4() -> String? {
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let firstAddr = ifaddr else { return nil }
        defer { freeifaddrs(ifaddr) }

        for ptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let flags = Int32(ptr.pointee.ifa_flags)
            if (flags & (IFF_UP | IFF_RUNNING | IFF_LOOPBACK)) == (IFF_UP | IFF_RUNNING) {
                if ptr.pointee.ifa_addr.pointee.sa_family == UInt8(AF_INET) {
                    var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                    if getnameinfo(ptr.pointee.ifa_addr, socklen_t(ptr.pointee.ifa_addr.pointee.sa_len),
                                   &hostname, socklen_t(hostname.count),
                                   nil, 0, NI_NUMERICHOST) == 0 {
                        let ip = String(cString: hostname)
                        if !ip.hasPrefix("127.") && !ip.hasPrefix("169.254.") {
                            return ip
                        }
                    }
                }
            }
        }
        return nil
    }
}
