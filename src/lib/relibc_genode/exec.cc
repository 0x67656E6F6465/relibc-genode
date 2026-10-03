/*
 * \brief  execve of the Genode shim of relibc
 *
 * As Genode's libc does it (src/lib/libc/execve.cc): Genode's dynamic
 * linker replaces the program within the component (Dynamic_linker::
 * respawn), the libraries stay, and the new main starts on the calling
 * thread's stack, from its top. The executable is the ROM module of its
 * (resolved) path, which the VFS must show; a script ("#!") names its
 * interpreter. relibc's
 * platform resets what belongs to the program before (atexit functions,
 * signal handlers, getopt, the other threads), the shim closes the fds with
 * FD_CLOEXEC; everything else stays: the fds, the VFS, the environment
 * given.
 */

#include <base/attached_rom_dataspace.h>
#include <base/shared_object.h>

#include "state.h"

using namespace Gxd;

extern "C" char **environ;            /* relibc's */
extern "C" void exit(int) __attribute__((noreturn));

namespace {

using Main_fn = int (*)(int, char **, char **);

/* the loaded program, until gxd_exec_run starts it */
struct Exec_state
{
	Main_fn main_fn = nullptr;
	Program program { };

	/* the new program's argv and environment (on the heap: never freed) */
	int     argc = 0;
	char  **argv = nullptr;
	char  **envp = nullptr;
};

/*
 * Constructed on first use: the shim has no static constructors, which a
 * forked child would run only at its execve (fork.cc)
 */
Exec_state &exec_state()
{
	static Exec_state *state;
	if (!state) state = new (main().heap) Exec_state();
	return *state;
}


char *copy_string(char const *s)
{
	size_t const n = strlen(s) + 1;
	char *p = (char *)main().heap.alloc(n);
	memcpy(p, s, n);
	return p;
}


/* a null-terminated array: 'prefix' (count 'n'), then 'array', copied */
char **copy_array(char *const *prefix, int n, char *const *array, int &count)
{
	int m = 0;
	while (array && array[m]) m++;
	count = n + m;
	char **a = (char **)main().heap.alloc((count + 1)*sizeof(char *));
	for (int i = 0; i < n; i++) a[i]     = copy_string(prefix[i]);
	for (int i = 0; i < m; i++) a[n + i] = copy_string(array[i]);
	a[count] = nullptr;
	return a;
}


/* the "#!" line of a script: interpreter and arguments into 'program' */
bool parse_shebang(char const *text, size_t len, Abs_path &interpreter, Program &program)
{
	if (len < 2 || text[0] != '#' || text[1] != '!') return false;
	size_t i = 2;
	auto skip_blank = [&] { while (i < len && (text[i] == ' ' || text[i] == '\t')) i++; };
	auto word = [&] (char *buf, size_t size) {
		size_t n = 0;
		while (i < len && text[i] != ' ' && text[i] != '\t' && text[i] != '\n') {
			if (n + 1 < size) buf[n++] = text[i];
			i++;
		}
		buf[n] = 0;
		return n;
	};
	char buf[256];
	skip_blank();
	if (!word(buf, sizeof(buf))) return false;
	interpreter = Abs_path(Cstring(buf));
	program.prefix[program.prefix_count++] = copy_string(buf);
	for (;;) {
		skip_blank();
		if (i >= len || text[i] == '\n' || program.prefix_count + 1 >= Program::MAX_PREFIX) break;
		if (word(buf, sizeof(buf))) program.prefix[program.prefix_count++] = copy_string(buf);
	}
	return true;
}

} /* anonymous namespace */


long Gxd::find_program(char const *path, Program &program)
{
	Main &m = main();
	program.prefix_count = 0;

	Abs_path resolved;
	if (long r = resolve(AT_FDCWD, path, resolved)) return r;

	enum { MAX_NESTING = 4 };
	for (unsigned i = 0; i < MAX_NESTING; i++) {

		/* in the VFS (as with Genode's libc), else a missing ROM module would stop us */
		Vfs::Directory_service::Stat st { };
		if (m.vfs.root_dir().stat(resolved.string(), st) != Vfs::Directory_service::STAT_OK)
			return -ENOENT;
		if (st.type == Vfs::Node_type::DIRECTORY)
			return -EACCES;

		try {
			Attached_rom_dataspace rom(m.env, resolved.string());
			char const *text = rom.local_addr<char const>();
			size_t const size = rom.size();
			if (size >= 4 && !memcmp(text, "\x7f" "ELF", 4)) { program.rom = resolved; return 0; }

			Abs_path interpreter;
			if (!parse_shebang(text, size, interpreter, program)) return -ENOEXEC;
			if (i == 0)  /* the script itself, after the interpreter's arguments */
				program.prefix[program.prefix_count++] = copy_string(resolved.string());
			resolved = interpreter;
		}
		catch (...) {
			warning("relibc: no ROM module ", resolved, " for ", Cstring(path));
			return -ENOENT;
		}
	}
	return -ELOOP;
}


namespace {

/* the new program's entry, on the fresh stack */
void exec_entry()
{
	main().env.exec_static_constructors();
	Exec_state &x = exec_state();
	exit(x.main_fn(x.argc, x.argv, x.envp));
}


/* switch to the stack 'sp' and call 'fn' there (x86_64) */
[[noreturn]] void call_on_stack(addr_t sp, void (*fn)())
{
	asm volatile ("mov %0, %%rsp\n"
	              "xor %%rbp, %%rbp\n"
	              "call *%1\n"
	              :: "r" (sp & ~(addr_t)15), "r" (fn) : "memory");
	__builtin_unreachable();
}

} /* anonymous namespace */


/*
 * Resolve 'path', check its ROM module (a script's interpreter, for one),
 * and load it in place of the running program: 0, or -errno (the running
 * program unchanged)
 */
extern "C" long gxd_exec_load(char const *path)
{
	Main &m = main();
	if (m.on_entrypoint()) return -ENOSYS;   /* main is a thread of its own */

	Exec_state &x = exec_state();
	long const r = ep_call([&] { return find_program(path, x.program); });
	if (r) return r;

	/* the libraries stay, the binary is replaced */
	Dynamic_linker::keep(m.env, "libc.lib.so");
	Dynamic_linker::keep(m.env, "libm.lib.so");
	Dynamic_linker::keep(m.env, "posix.lib.so");
	Dynamic_linker::keep(m.env, "vfs.lib.so");
	try {
		x.main_fn = Dynamic_linker::respawn<Main_fn>(m.env, x.program.rom.string(), "main");
	}
	catch (...) {
		error("relibc: execve: cannot load ", x.program.rom);
		m.env.parent().exit(-1);   /* the old program is gone */
		sleep_forever();
	}
	return 0;
}


/* start the loaded program, on the calling thread, with these arguments */
extern "C" void gxd_exec_run(char *const argv[], char *const envp[])
{
	/* a script: its interpreter with the script and the arguments after argv[0] */
	Exec_state &x = exec_state();
	Program const &p = x.program;
	char *const *args = argv;
	if (p.prefix_count && argv && argv[0]) args = argv + 1;

	x.argv = copy_array(p.prefix, p.prefix_count, args, x.argc);
	int envc;
	x.envp = copy_array(nullptr, 0, envp, envc);
	environ = x.envp;

	close_cloexec_fds();
	main().out.flush();

	call_on_stack(Thread::mystack().top, exec_entry);
}
