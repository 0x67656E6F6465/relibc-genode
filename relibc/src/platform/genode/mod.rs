//! Genode platform: relibc's Linux ABI (types, constants, headers) with
//! the system calls replaced by the Genode shim of relibc-genode
//! (src/lib/relibc_genode, the `gxd_*` functions of [`gxd`]), which
//! implements them with Genode's VFS, LOG, Timer and threads.
//!
//! Laid out as the Linux platform: this file the [`Pal`], the others its
//! siblings. What Genode has no counterpart for returns `ENOSYS`.

use core::{mem::offset_of, num::NonZeroU64};

use generic_rt::GenericTcb;

use super::{Pal, types::*};
use crate::{
    c_str::CStr,
    error::{Errno, Result},
    header::{
        dirent::dirent,
        errno::*,
        fcntl::{
            F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_GETFL, F_GETLK, F_SETFD, F_SETFL, F_SETLK, F_SETLKW,
            F_UNLCK, FD_CLOEXEC, O_CLOEXEC, O_NONBLOCK, flock,
        },
        pthread::PTHREAD_CANCELED,
        signal::{SIGEV_NONE, SIGEV_SIGNAL, sigevent},
        sys_mman::{MAP_ANONYMOUS, MAP_FIXED, MAP_PRIVATE, MAP_SHARED, PROT_WRITE},
        sys_resource::{rlimit, rusage},
        sys_select::timeval,
        sys_stat::stat,
        sys_statvfs::statvfs,
        sys_time::timezone,
        sys_utsname::utsname,
        time::{CLOCK_MONOTONIC, CLOCK_REALTIME, TIMER_ABSTIME, itimerspec, timespec},
        unistd::{SEEK_CUR, SEEK_SET},
    },
    ld_so::tcb::{OsSpecific, Tcb},
    out::Out,
    pthread::{OsTid, Retval, SIGRT_RLCT_CANCEL},
};

mod epoll;
mod ptrace;
mod signal;
mod socket;
mod spawn;

pub mod gxd;

/// A shim result: non-negative on success, -errno on failure.
fn e(ret: isize) -> Result<usize> {
    if ret < 0 { Err(Errno(-ret as c_int)) } else { Ok(ret as usize) }
}

fn ns_timespec(ns: i64) -> timespec {
    timespec { tv_sec: (ns / 1_000_000_000) as time_t, tv_nsec: (ns % 1_000_000_000) as c_long }
}

/// The stack size of a thread that asks for none: Genode's libc's
/// (Libc::Component::stack_size), as Genode backs a stack with RAM in full
pub fn default_stack_size() -> usize {
    unsafe { gxd::gxd_default_stack_size() }
}

/// Genode implementation of [`Pal`].
pub struct Sys;

impl Sys {
    pub unsafe fn ioctl(fd: c_int, request: c_ulong, out: *mut c_void) -> Result<c_int> {
        e(unsafe { gxd::gxd_ioctl(fd, request, out) }).map(|r| r as c_int)
    }
}

impl Pal for Sys {
    /* files and directories of the component's VFS */

    fn openat(dirfd: c_int, path: CStr, oflag: c_int, mode: mode_t) -> Result<c_int> {
        e(unsafe { gxd::gxd_openat(dirfd, path.as_ptr(), oflag, mode as c_uint) }).map(|fd| fd as c_int)
    }

    fn close(fildes: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_close(fildes) }).map(|_| ())
    }

    fn read(fildes: c_int, buf: &mut [u8]) -> Result<usize> {
        e(unsafe { gxd::gxd_read(fildes, buf.as_mut_ptr().cast(), buf.len()) })
    }

    fn write(fildes: c_int, buf: &[u8]) -> Result<usize> {
        e(unsafe { gxd::gxd_write(fildes, buf.as_ptr().cast(), buf.len()) })
    }

    fn pread(fildes: c_int, buf: &mut [u8], off: off_t) -> Result<usize> {
        e(unsafe { gxd::gxd_pread(fildes, buf.as_mut_ptr().cast(), buf.len(), off as i64) })
    }

    fn pwrite(fildes: c_int, buf: &[u8], off: off_t) -> Result<usize> {
        e(unsafe { gxd::gxd_pwrite(fildes, buf.as_ptr().cast(), buf.len(), off as i64) })
    }

    fn lseek(fildes: c_int, offset: off_t, whence: c_int) -> Result<off_t> {
        e(unsafe { gxd::gxd_lseek(fildes, offset as i64, whence) }).map(|o| o as off_t)
    }

    fn ftruncate(fildes: c_int, length: off_t) -> Result<()> {
        e(unsafe { gxd::gxd_ftruncate(fildes, length as i64) }).map(|_| ())
    }

    fn fsync(fildes: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_fsync(fildes) }).map(|_| ())
    }

    fn fdatasync(fildes: c_int) -> Result<()> {
        Self::fsync(fildes)
    }

    fn fstatat(fildes: c_int, path: Option<CStr>, mut buf: Out<stat>, flags: c_int) -> Result<()> {
        let mut s = gxd::GxdStat::default();
        let path = path.map_or(c"".as_ptr(), |p| p.as_ptr());
        e(unsafe { gxd::gxd_fstatat(fildes, path, &mut s, flags) })?;
        let mtime = timespec { tv_sec: s.mtime as time_t, tv_nsec: 0 };
        let mut st: stat = unsafe { core::mem::zeroed() };
        st.st_mode = s.mode as mode_t;
        st.st_ino = s.ino as ino_t;
        st.st_nlink = 1;
        st.st_size = s.size as off_t;
        st.st_blksize = s.blksize as blksize_t;
        st.st_blocks = ((s.size + 511) / 512) as blkcnt_t;
        st.st_mtim = mtime.clone();
        st.st_atim = mtime.clone();
        st.st_ctim = mtime;
        buf.write(st);
        Ok(())
    }

    fn fcntl(fildes: c_int, cmd: c_int, arg: c_ulonglong) -> Result<c_int> {
        match cmd {
            F_DUPFD | F_DUPFD_CLOEXEC => {
                e(unsafe { gxd::gxd_dup(fildes, arg as c_int, (cmd == F_DUPFD_CLOEXEC) as c_int) })
                    .map(|f| f as c_int)
            }
            F_GETFL => e(unsafe { gxd::gxd_fcntl_getfl(fildes) }).map(|f| f as c_int),
            F_SETFL => e(unsafe { gxd::gxd_fcntl_setfl(fildes, arg as c_int) }).map(|_| 0),
            F_GETFD => e(unsafe { gxd::gxd_fcntl_getfd(fildes) }).map(|f| f as c_int),
            F_SETFD => e(unsafe { gxd::gxd_fcntl_setfd(fildes, arg as c_int) }).map(|_| 0),
            /* one component, nobody to conflict with: every lock is granted, as with Genode's libc */
            F_GETLK => {
                e(unsafe { gxd::gxd_fcntl_getfl(fildes) })?;
                unsafe { (*(arg as *mut flock)).l_type = F_UNLCK as _ };
                Ok(0)
            }
            F_SETLK | F_SETLKW => e(unsafe { gxd::gxd_fcntl_getfl(fildes) }).map(|_| 0),
            _ => Err(Errno(EINVAL)),
        }
    }

    fn dup2(fildes: c_int, fildes2: c_int) -> Result<c_int> {
        e(unsafe { gxd::gxd_dup2(fildes, fildes2) }).map(|f| f as c_int)
    }

    fn pipe2(mut fildes: Out<[c_int; 2]>, flags: c_int) -> Result<()> {
        let mut fds = [0 as c_int; 2];
        e(unsafe { gxd::gxd_pipe(fds.as_mut_ptr()) })?;
        for fd in fds {
            if flags & O_NONBLOCK != 0 {
                e(unsafe { gxd::gxd_fcntl_setfl(fd, O_NONBLOCK) })?;
            }
            if flags & O_CLOEXEC != 0 {
                e(unsafe { gxd::gxd_fcntl_setfd(fd, FD_CLOEXEC) })?;
            }
        }
        fildes.write(fds);
        Ok(())
    }

    /// Linux `dirent64` records, `d_off` the seek position of the next.
    fn getdents(fd: c_int, buf: &mut [u8], _off: u64) -> Result<usize> {
        e(unsafe { gxd::gxd_getdents(fd, buf.as_mut_ptr().cast(), buf.len()) })
    }

    fn dir_seek(fd: c_int, off: u64) -> Result<()> {
        Self::lseek(fd, off as off_t, SEEK_SET).map(|_| ())
    }

    unsafe fn dent_reclen_offset(this_dent: &[u8], _offset: usize) -> Option<(u16, u64)> {
        let dent = this_dent.as_ptr().cast::<dirent>();
        unsafe { Some(((*dent).d_reclen, (*dent).d_off.cast_unsigned())) }
    }

    /// As on Linux: getdents from the current position, then seek past the
    /// last record.
    fn posix_getdents(fildes: c_int, buf: &mut [u8]) -> Result<usize> {
        let current = Self::lseek(fildes, 0, SEEK_CUR)?.cast_unsigned();
        let read = Self::getdents(fildes, buf, current)?;
        let (mut done, mut next) = (0, current);
        while done < read {
            let (reclen, opaque_next) =
                unsafe { Self::dent_reclen_offset(&buf[done..], done) }.ok_or(Errno(EIO))?;
            if reclen == 0 {
                return Err(Errno(EIO));
            }
            done += reclen as usize;
            next = opaque_next;
        }
        if read > 0 {
            Self::lseek(fildes, next as off_t, SEEK_SET)?;
        }
        Ok(read)
    }

    fn mkdirat(dir_fildes: c_int, path: CStr, mode: mode_t) -> Result<()> {
        e(unsafe { gxd::gxd_mkdirat(dir_fildes, path.as_ptr(), mode as c_uint) }).map(|_| ())
    }

    fn unlinkat(fd: c_int, path: CStr, flags: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_unlinkat(fd, path.as_ptr(), flags) }).map(|_| ())
    }

    fn renameat2(old_dir: c_int, old_path: CStr, new_dir: c_int, new_path: CStr, flags: c_uint) -> Result<()> {
        if flags != 0 {
            return Err(Errno(EINVAL)); /* no RENAME_NOREPLACE or RENAME_EXCHANGE */
        }
        e(unsafe { gxd::gxd_renameat(old_dir, old_path.as_ptr(), new_dir, new_path.as_ptr()) }).map(|_| ())
    }

    fn symlinkat(path1: CStr, fd: c_int, path2: CStr) -> Result<()> {
        e(unsafe { gxd::gxd_symlinkat(path1.as_ptr(), fd, path2.as_ptr()) }).map(|_| ())
    }

    fn readlinkat(dirfd: c_int, pathname: CStr, out: &mut [u8]) -> Result<usize> {
        e(unsafe { gxd::gxd_readlinkat(dirfd, pathname.as_ptr(), out.as_mut_ptr().cast(), out.len()) })
    }

    fn faccessat(fd: c_int, path: CStr, amode: c_int, _flags: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_faccessat(fd, path.as_ptr(), amode) }).map(|_| ())
    }

    fn chdir(path: CStr) -> Result<()> {
        e(unsafe { gxd::gxd_chdir(path.as_ptr()) }).map(|_| ())
    }

    fn fchdir(fildes: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_fchdir(fildes) }).map(|_| ())
    }

    fn getcwd(mut buf: Out<[u8]>) -> Result<()> {
        e(unsafe { gxd::gxd_getcwd(buf.as_mut_ptr().cast(), buf.len()) }).map(|_| ())
    }

    /* the VFS has no owners, no permission bits to change, no timestamps to set */

    fn fchownat(_: c_int, _: CStr, _: uid_t, _: gid_t, _: c_int) -> Result<()> {
        Ok(())
    }

    fn fchmodat(_: c_int, _: Option<CStr>, _: mode_t, _: c_int) -> Result<()> {
        Ok(())
    }

    unsafe fn utimensat(_: c_int, _: CStr, _: *const timespec, _: c_int) -> Result<()> {
        Ok(())
    }

    fn umask(_mask: mode_t) -> mode_t {
        0o022
    }

    /// Granted at once, as fcntl's locks
    fn flock(fd: c_int, _operation: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_fcntl_getfl(fd) }).map(|_| ())
    }

    /// The VFS reports no capacity: a file system of its own size
    fn fstatvfs(fildes: c_int, mut buf: Out<statvfs>) -> Result<()> {
        e(unsafe { gxd::gxd_fcntl_getfl(fildes) })?;
        let mut st: statvfs = unsafe { core::mem::zeroed() };
        st.f_bsize = 4096;
        st.f_frsize = 4096;
        st.f_namemax = 255;
        buf.write(st);
        Ok(())
    }

    fn linkat(_: c_int, _: CStr, _: c_int, _: CStr, _: c_int) -> Result<()> { Err(Errno(ENOSYS)) }
    fn mknodat(_: c_int, _: CStr, _: mode_t, _: dev_t) -> Result<()> { Err(Errno(ENOSYS)) }
    fn mkfifoat(_: c_int, _: CStr, _: mode_t) -> Result<()> { Err(Errno(ENOSYS)) }
    fn fpath(_: c_int, _: &mut [u8]) -> Result<usize> { Err(Errno(ENOSYS)) }
    fn posix_fallocate(_: c_int, _: u64, _: NonZeroU64) -> Result<()> { Err(Errno(ENOSYS)) }
    fn sync() -> Result<()> { Err(Errno(ENOSYS)) }

    /* memory: anonymous mappings of RAM dataspaces */

    /// Anonymous memory at an address of Genode's choosing, and private
    /// mappings of files as a copy of their content (as Genode's libc).
    unsafe fn mmap(_addr: *mut c_void, len: usize, prot: c_int, flags: c_int, fildes: c_int, off: off_t)
        -> Result<*mut c_void>
    {
        if flags & MAP_FIXED != 0 {
            return Err(Errno(EINVAL));
        }
        if flags & MAP_ANONYMOUS == 0 && (flags & MAP_PRIVATE == 0 || flags & MAP_SHARED != 0) {
            /* a shared file mapping would need the VFS's dataspaces */
            return Err(if prot & PROT_WRITE != 0 { Errno(EACCES) } else { Errno(ENODEV) });
        }
        let addr = e(unsafe { gxd::gxd_mmap_anon(len) })? as *mut c_void;
        if flags & MAP_ANONYMOUS != 0 {
            return Ok(addr);
        }
        /* the file's content; beyond its end, the zeroes of new RAM */
        let buf = unsafe { core::slice::from_raw_parts_mut(addr.cast::<u8>(), len) };
        let mut done = 0;
        while done < len {
            match Self::pread(fildes, &mut buf[done..], off + done as off_t) {
                Ok(0) => break,
                Ok(n) => done += n,
                Err(err) => {
                    let _ = unsafe { Self::munmap(addr, len) };
                    return Err(err);
                }
            }
        }
        Ok(addr)
    }

    unsafe fn munmap(addr: *mut c_void, len: usize) -> Result<()> {
        e(unsafe { gxd::gxd_munmap(addr, len) }).map(|_| ())
    }

    unsafe fn mprotect(_: *mut c_void, _: usize, _: c_int) -> Result<()> {
        Ok(()) /* RAM stays read-write */
    }

    unsafe fn madvise(_: *mut c_void, _: usize, _: c_int) -> Result<()> {
        Ok(())
    }

    unsafe fn brk(_addr: *mut c_void) -> Result<*mut c_void> {
        Err(Errno(ENOMEM)) /* no program break: relibc's allocator uses mmap */
    }

    fn getpagesize() -> usize {
        4096
    }

    unsafe fn mremap(_: *mut c_void, _: usize, _: usize, _: c_int, _: *mut c_void) -> Result<*mut c_void> {
        Err(Errno(ENOSYS))
    }
    unsafe fn msync(_: *mut c_void, _: usize, _: c_int) -> Result<()> { Err(Errno(ENOSYS)) }
    unsafe fn mlock(_: *const c_void, _: usize) -> Result<()> { Err(Errno(ENOSYS)) }
    unsafe fn mlockall(_: c_int) -> Result<()> { Err(Errno(ENOSYS)) }
    unsafe fn munlock(_: *const c_void, _: usize) -> Result<()> { Err(Errno(ENOSYS)) }
    unsafe fn munlockall() -> Result<()> { Err(Errno(ENOSYS)) }

    /* time and randomness */

    fn clock_gettime(clk_id: clockid_t, mut tp: Out<timespec>) -> Result<()> {
        let ns = unsafe { gxd::gxd_clock_ns(clk_id as c_int) };
        if ns < 0 {
            return Err(Errno(-ns as c_int));
        }
        tp.write(timespec { tv_sec: (ns / 1_000_000_000) as time_t, tv_nsec: (ns % 1_000_000_000) as c_long });
        Ok(())
    }

    fn clock_getres(_clk_id: clockid_t, res: Option<Out<timespec>>) -> Result<()> {
        if let Some(mut res) = res {
            res.write(timespec { tv_sec: 0, tv_nsec: 1_000 }); /* the timer's microseconds */
        }
        Ok(())
    }

    fn gettimeofday(mut tp: Out<timeval>, _tzp: Option<Out<timezone>>) -> Result<()> {
        let ns = unsafe { gxd::gxd_clock_ns(CLOCK_REALTIME as c_int) };
        if ns < 0 {
            return Err(Errno(-ns as c_int));
        }
        tp.write(timeval {
            tv_sec: (ns / 1_000_000_000) as time_t,
            tv_usec: ((ns % 1_000_000_000) / 1000) as suseconds_t,
        });
        Ok(())
    }

    /// Ends early when a handler ran for a signal (EINTR, the rest in `rmtp`)
    unsafe fn nanosleep(rqtp: *const timespec, rmtp: *mut timespec) -> Result<()> {
        let r = unsafe { &*rqtp };
        let ns = (r.tv_sec as i64).saturating_mul(1_000_000_000).saturating_add(r.tv_nsec as i64);
        let deadline = unsafe { gxd::gxd_clock_ns(CLOCK_MONOTONIC as c_int) }.saturating_add(ns);
        let remaining = || (deadline - unsafe { gxd::gxd_clock_ns(CLOCK_MONOTONIC as c_int) }).max(0);
        loop {
            match e(unsafe { gxd::gxd_sleep_ns(remaining() as u64) }) {
                Err(Errno(EINTR)) => {
                    if signal::deliver() {
                        if !rmtp.is_null() {
                            unsafe { rmtp.write(ns_timespec(remaining())) };
                        }
                        return Err(Errno(EINTR));
                    }
                }
                other => {
                    other?;
                    if !rmtp.is_null() {
                        unsafe { rmtp.write(timespec { tv_sec: 0, tv_nsec: 0 }) };
                    }
                    return Ok(());
                }
            }
        }
    }

    fn getrandom(buf: &mut [u8], _flags: c_uint) -> Result<usize> {
        e(unsafe { gxd::gxd_getrandom(buf.as_mut_ptr().cast(), buf.len()) })
    }

    unsafe fn clock_settime(_: clockid_t, _: *const timespec) -> Result<()> { Err(Errno(ENOSYS)) }
    /// POSIX timers of the shim, which raise their signal within the
    /// component (relibc's alarm uses them). SIGEV_THREAD has no place in
    /// Linux's sigevent, which relibc uses.
    fn timer_create(clock_id: clockid_t, evp: &sigevent) -> Result<timer_t> {
        let signo = match evp.sigev_notify {
            SIGEV_SIGNAL => evp.sigev_signo,
            SIGEV_NONE => 0,
            _ => return Err(Errno(EINVAL)),
        };
        if clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC {
            return Err(Errno(EINVAL));
        }
        e(unsafe { gxd::gxd_timer_create(signo, clock_id as c_int) }).map(|id| id as timer_t)
    }

    fn timer_delete(timerid: timer_t) -> Result<()> {
        e(unsafe { gxd::gxd_timer_delete(timerid as c_long) }).map(|_| ())
    }

    fn timer_gettime(timerid: timer_t) -> Result<itimerspec> {
        let (mut value, mut interval) = (0i64, 0i64);
        e(unsafe { gxd::gxd_timer_gettime(timerid as c_long, &mut value, &mut interval) })?;
        Ok(itimerspec { it_interval: ns_timespec(interval), it_value: ns_timespec(value) })
    }

    fn timer_settime(timerid: timer_t, flags: c_int, value: &itimerspec, ovalue: Option<Out<itimerspec>>) -> Result<()> {
        let ns = |t: &timespec| t.tv_sec as i64 * 1_000_000_000 + t.tv_nsec as i64;
        let (mut old_value, mut old_interval) = (0i64, 0i64);
        e(unsafe {
            gxd::gxd_timer_settime(timerid as c_long, ns(&value.it_value), ns(&value.it_interval),
                                   (flags & TIMER_ABSTIME != 0) as c_int, &mut old_value, &mut old_interval)
        })?;
        if let Some(mut ovalue) = ovalue {
            ovalue.write(itimerspec { it_interval: ns_timespec(old_interval), it_value: ns_timespec(old_value) });
        }
        Ok(())
    }

    /* threads */

    /// `stack` holds the entry point and its six arguments, as for Linux's
    /// clone; the shim starts a Genode thread that makes the call.
    unsafe fn rlct_clone(stack: *mut usize, _os_specific: &mut OsSpecific) -> Result<OsTid> {
        let tid = e(unsafe { gxd::gxd_thread_create(stack) })?;
        Ok(OsTid { thread_id: tid })
    }

    /// pthread_cancel: Genode delivers no signals, so the shim stops the
    /// thread at once (as Genode's libc does), and its joiner learns of the
    /// cancellation as from exit_current_thread. The thread's cleanup
    /// handlers and TLS destructors do not run, and a detached thread's TCB
    /// stays allocated.
    unsafe fn rlct_kill(os_tid: OsTid, signal: usize) -> Result<()> {
        if signal != SIGRT_RLCT_CANCEL {
            return Err(Errno(ENOSYS));
        }
        let tp = e(unsafe { gxd::gxd_thread_cancel(os_tid.thread_id as c_int) })?;
        let tcb = unsafe { *((tp + offset_of!(GenericTcb<OsSpecific>, tcb_ptr)) as *const *const Tcb) };
        let thread = unsafe { &(*tcb).pthread };
        let _ = unsafe { Self::munmap(thread.stack_base, thread.stack_size) };
        unsafe { thread.waitval.post(Retval(PTHREAD_CANCELED)) };
        Ok(())
    }

    fn current_os_tid() -> OsTid {
        OsTid { thread_id: unsafe { gxd::gxd_gettid() } as usize }
    }

    fn gettid() -> pid_t {
        unsafe { gxd::gxd_gettid() }
    }

    unsafe fn exit_thread(stack_base: *mut (), stack_size: usize) -> ! {
        /* the thread runs on a Genode stack: the one relibc allocated is unused */
        unsafe { gxd::gxd_munmap(stack_base.cast(), stack_size) };
        unsafe { gxd::gxd_thread_exit() }
    }

    unsafe fn futex_wait(addr: *mut u32, val: u32, deadline: Option<&timespec>) -> Result<()> {
        /* the absolute monotonic deadline, as with Linux's FUTEX_WAIT_BITSET */
        let deadline_ns = deadline.map_or(-1, |d| d.tv_sec as i64 * 1_000_000_000 + d.tv_nsec as i64);
        e(unsafe { gxd::gxd_futex_wait(addr, val, deadline_ns) }).map(|_| ())
    }

    unsafe fn futex_wake(addr: *mut u32, num: u32) -> Result<u32> {
        e(unsafe { gxd::gxd_futex_wake(addr, num) }).map(|n| n as u32)
    }

    fn sched_yield() -> Result<()> {
        Ok(())
    }

    /* the process: a Genode component, one user, no process tree */

    fn exit(status: c_int) -> ! {
        unsafe { gxd::gxd_exit(status) }
    }

    fn getpid() -> pid_t { unsafe { gxd::gxd_getpid() } }
    fn getppid() -> pid_t { unsafe { gxd::gxd_getppid() } }
    fn getuid() -> uid_t { 0 }
    fn geteuid() -> uid_t { 0 }
    fn getgid() -> gid_t { 0 }
    fn getegid() -> gid_t { 0 }

    fn getrlimit(_resource: c_int, mut rlim: Out<rlimit>) -> Result<()> {
        rlim.write(rlimit { rlim_cur: !0, rlim_max: !0 }); /* RLIM_INFINITY */
        Ok(())
    }

    fn uname(mut utsname: Out<utsname>) -> Result<()> {
        let mut u: utsname = unsafe { core::mem::zeroed() };
        let put = |dst: &mut [c_char], s: &[u8]| dst.iter_mut().zip(s).for_each(|(d, b)| *d = *b as c_char);
        put(&mut u.sysname, b"Genode");
        put(&mut u.nodename, b"genode");
        put(&mut u.release, b"genode");
        put(&mut u.version, b"relibc");
        put(&mut u.machine, b"x86_64");
        utsname.write(u);
        Ok(())
    }

    fn verify() -> bool {
        true
    }

    /// As Genode's libc: the dynamic linker replaces the program within the
    /// component, which keeps its libraries, fds (but those with
    /// `FD_CLOEXEC`) and VFS. The program's own state goes: its other
    /// threads, atexit functions, signal handlers, getopt's position.
    unsafe fn execve(path: CStr, argv: *const *mut c_char, envp: *const *mut c_char) -> Result<()> {
        e(unsafe { gxd::gxd_exec_load(path.as_ptr()) })?;
        /* the old program is gone from here on */
        unsafe {
            crate::header::stdio::flush_io_streams();
            crate::pthread::terminate_from_main_thread();
            crate::header::stdlib::forget_exit_functions();
            crate::header::getopt::reset();
        }
        crate::cxa::forget_cxa_exit_functions();
        signal::reset_for_exec();
        unsafe { gxd::gxd_exec_run(argv, envp) }
    }
    unsafe fn fexecve(_: c_int, _: *const *mut c_char, _: *const *mut c_char) -> Result<()> { Err(Errno(ENOSYS)) }
    /// As Genode's libc: a child component of the same binary, which copies
    /// relibc's state from the parent and continues on the forking thread,
    /// which must be the one of main
    unsafe fn fork() -> Result<pid_t> {
        let pid = e(unsafe { gxd::gxd_fork() })? as pid_t;
        if pid == 0 {
            signal::reset_for_fork();
        }
        Ok(pid)
    }
    unsafe fn spawn(
        program: CStr,
        file_actions: Option<&crate::header::spawn::posix_spawn_file_actions_t>,
        attrs: Option<&crate::header::spawn::posix_spawnattr_t>,
        argv: crate::iter::NulTerminated<*mut c_char>,
        envp: Option<crate::iter::NulTerminated<*mut c_char>>,
    ) -> Result<pid_t> {
        unsafe { spawn::spawn(program, file_actions, attrs, argv, envp) }
    }

    /// The children of fork and posix_spawn: their exit codes (no stops, no continues)
    fn waitpid(pid: pid_t, stat_loc: Option<Out<c_int>>, options: c_int) -> Result<pid_t> {
        let mut status: c_int = 0;
        let pid = loop {
            match e(unsafe { gxd::gxd_waitpid(pid, &mut status, options) }) {
                /* a handler, then EINTR: other signals (ignored ones) do not interrupt */
                Err(Errno(EINTR)) if !signal::deliver() => continue,
                r => break r? as pid_t,
            }
        };
        if let Some(mut out) = stat_loc {
            out.write(status);
        }
        Ok(pid)
    }
    fn getgroups(_: Out<[gid_t]>) -> Result<c_int> { Err(Errno(ENOSYS)) }
    unsafe fn setgroups(_: size_t, _: *const gid_t) -> Result<()> { Err(Errno(ENOSYS)) }
    fn getpgid(_: pid_t) -> Result<pid_t> { Err(Errno(ENOSYS)) }
    fn setpgid(_: pid_t, _: pid_t) -> Result<()> { Err(Errno(ENOSYS)) }
    fn getsid(_: pid_t) -> Result<pid_t> { Err(Errno(ENOSYS)) }
    fn setsid() -> Result<c_int> { Err(Errno(ENOSYS)) }
    fn getpriority(_: c_int, _: id_t) -> Result<c_int> { Err(Errno(ENOSYS)) }
    fn setpriority(_: c_int, _: id_t, _: c_int) -> Result<()> { Err(Errno(ENOSYS)) }
    fn getresgid(_: Option<Out<gid_t>>, _: Option<Out<gid_t>>, _: Option<Out<gid_t>>) -> Result<()> {
        Err(Errno(ENOSYS))
    }
    fn getresuid(_: Option<Out<uid_t>>, _: Option<Out<uid_t>>, _: Option<Out<uid_t>>) -> Result<()> {
        Err(Errno(ENOSYS))
    }
    fn setresgid(_: gid_t, _: gid_t, _: gid_t) -> Result<()> { Err(Errno(ENOSYS)) }
    fn setresuid(_: uid_t, _: uid_t, _: uid_t) -> Result<()> { Err(Errno(ENOSYS)) }
    unsafe fn setrlimit(_: c_int, _: *const rlimit) -> Result<()> { Err(Errno(ENOSYS)) }
    fn getrusage(_: c_int, _: Out<rusage>) -> Result<()> { Err(Errno(ENOSYS)) }
}
