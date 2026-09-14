import Foundation

extension NDSwiftLiteralStrings {
    @objc public func asciiLiteral() -> String {
        "A compiler-owned immutable string with more than fifteen bytes"
    }

    @objc public func sameAsciiLiteral() -> String {
        "A compiler-owned immutable string with more than fifteen bytes"
    }

    @objc public func unicodeLiteral() -> String {
        "不可变字符串🙂 café e\u{301}"
    }
}
