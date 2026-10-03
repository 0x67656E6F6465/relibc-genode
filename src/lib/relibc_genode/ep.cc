/*
 * \brief  The entrypoint's work for the other threads of the Genode shim
 *
 * The component's VFS (and with it an IP stack in the component, file-system
 * sessions, terminals, ...) is used by the entrypoint alone, which also
 * dispatches its I/O signals, as Genode's libc does it. Another thread hands
 * its VFS work over as a job and blocks; the entrypoint executes the job,
 * again whenever the VFS makes progress (Vfs::Env::User::wakeup_vfs_user),
 * until it completes, and then wakes the thread. Nothing polls.
 *
 * The program's main runs in a thread of its own (posix_main.cc), so that
 * the entrypoint is free to dispatch. Code that runs on the entrypoint (a
 * Libc::Component's, the start-up) executes its jobs at once and, while one
 * is incomplete, dispatches I/O signals itself (Main::dispatch_io).
 *
 * Deadlines of the waiting threads (timed jobs, timed futex waits) end
 * through one timeout of the entrypoint, set to the earliest of them.
 */

#include "state.h"

using namespace Gxd;

namespace {

struct Pending : List<Pending>::Element
{
	Ep_job         &job;
	long long const deadline;     /* monotonic ns, -1: none */
	Blockade        blockade { };
	bool            running   = false;
	bool            completed = false;

	Pending(Ep_job &job, long long deadline) : job(job), deadline(deadline) { }
};


struct Ep_jobs
{
	enum { MAX = Main::MAX_THREADS };

	Env &_env;

	Mutex          _mutex     { };
	List<Pending>  _jobs      { };
	List<Deadline> _deadlines { };

	Io_signal_handler<Ep_jobs> _examine_handler { _env.ep(), *this, &Ep_jobs::_examine };

	/* opened on first use (a program that never waits needs no timer) */
	Constructible<Timer::Connection>                     _timer   { };
	Constructible<Timer::One_shot_timeout<Ep_jobs>>      _timeout { };
	long long                                            _scheduled = -1;

	Ep_jobs(Env &env) : _env(env) { }

	void trigger() { Signal_transmitter(_examine_handler).submit(); }

	/* the earliest deadline (_mutex held) */
	long long _earliest()
	{
		long long at = -1;
		auto earlier = [&] (long long d) { if (d >= 0 && (at < 0 || d < at)) at = d; };
		for (Pending *p = _jobs.first(); p; p = p->next()) earlier(p->deadline);
		for (Deadline *d = _deadlines.first(); d; d = d->next()) if (!d->expired) earlier(d->at);
		return at;
	}

	/* set the timeout to the earliest deadline (_mutex held) */
	void _reschedule()
	{
		long long const at = _earliest();
		if (at < 0 || (_scheduled >= 0 && _scheduled <= at)) return;

		if (!_timer.constructed()) {
			_timer.construct(_env);
			_timeout.construct(*_timer, *this, &Ep_jobs::_handle_timeout);
		}
		long long const now = gxd_clock_ns(1);
		_scheduled = at;
		_timeout->schedule(Microseconds { (uint64_t)max((at - now + 999)/1000, 1LL) });
	}

	void _handle_timeout(Duration)
	{
		{
			Mutex::Guard guard(_mutex);
			_scheduled = -1;
			long long const now = gxd_clock_ns(1);
			for (Deadline *d = _deadlines.first(); d; d = d->next())
				if (!d->expired && d->at <= now) { d->expired = true; d->blockade.wakeup(); }
			_reschedule();
		}
		_examine();
	}

	/*
	 * Execute the incomplete jobs, on the entrypoint. A job may dispatch
	 * I/O signals itself (Main::wait_io), which may bring this function
	 * back: running jobs are left alone then.
	 */
	void _examine()
	{
		Pending *batch[MAX];
		unsigned n = 0;
		{
			Mutex::Guard guard(_mutex);
			for (Pending *p = _jobs.first(); p && n < MAX; p = p->next())
				if (!p->running) { p->running = true; batch[n++] = p; }
		}
		long long now = -1;   /* read once, when needed (the clock is a timer session) */
		for (unsigned i = 0; i < n; i++) {
			Pending &p = *batch[i];
			bool const done = p.job.execute();
			if (!done && p.deadline >= 0 && now < 0) now = gxd_clock_ns(1);
			bool const expired = !done && p.deadline >= 0 && now >= p.deadline;

			Mutex::Guard guard(_mutex);
			p.running = false;
			if (done || expired) {
				_jobs.remove(&p);
				p.completed = done;
				p.blockade.wakeup();
			}
		}
		/* the VFS's deferred wakeups of its peers (packets for a NIC, ...), also
		 * of the progress that brought us here without any job */
		main().vfs.io().commit();

		Mutex::Guard guard(_mutex);
		_reschedule();
	}

	bool run(Ep_job &job, long long deadline)
	{
		Pending p { job, deadline };
		{
			Mutex::Guard guard(_mutex);
			_jobs.insert(&p);
			_reschedule();
		}
		trigger();
		p.blockade.block();
		return p.completed;
	}

	void add(Deadline &d)
	{
		Mutex::Guard guard(_mutex);
		_deadlines.insert(&d);
		_reschedule();
	}

	void remove(Deadline &d)
	{
		Mutex::Guard guard(_mutex);
		_deadlines.remove(&d);
	}
};


Ep_jobs *_ep_jobs;

Ep_jobs &ep_jobs()
{
	if (!_ep_jobs) {
		/* static storage: no destructor for relibc's atexit */
		alignas(Ep_jobs) static char storage[sizeof(Ep_jobs)];
		_ep_jobs = construct_at<Ep_jobs>(storage, main().env);
	}
	return *_ep_jobs;
}

} /* anonymous namespace */


void Gxd::init_ep_jobs() { ep_jobs(); }


/* (the VFS plugins wake already while Main constructs them: no jobs yet) */
void Gxd::examine_ep_jobs() { if (_ep_jobs) _ep_jobs->trigger(); }


bool Gxd::run_on_ep(Ep_job &job, long long deadline_ns)
{
	Main &m = main();
	if (!m.on_entrypoint())
		return ep_jobs().run(job, deadline_ns);

	/* on the entrypoint: at once, dispatching I/O signals until complete */
	for (;;) {
		if (job.execute()) { m.vfs.io().commit(); return true; }
		if (deadline_ns < 0) { m.wait_io(); continue; }  /* no timer needed */

		long long const now = gxd_clock_ns(1);
		if (now >= deadline_ns) return false;
		m.dispatch_io((unsigned long)min((deadline_ns - now)/1000 + 1, 100*1000LL));
	}
}


void Gxd::add_deadline(Deadline &d)    { ep_jobs().add(d); }
void Gxd::remove_deadline(Deadline &d) { ep_jobs().remove(d); }


void Main::wakeup_vfs_user() { examine_ep_jobs(); }


void Main::wait_io()
{
	/* VFS work happens on the entrypoint alone (ep.cc) */
	if (!on_entrypoint()) {
		error("relibc: VFS access outside of the entrypoint");
		sleep_forever();
	}
	vfs.io().commit_and_wait();
}


void Main::dispatch_io(unsigned long max_us)
{
	if (!_tick_timer.constructed()) {
		_tick_timer.construct(env);
		_tick_timer->sigh(_tick_handler);
	}
	_tick_timer->trigger_once(max(max_us, 1UL));
	vfs.io().commit_and_wait();
}
