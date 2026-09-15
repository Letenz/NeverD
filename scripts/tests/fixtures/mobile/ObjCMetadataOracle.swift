import Foundation

@_cdecl("nd_metadata_oracle")
public func metadataOracle(_ index: UInt32) -> UnsafeRawPointer {
  switch index {
  case 0: return unsafeBitCast(Foundation.URL.self, to: UnsafeRawPointer.self)
  case 1: return unsafeBitCast(Foundation.Date.self, to: UnsafeRawPointer.self)
  case 2: return unsafeBitCast(Foundation.CharacterSet.self, to: UnsafeRawPointer.self)
  case 3: return unsafeBitCast(Foundation.DateComponents.self, to: UnsafeRawPointer.self)
  case 4: return unsafeBitCast(Foundation.URLRequest.self, to: UnsafeRawPointer.self)
  case 5: return unsafeBitCast(Foundation.IndexPath.self, to: UnsafeRawPointer.self)
  case 6: return unsafeBitCast(Foundation.Locale.self, to: UnsafeRawPointer.self)
  case 7: return unsafeBitCast(Foundation.Notification.self, to: UnsafeRawPointer.self)
  default: fatalError("invalid fixture type")
  }
}
