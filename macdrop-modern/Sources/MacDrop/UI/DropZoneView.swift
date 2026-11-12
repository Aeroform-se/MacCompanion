import SwiftUI
import UniformTypeIdentifiers

struct DropZoneHeightKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = nextValue()
    }
}

/// SwiftUI view rendered inside the expanded "Invisible Net" drop zone window.
/// Provides visual drop slots for all discovered vintage Macs (matching the web UI layout)
/// and displays active transfer progress.
public struct DropZoneView: View {
    @ObservedObject var discovery = DeviceDiscoveryManager.shared
    @ObservedObject var transfer = P2PTransferEngine.shared
    @ObservedObject var panelController = InvisibleNetPanelController.shared
    @ObservedObject var settings = DeviceSettingsManager.shared

    /// ID of the specific device currently hovered during a drag operation
    @Binding var hoveredDeviceId: String?

    @State private var manualIP = ""
    @State private var manualName = ""
    @State private var manualStatusMessage: String?
    @State private var isManualStatusError: Bool = false
    @State private var isProbingManual: Bool = false
    @State private var editingDeviceName = ""

    public init(hoveredDeviceId: Binding<String?>) {
        self._hoveredDeviceId = hoveredDeviceId
    }

    public var body: some View {
        VStack(spacing: 8) {
            // Header with comfortable top padding & current device name
            HStack {
                Text("MacDrop")
                    .font(.system(size: 13, weight: .bold))

                Spacer()

                Text(settings.effectiveDeviceName)
                    .font(.system(size: 10))
                    .foregroundColor(.secondary)
                    .lineLimit(1)
            }
            .padding(.horizontal, 4)
            .padding(.top, 2)

            // Content Area: Active Transfer HUD or Device Drop Slots
            if let current = transfer.currentTransfer,
               (transfer.isTransferring || current.status == .completed || current.status == .failed || current.status == .streaming || current.status == .finalizing || current.status == .connecting || current.status == .preparing) {
                activeTransferHUD(current)
            } else {
                deviceDropSlots
            }

            // Status message if a file was recently delivered
            if let last = transfer.lastDeliveredFile {
                let text = last.hasPrefix("Received") || last.hasPrefix("Sent") ? "✓ \(last)" : "✓ Sent \"\(last)\""
                HStack {
                    Text(text)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                    Spacer()
                }
                .padding(.horizontal, 4)
            }

            // Separator line before Actions
            Divider()
                .padding(.horizontal, -4)

            // Refresh Row (Spinner shows only during manual refresh next to "Scanning LAN...")
            PopoverMenuItemRow(
                icon: discovery.isManualRefreshing ? nil : "arrow.clockwise",
                title: discovery.isManualRefreshing ? "Scanning LAN..." : "Refresh",
                isLoading: discovery.isManualRefreshing,
                action: {
                    discovery.scanAll(isManual: true)
                }
            )

            // Server Indicator (when connected to external MacCompanion node server)
            if let srv = discovery.activeServer {
                HStack(spacing: 6) {
                    Circle()
                        .fill(Color.green)
                        .frame(width: 6, height: 6)
                    Text("MacCompanion: \(srv):6800")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                    Spacer()
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 2)
            }

            // Add Client Manually Row
            PopoverMenuItemRow(
                icon: panelController.showingAddManual ? "xmark.circle" : "plus.circle",
                title: panelController.showingAddManual ? "Close" : "Add client manually",
                action: {
                    withAnimation(.easeInOut(duration: 0.22)) {
                        panelController.showingAddManual.toggle()
                        if panelController.showingAddManual {
                            panelController.showingSettings = false
                        }
                    }
                }
            )

            // Manual IP Drawer (expands directly beneath Add client)
            if panelController.showingAddManual {
                manualDrawer
            }

            // Set Client Name Row
            PopoverMenuItemRow(
                icon: panelController.showingSettings ? "xmark.circle" : "gearshape",
                title: panelController.showingSettings ? "Close" : "Set client name",
                action: {
                    withAnimation(.easeInOut(duration: 0.22)) {
                        panelController.showingSettings.toggle()
                        if panelController.showingSettings {
                            panelController.showingAddManual = false
                            editingDeviceName = settings.customDeviceName
                        }
                    }
                }
            )

            // Settings Drawer (expands directly beneath Settings)
            if panelController.showingSettings {
                settingsDrawer
            }

            // Separator line before Quit
            Divider()
                .padding(.horizontal, -4)

            // Quit item row (matching standard menu bar popover)
            PopoverMenuItemRow(
                icon: nil,
                title: "Quit",
                action: {
                    NSApplication.shared.terminate(nil)
                }
            )
        }
        .padding(.horizontal, 12)
        .padding(.top, 14)
        .padding(.bottom, 12)
        .frame(width: 300)
        .fixedSize(horizontal: false, vertical: true)
        .onChange(of: transfer.isTransferring) { _ in
            DispatchQueue.main.async {
                InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
            }
        }
        .onChange(of: transfer.currentTransfer?.status) { _ in
            DispatchQueue.main.async {
                InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
            }
        }
        .onChange(of: transfer.currentTransfer == nil) { _ in
            DispatchQueue.main.async {
                InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
            }
        }
        .onChange(of: transfer.lastDeliveredFile) { _ in
            DispatchQueue.main.async {
                InvisibleNetPanelController.shared.updatePanelPosition(animated: true)
            }
        }
        .background(GeometryReader { geo in
            Color.clear.preference(key: DropZoneHeightKey.self, value: geo.size.height)
        })
        .onPreferenceChange(DropZoneHeightKey.self) { newHeight in
            if newHeight > 50 {
                InvisibleNetPanelController.shared.updatePanelHeight(newHeight, animated: true)
            }
        }
    }

    // MARK: - Manual Entry Drawer

    private var manualDrawer: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Add Vintage Mac or Server IP")
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(.primary)

            TextField("IP Address (e.g. 192.168.1.10)", text: $manualIP)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11, design: .monospaced))
                .disabled(isProbingManual)

            TextField("Optional Name (e.g. SE/30)", text: $manualName)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11))
                .disabled(isProbingManual)

            if let msg = manualStatusMessage {
                Text(msg)
                    .font(.system(size: 9.5))
                    .foregroundColor(isManualStatusError ? .red : .green)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.vertical, 2)
            }

            HStack {
                Button("Cancel") {
                    withAnimation(.easeInOut(duration: 0.22)) {
                        panelController.showingAddManual = false
                        manualIP = ""
                        manualName = ""
                        manualStatusMessage = nil
                        isProbingManual = false
                    }
                }
                .font(.system(size: 10))
                .buttonStyle(.plain)
                .disabled(isProbingManual)

                Spacer()

                if isProbingManual {
                    ProgressView()
                        .scaleEffect(0.6)
                        .frame(width: 14, height: 14)
                }

                Button("Save & Connect") {
                    submitManualDevice()
                }
                .font(.system(size: 10, weight: .semibold))
                .disabled(manualIP.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || isProbingManual)
            }
        }
        .padding(8)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.primary.opacity(0.04))
        )
    }

    private func submitManualDevice() {
        let cleanIp = manualIP.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanIp.isEmpty else { return }

        isProbingManual = true
        manualStatusMessage = "Connecting to \(cleanIp)..."
        isManualStatusError = false

        discovery.addManualDevice(ip: cleanIp, name: manualName) { result in
            DispatchQueue.main.async {
                isProbingManual = false
                switch result {
                case .success(let msg):
                    manualStatusMessage = "✓ \(msg)"
                    isManualStatusError = false
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) {
                        withAnimation(.easeInOut(duration: 0.22)) {
                            panelController.showingAddManual = false
                            manualIP = ""
                            manualName = ""
                            manualStatusMessage = nil
                        }
                    }
                case .failure(let err):
                    manualStatusMessage = err
                    isManualStatusError = true
                }
            }
        }
    }

    // MARK: - Settings Drawer (Custom Device Name)

    private var settingsDrawer: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Device Name")
                .font(.system(size: 11, weight: .semibold))
                .foregroundColor(.primary)

            TextField(settings.systemComputerName, text: $editingDeviceName)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11))

            Text("Enter a custom name for this device.")
                .font(.system(size: 9.5))
                .foregroundColor(.secondary)
                .lineLimit(2)

            HStack {
                if !settings.customDeviceName.isEmpty || !editingDeviceName.isEmpty {
                    Button("Reset name") {
                        settings.resetToDefault()
                        editingDeviceName = ""
                    }
                    .font(.system(size: 10))
                    .buttonStyle(.plain)
                    .foregroundColor(.secondary)
                }

                Spacer()

                Button("Save") {
                    settings.customDeviceName = editingDeviceName.trimmingCharacters(in: .whitespacesAndNewlines)
                    withAnimation(.easeInOut(duration: 0.22)) {
                        panelController.showingSettings = false
                    }
                }
                .font(.system(size: 10, weight: .semibold))
            }
            .padding(.top, 2)
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.primary.opacity(0.04))
        )
    }

    // MARK: - Happy Mac Asset

    private var happyMacImage: NSImage? {
        if let img = NSImage(named: "HappyMac") { return img }
        if let path = Bundle.main.path(forResource: "HappyMac", ofType: "png") {
            return NSImage(contentsOfFile: path)
        }
        return nil
    }

    // MARK: - Device Drop Slots (Matching Web UI separate targets)

    private var deviceDropSlots: some View {
        VStack(spacing: 8) {
            let onlineDevices = discovery.devices.filter { $0.isOnline }
            let manualOfflineDevices = discovery.devices.filter { !$0.isOnline && $0.source == "manual" }

            if onlineDevices.isEmpty && manualOfflineDevices.isEmpty {
                emptyState
            } else {
                ForEach(onlineDevices) { dev in
                    deviceSlotRow(dev)
                }
                if !manualOfflineDevices.isEmpty {
                    ForEach(manualOfflineDevices) { dev in
                        deviceSlotRow(dev)
                    }
                }
            }
        }
    }

    private func deviceSlotRow(_ dev: DiscoveredMac) -> some View {
        let isHovered = (hoveredDeviceId == dev.id)
        let isSelected = (discovery.selectedDevice?.id == dev.id)

        return HStack(spacing: 12) {
            // Icon (Vintage Happy Mac)
            ZStack {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(isHovered ? Color.accentColor : Color.primary.opacity(0.06))
                    .frame(width: 38, height: 38)

                if let img = happyMacImage {
                    Image(nsImage: img)
                        .interpolation(.high)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(width: 22, height: 28)
                        .opacity(dev.isOnline ? 1.0 : 0.45)
                } else {
                    Image(systemName: "desktopcomputer")
                        .font(.system(size: 16))
                        .foregroundColor(isHovered ? .white : (dev.isOnline ? .primary : .secondary))
                }
            }

            // Names & IP
            VStack(alignment: .leading, spacing: 2) {
                Text(dev.name)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundColor(dev.isOnline ? .primary : .secondary)
                    .lineLimit(1)

                Text(dev.isOnline ? dev.subtitle : "\(dev.subtitle) · Offline")
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(.secondary)
                    .lineLimit(1)
            }

            Spacer()

            // Online Badge / Drop indicator / Delete manual button
            if isHovered {
                Text("Drop to Send")
                    .font(.system(size: 10, weight: .bold))
                    .foregroundColor(.accentColor)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(
                        Capsule().fill(Color.accentColor.opacity(0.15))
                    )
            } else {
                HStack(spacing: 6) {
                    Circle()
                        .fill(dev.isOnline ? Color.green : Color.secondary.opacity(0.4))
                        .frame(width: 8, height: 8)

                    if dev.source == "manual" {
                        Button(action: {
                            discovery.removeDevice(id: dev.id)
                        }) {
                            Image(systemName: "xmark.circle.fill")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary.opacity(0.5))
                        }
                        .buttonStyle(.plain)
                        .help("Remove manual target")
                    }
                }
            }
        }
        .padding(8)
        .background(
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(isHovered ? Color.accentColor.opacity(0.12) : (isSelected ? Color.primary.opacity(0.05) : Color.clear))
                .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .stroke(isHovered ? Color.accentColor : Color.clear, lineWidth: 1.5)
                )
        )
        .contentShape(Rectangle())
        .onTapGesture {
            discovery.selectedDevice = dev
        }
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 20))
                .foregroundColor(.secondary)

            Text("Searching for Vintage Macs...")
                .font(.system(size: 11, weight: .medium))
                .foregroundColor(.secondary)

            if let srv = discovery.activeServer {
                Text("MacCompanion: \(srv):6800")
                    .font(.system(size: 9.5, design: .monospaced))
                    .foregroundColor(.secondary.opacity(0.8))
            } else {
                Text("Launch MacDrop on your 68k Mac to connect")
                    .font(.system(size: 9))
                    .foregroundColor(.secondary.opacity(0.8))
            }
        }
        .padding(.vertical, 16)
        .frame(maxWidth: .infinity)
    }

    // MARK: - Active Transfer HUD

    private let successGreen = Color(red: 0.10, green: 0.52, blue: 0.20)

    private func activeTransferHUD(_ xfer: TransferProgress) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            // Target & File Info
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(xfer.fileName)
                        .font(.system(size: 12, weight: .bold))
                        .lineLimit(1)

                    if xfer.status == .failed {
                        Text(xfer.errorMessage ?? "Transfer Failed")
                            .font(.system(size: 10))
                            .foregroundColor(.red)
                            .lineLimit(2)
                    } else if xfer.status == .completed {
                        Text("Delivered to \(xfer.targetName)")
                            .font(.system(size: 10, weight: .medium))
                            .foregroundColor(successGreen)
                    } else {
                        Text(xfer.isReceiving ? "Receiving from \(xfer.targetName)..." : "Sending to \(xfer.targetName)...")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                    }
                }

                Spacer()

                if xfer.status == .failed {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.red)
                } else if xfer.status == .completed {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundColor(successGreen)
                } else {
                    HStack(spacing: 6) {
                        Button(action: {
                            InvisibleNetPanelController.shared.hideDropZone(animated: true, force: true)
                        }) {
                            Image(systemName: "chevron.up.circle")
                                .font(.system(size: 13))
                                .foregroundColor(.secondary)
                        }
                        .buttonStyle(.plain)
                        .help("Hide window (transfer continues in background)")

                        if xfer.status == .streaming || xfer.status == .connecting {
                            Button(action: {
                                transfer.cancelCurrentTransfer()
                            }) {
                                Image(systemName: "xmark.circle.fill")
                                    .font(.system(size: 13))
                                    .foregroundColor(.secondary)
                            }
                            .buttonStyle(.plain)
                            .help("Cancel transfer")
                        }
                    }
                }
            }

            // Progress Bar (colored red on error, successGreen on complete, accent on streaming)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Color.primary.opacity(0.1))
                        .frame(height: 8)

                    RoundedRectangle(cornerRadius: 4)
                        .fill(xfer.status == .failed ? Color.red : (xfer.status == .completed ? successGreen : Color.accentColor))
                        .frame(width: max(8, geo.size.width * CGFloat(xfer.status == .completed ? 1.0 : xfer.progressFraction)), height: 8)
                        .animation(.easeOut(duration: 0.2), value: xfer.progressFraction)
                }
            }
            .frame(height: 8)

            // Speed & Bytes Footer
            HStack {
                Text(xfer.status == .completed ? "100%" : (xfer.status == .failed ? "Failed" : "\(xfer.percentInt)%"))
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundColor(xfer.status == .failed ? .red : (xfer.status == .completed ? successGreen : .primary))

                if !xfer.formattedSpeed.isEmpty && xfer.status != .failed {
                    Text("· \(xfer.formattedSpeed)")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                }

                Spacer()

                if xfer.status == .failed {
                    Text("Check MacDrop on 68k")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                } else if !xfer.formattedEta.isEmpty {
                    Text(xfer.formattedEta)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                } else {
                    Text(xfer.formattedProgress)
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding(10)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(Color.primary.opacity(0.04))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(xfer.status == .failed ? Color.red.opacity(0.3) : (xfer.status == .completed ? successGreen.opacity(0.4) : Color.primary.opacity(0.1)), lineWidth: 1)
                )
        )
    }
}

/// Menu-style action row matching standard macOS menu bar popovers (like Tailscale)
struct PopoverMenuItemRow: View {
    let icon: String?
    let title: String
    var isDestructive: Bool = false
    var isLoading: Bool = false
    let action: () -> Void

    @State private var isHovered = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                if isLoading {
                    ProgressView()
                        .scaleEffect(0.6)
                        .frame(width: 16, height: 16)
                } else if let icon = icon {
                    Image(systemName: icon)
                        .font(.system(size: 12))
                        .foregroundColor(isDestructive ? .secondary : .primary)
                        .frame(width: 16)
                }

                Text(title)
                    .font(.system(size: 13))
                    .foregroundColor(isDestructive ? .primary : .primary)

                Spacer()
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(isHovered ? Color.primary.opacity(0.08) : Color.clear)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering in
            isHovered = hovering
        }
    }
}
