import AppKit
import SwiftUI

/// Custom NSPanel that enables key window status so SwiftUI TextFields can receive focus and keyboard typing
final class DropZonePanel: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

/// The "Invisible Net" Proximity Drop Zone window controller.
/// Manages a borderless floating NSPanel positioned immediately below the menu bar status item.
/// It senses incoming drag events from Finder, expands with a smooth animation into a full landing pad,
/// and handles file drops directly to vintage Macs.
public final class InvisibleNetPanelController: NSObject, ObservableObject, NSDraggingDestination, NSWindowDelegate {
    public static let shared = InvisibleNetPanelController()

    public var panel: NSPanel!
    private var containerView: DropZoneContainerView!
    private var hostingView: NSHostingView<DropZoneWrapperView>!
    @Published public var hoveredDeviceId: String? = nil
    @Published public var showingAddManual: Bool = false
    @Published public var showingSettings: Bool = false
    public var deviceRowFrames: [String: CGRect] = [:]
    public var isDraggingOverPanel: Bool = false
    private var isExpanded: Bool = false
    public var isPinnedOpen: Bool = false
    private var autoDismissTimer: Timer?
    private var dismissTimer: Timer?
    private var dragMonitor: Any?
    private var mouseUpMonitor: Any?
    private var lastDragY: CGFloat = 0

    public weak var statusItemButton: NSStatusBarButton? {
        didSet {
            if statusItemButton != nil {
                startProximityMonitoring()
            }
        }
    }

    public override init() {
        super.init()
        setupPanel()
    }

    deinit {
        stopProximityMonitoring()
    }

    private func setupPanel() {
        panel = DropZonePanel(
            contentRect: NSRect(x: 0, y: 0, width: 320, height: 200),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )

        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.level = .floating
        panel.hasShadow = true
        panel.isMovableByWindowBackground = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        panel.delegate = self

        // Use native macOS Popover NSVisualEffectView for exact system vibrancy and grey shade
        containerView = DropZoneContainerView(frame: NSRect(x: 0, y: 0, width: 320, height: 200))
        containerView.controller = self
        containerView.autoresizingMask = [.width, .height]

        let wrapperView = DropZoneWrapperView(controller: self)
        hostingView = NSHostingView(rootView: wrapperView)
        hostingView.frame = containerView.bounds
        hostingView.autoresizingMask = [.width, .height]
        hostingView.wantsLayer = true
        hostingView.layer?.cornerRadius = 10
        hostingView.layer?.masksToBounds = true

        containerView.addSubview(hostingView)
        panel.contentView = containerView

        // Register for file drags from Finder
        let dragTypes: [NSPasteboard.PasteboardType] = [
            .fileURL,
            .init("public.file-url"),
            .init("NSFilenamesPboardType"),
            .init("com.apple.finder.node")
        ]
        panel.registerForDraggedTypes(dragTypes)
        containerView.registerForDraggedTypes(dragTypes)
        panel.alphaValue = 0.0
    }

    // MARK: - Window Positioning & Visibility

    public func updatePanelPosition(animated: Bool = false) {
        guard let button = statusItemButton, let window = button.window else { return }
        let buttonFrame = window.convertToScreen(button.frame)

        let targetWidth: CGFloat = 300

        // Measure exact content fitting height directly from the SwiftUI layout
        hostingView.layoutSubtreeIfNeeded()
        let fitting = hostingView.fittingSize
        let targetHeight = max(140.0, ceil(fitting.height) + 4)

        let x = buttonFrame.midX - (targetWidth / 2)
        let y = buttonFrame.minY - targetHeight // Anchor top edge flush below menu bar

        // Keep within screen bounds
        let screen = NSScreen.main?.visibleFrame ?? .zero
        let clampedX = min(max(x, screen.minX + 8), screen.maxX - targetWidth - 8)

        // Lock X coordinate while visible so content updates don't cause accidental X shifts
        let finalX = (isExpanded && panel.isVisible && abs(panel.frame.width - targetWidth) < 1.0) ? panel.frame.origin.x : clampedX
        let newFrame = NSRect(x: finalX, y: y, width: targetWidth, height: targetHeight)

        // Don't animate if already matching
        if abs(panel.frame.height - targetHeight) < 1.0 && abs(panel.frame.origin.y - y) < 1.0 && abs(panel.frame.origin.x - finalX) < 1.0 {
            return
        }

        if animated {
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.20
                ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                panel.animator().setFrame(newFrame, display: true)
            }, completionHandler: { [weak self] in
                self?.panel.invalidateShadow()
            })
        } else {
            panel.setFrame(newFrame, display: true)
            panel.invalidateShadow()
        }
    }

    /// Measures the exact SwiftUI fitting size and resizes the panel smoothly without clipping
    public func refreshPanelHeight(animated: Bool = true) {
        guard let button = statusItemButton, button.window != nil else { return }
        hostingView.layoutSubtreeIfNeeded()
        let fitting = hostingView.fittingSize
        let targetHeight = max(140.0, ceil(fitting.height) + 4)
        updatePanelHeight(targetHeight, animated: animated)
    }

    /// Dynamically resizes the panel height when SwiftUI content expands/collapses (e.g. drawers, device list).
    /// Expands downward by keeping the top edge anchored directly below the menu bar.
    public func updatePanelHeight(_ newHeight: CGFloat, animated: Bool = true) {
        guard let button = statusItemButton, let window = button.window else { return }
        let buttonFrame = window.convertToScreen(button.frame)

        let targetWidth: CGFloat = 300
        let targetHeight = max(140.0, ceil(newHeight) + 4)

        let x = buttonFrame.midX - (targetWidth / 2)
        let y = buttonFrame.minY - targetHeight // Top edge stays flush below menu bar

        let screen = NSScreen.main?.visibleFrame ?? .zero
        let clampedX = min(max(x, screen.minX + 8), screen.maxX - targetWidth - 8)

        // Lock X coordinate while visible so vertical height changes NEVER cause horizontal jitter
        let finalX = (isExpanded && panel.isVisible && abs(panel.frame.width - targetWidth) < 1.0) ? panel.frame.origin.x : clampedX
        let newFrame = NSRect(x: finalX, y: y, width: targetWidth, height: targetHeight)

        // Don't resize if already matching
        if abs(panel.frame.height - targetHeight) < 1.0 && abs(panel.frame.width - targetWidth) < 1.0 && abs(panel.frame.origin.x - finalX) < 1.0 && abs(panel.frame.origin.y - y) < 1.0 {
            return
        }

        if animated && isExpanded && panel.isVisible {
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.20
                ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
                panel.animator().setFrame(newFrame, display: true)
            }, completionHandler: { [weak self] in
                self?.panel.invalidateShadow()
            })
        } else {
            panel.setFrame(newFrame, display: true)
            panel.invalidateShadow()
        }
    }

    public func showDropZone(animated: Bool = true) {
        dismissTimer?.invalidate()
        dismissTimer = nil
        autoDismissTimer?.invalidate()

        // If already expanded and visible, avoid redundant opacity re-animations or position jumps
        if isExpanded && panel.isVisible {
            updatePanelPosition(animated: false)
            if panel.alphaValue < 0.99 {
                panel.alphaValue = 1.0
            }
            return
        }

        updatePanelPosition()

        if isPinnedOpen {
            NSApp.activate(ignoringOtherApps: true)
            panel.makeKeyAndOrderFront(nil)
        } else {
            panel.orderFrontRegardless()
        }

        if animated {
            NSAnimationContext.runAnimationGroup { ctx in
                ctx.duration = 0.15
                ctx.timingFunction = CAMediaTimingFunction(name: .easeOut)
                panel.animator().alphaValue = 1.0
            }
        } else {
            panel.alphaValue = 1.0
        }
        isExpanded = true
    }

    public func hideDropZone(animated: Bool = true, force: Bool = false) {
        if !force && P2PTransferEngine.shared.isTransferring {
            return
        }

        // If already hidden or currently fading out, avoid redundant animation
        if !isExpanded && (!panel.isVisible || panel.alphaValue <= 0.01) {
            return
        }

        isPinnedOpen = false
        isDraggingOverPanel = false
        showingSettings = false
        showingAddManual = false
        dismissTimer?.invalidate()
        dismissTimer = nil
        isExpanded = false

        if animated {
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.12
                panel.animator().alphaValue = 0.0
            }, completionHandler: { [weak self] in
                self?.panel.orderOut(nil)
                self?.hoveredDeviceId = nil
            })
        } else {
            panel.alphaValue = 0.0
            panel.orderOut(nil)
            hoveredDeviceId = nil
        }
    }

    // MARK: - NSWindowDelegate

    public func windowDidResignKey(_ notification: Notification) {
        if isPinnedOpen && !P2PTransferEngine.shared.isTransferring {
            hideDropZone(animated: true)
        }
    }

    public func toggleVisibility() {
        if isExpanded && panel.alphaValue > 0.5 {
            isPinnedOpen = false
            hideDropZone(animated: true, force: true)
        } else {
            isPinnedOpen = true
            showDropZone()
        }
    }

    // MARK: - NSDraggingDestination Event Handling

    public func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        guard canAcceptDrag(sender) else { return [] }
        isDraggingOverPanel = true
        dismissTimer?.invalidate()
        dismissTimer = nil
        showDropZone(animated: true)
        updateHoveredTarget(from: sender)
        return .copy
    }

    public func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        guard canAcceptDrag(sender) else { return [] }
        isDraggingOverPanel = true
        dismissTimer?.invalidate()
        dismissTimer = nil
        updateHoveredTarget(from: sender)
        return .copy
    }

    public func draggingExited(_ sender: NSDraggingInfo?) {
        isDraggingOverPanel = false
        let mouseLoc = NSEvent.mouseLocation
        if isMouseInProximity(mouseLoc) || panel.frame.contains(mouseLoc) {
            scheduleDismissCheck(delay: 0.25)
        } else {
            dismissTimer?.invalidate()
            dismissTimer = nil
            hideDropZone(animated: true)
        }
    }

    public func scheduleDismissCheck(delay: TimeInterval = 0.25) {
        guard !P2PTransferEngine.shared.isTransferring && !isPinnedOpen else { return }
        dismissTimer?.invalidate()
        dismissTimer = Timer.scheduledTimer(withTimeInterval: delay, repeats: false) { [weak self] _ in
            guard let self = self else { return }
            let mouseLoc = NSEvent.mouseLocation
            if let button = self.statusItemButton, let win = button.window {
                let btnRect = win.convertToScreen(button.frame)
                if btnRect.contains(mouseLoc) { return }
            }
            // Keep window rock solid if mouse is inside window bounds or sense zone
            if self.panel.frame.contains(mouseLoc) || self.isMouseInProximity(mouseLoc) { return }
            if !P2PTransferEngine.shared.isTransferring && !self.isPinnedOpen {
                self.hideDropZone(animated: true)
            }
        }
    }

    // MARK: - Proximity Sensor ("Invisible Net")

    public func isMouseInProximity(_ mouseLoc: NSPoint) -> Bool {
        guard let button = statusItemButton, let window = button.window else { return false }
        let buttonScreenFrame = window.convertToScreen(button.frame)

        // Proximity detection zone (shrunk to half vertical reach as requested):
        // Horizontal: within 160px of icon center (covers 300px drop window + 10px margin)
        // Vertical: from top of menu bar down to 150px below it (previously 320px)
        let triggerMinX = buttonScreenFrame.midX - 160
        let triggerMaxX = buttonScreenFrame.midX + 160
        let triggerMinY = buttonScreenFrame.minY - 150
        let triggerMaxY = buttonScreenFrame.maxY + 10

        return (mouseLoc.x >= triggerMinX && mouseLoc.x <= triggerMaxX &&
                mouseLoc.y >= triggerMinY && mouseLoc.y <= triggerMaxY)
    }

    public func startProximityMonitoring() {
        stopProximityMonitoring()

        dragMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDragged]) { [weak self] event in
            self?.handleGlobalMouseDrag(event)
        }

        mouseUpMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseUp]) { [weak self] _ in
            self?.handleGlobalMouseUp()
        }
    }

    public func stopProximityMonitoring() {
        if let monitor = dragMonitor {
            NSEvent.removeMonitor(monitor)
            dragMonitor = nil
        }
        if let monitor = mouseUpMonitor {
            NSEvent.removeMonitor(monitor)
            mouseUpMonitor = nil
        }
    }

    private func handleGlobalMouseDrag(_ event: NSEvent) {
        // Only trigger if left mouse button is pressed
        guard NSEvent.pressedMouseButtons & 1 != 0 else { return }

        // If an active file transfer is already running or user pinned it open, leave it alone
        guard !P2PTransferEngine.shared.isTransferring && !isPinnedOpen else { return }

        let mouseLoc = NSEvent.mouseLocation
        let isInOpenZone = isMouseInProximity(mouseLoc)
        let isOverWindow = (isExpanded && panel.isVisible && panel.frame.contains(mouseLoc))
        let isMovingTowardsMenuBar = (mouseLoc.y >= lastDragY - 2)
        lastDragY = mouseLoc.y

        if !isExpanded {
            // Static sense zone to open the window
            if isInOpenZone && isMovingTowardsMenuBar {
                dismissTimer?.invalidate()
                dismissTimer = nil
                showDropZone(animated: true)
            }
        } else {
            // Once opened, the window stays open if the pointer is anywhere over
            // the window's actual current bounds (even with 4+ clients) OR the sense zone.
            if isOverWindow || isInOpenZone {
                dismissTimer?.invalidate()
                dismissTimer = nil
            } else {
                // Immediately retract when mouse pointer leaves both the window frame and the sense zone
                dismissTimer?.invalidate()
                dismissTimer = nil
                hideDropZone(animated: true)
            }
        }
    }

    private func handleGlobalMouseUp() {
        guard !isPinnedOpen && !P2PTransferEngine.shared.isTransferring else { return }
        if isExpanded {
            let mouseLoc = NSEvent.mouseLocation
            if !panel.frame.contains(mouseLoc) && !isMouseInProximity(mouseLoc) {
                hideDropZone(animated: true)
            }
        }
    }

    public func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        DebugLogger.log("[DropZone] performDragOperation invoked")
        isDraggingOverPanel = false
        guard let fileURLs = extractFileURLs(from: sender), !fileURLs.isEmpty else {
            DebugLogger.log("[DropZone] extractFileURLs returned nil or empty!")
            hideDropZone(animated: true)
            return false
        }
        DebugLogger.log("[DropZone] Extracted \(fileURLs.count) file URL(s): \(fileURLs.map { $0.path })")

        // Clear hover indicator immediately upon drop release
        let targetId = hoveredDeviceId
        hoveredDeviceId = nil

        // Determine destination vintage Mac:
        // 1. The specifically hovered drop slot
        // 2. The actively selected device
        // 3. The first available online device
        let discovery = DeviceDiscoveryManager.shared
        let target: DiscoveredMac?

        if let hoveredId = targetId, let found = discovery.devices.first(where: { $0.id == hoveredId }) {
            target = found
            DebugLogger.log("[DropZone] Chosen target from hover: \(found.name) (\(found.ip), id=\(found.id))")
        } else if let selected = discovery.selectedDevice, selected.isOnline {
            target = selected
            DebugLogger.log("[DropZone] Chosen target from selected: \(selected.name) (\(selected.ip), id=\(selected.id))")
        } else {
            target = discovery.devices.first(where: { $0.isOnline })
            DebugLogger.log("[DropZone] Chosen target from first online: \(target?.name ?? "none")")
        }

        guard let chosenTarget = target else {
            DebugLogger.log("[DropZone] No online target found! Beeping and hiding.")
            NSSound.beep()
            hideDropZone(animated: true)
            return false
        }

        // Send the first file (or queue multiple)
        let file = fileURLs[0]
        DebugLogger.log("[DropZone] Initiating send of \(file.lastPathComponent) to \(chosenTarget.name)")
        P2PTransferEngine.shared.send(fileURL: file, to: chosenTarget)

        // Immediately update panel position and fitting height on the main thread
        DispatchQueue.main.async { [weak self] in
            self?.updatePanelPosition(animated: true)
        }

        // Ensure window stays visible without re-triggering entrance positioning jumps
        if !panel.isVisible {
            showDropZone(animated: true)
        }

        // Schedule auto-dismiss when completed
        scheduleAutoDismissWatcher()

        return true
    }

    public func scheduleAutoDismissWatcher() {
        autoDismissTimer?.invalidate()
        autoDismissTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] timer in
            guard let self = self else { timer.invalidate(); return }
            let engine = P2PTransferEngine.shared
            if engine.currentTransfer?.status == .completed {
                timer.invalidate()
                // Wait 4.0 seconds so user clearly sees "Delivered!", then reset state to revert to device list
                DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) {
                    engine.currentTransfer = nil
                    engine.isTransferring = false
                    if !self.isPinnedOpen {
                        self.hideDropZone(animated: true)
                    }
                }
            } else if engine.currentTransfer?.status == .failed || engine.currentTransfer?.status == .cancelled {
                timer.invalidate()
                DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) {
                    engine.currentTransfer = nil
                    engine.isTransferring = false
                    if !self.isPinnedOpen {
                        self.hideDropZone(animated: true)
                    }
                }
            }
        }
    }

    // MARK: - Helpers

    private func canAcceptDrag(_ sender: NSDraggingInfo) -> Bool {
        let pb = sender.draggingPasteboard
        let types = pb.types ?? []
        return types.contains {
            $0 == .fileURL ||
            $0.rawValue == "public.file-url" ||
            $0.rawValue == "NSFilenamesPboardType" ||
            $0.rawValue == "com.apple.finder.node"
        } || pb.canReadItem(withDataConformingToTypes: [NSPasteboard.PasteboardType.fileURL.rawValue])
    }

    private func extractFileURLs(from sender: NSDraggingInfo) -> [URL]? {
        let pb = sender.draggingPasteboard
        var foundURLs: [URL] = []

        // 1. Read file URLs via NSURL with file-URL filter
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: [
            NSPasteboard.ReadingOptionKey.urlReadingFileURLsOnly: true
        ]) as? [URL], !urls.isEmpty {
            foundURLs.append(contentsOf: urls)
        }

        // 2. Read without filter
        if foundURLs.isEmpty, let urls = pb.readObjects(forClasses: [NSURL.self], options: nil) as? [URL] {
            foundURLs.append(contentsOf: urls.filter { $0.isFileURL })
        }

        // 3. Modern NSPasteboardItem inspection
        if foundURLs.isEmpty, let items = pb.pasteboardItems {
            for item in items {
                for t in item.types {
                    if t == .fileURL || t.rawValue == "public.file-url" {
                        if let str = item.string(forType: t), let u = URL(string: str), u.isFileURL {
                            foundURLs.append(u)
                        }
                    }
                }
            }
        }

        // 4. Legacy NSFilenamesPboardType
        if foundURLs.isEmpty, let filenames = pb.propertyList(forType: .init("NSFilenamesPboardType")) as? [String] {
            foundURLs.append(contentsOf: filenames.map { URL(fileURLWithPath: $0) })
        }

        // 5. Direct string for .fileURL
        if foundURLs.isEmpty, let single = pb.string(forType: .fileURL), let u = URL(string: single), u.isFileURL {
            foundURLs.append(u)
        }

        guard !foundURLs.isEmpty else { return nil }

        // Standardize & resolve any file-reference URLs (e.g. file:///.file/id=...) to canonical file paths
        return foundURLs.map { u in
            if let pathURL = (u as NSURL).filePathURL {
                return pathURL.standardizedFileURL
            }
            return u.resolvingSymlinksInPath().standardizedFileURL
        }
    }

    private func updateHoveredTarget(from sender: NSDraggingInfo) {
        let pointInContainer = containerView.convert(sender.draggingLocation, from: nil)
        let onlineDevices = DeviceDiscoveryManager.shared.devices.filter { $0.isOnline }

        guard onlineDevices.count > 1 else {
            hoveredDeviceId = onlineDevices.first?.id
            return
        }

        // Pixel-exact hit-testing against live SwiftUI row frames (inset vertically to bridge 8pt VStack gap cleanly)
        for dev in onlineDevices {
            if let frame = deviceRowFrames[dev.id], frame.insetBy(dx: -8, dy: -4).contains(pointInContainer) {
                if hoveredDeviceId != dev.id {
                    hoveredDeviceId = dev.id
                }
                return
            }
        }

        // Fallback approximate math only if row frames have not yet laid out
        if deviceRowFrames.isEmpty {
            let relativeY = pointInContainer.y - 36
            if relativeY > 0 {
                let rowIndex = Int(relativeY / 62)
                if rowIndex >= 0 && rowIndex < onlineDevices.count {
                    let targetId = onlineDevices[rowIndex].id
                    if hoveredDeviceId != targetId {
                        hoveredDeviceId = targetId
                    }
                    return
                }
            }
        }

        if hoveredDeviceId == nil || !onlineDevices.contains(where: { $0.id == hoveredDeviceId }) {
            hoveredDeviceId = DeviceDiscoveryManager.shared.selectedDevice?.id ?? onlineDevices.first?.id
        }
    }
}

struct DeviceRowFramesKey: PreferenceKey {
    static var defaultValue: [String: CGRect] = [:]
    static func reduce(value: inout [String: CGRect], nextValue: () -> [String: CGRect]) {
        value.merge(nextValue(), uniquingKeysWith: { _, new in new })
    }
}

/// Container view for the panel that provides native macOS popover frosted glass vibrancy,
/// crisp 10pt continuous rounded corners, matching drop shadow, and NSDraggingDestination
final class DropZoneContainerView: NSVisualEffectView {
    weak var controller: InvisibleNetPanelController?

    /// Flipped coordinate system ensures subviews are anchored from the top-down.
    /// When the panel expands in height, it grows downwards without shifting or clipping header elements.
    override var isFlipped: Bool { true }

    /// 9-slice resizable mask with 10pt continuous rounded corners.
    /// This masks the WindowServer popover backdrop to eliminate sharp 90-degree corners.
    private static let roundedMask: NSImage = {
        let r: CGFloat = 10
        let mask = NSImage(size: NSSize(width: r * 2 + 2, height: r * 2 + 2), flipped: false) { rect in
            let path = NSBezierPath(roundedRect: rect, xRadius: r, yRadius: r)
            NSColor.black.setFill()
            path.fill()
            return true
        }
        mask.capInsets = NSEdgeInsets(top: r, left: r, bottom: r, right: r)
        mask.resizingMode = .stretch
        return mask
    }()

    private static let acceptedDragTypes: [NSPasteboard.PasteboardType] = [
        .fileURL,
        .init("public.file-url"),
        .init("NSFilenamesPboardType"),
        .init("com.apple.finder.node")
    ]

    override init(frame: NSRect) {
        super.init(frame: frame)
        setupAppearance()
        registerForDraggedTypes(Self.acceptedDragTypes)
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setupAppearance()
        registerForDraggedTypes(Self.acceptedDragTypes)
    }

    private func setupAppearance() {
        material = .popover
        blendingMode = .behindWindow
        state = .active
        maskImage = Self.roundedMask
        wantsLayer = true
        layer?.cornerRadius = 10
        layer?.masksToBounds = true
        layer?.borderColor = NSColor.separatorColor.withAlphaComponent(0.4).cgColor
        layer?.borderWidth = 0.5
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        return controller?.draggingEntered(sender) ?? []
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        return controller?.draggingUpdated(sender) ?? []
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        controller?.draggingExited(sender)
    }

    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        return true
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        return controller?.performDragOperation(sender) ?? false
    }
}

/// SwiftUI wrapper that binds hoveredDeviceId to DropZoneView
struct DropZoneWrapperView: View {
    @ObservedObject var controller: InvisibleNetPanelController

    init(controller: InvisibleNetPanelController) {
        self.controller = controller
    }

    var body: some View {
        DropZoneView(hoveredDeviceId: Binding(
            get: { controller.hoveredDeviceId },
            set: { controller.hoveredDeviceId = $0 }
        ))
    }
}
