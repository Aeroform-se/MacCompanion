import Foundation
import Combine

/// Manages modern MacDrop client settings and device identification.
/// Reads the native macOS computer name by default, and allows users to set a custom override.
public final class DeviceSettingsManager: ObservableObject {
    public static let shared = DeviceSettingsManager()

    private let customNameKey = "macdrop_custom_device_name"

    /// User-customized device name, or empty if using system default
    @Published public var customDeviceName: String {
        didSet {
            UserDefaults.standard.set(customDeviceName, forKey: customNameKey)
        }
    }

    public init() {
        self.customDeviceName = UserDefaults.standard.string(forKey: customNameKey) ?? ""
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
