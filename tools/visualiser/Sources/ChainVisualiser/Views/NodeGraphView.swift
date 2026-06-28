import SwiftUI

struct NodeGraphView: View {
    let videoStages: [StageInfo]
    let audioStages: [StageInfo]
    @Binding var selectedStage: Int?
    var onBypassToggle: ((Int) -> Void)? = nil

    var body: some View {
        ScrollView(.vertical) {
            HStack(alignment: .top, spacing: 60) {
                // Video chain column
                if !videoStages.isEmpty {
                    chainColumn(title: "VIDEO CHAIN", stages: videoStages)
                }
                // Audio chain column
                if !audioStages.isEmpty {
                    chainColumn(title: "AUDIO CHAIN", stages: audioStages)
                }
            }
            .padding()
        }
    }

    @ViewBuilder
    private func chainColumn(title: String, stages: [StageInfo]) -> some View {
        VStack(spacing: 0) {
            Text(title)
                .font(.caption.bold())
                .foregroundStyle(.secondary)
                .padding(.bottom, 8)

            ForEach(Array(stages.enumerated()), id: \.element.id) { index, stage in
                VStack(spacing: 0) {
                    // Connection line above (skip first)
                    if index > 0 {
                        Rectangle()
                            .fill(Color.gray.opacity(0.3))
                            .frame(width: 2, height: 12)
                    }

                    StageNodeView(
                        stage: stage,
                        isSelected: selectedStage == stage.id
                    )
                    .onTapGesture {
                        selectedStage = stage.id
                    }
                    .onTapGesture(count: 2) {
                        onBypassToggle?(stage.id)
                    }
                }
            }
        }
    }
}

struct StageNodeView: View {
    let stage: StageInfo
    let isSelected: Bool

    var body: some View {
        HStack(spacing: 8) {
            kernelIcon
                .frame(width: 20, height: 20)

            VStack(alignment: .leading, spacing: 1) {
                Text(stage.name)
                    .font(.system(size: 11, weight: .medium))
                    .lineLimit(1)
                Text(stage.kernelType.rawValue)
                    .font(.system(size: 9))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Text(String(format: "%.0f", stage.timingUs))
                .font(.system(size: 10).monospacedDigit())
                .foregroundStyle(.secondary)
            Text("us")
                .font(.system(size: 8))
                .foregroundStyle(.tertiary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .frame(width: 200)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(backgroundColor)
        )
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .strokeBorder(borderColor, lineWidth: isSelected ? 2 : 1)
        )
    }

    private var borderColor: Color {
        if isSelected { return .accentColor }
        if !stage.enabled { return .gray.opacity(0.4) }
        if stage.bypassed { return .yellow.opacity(0.6) }
        return .green.opacity(0.6)
    }

    private var backgroundColor: Color {
        if isSelected { return .accentColor.opacity(0.08) }
        if !stage.enabled { return .gray.opacity(0.05) }
        if stage.bypassed { return .yellow.opacity(0.05) }
        return .green.opacity(0.05)
    }

    private var kernelIcon: some View {
        Image(systemName: kernelSymbol)
            .font(.system(size: 12))
            .foregroundStyle(iconColor)
    }

    private var kernelSymbol: String {
        switch stage.kernelType {
        case .pointwise: return "function"
        case .rcFilter:  return "waveform.path.ecg"
        case .fir:       return "line.3.horizontal.decrease"
        case .delay:     return "timer"
        case .comb:      return "tuningfork"
        case .modulator: return "wave.3.right"
        case .dac:       return "square.grid.2x2"
        case .matrix:    return "tablecells"
        case .beam:      return "line.horizontal.star.fill.line.horizontal"
        }
    }

    private var iconColor: Color {
        if !stage.enabled { return .gray }
        if stage.bypassed { return .yellow }
        return .green
    }
}
