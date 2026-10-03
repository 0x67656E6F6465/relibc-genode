/*
 * \brief  Threads of the Genode shim of relibc
 *
 * Each relibc thread is a Genode thread with a record here: relibc's thread
 * pointer (its TCB), the emulated TLS (__emutls_get_address), and an id.
 * relibc prepares a thread's start like for Linux's clone: on the stack it
 * allocated, the entry point (its new_thread_shim) followed by its
 * arguments. The Genode thread runs on a Genode stack instead, reads those
 * words and makes the call.
 *
 * Futexes: waiters queue per address and block on a Blockade; a timed wait
 * also ends through a deadline of the entrypoint (ep.cc). On the
 * entrypoint itself, a wait dispatches I/O signals meanwhile.
 *
 * Ending: an exited thread blocks until the next gxd_thread_create reaps it
 * (destroys its Genode thread). pthread_cancel, for which relibc sends a
 * signal on Linux, stops the Genode thread at once, as Genode's libc does
 * (gxd_thread_cancel): asynchronous, like relibc's signal handler there.
 */

#include <libc/component.h>

#include "state.h"

using namespace Gxd;

namespace Gxd {

	struct Futex_waiter : List<Futex_waiter>::Element
	{
		uint32_t      *addr;
		Futex_waiter *&slot;    /* the waiting thread's record: 'waiting' */
		Blockade       blockade { };
		bool           woken = false;
		bool     const on_ep = main().on_entrypoint();

		Futex_waiter(uint32_t *addr, Futex_waiter *&slot) : addr(addr), slot(slot) { }

		Futex_waiter(Futex_waiter const &) = delete;
		Futex_waiter &operator = (Futex_waiter const &) = delete;
	};
}


struct Gxd::Relibc_genode_thread : Thread
{
	enum { MIN_STACK = 64*1024 };

	/*
	 * The size the thread asked for, which relibc's pthread_create passes
	 * beside the start arguments (the stack it maps carries only those)
	 */
	static size_t _stack_size(unsigned long const *stack)
	{
		size_t const wanted = max((size_t)stack[5], (size_t)MIN_STACK);
		return min(wanted, Thread::stack_virtual_size() - 64*1024);
	}

	using Entry = void (*)(unsigned long, unsigned long, unsigned long,
	                       unsigned long, unsigned long, unsigned long);

	Entry         _entry;
	unsigned long _args[6];

	static Name _name(int n) { return Name("relibc-", n); }

	Relibc_genode_thread(Env &env, int n, unsigned long const *stack)
	:
		Thread(env, _name(n), Stack_size { _stack_size(stack) }),
		_entry((Entry)stack[0]),
		_args { stack[1], stack[2], stack[3], stack[4], 0, 0 }
	{ }

	void entry() override
	{
		_entry(_args[0], _args[1], _args[2], _args[3], _args[4], _args[5]);
		gxd_thread_exit();  /* not reached: the shim exits through relibc */
	}
};


/*
 * The memory of the emulated TLS: anonymous mappings, which a forked child
 * gets with the rest of relibc's memory, at the same addresses (fork.cc).
 * Its state, zero-initialized, goes to the child as well. With
 * threads_mutex held.
 */
Gxd::Tls_memory Gxd::tls_memory;

static void *tls_alloc(size_t size)
{
	Tls_memory &tm = tls_memory;
	size = (size + 15) & ~(size_t)15;
	if (size > Tls_memory::MAX_SMALL) {
		long const r = gxd_mmap_anon(size);
		if (r < 0) { error("relibc: out of memory for TLS"); sleep_forever(); }
		return (void *)r;
	}
	Tls_memory::Free *&free = tm.free[size/16 - 1];
	if (free) { void *p = free; free = free->next; return p; }

	if (tm.left < size) {
		long const r = gxd_mmap_anon(Tls_memory::CHUNK);
		if (r < 0) { error("relibc: out of memory for TLS"); sleep_forever(); }
		tm.chunk = (char *)r; tm.left = Tls_memory::CHUNK;
	}
	void *p = tm.chunk;
	tm.chunk += size; tm.left -= size;
	return p;
}

static void tls_free(void *p, size_t size)
{
	size = (size + 15) & ~(size_t)15;
	if (size > Tls_memory::MAX_SMALL) { gxd_munmap(p, size); return; }
	Tls_memory::Free *&free = tls_memory.free[size/16 - 1];
	free = construct_at<Tls_memory::Free>(p, free);
}


/* with threads_mutex held */
static void release(Relibc_thread &t)
{
	Main &m = main();
	if (t.genode_thread) destroy(m.heap, t.genode_thread);
	if (t.emutls) tls_free(t.emutls, t.emutls_len*sizeof(void *));
	t.emutls = nullptr; t.emutls_len = 0; t.tp = 0;
	t.thread = nullptr; t.genode_thread = nullptr; t.waiting = nullptr;
	t.exited = false;
	t.used = false;
}


Relibc_thread &Gxd::thread_state()
{
	Main &m = main();
	Thread const *me = Thread::myself();

	Mutex::Guard guard(m.threads_mutex);
	Relibc_thread *free = nullptr;
	for (Relibc_thread &t : m.threads) {
		if (t.used && t.thread == me) return t;
		if (!free && !t.used) free = &t;
	}
	if (!free) {
		error("relibc: more than ", (unsigned)Main::MAX_THREADS, " threads");
		sleep_forever();
	}
	free->used   = true;
	free->thread = me;
	free->tid    = m.next_tid++;
	return *free;
}


extern "C" long gxd_set_tp(unsigned long tp)
{
	thread_state().tp = tp;
	return 0;
}


extern "C" unsigned long gxd_get_tp() { return thread_state().tp; }


extern "C" int gxd_gettid() { return thread_state().tid; }


/**
 * Emulated TLS (the ABI of GCC's and LLVM's emutls): one control structure
 * per thread-local variable; each thread gets its own copy on first use.
 */
struct __emutls_control
{
	size_t size;
	size_t align;
	union { size_t index; void *address; } object;
	void  *templ;
};

/*
 * relibc's (libc.mk links it with '--wrap=__emutls_get_address'); the
 * dynamic linker's serves the rest
 */
extern "C" __attribute__((visibility("hidden")))
void *__wrap___emutls_get_address(__emutls_control *control)
{
	size_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
	if (!index) {
		Mutex::Guard guard(main().threads_mutex);
		index = control->object.index;
		if (!index) {
			index = ++tls_memory.next_index;
			__atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
		}
	}

	Relibc_thread &t = thread_state();
	if (index <= t.emutls_len && t.emutls[index - 1])
		return t.emutls[index - 1];

	Mutex::Guard guard(main().threads_mutex);
	if (index > t.emutls_len) {
		size_t const len = max(max(index, 2*t.emutls_len), (size_t)16);
		void **table = (void **)tls_alloc(len*sizeof(void *));
		for (size_t i = 0; i < len; i++) table[i] = i < t.emutls_len ? t.emutls[i] : nullptr;
		if (t.emutls) tls_free(t.emutls, t.emutls_len*sizeof(void *));
		t.emutls = table; t.emutls_len = len;
	}

	void *&object = t.emutls[index - 1];
	if (!object) {
		size_t const align = control->align ? control->align : sizeof(void *);
		char *raw = (char *)tls_alloc(control->size + align);
		char *obj = (char *)(((unsigned long)raw + align - 1) & ~(align - 1));
		if (control->templ) memcpy(obj, control->templ, control->size);
		else for (size_t i = 0; i < control->size; i++) obj[i] = 0;
		object = obj;
	}
	return object;
}


/* a pthread's that asks for none, as with Genode's libc */
extern "C" unsigned long gxd_default_stack_size() { return Libc::Component::stack_size(); }


extern "C" long gxd_thread_create(unsigned long *stack)
{
	Main &m = main();
	Relibc_genode_thread *thread = nullptr;
	int tid;
	{
		/* the record exists before the thread runs, so that its id is known */
		Mutex::Guard guard(m.threads_mutex);
		for (Relibc_thread &t : m.threads)
			if (t.used && t.exited) release(t);

		Relibc_thread *free = nullptr;
		for (Relibc_thread &t : m.threads)
			if (!t.used) { free = &t; break; }
		if (!free) return -EAGAIN;
		tid = m.next_tid++;
		try { thread = new (m.heap) Relibc_genode_thread(m.env, tid, stack); }
		catch (...) { return -EAGAIN; }
		free->used = true; free->thread = thread; free->tid = tid;
		free->genode_thread = thread;
		if (!m.main_tid) m.main_tid = tid;   /* the first: main's (posix_main.cc) */
	}
	if (thread->start() != Thread::Start_result::OK) return -EAGAIN;
	return tid;
}


extern "C" void gxd_thread_exit()
{
	Relibc_thread &t = thread_state();
	{
		Mutex::Guard guard(main().threads_mutex);
		if (t.genode_thread) t.exited = true;   /* reaped by gxd_thread_create */
		else release(t);
	}
	sleep_forever();
}


/* stop a thread (pthread_cancel): its thread pointer (relibc's TCB) */
extern "C" long gxd_thread_cancel(int tid)
{
	Main &m = main();
	Mutex::Guard guard(m.threads_mutex);
	for (Relibc_thread &t : m.threads) {
		if (!t.used || t.tid != tid) continue;
		if (!t.genode_thread || t.exited) return -ESRCH;
		{
			/* not amid a futex operation, and out of the waiters */
			Mutex::Guard futex_guard(m.futex_mutex);
			if (t.waiting) m.futex_waiters.remove(t.waiting);
			t.waiting = nullptr;
			destroy(m.heap, t.genode_thread);
			t.genode_thread = nullptr;
		}
		unsigned long const tp = t.tp;
		release(t);
		return (long)tp;
	}
	return -ESRCH;
}


extern "C" long gxd_futex_wait(uint32_t *addr, uint32_t val, long long deadline_ns)
{
	Main &m = main();
	Relibc_thread &me = thread_state();
	Futex_waiter waiter(addr, me.waiting);
	{
		Mutex::Guard guard(m.futex_mutex);
		if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) return -EAGAIN;
		m.futex_waiters.insert(&waiter);
		me.waiting = &waiter;
	}

	/* the entrypoint dispatches I/O signals meanwhile (Main::dispatch_io) */
	if (waiter.on_ep) {
		for (;;) {
			long long remaining = -1;
			{
				Mutex::Guard guard(m.futex_mutex);
				if (waiter.woken) return 0;
				if (deadline_ns >= 0) {
					remaining = deadline_ns - gxd_clock_ns(1);
					if (remaining <= 0) {
						m.futex_waiters.remove(&waiter);
						me.waiting = nullptr;
						return -ETIMEDOUT;
					}
				}
			}
			if (remaining < 0) m.wait_io();   /* the waker signals the entrypoint */
			else m.dispatch_io((unsigned long)min(remaining/1000 + 1, 100*1000LL));
		}
	}

	if (deadline_ns < 0) {
		waiter.blockade.block();
		return 0;   /* the waker reset 'me.waiting' */
	}

	/* woken, or by the deadline */
	Deadline deadline { deadline_ns, waiter.blockade };
	add_deadline(deadline);
	waiter.blockade.block();
	remove_deadline(deadline);

	Mutex::Guard guard(m.futex_mutex);
	if (waiter.woken) return 0;
	m.futex_waiters.remove(&waiter);
	me.waiting = nullptr;
	return -ETIMEDOUT;
}


extern "C" long gxd_futex_wake(uint32_t *addr, uint32_t num)
{
	Main &m = main();
	Mutex::Guard guard(m.futex_mutex);
	uint32_t woken = 0;
	for (Futex_waiter *w = m.futex_waiters.first(); w && woken < num; ) {
		Futex_waiter *next = w->next();
		if (w->addr == addr) {
			m.futex_waiters.remove(w);
			w->woken = true;
			w->slot  = nullptr;
			if (w->on_ep) m.wake_entrypoint();
			w->blockade.wakeup();
			woken++;
		}
		w = next;
	}
	return woken;
}
