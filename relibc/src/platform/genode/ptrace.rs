use super::{
    super::{PalPtrace, types::*},
    Sys,
};
use crate::{
    error::{Errno, Result},
    header::errno::ENOSYS,
};

impl PalPtrace for Sys {
    unsafe fn ptrace(_: c_int, _: pid_t, _: *mut c_void, _: *mut c_void) -> Result<c_int> {
        Err(Errno(ENOSYS))
    }
}
