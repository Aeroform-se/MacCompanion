import Foundation

public enum TransferStatus: String, Codable {
    case idle
    case preparing
    case connecting
    case streaming
    case finalizing
    case completed
    case failed
    case cancelled

    public var displayText: String {
        switch self {
        case .idle: return "Ready"
        case .preparing: return "Preparing..."
        case .connecting: return "Connecting to Mac..."
        case .streaming: return "Sending..."
        case .finalizing: return "Finalizing on Macintosh..."
        case .completed: return "Delivered!"
        case .failed: return "Transfer Failed"
        case .cancelled: return "Cancelled"
        }
    }
}

public struct TransferProgress: Identifiable {
    public let id = UUID()
    public var status: TransferStatus = .idle
    public var fileName: String = ""
    public var fileSize: Int64 = 0
    public var bytesSent: Int64 = 0
    public var targetName: String = ""
    public var targetIP: String = ""
    public var speedBps: Double = 0
    public var startedAt: Date = Date()
    public var completedAt: Date?
    public var errorMessage: String?

    public init() {}

    public var progressFraction: Double {
        guard fileSize > 0 else { return 0 }
        let fraction = Double(bytesSent) / Double(fileSize)
        return min(1.0, max(0.0, fraction))
    }

    public var percentInt: Int {
        Int(progressFraction * 100)
    }

    public var formattedSpeed: String {
        if status == .finalizing {
            return "Writing to Disk..."
        }
        if status == .completed {
            guard let end = completedAt else { return "Complete" }
            let duration = max(0.1, end.timeIntervalSince(startedAt))
            let avgBps = Double(fileSize) / duration
            return "\(formatBps(avgBps)) avg"
        }
        guard speedBps > 0 else { return "" }
        return formatBps(speedBps)
    }

    public var formattedProgress: String {
        "\(ByteCountFormatter.string(fromByteCount: bytesSent, countStyle: .file)) of \(ByteCountFormatter.string(fromByteCount: fileSize, countStyle: .file))"
    }

    public var formattedEta: String {
        guard status == .streaming, speedBps > 1024, fileSize > bytesSent else { return "" }
        let remainingBytes = Double(fileSize - bytesSent)
        let remainingSeconds = remainingBytes / speedBps
        if remainingSeconds < 1.0 { return "almost done" }
        if remainingSeconds < 60 {
            return String(format: "~%.0fs left", remainingSeconds)
        }
        let mins = Int(remainingSeconds / 60)
        let secs = Int(remainingSeconds) % 60
        return "\(mins)m \(secs)s left"
    }

    private func formatBps(_ bps: Double) -> String {
        if bps >= 1_048_576 {
            return String(format: "%.2f MB/s", bps / 1_048_576)
        } else {
            return String(format: "%.1f KB/s", bps / 1024)
        }
    }
}
