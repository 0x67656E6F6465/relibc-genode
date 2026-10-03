/*
 * \brief  Anonymous memory of the Genode shim of relibc: RAM dataspaces
 */

#include "state.h"

using namespace Gxd;


extern "C" long gxd_mmap_anon(unsigned long len)
{
	Main &m = main();
	Mutex::Guard guard(m.memory_mutex);
	size_t const size = (len + 0xfff) & ~(size_t)0xfff;
	addr_t const at = m.mapping_space.alloc_aligned(size, Align { 12 }).convert<addr_t>(
		[] (auto &a) { a.deallocate = false; return addr_t(a.ptr); },
		[] (auto)    { return addr_t(0); });
	if (!at) return -ENOMEM;
	try {
		new (m.heap) Mapping(m.mappings, m.env.ram(), m.env.rm(), size, at);
		return (long)at;
	}
	catch (...) {
		m.mapping_space.free((void *)at);
		return -ENOMEM;
	}
}


/* a mapping at 'at' (a forked child's of its parent, fork.cc), 0 or -errno */
long Gxd::map_at(addr_t at, size_t size)
{
	Main &m = main();
	Mutex::Guard guard(m.memory_mutex);
	if (m.mapping_space.alloc_addr(size, at).failed()) return -ENOMEM;
	try {
		new (m.heap) Mapping(m.mappings, m.env.ram(), m.env.rm(), size, at);
		return 0;
	}
	catch (...) {
		m.mapping_space.free((void *)at);
		return -ENOMEM;
	}
}


extern "C" long gxd_munmap(void *addr, unsigned long)
{
	Main &m = main();
	Mutex::Guard guard(m.memory_mutex);
	Mapping *found = nullptr;
	m.mappings.for_each([&] (Mapping &mapping) {
		if (mapping.at == (addr_t)addr) found = &mapping; });
	if (!found) return -EINVAL;
	destroy(m.heap, found);
	m.mapping_space.free(addr);
	return 0;
}
