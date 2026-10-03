/*
 * \brief  Empty init/fini arrays for relibc_start
 *
 * relibc's start code runs __preinit_array .. __fini_array; Genode's
 * dynamic linker already runs the constructors, so these delimit empty
 * arrays (start and end at the same address), hidden within libc.
 */

#define EMPTY_ARRAY(name) \
	__asm__(".pushsection .rodata\n" \
	        ".balign 8\n" \
	        ".hidden " #name "_start\n" ".global " #name "_start\n" \
	        ".hidden " #name "_end\n"   ".global " #name "_end\n" \
	        #name "_start:\n" #name "_end:\n" \
	        ".popsection\n")

EMPTY_ARRAY(__preinit_array);
EMPTY_ARRAY(__init_array);
EMPTY_ARRAY(__fini_array);
