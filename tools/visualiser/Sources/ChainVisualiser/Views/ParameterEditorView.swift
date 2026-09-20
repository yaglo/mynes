import SwiftUI

struct ParameterEditorView: View {
    let group: String
    let connection: EmulatorConnection
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                Label(group, systemImage: ChainGroup.named(group).symbol)
                    .font(.title2.weight(.semibold))
                Text(ChainGroup.named(group).detail)
                    .font(.callout).foregroundStyle(.secondary)
                Divider()
                ForEach(connection.controls.filter { $0.group == group }) { control in
                    PhysicalSlider(control: control) { value in
                        connection.updateControl(control.id, value: value)
                    }
                }
                if connection.controls.isEmpty {
                    Text("Controls appear when a compatible emulator connects.")
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 24)
                Text("Changes are live. Use Save or Save As above to keep them.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(24)
        }
        .disabled(connection.state != .connected)
    }
}

private struct PhysicalSlider: View {
    let control: PhysicalControl
    let apply: (Double) -> Void
    @State private var value: Double = 0
    @State private var editing = false
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(control.name).font(.callout)
                Spacer()
                Text(formatted(value)).font(.system(.callout, design: .monospaced)).foregroundStyle(.cyan)
            }
            Slider(value: $value, in: control.minimum...control.maximum, onEditingChanged: { active in
                editing = active
                if !active { apply(value) }
            })
            HStack {
                Text(formatted(control.minimum))
                Spacer()
                Text(formatted(control.maximum))
            }.font(.caption2.monospacedDigit()).foregroundStyle(.tertiary)
        }
        .onAppear { value = min(control.maximum, max(control.minimum, control.value)) }
        .onChange(of: control.value) { _, newValue in
            if !editing { value = min(control.maximum, max(control.minimum, newValue)) }
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel(control.name)
    }
    private func formatted(_ value: Double) -> String {
        if control.maximum >= 100000 { return String(format: "%.2f MHz", value / 1_000_000) }
        return String(format: control.maximum > 10 ? "%.1f" : "%.3f", value)
    }
}
