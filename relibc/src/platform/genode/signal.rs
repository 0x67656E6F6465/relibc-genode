//! Signals within the component, as with Genode's libc: Genode delivers
//! none to a component. The shim keeps the pending signals (timers, raise,
//! kill of the component itself add to them; its blocking waits end with
//! EINTR for a signal the thread does not block); this module keeps the
//! actions and the masks and runs the handlers, at the points where the
//! program enters the platform: raise and kill, sigprocmask, sigsuspend,
//! nanosleep, epoll (poll, select).

use core::{
    cell::Cell,
    mem,
    sync::atomic::{AtomicI32, AtomicU64, AtomicUsize, Ordering},
};

use super::{
    super::{PalSignal, types::*},
    Sys, e, gxd,
};
#[expect(deprecated)]
use crate::header::sys_time::{ITIMER_REAL, itimerval};
use crate::{
    error::{Errno, Result},
    header::{
        bits_sigset_t::sigset_t,
        errno::{EAGAIN, EINTR, EINVAL, ESRCH},
        signal::{
            SA_NODEFER, SA_RESETHAND, SA_SIGINFO, SI_USER, SIGALRM, SIGCHLD, SIGCONT, SIGKILL, SIGSTOP, SIGURG,
            SIGWINCH, sigaction, siginfo_t, sigval, stack_t,
        },
        sys_select::timeval,
        time::timespec,
    },
    platform::Pal,
};

const SIGS: usize = 64;

const SIG_DFL: usize = 0;
const SIG_IGN: usize = 1;

static HANDLER: [AtomicUsize; SIGS] = [const { AtomicUsize::new(SIG_DFL) }; SIGS];
static FLAGS: [AtomicI32; SIGS] = [const { AtomicI32::new(0) }; SIGS];
static MASKS: [AtomicU64; SIGS] = [const { AtomicU64::new(0) }; SIGS];

#[thread_local]
static MASK: Cell<sigset_t> = Cell::new(0);

const fn bit(sig: usize) -> sigset_t {
    1 << (sig - 1)
}

fn valid(sig: c_int) -> Result<usize> {
    match sig {
        1..=64 => Ok(sig as usize),
        _ => Err(Errno(EINVAL)),
    }
}

fn set_mask(mask: sigset_t) {
    let mask = mask & !(bit(SIGKILL) | bit(SIGSTOP));
    MASK.set(mask);
    unsafe { gxd::gxd_set_sigmask(mask) };
}

/// Set the thread's mask, the previous one returned (epoll_pwait)
pub(super) fn swap_mask(mask: sigset_t) -> sigset_t {
    let old = MASK.get();
    set_mask(mask);
    old
}

/// Run the handlers of the pending signals the thread does not block.
/// Whether one ran.
pub(super) fn deliver() -> bool {
    let mut ran = false;
    loop {
        let sig = unsafe { gxd::gxd_signal_take(!MASK.get()) };
        if sig <= 0 {
            return ran;
        }
        ran |= run(sig as usize);
    }
}

fn run(sig: usize) -> bool {
    let i = sig - 1;
    let handler = HANDLER[i].load(Ordering::Acquire);
    let flags = FLAGS[i].load(Ordering::Acquire);
    match handler {
        SIG_IGN => false,
        SIG_DFL => default_action(sig),
        _ => {
            let old = MASK.get();
            let mut mask = old | MASKS[i].load(Ordering::Acquire);
            if flags & SA_NODEFER as c_int == 0 {
                mask |= bit(sig);
            }
            set_mask(mask);
            if flags & SA_RESETHAND as c_int != 0 {
                HANDLER[i].store(SIG_DFL, Ordering::Release);
                update_caught();
            }
            if flags & SA_SIGINFO as c_int != 0 {
                let f: extern "C" fn(c_int, *mut siginfo_t, *mut c_void) = unsafe { mem::transmute(handler) };
                let mut info: siginfo_t = unsafe { mem::zeroed() };
                info.si_signo = sig as c_int;
                info.si_code = SI_USER;
                f(sig as c_int, &mut info, core::ptr::null_mut());
            } else {
                let f: extern "C" fn(c_int) = unsafe { mem::transmute(handler) };
                f(sig as c_int);
            }
            set_mask(old);
            true
        }
    }
}

/// The signals with a handler, for the shim: it generates SIGCHLD only if
/// caught (or blocked), ignored as it is by default
fn update_caught() {
    let mut mask = 0u64;
    for (i, handler) in HANDLER.iter().enumerate() {
        let handler = handler.load(Ordering::Acquire);
        if handler != SIG_DFL && handler != SIG_IGN {
            mask |= 1 << i;
        }
    }
    unsafe { gxd::gxd_set_caught_signals(mask) };
}

/// In a forked child: the timers are the parent's (POSIX: not inherited),
/// the shim's notion of the handlers is new
pub(super) fn reset_for_fork() {
    REAL_TIMER.store(0, Ordering::Release);
    update_caught();
}

/// For execve: caught signals are reset to their default action, ignored
/// ones stay ignored, the mask and the pending signals stay (POSIX)
pub(super) fn reset_for_exec() {
    for i in 0..SIGS {
        if HANDLER[i].load(Ordering::Acquire) != SIG_IGN {
            HANDLER[i].store(SIG_DFL, Ordering::Release);
        }
        FLAGS[i].store(0, Ordering::Release);
        MASKS[i].store(0, Ordering::Release);
    }
    update_caught();
}

/// The default action: ignore, or end the component (no core dumps, no stop)
fn default_action(sig: usize) -> bool {
    match sig {
        SIGCHLD | SIGURG | SIGWINCH | SIGCONT | SIGSTOP => false,
        s if s > 31 => false, /* real-time signals, relibc's own */
        _ => Sys::exit(128 + sig as c_int),
    }
}

fn raise_and_deliver(sig: c_int) -> Result<()> {
    if sig == 0 {
        return Ok(()); /* the existence check of kill */
    }
    valid(sig)?;
    unsafe { gxd::gxd_signal_raise(sig) };
    deliver();
    Ok(())
}

/// The component itself: pid 1 (Sys::getpid), its group (0), every process (-1)
fn is_self(pid: pid_t) -> bool {
    pid == 0 || pid == -1 || pid == Sys::getpid()
}

/// The timer of ITIMER_REAL (SIGALRM), created when first armed
static REAL_TIMER: AtomicUsize = AtomicUsize::new(0);

fn real_timer() -> Result<c_long> {
    let id = REAL_TIMER.load(Ordering::Acquire);
    if id != 0 {
        return Ok(id as c_long);
    }
    let id = e(unsafe { gxd::gxd_timer_create(SIGALRM as c_int, crate::header::time::CLOCK_MONOTONIC as c_int) })?;
    match REAL_TIMER.compare_exchange(0, id, Ordering::AcqRel, Ordering::Acquire) {
        Ok(_) => Ok(id as c_long),
        Err(other) => {
            unsafe { gxd::gxd_timer_delete(id as c_long) };
            Ok(other as c_long)
        }
    }
}

fn timeval_ns(tv: &timeval) -> i64 {
    tv.tv_sec as i64 * 1_000_000_000 + tv.tv_usec as i64 * 1000
}

fn ns_timeval(ns: i64) -> timeval {
    timeval { tv_sec: (ns / 1_000_000_000) as time_t, tv_usec: ((ns % 1_000_000_000) / 1000) as suseconds_t }
}

impl PalSignal for Sys {
    fn sigaction(sig: c_int, act: Option<&sigaction>, oact: Option<&mut sigaction>) -> Result<(), Errno> {
        let sig = valid(sig)?;
        let i = sig - 1;
        if let Some(oact) = oact {
            *oact = unsafe { mem::zeroed() };
            oact.sa_handler = unsafe { mem::transmute(HANDLER[i].load(Ordering::Acquire)) };
            oact.sa_flags = FLAGS[i].load(Ordering::Acquire);
            oact.sa_mask = MASKS[i].load(Ordering::Acquire);
        }
        if let Some(act) = act {
            if sig == SIGKILL || sig == SIGSTOP {
                return Err(Errno(EINVAL));
            }
            MASKS[i].store(act.sa_mask, Ordering::Release);
            FLAGS[i].store(act.sa_flags, Ordering::Release);
            HANDLER[i].store(act.sa_handler.map_or(SIG_DFL, |f| f as usize), Ordering::Release);
            update_caught();
        }
        Ok(())
    }

    fn sigprocmask(how: c_int, set: Option<&sigset_t>, oset: Option<&mut sigset_t>) -> Result<()> {
        use crate::header::signal::{SIG_BLOCK, SIG_SETMASK, SIG_UNBLOCK};
        let old = MASK.get();
        if let Some(set) = set {
            set_mask(match how {
                SIG_BLOCK => old | *set,
                SIG_UNBLOCK => old & !*set,
                SIG_SETMASK => *set,
                _ => return Err(Errno(EINVAL)),
            });
        }
        if let Some(oset) = oset {
            *oset = old;
        }
        deliver(); /* what became unblocked */
        Ok(())
    }

    fn sigpending(set: &mut sigset_t) -> Result<()> {
        *set = unsafe { gxd::gxd_signals_pending() };
        Ok(())
    }

    fn sigsuspend(mask: &sigset_t) -> Errno {
        let old = MASK.get();
        set_mask(*mask);
        while !deliver() {
            unsafe { gxd::gxd_signal_wait(!MASK.get(), -1) };
        }
        set_mask(old);
        Errno(EINTR)
    }

    fn sigtimedwait(set: &sigset_t, sig: Option<&mut siginfo_t>, tp: Option<&timespec>) -> Result<c_int> {
        let deadline = tp.map_or(-1, |t| {
            let now = unsafe { gxd::gxd_clock_ns(crate::header::time::CLOCK_MONOTONIC as c_int) };
            now + t.tv_sec as i64 * 1_000_000_000 + t.tv_nsec as i64
        });
        loop {
            let signo = unsafe { gxd::gxd_signal_take(*set) };
            if signo > 0 {
                if let Some(info) = sig {
                    *info = unsafe { mem::zeroed() };
                    info.si_signo = signo;
                    info.si_code = SI_USER;
                }
                return Ok(signo);
            }
            if unsafe { gxd::gxd_signal_wait(*set, deadline) } < 0 {
                return Err(Errno(EAGAIN));
            }
        }
    }

    fn raise(sig: c_int) -> Result<()> {
        raise_and_deliver(sig)
    }

    fn kill(pid: pid_t, sig: c_int) -> Result<()> {
        if !is_self(pid) {
            return Err(Errno(ESRCH));
        }
        raise_and_deliver(sig)
    }

    fn killpg(pgrp: pid_t, sig: c_int) -> Result<()> {
        /* the component is its own process group */
        if pgrp != 0 && pgrp != Sys::getpid() {
            return Err(Errno(ESRCH));
        }
        raise_and_deliver(sig)
    }

    fn sigqueue(pid: pid_t, sig: c_int, _val: sigval) -> Result<()> {
        Self::kill(pid, sig)
    }

    #[expect(deprecated)]
    fn getitimer(which: c_int, out: &mut itimerval) -> Result<()> {
        if which != ITIMER_REAL {
            return Err(Errno(EINVAL));
        }
        let (mut value, mut interval) = (0i64, 0i64);
        e(unsafe { gxd::gxd_timer_gettime(real_timer()?, &mut value, &mut interval) })?;
        *out = itimerval { it_interval: ns_timeval(interval), it_value: ns_timeval(value) };
        Ok(())
    }

    #[expect(deprecated)]
    fn setitimer(which: c_int, new: &itimerval, old: Option<&mut itimerval>) -> Result<()> {
        if which != ITIMER_REAL {
            return Err(Errno(EINVAL));
        }
        let (mut value, mut interval) = (0i64, 0i64);
        e(unsafe {
            gxd::gxd_timer_settime(real_timer()?, timeval_ns(&new.it_value), timeval_ns(&new.it_interval), 0,
                                   &mut value, &mut interval)
        })?;
        if let Some(old) = old {
            *old = itimerval { it_interval: ns_timeval(interval), it_value: ns_timeval(value) };
        }
        Ok(())
    }

    unsafe fn sigaltstack(_ss: Option<&stack_t>, _old_ss: Option<&mut stack_t>) -> Result<()> {
        Ok(()) /* handlers run on the stack of the thread that enters the platform */
    }
}
