import Foundation
import CTables
@inline(never) func now() -> UInt64 { DispatchTime.now().uptimeNanoseconds }
let mode = CommandLine.arguments.dropFirst().first ?? "both"
var sink = 0
// COLD: the first string a fresh process asks for, in Russian.
if mode == "cold-swift" { let t0 = now(); let s = FoolishStringsRu["about"]!; let t1 = now(); sink &+= s.utf8.count; print(t1 - t0); exit(0) }
if mode == "cold-cache" { let t0 = now(); let s = CachedText.text(0); let t1 = now(); sink &+= s.utf8.count; print(t1 - t0); exit(0) }
if mode == "cold-c"     { let t0 = now(); let s = String(cString: toy_text(1, 0)); let t1 = now(); sink &+= s.utf8.count; print(t1 - t0); exit(0) }
// WARM: every key, many times; both ends produce a Swift String, as the UI needs.
let keys = Array(FoolishStringsEn.keys).sorted()
let N = 3_000_000
_ = FoolishStringsRu["about"]; _ = toy_text(1, 0)
var t0 = now()
for i in 0..<N { sink &+= FoolishStringsRu[keys[i % keys.count]]!.utf8.count }
let swiftNs = Double(now() - t0) / Double(N)
let n = Int(toy_count())
t0 = now()
for i in 0..<N { sink &+= String(cString: toy_text(1, Int32(i % n))).utf8.count }
let cNs = Double(now() - t0) / Double(N)
_ = CachedText.text(0)
t0 = now()
for i in 0..<N { sink &+= CachedText.text(Int32(i % n)).utf8.count }
let cacheNs = Double(now() - t0) / Double(N)
print(String(format: "warm lookup  swift dict %.1f ns   c array %.1f ns   c + cache %.1f ns  (%d keys)", swiftNs, cNs, cacheNs, n), sink & 0)
