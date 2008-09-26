/*
 * tunables.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef TUNABLES_H
#define TUNABLES_H

#ifdef linux
#define USE_TMPFS 1
#else
#define USE_TMPFS 0
#endif

#if USE_TMPFS
#define TMPDIR "/dev/shm"
#else
#define TMPDIR "/tmp"
#endif

/* driller */

#define MAP_TABLE_INITIAL_SIZE 32 /* items */
#define DONT_MAP_TEXT 1

#ifdef _LP64
# ifdef linux
# define STACK_MAP_OFFSET	(1L << 37) /* 128GB */
# else
# define STACK_MAP_OFFSET	(1L << 30) /* 1GB */
# endif
#else
# define STACK_MAP_OFFSET	(1L << 30) /* 1GB */
#endif
#define ALTSTACK_SIZE		(1L << 16) /* 64KB */
#define STACK_MIN_GROW		(1L << 20) /* 1MB */
/* no HEAP_MIN_GROW: malloc should be smart with sbrk */
#define STACK_GUARD_SIZE	(1L << 20) /* 1MB */

/* fdproxy */

#define FDPROXY_MAX_CLIENTS 32
#define CONNECT_TIMEOUT 5 /* seconds */
#define FDTABLE_HSIZE_INIT 32


/* mmpi */

#define CONNECT_TIMEOUT 5 /* seconds */
#define CACHELINE_ALIGN 64

#define MSG_PAYLOAD_SIZE_BYTES 4096
#define MSG_POOL_SIZE 1024
//#define MSG_DRILLER_SIZE_THRESHOLD (1<<11) /* 2kB */
#define MSG_DRILLER_SIZE_THRESHOLD (0ULL)


#endif /* TUNABLES_H */
