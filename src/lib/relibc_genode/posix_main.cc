/*
 * \brief  Genode's posix library on relibc: main, in a thread of its own
 *
 * As Genode's libc's posix library (src/lib/posix/construct.cc): main with
 * the config's arguments and environment, and exit with its result. It
 * runs in a thread of its own, so that the entrypoint stays free to
 * dispatch the component's I/O signals, the VFS's in particular (ep.cc).
 */

#include <libc/component.h>
#include <libc/args.h>

#include <pthread.h>
#include <stdlib.h>   /* exit */

extern char **environ;

extern "C" int main(int argc, char **argv, char **envp);

namespace { struct { int argc; char **argv; char **envp; } args; }

static void *main_thread(void *)
{
	exit(main(args.argc, args.argv, args.envp));
	return nullptr;
}


void Libc::Component::construct(Libc::Env &env)
{
	populate_args_and_env(env, args.argc, args.argv, args.envp);
	environ = args.envp;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, Libc::Component::stack_size());

	pthread_t thread;
	if (pthread_create(&thread, &attr, main_thread, nullptr) != 0)
		main_thread(nullptr);   /* no thread: on the entrypoint, then */
	pthread_attr_destroy(&attr);
}
