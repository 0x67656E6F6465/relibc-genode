/*
 * \brief  File descriptors of the Genode shim of relibc
 *
 * An fd refers to an 'Fd_object' (shared by dup'ed fds, reference-counted):
 * standard input (empty), standard output and error (the LOG), a file or a
 * directory of the component's VFS, or an end of a pipe. 'vfs_mutex' guards
 * the table, briefly. The VFS objects work on the entrypoint (ep.cc), the
 * blocking ones step by step, so that it never blocks for them; pipes have
 * their own lock and block through the shim's futexes.
 */

#include "fd.h"

using namespace Gxd;

namespace {

struct Stdin : Fd_object
{
	Stdin() : Fd_object(O_RDONLY) { }
	long read(void *, size_t) override { return 0; } /* no input: end of file */
	long stat(gxd_stat &st) override
	{
		st = { .mode = S_IFCHR | 0440, .ino = 0, .size = 0, .blksize = 1, .mtime = 0 };
		return 0;
	}
};


struct Stdout : Fd_object
{
	Stdout() : Fd_object(O_WRONLY) { }
	long write(void const *buf, size_t len) override
	{
		main().out.write((char const *)buf, len);
		return len;
	}
	long stat(gxd_stat &st) override
	{
		st = { .mode = S_IFCHR | 0220, .ino = 0, .size = 0,
		       .blksize = Log_session::MAX_STRING_LEN, .mtime = 0 };
		return 0;
	}
	long sync() override { main().out.flush(); return 0; }
};


struct Vfs_file : Fd_object
{
	Vfs::Vfs_handle &handle;
	Abs_path      const  _path;

	Vfs_file(Vfs::Vfs_handle &handle, Abs_path const &path, int oflag)
	: Fd_object(oflag), handle(handle), _path(path) { }

	~Vfs_file() override { handle.ds().close(&handle); }

	bool ep_bound() const override { return true; }
	Abs_path const *path() const override { return &_path; }
	long long position() const override { return (long long)handle.seek(); }

	bool nonblocking() const { return oflag & O_NONBLOCK; }

	/* until data, the end, or (non-blocking) none for now */
	long read(void *buf, size_t len) override
	{
		if ((oflag & O_ACCMODE) == O_WRONLY) return -EBADF;

		bool queued = false;
		long result = 0;
		on_ep([&] {
			using Result = Vfs::File_io_service::Read_result;
			if (!queued) {
				if (nonblocking() && !handle.fs().read_ready(handle)) {
					handle.fs().notify_read_ready(&handle);
					result = -EAGAIN;
					return true;
				}
				if (!handle.fs().queue_read(&handle, len)) return false;
				queued = true;
			}
			size_t n = 0;
			Result const res = handle.fs().complete_read(&handle, Byte_range_ptr((char *)buf, len), n);
			switch (res) {
			case Result::READ_QUEUED: return false;
			case Result::READ_OK:     handle.advance_seek(n); result = n; return true;
			case Result::READ_ERR_WOULD_BLOCK:
				if (nonblocking()) { result = -EAGAIN; return true; }
				queued = false;   /* again when the VFS reports data */
				handle.fs().notify_read_ready(&handle);
				return false;
			default: result = -EIO; return true;
			}
		});
		return result;
	}

	/* everything, or (non-blocking) what fits for now */
	long write(void const *buf, size_t len) override
	{
		if ((oflag & O_ACCMODE) == O_RDONLY) return -EBADF;

		bool   started = false;
		size_t total   = 0;
		long   result  = 0;
		on_ep([&] {
			using Result = Vfs::File_io_service::Write_result;
			if (!started) {
				started = true;
				if (oflag & O_APPEND) {
					Vfs::Directory_service::Stat s { };
					if (main().vfs.root_dir().stat(_path.string(), s) == Vfs::Directory_service::STAT_OK)
						handle.seek(s.size);
				}
			}
			while (total < len) {
				size_t n = 0;
				Result const res = handle.fs().write(&handle,
					Const_byte_range_ptr((char const *)buf + total, len - total), n);
				if (res == Result::WRITE_ERR_WOULD_BLOCK) {
					main().vfs.io().commit();
					if (nonblocking()) { result = total ? (long)total : -EAGAIN; return true; }
					return false;   /* again on progress */
				}
				if (res != Result::WRITE_OK) { result = total ? (long)total : -EIO; return true; }
				handle.advance_seek(n);
				total += n;

				/* stalled (a full pipe): as WOULD_BLOCK */
				if (n == 0) {
					main().vfs.io().commit();
					if (nonblocking()) { result = total ? (long)total : -EAGAIN; return true; }
					return false;
				}
			}
			main().vfs.io().commit();
			result = total;
			return true;
		});
		return result;
	}

	long seek(long long off, int whence) override
	{
		return ep_call([&] () -> long {
			long long base = 0;
			switch (whence) {
			case SEEK_SET: break;
			case SEEK_CUR: base = handle.seek(); break;
			case SEEK_END: {
				Vfs::Directory_service::Stat s { };
				if (main().vfs.root_dir().stat(_path.string(), s) != Vfs::Directory_service::STAT_OK)
					return -EIO;
				base = s.size;
				break; }
			default: return -EINVAL;
			}
			if (base + off < 0) return -EINVAL;
			handle.seek(base + off);
			return base + off;
		});
	}

	long stat(gxd_stat &st) override
	{
		return ep_call([&] { return stat_path(_path.string(), st); });
	}

	/* on the entrypoint (epoll) */
	unsigned poll() override
	{
		return (handle.fs().read_ready(handle)  ? 0x1 : 0)
		     | (handle.fs().write_ready(handle) ? 0x4 : 0);
	}

	void notify_ready() override { handle.fs().notify_read_ready(&handle); }

	long truncate(long long len) override
	{
		if (len < 0) return -EINVAL;
		return ep_call([&] () -> long {
			return handle.fs().ftruncate(&handle, len) == Vfs::File_io_service::FTRUNCATE_OK
			       ? 0 : -EIO; });
	}

	long sync() override
	{
		bool queued = false;
		on_ep([&] {
			if (!queued) {
				if (!handle.fs().queue_sync(&handle)) return false;
				queued = true;
			}
			return handle.fs().complete_sync(&handle) != Vfs::File_io_service::SYNC_QUEUED;
		});
		return 0;
	}
};


/**
 * A read-only file of the shim's own (/etc/resolv.conf)
 */
struct Memory_file : Fd_object
{
	Resolv_conf const content;
	size_t            pos = 0;

	Memory_file(Resolv_conf const &content) : Fd_object(O_RDONLY), content(content) { }

	long read(void *buf, size_t len) override
	{
		size_t const size = content.length() - 1;
		size_t const n = (pos < size) ? min(len, size - pos) : 0;
		memcpy(buf, content.string() + pos, n);
		pos += n;
		return n;
	}

	long seek(long long off, int whence) override
	{
		long long const base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? (long long)pos
		                     : (whence == SEEK_END) ? (long long)content.length() - 1 : -1;
		if (base < 0 || base + off < 0) return -EINVAL;
		pos = (size_t)(base + off);
		return pos;
	}

	long stat(gxd_stat &st) override
	{
		st = { .mode = S_IFREG | 0444, .ino = 0, .size = (int64_t)content.length() - 1,
		       .blksize = 4096, .mtime = 0 };
		return 0;
	}
};


/**
 * A directory: its entries read as Linux 'dirent64' records, the seek
 * position counts entries (it is the 'd_off' relibc hands back)
 */
struct Vfs_dir : Fd_object
{
	using Dirent      = Vfs::Directory_service::Dirent;
	using Dirent_type = Vfs::Directory_service::Dirent_type;

	Vfs::Vfs_handle &handle;
	Abs_path      const  _path;
	long long        index = 0;

	Vfs_dir(Vfs::Vfs_handle &handle, Abs_path const &path)
	: Fd_object(O_RDONLY | O_DIRECTORY), handle(handle), _path(path) { }

	~Vfs_dir() override { handle.ds().close(&handle); }

	bool ep_bound() const override { return true; }
	Abs_path const *path() const override { return &_path; }

	long stat(gxd_stat &st) override
	{
		return ep_call([&] { return stat_path(_path.string(), st); });
	}

	long seek(long long off, int whence) override
	{
		if (whence == SEEK_CUR) off += index;
		else if (whence != SEEK_SET) return -EINVAL;
		if (off < 0) return -EINVAL;
		index = off;
		return off;
	}

	bool _next(Dirent &d)
	{
		handle.seek(index*sizeof(Dirent));
		while (!handle.fs().queue_read(&handle, sizeof(Dirent))) main().wait_io();
		for (;;) {
			size_t n = 0;
			auto res = handle.fs().complete_read(&handle,
				Byte_range_ptr((char *)&d, sizeof(Dirent)), n);
			if (res == Vfs::File_io_service::READ_QUEUED) { main().wait_io(); continue; }
			if (res != Vfs::File_io_service::READ_OK || n < sizeof(Dirent)) return false;
			d.sanitize();
			return d.type != Dirent_type::END;
		}
	}

	long getdents(void *buf, size_t len) override
	{
		return ep_call([&] { return _getdents(buf, len); });
	}

	/* on the entrypoint (directory reads are short: they may wait there) */
	long _getdents(void *buf, size_t len)
	{
		enum { DT_UNKNOWN = 0, DT_CHR = 2, DT_DIR = 4, DT_REG = 8, DT_LNK = 10 };

		size_t used = 0;
		for (;;) {
			long long const start = index;
			Dirent d { };
			if (!_next(d)) break;

			size_t const name_len = strlen(d.name.buf);
			size_t const reclen   = (19 + name_len + 1 + 7) & ~(size_t)7;  /* 8-byte aligned */
			if (used + reclen > len) {
				index = start;
				if (used == 0) return -EINVAL;
				break;
			}

			char *rec = (char *)buf + used;
			*(uint64_t *)(rec + 0)  = d.fileno ? d.fileno : (uint64_t)start + 1;
			*(int64_t  *)(rec + 8)  = start + 1;   /* d_off: where the next one is */
			*(uint16_t *)(rec + 16) = (uint16_t)reclen;
			rec[18] = d.type == Dirent_type::DIRECTORY          ? DT_DIR
			        : d.type == Dirent_type::CONTINUOUS_FILE    ? DT_REG
			        : d.type == Dirent_type::TRANSACTIONAL_FILE ? DT_CHR
			        : d.type == Dirent_type::SYMLINK            ? DT_LNK : DT_UNKNOWN;
			copy_cstring(rec + 19, d.name.buf, name_len + 1);
			for (size_t i = 19 + name_len + 1; i < reclen; i++) rec[i] = 0;

			used += reclen;
			index = start + 1;
		}
		return used;
	}
};


/**
 * A pipe: a ring buffer, its ends counted. Waiting goes through the shim's
 * futexes on 'seq', which every change increments.
 */
struct Pipe
{
	enum { SIZE = 64*1024 };

	Mutex    mutex { };
	char     buf[SIZE];
	size_t   head = 0, count = 0;
	unsigned readers = 1, writers = 1;
	uint32_t seq = 0;

	void changed()
	{
		__atomic_add_fetch(&seq, 1, __ATOMIC_SEQ_CST);
		gxd_futex_wake(&seq, ~0u);
		io_changed();
	}
};


struct Pipe_end : Fd_object
{
	Pipe &pipe;
	bool  write_end;

	Pipe_end(Pipe &pipe, bool write_end, int oflag)
	: Fd_object((write_end ? O_WRONLY : O_RDONLY) | (oflag & O_NONBLOCK)),
	  pipe(pipe), write_end(write_end) { }

	~Pipe_end() override
	{
		bool last;
		{
			Mutex::Guard guard(pipe.mutex);
			(write_end ? pipe.writers : pipe.readers)--;
			last = !pipe.readers && !pipe.writers;
		}
		if (last) destroy(main().heap, &pipe);
		else      pipe.changed();
	}

	long read(void *dst, size_t len) override
	{
		if (write_end) return -EBADF;
		for (;;) {
			uint32_t seq;
			{
				Mutex::Guard guard(pipe.mutex);
				if (pipe.count) {
					size_t n = min(len, pipe.count);
					for (size_t i = 0; i < n; i++)
						((char *)dst)[i] = pipe.buf[(pipe.head + i) % Pipe::SIZE];
					pipe.head = (pipe.head + n) % Pipe::SIZE;
					pipe.count -= n;
					pipe.changed();
					return n;
				}
				if (!pipe.writers) return 0;
				if (oflag & O_NONBLOCK) return -EAGAIN;
				seq = pipe.seq;
			}
			gxd_futex_wait(&pipe.seq, seq, -1);
		}
	}

	long write(void const *src, size_t len) override
	{
		if (!write_end) return -EBADF;
		size_t total = 0;
		while (total < len) {
			uint32_t seq;
			{
				Mutex::Guard guard(pipe.mutex);
				if (!pipe.readers) return total ? (long)total : -EPIPE;
				size_t const room = Pipe::SIZE - pipe.count;
				if (room) {
					size_t n = min(len - total, room);
					for (size_t i = 0; i < n; i++)
						pipe.buf[(pipe.head + pipe.count + i) % Pipe::SIZE] = ((char const *)src)[total + i];
					pipe.count += n;
					total += n;
					pipe.changed();
					continue;
				}
				if (oflag & O_NONBLOCK) return total ? (long)total : -EAGAIN;
				seq = pipe.seq;
			}
			gxd_futex_wait(&pipe.seq, seq, -1);
		}
		return total;
	}

	long stat(gxd_stat &st) override
	{
		st = { .mode = S_IFIFO | 0600, .ino = 0, .size = 0, .blksize = Pipe::SIZE, .mtime = 0 };
		return 0;
	}

	unsigned poll() override
	{
		enum { IN = 0x1, OUT = 0x4, ERR = 0x8, HUP = 0x10 };
		Mutex::Guard guard(pipe.mutex);
		if (write_end) return !pipe.readers ? ERR : (pipe.count < Pipe::SIZE ? OUT : 0);
		return (pipe.count ? IN : 0) | (!pipe.writers ? HUP : 0);
	}
};


/**
 * An end of a socket pair (AF_UNIX, SOCK_STREAM): two pipes, crossed
 */
struct Stream_end : Fd_object
{
	Pipe_end _in, _out;

	Stream_end(Pipe &in, Pipe &out, int oflag)
	: Fd_object(O_RDWR | (oflag & O_NONBLOCK)), _in(in, false, 0), _out(out, true, 0) { }

	/* fcntl changes the flags of this object, the pipe ends follow */
	void _sync()
	{
		_in.oflag  = (_in.oflag  & ~O_NONBLOCK) | (oflag & O_NONBLOCK);
		_out.oflag = (_out.oflag & ~O_NONBLOCK) | (oflag & O_NONBLOCK);
	}

	bool stream_socket() const override { return true; }

	long read(void *buf, size_t len) override        { _sync(); return _in.read(buf, len); }
	long write(void const *buf, size_t len) override { _sync(); return _out.write(buf, len); }

	long stat(gxd_stat &st) override
	{
		enum { S_IFSOCK = 0140000 };
		st = { .mode = S_IFSOCK | 0600, .ino = 0, .size = 0, .blksize = Pipe::SIZE, .mtime = 0 };
		return 0;
	}

	unsigned poll() override
	{
		enum { IN = 0x1, OUT = 0x4, ERR = 0x8, HUP = 0x10 };
		return (_in.poll() & (IN | HUP)) | (_out.poll() & (OUT | ERR));
	}
};


} /* anonymous namespace */


Fd_object *Gxd::lookup(int fd)
{
	if (fd < 0 || fd >= (int)Main::MAX_FDS) return nullptr;
	return main().fds[fd];
}

int Gxd::install(Fd_object &obj, int min_fd, bool cloexec)
{
	for (unsigned i = max(min_fd, 0); i < Main::MAX_FDS; i++)
		if (!main().fds[i]) {
			main().fds[i]     = &obj;
			main().cloexec[i] = cloexec;
			return i;
		}
	return -EMFILE;
}

bool Gxd::unref(Fd_object &obj) { return --obj.refs == 0; }

void Gxd::dispose(Fd_object &obj)
{
	if (obj.ep_bound()) ep_call([&] { destroy(main().heap, &obj); return 0L; });
	else                destroy(main().heap, &obj);
}

void Gxd::release(Fd_object &obj)
{
	bool last;
	{
		Mutex::Guard guard(main().vfs_mutex);
		last = unref(obj);
	}
	if (last) dispose(obj);
}

void Gxd::io_changed()
{
	__atomic_add_fetch(&main().io_seq, 1, __ATOMIC_SEQ_CST);
	gxd_futex_wake(&main().io_seq, ~0u);
	examine_ep_jobs();   /* epoll waits look again */
}


namespace {

/**
 * Call 'fn' with the fd's object, a reference held (without vfs_mutex)
 */
template <typename FN>
long with_fd(int fd, FN const &fn)
{
	Fd_object *obj;
	{
		Mutex::Guard guard(main().vfs_mutex);
		obj = lookup(fd);
		if (!obj) return -EBADF;
		obj->refs++;  /* keeps it alive while it blocks */
	}
	long const ret = fn(*obj);
	release(*obj);
	return ret;
}

} /* anonymous namespace */


namespace Gxd {

	/*
	 * Standard I/O: the files the config's '+ libc' node names (stdin,
	 * stdout, stderr), as with Genode's libc, else no input and the LOG.
	 * The working directory: its 'cwd'.
	 */
	void init_stdio()
	{
		Main &m = main();
		char const * const attr[3] = { "stdin", "stdout", "stderr" };
		for (int i = 0; i < 3; i++) {
			Libc_path const path = libc_config(attr[i]);
			if (path.length() > 1
			 && gxd_openat(AT_FDCWD, path.string(), i ? O_WRONLY : O_RDONLY, 0) == i)
				continue;
			if (path.length() > 1)
				warning("relibc: cannot open ", path, " as ", Cstring(attr[i]));
			m.fds[i] = i ? (Fd_object *)new (m.heap) Stdout()
			             : (Fd_object *)new (m.heap) Stdin();
		}

		Libc_path const cwd = libc_config("cwd");
		if (cwd.length() > 1) {
			/* at start-up, on the entrypoint */
			Abs_path abs;
			if (resolve(AT_FDCWD, cwd.string(), abs) == 0
			 && m.vfs.root_dir().directory(abs.string()))
				m.cwd = abs;
			else
				warning("relibc: no directory ", cwd, " for cwd");
		}
	}

	/*
	 * The fds the config names, '+ libc | + fd | id: 3 | path: /file |
	 * readable: yes | seek: 5' as with Genode's libc (with 'writeable',
	 * 'append', 'nonblocking', 'directory', 'cloexec'), and those of a
	 * forked child's parent
	 */
	void init_fds()
	{
		using Path = String<Vfs::MAX_PATH_LEN>;
		main().config.node().with_optional_sub_node("libc", [&] (Node const &libc) {
			libc.for_each_sub_node("fd", [&] (Node const &node) {
				int  const id   = node.attribute_value("id", -1);
				Path const path = node.attribute_value("path", Path());
				if (id < 0 || id >= (int)Main::MAX_FDS || path.length() <= 1) {
					warning("relibc: an fd of the config without 'id' or 'path'");
					return;
				}
				bool const r = node.attribute_value("readable",  false);
				bool const w = node.attribute_value("writeable", false);
				int oflag = (r && w) ? O_RDWR : w ? O_WRONLY : O_RDONLY;
				if (node.attribute_value("directory",   false)) oflag |= O_DIRECTORY;
				if (node.attribute_value("append",      false)) oflag |= O_APPEND;
				if (node.attribute_value("nonblocking", false)) oflag |= O_NONBLOCK;

				long const fd = gxd_openat(AT_FDCWD, path.string(), oflag, 0);
				if (fd < 0) { warning("relibc: cannot open ", path, " as fd ", id); return; }
				if (fd != id) {
					gxd_dup2((int)fd, id);
					gxd_close((int)fd);
				}
				if (unsigned long const seek = node.attribute_value("seek", 0UL))
					gxd_lseek(id, (long long)seek, SEEK_SET);
				if (node.attribute_value("cloexec", false))
					gxd_fcntl_setfd(id, 1);
			});
		});
	}

	void sync_fds()
	{
		for (int fd = 0; fd < (int)Main::MAX_FDS; fd++)
			gxd_fsync(fd);
	}

	/* the path of a directory fd, for paths relative to it (fs.cc) */
	long dir_path(int fd, Abs_path &out)
	{
		Fd_object *obj = lookup(fd);
		if (!obj) return -EBADF;
		if (!(obj->oflag & O_DIRECTORY) || !obj->path()) return -ENOTDIR;
		out = *obj->path();
		return 0;
	}
}


extern "C" long gxd_write(int fd, void const *buf, unsigned long len)
{
	return with_fd(fd, [&] (Fd_object &o) { return o.write(buf, len); });
}


extern "C" long gxd_read(int fd, void *buf, unsigned long len)
{
	return with_fd(fd, [&] (Fd_object &o) { return o.read(buf, len); });
}


/* at an offset, the file position unchanged (pipes: ESPIPE) */
template <typename FN>
static long at_offset(Fd_object &o, long long off, FN const &fn)
{
	if (off < 0) return -EINVAL;
	long const pos = o.seek(0, SEEK_CUR);
	if (pos < 0) return pos;
	if (long r = o.seek(off, SEEK_SET); r < 0) return r;
	long const n = fn();
	o.seek(pos, SEEK_SET);
	return n;
}


extern "C" long gxd_pread(int fd, void *buf, unsigned long len, long long off)
{
	return with_fd(fd, [&] (Fd_object &o) {
		return at_offset(o, off, [&] { return o.read(buf, len); }); });
}


extern "C" long gxd_pwrite(int fd, void const *buf, unsigned long len, long long off)
{
	return with_fd(fd, [&] (Fd_object &o) {
		/* O_APPEND would move the write to the end */
		int const oflag = o.oflag;
		o.oflag &= ~O_APPEND;
		long const n = at_offset(o, off, [&] { return o.write(buf, len); });
		o.oflag = oflag;
		return n; });
}


extern "C" long gxd_lseek(int fd, long long off, int whence)
{
	return with_fd(fd, [&] (Fd_object &o) { return o.seek(off, whence); });
}


extern "C" long gxd_ftruncate(int fd, long long len)
{
	return with_fd(fd, [&] (Fd_object &o) { return o.truncate(len); });
}


extern "C" long gxd_fsync(int fd)
{
	return with_fd(fd, [&] (Fd_object &o) { return o.sync(); });
}


extern "C" long gxd_getdents(int fd, void *buf, unsigned long len)
{
	return with_fd(fd, [&] (Fd_object &o) { return o.getdents(buf, len); });
}


extern "C" long gxd_fcntl_getfl(int fd)
{
	return with_fd(fd, [&] (Fd_object &o) { return (long)(o.oflag & ~O_DIRECTORY); });
}


/**
 * The info node the VFS offers for a file (<dir>/.<name>/info), as Genode's
 * libc reads it for ioctls: e.g., a terminal's size (on the entrypoint).
 */
template <typename FN>
static void with_info(Abs_path const &path, FN const &fn)
{
	char const *s = path.string();
	size_t slash = 0;
	for (size_t i = 0; s[i]; i++) if (s[i] == '/') slash = i;
	Abs_path const info(Cstring(s, slash), "/.", s + slash + 1, "/info");

	char buf[4096];
	long const n = read_vfs_file(info.string(), buf, sizeof(buf));
	if (n > 0) fn(Node(Const_byte_range_ptr(buf, n)));
}


/**
 * Terminal ioctls (Linux's, as relibc issues them) on a VFS file that is a
 * terminal by its info node: its size, and terminal attributes as Genode's
 * libc reports them (echo on, no other modes; setting them is ignored).
 * Everything else is no terminal (ENOTTY), so isatty() tells.
 */
static long ioctl_terminal(Fd_object &o, unsigned long request, void *arg)
{
	enum : unsigned long { TCGETS = 0x5401, TIOCGWINSZ = 0x5413 };
	enum { NCCS = 32, ECHO = 0x8 };

	struct termios                  /* relibc's (Linux ABI) */
	{
		uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
		uint8_t  c_line;
		uint8_t  c_cc[NCCS];
		uint32_t c_ispeed, c_ospeed;
	};

	struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

	Abs_path const *path = o.path();
	if (!path) return -ENOTTY;

	bool     terminal = false;
	unsigned rows = 25, columns = 80;
	ep_call([&] {
		with_info(*path, [&] (Node const &info) {
			if (info.type() != "terminal") return;
			terminal = true;
			rows     = info.attribute_value("rows",    rows);
			columns  = info.attribute_value("columns", columns);
		});
		return 0L;
	});
	if (!terminal) return -ENOTTY;
	if (!arg)      return -EINVAL;

	if (request == TIOCGWINSZ) {
		*(winsize *)arg = { .ws_row = (uint16_t)rows, .ws_col = (uint16_t)columns,
		                    .ws_xpixel = 0, .ws_ypixel = 0 };
	} else if (request == TCGETS) {
		termios &t = *(termios *)arg;
		t = { };
		t.c_lflag = ECHO;   /* readline (bash) needs it to echo */
	}
	return 0;
}


/* the fd's flags (F_GETFD, F_SETFD): FD_CLOEXEC */
extern "C" long gxd_fcntl_getfd(int fd)
{
	enum { FD_CLOEXEC = 1 };
	Mutex::Guard guard(main().vfs_mutex);
	if (!lookup(fd)) return -EBADF;
	return main().cloexec[fd] ? FD_CLOEXEC : 0;
}


extern "C" long gxd_fcntl_setfd(int fd, int flags)
{
	enum { FD_CLOEXEC = 1 };
	Mutex::Guard guard(main().vfs_mutex);
	if (!lookup(fd)) return -EBADF;
	main().cloexec[fd] = flags & FD_CLOEXEC;
	return 0;
}


/* close the fds with FD_CLOEXEC (execve) */
void Gxd::close_cloexec_fds()
{
	for (int fd = 0; fd < (int)Main::MAX_FDS; fd++) {
		bool close_it;
		{
			Mutex::Guard guard(main().vfs_mutex);
			close_it = main().fds[fd] && main().cloexec[fd];
		}
		if (close_it) gxd_close(fd);
	}
}


/* the status flags fcntl(F_SETFL) may change: O_APPEND, O_NONBLOCK */
extern "C" long gxd_fcntl_setfl(int fd, int flags)
{
	return with_fd(fd, [&] (Fd_object &o) {
		o.oflag = (o.oflag & ~(O_APPEND | O_NONBLOCK)) | (flags & (O_APPEND | O_NONBLOCK));
		return 0L; });
}


extern "C" long gxd_ioctl(int fd, unsigned long request, void *arg)
{
	/* FreeBSD's disk ioctls, as Genode's libc offers them (sys/disk.h) */
	enum : unsigned long { DIOCGSECTORSIZE = 0x40046480, DIOCGMEDIASIZE = 0x40086481 };

	/* Linux's terminal ioctls */
	enum : unsigned long { TCGETS = 0x5401, TCSETS = 0x5402, TCSETSW = 0x5403,
	                       TCSETSF = 0x5404, TCFLSH = 0x540B, TIOCGWINSZ = 0x5413 };

	switch (request) {
	case TCGETS: case TCSETS: case TCSETSW: case TCSETSF: case TCFLSH: case TIOCGWINSZ:
		return with_fd(fd, [&] (Fd_object &o) { return ioctl_terminal(o, request, arg); });
	case DIOCGMEDIASIZE:
		return with_fd(fd, [&] (Fd_object &o) {
			gxd_stat st { };
			if (long r = o.stat(st)) return r;
			*(int64_t *)arg = st.size;
			return 0L; });
	case DIOCGSECTORSIZE:
		return with_fd(fd, [&] (Fd_object &) { *(unsigned *)arg = 512; return 0L; });
	default:
		return -ENOTTY;
	}
}


/* a new object into the table (on the entrypoint: destroyed at once if full) */
static long install_new(Fd_object &obj, bool cloexec = false)
{
	int fd;
	{
		Mutex::Guard guard(main().vfs_mutex);
		fd = install(obj, 0, cloexec);
	}
	if (fd < 0) destroy(main().heap, &obj);
	return fd;
}


extern "C" long gxd_openat(int dirfd, char const *path, int oflag, unsigned)
{
	/* on the entrypoint, which owns the VFS */
	return ep_call([&] () -> long {
		Abs_path abs;
		if (long r = resolve(dirfd, path, abs, !(oflag & O_NOFOLLOW))) return r;

		auto &root = main().vfs.root_dir();
		Main &m    = main();

		/* O_NOFOLLOW: the last element must not be a link */
		if (oflag & O_NOFOLLOW) {
			Vfs::Directory_service::Stat st { };
			if (root.stat(abs.string(), st) == Vfs::Directory_service::STAT_OK
			 && st.type == Vfs::Node_type::SYMLINK)
				return -ELOOP;
		}

		if (root.directory(abs.string())) {
			if ((oflag & O_ACCMODE) != O_RDONLY) return -EISDIR;
			Vfs::Vfs_handle *h = nullptr;
			if (root.opendir(abs.string(), false, &h, m.heap) != Vfs::Directory_service::OPENDIR_OK)
				return -EACCES;
			return install_new(*new (m.heap) Vfs_dir(*h, abs), oflag & O_CLOEXEC);
		}
		if (oflag & O_DIRECTORY)
			return root.leaf_path(abs.string()) ? -ENOTDIR : -ENOENT;

		unsigned mode = (oflag & O_ACCMODE) == O_WRONLY ? Vfs::Directory_service::OPEN_MODE_WRONLY
		              : (oflag & O_ACCMODE) == O_RDWR   ? Vfs::Directory_service::OPEN_MODE_RDWR
		              :                                   Vfs::Directory_service::OPEN_MODE_RDONLY;

		bool const exists = root.leaf_path(abs.string()) != nullptr;
		if (exists && (oflag & O_CREAT) && (oflag & O_EXCL)) return -EEXIST;
		if (!exists && (oflag & O_CREAT))
			mode |= Vfs::Directory_service::OPEN_MODE_CREATE;

		/* relibc's resolver reads the nameserver from /etc/resolv.conf */
		if (!exists && !(oflag & O_CREAT) && abs == "/etc/resolv.conf") {
			Resolv_conf content;
			if (resolv_conf(content)) {
				return install_new(*new (m.heap) Memory_file(content), oflag & O_CLOEXEC);
			}
		}

		Vfs::Vfs_handle *h = nullptr;
		switch (root.open(abs.string(), mode, &h, m.heap)) {
		case Vfs::Directory_service::OPEN_OK:             break;
		case Vfs::Directory_service::OPEN_ERR_NO_PERM:    return -EACCES;
		case Vfs::Directory_service::OPEN_ERR_EXISTS:     return -EEXIST;
		case Vfs::Directory_service::OPEN_ERR_NO_SPACE:   return -ENOSPC;
		case Vfs::Directory_service::OPEN_ERR_OUT_OF_RAM: return -ENOMEM;
		case Vfs::Directory_service::OPEN_ERR_NAME_TOO_LONG: return -ENAMETOOLONG;
		default:                                          return -ENOENT;
		}
		if ((oflag & O_TRUNC) && (oflag & O_ACCMODE) != O_RDONLY)
			h->fs().ftruncate(h, 0);

		return install_new(*new (m.heap) Vfs_file(*h, abs, oflag & ~O_CLOEXEC), oflag & O_CLOEXEC);
	});
}


extern "C" long gxd_close(int fd)
{
	Fd_object *obj;
	{
		Mutex::Guard guard(main().vfs_mutex);
		obj = lookup(fd);
		if (!obj) return -EBADF;
		main().fds[fd] = nullptr;
	}
	obj->sync();
	release(*obj);
	io_changed();
	return 0;
}


extern "C" long gxd_fstatat(int dirfd, char const *path, gxd_stat *st, int flags)
{
	if (!path || !path[0])                 /* fstat */
		return with_fd(dirfd, [&] (Fd_object &o) { return o.stat(*st); });

	return ep_call([&] () -> long {
		Abs_path abs;
		if (long r = resolve(dirfd, path, abs, !(flags & AT_SYMLINK_NOFOLLOW))) return r;
		return stat_path(abs.string(), *st);
	});
}


extern "C" long gxd_dup(int fd, int min_fd, int cloexec)
{
	Mutex::Guard guard(main().vfs_mutex);
	Fd_object *obj = lookup(fd);
	if (!obj) return -EBADF;
	int nfd = install(*obj, min_fd, cloexec);
	if (nfd >= 0) obj->refs++;
	return nfd;
}


extern "C" long gxd_dup2(int fd, int fd2)
{
	Fd_object *old = nullptr;
	{
		Mutex::Guard guard(main().vfs_mutex);
		Fd_object *obj = lookup(fd);
		if (!obj) return -EBADF;
		if (fd2 < 0 || fd2 >= (int)Main::MAX_FDS) return -EBADF;
		if (fd == fd2) return fd2;
		old = main().fds[fd2];
		if (old && !unref(*old)) old = nullptr;
		main().fds[fd2]     = obj;
		main().cloexec[fd2] = false;
		obj->refs++;
	}
	if (old) dispose(*old);
	return fd2;
}


/*
 * A pipe of the VFS's pipe plugin ('+ libc | pipe: /pipe', as with
 * Genode's libc): '<pipe>/new' names it, its ends are files ('out', 'in'),
 * which other components (a forked or spawned child) open as well
 */
static long vfs_pipe(Libc_path const &base, int fds[2])
{
	long const meta = gxd_openat(AT_FDCWD, String<160>(base, "/new").string(), O_RDONLY, 0);
	if (meta < 0) {
		warning("relibc: no pipe at ", base, "/new");
		return -EACCES;
	}
	char name[32] { };
	long const n = gxd_read((int)meta, name, sizeof(name) - 1);
	if (n < 1) {
		gxd_close((int)meta);
		return -EACCES;
	}
	if (name[n - 1] == '\n') name[n - 1] = 0;

	Abs_path const dir(base, "/", Cstring(name));
	long const r = gxd_openat(AT_FDCWD, Abs_path(dir, "/out").string(), O_RDONLY, 0);
	long const w = r < 0 ? r : gxd_openat(AT_FDCWD, Abs_path(dir, "/in").string(), O_WRONLY, 0);
	gxd_close((int)meta);
	if (w < 0) {
		if (r >= 0) gxd_close((int)r);
		return w == -EMFILE ? -EMFILE : -EACCES;
	}
	fds[0] = (int)r; fds[1] = (int)w;
	return 0;
}


extern "C" long gxd_pipe(int fds[2])
{
	Libc_path const base = libc_config("pipe");
	if (base.length() > 1)
		return vfs_pipe(base, fds);

	/* within the component */
	Main &m = main();
	Pipe &pipe = *new (m.heap) Pipe();
	Fd_object &r = *new (m.heap) Pipe_end(pipe, false, 0);
	Fd_object &w = *new (m.heap) Pipe_end(pipe, true, 0);
	Mutex::Guard guard(main().vfs_mutex);
	int rfd = install(r);
	int wfd = rfd < 0 ? rfd : install(w);
	if (rfd < 0 || wfd < 0) {
		if (rfd >= 0) m.fds[rfd] = nullptr;
		destroy(m.heap, &r); destroy(m.heap, &w);   /* pipe ends: no VFS */
		return -EMFILE;
	}
	fds[0] = rfd; fds[1] = wfd;
	return 0;
}


/* socketpair: AF_UNIX stream sockets within the component */
extern "C" long gxd_socketpair(int domain, int type, int protocol, int fds[2])
{
	enum { AF_UNIX = 1, SOCK_STREAM = 1, SOCK_NONBLOCK = 04000, EAFNOSUPPORT = 97, EPROTONOSUPPORT = 93 };
	if (domain != AF_UNIX)                       return -EAFNOSUPPORT;
	if ((type & 0xf) != SOCK_STREAM || protocol) return -EPROTONOSUPPORT;
	int const oflag = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;

	Main &m = main();
	Pipe &a = *new (m.heap) Pipe();
	Pipe &b = *new (m.heap) Pipe();
	Fd_object &x = *new (m.heap) Stream_end(a, b, oflag);
	Fd_object &y = *new (m.heap) Stream_end(b, a, oflag);
	Mutex::Guard guard(main().vfs_mutex);
	int xfd = install(x);
	int yfd = xfd < 0 ? xfd : install(y);
	if (xfd < 0 || yfd < 0) {
		if (xfd >= 0) m.fds[xfd] = nullptr;
		destroy(m.heap, &x); destroy(m.heap, &y);
		return -EMFILE;
	}
	fds[0] = xfd; fds[1] = yfd;
	return 0;
}
