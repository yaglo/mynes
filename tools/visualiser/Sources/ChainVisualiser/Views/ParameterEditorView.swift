import SwiftUI

struct ParameterEditorView: View {
    let stage: StageInfo
    let connection: EmulatorConnection

    // RC filter parameters
    @State private var rcAlpha: Double = 0.5
    // FIR filter parameters
    @State private var firTapCount: Int = 37
    @State private var firCutoff: Double = 0.1
    // Pointwise parameters
    @State private var pwGain: Double = 1.0
    @State private var pwOffset: Double = 0.0
    // Modulator parameters
    @State private var modFrequency: Double = 3.579545
    @State private var modPhase: Double = 0.0
    // Delay parameters
    @State private var delaySamples: Int = 0
    @State private var delayLevel: Double = 0.0

    var body: some View {
        Form {
            Section("Stage: \(stage.name)") {
                LabeledContent("Kernel") {
                    Text(stage.kernelType.description)
                        .foregroundStyle(.secondary)
                }
                LabeledContent("Status") {
                    HStack(spacing: 4) {
                        Circle()
                            .fill(stage.isActive ? .green : stage.bypassed ? .yellow : .gray)
                            .frame(width: 8, height: 8)
                        Text(stage.enabled ? (stage.bypassed ? "Bypassed" : "Active") : "Disabled")
                            .foregroundStyle(.secondary)
                    }
                }
                LabeledContent("Timing") {
                    Text(String(format: "%.1f us (avg %.1f us)", stage.timingUs, stage.timingAvgUs))
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            switch stage.kernelType {
            case .rcFilter:
                rcFilterSection
            case .fir:
                firFilterSection
            case .pointwise:
                pointwiseSection
            case .modulator:
                modulatorSection
            case .delay:
                delaySection
            default:
                Section("Parameters") {
                    Text("No editable parameters for this kernel type")
                        .foregroundStyle(.secondary)
                }
            }

            Section {
                HStack {
                    Button("Apply") { sendParams() }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                    Button("Reset") { resetParams() }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                }
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 280)
    }

    // MARK: - RC Filter

    private var rcFilterSection: some View {
        Section("RC Filter") {
            VStack(alignment: .leading, spacing: 4) {
                Text("Alpha: \(rcAlpha, specifier: "%.4f")")
                    .font(.caption.monospacedDigit())
                Slider(value: $rcAlpha, in: 0.0001...0.9999)
            }
            let cutoffHz = rcAlpha / (1.0 - rcAlpha) * 30000.0 / (2.0 * .pi)
            LabeledContent("Cutoff") {
                Text(String(format: "%.0f Hz", cutoffHz))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - FIR Filter

    private var firFilterSection: some View {
        Section("FIR Filter") {
            Stepper("Taps: \(firTapCount)", value: $firTapCount, in: 5...127, step: 2)
            VStack(alignment: .leading, spacing: 4) {
                Text("Cutoff: \(firCutoff, specifier: "%.4f")")
                    .font(.caption.monospacedDigit())
                Slider(value: $firCutoff, in: 0.005...0.499)
            }
            let bandwidthHz = firCutoff * 21477272.0 / 12.0
            LabeledContent("Bandwidth") {
                Text(String(format: "%.0f Hz", bandwidthHz))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - Pointwise

    private var pointwiseSection: some View {
        Section("Pointwise") {
            VStack(alignment: .leading, spacing: 4) {
                Text("Gain: \(pwGain, specifier: "%.3f")")
                    .font(.caption.monospacedDigit())
                Slider(value: $pwGain, in: 0.0...4.0)
            }
            VStack(alignment: .leading, spacing: 4) {
                Text("Offset: \(pwOffset, specifier: "%.3f")")
                    .font(.caption.monospacedDigit())
                Slider(value: $pwOffset, in: -1.0...1.0)
            }
        }
    }

    // MARK: - Modulator

    private var modulatorSection: some View {
        Section("Modulator") {
            VStack(alignment: .leading, spacing: 4) {
                Text("Frequency: \(modFrequency, specifier: "%.6f") MHz")
                    .font(.caption.monospacedDigit())
                Slider(value: $modFrequency, in: 0.1...10.0)
            }
            VStack(alignment: .leading, spacing: 4) {
                Text("Phase: \(modPhase, specifier: "%.3f") rad")
                    .font(.caption.monospacedDigit())
                Slider(value: $modPhase, in: 0.0...(2.0 * .pi))
            }
        }
    }

    // MARK: - Delay

    private var delaySection: some View {
        Section("Delay") {
            Stepper("Samples: \(delaySamples)", value: $delaySamples, in: 0...4096)
            VStack(alignment: .leading, spacing: 4) {
                Text("Level: \(delayLevel, specifier: "%.3f")")
                    .font(.caption.monospacedDigit())
                Slider(value: $delayLevel, in: -1.0...1.0)
            }
        }
    }

    // MARK: - Actions

    private func sendParams() {
        var data = Data()
        switch stage.kernelType {
        case .rcFilter:
            var a = Float(rcAlpha)
            let b = 1.0 - a
            withUnsafeBytes(of: &a) { data.append(contentsOf: $0) }
            var bVal = Float(b)
            withUnsafeBytes(of: &bVal) { data.append(contentsOf: $0) }
        case .fir:
            var taps = UInt32(firTapCount)
            withUnsafeBytes(of: &taps) { data.append(contentsOf: $0) }
            var cutoff = Float(firCutoff)
            withUnsafeBytes(of: &cutoff) { data.append(contentsOf: $0) }
        case .pointwise:
            var gain = Float(pwGain)
            withUnsafeBytes(of: &gain) { data.append(contentsOf: $0) }
            var offset = Float(pwOffset)
            withUnsafeBytes(of: &offset) { data.append(contentsOf: $0) }
        case .modulator:
            var freq = Float(modFrequency)
            withUnsafeBytes(of: &freq) { data.append(contentsOf: $0) }
            var phase = Float(modPhase)
            withUnsafeBytes(of: &phase) { data.append(contentsOf: $0) }
        case .delay:
            var samples = Int32(delaySamples)
            withUnsafeBytes(of: &samples) { data.append(contentsOf: $0) }
            var level = Float(delayLevel)
            withUnsafeBytes(of: &level) { data.append(contentsOf: $0) }
        default:
            return
        }
        connection.updateStageParams(data, stageIndex: stage.id)
    }

    private func resetParams() {
        rcAlpha = 0.5
        firTapCount = 37
        firCutoff = 0.1
        pwGain = 1.0
        pwOffset = 0.0
        modFrequency = 3.579545
        modPhase = 0.0
        delaySamples = 0
        delayLevel = 0.0
    }
}

// MARK: - KernelType description

extension KernelType: CustomStringConvertible {
    var description: String {
        switch self {
        case .pointwise: return "Pointwise"
        case .rcFilter:  return "RC Filter"
        case .fir:       return "FIR Filter"
        case .delay:     return "Delay"
        case .comb:      return "Comb Filter"
        case .modulator: return "Modulator"
        case .dac:       return "DAC (2C02)"
        case .matrix:    return "Matrix Decode"
        case .beam:      return "Beam Profile"
        }
    }
}
