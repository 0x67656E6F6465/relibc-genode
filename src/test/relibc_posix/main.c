/*
 * POSIX on relibc on Genode: threads, synchronization, directories, file
 * operations, pipes. Prints one line per check and "relibc posix: all
 * checks passed" at the end; exits with the number of failed checks.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <search.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int failed = 0;

static void check(int ok, char const *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failed++;
}


/* threads and a mutex under contention */

enum { THREADS = 4, ROUNDS = 20000 };

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

static void *count(void *arg)
{
	for (int i = 0; i < ROUNDS; i++) {
		pthread_mutex_lock(&counter_mutex);
		counter++;
		pthread_mutex_unlock(&counter_mutex);
	}
	return arg;
}

static void test_threads(void)
{
	pthread_t t[THREADS];
	int created = 0;
	for (long i = 0; i < THREADS; i++)
		created += pthread_create(&t[i], NULL, count, (void *)(i + 1)) == 0;
	check(created == THREADS, "pthread_create x4");

	long sum = 0;
	for (int i = 0; i < THREADS; i++) {
		void *ret = NULL;
		pthread_join(t[i], &ret);
		sum += (long)ret;
	}
	check(sum == 1 + 2 + 3 + 4, "pthread_join returns each thread's value");
	check(counter == THREADS*ROUNDS, "mutex under contention: no lost increments");
}


/* a condition variable, and a timed wait that times out */

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond  = PTHREAD_COND_INITIALIZER;
static int queue[16], queue_len = 0, produced_done = 0;

static void *produce(void *arg)
{
	(void)arg;
	for (int i = 1; i <= 100; i++) {
		pthread_mutex_lock(&queue_mutex);
		while (queue_len == 16) pthread_cond_wait(&queue_cond, &queue_mutex);
		queue[queue_len++] = i;
		pthread_cond_broadcast(&queue_cond);
		pthread_mutex_unlock(&queue_mutex);
	}
	pthread_mutex_lock(&queue_mutex);
	produced_done = 1;
	pthread_cond_broadcast(&queue_cond);
	pthread_mutex_unlock(&queue_mutex);
	return NULL;
}

static void test_cond(void)
{
	pthread_t p;
	pthread_create(&p, NULL, produce, NULL);
	long sum = 0;
	pthread_mutex_lock(&queue_mutex);
	for (;;) {
		while (queue_len == 0 && !produced_done) pthread_cond_wait(&queue_cond, &queue_mutex);
		if (queue_len == 0 && produced_done) break;
		sum += queue[--queue_len];
		pthread_cond_broadcast(&queue_cond);
	}
	pthread_mutex_unlock(&queue_mutex);
	pthread_join(p, NULL);
	check(sum == 5050, "producer/consumer over a condition variable");

	struct timespec start, deadline, end;
	clock_gettime(CLOCK_REALTIME, &start);
	deadline = start;
	deadline.tv_nsec += 50*1000*1000;
	if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
	pthread_mutex_lock(&queue_mutex);
	int r = pthread_cond_timedwait(&queue_cond, &queue_mutex, &deadline);
	pthread_mutex_unlock(&queue_mutex);
	clock_gettime(CLOCK_REALTIME, &end);
	long ms = (end.tv_sec - start.tv_sec)*1000 + (end.tv_nsec - start.tv_nsec)/1000000;
	check(r == ETIMEDOUT && ms >= 45 && ms < 1000, "pthread_cond_timedwait times out after ~50 ms");
}


/* errno is per thread (relibc's thread-locals, emulated TLS) */

static void *set_errno(void *arg)
{
	errno = 77;
	usleep(20*1000);
	return (void *)(long)(errno == 77);
}

static void test_errno(void)
{
	errno = 5;
	pthread_t t;
	void *ok;
	pthread_create(&t, NULL, set_errno, NULL);
	pthread_join(t, &ok);
	check(ok && errno == 5, "errno is per thread");
}


/* directories and file operations */

static int names_equal(char const *dir, char const *expected)
{
	DIR *d = opendir(dir);
	if (!d) return 0;
	char names[256] = "";
	char sorted[8][32];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 8) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		strncpy(sorted[n++], e->d_name, 31);
	}
	closedir(d);
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (strcmp(sorted[i], sorted[j]) > 0) {
				char tmp[32]; strcpy(tmp, sorted[i]); strcpy(sorted[i], sorted[j]); strcpy(sorted[j], tmp);
			}
	for (int i = 0; i < n; i++) { strcat(names, sorted[i]); if (i + 1 < n) strcat(names, " "); }
	printf("      %s: %s\n", dir, names);
	return !strcmp(names, expected);
}

static void touch(char const *path, char const *text)
{
	FILE *f = fopen(path, "w");
	if (f) { fputs(text, f); fclose(f); }
}

static void test_files(void)
{
	check(mkdir("/tmp/d", 0755) == 0, "mkdir /tmp/d");
	check(mkdir("/tmp/d", 0755) == -1 && errno == EEXIST, "mkdir of an existing directory: EEXIST");
	check(mkdir("/tmp/d/sub", 0755) == 0, "mkdir /tmp/d/sub");
	touch("/tmp/d/a", "a"); touch("/tmp/d/b", "bb"); touch("/tmp/d/c", "ccc");
	check(names_equal("/tmp/d", "a b c sub"), "readdir lists the entries");

	struct stat st;
	check(stat("/tmp/d/c", &st) == 0 && st.st_size == 3 && S_ISREG(st.st_mode), "stat of a file");
	check(stat("/tmp/d/sub", &st) == 0 && S_ISDIR(st.st_mode), "stat of a directory");

	check(rename("/tmp/d/b", "/tmp/d/e") == 0, "rename");
	check(unlink("/tmp/d/a") == 0, "unlink");
	check(unlink("/tmp/d/a") == -1 && errno == ENOENT, "unlink of a missing file: ENOENT");
	check(names_equal("/tmp/d", "c e sub"), "readdir after rename and unlink");

	check(chdir("/tmp/d/sub") == 0, "chdir");
	char cwd[64] = "";
	check(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/tmp/d/sub"), "getcwd");
	touch("../rel", "relative");
	check(access("/tmp/d/rel", R_OK) == 0, "relative paths, '..'");
	check(rmdir("/tmp/d") == -1 && errno == ENOTEMPTY, "rmdir of a non-empty directory: ENOTEMPTY");
	chdir("/");
	check(rmdir("/tmp/d/sub") == 0, "rmdir");

	int fd = open("/tmp/d/e", O_RDWR | O_APPEND);
	write(fd, "!", 1);
	check(lseek(fd, 0, SEEK_END) == 3, "O_APPEND appends");
	check(ftruncate(fd, 1) == 0 && fstat(fd, &st) == 0 && st.st_size == 1, "ftruncate");
	close(fd);
}


/* pipes, across threads and through more than their buffer */

enum { PIPE_BYTES = 200*1000 };

static void *pipe_writer(void *arg)
{
	int fd = (int)(long)arg;
	char buf[1000];
	for (int i = 0; i < PIPE_BYTES; i += (int)sizeof(buf)) {
		for (int j = 0; j < (int)sizeof(buf); j++) buf[j] = (char)((i + j) & 0x7f);
		write(fd, buf, sizeof(buf));
	}
	close(fd);
	return NULL;
}

static void test_pipe(void)
{
	int fds[2];
	check(pipe(fds) == 0, "pipe");
	pthread_t t;
	pthread_create(&t, NULL, pipe_writer, (void *)(long)fds[1]);

	long total = 0, bad = 0;
	char buf[4096];
	ssize_t n;
	while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
		for (ssize_t j = 0; j < n; j++) if (buf[j] != (char)((total + j) & 0x7f)) bad++;
		total += n;
	}
	pthread_join(t, NULL);
	close(fds[0]);
	check(total == PIPE_BYTES && bad == 0, "200 KB through a 64 KB pipe from another thread, then EOF");

	pipe(fds);
	check(dup2(fds[1], 20) == 20, "dup2");
	write(20, "via dup", 7);
	close(20); close(fds[1]);
	char got[16] = "";
	read(fds[0], got, sizeof(got) - 1);
	close(fds[0]);
	check(!strcmp(got, "via dup"), "writing through a dup'ed fd");
}


/* poll and select (relibc builds them on epoll) */

static long ms_since(struct timespec const *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec)*1000 + (now.tv_nsec - t->tv_nsec)/1000000;
}

static void *write_later(void *arg)
{
	usleep(30*1000);
	write((int)(long)arg, "x", 1);
	return NULL;
}

static void test_poll(void)
{
	int fds[2];
	pipe(fds);
	struct timespec t;

	struct pollfd p = { .fd = fds[0], .events = POLLIN };
	clock_gettime(CLOCK_MONOTONIC, &t);
	int r = poll(&p, 1, 40);
	long ms = ms_since(&t);
	check(r == 0 && ms >= 35, "poll on an empty pipe times out");

	struct pollfd w = { .fd = fds[1], .events = POLLOUT };
	check(poll(&w, 1, 0) == 1 && (w.revents & POLLOUT), "poll: a pipe's write end is writable");

	pthread_t th;
	pthread_create(&th, NULL, write_later, (void *)(long)fds[1]);
	clock_gettime(CLOCK_MONOTONIC, &t);
	p.revents = 0;
	r = poll(&p, 1, 2000);
	ms = ms_since(&t);
	pthread_join(th, NULL);
	check(r == 1 && (p.revents & POLLIN) && ms >= 20 && ms < 1000,
	      "poll wakes up when another thread writes");
	char c; read(fds[0], &c, 1);

	fd_set set;
	FD_ZERO(&set); FD_SET(fds[0], &set);
	struct timeval tv = { 0, 30*1000 };
	check(select(fds[0] + 1, &set, NULL, NULL, &tv) == 0, "select on an empty pipe times out");

	pthread_create(&th, NULL, write_later, (void *)(long)fds[1]);
	FD_ZERO(&set); FD_SET(fds[0], &set);
	tv.tv_sec = 2; tv.tv_usec = 0;
	r = select(fds[0] + 1, &set, NULL, NULL, &tv);
	pthread_join(th, NULL);
	check(r == 1 && FD_ISSET(fds[0], &set), "select reports the pipe readable");
	read(fds[0], &c, 1);

	close(fds[1]);
	p.revents = 0;
	check(poll(&p, 1, 1000) == 1 && (p.revents & POLLHUP), "poll reports POLLHUP after the writer closed");
	close(fds[0]);
}


/* symbolic links of the VFS: kept by it, followed by the shim */
static void test_symlinks(void)
{
	check(mkdir("/tmp/l", 0777) == 0 && mkdir("/tmp/l/dir", 0777) == 0, "mkdir /tmp/l/dir");
	int fd = open("/tmp/l/dir/file", O_CREAT | O_WRONLY, 0600);
	check(fd >= 0 && write(fd, "linked", 6) == 6 && close(fd) == 0, "a file to link to");

	check(symlink("dir", "/tmp/l/rel") == 0, "symlink, relative target");
	check(symlink("/tmp/l/rel/file", "/tmp/l/abs") == 0, "symlink, absolute target through a link");
	check(symlink("x", "/tmp/l/rel") == -1 && errno == EEXIST, "symlink over an existing node: EEXIST");

	char buf[64] = { 0 };
	check(readlink("/tmp/l/rel", buf, sizeof(buf)) == 3 && !memcmp(buf, "dir", 3), "readlink");
	check(readlink("/tmp/l/dir", buf, sizeof(buf)) == -1 && errno == EINVAL, "readlink of a directory: EINVAL");

	fd = open("/tmp/l/abs", O_RDONLY);
	check(fd >= 0 && read(fd, buf, sizeof(buf)) == 6 && !memcmp(buf, "linked", 6), "open follows links");
	if (fd >= 0) close(fd);

	struct stat st;
	check(lstat("/tmp/l/abs", &st) == 0 && S_ISLNK(st.st_mode), "lstat of a link");
	check(stat("/tmp/l/abs", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6, "stat follows links");
	check(open("/tmp/l/abs", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP, "O_NOFOLLOW on a link: ELOOP");

	check(symlink("loop", "/tmp/l/loop") == 0 && open("/tmp/l/loop", O_RDONLY) == -1 && errno == ELOOP,
	      "a link to itself: ELOOP");
	check(unlink("/tmp/l/abs") == 0 && lstat("/tmp/l/abs", &st) == -1 && stat("/tmp/l/dir/file", &st) == 0,
	      "unlink removes the link, not its target");
}


/* file features beyond reading and writing */
static void test_file_extras(void)
{
	int fd = open("/tmp/m", O_CREAT | O_RDWR, 0600);
	check(fd >= 0 && write(fd, "mapped content", 14) == 14, "a file to map");

	char *m = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0);
	check(m != MAP_FAILED && !memcmp(m, "mapped content", 14) && m[14] == 0 && m[8191] == 0,
	      "mmap of a file (MAP_PRIVATE), zeroes beyond its end");
	if (m != MAP_FAILED) munmap(m, 8192);
	check(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED,
	      "mmap MAP_SHARED of a file: refused");

	struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	check(fcntl(fd, F_SETLK, &fl) == 0, "fcntl F_SETLK");
	check(fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_UNLCK, "fcntl F_GETLK: nobody else holds it");
	check(flock(fd, LOCK_EX) == 0 && flock(fd, LOCK_UN) == 0, "flock");

	struct statvfs vfs;
	check(fstatvfs(fd, &vfs) == 0 && vfs.f_bsize > 0, "fstatvfs");
	close(fd);

	int p[2];
	char c;
	check(pipe2(p, O_NONBLOCK) == 0 && read(p[0], &c, 1) == -1 && errno == EAGAIN,
	      "pipe2 O_NONBLOCK: EAGAIN on an empty pipe");
	close(p[0]); close(p[1]);
}


/* functions relibc gained on the way (relibc-fixes.patch) */
static int cmp_int(void const *a, void const *b) { return *(int const *)a - *(int const *)b; }

static int walked;
static void count_node(void const *node, VISIT v, int depth)
{
	(void)node; (void)depth;
	if (v == postorder || v == leaf) walked++;
}

static void test_libc_additions(void)
{
	unsigned char a[32] = { 0 }, b[32] = { 0 };
	check(getentropy(a, sizeof(a)) == 0 && getentropy(b, sizeof(b)) == 0 && memcmp(a, b, sizeof(a)),
	      "getentropy");
	check(getentropy(a, 257) == -1 && errno == EINVAL, "getentropy of more than 256 bytes: EINVAL");

	int in_range = 1;
	for (int i = 0; i < 1000; i++) if (arc4random_uniform(7) >= 7) in_range = 0;
	check(in_range, "arc4random_uniform stays below its bound");

	static int keys[100];
	void *root = NULL;
	for (int i = 0; i < 100; i++) { keys[i] = (i * 37) % 100; tsearch(&keys[i], &root, cmp_int); }
	int k = 42, missing = 1000;
	check(tfind(&k, &root, cmp_int) && **(int **)tfind(&k, &root, cmp_int) == 42
	      && !tfind(&missing, &root, cmp_int), "tsearch, tfind");
	walked = 0;
	twalk(root, count_node);
	check(walked == 100, "twalk visits every node once");
	check(tdelete(&k, &root, cmp_int) && !tfind(&k, &root, cmp_int), "tdelete");
	tdestroy(root, NULL);
}


/* AF_UNIX stream sockets within the component */
static void test_socketpair(void)
{
	int sv[2];
	check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	check(write(sv[0], "ping", 4) == 4, "write to one end");

	char buf[16] = { 0 };
	check(read(sv[1], buf, sizeof(buf)) == 4 && !memcmp(buf, "ping", 4), "read at the other");

	struct iovec out[2] = { { "pa", 2 }, { "rts", 3 } };
	struct msghdr msg = { .msg_iov = out, .msg_iovlen = 2 };
	check(sendmsg(sv[1], &msg, 0) == 5, "sendmsg of two buffers, back the other way");

	char x[2], y[8] = { 0 };
	struct iovec in[2] = { { x, 2 }, { y, sizeof(y) } };
	struct msghdr rmsg = { .msg_iov = in, .msg_iovlen = 2 };
	check(recvmsg(sv[0], &rmsg, 0) == 5 && !memcmp(x, "pa", 2) && !memcmp(y, "rts", 3),
	      "recvmsg scatters into two buffers");

	fcntl(sv[0], F_SETFL, O_NONBLOCK);
	check(recv(sv[0], buf, sizeof(buf), 0) == -1 && errno == EAGAIN, "O_NONBLOCK: EAGAIN when empty");

	struct pollfd p = { .fd = sv[0], .events = POLLIN };
	write(sv[1], "x", 1);
	check(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "poll on a socket pair");
	close(sv[1]);
	read(sv[0], buf, 1);
	check(read(sv[0], buf, 1) == 0, "end of stream when the other end is closed");
	close(sv[0]);
}


/* signals within the component: raise, kill, masks, timers, alarm */
static volatile int got_usr1, got_alrm, got_timer, info_signo;
static void on_usr1(int sig)  { (void)sig; got_usr1++; }
static void on_alrm(int sig)  { (void)sig; got_alrm++; }
static void on_timer(int sig) { (void)sig; got_timer++; }
static void on_info(int sig, siginfo_t *info, void *ctx) { (void)sig; (void)ctx; info_signo = info->si_signo; }

static void test_signals(void)
{
	check(signal(SIGUSR1, on_usr1) != SIG_ERR && raise(SIGUSR1) == 0 && got_usr1 == 1,
	      "raise runs the handler");
	check(kill(getpid(), SIGUSR1) == 0 && got_usr1 == 2, "kill of the component itself");
	check(kill(4242, SIGUSR1) == -1 && errno == ESRCH, "kill of another process: ESRCH");

	struct sigaction sa = { 0 };
	sa.sa_sigaction = on_info;
	sa.sa_flags = SA_SIGINFO;
	check(sigaction(SIGUSR2, &sa, NULL) == 0 && raise(SIGUSR2) == 0 && info_signo == SIGUSR2,
	      "SA_SIGINFO handler with siginfo");

	sigset_t set, old, pending;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, &old);
	raise(SIGUSR1);
	sigpending(&pending);
	check(got_usr1 == 2 && sigismember(&pending, SIGUSR1), "a blocked signal stays pending");
	sigprocmask(SIG_SETMASK, &old, NULL);
	check(got_usr1 == 3, "unblocking delivers it");

	check(signal(SIGUSR1, SIG_IGN) != SIG_ERR && raise(SIGUSR1) == 0 && got_usr1 == 3, "SIG_IGN");

	signal(SIGALRM, on_alrm);
	alarm(1);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	unsigned const left = sleep(5);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	check(got_alrm == 1 && left > 0 && t1.tv_sec - t0.tv_sec < 3, "alarm interrupts sleep, the handler runs");

	struct itimerval it = { .it_interval = { 0, 100000 }, .it_value = { 0, 100000 } };
	got_alrm = 0;
	setitimer(ITIMER_REAL, &it, NULL);
	while (got_alrm < 3) usleep(20000);
	struct itimerval off = { 0 };
	setitimer(ITIMER_REAL, &off, &it);
	check(got_alrm >= 3 && it.it_interval.tv_usec == 100000, "setitimer: periodic SIGALRM, getitimer's setting");

	signal(SIGRTMIN, on_timer);
	struct sigevent ev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN };
	timer_t timer;
	struct itimerspec spec = { .it_interval = { 0, 50000000 }, .it_value = { 0, 50000000 } };
	check(timer_create(CLOCK_MONOTONIC, &ev, &timer) == 0 && timer_settime(timer, 0, &spec, NULL) == 0,
	      "timer_create, timer_settime");
	while (got_timer < 4) usleep(10000);
	struct itimerspec cur;
	check(timer_gettime(timer, &cur) == 0 && cur.it_interval.tv_nsec == 50000000 && timer_delete(timer) == 0,
	      "timer_gettime, timer_delete");

	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_BLOCK, &set, NULL);
	raise(SIGUSR2);
	struct timespec no_wait = { 0, 0 };
	siginfo_t info;
	check(sigtimedwait(&set, &info, &no_wait) == SIGUSR2 && info.si_signo == SIGUSR2
	      && sigtimedwait(&set, NULL, &no_wait) == -1 && errno == EAGAIN, "sigtimedwait");
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}


/* a terminal of the VFS (its terminal plugin), and a file that is none */
static void test_terminal(void)
{
	int const fd = open("/dev/terminal", O_RDWR);
	check(fd >= 0 && isatty(fd), "isatty of a terminal");

	struct winsize ws;
	check(ioctl(fd, TIOCGWINSZ, &ws) == 0, "TIOCGWINSZ of a terminal");

	struct termios t;
	check(tcgetattr(fd, &t) == 0 && (t.c_lflag & ECHO), "tcgetattr of a terminal");
	check(tcsetattr(fd, TCSANOW, &t) == 0, "tcsetattr of a terminal");
	close(fd);

	int const f = open("/tmp/no-tty", O_CREAT | O_RDWR, 0600);
	errno = 0;
	check(f >= 0 && !isatty(f) && errno == ENOTTY, "isatty of a file: ENOTTY");
	close(f);
}


int main(void)
{
	test_threads();
	test_cond();
	test_errno();
	test_files();
	test_pipe();
	test_poll();
	test_symlinks();
	test_file_extras();
	test_libc_additions();
	test_socketpair();
	test_signals();
	test_terminal();

	if (failed) printf("relibc posix: %d check(s) failed\n", failed);
	else        printf("relibc posix: all checks passed\n");
	return failed;
}
