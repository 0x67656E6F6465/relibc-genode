/*
 * \brief  epoll of the Genode shim of relibc (relibc's poll and select use it)
 *
 * An epoll fd holds its interest list. A wait is a job of the entrypoint
 * (ep.cc), which checks the readiness of each fd (Fd_object::poll), asks
 * the VFS to report changes of the others, and checks again when the VFS
 * made progress or pipes, close or a signal changed something (io_changed).
 */

#include "fd.h"

using namespace Gxd;

namespace {

enum { EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3 };
enum { EPOLLERR = 0x8, EPOLLHUP = 0x10, EPOLLNVAL = 0x20, EPOLLONESHOT = 1u << 30 };

struct Epoll : Fd_object
{
	struct Entry { int fd; uint32_t events; uint64_t data; bool armed; };

	enum { MAX = 256 };

	Mutex    mutex { };
	Entry    entries[MAX] { };
	unsigned count = 0;

	Epoll() : Fd_object(O_RDWR) { }

	long stat(gxd_stat &st) override
	{
		st = { .mode = 0600, .ino = 0, .size = 0, .blksize = 0, .mtime = 0 };
		return 0;
	}

	Entry *find(int fd)
	{
		for (unsigned i = 0; i < count; i++)
			if (entries[i].fd == fd) return &entries[i];
		return nullptr;
	}

	long ctl(int op, int fd, uint32_t events, uint64_t data)
	{
		Mutex::Guard guard(mutex);
		Entry *e = find(fd);
		switch (op) {
		case EPOLL_CTL_ADD:
			if (e) return -EEXIST;
			if (count == MAX) return -ENOSPC;
			entries[count++] = { fd, events, data, true };
			return 0;
		case EPOLL_CTL_MOD:
			if (!e) return -ENOENT;
			*e = { fd, events, data, true };
			return 0;
		case EPOLL_CTL_DEL:
			if (!e) return -ENOENT;
			*e = entries[--count];
			return 0;
		default:
			return -EINVAL;
		}
	}

	/* the ready entries, written to the arrays (on the entrypoint, vfs_mutex held) */
	long collect(uint32_t *events, uint64_t *data, int max)
	{
		Mutex::Guard guard(mutex);
		int n = 0;
		for (unsigned i = 0; i < count && n < max; i++) {
			Entry &e = entries[i];
			if (!e.armed) continue;
			Fd_object *obj = lookup(e.fd);
			uint32_t ready = obj ? (obj->poll() & (e.events | EPOLLERR | EPOLLHUP)) : (uint32_t)EPOLLNVAL;
			if (!ready) { obj->notify_ready(); continue; }
			events[n] = ready; data[n] = e.data; n++;
			if (e.events & EPOLLONESHOT) e.armed = false;
		}
		return n;
	}
};

} /* anonymous namespace */


extern "C" long gxd_epoll_create()
{
	Fd_object &obj = *new (main().heap) Epoll();
	Mutex::Guard guard(main().vfs_mutex);
	int fd = install(obj);
	if (fd < 0) destroy(main().heap, &obj);
	return fd;
}


extern "C" long gxd_epoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data)
{
	Mutex::Guard guard(main().vfs_mutex);
	Epoll *ep = dynamic_cast<Epoll *>(lookup(epfd));
	if (!ep) return lookup(epfd) ? -EINVAL : -EBADF;
	if (fd == epfd) return -EINVAL;
	if (op != EPOLL_CTL_DEL && !lookup(fd)) return -EBADF;
	return ep->ctl(op, fd, events, data);
}


/* until an fd is ready, the timeout, or a signal the thread does not block */
extern "C" long gxd_epoll_wait(int epfd, uint32_t *events, uint64_t *data, int max, int timeout_ms)
{
	if (max <= 0) return -EINVAL;

	uint64_t  const mask     = thread_state().sigmask;
	long long const deadline = timeout_ms < 0 ? -1
	                         : gxd_clock_ns(1) + (long long)timeout_ms*1000000;
	long result = 0;
	bool const done = on_ep([&] {
		{
			Mutex::Guard guard(main().vfs_mutex);
			Epoll *ep = dynamic_cast<Epoll *>(lookup(epfd));
			if (!ep) { result = -EBADF; return true; }
			if ((result = ep->collect(events, data, max))) return true;
		}
		if (gxd_signals_pending() & ~mask) { result = -EINTR; return true; }
		return timeout_ms == 0;
	}, deadline);

	return done ? result : 0;
}
