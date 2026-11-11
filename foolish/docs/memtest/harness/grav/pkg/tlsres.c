/* tlsres PID: each thread's TLS block [tp, tp + 16 + PT_TLS memsz) (musl aarch64: TLS above the thread
 * pointer, a 16-byte gap) and how many of its 4 KiB pages are resident (/proc/PID/pagemap bit 63).
 * The thread pointer is read with ptrace (PTRACE_SEIZE + INTERRUPT, NT_ARM_TLS), so run as root.
 * Prints: tid comm tp tls_bytes pages resident_pages resident_kB */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef NT_ARM_TLS
#define NT_ARM_TLS 0x401
#endif
static uint64_t tls_memsz(int pid) {
    char p[64]; snprintf(p, sizeof p, "/proc/%d/exe", pid);
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
    Elf64_Ehdr eh; if (fread(&eh, sizeof eh, 1, f) != 1) exit(1);
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph; fseek(f, (long)(eh.e_phoff + (uint64_t)i * eh.e_phentsize), SEEK_SET);
        if (fread(&ph, sizeof ph, 1, f) != 1) exit(1);
        if (ph.p_type == PT_TLS) { fclose(f); return ph.p_memsz; }
    }
    fclose(f); return 0;
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: tlsres PID\n"); return 2; }
    int pid = atoi(argv[1]);
    uint64_t msz = tls_memsz(pid), span = 16 + msz;
    printf("# pid %d PT_TLS memsz %llu\n", pid, (unsigned long long)msz);
    char p[64]; snprintf(p, sizeof p, "/proc/%d/pagemap", pid);
    int pm = open(p, O_RDONLY); if (pm < 0) { perror(p); return 1; }
    snprintf(p, sizeof p, "/proc/%d/task", pid);
    DIR *d = opendir(p); if (!d) { perror(p); return 1; }
    struct dirent *e; uint64_t total = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int tid = atoi(e->d_name);
        char cp[96], comm[32] = "?"; snprintf(cp, sizeof cp, "/proc/%d/task/%d/comm", pid, tid);
        FILE *cf = fopen(cp, "r"); if (cf) { if (fgets(comm, sizeof comm, cf)) comm[strcspn(comm, "\n")] = 0; fclose(cf); }
        if (ptrace(PTRACE_SEIZE, tid, 0, 0) != 0) { printf("%d %s seize failed %s\n", tid, comm, strerror(errno)); continue; }
        ptrace(PTRACE_INTERRUPT, tid, 0, 0);
        int st; waitpid(tid, &st, __WALL);
        uint64_t tp = 0; struct iovec iv = { &tp, sizeof tp };
        long r = ptrace(PTRACE_GETREGSET, tid, (void *)NT_ARM_TLS, &iv);
        ptrace(PTRACE_DETACH, tid, 0, 0);
        if (r != 0 || !tp) { printf("%d %s no tp\n", tid, comm); continue; }
        uint64_t first = tp / 4096, last = (tp + span - 1) / 4096, n = last - first + 1, res = 0;
        uint64_t *v = malloc(n * 8);
        if (pread(pm, v, n * 8, (off_t)(first * 8)) == (ssize_t)(n * 8))
            for (uint64_t i = 0; i < n; i++) res += v[i] >> 63;
        free(v);
        total += res;
        printf("%d %s 0x%llx %llu %llu %llu %llu\n", tid, comm, (unsigned long long)tp, (unsigned long long)span,
               (unsigned long long)n, (unsigned long long)res, (unsigned long long)res * 4);
    }
    printf("# total resident TLS %llu kB\n", (unsigned long long)total * 4);
    return 0;
}
