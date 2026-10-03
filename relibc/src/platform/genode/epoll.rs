//! epoll (relibc's poll and select use it), of the shim's file descriptors

use super::{
    super::{PalEpoll, types::*},
    Sys, e, gxd, signal,
};
use crate::{
    error::{Errno, Result},
    header::errno::EINTR,
    header::{
        bits_sigset_t::sigset_t,
        sys_epoll::{epoll_data, epoll_event},
    },
};

impl PalEpoll for Sys {
    fn epoll_create1(_flags: c_int) -> Result<c_int> {
        e(unsafe { gxd::gxd_epoll_create() }).map(|fd| fd as c_int)
    }

    unsafe fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, event: *mut epoll_event) -> Result<()> {
        let (events, data) =
            if event.is_null() { (0, 0) } else { unsafe { ((*event).events, (*event).data.u64) } };
        e(unsafe { gxd::gxd_epoll_ctl(epfd, op, fd, events, data) }).map(|_| ())
    }

    /// With `sigmask` as the thread's mask meanwhile; a signal's handler
    /// ends the wait (EINTR).
    unsafe fn epoll_pwait(
        epfd: c_int,
        events: *mut epoll_event,
        maxevents: c_int,
        timeout: c_int,
        sigmask: *const sigset_t,
    ) -> Result<usize> {
        let old = (!sigmask.is_null()).then(|| signal::swap_mask(unsafe { *sigmask }));
        let result = unsafe { wait(epfd, events, maxevents, timeout) };
        if let Some(old) = old {
            signal::swap_mask(old);
        }
        result
    }
}

unsafe fn wait(epfd: c_int, events: *mut epoll_event, maxevents: c_int, timeout: c_int) -> Result<usize> {
    loop {
        /* the shim fills two arrays (epoll_event is packed on x86_64) */
        const CHUNK: usize = 64;
        let max = (maxevents.max(0) as usize).min(CHUNK);
        let (mut ev, mut data) = ([0u32; CHUNK], [0u64; CHUNK]);
        let n = match e(unsafe {
            gxd::gxd_epoll_wait(epfd, ev.as_mut_ptr(), data.as_mut_ptr(), max as c_int, timeout)
        }) {
            Err(Errno(EINTR)) if !signal::deliver() => continue, /* taken by another thread */
            other => other?,
        };
        for i in 0..n {
            unsafe { events.add(i).write(epoll_event { events: ev[i], data: epoll_data { u64: data[i] } }) };
        }
        return Ok(n);
    }
}
