/*
 * \brief  Abs_path operations of the Genode shim of relibc
 *
 * Paths are resolved against the working directory (or a directory fd),
 * normalized ('.', '..', repeated '/'), and their symbolic links followed
 * before they reach the VFS, which knows absolute paths only and keeps
 * symbolic links without following them (as Genode's libc does it).
 *
 * The VFS is the entrypoint's: the gxd_* functions run their work there
 * (ep_call), and the helpers here are called there.
 */

#include "state.h"

using namespace Gxd;

namespace Gxd { long dir_path(int fd, Abs_path &out); }  /* fd.cc */


namespace {

/* normalize an absolute path into 'out' */
long normalize(char const *abs, Abs_path &out)
{
	enum { MAX = Abs_path::capacity() };
	char buf[MAX];
	size_t len = 0;
	buf[len++] = '/';

	char const *p = abs;
	while (*p) {
		while (*p == '/') p++;
		char const *start = p;
		while (*p && *p != '/') p++;
		size_t const n = p - start;
		if (n == 0 || (n == 1 && start[0] == '.')) continue;
		if (n == 2 && start[0] == '.' && start[1] == '.') {
			/* drop the last element, and the '/' before it unless it is the root */
			while (len > 1 && buf[len - 1] != '/') len--;
			if (len > 1) len--;
			continue;
		}
		if (len + n + 2 > MAX) return -ENAMETOOLONG;
		if (len > 1) buf[len++] = '/';
		for (size_t i = 0; i < n; i++) buf[len++] = start[i];
	}
	buf[len] = 0;
	out = Abs_path(Cstring(buf));
	return 0;
}

} /* anonymous namespace */


/* 'once': symbolic links ignore the seek position */
long Gxd::read_handle(Vfs::Vfs_handle &h, void *buf, size_t len, bool once)
{
	Main &m = main();
	size_t got = 0;
	while (got < len) {
		while (!h.fs().queue_read(&h, len - got)) m.wait_io();
		size_t n = 0;
		Vfs::File_io_service::Read_result res;
		while ((res = h.fs().complete_read(&h, Byte_range_ptr((char *)buf + got, len - got), n))
		       == Vfs::File_io_service::READ_QUEUED)
			m.wait_io();
		if (res != Vfs::File_io_service::READ_OK) return -EIO;
		if (n == 0) break;
		got += n;
		h.advance_seek(n);
		if (once) break;
	}
	return got;
}


/* the target of the symbolic link at 'abs': its length, or -errno */
static long read_link(char const *abs, char *buf, size_t len)
{
	Vfs::Vfs_handle *h = nullptr;
	if (main().vfs.root_dir().openlink(abs, false, &h, main().heap)
	    != Vfs::Directory_service::OPENLINK_OK)
		return -EINVAL;
	long n = read_handle(*h, buf, len, true);
	h->ds().close(h);

	/* a file-system session hands the target over with its terminating 0 */
	for (long i = 0; i < n; i++) if (!buf[i]) { n = i; break; }
	return n;
}


static bool is_symlink(char const *abs)
{
	Vfs::Directory_service::Stat st { };
	return main().vfs.root_dir().stat(abs, st) == Vfs::Directory_service::STAT_OK
	    && st.type == Vfs::Node_type::SYMLINK;
}


/* follow the symbolic links of the normalized path 'abs' */
static long follow_links(Abs_path const &abs, Abs_path &out, bool follow_last)
{
	enum { FOLLOW_LIMIT = 10, MAX = Abs_path::capacity() };

	Abs_path path = abs;
	for (unsigned follows = 0; ; ) {
		char const * const s = path.string();
		bool followed = false;

		/* each prefix that ends an element, the whole path last */
		for (size_t i = 1; ; i++) {
			if (s[i] != '/' && s[i] != 0) continue;
			bool const last = (s[i] == 0);
			if (last && !follow_last) break;

			Abs_path const prefix(Cstring(s, i));
			if (is_symlink(prefix.string())) {
				if (++follows > FOLLOW_LIMIT) return -ELOOP;

				char target[MAX];
				long const n = read_link(prefix.string(), target, sizeof(target) - 1);
				if (n < 0) return n;
				target[n] = 0;

				/* relative targets are relative to the link's directory */
				size_t dir_len = 0;
				if (target[0] != '/')
					for (size_t j = 0; j < i; j++) if (s[j] == '/') dir_len = j;

				if (dir_len + 1 + (size_t)n + strlen(s + i) + 1 > MAX) return -ENAMETOOLONG;
				char joined[MAX];
				size_t len = 0;
				for (size_t j = 0; j < dir_len; j++) joined[len++] = s[j];
				joined[len++] = '/';
				for (long j = 0; j < n; j++) joined[len++] = target[j];
				for (char const *r = s + i; *r; r++) joined[len++] = *r;
				joined[len] = 0;

				Abs_path next;
				if (long r = normalize(joined, next)) return r;
				path = next;
				followed = true;
				break;
			}
			if (last) break;
		}
		if (!followed) { out = path; return 0; }
	}
}


long Gxd::resolve(int dirfd, char const *path, Abs_path &out, bool follow_last)
{
	if (!path || !path[0]) return -ENOENT;

	Abs_path abs;
	if (path[0] == '/') {
		if (long r = normalize(path, abs)) return r;
	} else {
		Abs_path base;
		{
			Mutex::Guard guard(main().vfs_mutex);
			base = main().cwd;
			if (dirfd != AT_FDCWD)
				if (long r = dir_path(dirfd, base)) return r;
		}

		if (base.length() + strlen(path) + 2 > Abs_path::capacity()) return -ENAMETOOLONG;
		if (long r = normalize(Abs_path(base, "/", path).string(), abs)) return r;
	}
	return follow_links(abs, out, follow_last);
}


long Gxd::read_vfs_file(char const *abs, void *buf, size_t len)
{
	Vfs::Vfs_handle *h = nullptr;
	if (main().vfs.root_dir().open(abs, Vfs::Directory_service::OPEN_MODE_RDONLY, &h, main().heap)
	    != Vfs::Directory_service::OPEN_OK)
		return -ENOENT;
	long const n = read_handle(*h, buf, len);
	h->ds().close(h);
	return n;
}


Gxd::Libc_path Gxd::libc_config(char const *attr)
{
	return main().config.node().with_sub_node("libc",
		[&] (Node const &n) { return n.attribute_value(attr, Libc_path()); },
		[&] { return Libc_path(); });
}


long Gxd::stat_path(char const *abs, gxd_stat &st)
{
	Vfs::Directory_service::Stat s { };
	if (main().vfs.root_dir().stat(abs, s) != Vfs::Directory_service::STAT_OK)
		return -ENOENT;

	using Type = Vfs::Node_type;
	unsigned type = s.type == Type::DIRECTORY ? S_IFDIR
	              : s.type == Type::SYMLINK   ? S_IFLNK
	              :                             S_IFREG;
	unsigned perm = (s.rwx.readable ? 0444 : 0) | (s.rwx.writeable ? 0200 : 0)
	              | ((s.rwx.executable || type == S_IFDIR) ? 0111 : 0);
	st = gxd_stat { .mode    = type | perm,
	                .ino     = s.inode,
	                .size    = (int64_t)s.size,
	                .blksize = 4096,
	                .mtime   = (int64_t)(s.modification_time.ms_since_1970 / 1000) };
	return 0;
}


extern "C" long gxd_unlinkat(int dirfd, char const *path, int flags)
{
	return ep_call([&] () -> long {
		Abs_path abs;
		if (long r = resolve(dirfd, path, abs, false)) return r;

		auto &root = main().vfs.root_dir();
		bool const is_dir = root.directory(abs.string());
		if (!is_dir && !root.leaf_path(abs.string())) return -ENOENT;
		if (is_dir  && !(flags & AT_REMOVEDIR)) return -EISDIR;
		if (!is_dir &&  (flags & AT_REMOVEDIR)) return -ENOTDIR;

		/* the VFS would remove a directory with its content */
		if (is_dir && root.num_dirent(abs.string())) return -ENOTEMPTY;

		switch (root.unlink(abs.string())) {
		case Vfs::Directory_service::UNLINK_OK:            return 0;
		case Vfs::Directory_service::UNLINK_ERR_NO_PERM:   return -EACCES;
		case Vfs::Directory_service::UNLINK_ERR_NOT_EMPTY: return -ENOTEMPTY;
		default:                                           return -ENOENT;
		}
	});
}


extern "C" long gxd_mkdirat(int dirfd, char const *path, unsigned)
{
	return ep_call([&] () -> long {
		Abs_path abs;
		if (long r = resolve(dirfd, path, abs, false)) return r;

		auto &root = main().vfs.root_dir();
		if (root.leaf_path(abs.string())) return -EEXIST;

		Vfs::Vfs_handle *h = nullptr;
		switch (root.opendir(abs.string(), true, &h, main().heap)) {
		case Vfs::Directory_service::OPENDIR_OK:
			h->ds().close(h);
			return 0;
		case Vfs::Directory_service::OPENDIR_ERR_NODE_ALREADY_EXISTS: return -EEXIST;
		case Vfs::Directory_service::OPENDIR_ERR_NAME_TOO_LONG:       return -ENAMETOOLONG;
		case Vfs::Directory_service::OPENDIR_ERR_NO_SPACE:            return -ENOSPC;
		case Vfs::Directory_service::OPENDIR_ERR_PERMISSION_DENIED:   return -EACCES;
		default:                                                      return -ENOENT;
		}
	});
}


extern "C" long gxd_renameat(int olddirfd, char const *oldpath, int newdirfd, char const *newpath)
{
	return ep_call([&] () -> long {
		Abs_path from, to;
		if (long r = resolve(olddirfd, oldpath, from, false)) return r;
		if (long r = resolve(newdirfd, newpath, to,   false)) return r;

		auto &root = main().vfs.root_dir();
		if (!root.leaf_path(from.string())) return -ENOENT;
		if (root.directory(to.string())) {
			if (!root.directory(from.string())) return -EISDIR;
			if (root.num_dirent(to.string()))   return -ENOTEMPTY;
		} else if (root.leaf_path(to.string()) && root.directory(from.string())) {
			return -ENOTDIR;
		}

		switch (root.rename(from.string(), to.string())) {
		case Vfs::Directory_service::RENAME_OK:           return 0;
		case Vfs::Directory_service::RENAME_ERR_CROSS_FS: return -EXDEV;
		case Vfs::Directory_service::RENAME_ERR_NO_PERM:  return -EACCES;
		default:                                          return -ENOENT;
		}
	});
}


extern "C" long gxd_faccessat(int dirfd, char const *path, int mode)
{
	return ep_call([&] () -> long {
		enum { R_OK = 4, W_OK = 2, X_OK = 1 };

		Abs_path abs;
		if (long r = resolve(dirfd, path, abs)) return r;
		gxd_stat st { };
		if (long r = stat_path(abs.string(), st)) return r;
		if ((mode & R_OK) && !(st.mode & 0444)) return -EACCES;
		if ((mode & W_OK) && !(st.mode & 0222)) return -EACCES;
		if ((mode & X_OK) && !(st.mode & 0111)) return -EACCES;
		return 0;
	});
}


extern "C" long gxd_chdir(char const *path)
{
	return ep_call([&] () -> long {
		Abs_path abs;
		if (long r = resolve(AT_FDCWD, path, abs)) return r;
		if (!main().vfs.root_dir().directory(abs.string()))
			return main().vfs.root_dir().leaf_path(abs.string()) ? -ENOTDIR : -ENOENT;
		Mutex::Guard guard(main().vfs_mutex);
		main().cwd = abs;
		return 0;
	});
}


extern "C" long gxd_readlinkat(int dirfd, char const *path, char *buf, unsigned long len)
{
	return ep_call([&] () -> long {
		Abs_path abs;
		if (long r = resolve(dirfd, path, abs, false)) return r;
		if (!is_symlink(abs.string()))
			return main().vfs.root_dir().leaf_path(abs.string()) ? -EINVAL : -ENOENT;
		return read_link(abs.string(), buf, len);
	});
}


extern "C" long gxd_symlinkat(char const *target, int dirfd, char const *path)
{
	return ep_call([&] () -> long {
		Main &m = main();
		Abs_path abs;
		if (long r = resolve(dirfd, path, abs, false)) return r;

		auto &root = m.vfs.root_dir();
		if (root.leaf_path(abs.string())) return -EEXIST;

		Vfs::Vfs_handle *h = nullptr;
		switch (root.openlink(abs.string(), true, &h, m.heap)) {
		case Vfs::Directory_service::OPENLINK_OK:                      break;
		case Vfs::Directory_service::OPENLINK_ERR_NODE_ALREADY_EXISTS: return -EEXIST;
		case Vfs::Directory_service::OPENLINK_ERR_NAME_TOO_LONG:       return -ENAMETOOLONG;
		case Vfs::Directory_service::OPENLINK_ERR_PERMISSION_DENIED:   return -EPERM;
		case Vfs::Directory_service::OPENLINK_ERR_LOOKUP_FAILED:       return -ENOENT;
		default:                                                       return -ENOSPC;
		}

		size_t const len = strlen(target);
		size_t n = 0;
		long ret = 0;
		for (;;) {
			auto res = h->fs().write(h, Const_byte_range_ptr(target, len), n);
			if (res == Vfs::File_io_service::WRITE_ERR_WOULD_BLOCK) { m.wait_io(); continue; }
			if (res != Vfs::File_io_service::WRITE_OK || n != len) ret = -EIO;
			break;
		}
		while (!h->fs().queue_sync(h)) m.wait_io();
		while (h->fs().complete_sync(h) == Vfs::File_io_service::SYNC_QUEUED) m.wait_io();
		h->ds().close(h);
		return ret;
	});
}


extern "C" long gxd_fchdir(int fd)
{
	Mutex::Guard guard(main().vfs_mutex);
	Abs_path abs;
	if (long r = dir_path(fd, abs)) return r;
	main().cwd = abs;
	return 0;
}


extern "C" long gxd_getcwd(char *buf, unsigned long len)
{
	Mutex::Guard guard(main().vfs_mutex);
	Abs_path const &cwd = main().cwd;
	if (len < cwd.length()) return -ERANGE;
	copy_cstring(buf, cwd.string(), len);
	return 0;
}
