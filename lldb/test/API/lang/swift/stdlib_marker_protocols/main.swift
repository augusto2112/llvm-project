// Copyable and Escapable are not writable as `any Copyable` / `any Escapable`
// at the source level: they round-trip through debug-info reconstruction to
// `Any`, which trips an IRGenDebugInfo assertion
// (IRGenDebugInfo.cpp: "Incorrect reconstructed type for $sypD"). Only
// Sendable and BitwiseCopyable are testable as source-level existentials.

func f() {
    let sendable: any Sendable = 1
    let bitwiseCopyable: any BitwiseCopyable = 2
    let sendableAndBitwise: any Sendable & BitwiseCopyable = 3
    let realAndSendable: any Hashable & Sendable = 4

    print("break here")
    print(sendable, bitwiseCopyable, sendableAndBitwise, realAndSendable)
}

f()
