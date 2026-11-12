import Foundation

/// Represents a vintage Macintosh peer discovered on the local network.
public struct DiscoveredMac: Identifiable, Hashable, Codable {
    public var id: String
    public var name: String
    public var machine: String
    public var ip: String
    public var port: Int
    public var version: String
    public var isOnline: Bool
    public var lastSeen: Date
    public var source: String // "direct_tcp", "server_api", "udp_broadcast", "manual"
    public var relayServerIP: String?

    public init(
        id: String? = nil,
        name: String,
        machine: String = "Macintosh 68k",
        ip: String,
        port: Int = 6803,
        version: String = "0.2.16",
        isOnline: Bool = true,
        lastSeen: Date = Date(),
        source: String = "direct_tcp",
        relayServerIP: String? = nil
    ) {
        self.id = id ?? "mac_\(ip.replacingOccurrences(of: ".", with: "_"))_\(port)"
        self.name = name.isEmpty ? "Macintosh (\(ip))" : name
        self.machine = machine.isEmpty ? "Macintosh 68k" : machine
        self.ip = ip
        self.port = port
        self.version = version
        self.isOnline = isOnline
        self.lastSeen = lastSeen
        self.source = source
        self.relayServerIP = relayServerIP
    }

    /// Clean model subtitle for UI display.
    public var subtitle: String {
        if machine.isEmpty || machine == "Macintosh 68k" {
            return "\(ip)"
        }
        return "\(machine) · \(ip)"
    }

    /// Chooses an SF Symbol or icon suited to the Mac model.
    public var iconSymbol: String {
        let m = machine.lowercased()
        if m.contains("portable") || m.contains("powerbook") {
            return "laptopcomputer"
        } else if m.contains("classic") || m.contains("se") || m.contains("plus") || m.contains("512") || m.contains("128") {
            return "display"
        } else {
            return "desktopcomputer"
        }
    }
}
