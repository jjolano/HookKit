// A real Swift class compiled into the device smoke executable. It deliberately
// avoids Foundation so this probe only depends on the Swift runtime shipped on
// the test device.

open class HK3SwiftProbe {
    public init() {}

    @inline(never) open func target() -> Int { 7 }
    @inline(never) open func replacement() -> Int { 42 }
}

@inline(never)
private func callThroughVTable(_ object: HK3SwiftProbe) -> Int {
    object.target()
}

@_cdecl("hk3_swift_probe_metadata")
public func hk3_swift_probe_metadata() -> UnsafeRawPointer {
    unsafeBitCast(HK3SwiftProbe.self, to: UnsafeRawPointer.self)
}

@_cdecl("hk3_swift_probe_call_target")
public func hk3_swift_probe_call_target() -> Int64 {
    Int64(callThroughVTable(HK3SwiftProbe()))
}

// The probe declares exactly two ordinary instance methods, target then
// replacement. Return the latter's live vtable entry so the C smoke swaps
// like-for-like Swift calling conventions instead of using a C ABI shim.
@_cdecl("hk3_swift_probe_replacement")
public func hk3_swift_probe_replacement() -> UnsafeRawPointer? {
    let metadata = unsafeBitCast(HK3SwiftProbe.self, to: UnsafeRawPointer.self)
    guard let descriptor = metadata.load(fromByteOffset: 0x40,
                                         as: UnsafeRawPointer?.self) else {
        return nil
    }
    let vtableOffset = Int(descriptor.load(fromByteOffset: 0x2c,
                                            as: UInt32.self))
    let vtableCount = Int(descriptor.load(fromByteOffset: 0x30,
                                           as: UInt32.self))
    let vtable = metadata.advanced(by: vtableOffset * MemoryLayout<UnsafeRawPointer>.stride)

    var ordinaryInstanceMethods = 0
    for index in 0..<vtableCount {
        let flags = descriptor.load(fromByteOffset: 0x34 + index * 8,
                                    as: UInt32.self)
        if (flags & 0x0f) == 0 && (flags & 0x10) != 0 {
            ordinaryInstanceMethods += 1
            if ordinaryInstanceMethods == 2 {
                return vtable.load(fromByteOffset: index * MemoryLayout<UnsafeRawPointer>.stride,
                                   as: UnsafeRawPointer.self)
            }
        }
    }
    return nil
}
