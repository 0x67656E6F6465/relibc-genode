/*
 * \brief  fork, posix_spawn, waitpid and the process ids of the Genode shim
 *
 * posix_spawn starts the program's ROM module as a child component with
 * the parent's config, its arguments, environment, working directory and
 * fds: nothing to copy, from any thread.
 *
 * As with Genode's libc (src/lib/libc/fork.cc), the forking component
 * becomes the parent of a child that runs the same binary: it serves the
 * child the ROM modules of the binary and its libraries, a config, the fork
 * info, and a clone session, through which the child copies the parent's
 * memory. The child builds the shim (VFS, sessions, fds) anew from the
 * config (the parent's, with its fds and working directory), and takes
 * relibc's state from the parent:
 *
 * - relibc's data (relibc_state.ld), the data of the program and of its
 *   libraries but ld, libc, libm and vfs (the objects loaded before the
 *   VFS, whose plugins have state of their own),
 * - the anonymous mappings (relibc's heap, the TCBs, the emulated TLS), at
 *   the same addresses,
 * - the stack of the forking thread, on which the child continues from
 *   gxd_fork.
 *
 * The forking thread is the one of main (posix_main.cc), the first the shim
 * starts: the child's first thread has its stack at the same address. Only
 * that thread continues in the child, as with POSIX.
 */

/* (Genode's Dynamic_linker::for_each_loaded_object, a template, lacks an 'override') */
#pragma GCC diagnostic ignored "-Wsuggest-override"

#include <base/child.h>
#include <base/session_object.h>
#include <base/service.h>
#include <base/shared_object.h>
#include <base/attached_dataspace.h>
#include <base/connection.h>
#include <base/rpc_client.h>

#include "fd.h"

using namespace Gxd;

/* (relibc_state.ld, of libc.lib.so; the static relibc_genode library has none: no fork) */
extern "C" char __relibc_data_start[] __attribute__((weak)), __relibc_data_end[] __attribute__((weak));
extern "C" char __relibc_bss_start[]  __attribute__((weak)), __relibc_bss_end[]  __attribute__((weak));

namespace Gxd {
	struct Clone_session;
	struct Clone_connection;
	struct Fork_info;
}


/**
 * The child's access to the parent's memory
 */
struct Gxd::Clone_session : Session
{
	static const char *service_name() { return "Clone"; }

	static constexpr size_t   BUFFER_SIZE = 512*1024;
	static constexpr size_t   RAM_QUOTA   = BUFFER_SIZE + 4096;
	static constexpr unsigned CAP_QUOTA   = 2;

	struct Range { addr_t at; size_t size; };

	GENODE_RPC(Rpc_dataspace, Dataspace_capability, dataspace);
	GENODE_RPC(Rpc_copy, void, copy, Range);   /* into the dataspace */
	GENODE_RPC_INTERFACE(Rpc_dataspace, Rpc_copy);
};


struct Gxd::Clone_connection : Connection<Clone_session>, Rpc_client<Clone_session>
{
	Attached_dataspace const _buffer;

	Clone_connection(Env &env)
	:
		Connection<Clone_session>(env, Label(), Ram_quota { RAM_QUOTA }, Args()),
		Rpc_client<Clone_session>(cap()),
		_buffer(env.rm(), call<Rpc_dataspace>())
	{ }

	/* the parent's memory at 'at' to 'dst' */
	void copy(addr_t at, size_t size, char *dst)
	{
		while (size) {
			size_t const n = min(size, BUFFER_SIZE);
			call<Rpc_copy>(Clone_session::Range { at, n });
			memcpy(dst, _buffer.local_addr<char>(), n);
			at += n; dst += n; size -= n;
		}
	}

	/* the parent's memory at 'at' to the same place */
	void copy(addr_t at, size_t size) { copy(at, size, (char *)at); }
};


/**
 * What the child learns from the parent besides its config (a ROM module)
 */
struct Gxd::Fork_info
{
	enum { MAX_MAPPINGS = 1024 };

	struct Thread_state
	{
		unsigned long tp;
		void        **emutls;
		size_t        emutls_len;
		uint64_t      sigmask;
	};

	int pid, ppid;
	int tid, next_tid;           /* the forking thread's, the parent's next */

	Thread_state thread, ep;

	void  *context[5];           /* __builtin_setjmp's, in gxd_fork */
	addr_t stack_base, stack_top; /* of the forking thread */
	addr_t stack_from;            /* the part in use, to copy */

	unsigned num_mappings;
	struct { addr_t at; size_t size; } mappings[MAX_MAPPINGS];
};


namespace {

using Fork_info = Gxd::Fork_info;

/* where gxd_fork continues in the child */
void *_context[5];


/**
 * A ROM module the parent provides
 */
struct Local_rom_service : Noncopyable
{
	struct Session : Session_object<Rom_session>
	{
		Rom_dataspace_capability _ds;

		static Session::Resources _resources() {
			return { .ram_quota = { 0 }, .cap_quota = { Rom_session::CAP_QUOTA } }; }

		Session(Entrypoint &ep, Session_label const &name, Rom_dataspace_capability ds)
		: Session_object<Rom_session>(ep.rpc_ep(), _resources(), name), _ds(ds) { }

		Rom_dataspace_capability dataspace() override { return _ds; }

		void sigh(Signal_context_capability) override { }

	} _session;

	Local_service<Session>::Single_session_factory _factory { _session };
	Local_service<Session>                         service  { _factory };

	bool matches(Session_label const &label) const {
		return label.last_element() == _session.label(); }

	Local_rom_service(Entrypoint &ep, Session_label const &name, Rom_dataspace_capability ds)
	: _session(ep, name, ds) { }

	virtual ~Local_rom_service() { }
};


static Rom_dataspace_capability rom_cap(Attached_ram_dataspace const &ds)
{
	Dataspace_capability cap = ds.cap();
	return static_cap_cast<Rom_dataspace>(cap);
}


/**
 * The binary and libraries, as loaded
 */
struct Local_rom_services : Noncopyable
{
	Allocator &_alloc;

	Registry<Registered<Local_rom_service>> _services { };

	Local_rom_services(Env &env, Entrypoint &ep, Allocator &alloc) : _alloc(alloc)
	{
		Dynamic_linker::for_each_loaded_object(env, [&] (Dynamic_linker::Object_info const &info) {
			new (alloc) Registered<Local_rom_service>(_services, ep, info.name, info.ds_cap); });
	}

	~Local_rom_services()
	{
		_services.for_each([&] (Registered<Local_rom_service> &s) { destroy(_alloc, &s); });
	}

	Service *matching_service(Session_label const &label)
	{
		Service *service = nullptr;
		_services.for_each([&] (Local_rom_service &s) {
			if (!service && s.matches(label)) service = &s.service; });
		return service;
	}
};


/**
 * The services of the parent's parent, for the rest
 */
struct Parent_services : Noncopyable
{
	Env       &_env;
	Allocator &_alloc;

	Registry<Registered<Parent_service>> _services { };

	Parent_services(Env &env, Allocator &alloc) : _env(env), _alloc(alloc) { }

	~Parent_services()
	{
		_services.for_each([&] (Registered<Parent_service> &s) { destroy(_alloc, &s); });
	}

	Service &matching_service(Service::Name const &name)
	{
		Service *service = nullptr;
		_services.for_each([&] (Parent_service &s) {
			if (!service && name == s.name()) service = &s; });
		if (service) return *service;
		return *new (_alloc) Registered<Parent_service>(_services, _env, name);
	}
};


/**
 * The clone session of one child: its end (the session's close) means the
 * child is ready
 */
struct Local_clone_service : Noncopyable
{
	struct Session : Session_object<Clone_session, Session>
	{
		Attached_ram_dataspace _ds;

		static Session::Resources _resources() {
			return { .ram_quota = { Clone_session::RAM_QUOTA },
			         .cap_quota = { Clone_session::CAP_QUOTA } }; }

		Session(Env &env, Entrypoint &ep)
		:
			Session_object<Clone_session, Session>(ep.rpc_ep(), _resources(), "cloned"),
			_ds(env.ram(), env.rm(), Clone_session::BUFFER_SIZE)
		{ }

		Dataspace_capability dataspace() { return _ds.cap(); }

		void copy(Clone_session::Range range) {
			memcpy(_ds.local_addr<void>(), (void const *)range.at, range.size); }

	} _session;

	struct Factory : Local_service<Session>::Factory
	{
		Session                  &_session;
		Signal_context_capability _ready_sigh;

		Factory(Session &session, Signal_context_capability ready_sigh)
		: _session(session), _ready_sigh(ready_sigh) { }

		Result create(Args const &, Affinity) override { return _session; }

		void upgrade(Session &, Args const &) override { }

		void destroy(Session &) override { Signal_transmitter(_ready_sigh).submit(); }

	} _factory;

	Local_service<Session> service { _factory };

	Local_clone_service(Env &env, Entrypoint &ep, Signal_context_capability ready_sigh)
	: _session(env, ep), _factory(_session, ready_sigh) { }
};


/**
 * The parent's record of a child: forked (with the clone session and the
 * fork info), or spawned (of another program's ROM module)
 */
struct Child_process : Child_policy
{
	Env &_env;

	int const pid;

	Binary_name const _binary;
	bool        const _forked;

	enum class State { STARTING_UP, RUNNING, EXITED } state;

	int exit_code = 0;

	Name const _name { pid };

	Parent_services    &_parent_services;
	Local_rom_services &_local_rom_services;

	Attached_ram_dataspace const &_config;
	Attached_ram_dataspace const &_info;

	/* on the entrypoint: the fork, waitpid and SIGCHLD go on */
	void _handle_ready()
	{
		if (state == State::STARTING_UP) state = State::RUNNING;
		examine_ep_jobs();
	}

	void _handle_exit();

	Io_signal_handler<Child_process> _ready_handler { _env.ep(), *this, &Child_process::_handle_ready };
	Io_signal_handler<Child_process> _exit_handler  { _env.ep(), *this, &Child_process::_handle_exit };

	Local_clone_service _clone_service;
	Local_rom_service   _config_rom;
	Local_rom_service   _info_rom;

	Id_space<Parent::Server> _server_ids { };

	/* as Genode's libc gives, and the parent's mappings (more on request) */
	size_t   const _initial_ram;
	unsigned const _initial_caps;

	Child _child;

	/*
	 * Child_policy
	 */

	Name        name()        const override { return _name; }
	Binary_name binary_name() const override { return _binary; }

	Ram_allocator &session_md_ram() override { return _env.ram(); }

	Pd_account            &ref_account()           override { return _env.pd(); }
	Capability<Pd_account> ref_account_cap() const override { return _env.pd_session_cap(); }

	void init(Pd_session &session, Pd_session_capability cap) override
	{
		session.ref_account(_env.pd_session_cap());
		_env.pd().transfer_quota(cap, Ram_quota { _initial_ram });
		_env.pd().transfer_quota(cap, Cap_quota { _initial_caps });
	}

	void _with_route(Service::Name     const &name,
	                 Session_label     const &label,
	                 With_route::Ft    const &fn,
	                 With_no_route::Ft const &denied_fn) override
	{
		Service *service = nullptr;

		if (_forked && state == State::STARTING_UP && name == Clone_session::service_name())
			service = &_clone_service.service;

		if (name == Rom_session::service_name()) {
			if (!service) service = _local_rom_services.matching_service(label);
			if (!service && _config_rom.matches(label)) service = &_config_rom.service;
			if (!service && _forked && _info_rom.matches(label)) service = &_info_rom.service;
		}

		if (!service) service = &_parent_services.matching_service(name);

		/* as Genode's libc: labels without the chain of forks (LOG's kept) */
		Session_label const rewritten = (name == "LOG") ? label : label.last_element();

		if (service) fn(Route { .service = *service, .label = rewritten });
		else         denied_fn();
	}

	void resource_request(Parent::Resource_args const &args) override
	{
		Session::Resources const resources = session_resources_from_args(args.string());
		if (resources.ram_quota.value)
			_env.pd().transfer_quota(_child.pd_session_cap(), resources.ram_quota);
		if (resources.cap_quota.value)
			_env.pd().transfer_quota(_child.pd_session_cap(), resources.cap_quota);
		_child.notify_resource_avail();
	}

	/* on the fork entrypoint (an RPC): the rest on the entrypoint */
	void exit(int code) override
	{
		exit_code = code;
		state     = State::EXITED;
		Signal_transmitter(_exit_handler).submit();
	}

	Id_space<Parent::Server> &server_id_space() override { return _server_ids; }

	Child_process(Env &env, Entrypoint &fork_ep, int pid, Binary_name const &binary, bool forked,
	              Parent_services &parent_services, Local_rom_services &local_rom_services,
	              Attached_ram_dataspace const &config, Attached_ram_dataspace const &info,
	              size_t initial_ram, unsigned initial_caps)
	:
		_env(env), pid(pid), _binary(binary), _forked(forked),
		/* a spawned child has nothing to copy */
		state(forked ? State::STARTING_UP : State::RUNNING),
		_parent_services(parent_services), _local_rom_services(local_rom_services),
		_config(config), _info(info),
		_clone_service(env, fork_ep, _ready_handler),
		_config_rom(fork_ep, "config", rom_cap(config)),
		_info_rom(fork_ep, "relibc_fork", rom_cap(info)),
		_initial_ram(initial_ram), _initial_caps(initial_caps),
		_child(env.rm(), fork_ep.rpc_ep(), *this)
	{ }

	~Child_process() override { }
};


/* a child, with its config and fork info, filled before it starts */
struct Child_record
{
	Attached_ram_dataspace          config;
	Attached_ram_dataspace          info;
	Constructible<Child_process>    _child { };

	Child_process &child() { return *_child; }

	virtual ~Child_record() { }

	Child_record(Env &env, size_t config_size)
	:
		config(env.ram(), env.rm(), config_size),
		info(env.ram(), env.rm(), sizeof(Fork_info))
	{ }

	void start(Env &env, Entrypoint &ep, int pid, Child_policy::Binary_name const &binary,
	           bool forked, Parent_services &ps, Local_rom_services &lrs,
	           size_t initial_ram, unsigned initial_caps)
	{
		_child.construct(env, ep, pid, binary, forked, ps, lrs, config, info,
		                 initial_ram, initial_caps);
	}
};

using Children = Registry<Registered<Child_record>>;

Children &children() { static Children c; return c; }


/* whether SIGCHLD is caught or blocked (else it is discarded, as ignored) */
uint64_t _caught_signals;

bool sigchld_wanted()
{
	enum { SIGCHLD = 17 };
	uint64_t const bit = 1ull << (SIGCHLD - 1);
	if (__atomic_load_n(&_caught_signals, __ATOMIC_ACQUIRE) & bit) return true;
	Main &m = main();
	Mutex::Guard guard(m.threads_mutex);
	for (Relibc_thread const &t : m.threads)
		if (t.used && (t.sigmask & bit)) return true;
	return false;
}


void Child_process::_handle_exit()
{
	enum { SIGCHLD = 17 };
	if (sigchld_wanted()) gxd_signal_raise(SIGCHLD);
	examine_ep_jobs();
}


/* what a spawned child gets in place of its parent's state */
struct Spawn
{
	Program  const &program;
	char    *const *argv;
	char    *const *envp;
	int      const (*fds)[2];   /* the child's fd, the parent's */
	unsigned        num_fds;
	Abs_path const &cwd;
};


/* an fd of the parent for the child, as Genode's libc writes it (vfs_mutex held) */
void generate_fd(Generator &g, int id, Fd_object const &obj, bool cloexec)
{
	using Path = String<Vfs::MAX_PATH_LEN>;
	int const mode = obj.oflag & O_ACCMODE;
	g.node("fd", [&] {
		g.attribute("id", id);
		g.attribute("path", Path(*obj.path()));
		if (obj.oflag & O_DIRECTORY) g.attribute("directory", true);
		if (mode != O_WRONLY) g.attribute("readable",  true);
		if (mode != O_RDONLY) g.attribute("writeable", true);
		if (obj.oflag & O_APPEND)   g.attribute("append", true);
		if (obj.oflag & O_NONBLOCK) g.attribute("nonblocking", true);
		if (cloexec)                g.attribute("cloexec", true);
		long long const pos = obj.position();
		if (pos > 0) g.attribute("seek", pos);
	});
}


/*
 * The child's config: the parent's, with the fds ('+ libc | + fd'), the
 * working directory, and for a fork its mark, for a spawn the arguments and
 * the environment (vfs_mutex held)
 */
void generate_config(Generator &g, Node const &config, int pid, Spawn const *spawn)
{
	Main &m = main();

	/* a forked child: the program's static constructors ran in the parent */
	if (!spawn) g.attribute("ld_check_ctors", "no");

	config.for_each_sub_node([&] (Node const &node) {
		bool const replaced = node.type() == "libc" || node.type() == "relibc_fork"
		                   || (spawn && (node.type() == "arg" || node.type() == "env"));
		if (!replaced && !g.append_node(node, Generator::Max_depth { 20 }))
			warning("relibc: the config of a child: too deeply nested"); });

	if (spawn) {
		/* a script: its interpreter, the script, and the arguments after argv[0] */
		Program const &p = spawn->program;
		auto arg = [&] (char const *value) { g.node("arg", [&] { g.attribute("value", value); }); };
		for (int i = 0; i < p.prefix_count; i++) arg(p.prefix[i]);
		for (char *const *a = spawn->argv + (p.prefix_count ? 1 : 0); a && *a; a++) arg(*a);

		for (char *const *e = spawn->envp; e && *e; e++) {
			char const *eq = *e;
			while (*eq && *eq != '=') eq++;
			if (!*eq) continue;
			g.node("env", [&] {
				g.attribute("key", *e, eq - *e);
				g.attribute("value", eq + 1); });
		}
	}

	using Path = String<Vfs::MAX_PATH_LEN>;
	g.node("libc", [&] {
		config.with_optional_sub_node("libc", [&] (Node const &libc) {
			static char const * const attrs[] { "stdin", "stdout", "stderr", "rtc", "rng", "pipe", "socket" };
			for (char const *attr : attrs)
				if (libc.has_attribute(attr))
					g.attribute(attr, libc.attribute_value(attr, Path()));
		});
		g.attribute("cwd", Path(spawn ? spawn->cwd : m.cwd));
		g.attribute("pid",  pid);
		g.attribute("ppid", m.pid);

		auto with_path = [&] (int fd, auto const &fn) {
			if (fd < 0 || fd >= (int)Main::MAX_FDS) return;
			if (Fd_object const *obj = m.fds[fd])
				if (obj->path()) fn(*obj);
		};
		if (spawn) {
			for (unsigned i = 0; i < spawn->num_fds; i++)
				with_path(spawn->fds[i][1], [&] (Fd_object const &obj) {
					generate_fd(g, spawn->fds[i][0], obj, false); });
		} else {
			for (int fd = 0; fd < (int)Main::MAX_FDS; fd++)
				with_path(fd, [&] (Fd_object const &obj) { generate_fd(g, fd, obj, m.cloexec[fd]); });
		}
	});

	if (!spawn) g.node("relibc_fork");
}


/* on the entrypoint: a fork's child (with the snapshot), or a spawn's */
Registered<Child_record> &create_child(Fork_info const *snapshot, Spawn const *spawn)
{
	Main &m = main();
	Env  &env = m.env;

	enum { STACK_SIZE = 32*1024 };
	static Entrypoint         fork_ep(env, STACK_SIZE, "fork_ep", Affinity::Location());
	static Parent_services    parent_services(env, m.heap);
	static Local_rom_services local_rom_services(env, fork_ep, m.heap);

	static int pid_count = m.pid;
	int const pid = ++pid_count;

	/* as Genode's libc gives a forked child, and the memory it copies */
	size_t   ram  = 2500*1000;
	unsigned caps = 100;
	if (snapshot)
		for (unsigned i = 0; i < snapshot->num_mappings; i++) {
			ram += snapshot->mappings[i].size;
			caps++;
		}

	for (size_t config_size = 4096; ; config_size += 4096) {
		Registered<Child_record> &record = *new (m.heap) Registered<Child_record>(children(), env, config_size);
		bool ok = false;
		{
			Mutex::Guard guard(m.vfs_mutex);
			ok = Generator::generate({ record.config.local_addr<char>(), config_size }, "config",
				[&] (Generator &g) { generate_config(g, m.config.node(), pid, spawn); }).ok();
		}
		if (!ok) { destroy(m.heap, &record); continue; }

		if (snapshot) {
			Fork_info &info = *record.info.local_addr<Fork_info>();
			info     = *snapshot;
			info.pid = pid;
		}
		record.start(env, fork_ep, pid,
		             spawn ? Child_policy::Binary_name(spawn->program.rom) : Child_policy::Binary_name("binary"),
		             !spawn, parent_services, local_rom_services, ram, caps);
		return record;
	}
}

} /* anonymous namespace */


/* SIGCHLD's disposition, from relibc's sigaction (signal.rs) */
extern "C" void gxd_set_caught_signals(uint64_t mask)
{
	__atomic_store_n(&_caught_signals, mask, __ATOMIC_RELEASE);
}


extern "C" int gxd_getpid()  { return main().pid; }
extern "C" int gxd_getppid() { return main().ppid; }


/* the rest of the fork in the child, after its jump back here */
static long _resumed_in_child()
{
	/* (as Genode's libc: the linker's data on its objects is stale after the clone) */
	Dynamic_linker::update_dependencies();
	return 0;
}


extern "C" long gxd_fork()
{
	Main &m = main();
	Relibc_thread &me = thread_state();
	if (!__relibc_data_start) {
		warning("relibc: fork: only with relibc as the libc (libc.lib.so)");
		return -ENOSYS;
	}
	if (m.on_entrypoint() || !m.main_tid || me.tid != m.main_tid) {
		warning("relibc: fork: only from the thread of main");
		return -ENOSYS;
	}

	/* the child continues here, with a result of 1 */
	if (__builtin_setjmp(_context))
		return _resumed_in_child();

	static Fork_info snapshot;   /* (the forks of the thread of main follow each other) */
	Fork_info &s = snapshot;

	s.ppid = m.pid;
	s.tid  = me.tid;
	s.thread = { me.tp, me.emutls, me.emutls_len, me.sigmask };
	{
		Mutex::Guard guard(m.threads_mutex);
		s.next_tid = m.next_tid;
		for (Relibc_thread const &t : m.threads)
			if (t.used && t.thread == m.ep_thread)
				s.ep = { t.tp, t.emutls, t.emutls_len, t.sigmask };
	}
	memcpy(s.context, _context, sizeof(_context));

	/* from below the frame of gxd_fork (sp in __builtin_setjmp's buffer) */
	s.stack_from = ((addr_t)_context[2] - 1024) & ~(addr_t)15;
	s.stack_base = Thread::mystack().base;
	s.stack_top  = Thread::mystack().top;

	/* the mappings stay as they are while the child copies them */
	Mutex::Guard memory_guard(m.memory_mutex);
	s.num_mappings = 0;
	bool too_many = false;
	m.mappings.for_each([&] (Mapping const &mapping) {
		if (s.num_mappings == Fork_info::MAX_MAPPINGS) { too_many = true; return; }
		s.mappings[s.num_mappings++] = { mapping.at, mapping.size }; });
	if (too_many) {
		warning("relibc: fork: more than ", (unsigned)Fork_info::MAX_MAPPINGS, " mappings");
		return -ENOMEM;
	}

	Registered<Child_record> *record = nullptr;
	if (long r = ep_call([&] () -> long {
		try { record = &create_child(&s, nullptr); return 0; }
		catch (...) { return -EAGAIN; } }))
		return r;

	/* until the child copied what it needs (or ended) */
	on_ep([&] { return record->child().state != Child_process::State::STARTING_UP; });
	return record->child().pid;
}


/*
 * posix_spawn: the program of 'path' as a child, with the parent's fds of
 * the map (relibc's file actions applied to them, spawn.rs), its working
 * directory 'cwd' (relative to the directory fd 'cwd_fd', if not -1)
 */
extern "C" long gxd_spawn(char const *path, char *const argv[], char *const envp[],
                          int const (*fds)[2], unsigned num_fds,
                          char const *cwd, int cwd_fd)
{
	Main &m = main();
	Program program { };
	Registered<Child_record> *record = nullptr;

	long const r = ep_call([&] () -> long {
		if (long r = find_program(path, program)) return r;

		Abs_path dir;
		if (long r = resolve(cwd_fd >= 0 ? cwd_fd : AT_FDCWD, cwd ? cwd : ".", dir)) return r;
		if (!m.vfs.root_dir().directory(dir.string())) return -ENOTDIR;

		Spawn const spawn { program, argv, envp, fds, num_fds, dir };
		try { record = &create_child(nullptr, &spawn); return 0; }
		catch (...) { return -EAGAIN; }
	});

	for (int i = 0; i < program.prefix_count; i++)
		m.heap.free(program.prefix[i], strlen(program.prefix[i]) + 1);

	return r ? r : record->child().pid;
}


/* wait4's: the pid, 0 (WNOHANG, none exited), or -errno */
extern "C" long gxd_waitpid(int pid, int *status, int options)
{
	enum { WNOHANG = 1 };
	Main &m = main();
	uint64_t const mask = thread_state().sigmask;

	long result = 0;
	on_ep([&] {
		Registered<Child_record> *any = nullptr, *exited = nullptr;
		children().for_each([&] (Registered<Child_record> &r) {
			if (pid > 0 && r.child().pid != pid) return;
			any = &r;
			if (!exited && r.child().state == Child_process::State::EXITED) exited = &r;
		});
		if (!any) { result = -ECHILD; return true; }
		if (exited) {
			result = exited->child().pid;
			if (status) *status = (exited->child().exit_code & 0xff) << 8;
			destroy(m.heap, exited);
			return true;
		}
		if (options & WNOHANG)                  { result = 0;      return true; }
		if (gxd_signals_pending() & ~mask)      { result = -EINTR; return true; }
		return false;
	});
	return result;
}


/*
 * The child's start, on the entrypoint (start.cc), in place of relibc's
 */

namespace {

/* the objects whose data the child takes over, as loaded before the VFS */
struct Copied_object { addr_t at; size_t size; };
enum { MAX_COPIED_OBJECTS = 64 };
Copied_object _copied_objects[MAX_COPIED_OBJECTS];
unsigned      _num_copied_objects;

/* the forking thread's stack (the part in use), until the thread runs */
char  *_stack_copy;
addr_t _stack_from, _stack_top;

/* a stack for copying that one in place */
alignas(16) char _switch_stack[4096];


void _resume()
{
	memcpy((void *)_stack_from, _stack_copy, _stack_top - _stack_from);
	__builtin_longjmp(_context, 1);
}


[[noreturn]] void _call_on_switch_stack(void (*fn)())
{
	asm volatile ("mov %0, %%rsp\n"
	              "xor %%rbp, %%rbp\n"
	              "call *%1\n"
	              :: "r" ((addr_t)(_switch_stack + sizeof(_switch_stack)) & ~(addr_t)15),
	                 "r" (fn) : "memory");
	__builtin_unreachable();
}


/* the child's thread of main, with the parent's stack */
struct Forked_thread : Thread
{
	Forked_thread(Env &env, int tid, size_t stack_size)
	: Thread(env, Name("relibc-", tid), Stack_size { stack_size }) { }

	void entry() override
	{
		Stack_info const stack = mystack();
		if (stack.top != _stack_top || stack.base > _stack_from) {
			error("relibc: fork: the stack of main is elsewhere than in the parent");
			main().env.parent().exit(-1);
			sleep_forever();
		}
		_call_on_switch_stack(_resume);
	}
};

} /* anonymous namespace */


void Gxd::note_loaded_objects(Env &env)
{
	_num_copied_objects = 0;
	Dynamic_linker::for_each_loaded_object(env, [&] (Dynamic_linker::Object_info const &info) {
		bool const own = info.name == "ld.lib.so" || info.name == "libc.lib.so"
		              || info.name == "libm.lib.so" || info.name == "vfs.lib.so";
		if (own || !info.rw_size || _num_copied_objects == MAX_COPIED_OBJECTS) return;
		_copied_objects[_num_copied_objects++] = { (addr_t)info.rw_start, info.rw_size };
	});
}


bool Gxd::forked() { return main().config.node().has_sub_node("relibc_fork"); }


void Gxd::clone_from_parent()
{
	Main &m = main();
	Env  &env = m.env;

	/* the fork info (on the stack: its size is bounded) */
	Fork_info info;
	{
		Attached_rom_dataspace rom(env, "relibc_fork");
		memcpy(&info, rom.local_addr<void>(), min(rom.size(), sizeof(info)));
	}

	/* the parent's mappings, at the same addresses (in their window) */
	for (unsigned i = 0; i < info.num_mappings; i++) {
		auto const &mapping = info.mappings[i];
		if (map_at(mapping.at, mapping.size) < 0) {
			error("relibc: fork: cannot place the memory at ", Hex(mapping.at),
			      " (", mapping.size, " bytes) as in the parent");
			env.parent().exit(-1);
			sleep_forever();
		}
	}

	Clone_connection clone(env);
	for (unsigned i = 0; i < info.num_mappings; i++)
		clone.copy(info.mappings[i].at, info.mappings[i].size);

	clone.copy((addr_t)__relibc_data_start, __relibc_data_end - __relibc_data_start);
	clone.copy((addr_t)__relibc_bss_start,  __relibc_bss_end  - __relibc_bss_start);
	for (unsigned i = 0; i < _num_copied_objects; i++)
		clone.copy(_copied_objects[i].at, _copied_objects[i].size);
	clone.copy((addr_t)&tls_memory, sizeof(tls_memory));

	_stack_from = info.stack_from;
	_stack_top  = info.stack_top;
	_stack_copy = (char *)m.heap.alloc(_stack_top - _stack_from);
	clone.copy(_stack_from, _stack_top - _stack_from, _stack_copy);
	memcpy(_context, info.context, sizeof(_context));

	m.pid      = info.pid;
	m.ppid     = info.ppid;
	m.next_tid = info.next_tid;
	m.main_tid = info.tid;

	auto take = [] (Relibc_thread &t, Fork_info::Thread_state const &s) {
		t.tp = s.tp; t.emutls = s.emutls; t.emutls_len = s.emutls_len; t.sigmask = s.sigmask; };

	take(thread_state(), info.ep);

	/* the thread of main, on a stack where the parent's was */
	Forked_thread *thread = new (m.heap) Forked_thread(env, info.tid, info.stack_top - info.stack_base);
	{
		Mutex::Guard guard(m.threads_mutex);
		Relibc_thread *free = nullptr;
		for (Relibc_thread &t : m.threads)
			if (!t.used) { free = &t; break; }
		free->used = true; free->thread = thread; free->tid = info.tid;
		take(*free, info.thread);
	}

	/* the fds of the parent, from the config, before it goes on (the
	   clone session's end, here) */
	init_fds();
}


void Gxd::start_forked_thread()
{
	Main &m = main();
	Mutex::Guard guard(m.threads_mutex);
	for (Relibc_thread &t : m.threads)
		if (t.used && t.tid == m.main_tid && t.thread != m.ep_thread) {
			Thread &thread = *const_cast<Thread *>(t.thread);
			thread.start();
			return;
		}
}
