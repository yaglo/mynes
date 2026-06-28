import SwiftUI

struct OscilloscopeView: View {
    let waveform: WaveformData?
    @State private var scanline: Int = 120
    @State private var zoom: CGFloat = 1.0
    @State private var offset: CGFloat = 0.0
    @State private var hoverSample: Int? = nil
    @State private var hoverValue: Float? = nil
    @State private var isDragging = false
    @State private var dragStart: CGFloat = 0.0

    var body: some View {
        VStack(spacing: 0) {
            // Header bar
            HStack {
                Text("Oscilloscope")
                    .font(.headline)
                Spacer()
                if let wf = waveform {
                    Text("Stage \(wf.stageIndex)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("\(wf.samplesPerLine) spl")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
                if let idx = hoverSample, let val = hoverValue {
                    Text("[\(idx)] = \(val, specifier: "%.4f")")
                        .font(.caption.monospacedDigit())
                        .padding(.horizontal, 6)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 4))
                }
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)

            // Waveform canvas
            GeometryReader { geo in
                let size = geo.size
                Canvas { context, canvasSize in
                    drawBackground(context: context, size: canvasSize)

                    guard let wf = waveform, wf.samplesPerLine > 0 else {
                        drawNoData(context: context, size: canvasSize)
                        return
                    }

                    let samples = scanlineSamples(wf)
                    guard !samples.isEmpty else { return }

                    let range = amplitudeRange(samples)
                    drawGrid(context: context, size: canvasSize, range: range, sampleCount: samples.count)
                    drawWaveform(context: context, size: canvasSize, samples: samples, range: range)
                }
                .gesture(
                    DragGesture(minimumDistance: 1)
                        .onChanged { value in
                            if !isDragging {
                                isDragging = true
                                dragStart = offset
                            }
                            let dx = value.translation.width / zoom
                            offset = dragStart - dx
                        }
                        .onEnded { _ in
                            isDragging = false
                        }
                )
                .onContinuousHover { phase in
                    switch phase {
                    case .active(let location):
                        updateHover(at: location, size: size)
                    case .ended:
                        hoverSample = nil
                        hoverValue = nil
                    }
                }
                .onAppear {
                    // Reset zoom/offset when waveform changes
                    zoom = 1.0
                    offset = 0.0
                }
            }
            .background(Color.black)
            .clipShape(RoundedRectangle(cornerRadius: 4))
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .strokeBorder(Color.gray.opacity(0.3), lineWidth: 1)
            )
            .gesture(
                MagnifyGesture()
                    .onChanged { value in
                        zoom = max(0.1, min(100, value.magnification))
                    }
            )

            // Scanline selector
            if let wf = waveform, wf.lines > 1 {
                HStack {
                    Text("Scanline:")
                        .font(.caption)
                    Text("\(scanline)")
                        .font(.caption.monospacedDigit())
                        .frame(width: 30, alignment: .trailing)
                    Slider(
                        value: Binding(
                            get: { Double(scanline) },
                            set: { scanline = Int($0) }
                        ),
                        in: 0...Double(max(0, wf.lines - 1)),
                        step: 1
                    )
                    HStack(spacing: 4) {
                        Button("-") { zoom = max(0.1, zoom / 1.5) }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                        Text("\(zoom, specifier: "%.1f")x")
                            .font(.caption.monospacedDigit())
                            .frame(width: 36)
                        Button("+") { zoom = min(100, zoom * 1.5) }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                        Button("Reset") {
                            zoom = 1.0
                            offset = 0.0
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    }
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
            }
        }
    }

    // MARK: - Data helpers

    private func scanlineSamples(_ wf: WaveformData) -> [Float] {
        let spl = wf.samplesPerLine
        guard spl > 0, scanline < wf.lines else {
            // Audio or single-line: return all samples
            return wf.samples
        }
        let start = scanline * spl
        let end = min(start + spl, wf.samples.count)
        guard start < wf.samples.count else { return [] }
        return Array(wf.samples[start..<end])
    }

    private func amplitudeRange(_ samples: [Float]) -> (min: Float, max: Float) {
        guard !samples.isEmpty else { return (-1, 1) }
        var lo = samples[0], hi = samples[0]
        for s in samples {
            if s < lo { lo = s }
            if s > hi { hi = s }
        }
        // Add 10% margin
        let margin = max((hi - lo) * 0.1, 0.01)
        return (lo - margin, hi + margin)
    }

    // MARK: - Drawing

    private func drawBackground(context: GraphicsContext, size: CGSize) {
        context.fill(
            Path(CGRect(origin: .zero, size: size)),
            with: .color(Color(white: 0.05))
        )
    }

    private func drawNoData(context: GraphicsContext, size: CGSize) {
        context.draw(
            Text("No waveform data")
                .font(.callout)
                .foregroundStyle(.gray),
            at: CGPoint(x: size.width / 2, y: size.height / 2)
        )
    }

    private func drawGrid(context: GraphicsContext, size: CGSize, range: (min: Float, max: Float), sampleCount: Int) {
        let gridColor = Color.gray.opacity(0.2)

        // Horizontal grid: ~5 divisions
        let ySpan = range.max - range.min
        let yStep = niceStep(Double(ySpan), targetDivisions: 5)
        let firstY = ceil(Double(range.min) / yStep) * yStep
        var y = firstY
        while Float(y) <= range.max {
            let py = mapY(Float(y), range: range, height: size.height)
            var path = Path()
            path.move(to: CGPoint(x: 0, y: py))
            path.addLine(to: CGPoint(x: size.width, y: py))
            context.stroke(path, with: .color(gridColor), lineWidth: 0.5)

            // Label
            context.draw(
                Text(String(format: "%.2f", y))
                    .font(.system(size: 9).monospacedDigit())
                    .foregroundStyle(Color.gray.opacity(0.6)),
                at: CGPoint(x: 28, y: py - 6)
            )
            y += yStep
        }

        // Zero line (brighter)
        if range.min < 0 && range.max > 0 {
            let zeroY = mapY(0, range: range, height: size.height)
            var path = Path()
            path.move(to: CGPoint(x: 0, y: zeroY))
            path.addLine(to: CGPoint(x: size.width, y: zeroY))
            context.stroke(path, with: .color(Color.gray.opacity(0.4)), lineWidth: 0.5)
        }

        // Vertical grid: sample index ticks
        let visibleSamples = Double(sampleCount) / Double(zoom)
        let xStep = niceStep(visibleSamples, targetDivisions: 8)
        let firstX = ceil(Double(offset) / xStep) * xStep
        var xi = firstX
        let lastVisible = Double(offset) + visibleSamples
        while xi <= lastVisible {
            let px = mapX(xi, sampleCount: sampleCount, width: size.width)
            var path = Path()
            path.move(to: CGPoint(x: px, y: 0))
            path.addLine(to: CGPoint(x: px, y: size.height))
            context.stroke(path, with: .color(gridColor), lineWidth: 0.5)
            xi += xStep
        }
    }

    private func drawWaveform(context: GraphicsContext, size: CGSize, samples: [Float], range: (min: Float, max: Float)) {
        guard samples.count > 1 else { return }

        var path = Path()
        let visibleStart = max(0, Int(offset))
        let visibleEnd = min(samples.count, Int(Double(offset) + Double(samples.count) / Double(zoom)) + 1)
        guard visibleStart < visibleEnd else { return }

        var first = true
        for i in visibleStart..<visibleEnd {
            let px = mapX(Double(i), sampleCount: samples.count, width: size.width)
            let py = mapY(samples[i], range: range, height: size.height)
            let pt = CGPoint(x: px, y: py)
            if first {
                path.move(to: pt)
                first = false
            } else {
                path.addLine(to: pt)
            }
        }

        context.stroke(path, with: .color(.green), lineWidth: 1.0)
    }

    // MARK: - Coordinate mapping

    private func mapX(_ sampleIndex: Double, sampleCount: Int, width: CGFloat) -> CGFloat {
        let visibleSamples = Double(sampleCount) / Double(zoom)
        return CGFloat((sampleIndex - Double(offset)) / visibleSamples) * width
    }

    private func mapY(_ value: Float, range: (min: Float, max: Float), height: CGFloat) -> CGFloat {
        let normalized = CGFloat((value - range.min) / (range.max - range.min))
        return (1.0 - normalized) * height
    }

    private func updateHover(at location: CGPoint, size: CGSize) {
        guard let wf = waveform else { return }
        let samples = scanlineSamples(wf)
        guard !samples.isEmpty else { return }

        let visibleSamples = Double(samples.count) / Double(zoom)
        let sampleIndex = Int(Double(offset) + (Double(location.x) / Double(size.width)) * visibleSamples)
        if sampleIndex >= 0 && sampleIndex < samples.count {
            hoverSample = sampleIndex
            hoverValue = samples[sampleIndex]
        }
    }

    // MARK: - Utilities

    private func niceStep(_ range: Double, targetDivisions: Int) -> Double {
        let rough = range / Double(targetDivisions)
        let mag = pow(10, floor(log10(rough)))
        let norm = rough / mag
        let nice: Double
        if norm < 1.5 { nice = 1 }
        else if norm < 3.5 { nice = 2 }
        else if norm < 7.5 { nice = 5 }
        else { nice = 10 }
        return nice * mag
    }
}
