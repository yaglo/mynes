import SwiftUI
import AppKit

@main
struct ChainVisualiserApp: App {
    @State private var connection = EmulatorConnection()

    init() {
        // Required for SwiftPM executables: register as a regular GUI app
        // so macOS shows the window and dock icon.
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup {
            ContentView(connection: connection)
                .task {
                    // Optional UI review capture of this app's own view.
                    guard let path = ProcessInfo.processInfo.environment["MYNES_EDITOR_CAPTURE"] else { return }
                    try? await Task.sleep(for: .seconds(2))
                    guard let view = NSApplication.shared.windows.first(where: { $0.isVisible })?.contentView,
                          let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
                    view.cacheDisplay(in: view.bounds, to: bitmap)
                    if let data = bitmap.representation(using: .png, properties: [:]) {
                        try? data.write(to: URL(fileURLWithPath: path))
                    }
                }
        }
        .defaultSize(width: 1180, height: 820)
    }
}
