#ifndef DRILLER_H
#define DRILLER_H

#define USE_TMPFS 1
#define MAP_TABLE_INITIAL_SIZE 32 /* items */

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

struct map_rec {
	off_t start;
	off_t end;
	int prot;
	off_t offset;
	char *path;
	int fd;
	int user_flags;
};

void driller_init(void);
void driller_register_map_invalidate_cb(void (*f)(struct map_rec *map));
struct map_rec *driller_lookup_map(void *start, size_t length);

#endif /* DRILLER_H */
