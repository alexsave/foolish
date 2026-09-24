import Foundation
import os

/// The one log, under the subsystem the rig streams (`cards.uttt`).
///
/// NOTHING THAT GOES WRONG IN THE CONVERSATION IS ALLOWED TO BE SILENT. An
/// insert Messages refused used to vanish into a `{ _ in }`, and a join that
/// never reached the input field looked exactly like one that did. Every
/// lifecycle callback, every stage and every insert result is written here,
/// with the time since the process started, so a slow first paint or a lost
/// bubble can be read off `log stream` instead of guessed at.
///
/// `.public` on purpose: nothing logged here is personal - a style, a seat
/// verdict, a byte count, an error string from Messages.
public enum UtttLog {
    private static let log = Logger(subsystem: "cards.uttt", category: "msg")

    /// Seconds since this process was launched, from the kernel's own clock
    /// for the process rather than a static set on first use (which would be
    /// "since the first log line" and hide exactly the launch cost).
    public static var uptime: Double {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&mib, 4, &info, &size, nil, 0) == 0 else { return -1 }
        let st = info.kp_proc.p_un.__p_starttime
        let start = Double(st.tv_sec) + Double(st.tv_usec) / 1e6
        return Date().timeIntervalSince1970 - start
    }

    public static func note(_ event: String, _ detail: String = "") {
        let t = String(format: "%.3f", uptime)
        log.notice("uttt +\(t, privacy: .public)s \(event, privacy: .public) \(detail, privacy: .public)")
    }

#if DEBUG
    /// DEBUG: this process's physical footprint and its peak so far, in MB,
    /// logged under `mem` - the stage's memory read step by step.
    public static func mem(_ tag: String) {
        var info = task_vm_info_data_t()
        var n = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(n)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &n)
            }
        }
        guard kr == KERN_SUCCESS else { return }
        note("mem", String(format: "%@ now %.1f peak %.1f", tag,
                           Double(info.phys_footprint) / 1_048_576,
                           Double(info.ledger_phys_footprint_peak) / 1_048_576))
    }
#endif

    public static func fault(_ event: String, _ detail: String) {
        let t = String(format: "%.3f", uptime)
        log.error("uttt +\(t, privacy: .public)s \(event, privacy: .public) \(detail, privacy: .public)")
    }
}
