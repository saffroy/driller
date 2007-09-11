#ifndef DRILLER_INTERNAL_H
#define DRILLER_INTERNAL_H

#define USE_TMPFS 1
#define MAP_TABLE_INITIAL_SIZE 32 /* items */
#define DONT_MAP_TEXT 1

#if _LP64
#define STACK_MAP_OFFSET	(1L << 37) /* 128GB */
#else
#define STACK_MAP_OFFSET	(1L << 30) /* 1GB */
#endif
#define ALTSTACK_SIZE		(1L << 16) /* 64KB */
#define STACK_MIN_GROW		(1L << 20) /* 1MB */
/* no HEAP_MIN_GROW: malloc should be smart with sbrk */

#if USE_TMPFS
#define TMPDIR "/dev/shm"
#else
#define TMPDIR "/tmp"
#endif

enum overload_t {
	OVERLOAD_REG,
	OVERLOAD_HEAP,
	OVERLOAD_STACK,
};

#endif /* DRILLER_INTERNAL_H */
