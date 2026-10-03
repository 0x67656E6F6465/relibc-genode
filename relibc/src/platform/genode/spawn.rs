//! posix_spawn: the shim starts the program's ROM module as a child
//! component (nothing of the parent copied, from any thread). The file
//! actions apply here, to a map of the child's fds to the parent's: the
//! parent's (but those with FD_CLOEXEC) to begin with, files opened for it
//! in the parent (closed again after the start). The child's signal mask
//! and actions are the defaults, whatever the attributes say.

use alloc::vec::Vec;
use core::ptr;

use super::{
    super::{Pal, types::*},
    Sys, e, gxd,
};
use crate::{
    c_str::{CStr, CString},
    error::{Errno, Result},
    header::{
        errno::EBADF,
        fcntl::AT_FDCWD,
        spawn::{Action, posix_spawn_file_actions_t, posix_spawnattr_t},
    },
    iter::NulTerminated,
};

/// The shim's number of fds
const MAX_FDS: c_int = 256;

/// The working directory: relative to a directory fd, or the parent's
struct Cwd {
    dir_fd: c_int,
    path: Option<Vec<u8>>,
}

impl Cwd {
    fn chdir(&mut self, path: &[u8]) {
        match &mut self.path {
            Some(cur) if path.first() != Some(&b'/') => {
                cur.push(b'/');
                cur.extend_from_slice(path);
            }
            _ => {
                if path.first() == Some(&b'/') {
                    self.dir_fd = AT_FDCWD;
                }
                self.path = Some(path.to_vec());
            }
        }
    }
}

pub(super) unsafe fn spawn(
    program: CStr,
    actions: Option<&posix_spawn_file_actions_t>,
    _attrs: Option<&posix_spawnattr_t>,
    argv: NulTerminated<*mut c_char>,
    envp: Option<NulTerminated<*mut c_char>>,
) -> Result<pid_t> {
    /* (child fd, parent fd), the parent's that survive exec to begin with */
    let mut fds: Vec<[c_int; 2]> = (0..MAX_FDS)
        .filter(|&fd| unsafe { gxd::gxd_fcntl_getfd(fd) } == 0)
        .map(|fd| [fd, fd])
        .collect();
    let mut opened: Vec<c_int> = Vec::new();
    let mut cwd = Cwd { dir_fd: AT_FDCWD, path: None };

    let parent_fd = |fds: &Vec<[c_int; 2]>, fd: c_int| fds.iter().find(|m| m[0] == fd).map(|m| m[1]);
    let set = |fds: &mut Vec<[c_int; 2]>, fd: c_int, parent: c_int| {
        fds.retain(|m| m[0] != fd);
        fds.push([fd, parent]);
    };

    let result = (|| {
        for action in actions.into_iter().flatten() {
            match action {
                Action::Open { fd, path, flag, mode } => {
                    let parent = Sys::openat(AT_FDCWD, CStr::borrow(&path), flag, mode)?;
                    opened.push(parent);
                    set(&mut fds, fd, parent);
                }
                Action::Close(fd) => fds.retain(|m| m[0] != fd),
                Action::Dup2(fd, new_fd) => {
                    let parent = parent_fd(&fds, fd).ok_or(Errno(EBADF))?;
                    set(&mut fds, new_fd, parent);
                }
                Action::Chdir(path) => cwd.chdir(path.as_bytes()),
                Action::FChdir(fd) => {
                    cwd.dir_fd = parent_fd(&fds, fd).ok_or(Errno(EBADF))?;
                    cwd.path = None;
                }
            }
        }

        let argv: Vec<*mut c_char> = argv.copied().chain([ptr::null_mut()]).collect();
        let envp: Vec<*mut c_char> = match envp {
            Some(envp) => envp.copied().chain([ptr::null_mut()]).collect(),
            None => crate::platform::environ_iter().chain([ptr::null_mut()]).collect(),
        };
        let cwd_path = match cwd.path.take() {
            Some(path) => Some(CString::new(path).map_err(|_| Errno(EBADF))?),
            None => None,
        };
        e(unsafe {
            gxd::gxd_spawn(
                program.as_ptr(),
                argv.as_ptr(),
                envp.as_ptr(),
                fds.as_ptr(),
                fds.len() as c_uint,
                cwd_path.as_ref().map_or(ptr::null(), |p| p.as_ptr()),
                cwd.dir_fd,
            )
        })
        .map(|pid| pid as pid_t)
    })();

    for fd in opened {
        let _ = Sys::close(fd);
    }
    result
}
