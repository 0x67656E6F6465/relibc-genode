/*
 * \brief  State of the Genode shim of relibc, shared by its parts
 *
 * Everything lives in one 'Main' object that Component::construct builds
 * in place (start.cc). There are no global objects with constructors or
 * destructors: those would run, or register with relibc's atexit, before
 * relibc is initialized.
 *
 * The VFS is used by the entrypoint alone (ep.cc): other threads hand
 * their VFS work over as jobs ('on_ep').
 *
 * Locking: 'vfs_mutex' guards the fd table and the working directory; it
 * is held only briefly, never while waiting for the entrypoint (which
 * takes it, too). 'threads_mutex' guards the thread registry, 'futex_mutex'
 * the futex waiters, 'memory_mutex' the anonymous mappings, 'log_mutex'
 * standard output.
 */

#ifndef _STATE_H_
#define _STATE_H_

#include <base/component.h>
#include <base/heap.h>
#include <base/allocator_avl.h>
#include <base/log.h>
#include <base/mutex.h>
#include <base/blockade.h>
#include <base/thread.h>
#include <base/attached_rom_dataspace.h>
#include <base/attached_ram_dataspace.h>
#include <base/registry.h>
#include <log_session/log_session.h>
#include <timer_session/connection.h>
#include <util/list.h>
#include <util/construct_at.h>
#include <util/reconstructible.h>
#include <vfs/simple_env.h>

#include "gxd.h"

namespace Gxd {

	using namespace Genode;

	/* errno values and flags of relibc's (Linux) ABI */
	enum {
		EPERM = 1, ENOENT = 2, ESRCH = 3, EINTR = 4, EIO = 5, EBADF = 9, EAGAIN = 11,
		ENOMEM = 12, EACCES = 13, EEXIST = 17, ENOTDIR = 20, EISDIR = 21,
		EINVAL = 22, EMFILE = 24, ENOTTY = 25, ENOSPC = 28, ESPIPE = 29,
		EPIPE = 32, ENAMETOOLONG = 36, ENOSYS = 38, ENOTEMPTY = 39,
		ETIMEDOUT = 110, EXDEV = 18, ERANGE = 34, ELOOP = 40, ENOEXEC = 8, ECHILD = 10,
	};

	enum {
		O_ACCMODE = 3, O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2,
		O_CREAT = 0100, O_EXCL = 0200, O_TRUNC = 01000, O_APPEND = 02000,
		O_NONBLOCK = 04000, O_DIRECTORY = 0200000, O_NOFOLLOW = 0400000,
		O_CLOEXEC = 02000000,
	};

	enum { S_IFDIR = 0040000, S_IFREG = 0100000, S_IFCHR = 0020000,
	       S_IFLNK = 0120000, S_IFIFO = 0010000 };

	enum { SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2 };

	enum { AT_FDCWD = -100, AT_SYMLINK_NOFOLLOW = 0x100, AT_REMOVEDIR = 0x200 };

	using Abs_path = String<256>;

	struct Fd_object;   /* fd.cc */
	struct Futex_waiter;
	struct Relibc_genode_thread;   /* thread.cc */

	/**
	 * Line-buffered standard output and error, to the component's LOG
	 */
	struct Log_writer
	{
		Mutex  _mutex { };
		char   _line[Log_session::MAX_STRING_LEN] { };
		size_t _len = 0;

		void _flush() { if (_len) { _line[_len] = 0; log(Cstring(_line)); _len = 0; } }

		void flush() { Mutex::Guard guard(_mutex); _flush(); }

		void write(char const *buf, size_t n)
		{
			Mutex::Guard guard(_mutex);
			for (size_t i = 0; i < n; i++) {
				if (buf[i] == '\n') { _flush(); continue; }
				_line[_len++] = buf[i];
				if (_len == sizeof(_line) - 2) _flush();
			}
		}
	};

	/**
	 * What relibc keeps per thread
	 */
	struct Relibc_thread
	{
		Thread const *thread     = nullptr;
		bool          used       = false;
		bool          exited     = false;    /* to be reaped, no longer cancellable */
		Relibc_genode_thread *genode_thread = nullptr;  /* if the shim started it */
		Futex_waiter *waiting    = nullptr;  /* its futex wait (futex_mutex) */
		uint64_t      sigmask    = 0;        /* blocked signals, as relibc's */
		int           tid        = 0;
		unsigned long tp         = 0;        /* relibc's TCB */
		void        **emutls     = nullptr;  /* thread-locals by index - 1 */
		size_t        emutls_len = 0;

	};

	/**
	 * The memory of the emulated TLS (thread.cc): in anonymous mappings,
	 * zero-initialized, which a forked child inherits (fork.cc)
	 */
	struct Tls_memory
	{
		enum : size_t { CHUNK = 64*1024, MAX_SMALL = 512 };

		struct Free { Free *next; Free(Free *next) : next(next) { } };

		size_t next_index;           /* of the emutls variables, from 1 */
		char  *chunk;
		size_t left;
		Free  *free[MAX_SMALL/16];   /* by size, in steps of 16 */
	};

	extern Tls_memory tls_memory;

	/**
	 * Anonymous memory (mmap): RAM dataspaces in a window of the address
	 * space of its own, where the shim places them, apart from what Genode
	 * attaches anywhere: a forked child places the parent's at the same
	 * addresses, whatever it attached before (fork.cc)
	 */
	enum : addr_t { MAPPING_WINDOW = 0x100000000000UL };   /* 16 TiB */
	enum : size_t { MAPPING_WINDOW_SIZE = 0x10000000000UL }; /* 1 TiB */

	struct Mapping
	{
		struct Attach_failed { };

		Registry<Mapping>::Element _element;
		Ram_allocator             &_ram;
		Env::Local_rm             &_rm;
		size_t               const size;
		addr_t               const at;
		Ram_dataspace_capability   _ds;

		Mapping(Registry<Mapping> &registry, Ram_allocator &ram, Env::Local_rm &rm,
		        size_t size, addr_t at)
		:
			_element(registry, *this), _ram(ram), _rm(rm), size(size), at(at), _ds(ram.alloc(size))
		{
			rm.attach(_ds, { .size = { }, .offset = { }, .use_at = true, .at = at,
			                 .executable = { }, .writeable = true }).with_result(
				[&] (Env::Local_rm::Attachment &a) { a.deallocate = false; },
				[&] (Env::Local_rm::Error) { ram.free(_ds, size); throw Attach_failed(); });
		}

		~Mapping() { _rm.detach(at); _ram.free(_ds, size); }

		Mapping(Mapping const &) = delete;
		Mapping &operator = (Mapping const &) = delete;
	};

	struct Main : Vfs::Env::User
	{
		static constexpr unsigned MAX_FDS = 256, MAX_THREADS = 128;

		Env &env;

		Heap                   heap   { env.ram(), env.rm() };
		Attached_rom_dataspace config { env, "config" };
		Log_writer             out    { };
		/* the clock (elapsed_us only), opened on first use: a program that
		   never asks for the time needs no timer session (as with Genode's libc) */
		Mutex                            clock_mutex { };
		Constructible<Timer::Connection> _clock { };

		Timer::Connection &clock()
		{
			Mutex::Guard guard(clock_mutex);
			if (!_clock.constructed()) _clock.construct(env);
			return *_clock;
		}

		Mutex vfs_mutex { };
		Vfs::Simple_env vfs = config.node().with_sub_node("vfs",
			[&] (Node const &node) -> Vfs::Simple_env { return { env, heap, node, *this }; },
			[&] ()                 -> Vfs::Simple_env { return { env, heap, Node(), *this }; });

		/* Vfs::Env::User: the VFS made progress, jobs may complete now */
		void wakeup_vfs_user() override;

		Fd_object *fds[MAX_FDS] { };
		bool       cloexec[MAX_FDS] { };   /* FD_CLOEXEC, per fd (vfs_mutex) */
		Abs_path       cwd { "/" };

		Mutex             memory_mutex  { };
		Registry<Mapping> mappings      { };
		Allocator_avl     mapping_space { &heap };   /* of the window */

		Mutex         threads_mutex { };
		Relibc_thread threads[MAX_THREADS] { };
		int           next_tid = 1;
		int           main_tid = 0;   /* of the thread that runs main (posix_main.cc) */

		/* the process ids: the component's, its parent's if forked (fork.cc) */
		int pid = 1, ppid = 0;

		uint32_t io_seq = 0;  /* incremented on every change of readiness */

		Mutex                futex_mutex { };
		List<Futex_waiter>   futex_waiters { };

		unsigned long long random_state;

		Thread const *ep_thread = Thread::myself();  /* constructed on the entrypoint */

		Main(Env &env) : env(env), random_state(0)
		{
			(void)mapping_space.add_range(MAPPING_WINDOW, MAPPING_WINDOW_SIZE);
		}

		Main(Main const &) = delete;
		Main &operator = (Main const &) = delete;

		/* the entrypoint alone uses the VFS (ep.cc) */
		bool on_entrypoint() const { return Thread::myself() == ep_thread; }

		/* on the entrypoint: wait for VFS progress (dispatching an I/O signal) */
		void wait_io();

		/*
		 * On the entrypoint: dispatch I/O signals until one arrives or
		 * 'max_us' passed. Code that runs on the entrypoint (a
		 * Libc::Component's) waits that way, so that the VFS and the other
		 * threads' jobs keep progressing; 'wake_entrypoint' ends the wait.
		 */
		Constructible<Timer::Connection> _tick_timer { };
		Io_signal_handler<Main> _tick_handler { env.ep(), *this, &Main::_handle_tick };
		Io_signal_handler<Main> _wake_handler { env.ep(), *this, &Main::_handle_tick };
		void _handle_tick() { }
		void dispatch_io(unsigned long max_us = 10*1000);
		void wake_entrypoint() { Signal_transmitter(_wake_handler).submit(); }
	};

	Main &main();

	/* the calling thread's record (thread.cc) */
	Relibc_thread &thread_state();

	/* whether a signal the calling thread does not block is pending, which
	   ends its blocking waits with EINTR (signals.cc) */
	bool signal_interrupts();

	/* changes with every signal: a futex to wait for one (signals.cc) */
	uint32_t *signal_seq();

	/**
	 * Work for the entrypoint: a function it executes, again on progress of
	 * the VFS, until it returns true (complete). It must not block; short
	 * waits within it may use Main::wait_io.
	 */
	struct Ep_job : Interface { virtual bool execute() = 0; };

	/* execute 'job' on the entrypoint, until complete (true) or the
	   deadline (monotonic ns, -1: none) passed (false) (ep.cc) */
	bool run_on_ep(Ep_job &, long long deadline_ns = -1);

	template <typename FN>
	bool on_ep(FN const &fn, long long deadline_ns = -1)
	{
		struct Job : Ep_job
		{
			FN const &fn;
			Job(FN const &fn) : fn(fn) { }
			bool execute() override { return fn(); }
		} job { fn };
		return run_on_ep(job, deadline_ns);
	}

	/* 'fn' once on the entrypoint, its result */
	template <typename FN>
	long ep_call(FN const &fn)
	{
		long result = 0;
		on_ep([&] { result = fn(); return true; });
		return result;
	}

	/* let the entrypoint examine the jobs again (from any thread) */
	void examine_ep_jobs();
	void init_ep_jobs();

	/* a deadline that wakes a blocked thread (ep.cc) */
	struct Deadline : List<Deadline>::Element
	{
		long long const at;          /* monotonic ns */
		Blockade       &blockade;
		bool            expired = false;

		Deadline(long long at, Blockade &blockade) : at(at), blockade(blockade) { }
	};
	void add_deadline(Deadline &);
	void remove_deadline(Deadline &);

	/*
	 * The absolute, normalized path of 'path' relative to 'dirfd', its
	 * symbolic links followed, the last element's only if 'follow_last'
	 * (fs.cc, vfs_mutex held)
	 */
	long resolve(int dirfd, char const *path, Abs_path &out, bool follow_last = true);

	/* the fds of the config's '+ libc | + fd' nodes (fd.cc), as Genode's libc */
	void init_fds();

	/* their pending writes, at exit (fd.cc) */
	void sync_fds();

	/*
	 * fork (fork.cc): in the child, its start in place of relibc's: the
	 * objects loaded before the VFS (whose data come from the parent), the
	 * clone of the parent's state, the start of the thread of main
	 */
	void note_loaded_objects(Env &);
	long map_at(addr_t at, size_t size);   /* memory.cc */
	bool forked();
	void clone_from_parent();
	void start_forked_thread();

	/*
	 * A program to start (execve, posix_spawn): its ROM module, a script's
	 * interpreter (with the arguments of its "#!" line and the script) to
	 * precede the arguments (exec.cc, on the entrypoint)
	 */
	struct Program
	{
		enum { MAX_PREFIX = 8 };

		Abs_path rom { };
		char    *prefix[MAX_PREFIX] { };   /* on the heap */
		int      prefix_count = 0;
	};

	long find_program(char const *path, Program &);

	/* close the fds with FD_CLOEXEC (fd.cc, for execve) */
	void close_cloexec_fds();

	/* stat of an absolute path (fs.cc) */
	long stat_path(char const *abs, gxd_stat &st);

	/* read from an open handle, from its start, until 'len' or the end, or
	   'once' (fs.cc, vfs_mutex held) */
	long read_handle(Vfs::Vfs_handle &h, void *buf, size_t len, bool once = false);

	/* up to 'len' bytes of a VFS file from its start: their number, or
	   -errno (fs.cc, vfs_mutex held) */
	long read_vfs_file(char const *abs, void *buf, size_t len);

	/* the text of /etc/resolv.conf where the VFS has none (socket.cc,
	   vfs_mutex held) */
	using Resolv_conf = String<96>;
	bool resolv_conf(Resolv_conf &);

	/* an attribute of the config's '+ libc' node, as Genode's libc has it */
	using Libc_path = String<128>;
	Libc_path libc_config(char const *attr);
}

#endif /* _STATE_H_ */
