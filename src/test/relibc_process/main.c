/*
 * Processes on relibc on Genode (the drop-in libc): posix_spawn, fork with
 * execve, waitpid, SIGCHLD. One binary, the parent and (by its arguments)
 * the children. Prints one line per check and "relibc process: all checks
 * passed" at the end; exits with the number of failed checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int failed = 0;

static void check(int ok, char const *what)
{
	printf("relibc process: %s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok) failed++;
}


/* a child: "spawned FD CODE" writes its greeting to FD, "exec CODE" */
static int child(int argc, char **argv)
{
	if (!strcmp(argv[1], "spawned") && argc == 4) {
		char const *greeting = getenv("GREETING");
		char cwd[64] = { };
		getcwd(cwd, sizeof(cwd));
		dprintf(atoi(argv[2]), "%s from %d in %s", greeting ? greeting : "(none)", getpid(), cwd);
		return atoi(argv[3]);
	}
	if (!strcmp(argv[1], "exec") && argc == 3)
		return atoi(argv[2]);
	return 99;
}


static volatile int sigchld_count = 0;

static void on_sigchld(int sig) { (void)sig; sigchld_count++; }


static int exit_code(pid_t pid)
{
	int status = -1;
	pid_t const r = waitpid(pid, &status, 0);
	if (r != pid || !WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}


int main(int argc, char **argv)
{
	if (argc > 1) return child(argc, argv);

	/* posix_spawn: arguments, environment, an fd opened for it, the cwd */
	{
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, 5, "/tmp/spawned", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		posix_spawn_file_actions_addchdir(&actions, "/tmp");

		char *child_argv[] = { "relibc_process", "spawned", "5", "7", NULL };
		char *child_env[]  = { "GREETING=hello", NULL };
		pid_t pid = 0;
		int const r = posix_spawn(&pid, "/relibc_process", &actions, NULL, child_argv, child_env);
		posix_spawn_file_actions_destroy(&actions);
		check(r == 0 && pid > 0, "posix_spawn");
		check(exit_code(pid) == 7, "posix_spawn: waitpid, exit code");

		char buf[64] = { };
		int const fd = open("/tmp/spawned", O_RDONLY);
		read(fd, buf, sizeof(buf) - 1);
		close(fd);
		char expected[64];
		snprintf(expected, sizeof(expected), "hello from %d in /tmp", pid);
		printf("relibc process: the spawned child wrote '%s'\n", buf);
		check(!strcmp(buf, expected), "posix_spawn: args, env, fd, cwd");

		char *missing_argv[] = { "missing", NULL };
		check(posix_spawn(&pid, "/missing", NULL, NULL, missing_argv, environ) == ENOENT,
		      "posix_spawn: ENOENT");
	}

	/* fork, then execve in the child */
	{
		pid_t const pid = fork();
		if (pid == 0) {
			char *exec_argv[] = { "relibc_process", "exec", "3", NULL };
			execve("/relibc_process", exec_argv, environ);
			_exit(98);
		}
		check(pid > 0, "fork");
		check(exit_code(pid) == 3, "fork and execve: exit code");
	}

	/* SIGCHLD, a handler's */
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_sigchld;
		sigaction(SIGCHLD, &sa, NULL);

		pid_t const pid = fork();
		if (pid == 0) _exit(5);

		for (int i = 0; i < 200 && !sigchld_count; i++) {
			struct timespec ts = { 0, 10*1000*1000 };
			nanosleep(&ts, NULL);
		}
		check(sigchld_count == 1, "SIGCHLD");
		check(exit_code(pid) == 5, "fork: _exit code");
	}

	/* waitpid without children */
	{
		int status;
		check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "waitpid: ECHILD");
	}

	if (failed == 0) printf("relibc process: all checks passed\n");
	else             printf("relibc process: %d checks failed\n", failed);
	return failed;
}
