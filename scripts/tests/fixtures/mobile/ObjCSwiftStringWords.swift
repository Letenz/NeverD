import Foundation
// Swift owns these strings while both original and recovered methods run.
// Read their actual runtime words instead of constructing a guessed layout.
private let samples: [String] = [
  "", "A", "hello", "é", "你好", "🙂", "a\0b",
  String(repeating: "long bridge content🙂", count: 64),
  String(NSString(string: "NSString-backed value that exceeds small string storage"))
]

@_cdecl("nd_swift_string_case_count")
public func caseCount() -> Int32 { Int32(samples.count) }
@_cdecl("nd_swift_string_words")
public func words(_ index: Int32, _ output: UnsafeMutablePointer<UInt64>) {
  precondition(MemoryLayout<String>.size == 16)
  let value = samples[Int(index)]
  withUnsafeBytes(of: value) { bytes in
    output[0] = bytes.load(fromByteOffset: 0, as: UInt64.self)
    output[1] = bytes.load(fromByteOffset: 8, as: UInt64.self)
  }
}
@_cdecl("nd_swift_string_expected")
public func expected(_ index: Int32) -> UnsafeMutableRawPointer {
  Unmanaged.passRetained(samples[Int(index)] as NSString).toOpaque()
}
