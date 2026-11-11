// gmrender: offline General MIDI renderer over Apple's gs_instruments.dls.
// usage: gmrender events.txt outdir
// events.txt lines (text, whitespace separated, times in seconds):
//   G <track> <program> <bankMSB> <bankLSB> <seconds_total>     one sampler per track
//   N <track> <t> <dur> <midi_note> <velocity>
//   C <track> <t> <cc> <value>
//   P <track> <t> <bend 0..16383>
// Each track is rendered alone to outdir/<track>.f32 (stereo interleaved float32, 48 kHz),
// so the C mixer owns every gain, pan and effect.
import AVFoundation
import Foundation

let sr = 48000.0
let args = CommandLine.arguments
guard args.count >= 3 else { print("usage: gmrender events.txt outdir"); exit(2) }
let text = try! String(contentsOfFile: args[1], encoding: .utf8)
let outdir = args[2]
let dls = URL(fileURLWithPath: "/System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls")

struct Track { var program: UInt8; var msb: UInt8; var lsb: UInt8; var secs: Double }
struct Ev { var frame: Int; var bytes: [UInt8]; var order: Int }
var tracks: [String: Track] = [:]
var evs: [String: [Ev]] = [:]
var order = 0
for line in text.split(separator: "\n") {
    let f = line.split(separator: " ").map(String.init)
    if f.isEmpty { continue }
    order += 1
    switch f[0] {
    case "G":
        tracks[f[1]] = Track(program: UInt8(f[2])!, msb: UInt8(f[3])!, lsb: UInt8(f[4])!, secs: Double(f[5])!)
        evs[f[1]] = evs[f[1]] ?? []
    case "N":
        let t = Double(f[2])!, d = Double(f[3])!, n = UInt8(f[4])!, v = UInt8(max(1, min(127, Int(f[5])!)))
        let on = Int((t * sr).rounded()), off = max(on + 1, Int(((t + d) * sr).rounded()))
        evs[f[1], default: []].append(Ev(frame: on, bytes: [0x90, n, v], order: order))
        // note-offs sort before note-ons at the same frame (a repeated note retriggers cleanly)
        evs[f[1], default: []].append(Ev(frame: off, bytes: [0x80, n, 0], order: -order))
    case "C":
        let t = Double(f[2])!
        evs[f[1], default: []].append(Ev(frame: Int((t * sr).rounded()), bytes: [0xB0, UInt8(f[3])!, UInt8(f[4])!], order: order))
    case "P":
        let t = Double(f[2])!, b = max(0, min(16383, Int(f[3])!))
        evs[f[1], default: []].append(Ev(frame: Int((t * sr).rounded()), bytes: [0xE0, UInt8(b & 127), UInt8(b >> 7)], order: order))
    default: break
    }
}

let fmt = AVAudioFormat(standardFormatWithSampleRate: sr, channels: 2)!
let chunk: AVAudioFrameCount = 128
for (name, tr) in tracks.sorted(by: { $0.key < $1.key }) {
    let engine = AVAudioEngine()
    let sampler = AVAudioUnitSampler()
    engine.attach(sampler)
    engine.connect(sampler, to: engine.mainMixerNode, format: fmt)
    try! engine.enableManualRenderingMode(.offline, format: fmt, maximumFrameCount: chunk)
    try! sampler.loadSoundBankInstrument(at: dls, program: tr.program, bankMSB: tr.msb, bankLSB: tr.lsb)
    try! engine.start()
    let sched = sampler.auAudioUnit.scheduleMIDIEventBlock!
    var list = evs[name]!.sorted { $0.frame != $1.frame ? $0.frame < $1.frame : $0.order < $1.order }
    let total = Int(tr.secs * sr)
    let buf = AVAudioPCMBuffer(pcmFormat: engine.manualRenderingFormat, frameCapacity: chunk)!
    var out = [Float](repeating: 0, count: total * 2)
    var pos = 0, ei = 0
    list.reserveCapacity(0)
    while pos < total {
        let n = min(Int(chunk), total - pos)
        while ei < list.count && list[ei].frame < pos + n {
            let e = list[ei]
            let off = max(0, e.frame - pos)
            e.bytes.withUnsafeBufferPointer { p in sched(AUEventSampleTimeImmediate + AUEventSampleTime(off), 0, e.bytes.count, p.baseAddress!) }
            ei += 1
        }
        let st = try! engine.renderOffline(AVAudioFrameCount(n), to: buf)
        if st != .success { print("render status \(st.rawValue) for \(name)"); exit(1) }
        let l = buf.floatChannelData![0], r = buf.floatChannelData![1]
        for i in 0..<Int(buf.frameLength) { out[(pos + i) * 2] = l[i]; out[(pos + i) * 2 + 1] = r[i] }
        pos += Int(buf.frameLength)
    }
    engine.stop()
    let url = URL(fileURLWithPath: outdir + "/" + name + ".f32")
    out.withUnsafeBufferPointer { p in try! Data(buffer: p).write(to: url) }
    print("rendered \(name) prog \(tr.program) msb \(tr.msb) \(String(format: "%.1f", tr.secs)) s")
}
