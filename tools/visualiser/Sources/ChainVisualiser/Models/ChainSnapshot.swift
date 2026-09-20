import Foundation

struct ChainSnapshot: Sendable {
    let frameNumber: UInt32
    let timestampMs: UInt32
    let videoStages: [StageInfo]
    let audioStages: [StageInfo]
    let tapWaveform: WaveformData?

    var allStages: [StageInfo] { videoStages + audioStages }

    var totalTimingUs: Double {
        allStages.reduce(0) { $0 + $1.timingUs }
    }

    static let empty = ChainSnapshot(
        frameNumber: 0,
        timestampMs: 0,
        videoStages: [],
        audioStages: [],
        tapWaveform: nil
    )
}

// MARK: - Binary decoding

extension ChainSnapshot {
    /// Message types in the binary protocol.
    enum MessageType: UInt32 {
        case snapshot     = 0
        case paramUpdate  = 1
        case presetChange = 2
    }

    /// Decode a snapshot from the binary wire format (little-endian).
    ///
    /// Wire layout (msg_type=0):
    ///   header:  msg_type(4) + payload_size(4)
    ///   payload: frame_number(4) + timestamp_ms(4)
    ///            + num_video_stages(2) + num_audio_stages(2)
    ///            + per-stage records (44 bytes each:
    ///              enabled(1) + bypassed(1) + kernel_type(1) + pad(1)
    ///              + timing_us(4) + timing_avg_us(4) + name(32))
    ///
    /// Tap waveform data arrives as a separate message (msg_type=4).
    static func decode(from data: Data) -> ChainSnapshot? {
        guard data.count >= 8 else { return nil }

        var offset = 0

        func read<T: FixedWidthInteger>(_ type: T.Type) -> T? {
            let size = MemoryLayout<T>.size
            guard offset + size <= data.count else { return nil }
            let value = data.withUnsafeBytes { ptr in
                ptr.loadUnaligned(fromByteOffset: offset, as: T.self)
            }
            offset += size
            return T(littleEndian: value)
        }

        func skip(_ count: Int) {
            offset += count
        }

        func readFixedString(_ length: Int) -> String {
            guard offset + length <= data.count else { return "?" }
            let slice = data[offset..<(offset + length)]
            offset += length
            // Find null terminator
            if let nullIdx = slice.firstIndex(of: 0) {
                return String(data: data[slice.startIndex..<nullIdx], encoding: .utf8) ?? "?"
            }
            return String(data: slice, encoding: .utf8) ?? "?"
        }

        // Header
        guard let msgType = read(UInt32.self),
              msgType == MessageType.snapshot.rawValue,
              let payloadSize = read(UInt32.self),
              payloadSize == data.count - 8 else { return nil }

        // Snapshot header
        guard let frameNumber = read(UInt32.self),
              let timestampMs = read(UInt32.self),
              let numVideo = read(UInt16.self),
              let numAudio = read(UInt16.self) else { return nil }

        let totalStages = Int(numVideo) + Int(numAudio)
        guard totalStages <= 64, data.count == 20 + totalStages * 44 else { return nil }

        // Per-stage metadata (C struct DebugStageInfo is 44 bytes)
        var videoStages: [StageInfo] = []
        var audioStages: [StageInfo] = []

        for i in 0..<totalStages {
            guard let enabledByte = read(UInt8.self),
                  let bypassedByte = read(UInt8.self),
                  let kernelOrdinal = read(UInt8.self) else { return nil }
            skip(1) // pad byte
            guard let timingUs = read(UInt32.self),
                  let timingAvgUs = read(UInt32.self) else { return nil }
            let name = readFixedString(32)

            let isVideo = i < Int(numVideo)
            let localIndex = isVideo ? i : i - Int(numVideo)
            let stage = StageInfo(
                id: i,
                name: name.isEmpty ? "Stage \(localIndex)" : name,
                kernelType: KernelType.from(ordinal: kernelOrdinal),
                chainType: isVideo ? .video : .audio,
                enabled: enabledByte != 0,
                bypassed: bypassedByte != 0,
                timingUs: Double(timingUs),
                timingAvgUs: Double(timingAvgUs)
            )
            if isVideo {
                videoStages.append(stage)
            } else {
                audioStages.append(stage)
            }
        }

        return ChainSnapshot(
            frameNumber: frameNumber,
            timestampMs: timestampMs,
            videoStages: videoStages,
            audioStages: audioStages,
            tapWaveform: nil
        )
    }
}
