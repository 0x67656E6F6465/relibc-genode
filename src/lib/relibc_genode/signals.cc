/*
 * \brief  Signals and POSIX timers of the Genode shim of relibc
 *
 * Genode delivers no signals to a component, so they stay within it, as
 * with Genode's libc: the set of pending signals is kept here, where
 * timers, raise and kill (of the component itself) add to it and blocking
 * waits notice it (they end with EINTR). relibc's platform keeps the
 * actions and the masks and runs the handlers when the program enters it
 * (src/platform/genode/signal.rs).
 *
 * Timers are timeouts of a timer session of their own, which fire on the
 * entrypoint.
 */

#include "fd.h"

using namespace Gxd;

namespace {

/* pending signals, bit (signo - 1) */
uint64_t _pending  = 0;
uint32_t _sig_seq  = 0;   /* changes with every signal, for waiting */

void _raised()
{
	__atomic_add_fetch(&_sig_seq, 1, __ATOMIC_SEQ_CST);
	gxd_futex_wake(&_sig_seq, ~0u);
	io_changed();   /* epoll waits look again */
}


struct Posix_timer
{
	enum { MAX = 32 };

	bool               used     = false;
	int                signo    = 0;      /* 0: no signal (SIGEV_NONE) */
	int                clock    = 1;      /* of absolute times (Linux clock ids) */
	unsigned long long interval = 0;      /* us */
	long long          deadline = -1;     /* monotonic ns, -1: disarmed */
	unsigned           overruns = 0;

	Constructible<Timer::One_shot_timeout<Posix_timer>> timeout { };

	void handle(Duration)
	{
		Mutex::Guard guard(mutex());
		if (deadline < 0) return;
		if (signo) gxd_signal_raise(signo);
		if (interval) {
			deadline += (long long)interval*1000;
			timeout->schedule(Microseconds { interval });
		} else {
			deadline = -1;
		}
	}

	static Mutex &mutex() { static Mutex m; return m; }
};

/* allocated on first use: the shim has no static constructors (fork.cc) */
Posix_timer *_timers;

Timer::Connection &_timer_session()
{
	static Constructible<Timer::Connection> timer;
	if (!timer.constructed()) timer.construct(main().env);
	return *timer;
}

} /* anonymous namespace */


extern "C" void gxd_signal_raise(int signo)
{
	if (signo < 1 || signo > 64) return;
	__atomic_or_fetch(&_pending, 1ull << (signo - 1), __ATOMIC_SEQ_CST);
	_raised();
}


/* take the lowest pending signal of 'allowed', or 0 */
extern "C" int gxd_signal_take(uint64_t allowed)
{
	uint64_t cur = __atomic_load_n(&_pending, __ATOMIC_SEQ_CST);
	for (;;) {
		uint64_t const ready = cur & allowed;
		if (!ready) return 0;
		uint64_t const bit = ready & -ready;
		if (__atomic_compare_exchange_n(&_pending, &cur, cur & ~bit, false,
		                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return __builtin_ctzll(bit) + 1;
	}
}


extern "C" uint64_t gxd_signals_pending() { return __atomic_load_n(&_pending, __ATOMIC_SEQ_CST); }


/* the calling thread's mask, as relibc keeps it, for interrupting its waits */
extern "C" void gxd_set_sigmask(uint64_t mask) { thread_state().sigmask = mask; }


uint32_t *Gxd::signal_seq() { return &_sig_seq; }


bool Gxd::signal_interrupts()
{
	return gxd_signals_pending() & ~thread_state().sigmask;
}


/* wait until a signal of 'allowed' is pending, or the deadline (monotonic ns, -1: none) */
extern "C" long gxd_signal_wait(uint64_t allowed, long long deadline_ns)
{
	for (;;) {
		uint32_t const seq = __atomic_load_n(&_sig_seq, __ATOMIC_SEQ_CST);
		if (gxd_signals_pending() & allowed) return 0;
		if (deadline_ns >= 0 && gxd_clock_ns(1) >= deadline_ns) return -ETIMEDOUT;
		gxd_futex_wait(&_sig_seq, seq, deadline_ns);
	}
}


/* POSIX timers: their ids are 1.. */

extern "C" long gxd_timer_create(int signo, int clock)
{
	Mutex::Guard guard(Posix_timer::mutex());
	if (!_timers) {
		try { _timers = new (main().heap) Posix_timer[Posix_timer::MAX]; }
		catch (...) { return -EAGAIN; }
	}
	for (unsigned i = 0; i < Posix_timer::MAX; i++) {
		Posix_timer &t = _timers[i];
		if (t.used) continue;
		t.used = true; t.signo = signo; t.clock = clock; t.interval = 0; t.deadline = -1; t.overruns = 0;
		if (!t.timeout.constructed())
			t.timeout.construct(_timer_session(), t, &Posix_timer::handle);
		return i + 1;
	}
	return -EAGAIN;
}


static Posix_timer *_timer(long id)
{
	if (!_timers || id < 1 || id > Posix_timer::MAX || !_timers[id - 1].used) return nullptr;
	return &_timers[id - 1];
}


/*
 * Arm ('value_ns' relative, or absolute on the timer's clock; 0 disarms),
 * the previous setting into 'old_*'
 */
extern "C" long gxd_timer_settime(long id, long long value_ns, long long interval_ns, int absolute,
                                  long long *old_value_ns, long long *old_interval_ns)
{
	Mutex::Guard guard(Posix_timer::mutex());
	Posix_timer *t = _timer(id);
	if (!t) return -EINVAL;

	long long const now = gxd_clock_ns(1);
	if (absolute && value_ns > 0)
		value_ns = max(value_ns - gxd_clock_ns(t->clock), 1LL);  /* passed: at once */
	if (old_value_ns)    *old_value_ns    = t->deadline < 0 ? 0 : max(t->deadline - now, 1LL);
	if (old_interval_ns) *old_interval_ns = (long long)t->interval*1000;

	t->timeout->discard();
	t->deadline = -1;
	t->interval = 0;
	if (value_ns <= 0) return 0;

	t->interval = (unsigned long long)(interval_ns + 999)/1000;
	t->deadline = now + value_ns;
	t->timeout->schedule(Microseconds { (uint64_t)(value_ns + 999)/1000 });
	return 0;
}


extern "C" long gxd_timer_gettime(long id, long long *value_ns, long long *interval_ns)
{
	Mutex::Guard guard(Posix_timer::mutex());
	Posix_timer *t = _timer(id);
	if (!t) return -EINVAL;
	long long const now = gxd_clock_ns(1);
	*value_ns    = t->deadline < 0 ? 0 : max(t->deadline - now, 1LL);
	*interval_ns = (long long)t->interval*1000;
	return 0;
}


extern "C" long gxd_timer_delete(long id)
{
	Mutex::Guard guard(Posix_timer::mutex());
	Posix_timer *t = _timer(id);
	if (!t) return -EINVAL;
	t->timeout->discard();
	t->used = false;
	t->deadline = -1;
	return 0;
}
