/*
 * \brief  File-descriptor objects of the Genode shim of relibc
 */

#ifndef _FD_H_
#define _FD_H_

#include "state.h"

namespace Gxd {

	struct Fd_object : Interface
	{
		unsigned refs = 1;
		int      oflag;

		Fd_object(int oflag) : oflag(oflag) { }

		virtual ~Fd_object() { }

		virtual long read(void *, size_t)        { return -EBADF; }
		virtual long write(void const *, size_t) { return -EBADF; }
		virtual long seek(long long, int)        { return -ESPIPE; }
		virtual long stat(gxd_stat &)            = 0;
		virtual long getdents(void *, size_t)    { return -ENOTDIR; }
		virtual long truncate(long long)         { return -EINVAL; }
		virtual long sync()                      { return 0; }
		virtual Abs_path const *path() const         { return nullptr; }

		/* the file position, for a forked child (on the entrypoint) */
		virtual long long position() const       { return -1; }

		/* an object of the VFS, which the entrypoint alone uses: it works,
		   and is destroyed, there (ep.cc) */
		virtual bool ep_bound() const            { return false; }

		/* epoll readiness (EPOLLIN 0x1, EPOLLOUT 0x4, EPOLLERR 0x8, EPOLLHUP 0x10) */
		virtual unsigned poll() { return 0x1 | 0x4; }

		/* a socket that send and recv use as a stream (socketpair) */
		virtual bool stream_socket() const       { return false; }

		/* ask the VFS to report a change of readiness (on the entrypoint) */
		virtual void notify_ready() { }
	};

	/* the object of an fd, its installation (vfs_mutex held) */
	Fd_object *lookup(int fd);
	int        install(Fd_object &, int min_fd = 0, bool cloexec = false);

	/* the release of a reference: 'unref' with vfs_mutex held (true for the
	   last), 'dispose' of the object then without; 'release' does both */
	bool unref(Fd_object &);
	void dispose(Fd_object &);
	void release(Fd_object &);

	/* a change of readiness: wakes epoll waiters */
	void io_changed();
}

#endif /* _FD_H_ */
