import SwiftUI
import AppKit

@main
struct ChainVisualiserApp: App {
    @StateObject private var connection = EmulatorConnection()

    init() {
        // Required for SwiftPM executables: register as a regular GUI app
        // so macOS shows the window and dock icon.
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup {
            ContentView(connection: connection)
        }
        .defaultSize(width: 960, height: 640)
    }
}
