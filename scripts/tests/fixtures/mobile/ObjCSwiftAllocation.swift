private var destroyed: UInt64 = 0

private final class AllocationSeed {
    deinit { destroyed += 1 }
}

@_cdecl("NDMakeAllocationSeed")
public func makeAllocationSeed() -> UnsafeMutableRawPointer {
    Unmanaged.passRetained(AllocationSeed()).toOpaque()
}

@_cdecl("NDAllocationDestroyed")
public func allocationDestroyed() -> UInt64 {
    destroyed
}
