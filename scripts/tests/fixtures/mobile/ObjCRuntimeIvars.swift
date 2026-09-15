import Foundation
@objc(NDRuntimeIvars)
public final class RuntimeIvars: NSObject {
  private var url: URL?
  @objc public var word: UInt64
  public init(seed: UInt64) {
    url = URL(fileURLWithPath: "/neverd/runtime-ivar-probe")
    word = seed
    super.init()
  }
  fileprivate func matches(_ expected: UInt64) -> Bool {
    word == expected && url?.path == "/neverd/runtime-ivar-probe"
  }
}
@_cdecl("nd_runtime_ivars_create")
public func createIvars(_ seed: UInt64) -> UnsafeMutableRawPointer {
  Unmanaged.passRetained(RuntimeIvars(seed: seed)).toOpaque()
}
@_cdecl("nd_runtime_ivars_check")
public func checkIvars(_ pointer: UnsafeMutableRawPointer, _ expected: UInt64) -> Int32 {
  Unmanaged<RuntimeIvars>.fromOpaque(pointer).takeUnretainedValue().matches(expected) ? 1 : 0
}
