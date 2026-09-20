import SwiftUI

struct PerformanceDashboardView: View {
    let stages: [StageInfo]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Header
            HStack {
                Text("CPU command encoding")
                    .font(.headline)
                Spacer()
                Text(String(format: "Dispatch total: %.0f µs", totalUs))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                // These are CPU encoding times, not GPU execution durations.
            }

            if enabledStages.isEmpty {
                Text("No active stages")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    VStack(spacing: 4) {
                        ForEach(enabledStages) { stage in
                            timingRow(stage)
                        }
                    }
                }
            }
        }
        .padding()
    }

    // MARK: - Subviews

    private func timingRow(_ stage: StageInfo) -> some View {
        HStack(spacing: 8) {
            Text(stage.name)
                .font(.system(size: 11))
                .frame(width: 130, alignment: .leading)
                .lineLimit(1)

            GeometryReader { geo in
                let fraction = totalUs > 0 ? stage.timingAvgUs / totalUs : 0
                ZStack(alignment: .leading) {
                    Rectangle()
                        .fill(Color.gray.opacity(0.1))
                    Rectangle()
                        .fill(barColor(stage.timingAvgUs))
                        .frame(width: max(1, geo.size.width * fraction))
                }
                .clipShape(RoundedRectangle(cornerRadius: 2))
            }
            .frame(height: 14)

            Text(String(format: "%.0f us", stage.timingAvgUs))
                .font(.system(size: 10).monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(width: 50, alignment: .trailing)

            Text(String(format: "%.0f%%", totalUs > 0 ? stage.timingAvgUs / totalUs * 100 : 0))
                .font(.system(size: 10).monospacedDigit())
                .foregroundStyle(.tertiary)
                .frame(width: 36, alignment: .trailing)
        }
    }

    // MARK: - Helpers

    private var enabledStages: [StageInfo] {
        stages.filter { $0.isActive }
    }

    private var totalUs: Double {
        enabledStages.reduce(0) { $0 + $1.timingAvgUs }
    }

    private func barColor(_ timingUs: Double) -> Color {
        if timingUs < 50 { return .green }
        if timingUs < 200 { return .yellow }
        return .orange
    }
}
