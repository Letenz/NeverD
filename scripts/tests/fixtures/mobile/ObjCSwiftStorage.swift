import Foundation

@objc(NDSwiftStorage)
public class NDSwiftStorage: NSObject {
    private let opaque: String
    @objc public let value: Double
    @objc public let count: Int64

    @objc public init(value: Double, count: Int64) {
        opaque = "storage with no Objective-C type encoding"
        self.value = value
        self.count = count
        super.init()
    }
}
