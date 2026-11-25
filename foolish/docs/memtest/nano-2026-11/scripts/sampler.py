# sampler.py PID OUTFILE [INTERVAL_S]: once a second (or INTERVAL_S), append
# "epoch cpu_s rss_kb vsz_kb" for PID. cpu_s is the process's cumulative user+system
# CPU time from libproc (proc_pidinfo PROC_PIDTASKINFO), converted with mach timebase.
# It runs until PID exits or the file /Users/alex/Dev/foolish-bench-results/scripts/STOP.<PID> exists.
import ctypes, ctypes.util, os, sys, time

pid = int(sys.argv[1]); out = sys.argv[2]
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
libc = ctypes.CDLL(ctypes.util.find_library("System"))

class TimebaseInfo(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]
tb = TimebaseInfo(); libc.mach_timebase_info(ctypes.byref(tb))

class TaskInfo(ctypes.Structure):
    _fields_ = [("virtual_size", ctypes.c_uint64), ("resident_size", ctypes.c_uint64),
                ("total_user", ctypes.c_uint64), ("total_system", ctypes.c_uint64),
                ("threads_user", ctypes.c_uint64), ("threads_system", ctypes.c_uint64),
                ("policy", ctypes.c_int32), ("faults", ctypes.c_int32), ("pageins", ctypes.c_int32),
                ("cow_faults", ctypes.c_int32), ("messages_sent", ctypes.c_int32),
                ("messages_received", ctypes.c_int32), ("syscalls_mach", ctypes.c_int32),
                ("syscalls_unix", ctypes.c_int32), ("csw", ctypes.c_int32), ("threadnum", ctypes.c_int32),
                ("numrunning", ctypes.c_int32), ("priority", ctypes.c_int32)]

libc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
PROC_PIDTASKINFO = 4
stop_file = "/Users/alex/Dev/foolish-bench-results/scripts/STOP.%d" % pid
with open(out, "w") as f:
    f.write("# epoch cpu_s rss_kb vsz_kb\n")
    while True:
        ti = TaskInfo()
        n = libc.proc_pidinfo(pid, PROC_PIDTASKINFO, 0, ctypes.byref(ti), ctypes.sizeof(ti))
        if n != ctypes.sizeof(ti):
            f.write("# pid %d gone at %.3f\n" % (pid, time.time())); break
        ticks = ti.total_user + ti.total_system
        cpu_s = ticks * tb.numer / tb.denom / 1e9
        f.write("%.3f %.6f %d %d\n" % (time.time(), cpu_s, ti.resident_size // 1024, ti.virtual_size // 1024))
        f.flush()
        if os.path.exists(stop_file):
            break
        time.sleep(interval)
