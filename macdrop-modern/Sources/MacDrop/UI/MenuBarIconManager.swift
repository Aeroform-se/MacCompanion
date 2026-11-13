import AppKit
import Combine

/// Manages the visual rendering of the NSStatusItem in the macOS menu bar.
/// Displays an icon in idle state, and dynamically renders a circular progress ring
/// and badge when a file transfer is actively running in the background.
public final class MenuBarIconManager: NSObject, NSDraggingDestination {
    public static let shared = MenuBarIconManager()

    public var statusItem: NSStatusItem!
    private var cancellables = Set<AnyCancellable>()

    public override init() {
        super.init()
    }

    public func setupStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        guard let button = statusItem.button else { return }

        button.image = createBaseStatusIcon()
        button.imagePosition = .imageLeft
        button.action = #selector(statusItemClicked)
        button.target = self
        button.sendAction(on: [.leftMouseUp, .rightMouseUp])

        // Register button as a direct drag destination as well
        button.registerForDraggedTypes([.fileURL])

        // Add invisible drop target overlay over status bar button to intercept drags & clicks directly
        let dropOverlay = MenuBarDropTargetOverlay(frame: button.bounds)
        dropOverlay.autoresizingMask = [.width, .height]
        button.addSubview(dropOverlay)

        // Link with Invisible Net Panel Controller
        InvisibleNetPanelController.shared.statusItemButton = button

        // Observe active transfers to update icon progress ring
        P2PTransferEngine.shared.$currentTransfer
            .receive(on: DispatchQueue.main)
            .sink { [weak self] xfer in
                self?.updateIconAppearance(transfer: xfer)
            }
            .store(in: &cancellables)
    }

    @objc private func statusItemClicked() {
        InvisibleNetPanelController.shared.toggleVisibility()
    }

    // MARK: - Dragging Destination on Menu Bar Button

    public func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        return InvisibleNetPanelController.shared.draggingEntered(sender)
    }

    public func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        return InvisibleNetPanelController.shared.draggingUpdated(sender)
    }

    public func draggingExited(_ sender: NSDraggingInfo?) {
        InvisibleNetPanelController.shared.draggingExited(sender)
    }

    public func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        return InvisibleNetPanelController.shared.performDragOperation(sender)
    }

    // MARK: - Dynamic Icon Rendering

    private func updateIconAppearance(transfer: TransferProgress?) {
        guard let button = statusItem.button else { return }

        guard let xfer = transfer, (xfer.status == .streaming || xfer.status == .finalizing) else {
            button.image = createBaseStatusIcon()
            button.title = ""
            button.toolTip = "MacDrop X"
            return
        }

        // Render circular progress ring around icon (keep title empty so icon position & window X-anchor never shift)
        let progress = CGFloat(xfer.progressFraction)
        button.image = createProgressIcon(fraction: progress)
        button.title = ""
        button.toolTip = xfer.status == .finalizing ? "MacDrop X: Finalizing..." : "MacDrop X: \(xfer.percentInt)%"
    }

    /// Creates the standard monochrome menu bar icon (Retro Mac with a downward drop arrow)
    public func createBaseStatusIcon() -> NSImage {
        // 1. Check if user provided custom MenuBarIcon in Resources (loads @1x and @2x)
        if let custom = NSImage(named: "MenuBarIcon") {
            custom.isTemplate = true
            custom.size = NSSize(width: 18, height: 18)
            return custom
        }

        if let resURL = Bundle.main.resourceURL {
            let img = NSImage(size: NSSize(width: 18, height: 18))
            let url2x = resURL.appendingPathComponent("MenuBarIcon@2x.png")
            let url1x = resURL.appendingPathComponent("MenuBarIcon.png")
            if let data2x = try? Data(contentsOf: url2x), let rep2x = NSBitmapImageRep(data: data2x) {
                rep2x.size = NSSize(width: 18, height: 18)
                img.addRepresentation(rep2x)
            }
            if let data1x = try? Data(contentsOf: url1x), let rep1x = NSBitmapImageRep(data: data1x) {
                rep1x.size = NSSize(width: 18, height: 18)
                img.addRepresentation(rep1x)
            }
            if !img.representations.isEmpty {
                img.isTemplate = true
                return img
            }
        }

        let size = NSSize(width: 18, height: 18)
        let img = NSImage(size: size, flipped: false) { rect in
            let ctx = NSGraphicsContext.current?.cgContext

            // 1. Draw tiny retro classic Mac CRT silhouette (12x12)
            let macRect = NSRect(x: 1, y: 3, width: 14, height: 13)
            let path = NSBezierPath(roundedRect: macRect, xRadius: 2.5, yRadius: 2.5)
            path.lineWidth = 1.3
            NSColor.labelColor.setStroke()
            path.stroke()

            // Screen inset
            let screenRect = NSRect(x: 3, y: 7, width: 10, height: 7)
            let screenPath = NSBezierPath(roundedRect: screenRect, xRadius: 1, yRadius: 1)
            screenPath.lineWidth = 1.0
            screenPath.stroke()

            // Floppy slot
            let slotPath = NSBezierPath()
            slotPath.move(to: NSPoint(x: 8, y: 4.5))
            slotPath.line(to: NSPoint(x: 12, y: 4.5))
            slotPath.lineWidth = 1.0
            slotPath.stroke()

            // Downward transfer arrow in top right
            ctx?.saveGState()
            let arrowPath = NSBezierPath()
            arrowPath.move(to: NSPoint(x: 15, y: 15))
            arrowPath.line(to: NSPoint(x: 15, y: 7))
            arrowPath.move(to: NSPoint(x: 13, y: 9.5))
            arrowPath.line(to: NSPoint(x: 15, y: 7))
            arrowPath.line(to: NSPoint(x: 17, y: 9.5))
            arrowPath.lineWidth = 1.5
            arrowPath.lineCapStyle = .round
            arrowPath.lineJoinStyle = .round
            NSColor.labelColor.setStroke()
            arrowPath.stroke()
            ctx?.restoreGState()

            return true
        }

        img.isTemplate = true
        return img
    }

    /// Renders dynamic pie / circular progress ring around icon
    private func createProgressIcon(fraction: CGFloat) -> NSImage {
        let size = NSSize(width: 18, height: 18)
        let img = NSImage(size: size, flipped: false) { rect in
            let center = NSPoint(x: 9, y: 9)
            let radius: CGFloat = 7.0

            // Background circle track
            let trackPath = NSBezierPath()
            trackPath.appendArc(withCenter: center, radius: radius, startAngle: 0, endAngle: 360)
            trackPath.lineWidth = 1.5
            NSColor.labelColor.withAlphaComponent(0.2).setStroke()
            trackPath.stroke()

            // Filled progress arc
            let progressPath = NSBezierPath()
            let startAngle: CGFloat = 90.0
            let endAngle: CGFloat = 90.0 - (fraction * 360.0)
            progressPath.appendArc(withCenter: center, radius: radius, startAngle: startAngle, endAngle: endAngle, clockwise: true)
            progressPath.lineWidth = 2.0
            progressPath.lineCapStyle = .round
            NSColor.controlAccentColor.setStroke()
            progressPath.stroke()

            // Center down arrow
            let arrowPath = NSBezierPath()
            arrowPath.move(to: NSPoint(x: 9, y: 12))
            arrowPath.line(to: NSPoint(x: 9, y: 6))
            arrowPath.move(to: NSPoint(x: 7, y: 8))
            arrowPath.line(to: NSPoint(x: 9, y: 6))
            arrowPath.line(to: NSPoint(x: 11, y: 8))
            arrowPath.lineWidth = 1.5
            arrowPath.lineCapStyle = .round
            arrowPath.lineJoinStyle = .round
            NSColor.controlAccentColor.setStroke()
            arrowPath.stroke()

            return true
        }

        img.isTemplate = false
        return img
    }
}

/// Transparent overlay view placed over the NSStatusBarButton to capture drag-and-drop operations
/// and mouse clicks immediately from Finder.
final class MenuBarDropTargetOverlay: NSView {
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        registerForDraggedTypes([.fileURL])
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        return InvisibleNetPanelController.shared.draggingEntered(sender)
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        return InvisibleNetPanelController.shared.draggingUpdated(sender)
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        let mouseLoc = NSEvent.mouseLocation
        if InvisibleNetPanelController.shared.isMouseInProximity(mouseLoc) {
            InvisibleNetPanelController.shared.scheduleDismissCheck(delay: 0.10)
        } else {
            InvisibleNetPanelController.shared.hideDropZone(animated: true)
        }
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        return InvisibleNetPanelController.shared.performDragOperation(sender)
    }

    override func mouseDown(with event: NSEvent) {
        InvisibleNetPanelController.shared.toggleVisibility()
    }

    override func rightMouseDown(with event: NSEvent) {
        InvisibleNetPanelController.shared.toggleVisibility()
    }
}
