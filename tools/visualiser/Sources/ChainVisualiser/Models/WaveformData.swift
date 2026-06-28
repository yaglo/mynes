import Foundation

struct WaveformData: Sendable {
    let stageIndex: Int
    let samples: [Float]
    let samplesPerLine: Int
    let lines: Int

    var sampleCount: Int { samples.count }

    /// Decode a tap data message (msg_type=4) from the debug server.
    ///
    /// Wire layout:
    ///   header:  msg_type(4) + payload_size(4)
    ///   payload: stage_index(4) + sample_count(4) + samples_per_line(4) + lines(4)
    ///            + sample_count * float32
    static func decode(from data: Data) -> WaveformData? {
        guard data.count >= 8 + 16 else { return nil }

        var offset = 0

        func readU32() -> UInt32? {
            guard offset + 4 <= data.count else { return nil }
            let value = data.withUnsafeBytes { ptr in
                ptr.loadUnaligned(fromByteOffset: offset, as: UInt32.self)
            }
            offset += 4
            return UInt32(littleEndian: value)
        }

        // Skip message header (msg_type + payload_size)
        guard let _ = readU32(), let _ = readU32() else { return nil }

        // Tap header: stage_index(4) + sample_count(4) + samples_per_line(4) + lines(4)
        guard let stageIndex = readU32(),
              let sampleCount = readU32(),
              let samplesPerLine = readU32(),
              let lines = readU32() else { return nil }

        guard sampleCount > 0, sampleCount < 10_000_000 else { return nil }
        guard offset + Int(sampleCount) * 4 <= data.count else { return nil }

        let samples: [Float] = data.withUnsafeBytes { ptr in
            var result = [Float](repeating: 0, count: Int(sampleCount))
            for i in 0..<Int(sampleCount) {
                let bits = ptr.loadUnaligned(fromByteOffset: offset + i * 4, as: UInt32.self)
                result[i] = Float(bitPattern: UInt32(littleEndian: bits))
            }
            return result
        }

        return WaveformData(
            stageIndex: Int(stageIndex),
            samples: samples,
            samplesPerLine: Int(samplesPerLine),
            lines: Int(lines)
        )
    }
}
