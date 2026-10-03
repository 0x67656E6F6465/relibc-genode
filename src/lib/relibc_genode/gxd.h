/*
 * \brief  C ABI between relibc's Genode platform and the Genode shim
 *
 * Mirrors relibc/src/platform/genode/gxd.rs. Results are non-negative on
 * success and -errno on failure. Flags are relibc's (Linux) values.
 */

#ifndef _GXD_H_
#define _GXD_H_

#include <base/stdint.h>

extern "C" {

	struct gxd_stat
	{
		Genode::uint32_t mode;
		Genode::uint64_t ino;
		Genode::int64_t  size;
		Genode::int64_t  blksize;
		Genode::int64_t  mtime;
	};

	/* file descriptors (fd.cc) */
	long  gxd_write(int fd, void const *buf, unsigned long len);
	long  gxd_read(int fd, void *buf, unsigned long len);
	long  gxd_pread(int fd, void *buf, unsigned long len, long long off);
	long  gxd_pwrite(int fd, void const *buf, unsigned long len, long long off);
	long  gxd_openat(int dirfd, char const *path, int oflag, unsigned mode);
	long  gxd_close(int fd);
	long  gxd_lseek(int fd, long long off, int whence);
	long  gxd_fcntl_setfl(int fd, int flags);
	long  gxd_fcntl_getfd(int fd);
	long  gxd_fcntl_setfd(int fd, int flags);

	/* sockets (socket.cc), the Linux ABI's arguments */
	long  gxd_socket(int domain, int type, int protocol);
	long  gxd_bind(int fd, void const *addr, Genode::uint32_t len);
	long  gxd_listen(int fd, int backlog);
	long  gxd_accept(int fd, void *addr, Genode::uint32_t *addrlen, int flags);
	long  gxd_connect(int fd, void const *addr, Genode::uint32_t len);
	long  gxd_sendto(int fd, void const *buf, unsigned long len, int flags,
	                 void const *dest, Genode::uint32_t destlen);
	long  gxd_recvfrom(int fd, void *buf, unsigned long len, int flags,
	                   void *addr, Genode::uint32_t *addrlen);
	long  gxd_getsockname(int fd, void *addr, Genode::uint32_t *addrlen);
	long  gxd_getpeername(int fd, void *addr, Genode::uint32_t *addrlen);
	long  gxd_getsockopt(int fd, int level, int name, void *val, Genode::uint32_t *len);
	long  gxd_setsockopt(int fd, int level, int name, void const *val, Genode::uint32_t len);
	long  gxd_shutdown(int fd, int how);
	long  gxd_socketpair(int domain, int type, int protocol, int fds[2]);

	/* signals and POSIX timers (signals.cc) */
	void            gxd_signal_raise(int signo);
	int             gxd_signal_take(Genode::uint64_t allowed);
	Genode::uint64_t gxd_signals_pending();
	void            gxd_set_sigmask(Genode::uint64_t mask);
	long            gxd_signal_wait(Genode::uint64_t allowed, long long deadline_ns);
	long            gxd_timer_create(int signo, int clock);
	long            gxd_timer_settime(long id, long long value_ns, long long interval_ns, int absolute,
	                                  long long *old_value_ns, long long *old_interval_ns);
	long            gxd_timer_gettime(long id, long long *value_ns, long long *interval_ns);
	long            gxd_timer_delete(long id);

	long  gxd_readlinkat(int dirfd, char const *path, char *buf, unsigned long len);
	long  gxd_symlinkat(char const *target, int dirfd, char const *path);
	long  gxd_fstatat(int dirfd, char const *path, gxd_stat *st, int flags);
	long  gxd_ftruncate(int fd, long long len);
	long  gxd_fsync(int fd);
	long  gxd_getdents(int fd, void *buf, unsigned long len);
	long  gxd_dup(int fd, int min_fd, int cloexec);
	long  gxd_dup2(int fd, int fd2);
	long  gxd_pipe(int fds[2]);
	long  gxd_fcntl_getfl(int fd);
	long  gxd_ioctl(int fd, unsigned long request, void *arg);

	/* epoll, which relibc's poll and select use (epoll.cc) */
	long  gxd_epoll_create();
	long  gxd_epoll_ctl(int epfd, int op, int fd, Genode::uint32_t events, Genode::uint64_t data);
	long  gxd_epoll_wait(int epfd, Genode::uint32_t *events, Genode::uint64_t *data,
	                     int max, int timeout_ms);

	/* paths (fs.cc) */
	long  gxd_unlinkat(int dirfd, char const *path, int flags);
	long  gxd_mkdirat(int dirfd, char const *path, unsigned mode);
	long  gxd_renameat(int olddirfd, char const *oldpath, int newdirfd, char const *newpath);
	long  gxd_faccessat(int dirfd, char const *path, int mode);
	long  gxd_chdir(char const *path);
	long  gxd_fchdir(int fd);
	long  gxd_getcwd(char *buf, unsigned long len);

	/* memory (memory.cc) */
	long  gxd_mmap_anon(unsigned long len);
	long  gxd_munmap(void *addr, unsigned long len);

	/* time and randomness (time.cc) */
	long long gxd_clock_ns(int clock);
	long  gxd_sleep_ns(unsigned long long ns);
	long  gxd_getrandom(void *buf, unsigned long len);

	/* threads (thread.cc) */
	long  gxd_set_tp(unsigned long tp);
	unsigned long gxd_get_tp();
	int   gxd_gettid();
	unsigned long gxd_default_stack_size();
	long  gxd_thread_create(unsigned long *stack);
	void  gxd_thread_exit() __attribute__((noreturn));
	long  gxd_thread_cancel(int tid);
	long  gxd_futex_wait(Genode::uint32_t *addr, Genode::uint32_t val, long long deadline_ns);
	long  gxd_futex_wake(Genode::uint32_t *addr, Genode::uint32_t num);
	void  gxd_exit(int status) __attribute__((noreturn));

	/* processes (fork.cc): a child of the same binary, its end; the ids */
	long  gxd_fork();
	long  gxd_waitpid(int pid, int *status, int options);
	int   gxd_getpid();
	int   gxd_getppid();
	void  gxd_set_caught_signals(Genode::uint64_t mask);  /* the signals with a handler */

	/* execve (exec.cc): the new program loaded, then started (no return) */
	long  gxd_exec_load(char const *path);
	void  gxd_exec_run(char *const argv[], char *const envp[]) __attribute__((noreturn));
}

#endif /* _GXD_H_ */
