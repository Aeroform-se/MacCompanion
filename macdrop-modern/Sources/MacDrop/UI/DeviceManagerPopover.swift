import SwiftUI

/// Popover / Control Panel shown when the user clicks the menu bar icon directly.
/// Displays discovered vintage Macs, connection status, manual IP entry, and recent transfers.
public struct DeviceManagerPopoverView: View {
    @ObservedObject var discovery = DeviceDiscoveryManager.shared
    @ObservedObject var transfer = P2PTransferEngine.shared

    @State private var showingAddManual = false
    @State private var manualIP = ""
    @State private var manualName = ""

    public init() {}

    public var body: some View {
        VStack(spacing: 0) {
            // Header Bar
            HStack(spacing: 8) {
                Image(systemName: "desktopcomputer.trianglebadge.exclamationmark")
                    .foregroundColor(.accentColor)
                    .font(.system(size: 15))

                Text("Vintage Mac Targets")
                    .font(.system(size: 13, weight: .bold))

                Spacer()

                Button(action: {
                    discovery.scanAll()
                }) {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 12, weight: .medium))
                        .rotationEffect(.degrees(discovery.isScanning ? 360 : 0))
                        .animation(discovery.isScanning ? Animation.linear(duration: 1).repeatForever(autoreverses: false) : .default, value: discovery.isScanning)
                }
                .buttonStyle(.plain)
                .help("Refresh and scan for vintage Macs")

                Button(action: {
                    showingAddManual.toggle()
                }) {
                    Image(systemName: "plus.circle")
                        .font(.system(size: 13, weight: .medium))
                }
                .buttonStyle(.plain)
                .help("Add vintage Mac by IP")
            }
            .padding(12)
            .background(Color.primary.opacity(0.04))

            Divider()

            // Manual IP Entry Drawer
            if showingAddManual {
                VStack(spacing: 8) {
                    TextField("IP Address (e.g. 192.168.1.68)", text: $manualIP)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 12, design: .monospaced))

                    TextField("Name (e.g. SE/30 in Lab)", text: $manualName)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 12))

                    HStack {
                        Button("Cancel") {
                            showingAddManual = false
                            manualIP = ""
                            manualName = ""
                        }
                        .font(.system(size: 11))

                        Spacer()

                        Button("Connect & Add") {
                            discovery.addManualDevice(ip: manualIP, name: manualName)
                            showingAddManual = false
                            manualIP = ""
                            manualName = ""
                        }
                        .font(.system(size: 11, weight: .semibold))
                        .disabled(manualIP.isEmpty)
                    }
                }
                .padding(10)
                .background(Color.accentColor.opacity(0.08))
                Divider()
            }

            // Devices List
            ScrollView {
                VStack(spacing: 6) {
                    if discovery.devices.isEmpty {
                        VStack(spacing: 6) {
                            Text("No vintage Macs discovered yet.")
                                .font(.system(size: 11))
                                .foregroundColor(.secondary)
                            Text("Click + to add an IP or make sure MacDrop is running on your 68k Mac.")
                                .font(.system(size: 10))
                                .foregroundColor(.secondary)
                                .multilineTextAlignment(.center)
                        }
                        .padding(20)
                    } else {
                        ForEach(discovery.devices) { dev in
                            deviceRow(dev)
                        }
                    }
                }
                .padding(8)
            }
            .frame(maxHeight: 220)

            Divider()

            // Footer
            HStack {
                if let last = transfer.lastDeliveredFile {
                    Text("✓ Sent \"\(last)\"")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                        .lineLimit(1)
                } else {
                    Text("Drop files onto the menu bar to send")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                }

                Spacer()

                Button("Quit") {
                    NSApplication.shared.terminate(nil)
                }
                .font(.system(size: 10))
                .buttonStyle(.plain)
                .foregroundColor(.secondary)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 8)
            .background(Color.primary.opacity(0.03))
        }
        .frame(width: 320)
    }

    private func deviceRow(_ dev: DiscoveredMac) -> some View {
        let isSelected = (discovery.selectedDevice?.id == dev.id)

        return HStack(spacing: 10) {
            Image(systemName: dev.iconSymbol)
                .font(.system(size: 15))
                .foregroundColor(dev.isOnline ? .accentColor : .secondary)
                .frame(width: 24)

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 4) {
                    Text(dev.name)
                        .font(.system(size: 12, weight: isSelected ? .bold : .medium))
                    if isSelected {
                        Text("(Active)")
                            .font(.system(size: 9, weight: .bold))
                            .foregroundColor(.accentColor)
                    }
                }

                Text(dev.subtitle)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(.secondary)
            }

            Spacer()

            Circle()
                .fill(dev.isOnline ? Color.green : Color.secondary.opacity(0.4))
                .frame(width: 8, height: 8)
        }
        .padding(8)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color.accentColor.opacity(0.12) : Color.primary.opacity(0.03))
        )
        .contentShape(Rectangle())
        .onTapGesture {
            discovery.selectedDevice = dev
        }
    }
}
