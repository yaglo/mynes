import SwiftUI

struct ContentView: View {
    let connection: EmulatorConnection
    @State private var group = "Decoder"
    @State private var showTiming = false
    var body: some View {
        VStack(spacing: 0) {
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 5) {
                    Text("Signal Studio").font(.system(size: 28, weight: .semibold, design: .rounded))
                    Text("NES COMPOSITE · CRT WORKBENCH").font(.system(size: 10, weight: .medium, design: .monospaced))
                        .tracking(2).foregroundStyle(.secondary)
                }
                Spacer()
                Circle().fill(connection.state == .connected ? Color.green : Color.orange).frame(width: 7, height: 7)
                Text(connection.state.label).font(.callout).foregroundStyle(.secondary).lineLimit(1)
                if connection.state != .connected {
                    Button("Connect") { connection.connect() }
                }
            }.padding(26)
            Divider()
            PresetBar(connection: connection)
            Divider()
            HStack {
                Text("Video signal path").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button { group = "Audio" } label: { Label("Audio controls", systemImage: "speaker.wave.2") }
                    .tint(group == "Audio" ? .cyan : .secondary)
            }.padding(.horizontal, 24).padding(.top, 12)
            NodeGraphView(stages: connection.videoStages, selectedGroup: $group)
            Divider()
            HSplitView {
                VStack(alignment: .leading, spacing: 16) {
                    HStack {
                        Text("Execution stages").font(.headline)
                        Spacer()
                        Toggle("CPU dispatch timing", isOn: $showTiming).toggleStyle(.checkbox).font(.caption)
                    }
                    if connection.videoStages.isEmpty {
                        ContentUnavailableView("Waiting for the emulator", systemImage: "waveform.path",
                            description: Text("Launch mynes_gpu with --debug-server. The graph and controls will follow the running chain."))
                    } else if showTiming {
                        LiveTimingView(connection: connection)
                    } else {
                        ScrollView {
                            LazyVStack(spacing: 5) {
                                ForEach(connection.videoStages) { stage in
                                    Button { group = stage.kernelType.group } label: {
                                        HStack(spacing: 12) {
                                            Text(String(format: "%02d", stage.id + 1)).font(.caption.monospaced()).foregroundStyle(.tertiary)
                                            Circle().fill(stage.isActive ? Color.green : Color.gray).frame(width: 6, height: 6)
                                            Text(stage.name).font(.callout)
                                            Spacer()
                                            Text(stage.isActive ? stage.kernelType.rawValue : "Inactive")
                                                .font(.caption).foregroundStyle(.secondary)
                                        }
                                        .padding(12)
                                        .background(stage.kernelType.group == group ? Color.cyan.opacity(0.07) : Color.clear,
                                                    in: RoundedRectangle(cornerRadius: 7))
                                    }.buttonStyle(.plain)
                                }
                            }
                        }
                    }
                    Text("The graph groups physical stages. Execution order and Y/C routing remain controlled by the emulator.")
                        .font(.caption).foregroundStyle(.secondary)
                }.padding(24).frame(minWidth: 400)
                ParameterEditorView(group: group, connection: connection).frame(minWidth: 340, idealWidth: 370, maxWidth: 460)
            }
            Divider()
            ConnectionFooter(connection: connection)
        }
        .background(Color(red: 0.055, green: 0.07, blue: 0.085))
        .preferredColorScheme(.dark)
        .frame(minWidth: 900, minHeight: 650)
        .onAppear { connection.connect() }
    }
}

private struct LiveTimingView: View {
    let connection: EmulatorConnection
    var body: some View { PerformanceDashboardView(stages: connection.snapshot.videoStages) }
}

private struct ConnectionFooter: View {
    let connection: EmulatorConnection
    var body: some View {
        HStack {
            Text(connection.socketPath)
            Spacer()
            Text(String(format: "%.1f fps · Frame %u", connection.framesPerSecond, connection.frameNumber))
        }.font(.caption.monospaced()).foregroundStyle(.tertiary).padding(12)
    }
}
