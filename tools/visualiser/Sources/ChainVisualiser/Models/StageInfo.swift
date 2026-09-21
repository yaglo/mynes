import Foundation

enum KernelType: String, CaseIterable, Sendable {
    case pointwise = "Transfer", rcFilter = "RC filter", fir = "FIR filter", delay = "Delay"
    case comb = "Y/C comb", modulator = "Demodulator", dac = "DAC", matrix = "Color matrix"
    case pal = "PAL correction", deflection = "Deflection", beam = "Electron beam", rf = "RF"
    case videoAmp = "Video amplifier", blur = "Beam width", temporal = "Persistence", agc = "AGC"
    case raster = "PPU raster", receiver = "Burst lock", receiverDemod = "Chroma detector"
    case separatedYC = "Separated Y/C"
    case receiverPLL = "Receiver PLL", crtLoad = "CRT supply", gunCurrent = "Gun current"
    case unknown = "Unknown"

    static func from(ordinal: UInt8) -> KernelType {
        let types: [KernelType] = [.pointwise, .rcFilter, .fir, .delay, .comb, .modulator, .dac,
                                  .matrix, .pal, .deflection, .beam, .rf, .videoAmp, .blur, .temporal, .agc, .raster, .receiver, .receiverDemod, .separatedYC,
                                  .receiverPLL, .crtLoad, .gunCurrent]
        return Int(ordinal) < types.count ? types[Int(ordinal)] : .unknown
    }
    var group: String {
        switch self {
        case .separatedYC, .raster, .rcFilter, .delay, .rf, .agc: return "Connection"
        case .receiver, .receiverPLL, .receiverDemod, .pointwise, .fir, .comb, .modulator, .dac, .matrix, .pal: return "Decoder"
        case .deflection, .beam, .videoAmp, .blur, .crtLoad, .gunCurrent: return "Beam"
        case .temporal: return "Phosphor"
        case .unknown: return "Decoder"
        }
    }
}

enum ChainType: UInt8, Sendable { case video = 0, audio = 1 }
struct StageInfo: Identifiable, Sendable, Equatable {
    let id: Int
    let name: String
    let kernelType: KernelType
    let chainType: ChainType
    let enabled: Bool
    let bypassed: Bool
    let timingUs: Double
    let timingAvgUs: Double
    var withoutTiming: StageInfo {
        .init(id: id, name: name, kernelType: kernelType, chainType: chainType,
              enabled: enabled, bypassed: bypassed, timingUs: 0, timingAvgUs: 0)
    }
    var isActive: Bool { enabled && !bypassed }
}
