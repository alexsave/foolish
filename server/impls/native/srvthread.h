// srvthread.h - what every thread this server starts does first, and the
// per-thread state it gets for free.
//
// Two things, both per-thread and both settled once at a thread's entry: the
// cancellation bracket this server refuses to pay (thread_disable_cancellation,
// and the INVARIANT that comes with it), and a random source no two threads
// share (next_rand). Every thread entry in this server - the acceptors, the
// typed HTTP workers, the epoll shards, each game's bot trampoline, the
// reaper, the QUIC listener, and main() itself - opens with the first.
#ifndef FOOLISH_SRVTHREAD_H
#define FOOLISH_SRVTHREAD_H

// glibc (2.39 on this box) brackets every cancellable syscall wrapper this
// server uses in its hot loops - read / write / writev / epoll_wait / accept -
// with an __pthread_enable_asynccancel / __pthread_disable_asynccancel pair, an
// atomic CAS on the thread's cancelhandling word before AND after each call.
// This server NEVER calls pthread_cancel (grep the tree: zero hits - threads
// exit on their own when a game ends / the process stops), so that bracket is
// pure overhead - it was ~4% of instructions on the epoll build and ~11% on the
// thread-per-connection (--tls) build (PROFILE_HOTPATH.md). Setting the cancel
// TYPE to asynchronous makes __pthread_enable_asynccancel find the type bit
// already set and take its no-CAS early break (and __pthread_disable_asynccancel
// early-return); DISABLE-ing the state as well means that even if a
// pthread_cancel were ever introduced it could not asynchronously tear a thread
// down mid-syscall - with nothing cancelling, the async type is inert. Setting
// both collapses the bracket regardless of which mechanism this libc gates it
// on. Call once at each thread's entry (the setting is per-thread).
// INVARIANT: do not introduce pthread_cancel without revisiting this.
void thread_disable_cancellation(void);

// POSIX doesn't guarantee libc rand() is thread-safe (glibc's implementation
// shares unlocked global state across callers), and every worker-pool size
// in this server (game/meta/create) is runtime-configurable - see httpd.c's
// "Work-queue thread routing" - so no call site gets to assume it's the only
// thread reaching it. next_rand() gives every thread its own rand_r state
// instead: seeded once per thread from the clock + thread id, no lock needed
// because nothing is shared.
unsigned int next_rand(void);

#endif
