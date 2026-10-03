/*
 * \brief  Clocks, sleeping and randomness of the Genode shim of relibc
 *
 * CLOCK_MONOTONIC counts from the timer's start. CLOCK_REALTIME adds the
 * real-time clock's reading at start-up: from the VFS file the config's
 * '+ libc | rtc: ...' names (Genode's rtc plugin: "YYYY-MM-DD HH:MM:SS"),
 * as with Genode's libc, or from an Rtc session ('+ relibc | rtc: yes');
 * otherwise it equals the monotonic clock. Random bytes come from the VFS
 * file '+ libc | rng: ...' names, else /dev/random of the VFS when it has
 * one (e.g. Genode's jitterentropy plugin), else from a xorshift generator
 * seeded from the timer, which is not cryptographically secure.
 */

#include <rtc_session/connection.h>

#include "state.h"

using namespace Gxd;

namespace {

long long _realtime_offset_ns = 0;

/* days since 1970-01-01 of a civil date (proleptic Gregorian) */
long long days_from_civil(long long y, unsigned m, unsigned d)
{
	y -= m <= 2;
	long long const era = (y >= 0 ? y : y - 399) / 400;
	unsigned  const yoe = (unsigned)(y - era*400);
	unsigned  const doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1;
	unsigned  const doe = yoe*365 + yoe/4 - yoe/100 + doy;
	return era*146097 + (long long)doe - 719468;
}

long long monotonic_ns() { return (long long)main().clock().elapsed_us()*1000; }

}


namespace Gxd {

	/* the reading of Genode's rtc VFS plugin: "YYYY-MM-DD HH:MM:SS", the
	   seconds optional (as Genode's libc reads it) */
	bool rtc_from_file(char const *path)
	{
		char buf[32] { };
		long const n = ep_call([&] () -> long {
			Abs_path abs;
			if (resolve(AT_FDCWD, path, abs)) return -ENOENT;
			return read_vfs_file(abs.string(), buf, sizeof(buf) - 1);
		});
		if (n < 16) return false;

		auto num = [&] (unsigned at, unsigned digits) {
			unsigned v = 0;
			for (unsigned i = at; i < at + digits; i++) {
				if (buf[i] < '0' || buf[i] > '9') return ~0u;
				v = v*10 + (buf[i] - '0');
			}
			return v;
		};
		unsigned const y = num(0, 4), mo = num(5, 2), d = num(8, 2),
		               h = num(11, 2), mi = num(14, 2),
		               se = (n >= 19 && buf[16] == ':') ? num(17, 2) : 0;
		if (y == ~0u || mo == ~0u || d == ~0u || h == ~0u || mi == ~0u || se == ~0u)
			return false;

		long long const secs = days_from_civil(y, mo, d)*86400 + h*3600 + mi*60 + se;
		_realtime_offset_ns = secs*1000000000LL - monotonic_ns();
		return true;
	}

	void init_time()
	{
		Libc_path const rtc_path = libc_config("rtc");
		if (rtc_path.length() > 1) {
			if (rtc_from_file(rtc_path.string())) return;
			warning("relibc: no time from ", rtc_path);
		}

		bool const rtc = main().config.node().with_sub_node("relibc",
			[] (Node const &n) { return n.attribute_value("rtc", false); },
			[] { return false; });
		if (!rtc) return;

		Rtc::Connection connection(main().env);
		Rtc::Timestamp const t = connection.current_time();
		long long const secs = days_from_civil(t.year, t.month, t.day)*86400
		                     + t.hour*3600 + t.minute*60 + t.second;
		_realtime_offset_ns = secs*1000000000LL + t.microsecond*1000LL - monotonic_ns();
	}
}


extern "C" long long gxd_clock_ns(int clock)
{
	enum { REALTIME = 0, MONOTONIC = 1, PROCESS_CPUTIME = 2, THREAD_CPUTIME = 3,
	       MONOTONIC_RAW = 4, REALTIME_COARSE = 5, MONOTONIC_COARSE = 6, BOOTTIME = 7 };
	switch (clock) {
	case REALTIME: case REALTIME_COARSE:
		return monotonic_ns() + _realtime_offset_ns;
	case MONOTONIC: case MONOTONIC_RAW: case MONOTONIC_COARSE: case BOOTTIME:
	case PROCESS_CPUTIME: case THREAD_CPUTIME:
		return monotonic_ns();
	default:
		return -EINVAL;
	}
}


/*
 * Sleep, until a signal the thread does not block (-EINTR): a timed wait
 * for the signals' sequence number (signals.cc)
 */
extern "C" long gxd_sleep_ns(unsigned long long ns)
{
	long long const deadline = gxd_clock_ns(1) + (long long)ns;
	for (;;) {
		uint32_t const seq = __atomic_load_n(signal_seq(), __ATOMIC_SEQ_CST);
		if (signal_interrupts()) return -EINTR;
		if (gxd_clock_ns(1) >= deadline) return 0;
		gxd_futex_wait(signal_seq(), seq, deadline);
	}
}


extern "C" long gxd_getrandom(void *buf, unsigned long len)
{
	Main &m = main();
	long const from_vfs = ep_call([&] () -> long {
		auto &root = m.vfs.root_dir();
		Libc_path const rng = libc_config("rng");
		Abs_path abs { "/dev/random" };
		if (rng.length() > 1 && resolve(AT_FDCWD, rng.string(), abs))
			abs = Abs_path();
		if (abs.length() > 1 && root.leaf_path(abs.string()))
			return read_vfs_file(abs.string(), buf, len);
		return -ENOENT;
	});
	if (from_vfs == (long)len) return len;

	unsigned long long const seed = (unsigned long long)monotonic_ns() | 1;
	Mutex::Guard guard(m.threads_mutex);
	unsigned long long &s = m.random_state;
	if (!s) s = seed;
	for (unsigned long i = 0; i < len; i++) {
		s ^= s << 13; s ^= s >> 7; s ^= s << 17;
		((unsigned char *)buf)[i] = (unsigned char)s;
	}
	return len;
}
