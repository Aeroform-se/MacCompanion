import AppKit
import SwiftUI
import UserNotifications

/// Main application delegate controlling menu bar life cycle,
/// invisible net proximity registration, outside-click dismissal,
/// and foreground notification presentation.
public final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    private var globalMonitor: Any?
    private var localMonitor: Any?
    private var keyDownMonitor: Any?

    public func applicationDidFinishLaunching(_ notification: Notification) {
        // Prevent app from quitting when all windows are closed
        NSApp.setActivationPolicy(.accessory)

        // Register UNUserNotificationCenter delegate to display notification banners even while app is running
        UNUserNotificationCenter.current().delegate = self
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { granted, error in
            if let error = error {
                print("[MacDrop] Notification authorization error: \(error)")
            }
        }

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

        // Handle keyboard shortcuts when MacDrop is the active/key application
        keyDownMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            // Handle Command+Q to quit MacDrop safely without affecting background apps
            if event.modifierFlags.contains(.command) && event.charactersIgnoringModifiers == "q" {
                NSApp.terminate(nil)
                return nil
            }
            // Handle Escape key to dismiss drop zone popover
            if event.keyCode == 53 {
                if InvisibleNetPanelController.shared.panel?.isVisible == true {
                    InvisibleNetPanelController.shared.hideDropZone(animated: true, force: true)
                    return nil
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
        // Collapse drop zone popover on outside click (transfers continue in background)
        InvisibleNetPanelController.shared.hideDropZone(animated: true, force: true)
    }

    public func applicationWillTerminate(_ notification: Notification) {
        InvisibleNetPanelController.shared.stopProximityMonitoring()
        if let monitor = globalMonitor {
            NSEvent.removeMonitor(monitor)
        }
        if let monitor = localMonitor {
            NSEvent.removeMonitor(monitor)
        }
        if let monitor = keyDownMonitor {
            NSEvent.removeMonitor(monitor)
        }
        InboundChannelServer.shared.stop()
        P2PTransferEngine.shared.cancelCurrentTransfer()
    }

    // MARK: - UNUserNotificationCenterDelegate

    public func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        // Ensure banners and sounds always pop up even while MacDrop is running or active
        if #available(macOS 11.0, *) {
            completionHandler([.banner, .sound, .badge, .list])
        } else {
            completionHandler([.alert, .sound, .badge])
        }
    }
}
