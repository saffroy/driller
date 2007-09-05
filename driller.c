/*
 * driller.c
 * install and maintain file-backed mappings for all readable parts 
 * of a process address space
 */

/*
 * portability note: 
 * - all stack-related code assumes a single stack that grows down
 * - not thread safe (but can be done with USE_LOCKS or ptmalloc)
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/resource.h>
#include <search.h>
#include <stdarg.h>

#include "driller.h"
#include "driller_internal.h"
#include "log.h"
#include "dlmalloc.h"

static int driller_initialized = 0;

/* root for the sorted tree of map structs */
static void *map_root = NULL;
/* maps for the user stack and heap */
static struct map_rec *map_stack = NULL;
static struct map_rec *map_heap = NULL;
/* cache call to sysconf(_SC_PAGESIZE) */
static unsigned int page_size;

/* overloaded routines have to be called */
static void *(*old_mmap)(void *start, size_t length, int prot, int flags,
			 int fd, off_t offset);
static int (*old_munmap)(void *start, size_t length);
static void *(*old_mremap)(void *old_address, size_t old_size,
			   size_t new_size, int flags);
static int (*old_brk)(void *end_data_segment);
static void *(*old_sbrk)(intptr_t increment);

/* temp stack for stack rebuild,
 * and for segfault handler used for stack growth */
static char *altstack;

/* previous (fallback) handler for sigsegv */
static struct sigaction old_segv_sigaction;

/* user-registered callback for map */
static void (*map_invalidate_cb)(struct map_rec *map);

/*
 * since driller can be entered from an allocator calling mmap,
 * and we may have to allocate maps from here,
 * let's define our own allocation space, and use malloc hooks
 */
static mspace driller_mspace;
static void *(*old_malloc_hook)(size_t bytes, const void *caller);
static void (*old_free_hook)(void *mem, const void *caller);
static void *(*old_realloc_hook)(void *mem, size_t bytes, const void *caller);
static void *(*old_memalign_hook)(size_t align, size_t bytes, const void *caller);

/*
 * including both malloc.h and dlmalloc.h is not possible, 
 * so duplicate the following hook declarations
 */
extern void *(*__malloc_hook)(size_t bytes, const void *caller);
extern void (*__free_hook)(void *mem, const void *caller);
extern void *(*__realloc_hook)(void *mem, size_t bytes, const void *caller);
extern void *(*__memalign_hook)(size_t align, size_t bytes, const void *caller);

/******************/

static inline void *driller_malloc(size_t bytes) {
	return mspace_malloc(driller_mspace, bytes);
}

static inline void driller_free(void *mem) {
	mspace_free(driller_mspace, mem);
}

static inline void *driller_realloc(void *mem, size_t bytes) {
	return mspace_realloc(driller_mspace, mem, bytes);
}

static void *driller_malloc_hook(size_t bytes, const void *caller) {
	return driller_malloc(bytes);
}

static void driller_free_hook(void *mem, const void *caller) {
	driller_free(mem);
}

static void *driller_realloc_hook(void *mem, size_t bytes, const void *caller) {
	return driller_realloc(mem, bytes);
}

static void *driller_memalign_hook(size_t align, size_t bytes, const void *caller) {
	return mspace_memalign(driller_mspace, align, bytes);
}

static void driller_malloc_install(void){
	if(!driller_initialized)
		return;

	old_malloc_hook = __malloc_hook;
	old_free_hook = __free_hook;
	old_realloc_hook = __realloc_hook;
	old_memalign_hook = __memalign_hook;

	__malloc_hook = driller_malloc_hook;
	__free_hook = driller_free_hook;
	__realloc_hook = driller_realloc_hook;
	__memalign_hook = driller_memalign_hook;
}

static void driller_malloc_restore(void){
	if(!driller_initialized)
		return;

	__malloc_hook = old_malloc_hook;
	__free_hook = old_free_hook;
	__realloc_hook = old_realloc_hook;
	__memalign_hook = old_memalign_hook;
}

/******************/

static int map_cmp(const void *a, const void *b) {
	const struct map_rec *left = a;
	const struct map_rec *right = b;

	assert(left->start <= left->end);
	assert(right->start <= right->end);

	if(left->end <= right->start)
		return -1;
	if(left->start >= right->end)
		return 1;
	/* "equality" here means a and b have a common interval */
	return 0;
}

static void map_record(off_t start, off_t end, int prot, off_t offset,
		       char *path, int fd) {
	struct map_rec *map;

	if(strcmp(path, "[vdso]") == 0)
		/* ignore gate page */
		return;
#if __i386__
	if(start == 0xffffe000)
		/* not sure what it is: gate page? */
		return;
#endif
	if(!(prot & PROT_READ))
		/* not readable, ignore */
		return;
	if(strncmp(path, "/dev/", strlen("/dev/")) == 0)
		/* special files are not welcome */
		return;

	/* now we have something to do */

	map = malloc(sizeof(*map));
	assert(map != NULL);

	map->start = start;
	map->end = end;
	map->prot = prot;
	map->offset = offset;
	map->path = strdup(path);
	assert(map->path != NULL);
	map->fd = fd;

	assert(tsearch((void *)map, &map_root, map_cmp) != NULL);
}

static void map_parse(void)
{
	const char *file = "/proc/self/maps";
	int fd;
	char buf[4096] = {};
	off_t start, end, offset;
	char prot_str[5];
	long maj, min, ino;
	char *line, *p;
	int lineno, i;
	int prot;

	/* no allocation while we read maps, otherwise map_rebuild might
	   try to map a segment that is gone (or has shrinked) */
	fd = open(file, O_RDONLY);
	if(fd < 0)
		perr("open");
	if(read(fd, buf, sizeof(buf)) < 0)
		perr("read");
	/* make sure we've read the whole thing */
	assert(read(fd, buf, sizeof(buf)) == 0);
	close(fd);

	line = buf;
	for(lineno = 0; ; lineno++) {
		if(sscanf(line, "%lx-%lx %s %lx %lx:%lx %ld", 
			  &start, &end, prot_str, &offset,
			  &maj, &min, &ino) != 7) {
			p = strchrnul(line, '\n');
			*p = '\0';
			err("could not parse line %d: '%s'\n",
			    lineno, line);
		}

		p = line;
		for(i = 0; i < 5; i++)
			p = strchr(p+1, ' ');
		while(*p == ' ')
			p++;
		line = strchrnul(p, '\n');
		*line++ = '\0'; /* remove trailing \n */
		dbg("% 2d: %lx-%lx %s %lx %lx:%lx %ld '%s'\n",
		    lineno, start, end, prot_str, offset, maj, min, ino, p);
		prot = ((prot_str[0] == 'r') ? PROT_READ : 0)
			| ((prot_str[1] == 'w') ? PROT_WRITE : 0)
			| ((prot_str[2] == 'x') ? PROT_EXEC : 0) ;
		map_record(start, end, prot, offset, p, -1);
		if(*line == '\0')
			return;
	}
}

/*
 * remplace any mapping (or the heap) with a file-backed mapping
 */
static void map_overload(void *start, size_t length, int prot, int flags,
			 int fd, off_t offset, int reset_brk) {
	char *shmem;

#if 0 /* no need to shrink heap? */
	if(reset_brk)
		/* no param on the heap, it can disappear */
		brk(start);
#endif
	shmem = mmap(start, length, prot, flags, fd, offset);
	if(shmem == MAP_FAILED)
		perr("mmap");
}

/*
 * overloading the stack requires running this from a separate stack
 */
static void map_overload_stack(void) {
	off_t size;
	int rc;

	/* we're on a separate stack, but globals are still here */
	size = map_stack->end - map_stack->start;

	/* the new stack is at the end of a large sparse file */
	if(ftruncate(map_stack->fd, STACK_MAP_OFFSET) != 0)
		perr("ftruncate");
	if(lseek(map_stack->fd, STACK_MAP_OFFSET - size, SEEK_SET) < 0)
		perr("lseek");

	/* copy mapped area to file */
	rc = write(map_stack->fd, (char*)map_stack->start, size);
	if(rc < 0)
		perr("write");
	if(rc < size)
		err("short write (%d instead of %zd)", rc, size);

	map_overload((void*)map_stack->start, size, map_stack->prot,
		     MAP_SHARED | MAP_FIXED, map_stack->fd,
		     STACK_MAP_OFFSET - size, 0);
	dbg("remapped stack at %lx", map_stack->start);
}

static inline long stack_base(void) {
        unsigned long sp;
	unsigned long mask = ~((unsigned long)page_size-1);

#if __x86_64__
	asm volatile ("movq %%rsp,%0" : "=r"(sp));
#elif __i386__
	asm volatile ("movl %%esp,%0" : "=r"(sp));
#else
#error function stack_base needs porting to your architecture!
#endif
        return sp & mask;
}

static void run_altstack(void (*f)(void), void *stack, long stack_size) {
	ucontext_t alts_main, alts_func;

	if(getcontext(&alts_func) < 0)
		perr("getcontext");
	alts_func.uc_stack.ss_sp = stack;
	alts_func.uc_stack.ss_size = stack_size;
	alts_func.uc_link = &alts_main;
	makecontext(&alts_func, f, 0);

	if (swapcontext(&alts_main, &alts_func) < 0)
		perr("swapcontext");
}

static void segv_sigaction(int signum, siginfo_t *si, void *uctx) {
	off_t addr = (off_t)si->si_addr;
	off_t size;
	void *rc;
	struct rlimit rl;
	int errno_sav = errno;

	/* we handle stack growth and nothing else */
	if(si->si_code != SEGV_MAPERR
	   || addr >= map_stack->start
	   || addr < (map_stack->end - STACK_MAP_OFFSET))
		goto out_raise;

	/* grow stack by at least STACK_MIN_GROW */
	map_stack->start = min((addr & ~((unsigned long)page_size - 1)),
			       map_stack->start - STACK_MIN_GROW);
	size = map_stack->end - map_stack->start;

	if(getrlimit(RLIMIT_STACK, &rl) != 0)
		perr("getrlimit");
	if(size > rl.rlim_cur) {
		err_noabort("stack limit exceeded");
		goto out_raise;
	}

	rc = mmap((void*)map_stack->start, size, map_stack->prot,
		  MAP_SHARED | MAP_FIXED, map_stack->fd,
		  STACK_MAP_OFFSET - size);
	if(rc == MAP_FAILED)
		perr("mmap");
	dbg("stack grows to %lx", map_stack->start);
	errno = errno_sav;
	return;

out_raise:
	/* let the previous handler run, usually the default action
	 * if a core is produced, it will have the right stack (not this one) */
	if(sigaction(signum, &old_segv_sigaction, NULL) != 0)
		perr("sigaction");
	errno = errno_sav;
}

static int map_create_fd(char *fmt, ...) {
	char *filename;
	int len;
	va_list ap;
	int fd;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	filename = malloc(len+1);
	assert(filename != NULL);

	va_start(ap, fmt);
	vsprintf(filename, fmt, ap);
	va_end(ap);

	fd = open(filename, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if(fd < 0)
		perr("open");
	if(unlink(filename))
		perr("unlink");
	free(filename);

	return fd;
}

static void map_rebuild(struct map_rec *map, int index) {
	size_t size;
	int rc;
	enum overload_t type;
	stack_t ss;
	struct sigaction sa;

	dbg("rebuild %d: %lx %s", index, map->start, map->path);
	if(strcmp(map->path, "[heap]") == 0)
		type = OVERLOAD_HEAP;
	else if(strcmp(map->path, "[stack]") == 0)
		type = OVERLOAD_STACK;
	else
		type = OVERLOAD_REG;

	//XXX todo: improve handling of read mappings to existing files (optim)

	/* create file, unlink immediately */
	map->fd = map_create_fd("%s/shmem-%d-%d%s",
				TMPDIR, getpid(), index,
				type == OVERLOAD_REG ? "" : map->path);

	switch (type) {
	case OVERLOAD_HEAP:
		/* allocations after map_parse() could have extended the
		   heap limit, so reread it; and avoid any further alloc */
		map->end = (off_t)sbrk(0);
		dbg("switching to new heap: %lx-%lx", map->start, map->end);
		map_heap = map;
		/* FALL THROUGH */

	case OVERLOAD_REG:
		size = map->end - map->start;

		/* copy mapped area to file */
		rc = write(map->fd, (char*)map->start, size);
		if(rc < 0)
			perr("write");
		if(rc < size)
			err("short write (%d instead of %zd)", rc, size);

		/* map file over original area */
		map_overload((void*)map->start, size, map->prot,
			     MAP_SHARED | MAP_FIXED, map->fd, 0,
			     (type == OVERLOAD_HEAP) );
		break;

	case OVERLOAD_STACK:
		map_stack = map;
		//XXX stack may grow after map_parse - but how much??
		map_stack->start = min(stack_base(), map_stack->start);
		dbg("switching to new stack: %lx-%lx", map->start, map->end);

		/* overload current stack: use alternate stack */
		altstack = malloc(ALTSTACK_SIZE);
		assert(altstack != NULL);
		run_altstack(map_overload_stack, altstack, ALTSTACK_SIZE);

		/* activate segv handler on altstack_size to grow stack */
		assert(ALTSTACK_SIZE >= SIGSTKSZ);
		ss.ss_sp = altstack;
		ss.ss_size = ALTSTACK_SIZE;
		ss.ss_flags = 0;
		if(sigaltstack(&ss, NULL) < 0)
			perr("sigaltstack");

		sa.sa_sigaction = segv_sigaction;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		if(sigaction(SIGSEGV, &sa, &old_segv_sigaction) != 0)
			perr("sigaction");
		break;
	}
}

static void map_invalidate_range(off_t start, off_t end) {
	struct map_rec *map;

	while(1) {
		map = driller_lookup_map((void*)start, (size_t)(end-start));
		if(map == NULL)
			return;
		/* map shares an interval with [start-end] */

		/* notify user of the end of this map as it knows it */
		if(map_invalidate_cb != NULL)
			map_invalidate_cb(map);

		if( (start <= map->start)
		    && (map->end <= end) ) {
			/* map has disappeared completely */

			assert(tdelete(map, &map_root, map_cmp) != NULL);
			close(map->fd);
			free(map->path);
			free(map);
		} else {
			/* map needs trimming */
			if(start <= map->start) {
				off_t new_start;

				/* trim the start */
				new_start = min(end, map->end);
				map->offset += new_start - map->start;
				map->start = new_start;
			} else if(map->end <= end) {
				/* trim the end */
				map->end = max(start, map->start);
			}
		}
	}
}

/******************/

void *mmap(void *start, size_t length, int prot, int flags,
	   int fd, off_t offset) {
	void *rc = MAP_FAILED;
	int new_flags;
	int errno_sav;

	if(!driller_initialized
	   || !(flags & MAP_ANONYMOUS)
	   || !(prot & PROT_READ) ) {
		rc = old_mmap(start, length, prot, flags, fd, offset);
		errno_sav = errno;
		goto out_ret;
	}
	//XXX todo: handle non anonymous maps?

	driller_malloc_install();

	fd = map_create_fd("%s/shmem-%d-anon", TMPDIR, getpid());
	if(ftruncate(fd, length) != 0)
		goto out_close;

	new_flags = (flags & ~(MAP_ANONYMOUS|MAP_PRIVATE)) | MAP_SHARED;
	rc = old_mmap(start, length, prot, new_flags, fd, offset);
	if(rc != MAP_FAILED) {
		map_invalidate_range((off_t)rc, (off_t)rc + length);
		map_record((off_t)rc, (off_t)rc + length, prot, offset, "", fd);
	}
out_close:
	if(rc == MAP_FAILED) {
		errno_sav = errno;
		close(fd);
	}

	driller_malloc_restore();
out_ret:
	dbg("mmap(%p, %ld, 0x%x, 0x%x, %d, %ld) = %p %s",
	    start, length, prot, flags, fd, offset, rc,
	    rc == MAP_FAILED ? strerror(errno_sav) : "");
	errno = errno_sav;
	return rc;
}

int munmap(void *start, size_t length) {
	int rc, errno_sav;

	if(!driller_initialized) {
		rc = old_munmap(start, length);
		errno_sav = errno;
		goto out_ret;
	}

	driller_malloc_install();

	rc = old_munmap(start, length);
	if(rc == 0)
		map_invalidate_range((off_t)start, (off_t)start + length);
	else
		errno_sav = errno;

	driller_malloc_restore();
out_ret:
	dbg("munmap(%p, %ld) = %d", start, length, rc);
	errno = errno_sav;
	return rc;
}

void * mremap(void *old_address, size_t old_size ,
	      size_t new_size, int flags) {
	void *rc;
	int errno_sav;
	struct map_rec key;
	struct map_rec *map, **mptr;

	if(!driller_initialized) {
		rc = old_mremap(old_address, old_size, new_size, flags);
		errno_sav = errno;
		goto out;
	}

	/* flags other than MAYMOBE are not handled yet (extra arg) */
	assert((flags & ~MREMAP_MAYMOVE) == 0);

	/* identify affected mapping */
	key.start = (off_t)old_address;
	key.end = key.start + old_size;

	mptr = tfind(&key, &map_root, map_cmp);
	if(mptr == NULL)
		/* not one of our mappings, ignore it */
		goto do_remap;
	map = *mptr;

	/* rule out unlikely corner cases */
	assert(map->start == key.start);
	assert(map->end == key.end);

do_remap:
	rc = old_mremap(old_address, old_size, new_size, flags);
	if(rc == MAP_FAILED || mptr == NULL) {
		errno_sav = errno;
		goto out;
	}

	/* file size must agree with mapping size */
	if(ftruncate(map->fd, new_size) != 0)
		perr("ftruncate");

	/* update map */
	if(old_address == rc)
		/* map did not move */
		map->end = map->start + new_size;
	else {
		/* need to reinsert map to keep the map tree sorted */
		driller_malloc_install();
		assert(tdelete(map, &map_root, map_cmp) != NULL);
		map->start = (off_t)rc;
		map->end = map->start + new_size;
		assert(tsearch((void *)map, &map_root, map_cmp) != NULL);
		driller_malloc_restore();
	}

out:
	dbg("mremap(%p, %ld, %ld, %x) = %p",
	    old_address, old_size, new_size, flags, rc);
	errno = errno_sav;
	return rc;
}

static int driller_brk(void *end_data_segment){
	off_t new_size;

	if((off_t)end_data_segment == map_heap->end)
		return 0;
	if((off_t)end_data_segment <= map_heap->start)
		return 0;
	new_size = (off_t)end_data_segment - map_heap->start;
	if(ftruncate(map_heap->fd, new_size) != 0)
		perr("ftruncate");
	if(mremap((void*)map_heap->start, map_heap->end - map_heap->start,
		  new_size, 0) == MAP_FAILED)
		perr("mremap");
	map_heap->end = (off_t)end_data_segment;
	dbg("heap end moves to %p", end_data_segment);
	return 0;
}

int brk(void *end_data_segment){
	int rc;

	dbg("brk(%p)", end_data_segment);
	if(!driller_initialized)
		return old_brk(end_data_segment);

	driller_malloc_install();
	rc = driller_brk(end_data_segment);
	driller_malloc_restore();
	return rc;
}

static void *driller_sbrk(intptr_t increment){
	void *old_brk;

	old_brk = (void*)map_heap->end;
	if(increment == 0)
		return old_brk;
	if(driller_brk(old_brk + increment) == 0)
		return old_brk;
	else
		return (void*)-1;
}

void *sbrk(intptr_t increment){
	void *rc;

	dbg("sbrk(%ld)", increment);
	if(!driller_initialized)
		return old_sbrk(increment);

	driller_malloc_install();
	rc = driller_sbrk(increment);
	driller_malloc_restore();
	return rc;
}

/******************/

static void * get_sym(const char *symbol) {
	void *sym;
	char *err;

	sym = dlsym(RTLD_NEXT, symbol);
	err = dlerror();
	if(err)
		err("dlsym(%s) error: %s", symbol, err);
	return sym;
}

#if 0 /* for separate shared obj */
static void driller_init_syms(void) __attribute__((constructor));
#endif

static void driller_init_syms(void) {
	/* locate overloaded functions */
	old_mmap = get_sym("mmap");
	old_munmap = get_sym("munmap");
	old_mremap = get_sym("mremap");
	old_brk = get_sym("brk");
	old_sbrk = get_sym("sbrk");
}

static void
map_rebuild_action(const void *nodep, const VISIT which, const int depth) {
	static int map_idx;

	switch(which) {
	case preorder:
		break;
	case postorder:
		map_rebuild(*(struct map_rec**)nodep, map_idx++);
		break;
	case endorder:
		break;
	case leaf:
		map_rebuild(*(struct map_rec**)nodep, map_idx++);
		break;
	}
}

void driller_init(void) {

	page_size = sysconf(_SC_PAGESIZE);
	driller_init_syms();

	/* force first call to brk, so heap becomes visible */
	free(malloc(1));

	driller_mspace = create_mspace(0, 0);
	driller_malloc_install();

	/* analyze own mappings */
	map_parse();

	/* replace own mappings */
	twalk(map_root, map_rebuild_action);

	/* join siblings */
	/* publish new mappings */
	/* gather siblings' mappings */

	dbg("init complete");

	driller_malloc_restore();

	driller_initialized = 1;
}

void driller_register_map_invalidate_cb(void (*f)(struct map_rec *map)) {
	map_invalidate_cb = f;
}

struct map_rec *driller_lookup_map(void *start, size_t length) {
	struct map_rec key, **mptr;

	key.start = (off_t)start;
	key.end = key.start + length;
	mptr = tfind(&key, &map_root, map_cmp);
	if(mptr == NULL)
		return NULL;
	else
		return *mptr;
}
