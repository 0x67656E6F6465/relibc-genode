/*
 * \brief  Sockets of the Genode shim of relibc, through the VFS's socket
 *         file system
 *
 * As Genode's libc does it (socket_fs_plugin.cc): the config names the
 * socket file system ('+ libc | socket: /socket'), whose IP stack (the VFS
 * plugins lwip or lxip) runs in the component. A socket is a directory of
 * it ('tcp/1'), which reading 'tcp/new_socket' creates and the open
 * handle keeps; its files are the operations: 'bind', 'listen' and
 * 'connect' take addresses ("10.0.0.1:80") or the backlog, 'accept'
 * becomes readable with a pending connection (opening 'accept_socket'
 * takes it), 'data' and 'peek' carry the payload, 'local' and 'remote'
 * name the ends, 'error' holds the last error (a Genode errno).
 *
 * Only IPv4 (AF_INET), TCP and UDP, as with Genode's libc.
 *
 * The sockets are the entrypoint's, as the whole VFS (ep.cc): a blocking
 * operation is a job there, which waits for readiness without blocking the
 * entrypoint and continues on the VFS's progress.
 */

#include "fd.h"

using namespace Gxd;

namespace {

enum {
	AF_UNSPEC = 0, AF_INET = 2,
	SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_NONBLOCK = 04000, SOCK_CLOEXEC = 02000000,
	IPPROTO_TCP = 6, IPPROTO_UDP = 17,
	MSG_PEEK = 0x2, MSG_DONTWAIT = 0x40, MSG_WAITALL = 0x100,
	SOL_SOCKET = 1, SO_REUSEADDR = 2, SO_TYPE = 3, SO_ERROR = 4, SO_KEEPALIVE = 9,
	SO_LINGER = 13, TCP_NODELAY = 1, TCP_KEEPIDLE = 4, TCP_KEEPINTVL = 5, TCP_KEEPCNT = 6,
	SHUT_RD = 0, SHUT_RDWR = 2,

	/* Linux errno values beyond those of state.h */
	EFAULT = 14, EBUSY = 16, EFBIG = 27, EDOM = 33, ENFILE = 23, ENXIO = 6,
	E2BIG = 7, EMLINK = 31, ENODEV = 19, ENOMSG = 42, ENODATA = 61, ENOLINK = 67,
	EPROTO = 71, EBADMSG = 74, EOVERFLOW = 75, EBADFD = 77, EREMCHG = 78,
	EUSERS = 87, ENOTSOCK = 88, EDESTADDRREQ = 89, EMSGSIZE = 90,
	EPROTOTYPE = 91, ENOPROTOOPT = 92, EPROTONOSUPPORT = 93, ESOCKTNOSUPPORT = 94,
	EOPNOTSUPP = 95, EPFNOSUPPORT = 96, EAFNOSUPPORT = 97, EADDRINUSE = 98,
	EADDRNOTAVAIL = 99, ENETDOWN = 100, ENETUNREACH = 101, ECONNABORTED = 103,
	ECONNRESET = 104, ENOBUFS = 105, EISCONN = 106, ENOTCONN = 107,
	ETOOMANYREFS = 109, ECONNREFUSED = 111, EHOSTUNREACH = 113, EALREADY = 114,
	EINPROGRESS = 115, ESTALE = 116, EBADRQC = 56, ENOIOCTLCMD = 515, ENOTSUPP = 524,
};


/*
 * The Linux value of an error of the socket file system: a Genode errno
 * (genode_c_api/socket.h, enum Errno, in its order)
 */
int linux_errno(int genode_errno)
{
	static int const table[] = {
		0, E2BIG, EACCES, EADDRINUSE, EADDRNOTAVAIL, EAFNOSUPPORT, EAGAIN, EALREADY, EBADF,
		EBADFD, EBADMSG, EBADRQC, EBUSY, ECONNABORTED, ECONNREFUSED, EDESTADDRREQ, EDOM,
		EEXIST, EFAULT, EFBIG, EHOSTUNREACH, EINPROGRESS, EINTR, EINVAL, EIO, EISCONN, ELOOP,
		EMLINK, EMSGSIZE, ENAMETOOLONG, ENETDOWN, ENETUNREACH, ENFILE, ENOBUFS, ENODATA,
		ENODEV, ENOENT, ENOIOCTLCMD, ENOLINK, ENOMEM, ENOMSG, ENOPROTOOPT, ENOSPC, ENOSYS,
		ENOTCONN, ENOTSUPP, ENOTTY, ENXIO, EOPNOTSUPP, EOVERFLOW, EPERM, EPFNOSUPPORT, EPIPE,
		EPROTO, EPROTONOSUPPORT, EPROTOTYPE, ERANGE, EREMCHG, ESOCKTNOSUPPORT, ESPIPE, ESRCH,
		ESTALE, ETIMEDOUT, ETOOMANYREFS, EUSERS, EXDEV, ECONNRESET,
	};
	enum { MAX = sizeof(table)/sizeof(table[0]) };
	return (genode_errno >= 0 && genode_errno < (int)MAX) ? table[genode_errno] : EIO;
}


/* sockaddr_in of Linux */
struct Sockaddr_in
{
	uint16_t family;
	uint16_t port;     /* network byte order */
	uint8_t  addr[4];
	uint8_t  zero[8];
};

static_assert(sizeof(Sockaddr_in) == 16);

using Addr_string = String<32>;   /* "255.255.255.255:65535" */

Addr_string addr_string(Sockaddr_in const &a)
{
	return Addr_string((unsigned)a.addr[0], ".", (unsigned)a.addr[1], ".",
	                   (unsigned)a.addr[2], ".", (unsigned)a.addr[3], ":",
	                   (unsigned)((a.port >> 8) | ((a.port & 0xff) << 8)));
}

/* "a.b.c.d:port" (a trailing newline allowed) */
bool parse_addr(char const *s, size_t len, Sockaddr_in &a)
{
	a = { };
	a.family = AF_INET;
	unsigned v[5] { };
	unsigned part = 0;
	bool digit = false;
	for (size_t i = 0; i < len && s[i] && s[i] != '\n'; i++) {
		char const c = s[i];
		if (c >= '0' && c <= '9') { v[part] = v[part]*10 + (c - '0'); digit = true; continue; }
		if (!digit) return false;
		if ((c == '.' && part < 3) || (c == ':' && part == 3)) { part++; digit = false; continue; }
		return false;
	}
	if (part != 4 || !digit) return false;
	for (unsigned i = 0; i < 4; i++) { if (v[i] > 255) return false; a.addr[i] = (uint8_t)v[i]; }
	if (v[4] > 65535) return false;
	a.port = (uint16_t)((v[4] >> 8) | ((v[4] & 0xff) << 8));
	return true;
}


struct Socket : Fd_object
{
	enum Proto { TCP, UDP };
	enum State { UNCONNECTED, ACCEPT_ONLY, CONNECTING, CONNECTED, CONNECT_ABORTED };
	enum File  { DATA, PEEK, CONNECT, BIND, LISTEN, ACCEPT, LOCAL, REMOTE, ERROR, MAX };

	struct Open_failed { };

	Vfs::Vfs_handle &_handle;   /* of 'new_socket' or 'accept_socket': keeps the socket */
	Abs_path const   dir;
	Proto    const   proto;
	State            state = UNCONNECTED;

	Vfs::Vfs_handle *_files[MAX] { };

	Socket(Socket const &) = delete;
	Socket &operator = (Socket const &) = delete;

	static Abs_path _read_dir(Vfs::Vfs_handle &h, Abs_path const &root)
	{
		char buf[Abs_path::capacity()];
		long const n = read_handle(h, buf, sizeof(buf) - 1, true);
		if (n <= 0) {
			warning("relibc: reading the socket's name failed (", n, ")");
			throw Open_failed();
		}
		buf[n] = 0;
		if (buf[n - 1] == '\n') buf[n - 1] = 0;
		return Abs_path(root, "/", Cstring(buf));
	}

	/* on the entrypoint */
	Socket(Vfs::Vfs_handle &handle, Abs_path const &root, Proto proto, int oflag)
	:
		Fd_object(O_RDWR | (oflag & O_NONBLOCK)),
		_handle(handle), dir(_read_dir(handle, root)), proto(proto)
	{
		static char const *name[MAX] = { "data", "peek", "connect", "bind", "listen",
		                                 "accept", "local", "remote", "error" };
		static unsigned const mode[MAX] = {
			Vfs::Directory_service::OPEN_MODE_RDWR,   Vfs::Directory_service::OPEN_MODE_RDONLY,
			Vfs::Directory_service::OPEN_MODE_RDWR,   Vfs::Directory_service::OPEN_MODE_WRONLY,
			Vfs::Directory_service::OPEN_MODE_WRONLY, Vfs::Directory_service::OPEN_MODE_RDONLY,
			Vfs::Directory_service::OPEN_MODE_RDWR,   Vfs::Directory_service::OPEN_MODE_RDWR,
			Vfs::Directory_service::OPEN_MODE_RDONLY };

		for (unsigned i = 0; i < MAX; i++) {
			Abs_path const path(dir, "/", Cstring(name[i]));
			if (main().vfs.root_dir().open(path.string(), mode[i], &_files[i], main().heap)
			    != Vfs::Directory_service::OPEN_OK) {
				warning("relibc: cannot open the socket file ", path);
				_close_files();
				throw Open_failed();
			}
		}
	}

	void _close_files()
	{
		for (Vfs::Vfs_handle *&h : _files)
			if (h) { h->ds().close(h); h = nullptr; }
	}

	/* on the entrypoint (dispose) */
	~Socket() override
	{
		_close_files();
		_handle.ds().close(&_handle);  /* releases the socket */
	}

	bool ep_bound() const override { return true; }

	bool nonblocking(int flags = 0) const { return (oflag & O_NONBLOCK) || (flags & MSG_DONTWAIT); }

	/*
	 * What follows runs on the entrypoint. The operations are jobs (ep.cc):
	 * while one would block, it returns incomplete, the VFS asked to
	 * report readiness, and runs again on the VFS's progress.
	 */

	Vfs::Vfs_handle &file(File f) { return *_files[f]; }

	bool read_ready(File f)
	{
		Vfs::Vfs_handle &h = file(f);
		h.fs().notify_read_ready(&h);
		return h.fs().read_ready(h);
	}

	bool write_ready() { return file(DATA).fs().write_ready(file(DATA)); }

	/*
	 * One read at the start of a short socket file (an address, a status,
	 * an error): its length, -EAGAIN when it would block. Read when ready,
	 * so the wait for its completion is short.
	 */
	long read_file(File f, void *buf, size_t len)
	{
		Vfs::Vfs_handle &h = file(f);
		h.seek(0);
		while (!h.fs().queue_read(&h, len)) main().wait_io();
		for (;;) {
			size_t n = 0;
			auto res = h.fs().complete_read(&h, Byte_range_ptr((char *)buf, len), n);
			if (res == Vfs::File_io_service::READ_QUEUED) { main().wait_io(); continue; }
			if (res == Vfs::File_io_service::READ_ERR_WOULD_BLOCK) return -EAGAIN;
			if (res != Vfs::File_io_service::READ_OK) return -EIO;
			return n;
		}
	}

	long write_file(File f, void const *buf, size_t len)
	{
		Vfs::Vfs_handle &h = file(f);
		h.seek(0);
		size_t n = 0;
		auto res = h.fs().write(&h, Const_byte_range_ptr((char const *)buf, len), n);
		main().vfs.io().commit();
		if (res == Vfs::File_io_service::WRITE_ERR_WOULD_BLOCK) return -EAGAIN;
		if (res != Vfs::File_io_service::WRITE_OK) return -EIO;
		return n;
	}

	void sync_file(File f)
	{
		Vfs::Vfs_handle &h = file(f);
		while (!h.fs().queue_sync(&h)) main().wait_io();
		while (h.fs().complete_sync(&h) == Vfs::File_io_service::SYNC_QUEUED)
			main().wait_io();
	}

	/* the socket's last error, as -errno */
	long error(long fallback = -EIO)
	{
		char buf[64];
		long const n = read_file(ERROR, buf, sizeof(buf));
		if (n <= 0) return fallback;
		int const genode_errno = Node(Const_byte_range_ptr(buf, n)).attribute_value("value", -1);
		int const err = linux_errno(genode_errno);
		return err ? -err : fallback;
	}

	/* write an operation's argument, then sync for its completion */
	long control(File f, char const *text)
	{
		size_t const len = strlen(text);
		if (write_file(f, text, len) != (long)len) return error();
		sync_file(f);
		return 0;
	}

	long connect_status()
	{
		char buf[32] { };
		long const n = read_file(CONNECT, buf, sizeof(buf) - 1);
		if (n <= 0) return -ECONNREFUSED;
		if (!strcmp(buf, "connected", 9))          return 0;
		if (!strcmp(buf, "connection refused", 18)) return -ECONNREFUSED;
		if (!strcmp(buf, "not connected", 13))      return -ENOTCONN;
		if (!strcmp(buf, "no route to host", 16))   return -EHOSTUNREACH;
		return -ECONNREFUSED;
	}

	/* the result of a connect (in progress), as -errno */
	long finish_connect()
	{
		long const r = connect_status();
		state = r ? CONNECT_ABORTED : CONNECTED;
		return r;
	}

	/* an address file ('local', 'remote'), present, as sockaddr_in */
	long address(File f, void *addr, uint32_t *addrlen)
	{
		if (!addr || !addrlen) return -EFAULT;
		char buf[64];
		long const n = read_file(f, buf, sizeof(buf));
		if (n == -EAGAIN || n == 0) return proto == UDP ? -EAGAIN : -ENOTCONN;
		if (n < 0) return n;

		Sockaddr_in a;
		if (!parse_addr(buf, n, a)) return -ENOBUFS;
		memcpy(addr, &a, min((size_t)*addrlen, sizeof(a)));
		*addrlen = sizeof(a);
		return 0;
	}

	/* a job's step: 'f' ready, else incomplete (or -EAGAIN when non-blocking) */
	bool wait_ready(File f, bool nonblock, long &result)
	{
		if (read_ready(f)) return true;
		if (nonblock) result = -EAGAIN;
		return false;
	}

	long recv(void *buf, size_t len, int flags, void *addr, uint32_t *addrlen)
	{
		if (!len) return 0;

		File const f        = (flags & MSG_PEEK) ? PEEK : DATA;
		bool       addr_done = !addr;
		bool       queued    = false;
		size_t     got       = 0;
		long       result    = 0;

		on_ep([&] {
			/* a datagram's sender, before its data */
			if (!addr_done) {
				if (!wait_ready(REMOTE, nonblocking(flags), result)) return result != 0;
				if ((result = address(REMOTE, addr, addrlen))) return true;
				addr_done = true;
			}
			for (;;) {
				Vfs::Vfs_handle &h = file(f);
				if (!queued) {
					if ((got || nonblocking(flags)) && !read_ready(f)) {
						result = got ? (long)got : -EAGAIN;
						return true;
					}
					h.seek(0);
					if (!h.fs().queue_read(&h, len - got)) return false;
					queued = true;
				}
				size_t n = 0;
				auto const res = h.fs().complete_read(&h, Byte_range_ptr((char *)buf + got, len - got), n);
				if (res == Vfs::File_io_service::READ_QUEUED) return false;
				queued = false;

				if (res == Vfs::File_io_service::READ_ERR_WOULD_BLOCK) {
					if (got || nonblocking(flags)) { result = got ? (long)got : -EAGAIN; return true; }
					h.fs().notify_read_ready(&h);
					return false;   /* again when data arrived */
				}
				if (res != Vfs::File_io_service::READ_OK) { result = got ? (long)got : error(); return true; }
				if (n == 0) { result = got; return true; }   /* end of the stream */
				got += n;
				if (!(flags & MSG_WAITALL) || got == len || f == PEEK) { result = got; return true; }
			}
		});
		return result;
	}

	long send(void const *buf, size_t len, int flags, void const *dest, uint32_t destlen)
	{
		if (dest && proto == UDP) {
			if (destlen < sizeof(Sockaddr_in)) return -EINVAL;
			if (((Sockaddr_in const *)dest)->family != AF_INET) return -EAFNOSUPPORT;
		}
		bool   addr_done = !(dest && proto == UDP);
		size_t sent      = 0;
		long   result    = 0;

		on_ep([&] {
			if (!addr_done) {
				Addr_string const s = addr_string(*(Sockaddr_in const *)dest);
				if (write_file(REMOTE, s.string(), s.length() - 1) != (long)s.length() - 1) {
					result = -EIO;
					return true;
				}
				addr_done = true;
			}
			while (sent < len) {
				long const n = write_file(DATA, (char const *)buf + sent, len - sent);
				if (n == -EAGAIN || n == 0) {
					if (sent || nonblocking(flags)) { result = sent ? (long)sent : -EAGAIN; return true; }
					return false;   /* again when there is room */
				}
				if (n < 0) { result = sent ? (long)sent : error(n); return true; }
				sent += n;
				if (proto == UDP) break;
			}
			result = sent;
			return true;
		});
		return result;
	}

	long read(void *buf, size_t len) override               { return recv(buf, len, 0, nullptr, nullptr); }
	long write(void const *buf, size_t len) override        { return send(buf, len, 0, nullptr, 0); }

	long stat(gxd_stat &st) override
	{
		enum { S_IFSOCK = 0140000 };
		st = { .mode = S_IFSOCK | 0600, .ino = 0, .size = 0, .blksize = 4096, .mtime = 0 };
		return 0;
	}

	/* on the entrypoint (epoll) */
	unsigned poll() override
	{
		enum { IN = 0x1, OUT = 0x4 };
		unsigned r = 0;
		if (state == ACCEPT_ONLY ? read_ready(ACCEPT) : read_ready(DATA)) r |= IN;
		if (state == CONNECTING ? read_ready(CONNECT)
		                        : (state != ACCEPT_ONLY && write_ready()))
			r |= OUT;
		return r;
	}
};


Abs_path socket_root()
{
	Libc_path const root = libc_config("socket");
	return Abs_path(root.string());
}


/* a new socket from the handle of 'new_socket' or 'accept_socket' (on the entrypoint) */
long install_socket(Vfs::Vfs_handle &h, Socket::Proto proto, int oflag, Socket *&out)
{
	Socket *s = nullptr;
	try { s = new (main().heap) Socket(h, socket_root(), proto, oflag); }
	catch (Socket::Open_failed) { h.ds().close(&h); return -ENFILE; }
	int fd;
	{
		Mutex::Guard guard(main().vfs_mutex);
		fd = install(*s, 0, oflag & O_CLOEXEC);
	}
	if (fd < 0) { destroy(main().heap, s); return fd; }
	out = s;
	return fd;
}


template <typename FN>
long with_socket(int fd, FN const &fn)
{
	Socket *s;
	{
		Mutex::Guard guard(main().vfs_mutex);
		Fd_object *obj = lookup(fd);
		if (!obj) return -EBADF;
		s = dynamic_cast<Socket *>(obj);
		if (!s) return -ENOTSOCK;
		s->refs++;  /* keeps it while it blocks */
	}
	long const ret = fn(*s);
	release(*s);
	return ret;
}

} /* anonymous namespace */


extern "C" long gxd_socket(int domain, int type, int protocol)
{
	if (domain != AF_INET) return -EAFNOSUPPORT;

	int const kind = type & 0xf;
	Socket::Proto proto;
	if (kind == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP))     proto = Socket::TCP;
	else if (kind == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) proto = Socket::UDP;
	else return -EPROTONOSUPPORT;

	return ep_call([&] () -> long {
		Abs_path const root = socket_root();
		if (root.length() <= 1) {
			warning("relibc: no socket file system ('+ libc | socket: ...' in the config)");
			return -EACCES;
		}
		Abs_path const path(root, proto == Socket::TCP ? "/tcp" : "/udp", "/new_socket");

		Vfs::Vfs_handle *h = nullptr;
		if (main().vfs.root_dir().open(path.string(), Vfs::Directory_service::OPEN_MODE_RDONLY,
		                               &h, main().heap) != Vfs::Directory_service::OPEN_OK)
			return -EACCES;

		Socket *s;
		return install_socket(*h, proto, ((type & SOCK_NONBLOCK) ? O_NONBLOCK : 0)
		                               | ((type & SOCK_CLOEXEC) ? O_CLOEXEC : 0), s);
	});
}


extern "C" long gxd_bind(int fd, void const *addr, uint32_t len)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		if (!addr || len < sizeof(Sockaddr_in)) return -EINVAL;
		Sockaddr_in const &a = *(Sockaddr_in const *)addr;
		if (a.family != AF_INET) return -EAFNOSUPPORT;
		return ep_call([&] { return s.control(Socket::BIND, addr_string(a).string()); });
	});
}


extern "C" long gxd_listen(int fd, int backlog)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		return ep_call([&] () -> long {
			if (long r = s.control(Socket::LISTEN, String<16>(backlog).string())) return r;
			s.state = Socket::ACCEPT_ONLY;
			return 0;
		});
	});
}


extern "C" long gxd_accept(int fd, void *addr, uint32_t *addrlen, int flags)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		long result = 0;
		on_ep([&] {
			if (s.state != Socket::ACCEPT_ONLY) { result = -EINVAL; return true; }

			/* a pending connection */
			if (!s.wait_ready(Socket::ACCEPT, s.nonblocking(), result)) return result != 0;
			char buf[64];
			long const n = s.read_file(Socket::ACCEPT, buf, sizeof(buf));
			if (n == -EAGAIN || n == 0) return false;   /* taken meanwhile */
			if (n < 0) { result = -EINVAL; return true; }

			Abs_path const path(s.dir, "/accept_socket");
			Vfs::Vfs_handle *h = nullptr;
			if (main().vfs.root_dir().open(path.string(), Vfs::Directory_service::OPEN_MODE_RDONLY,
			                               &h, main().heap) != Vfs::Directory_service::OPEN_OK) {
				result = -ECONNABORTED;
				return true;
			}
			Socket *n_socket = nullptr;
			long const new_fd = install_socket(*h, s.proto, ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0)
			                                   | ((flags & SOCK_CLOEXEC) ? O_CLOEXEC : 0), n_socket);
			if (new_fd < 0) { result = new_fd; return true; }

			n_socket->state = Socket::CONNECTED;
			if (addr && addrlen)
				if (long r = n_socket->address(Socket::REMOTE, addr, addrlen)) {
					{
						Mutex::Guard guard(main().vfs_mutex);
						main().fds[(int)new_fd] = nullptr;
					}
					destroy(main().heap, n_socket);
					result = r;
					return true;
				}
			result = new_fd;
			return true;
		});
		return result;
	});
}


extern "C" long gxd_connect(int fd, void const *addr, uint32_t len)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		if (!addr || len < sizeof(uint16_t)) return -EINVAL;
		Sockaddr_in const &a = *(Sockaddr_in const *)addr;

		/* AF_UNSPEC dissolves the association */
		if (a.family == AF_UNSPEC)
			return ep_call([&] () -> long {
				if (s.state != Socket::CONNECTED) return 0;
				if (long r = s.control(Socket::CONNECT, ";0")) return r;
				s.state = Socket::UNCONNECTED;
				return 0;
			});

		if (a.family != AF_INET)       return -EAFNOSUPPORT;
		if (len < sizeof(Sockaddr_in)) return -EINVAL;

		bool started = false;
		long result  = 0;
		on_ep([&] {
			if (!started) {
				started = true;
				switch (s.state) {
				case Socket::UNCONNECTED: break;
				case Socket::ACCEPT_ONLY:     result = -EINVAL;       return true;
				case Socket::CONNECTED:       result = -EISCONN;      return true;
				case Socket::CONNECT_ABORTED: result = -ECONNABORTED; return true;
				case Socket::CONNECTING:
					result = s.read_ready(Socket::CONNECT) ? s.finish_connect() : -EALREADY;
					return true;
				}
				Addr_string const str = addr_string(a);
				s.state = Socket::CONNECTING;
				if (s.write_file(Socket::CONNECT, str.string(), str.length() - 1) != (long)str.length() - 1) {
					s.state = Socket::UNCONNECTED;
					result = s.error(-ECONNREFUSED);
					return true;
				}
				if (s.proto == Socket::UDP) { s.state = Socket::CONNECTED; return true; }
				if (s.nonblocking()) { result = -EINPROGRESS; return true; }
			}
			/* the connection's outcome */
			if (!s.read_ready(Socket::CONNECT)) return false;
			result = s.finish_connect();
			return true;
		});
		return result;
	});
}


/* send and recv on a stream socket of a pair: its write and read */
static bool stream_socket(int fd)
{
	Mutex::Guard guard(main().vfs_mutex);
	Fd_object *obj = lookup(fd);
	return obj && obj->stream_socket();
}


extern "C" long gxd_sendto(int fd, void const *buf, unsigned long len, int flags,
                           void const *dest, uint32_t destlen)
{
	if (stream_socket(fd)) return dest ? -EISCONN : gxd_write(fd, buf, len);

	return with_socket(fd, [&] (Socket &s) { return s.send(buf, len, flags, dest, destlen); });
}


extern "C" long gxd_recvfrom(int fd, void *buf, unsigned long len, int flags,
                             void *addr, uint32_t *addrlen)
{
	if (stream_socket(fd)) {
		if (addrlen) *addrlen = 0;   /* an unnamed peer */
		return gxd_read(fd, buf, len);
	}

	return with_socket(fd, [&] (Socket &s) { return s.recv(buf, len, flags, addr, addrlen); });
}


extern "C" long gxd_getsockname(int fd, void *addr, uint32_t *addrlen)
{
	return with_socket(fd, [&] (Socket &s) {
		return ep_call([&] { return s.address(Socket::LOCAL, addr, addrlen); }); });
}


extern "C" long gxd_getpeername(int fd, void *addr, uint32_t *addrlen)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		if (s.proto == Socket::UDP && s.state != Socket::CONNECTED) return -ENOTCONN;
		return ep_call([&] { return s.address(Socket::REMOTE, addr, addrlen); }); });
}


/* socket options the IP stack keeps: files of '<socket>/sockopts' */
static char const *sockopt_file(int level, int name)
{
	if (level == SOL_SOCKET && name == SO_REUSEADDR)  return "so_reuseaddr";
	if (level == SOL_SOCKET && name == SO_KEEPALIVE)  return "so_keepalive";
	if (level == IPPROTO_TCP && name == TCP_KEEPCNT)   return "tcp_keepcnt";
	if (level == IPPROTO_TCP && name == TCP_KEEPIDLE)  return "tcp_keepidle";
	if (level == IPPROTO_TCP && name == TCP_KEEPINTVL) return "tcp_keepintvl";
	return nullptr;
}


template <typename FN>
static long with_sockopt(Socket &s, int level, int name, FN const &fn)
{
	char const *file = sockopt_file(level, name);
	if (!file) return -ENOPROTOOPT;
	Abs_path const path(s.dir, "/sockopts/", Cstring(file));
	Vfs::Vfs_handle *h = nullptr;
	if (main().vfs.root_dir().open(path.string(), Vfs::Directory_service::OPEN_MODE_RDWR,
	                               &h, main().heap) != Vfs::Directory_service::OPEN_OK)
		return -ENOPROTOOPT;
	long const r = fn(*h);
	h->ds().close(h);
	return r;
}


extern "C" long gxd_getsockopt(int fd, int level, int name, void *val, uint32_t *len)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		if (!val || !len) return -EFAULT;
		if (*len < sizeof(int)) return -EINVAL;
		return ep_call([&] () -> long {

		if (level == SOL_SOCKET && name == SO_ERROR) {
			*(int *)val = (s.state == Socket::CONNECTING) ? -(int)s.finish_connect() : 0;
			*len = sizeof(int);
			return 0;
		}
		if (level == SOL_SOCKET && name == SO_TYPE) {
			*(int *)val = s.proto == Socket::TCP ? SOCK_STREAM : SOCK_DGRAM;
			*len = sizeof(int);
			return 0;
		}
		return with_sockopt(s, level, name, [&] (Vfs::Vfs_handle &h) -> long {
			long const n = read_handle(h, val, min((size_t)*len, sizeof(long)), true);
			if (n <= 0) return -ENOPROTOOPT;
			*len = (uint32_t)n;
			return 0;
		});
		});
	});
}


extern "C" long gxd_setsockopt(int fd, int level, int name, void const *val, uint32_t len)
{
	return with_socket(fd, [&] (Socket &s) -> long {
		if (!val) return -EFAULT;
		if (len < sizeof(int)) return -EINVAL;

		/* emulated, as with Genode's libc */
		if (level == SOL_SOCKET && name == SO_LINGER) return 0;
		if (level == IPPROTO_TCP && name == TCP_NODELAY) return 0;

		return ep_call([&] {
			return with_sockopt(s, level, name, [&] (Vfs::Vfs_handle &h) -> long {
				size_t n = 0;
				auto res = h.fs().write(&h, Const_byte_range_ptr((char const *)val,
				                        min((size_t)len, sizeof(long))), n);
				main().vfs.io().commit();
				return (res == Vfs::File_io_service::WRITE_OK && n) ? 0 : -ENOPROTOOPT;
			});
		});
	});
}


extern "C" long gxd_shutdown(int fd, int how)
{
	/* as with Genode's libc: accepted, the connection ends with close */
	if (how < SHUT_RD || how > SHUT_RDWR) return -EINVAL;
	return with_socket(fd, [&] (Socket &) { return 0L; });
}


/*
 * /etc/resolv.conf where the VFS has none, for relibc's resolver: the
 * nameserver as Genode's libc finds it, in the file '+ libc |
 * nameserver_file: ...' names, else '<socket fs>/nameserver', which the IP
 * stack fills (from DHCP, say) with an address (on the entrypoint)
 */
bool Gxd::resolv_conf(Resolv_conf &out)
{
	Libc_path const file = libc_config("nameserver_file");
	Abs_path  const root = socket_root();
	Abs_path  const path = (file.length() > 1) ? Abs_path(file.string())
	                     : (root.length() > 1) ? Abs_path(root, "/nameserver") : Abs_path();
	if (path.length() <= 1) return false;

	char buf[64];
	long n = read_vfs_file(path.string(), buf, sizeof(buf) - 1);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == 0)) n--;
	if (n <= 0) return false;
	buf[n] = 0;
	out = Resolv_conf("nameserver ", Cstring(buf), "\n");
	return true;
}
