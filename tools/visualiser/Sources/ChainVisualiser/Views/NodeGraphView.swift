import SwiftUI

struct ChainGroup: Identifiable {
    let name: String, symbol: String, subtitle: String, detail: String
    var id: String { name }
    static let all: [ChainGroup] = [
        .init(name: "Connection", symbol: "cable.connector", subtitle: "Console → receiver", detail: "Shape the signal before decoding: cable bandwidth, receiver noise, and power-supply hum."),
        .init(name: "Decoder", symbol: "waveform.path", subtitle: "Composite → color", detail: "Separate brightness and color. Bandwidth affects fine detail, color bleed, and composite artifacts."),
        .init(name: "Beam", symbol: "scope", subtitle: "Voltage → scanlines", detail: "Adjust the electron beam’s focus and brightness-dependent width."),
        .init(name: "Phosphor", symbol: "sparkles", subtitle: "Light over time", detail: "Control the phosphor mask and exponential afterglow. Zero persistence disables decay history."),
        .init(name: "Glass", symbol: "display", subtitle: "Screen → room", detail: "Adjust screen geometry, scattered light, and the viewing environment.")
    ]
    static let audio = ChainGroup(name: "Audio", symbol: "speaker.wave.2", subtitle: "APU → speaker",
        detail: "Adjust amplifier compression, power-supply hum, and noise. CPU and GPU audio use the same continuous console, cable, and speaker model. Switch processing with A in the emulator or Setup → Audio.")
    static func named(_ name: String) -> ChainGroup { name == "Audio" ? audio : all.first { $0.name == name } ?? all[0] }
}

struct NodeGraphView: View {
    let stages: [StageInfo]
    @Binding var selectedGroup: String
    var body: some View {
        GeometryReader { geometry in
            let cardWidth: CGFloat = max(120, (geometry.size.width - 112) / 5)
            HStack(spacing: 0) {
                ForEach(Array(ChainGroup.all.enumerated()), id: \.element.id) { index, group in
                    if index > 0 {
                        Image(systemName: "arrow.right").font(.caption.weight(.semibold))
                            .foregroundStyle(.cyan.opacity(0.5)).frame(width: 20)
                    }
                    Button { selectedGroup = group.name } label: {
                        ChainCard(group: group, index: index, selected: selectedGroup == group.name,
                                  count: stages.filter { $0.kernelType.group == group.name && $0.isActive }.count,
                                  width: cardWidth)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Edit \(group.name)")
                }
            }.padding(.horizontal, 16).padding(.vertical, 24)
        }
        .frame(height: 192)
        .background {
            Canvas { context, size in
                var grid = Path()
                for x in stride(from: 0.0, to: size.width, by: 24) {
                    for y in stride(from: 0.0, to: size.height, by: 24) {
                        grid.addEllipse(in: CGRect(x: x, y: y, width: 1, height: 1))
                    }
                }
                context.fill(grid, with: .color(.white.opacity(0.12)))
            }
        }
    }
}

private struct ChainCard: View {
    let group: ChainGroup
    let index: Int
    let selected: Bool
    let count: Int
    let width: CGFloat
    private var status: String {
        switch group.name {
        case "Glass": return "Display pass"
        case "Phosphor": return "Afterglow + mask"
        default: return "\(count) active stages"
        }
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Image(systemName: group.symbol).font(.title2).foregroundStyle(.cyan)
                Spacer()
                Text(String(format: "%02d", index + 1)).font(.caption.monospaced()).foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 5) {
                Text(group.name).font(.headline)
                Text(group.subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1).minimumScaleFactor(0.8)
            }
            Text(status).font(.caption2.monospaced()).foregroundStyle(.secondary).lineLimit(1).minimumScaleFactor(0.8)
        }
        .padding(14).frame(width: width, height: 144, alignment: .leading)
        .background(selected ? Color.cyan.opacity(0.12) : Color.white.opacity(0.035), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(selected ? Color.cyan.opacity(0.8) : Color.white.opacity(0.1)))
    }
}
