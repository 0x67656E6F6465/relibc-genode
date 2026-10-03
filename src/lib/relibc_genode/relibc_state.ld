/*
 * relibc's writable data (its own, its Rust crates', openlibm's) in output
 * sections of its own within libc.lib.so's data segment, apart from the
 * shim's: a forked child copies relibc's state from its parent and builds
 * the shim's anew (fork.cc). An addition to Genode's genode_rel.ld.
 */

SECTIONS
{
	. = ALIGN(0x1000);   /* the start of the data segment (genode_rel.ld's .data follows) */
	.relibc.data : {
		HIDDEN(__relibc_data_start = .);
		*librelibc.a:*(.data .data.*)
		*libopenlibm.a:*(.data .data.*)
		HIDDEN(__relibc_data_end = .);
	} : rw
}
INSERT BEFORE .data;

SECTIONS
{
	.relibc.bss (NOLOAD) : {
		HIDDEN(__relibc_bss_start = .);
		*librelibc.a:*(.bss .bss.* COMMON)
		*libopenlibm.a:*(.bss .bss.* COMMON)
		HIDDEN(__relibc_bss_end = .);
	} : rw
}
INSERT BEFORE .bss;
