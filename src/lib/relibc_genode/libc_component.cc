/*
 * \brief  Genode's libc API (libc/component.h) on relibc
 *
 * Components written for Genode's libc implement Libc::Component::construct
 * and get a Libc::Env; Genode's posix library (src/lib/posix/construct.cc,
 * used as is) implements it by calling main. Libc::with_libc runs its
 * function directly: the shim needs no separate application context, as
 * it blocks for I/O on the entrypoint itself.
 */

#include <libc/component.h>

#include "state.h"

using namespace Genode;

namespace Gxd { class Libc_env; }


class Gxd::Libc_env : public Libc::Env
{
	private:

		Genode::Env                  &_env;
		Vfs::Env                     &_vfs_env;
		Attached_rom_dataspace const &_config_rom;

	public:

		Libc_env(Genode::Env &env, Vfs::Env &vfs_env, Attached_rom_dataspace const &config)
		: _env(env), _vfs_env(vfs_env), _config_rom(config) { }

		void _with_config(With_config::Ft const &fn) const override { fn(_config_rom.node()); }

		Vfs::Env &vfs_env() override { return _vfs_env; }

		Parent        &parent()  override { return _env.parent(); }
		Cpu_session   &cpu()     override { return _env.cpu(); }
		Env::Local_rm &rm()      override { return _env.rm(); }
		Pd_session    &pd()      override { return _env.pd(); }
		Ram_allocator &ram()     override { return _env.ram(); }
		Entrypoint    &ep()      override { return _env.ep(); }
		Runtime       &runtime() override { return _env.runtime(); }

		Cpu_session_capability cpu_session_cap() override { return _env.cpu_session_cap(); }
		Pd_session_capability  pd_session_cap()  override { return _env.pd_session_cap(); }

		Id_space<Parent::Client> &id_space() override { return _env.id_space(); }

		Session_capability session(Parent::Service_name const &name, Parent::Client::Id id,
		                           Parent::Session_args const &args, Affinity const &aff) override {
			return _env.session(name, id, args, aff); }

		Session_result try_session(Parent::Service_name const &name, Parent::Client::Id id,
		                           Parent::Session_args const &args, Affinity const &aff) override {
			return _env.try_session(name, id, args, aff); }

		void upgrade(Parent::Client::Id id, Parent::Upgrade_args const &args) override {
			_env.upgrade(id, args); }

		void close(Parent::Client::Id id) override { _env.close(id); }

		void exec_static_constructors() override { _env.exec_static_constructors(); }
};


void Libc::execute_in_application_context(Application_code &code) { code.execute(); }


/*
 * The C++ ABI's thread-exit destructors, which Genode's libc provides for
 * Genode's C++ library (libsupc++ defines it elsewhere): relibc's
 * glibc-style implementation
 */
extern "C" int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);

extern "C" int __cxa_thread_atexit(void (*dtor)(void *), void *obj, void *dso)
{
	return __cxa_thread_atexit_impl(dtor, obj, dso);
}


/* a component may define its own */
__attribute__((weak)) size_t Libc::Component::stack_size() { return 256*1024; }


namespace Gxd {

	void construct_libc_component()
	{
		alignas(Libc_env) static char storage[sizeof(Libc_env)];
		Libc_env &env = *construct_at<Libc_env>(storage, main().env, main().vfs, main().config);

		/*
		 * The dynamic linker defers the static constructors of the program
		 * and its libraries (e.g., stdcxx's iostreams) until now, when the
		 * C library is ready, as Genode's libc does
		 */
		main().env.exec_static_constructors();

		Libc::Component::construct(env);
	}
}
