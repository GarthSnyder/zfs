# SPDX-License-Identifier: CDDL-1.0
zstream_CPPFLAGS = $(AM_CPPFLAGS) $(LIBZPOOL_CPPFLAGS)

sbin_PROGRAMS   += zstream
CPPCHECKTARGETS += zstream

zstream_SOURCES = \
	%D%/zstream.c \
	%D%/zstream.h \
	%D%/zstream_decompress.c \
	%D%/zstream_dedup.c \
	%D%/zstream_dump.c \
	%D%/zstream_recompress.c \
	%D%/zstream_redup.c \
	%D%/zstream_token.c \
	%D%/zstream_shared.c \
	%D%/zstream_shared.h \
	%D%/zstream_stream.c \
	%D%/zstream_stream.h \
	%D%/zstream_queue.c \
	%D%/zstream_queue.h \
	%D%/zstream_chain.c \
	%D%/zstream_chain.h \
	%D%/zstream_chain_types.h \
	%D%/zstream_fletcher4.c \
	%D%/zstream_fletcher4.h \
	%D%/allocator.c \
	%D%/allocator.h \
	%D%/linear_hash.c \
	%D%/linear_hash_debug.c \
	%D%/linear_hash.h \
	%D%/linear_hash_stats.h \
	%D%/linear_hash_impl.h \
	%D%/linear_hash_debug.h

zstream_LDADD = \
	libzfs.la \
	libzfs_core.la \
	libzpool.la \
	libnvpair.la \
	-lm

cmd-zstream-install-exec-hook:
	cd $(DESTDIR)$(sbindir) && $(LN_S) -f zstream zstreamdump

INSTALL_EXEC_HOOKS += cmd-zstream-install-exec-hook
