import Foundation
import Combine

/// Manages modern MacDrop client settings and device identification.
/// Reads the native macOS computer name by default, and allows users to set a custom override.
public final class DeviceSettingsManager: ObservableObject {
    public static let shared = DeviceSettingsManager()

    private let customNameKey = "macdrop_custom_device_name"
    private let launchAgentLabel = "se.aeroform.macdrop.autostart"

    /// User-customized device name, or empty if using system default
    @Published public var customDeviceName: String {
        didSet {
            UserDefaults.standard.set(customDeviceName, forKey: customNameKey)
        }
    }

    /// Whether MacDrop X is configured to launch automatically on login/boot
    @Published public var launchAtLogin: Bool = false

    private var launchAgentURL: URL {
        let home = FileManager.default.homeDirectoryForCurrentUser
        return home.appendingPathComponent("Library/LaunchAgents/\(launchAgentLabel).plist")
    }

    public init() {
        self.customDeviceName = UserDefaults.standard.string(forKey: customNameKey) ?? ""
        self.launchAtLogin = FileManager.default.fileExists(atPath: launchAgentURL.path)
    }

    /// Enables or disables automatic launch at macOS login/boot using a user LaunchAgent.
    public func setLaunchAtLogin(_ enabled: Bool) {
        if enabled {
            if writeLaunchAgentPlist() {
                DispatchQueue.main.async {
                    self.launchAtLogin = true
                }
            }
        } else {
            try? FileManager.default.removeItem(at: launchAgentURL)
            DispatchQueue.main.async {
                self.launchAtLogin = false
            }
        }
    }

    /// Keeps the LaunchAgent path synced if the user moved MacDrop.app (e.g., to /Applications).
    public func syncLaunchAtLoginPathIfNeeded() {
        guard FileManager.default.fileExists(atPath: launchAgentURL.path) else { return }
        _ = writeLaunchAgentPlist()
    }

    private func writeLaunchAgentPlist() -> Bool {
        let bundlePath = Bundle.main.bundlePath
        let programArgs: [String]
        if bundlePath.hasSuffix(".app") {
            programArgs = ["/usr/bin/open", "-a", bundlePath]
        } else if let execPath = Bundle.main.executablePath {
            programArgs = [execPath]
        } else {
            return false
        }

        let plistDict: [String: Any] = [
            "Label": launchAgentLabel,
            "ProgramArguments": programArgs,
            "RunAtLoad": true
        ]

        let folderURL = launchAgentURL.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(at: folderURL, withIntermediateDirectories: true)
            let data = try PropertyListSerialization.data(fromPropertyList: plistDict, format: .xml, options: 0)
            try data.write(to: launchAgentURL, options: .atomic)
            return true
        } catch {
            DebugLogger.log("[Settings] Failed to write LaunchAgent plist: \(error)")
            return false
        }
    }

    /// Native macOS computer name (from System Settings > General > About)
    public var systemComputerName: String {
        if let name = Host.current().localizedName, !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return name.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return "Mac"
    }

    /// Active device name advertised to 68k MacDrop clients on the network
    public var effectiveDeviceName: String {
        let clean = customDeviceName.trimmingCharacters(in: .whitespacesAndNewlines)
        return clean.isEmpty ? systemComputerName : clean
    }

    /// Machine category / architecture advertised to 68k Mac
    public var machineName: String {
        return "macOS"
    }

    /// Clears custom override and restores the system computer name
    public func resetToDefault() {
        customDeviceName = ""
    }
}
