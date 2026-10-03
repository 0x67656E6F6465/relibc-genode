/*
 * \brief  Start of a relibc program on Genode, and its exit
 *
 * Component::construct builds the shim's state and hands relibc the
 * initial stack it expects from Linux: argc, argv and envp from the
 * config ('+ arg | value: ...', '+ env | key: ... | value: ...') and an
 * auxiliary vector. relibc_start_v1 sets up relibc (its TCB, standard I/O,
 * the environment) and would then run main; instead, its callback returns
 * to Component::construct, which calls Libc::Component::construct as
 * Genode's libc does. The posix library implements that by running main
 * (with the config's arguments) in a thread of its own (posix_main.cc), and
 * the entrypoint returns to dispatching signals, the VFS's above all.
 *
 * A forked child starts differently: it takes relibc's state and the
 * thread of main from its parent (fork.cc).
 */

#include <base/sleep.h>
#include <util/construct_at.h>

#include <libc/component.h>

#include "state.h"

using namespace Gxd;

namespace Gxd {
	void init_stdio();                /* fd.cc */
	void init_time();                 /* time.cc */
	void construct_libc_component();  /* libc_component.cc */
}

static Main *_main;

Main &Gxd::main() { return *_main; }


extern "C" void gxd_exit(int status)
{
	/* what the files have yet to write (a file-system session's), as
	   Genode's libc closes the fds at exit */
	sync_fds();

	main().out.flush();  /* a pending line */

	/* no pending tick (Main::dispatch_io) for a signal context gone */
	main()._tick_timer.destruct();

	main().env.parent().exit(status);
	sleep_forever();
}


extern "C" void relibc_start_v1(unsigned long *sp, int (*)(int, char **, char **))
	__attribute__((noreturn));


/* where relibc_start_v1's callback returns to (__builtin_setjmp buffer) */
static void *_relibc_ready[5];

static int relibc_ready(int, char **, char **)
{
	__builtin_longjmp(_relibc_ready, 1);
}


size_t Component::stack_size() { return Libc::Component::stack_size(); }

void Component::construct(Env &env)
{
	/*
	 * Not a function-local static object: its destructor would be registered
	 * with __cxa_atexit, which is relibc's, before relibc is initialized.
	 */
	note_loaded_objects(env);   /* the program's, before the VFS's (fork.cc) */

	alignas(Main) static char storage[sizeof(Main)];
	_main = construct_at<Main>(storage, env);
	Main &state = *_main;
	init_ep_jobs();

	init_stdio();
	init_time();

	/* the process ids a parent gave (fork.cc), as Genode's libc's 'pid' */
	state.pid  = (int)state.config.node().with_sub_node("libc",
		[] (Node const &libc) { return libc.attribute_value("pid", 1U); }, [] { return 1U; });
	state.ppid = (int)state.config.node().with_sub_node("libc",
		[] (Node const &libc) { return libc.attribute_value("ppid", 0U); }, [] { return 0U; });

	/* a forked child: relibc's state, and main's thread, from the parent */
	if (forked()) {
		clone_from_parent();
		start_forked_thread();
		return;
	}
	init_fds();

	/* argv and envp from the config, as with Genode's libc */
	enum { MAX_ARGS = 32, MAX_ENV = 32, STRINGS = 4096 };
	static char           strings[STRINGS];
	static char          *argv[MAX_ARGS + 1];
	static char          *envp[MAX_ENV + 1];
	static unsigned long  stack[1 + MAX_ARGS + 1 + MAX_ENV + 1 + 8];
	size_t used = 0;
	auto keep = [&] (auto const &s) -> char * {
		size_t n = s.length();
		if (!n || used + n > STRINGS) return nullptr;
		copy_cstring(strings + used, s.string(), n);
		char *p = strings + used; used += n;
		return p;
	};

	int argc = 0, envc = 0;
	state.config.node().for_each_sub_node("arg", [&] (Node const &arg) {
		if (argc < MAX_ARGS)
			if (char *p = keep(arg.attribute_value("value", String<512>())))
				argv[argc++] = p; });
	if (argc == 0) argv[argc++] = (char *)"relibc";

	state.config.node().for_each_sub_node("env", [&] (Node const &e) {
		using S = String<512>;
		S const pair(e.attribute_value("key", S()), "=", e.attribute_value("value", S()));
		if (envc < MAX_ENV)
			if (char *p = keep(pair)) envp[envc++] = p; });

	/*
	 * A program header table for relibc with the one entry it looks for,
	 * PT_TLS, which makes it set up its TCB (with emulated TLS, the
	 * template itself is empty).
	 */
	struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
	enum { PT_TLS = 7 };
	static Phdr phdr { .type = PT_TLS, .flags = 4, .offset = 0, .vaddr = 0, .paddr = 0,
	                   .filesz = 0, .memsz = 0, .align = 8 };

	enum { AT_NULL = 0, AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_PAGESZ = 6 };

	unsigned i = 0;
	stack[i++] = argc;
	for (int a = 0; a < argc; a++) stack[i++] = (unsigned long)argv[a];
	stack[i++] = 0;
	for (int e = 0; e < envc; e++) stack[i++] = (unsigned long)envp[e];
	stack[i++] = 0;
	stack[i++] = AT_PHDR;   stack[i++] = (unsigned long)&phdr;
	stack[i++] = AT_PHENT;  stack[i++] = sizeof(Phdr);
	stack[i++] = AT_PHNUM;  stack[i++] = 1;
	stack[i++] = AT_PAGESZ; stack[i++] = 4096;
	stack[i++] = AT_NULL;   stack[i++] = 0;

	/* relibc's start-up, which returns here instead of running main */
	if (__builtin_setjmp(_relibc_ready) == 0)
		relibc_start_v1(stack, relibc_ready);

	construct_libc_component();
}
