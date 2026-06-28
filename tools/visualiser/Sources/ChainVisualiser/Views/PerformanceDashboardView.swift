import SwiftUI

struct PerformanceDashboardView: View {
    let stages: [StageInfo]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            // Header
            HStack {
                Text("GPU Performance")
                    .font(.headline)
                Spacer()
                Text(String(format: "Total: %.0f us/frame", totalUs))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                frameTimeBadge
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

    private var frameTimeBadge: some View {
        let frameMs = totalUs / 1000.0
        let color: Color = frameMs < 16.67 ? .green : frameMs < 33.33 ? .yellow : .red
        return Text(String(format: "%.2f ms", frameMs))
            .font(.caption.monospacedDigit().bold())
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(color.opacity(0.15), in: RoundedRectangle(cornerRadius: 4))
            .foregroundStyle(color)
    }

    // MARK: - Helpers

    private var enabledStages: [StageInfo] {
        stages.filter { $0.enabled }
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
