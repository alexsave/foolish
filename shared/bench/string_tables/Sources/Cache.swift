import CTables
/// C tables, each key turned into a Swift String once, on first use.
/// A fixed array indexed by the C key: no hashing, nothing built up front.
enum CachedText {
    static var lang: Int32 = 1
    static var slots = [String?](repeating: nil, count: Int(toy_count()))
    @inline(__always) static func text(_ key: Int32) -> String {
        if let s = slots[Int(key)] { return s }
        let s = String(cString: toy_text(lang, key))
        slots[Int(key)] = s
        return s
    }
}
