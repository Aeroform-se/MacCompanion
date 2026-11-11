import Foundation
import Network
import Combine

/// Discovers vintage Macintosh clients on the local network via:
/// 1. Direct TCP handshake on Port 6803 (`MACDROP_HELLO` -> `MACDROP_PONG`)
/// 2. MacCompanion Server API (`/api/macdrop/status` on port 6800, if online)
/// 3. UDP Broadcast listener / prober on Port 6801
public final class DeviceDiscoveryManager: ObservableObject {
    public static let shared = DeviceDiscoveryManager()

    @Published public var devices: [DiscoveredMac] = []
    @Published public var selectedDevice: DiscoveredMac?
    @Published public var isScanning: Bool = false
    @Published public var isManualRefreshing: Bool = false
    @Published public var serverAddress: String = "127.0.0.1"

    private var scanTimer: Timer?
    private let queue = DispatchQueue(label: "se.aeroform.macdrop.discovery", qos: .utility)
    private let defaultsKey = "macdrop_known_devices_v1"

    public init() {
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

        let group = DispatchGroup()

        // 1. Query local MacCompanion server if reachable
        group.enter()
        queryLocalServer { group.leave() }

        // 2. Probe all known and local IP addresses
        group.enter()
        probeKnownDevices { group.leave() }

        group.notify(queue: .main) {
            self.isScanning = false
            self.isManualRefreshing = false
            self.persistDevices()
            if self.selectedDevice == nil, let first = self.devices.first(where: { $0.isOnline }) {
                self.selectedDevice = first
            }
        }
    }

    /// Manually adds an IP address to the target list.
    public func addManualDevice(ip: String, name: String = "") {
        let cleanIp = ip.trimmingCharacters(in: .whitespacesAndNewlines)
        guard isValidIP(cleanIp) else { return }

        let newDev = DiscoveredMac(
            name: name.isEmpty ? "Macintosh (\(cleanIp))" : name,
            machine: "Vintage Macintosh",
            ip: cleanIp,
            port: 6803,
            version: "",
            isOnline: false,
            source: "manual"
        )

        DispatchQueue.main.async {
            if let idx = self.devices.firstIndex(where: { $0.ip == cleanIp }) {
                self.devices[idx].name = newDev.name
            } else {
                self.devices.append(newDev)
            }
            self.selectedDevice = self.devices.first(where: { $0.ip == cleanIp })
            self.persistDevices()
        }

        probeSingleIP(cleanIp) { [weak self] online, pong in
            if online {
                self?.scanAll()
            }
        }
    }

    /// Registers or updates a vintage Mac that connected via the inbound reverse channel (port 6800).
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

    // MARK: - Direct TCP 6803 Probing

    private func probeKnownDevices(completion: @escaping () -> Void) {
        let currentIps = devices.map { $0.ip }
        var targetIps = Set(currentIps)

        // Include default local subnet common IPs
        targetIps.insert("192.168.97.1")

        let group = DispatchGroup()
        for ip in targetIps {
            // Never probe SLIRP private IPs directly over TCP (SLIRP NAT is unroutable inbound)
            if ip.hasPrefix("10.0.2.") {
                continue
            }
            // Skip probing devices that are already actively connected via reverse channel
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
        tcp.connectionTimeout = 1 // 1-second fast timeout
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
                // Connected! Send MACDROP_HELLO probe
                let helloData = Data("MACDROP_HELLO\n".utf8)
                conn.send(content: helloData, completion: .contentProcessed { err in
                    if err != nil {
                        finishOnce(false, nil)
                        return
                    }

                    // Await MACDROP_PONG response
                    conn.receive(minimumIncompleteLength: 1, maximumLength: 512) { data, _, _, recvErr in
                        guard let data = data, let text = String(data: data, encoding: .utf8), recvErr == nil else {
                            // If socket connected and stayed open, it's online even if pong was quiet
                            let mac = DiscoveredMac(name: "Macintosh (\(ip))", machine: "Macintosh 68k", ip: ip, isOnline: true)
                            finishOnce(true, mac)
                            return
                        }

                        // Parse MACDROP_PONG|<id>|<name>|<machine>|<ver>|<ourIp>
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

        // Safety timeout
        self.queue.asyncAfter(deadline: .now() + 1.2) {
            finishOnce(false, nil)
        }

        conn.start(queue: self.queue)
    }

    // MARK: - MacCompanion Server API Query (Fallback / Multi-subnet)

    private func queryLocalServer(completion: @escaping () -> Void) {
        guard let url = URL(string: "http://\(serverAddress):6800/api/macdrop/status") else {
            completion()
            return
        }

        var req = URLRequest(url: url)
        req.timeoutInterval = 1.0

        URLSession.shared.dataTask(with: req) { [weak self] data, response, error in
            guard let self = self, let data = data, error == nil else {
                completion()
                return
            }

            do {
                if let json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                   let clients = json["clients"] as? [[String: Any]] {
                    for c in clients {
                        let id = c["id"] as? String ?? ""
                        let name = c["name"] as? String ?? "Macintosh"
                        let machine = c["machine"] as? String ?? "Macintosh 68k"
                        let ip = c["ip"] as? String ?? ""
                        let ver = c["version"] as? String ?? ""
                        let online = c["online"] as? Bool ?? true

                        if !ip.isEmpty && ip != "127.0.0.1" {
                            let dev = DiscoveredMac(
                                id: id,
                                name: name,
                                machine: machine,
                                ip: ip,
                                port: 6803,
                                version: ver,
                                isOnline: online,
                                lastSeen: Date(),
                                source: "server_api"
                            )
                            self.updateOrAddDevice(dev)
                        }
                    }
                }
            } catch {}
            completion()
        }.resume()
    }

    // MARK: - State Management & Persistence

    private func updateOrAddDevice(_ dev: DiscoveredMac) {
        DispatchQueue.main.async {
            if let idx = self.devices.firstIndex(where: { $0.ip == dev.ip }) {
                self.devices[idx].name = dev.name
                self.devices[idx].machine = dev.machine
                self.devices[idx].version = dev.version
                self.devices[idx].isOnline = true
                self.devices[idx].lastSeen = Date()
                self.devices[idx].source = dev.source
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
                // Never mark offline if device has an active reverse channel or was seen recently via channel
                if InboundChannelServer.shared.hasActiveChannel(for: dev.id) {
                    return
                }
                if dev.source == "reverse_channel" && Date().timeIntervalSince(dev.lastSeen) < 45.0 {
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
            UserDefaults.standard.set(data, forKey: defaultsKey)
        }
    }

    private func loadPersistedDevices() {
        if let data = UserDefaults.standard.data(forKey: defaultsKey),
           let saved = try? JSONDecoder().decode([DiscoveredMac].self, from: data) {
            self.devices = saved
        }
    }

    private func isValidIP(_ ip: String) -> Bool {
        let parts = ip.components(separatedBy: ".")
        guard parts.count == 4 else { return false }
        return parts.allSatisfy { Int($0) != nil && (0...255).contains(Int($0)!) }
    }
}
