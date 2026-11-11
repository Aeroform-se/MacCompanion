import SwiftUI

@main
struct MacDropApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        // Runs as menu bar accessory with custom NSStatusItem and NSPanel
        Settings {
            EmptyView()
        }
    }
}
