//! The C ABI of the Genode shim (relibc-genode/src/lib/relibc_genode/gxd.h).
//! Results are non-negative on success and -errno on failure. Flags,
//! constants and structures are relibc's own (Linux) ones, except
//! [`GxdStat`].

use crate::platform::types::*;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct GxdStat {
    pub mode: u32,
    pub ino: u64,
    pub size: i64,
    pub blksize: i64,
    pub mtime: i64,
}

unsafe extern "C" {
    /* file descriptors */
    pub fn gxd_openat(dirfd: c_int, path: *const c_char, oflag: c_int, mode: c_uint) -> isize;
    pub fn gxd_close(fd: c_int) -> isize;
    pub fn gxd_read(fd: c_int, buf: *mut c_void, len: usize) -> isize;
    pub fn gxd_write(fd: c_int, buf: *const c_void, len: usize) -> isize;
    /// At an offset, the file position unchanged.
    pub fn gxd_pread(fd: c_int, buf: *mut c_void, len: usize, off: i64) -> isize;
    pub fn gxd_pwrite(fd: c_int, buf: *const c_void, len: usize, off: i64) -> isize;
    pub fn gxd_lseek(fd: c_int, off: i64, whence: c_int) -> isize;
    pub fn gxd_fstatat(dirfd: c_int, path: *const c_char, st: *mut GxdStat, flags: c_int) -> isize;
    pub fn gxd_ftruncate(fd: c_int, len: i64) -> isize;
    pub fn gxd_fsync(fd: c_int) -> isize;
    /// Linux `dirent64` records, `d_off` the seek position of the next one.
    pub fn gxd_getdents(fd: c_int, buf: *mut c_void, len: usize) -> isize;
    pub fn gxd_dup(fd: c_int, min_fd: c_int, cloexec: c_int) -> isize;
    pub fn gxd_dup2(fd: c_int, fd2: c_int) -> isize;
    pub fn gxd_pipe(fds: *mut c_int) -> isize;
    pub fn gxd_fcntl_getfl(fd: c_int) -> isize;
    pub fn gxd_fcntl_setfl(fd: c_int, flags: c_int) -> isize;
    /// `FD_CLOEXEC`, the fd's only flag
    pub fn gxd_fcntl_getfd(fd: c_int) -> isize;
    pub fn gxd_fcntl_setfd(fd: c_int, flags: c_int) -> isize;
    pub fn gxd_ioctl(fd: c_int, request: c_ulong, arg: *mut c_void) -> isize;

    /* paths */
    pub fn gxd_unlinkat(dirfd: c_int, path: *const c_char, flags: c_int) -> isize;
    pub fn gxd_mkdirat(dirfd: c_int, path: *const c_char, mode: c_uint) -> isize;
    pub fn gxd_renameat(olddirfd: c_int, oldpath: *const c_char, newdirfd: c_int, newpath: *const c_char)
        -> isize;
    pub fn gxd_symlinkat(target: *const c_char, dirfd: c_int, path: *const c_char) -> isize;
    pub fn gxd_readlinkat(dirfd: c_int, path: *const c_char, buf: *mut c_char, len: usize) -> isize;
    pub fn gxd_faccessat(dirfd: c_int, path: *const c_char, mode: c_int) -> isize;
    pub fn gxd_chdir(path: *const c_char) -> isize;
    pub fn gxd_fchdir(fd: c_int) -> isize;
    pub fn gxd_getcwd(buf: *mut c_char, len: usize) -> isize;

    /* epoll */
    pub fn gxd_epoll_create() -> isize;
    pub fn gxd_epoll_ctl(epfd: c_int, op: c_int, fd: c_int, events: u32, data: u64) -> isize;
    /// Ready events and their data, in two arrays of `max` entries.
    pub fn gxd_epoll_wait(epfd: c_int, events: *mut u32, data: *mut u64, max: c_int, timeout_ms: c_int)
        -> isize;

    /* sockets */
    pub fn gxd_socket(domain: c_int, kind: c_int, protocol: c_int) -> isize;
    pub fn gxd_bind(fd: c_int, addr: *const c_void, len: u32) -> isize;
    pub fn gxd_listen(fd: c_int, backlog: c_int) -> isize;
    pub fn gxd_accept(fd: c_int, addr: *mut c_void, len: *mut u32, flags: c_int) -> isize;
    pub fn gxd_connect(fd: c_int, addr: *const c_void, len: u32) -> isize;
    pub fn gxd_sendto(fd: c_int, buf: *const c_void, len: usize, flags: c_int, dest: *const c_void,
                      destlen: u32) -> isize;
    pub fn gxd_recvfrom(fd: c_int, buf: *mut c_void, len: usize, flags: c_int, addr: *mut c_void,
                        addrlen: *mut u32) -> isize;
    pub fn gxd_getsockname(fd: c_int, addr: *mut c_void, len: *mut u32) -> isize;
    pub fn gxd_getpeername(fd: c_int, addr: *mut c_void, len: *mut u32) -> isize;
    pub fn gxd_getsockopt(fd: c_int, level: c_int, name: c_int, val: *mut c_void, len: *mut u32) -> isize;
    pub fn gxd_setsockopt(fd: c_int, level: c_int, name: c_int, val: *const c_void, len: u32) -> isize;
    pub fn gxd_shutdown(fd: c_int, how: c_int) -> isize;
    /// AF_UNIX stream sockets within the component
    pub fn gxd_socketpair(domain: c_int, kind: c_int, protocol: c_int, fds: *mut c_int) -> isize;

    /* memory */
    /// Anonymous memory: the address, or -errno.
    pub fn gxd_mmap_anon(len: usize) -> isize;
    pub fn gxd_munmap(addr: *mut c_void, len: usize) -> isize;

    /* time and randomness */
    /// Nanoseconds of the clock (Linux clock ids), or -errno.
    pub fn gxd_clock_ns(clock: c_int) -> i64;
    pub fn gxd_sleep_ns(ns: u64) -> isize;
    pub fn gxd_getrandom(buf: *mut c_void, len: usize) -> isize;

    /* threads */
    /// relibc's thread pointer (its TCB), kept per Genode thread: generic-rt
    /// and ld_so use it in place of the fs register.
    pub fn gxd_set_tp(tp: usize) -> isize;
    pub fn gxd_get_tp() -> usize;
    pub fn gxd_gettid() -> c_int;
    /// A thread started like Linux's clone: `stack` holds the entry point
    /// and its six arguments. Returns the thread's id.
    pub fn gxd_default_stack_size() -> usize;
    pub fn gxd_thread_create(stack: *mut usize) -> isize;
    pub fn gxd_thread_exit() -> !;
    /// Stop a thread created by gxd_thread_create (pthread_cancel). Its
    /// thread pointer, or -ESRCH if it exited.
    pub fn gxd_thread_cancel(tid: c_int) -> isize;
    /// Wait while *addr == val, until the absolute monotonic deadline (ns),
    /// or without one if it is negative.
    pub fn gxd_futex_wait(addr: *mut u32, val: u32, deadline_ns: i64) -> isize;
    pub fn gxd_futex_wake(addr: *mut u32, num: u32) -> isize;

    /* signals within the component, and POSIX timers */
    pub fn gxd_signal_raise(signo: c_int);
    /// The lowest pending signal of `allowed` (bit signo - 1), taken; 0 if none.
    pub fn gxd_signal_take(allowed: u64) -> c_int;
    pub fn gxd_signals_pending() -> u64;
    /// The thread's mask, for which signals end its blocking waits (EINTR).
    pub fn gxd_set_sigmask(mask: u64);
    /// Until a signal of `allowed` is pending, or the monotonic deadline (ns, -1: none).
    pub fn gxd_signal_wait(allowed: u64, deadline_ns: i64) -> isize;
    /// A timer raising `signo` (0: none); its id (1..).
    pub fn gxd_timer_create(signo: c_int, clock: c_int) -> isize;
    pub fn gxd_timer_settime(id: c_long, value_ns: i64, interval_ns: i64, absolute: c_int,
                             old_value_ns: *mut i64, old_interval_ns: *mut i64) -> isize;
    pub fn gxd_timer_gettime(id: c_long, value_ns: *mut i64, interval_ns: *mut i64) -> isize;
    pub fn gxd_timer_delete(id: c_long) -> isize;

    /* the component */
    pub fn gxd_exit(status: c_int) -> !;
    /// Processes: fork (as Genode's libc, a child component of the same
    /// binary), the end of a child (wait4's options), the ids
    pub fn gxd_fork() -> isize;
    pub fn gxd_waitpid(pid: pid_t, status: *mut c_int, options: c_int) -> isize;
    /// posix_spawn: a child of the program's ROM module with the fds of
    /// the map (child fd, parent fd) and a working directory (null: the
    /// parent's; relative to `cwd_fd`, a directory fd or AT_FDCWD)
    pub fn gxd_spawn(path: *const c_char, argv: *const *mut c_char, envp: *const *mut c_char,
                     fds: *const [c_int; 2], num_fds: c_uint, cwd: *const c_char, cwd_fd: c_int)
        -> isize;
    pub fn gxd_getpid() -> c_int;
    pub fn gxd_getppid() -> c_int;
    /// The signals with a handler: SIGCHLD only for those, or blocked ones
    pub fn gxd_set_caught_signals(mask: u64);
    /// execve: load the program of `path` in place of the running one (0
    /// or -errno, the running one unchanged), then start it on the calling
    /// thread
    pub fn gxd_exec_load(path: *const c_char) -> isize;
    pub fn gxd_exec_run(argv: *const *mut c_char, envp: *const *mut c_char) -> !;
}
