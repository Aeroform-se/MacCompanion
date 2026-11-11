import AppKit
import SwiftUI

/// Main application delegate controlling menu bar life cycle,
/// invisible net proximity registration, and outside-click dismissal.
public final class AppDelegate: NSObject, NSApplicationDelegate {
    private var globalMonitor: Any?
    private var localMonitor: Any?

    public func applicationDidFinishLaunching(_ notification: Notification) {
        // Prevent app from quitting when all windows are closed
        NSApp.setActivationPolicy(.accessory)

        // Initialize status item
        MenuBarIconManager.shared.setupStatusItem()

        // Set up outside-click event monitors to collapse drop zone when user clicks elsewhere
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] _ in
            self?.handleOutsideClick()
        }

        localMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] event in
            guard let self = self else { return event }
            if let panel = InvisibleNetPanelController.shared.panel, panel.isVisible {
                let mouseLoc = NSEvent.mouseLocation
                if !panel.frame.contains(mouseLoc) {
                    if let button = InvisibleNetPanelController.shared.statusItemButton,
                       let btnWin = button.window {
                        let btnScreenRect = btnWin.convertToScreen(button.frame)
                        if !btnScreenRect.contains(mouseLoc) {
                            self.handleOutsideClick()
                        }
                    } else {
                        self.handleOutsideClick()
                    }
                }
            }
            return event
        }

        // Start background discovery responder (UDP 6801) & reverse channel server (TCP 6800)
        InboundChannelServer.shared.start()

        // Kick off peer discovery
        DeviceDiscoveryManager.shared.scanAll()
    }

    private func handleOutsideClick() {
        // Only collapse if there is NO active transfer running
        if !P2PTransferEngine.shared.isTransferring {
            InvisibleNetPanelController.shared.hideDropZone(animated: true)
        }
    }

    public func applicationWillTerminate(_ notification: Notification) {
        if let monitor = globalMonitor {
            NSEvent.removeMonitor(monitor)
        }
        if let monitor = localMonitor {
            NSEvent.removeMonitor(monitor)
        }
        InboundChannelServer.shared.stop()
        P2PTransferEngine.shared.cancelCurrentTransfer()
    }
}
