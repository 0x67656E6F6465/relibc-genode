//! Sockets: the shim implements them with the VFS's socket file system
//! (lwip or lxip), as Genode's libc does. The arguments are Linux's.

use core::{ptr, slice};

use alloc::vec::Vec;

use super::{
    super::{PalSocket, types::*},
    Sys, e, gxd,
};
use crate::{
    error::Result,
    header::{
        bits_iovec::iovec,
        sys_socket::{msghdr, socklen_t},
    },
    out::Out,
};

fn iovecs<'a>(iov: *mut iovec, len: usize) -> &'a [iovec] {
    if iov.is_null() || len == 0 { &[] } else { unsafe { slice::from_raw_parts(iov, len) } }
}

impl PalSocket for Sys {
    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> Result<c_int> {
        e(unsafe { gxd::gxd_socket(domain, kind, protocol) }).map(|fd| fd as c_int)
    }

    fn bind(socket: c_int, address_raw: &[u8]) -> Result<()> {
        e(unsafe { gxd::gxd_bind(socket, address_raw.as_ptr().cast(), address_raw.len() as socklen_t) })
            .map(|_| ())
    }

    fn listen(socket: c_int, backlog: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_listen(socket, backlog) }).map(|_| ())
    }

    fn accept(socket: c_int, address_dst: Option<&mut [u8]>) -> Result<(c_int, socklen_t)> {
        let mut len = address_dst.as_ref().map_or(0, |a| a.len()) as socklen_t;
        let addr = address_dst.map_or(ptr::null_mut(), |a| a.as_mut_ptr().cast());
        let fd = e(unsafe { gxd::gxd_accept(socket, addr, &raw mut len, 0) })?;
        Ok((fd as c_int, len))
    }

    fn connect(socket: c_int, address_raw: &[u8]) -> Result<c_int> {
        e(unsafe { gxd::gxd_connect(socket, address_raw.as_ptr().cast(), address_raw.len() as socklen_t) })
            .map(|r| r as c_int)
    }

    fn recvfrom(
        socket: c_int,
        mut buf: Out<[u8]>,
        flags: c_int,
        address_raw: Option<&mut [u8]>,
    ) -> Result<(usize, socklen_t)> {
        let mut len = address_raw.as_ref().map_or(0, |a| a.len()) as socklen_t;
        let addr = address_raw.map_or(ptr::null_mut(), |a| a.as_mut_ptr().cast());
        let n = e(unsafe {
            gxd::gxd_recvfrom(socket, buf.as_mut_ptr().as_mut_ptr().cast(), buf.len(), flags, addr, &raw mut len)
        })?;
        Ok((n, len))
    }

    fn sendto(socket: c_int, buf: &[u8], flags: c_int, dest: Option<&[u8]>) -> Result<usize> {
        let (dest, dest_len) = dest.map_or((ptr::null(), 0), |d| (d.as_ptr().cast(), d.len() as socklen_t));
        e(unsafe { gxd::gxd_sendto(socket, buf.as_ptr().cast(), buf.len(), flags, dest, dest_len) })
    }

    fn getsockname(socket: c_int, address_dst: &mut [u8]) -> Result<socklen_t> {
        let mut len = address_dst.len() as socklen_t;
        e(unsafe { gxd::gxd_getsockname(socket, address_dst.as_mut_ptr().cast(), &raw mut len) })?;
        Ok(len)
    }

    fn getpeername(socket: c_int, address_dst: &mut [u8]) -> Result<socklen_t> {
        let mut len = address_dst.len() as socklen_t;
        e(unsafe { gxd::gxd_getpeername(socket, address_dst.as_mut_ptr().cast(), &raw mut len) })?;
        Ok(len)
    }

    fn getsockopt(socket: c_int, level: c_int, option_name: c_int, option_value: &mut [u8]) -> Result<socklen_t> {
        let mut len = option_value.len() as socklen_t;
        e(unsafe {
            gxd::gxd_getsockopt(socket, level, option_name, option_value.as_mut_ptr().cast(), &raw mut len)
        })?;
        Ok(len)
    }

    fn setsockopt(socket: c_int, level: c_int, option_name: c_int, option_value: &[u8]) -> Result<()> {
        e(unsafe {
            gxd::gxd_setsockopt(socket, level, option_name, option_value.as_ptr().cast(),
                                option_value.len() as socklen_t)
        })
        .map(|_| ())
    }

    fn shutdown(socket: c_int, how: c_int) -> Result<()> {
        e(unsafe { gxd::gxd_shutdown(socket, how) }).map(|_| ())
    }

    /// recvfrom into the message's buffers; no ancillary data (as with
    /// Genode's libc)
    unsafe fn recvmsg(socket: c_int, msg: *mut msghdr, flags: c_int) -> Result<usize> {
        let msg = unsafe { &mut *msg };
        let iovs = iovecs(msg.msg_iov, msg.msg_iovlen);
        let addr = (!msg.msg_name.is_null())
            .then(|| unsafe { slice::from_raw_parts_mut(msg.msg_name.cast::<u8>(), msg.msg_namelen as usize) });

        let (n, addr_len) = match iovs {
            [] => (0, 0),
            [one] => Self::recvfrom(socket, unsafe { Out::from_raw_parts(one.iov_base.cast(), one.iov_len) }, flags, addr)?,
            _ => {
                /* into one buffer, then scattered */
                let total = iovs.iter().map(|v| v.iov_len).sum();
                let mut buf = Vec::<u8>::with_capacity(total);
                let (n, addr_len) =
                    Self::recvfrom(socket, unsafe { Out::from_raw_parts(buf.as_mut_ptr(), total) }, flags, addr)?;
                unsafe { buf.set_len(n) };
                let mut rest = &buf[..];
                for v in iovs {
                    let k = rest.len().min(v.iov_len);
                    unsafe { ptr::copy_nonoverlapping(rest.as_ptr(), v.iov_base.cast(), k) };
                    rest = &rest[k..];
                }
                (n, addr_len)
            }
        };
        if !msg.msg_name.is_null() {
            msg.msg_namelen = addr_len;
        }
        msg.msg_controllen = 0;
        msg.msg_flags = 0;
        Ok(n)
    }

    /// sendto of the message's buffers, gathered; no ancillary data
    unsafe fn sendmsg(socket: c_int, msg: *const msghdr, flags: c_int) -> Result<usize> {
        let msg = unsafe { &*msg };
        let iovs = iovecs(msg.msg_iov, msg.msg_iovlen);
        let dest = (!msg.msg_name.is_null())
            .then(|| unsafe { slice::from_raw_parts(msg.msg_name.cast::<u8>(), msg.msg_namelen as usize) });
        let bytes = |v: &iovec| unsafe { slice::from_raw_parts(v.iov_base.cast::<u8>(), v.iov_len) };
        match iovs {
            [] => Self::sendto(socket, &[], flags, dest),
            [one] => Self::sendto(socket, bytes(one), flags, dest),
            _ => Self::sendto(socket, &iovs.iter().flat_map(bytes).copied().collect::<Vec<u8>>(), flags, dest),
        }
    }
    /// AF_UNIX stream sockets within the component (two pipes, crossed)
    fn socketpair(domain: c_int, kind: c_int, protocol: c_int, sv: &mut [c_int; 2]) -> Result<()> {
        e(unsafe { gxd::gxd_socketpair(domain, kind, protocol, sv.as_mut_ptr()) }).map(|_| ())
    }
}
