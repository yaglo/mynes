import Foundation

enum KernelType: String, CaseIterable, Sendable {
    case pointwise = "pw"
    case rcFilter  = "rc"
    case fir       = "fir"
    case delay     = "dly"
    case comb      = "comb"
    case modulator = "mod"
    case dac       = "dac"
    case matrix    = "mtx"
    case beam      = "bm"

    /// Map from the C enum CHAIN_KERNEL_* ordinal.
    static func from(ordinal: UInt8) -> KernelType {
        switch ordinal {
        case 0: return .pointwise
        case 1: return .rcFilter
        case 2: return .fir
        case 3: return .delay
        case 4: return .comb
        case 5: return .modulator
        case 6: return .dac
        case 7: return .matrix
        case 8: return .beam
        default: return .pointwise
        }
    }
}

enum ChainType: UInt8, Sendable {
    case video = 0
    case audio = 1
}

struct StageInfo: Identifiable, Sendable {
    let id: Int
    let name: String
    let kernelType: KernelType
    let chainType: ChainType
    let enabled: Bool
    let bypassed: Bool
    let timingUs: Double
    let timingAvgUs: Double

    var isActive: Bool { enabled && !bypassed }
}
