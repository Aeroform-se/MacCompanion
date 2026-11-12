import Foundation

public final class DebugLogger {
    public static let shared = DebugLogger()
    private let logPath = "/tmp/macdrop_debug.log"
    private let queue = DispatchQueue(label: "se.aeroform.macdrop.logger")

    public static func log(_ message: String) {
        let ts = ISO8601DateFormatter().string(from: Date())
        let line = "[\(ts)] \(message)\n"
        print(line, terminator: "")

        shared.queue.async {
            if let data = line.data(using: .utf8) {
                if let handle = FileHandle(forWritingAtPath: shared.logPath) {
                    handle.seekToEndOfFile()
                    handle.write(data)
                    handle.closeFile()
                } else {
                    try? data.write(to: URL(fileURLWithPath: shared.logPath))
                }
            }
        }
    }
}
