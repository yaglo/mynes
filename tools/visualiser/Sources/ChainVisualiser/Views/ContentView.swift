import SwiftUI

enum DetailTab: String, CaseIterable {
    case oscilloscope = "Oscilloscope"
    case parameters   = "Parameters"
    case performance  = "Performance"
}

struct ContentView: View {
    @ObservedObject var connection: EmulatorConnection
    @State private var selectedStage: Int? = nil
    @State private var detailTab: DetailTab = .oscilloscope

    private var selectedStageInfo: StageInfo? {
        guard let id = selectedStage else { return nil }
        return connection.snapshot.allStages.first(where: { $0.id == id })
    }

    var body: some View {
        NavigationSplitView {
            StageListView(
                videoStages: connection.snapshot.videoStages,
                audioStages: connection.snapshot.audioStages,
                selectedStage: $selectedStage
            )
            .navigationSplitViewColumnWidth(min: 220, ideal: 260)
        } detail: {
            VSplitView {
                NodeGraphView(
                    videoStages: connection.snapshot.videoStages,
                    audioStages: connection.snapshot.audioStages,
                    selectedStage: $selectedStage
                )
                .frame(minHeight: 200)

                VStack(spacing: 0) {
                    // Tab bar
                    Picker("View", selection: $detailTab) {
                        ForEach(DetailTab.allCases, id: \.self) { tab in
                            Text(tab.rawValue).tag(tab)
                        }
                    }
                    .pickerStyle(.segmented)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)

                    // Tab content
                    switch detailTab {
                    case .oscilloscope:
                        OscilloscopeView(waveform: connection.tapWaveform)
                    case .parameters:
                        if let stage = selectedStageInfo {
                            ParameterEditorView(stage: stage, connection: connection)
                        } else {
                            noSelectionPlaceholder
                        }
                    case .performance:
                        PerformanceDashboardView(stages: connection.snapshot.allStages)
                    }
                }
                .frame(minHeight: 200)
            }
        }
        .toolbar {
            ToolbarItem(placement: .automatic) {
                ConnectionStatusView(connection: connection)
            }
        }
        .onChange(of: selectedStage) { _, newValue in
            if let stageId = newValue {
                let chainType: ChainType =
                    connection.snapshot.videoStages.contains(where: { $0.id == stageId })
                    ? .video : .audio
                connection.requestTap(stageIndex: stageId, chainType: chainType)
            }
        }
        .onAppear {
            connection.connect()
        }
    }

    private var noSelectionPlaceholder: some View {
        VStack {
            Text("Select a stage to edit parameters")
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Stage List

struct StageListView: View {
    let videoStages: [StageInfo]
    let audioStages: [StageInfo]
    @Binding var selectedStage: Int?

    var body: some View {
        List(selection: $selectedStage) {
            if !videoStages.isEmpty {
                Section("Video") {
                    ForEach(videoStages) { stage in
                        StageRowView(stage: stage)
                            .tag(stage.id)
                    }
                }
            }
            if !audioStages.isEmpty {
                Section("Audio") {
                    ForEach(audioStages) { stage in
                        StageRowView(stage: stage)
                            .tag(stage.id)
                    }
                }
            }
            if videoStages.isEmpty && audioStages.isEmpty {
                Text("No stages")
                    .foregroundStyle(.secondary)
            }
        }
        .listStyle(.sidebar)
    }
}

struct StageRowView: View {
    let stage: StageInfo

    var body: some View {
        HStack {
            Circle()
                .fill(statusColor)
                .frame(width: 8, height: 8)
            VStack(alignment: .leading, spacing: 2) {
                Text(stage.name)
                    .font(.body)
                Text(stage.kernelType.rawValue)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Text(String(format: "%.0f us", stage.timingUs))
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
        }
    }

    private var statusColor: Color {
        if !stage.enabled { return .gray }
        if stage.bypassed { return .yellow }
        return .green
    }
}

// MARK: - Connection Status

struct ConnectionStatusView: View {
    @ObservedObject var connection: EmulatorConnection

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(indicatorColor)
                .frame(width: 8, height: 8)
            Text(connection.state.label)
                .font(.caption)

            if case .disconnected = connection.state {
                Button("Connect") { connection.connect() }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            } else if case .error = connection.state {
                Button("Retry") { connection.connect() }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            }
        }
    }

    private var indicatorColor: Color {
        switch connection.state {
        case .connected:    return .green
        case .connecting:   return .orange
        case .disconnected: return .gray
        case .error:        return .red
        }
    }
}
