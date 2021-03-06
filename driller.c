/*
 * driller.c
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 * install and maintain file-backed memory mappings for most readable
 * parts of a process address space
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
#include <stdint.h>

#include "tunables.h"
#include "driller.h"
#include "driller_internal.h"
#include "log.h"
#include "dlmalloc.h"

static int driller_initialized = 0;
static int driller_malloc_installed = 0;

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
#ifdef linux
static void *(*old_mremap)(void *old_address, size_t old_size,
			   size_t new_size, int flags);
#endif
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
#ifdef linux
static void *(*old_malloc_hook)(size_t bytes, const void *caller);
static void (*old_free_hook)(void *mem, const void *caller);
static void *(*old_realloc_hook)(void *mem, size_t bytes, const void *caller);
static void *(*old_memalign_hook)(size_t align, size_t bytes, const void *caller);
#endif

/*
 * including both malloc.h and dlmalloc.h is not possible, 
 * so duplicate the following hook declarations
 */
#ifdef linux
extern void *(*__malloc_hook)(size_t bytes, const void *caller);
extern void (*__free_hook)(void *mem, const void *caller);
extern void *(*__realloc_hook)(void *mem, size_t bytes, const void *caller);
extern void *(*__memalign_hook)(size_t align, size_t bytes, const void *caller);
#endif /* linux */

/******************/

/*
 * hooks and allocation routines for cases when the regular malloc/free
 * cannot be used, because they are calling us
 */

void *driller_malloc(size_t bytes) {
	return mspace_malloc(driller_mspace, bytes);
}

void driller_free(void *mem) {
	mspace_free(driller_mspace, mem);
}

static inline void *driller_realloc(void *mem, size_t bytes) {
	return mspace_realloc(driller_mspace, mem, bytes);
}

#ifdef linux
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
#else
void *malloc(size_t bytes) {
	if(driller_malloc_installed)
		return mspace_malloc(driller_mspace, bytes);
	else
		return dlmalloc(bytes);
}

void free(void *mem) {
	if(driller_malloc_installed)
		mspace_free(driller_mspace, mem);
	else
		dlfree(mem);
}

void *realloc(void *mem, size_t bytes) {
	if(driller_malloc_installed)
		return mspace_realloc(driller_mspace, mem, bytes);
	else
		return dlrealloc(mem, bytes);
}

void *memalign(size_t align, size_t bytes) {
	if(driller_malloc_installed)
		return mspace_memalign(driller_mspace, align, bytes);
	else
		return dlmemalign(align, bytes);
}
#endif

static void driller_malloc_install(void){
	if(!driller_initialized || driller_malloc_installed)
		return;

#ifdef linux
	old_malloc_hook = __malloc_hook;
	old_free_hook = __free_hook;
	old_realloc_hook = __realloc_hook;
	old_memalign_hook = __memalign_hook;

	__malloc_hook = driller_malloc_hook;
	__free_hook = driller_free_hook;
	__realloc_hook = driller_realloc_hook;
	__memalign_hook = driller_memalign_hook;
#endif

	driller_malloc_installed = 1;
}

static void driller_malloc_restore(void){
	if(!driller_initialized)
		return;

#ifdef linux
	__malloc_hook = old_malloc_hook;
	__free_hook = old_free_hook;
	__realloc_hook = old_realloc_hook;
	__memalign_hook = old_memalign_hook;
#endif

	driller_malloc_installed = 0;
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

/*
 * record a description of a memory segment that is/will become
 * a file-backed memory mapping - uses tsearch(3)
 */
void map_record(void *start, void *end, int prot, off_t offset,
		char *path, int fd) {
	struct map_rec *map;
	void *rc;

	if(strcmp(path, "[vdso]") == 0)
		/* ignore gate page */
		return;
#if __i386__
	if(start == (void*)0xffffe000)
		/* ignore gate page */
		return;
#endif
	if(!(prot & PROT_READ))
		/* not readable, ignore */
		return;
#ifdef DONT_MAP_TEXT
	if((prot & PROT_EXEC) && !(prot & PROT_WRITE))
		/* prefer to keep text as is, otherwise oprofile can't get
		 * symbol information
		 * a side effect is that rodata may not be shared */
		return;
#endif
	if(strncmp(path, "/dev/", strlen("/dev/")) == 0)
		/* special files are not welcome */
		return;
#if __x86_64__
	if(offset > (2L << 40))
		/* some strange offsets in /proc/self/maps */
		offset = 0;
#endif

	/* now we have something to do */

	map = malloc(sizeof(*map));
	assert(map != NULL);
	memset(map, 0, sizeof(*map));

	map->start = start;
	map->end = end;
	map->prot = prot;
	map->offset = offset;
	map->path = strdup(path);
	assert(map->path != NULL);
	map->fd = fd;

	rc = tsearch((void *)map, &map_root, map_cmp);
	assert(rc != NULL);
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
 * create a guard zone below the stack (mapped area with no access rights)
 * this is required on some platforms to detect (and handle) stack growth
 */
static void stack_guard_map(void) {
#ifndef linux
	mmap(map_stack->start - STACK_GUARD_SIZE, STACK_GUARD_SIZE, 0,
	     MAP_PRIVATE | MAP_FIXED, map_stack->fd, 0);
#endif
}

/*
 * overloading the stack requires running this function from a separate stack
 */
static void map_overload_stack(void) {
	uintptr_t size;
	int rc;

	/* we're on a separate stack, but globals are still here */
	size = map_stack->end - map_stack->start;

	/* the new stack is at the end of a large sparse file */
	map_stack->offset = STACK_MAP_OFFSET - size;
	if(lseek(map_stack->fd, map_stack->offset, SEEK_SET) < 0)
		perr("lseek");

	/* copy mapped area to file */
	rc = write(map_stack->fd, map_stack->start, size);
	if(rc < 0)
		perr("write");
	if(rc < size)
		err("short write (%d instead of %zd)", rc, size);

	map_overload(map_stack->start, size, map_stack->prot,
		     MAP_SHARED | MAP_FIXED, map_stack->fd,
		     map_stack->offset, 0);
	stack_guard_map();

	dbg("remapped stack at %p", map_stack->start);
}

/*
 * return the page address of the current stack pointer
 */
static inline void *stack_base(void) {
        uintptr_t sp;
	uintptr_t mask = ~((uintptr_t)page_size-1);

#if __x86_64__
	asm volatile ("movq %%rsp,%0" : "=r"(sp));
#elif __i386__
	asm volatile ("movl %%esp,%0" : "=r"(sp));
#elif __sparc__
	asm volatile ("mov %%sp,%0" : "=r"(sp));
#else
#error function stack_base needs porting to your architecture!
#endif
        return (void*)(sp & mask);
}

/*
 * run a function from an alternate stack
 */
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

/*
 * SIGSEGV handler used to grow the stack on demand
 */
static void segv_sigaction(int signum, siginfo_t *si, void *uctx) {
	void *addr = si->si_addr;
	size_t size;
	void *rc;
	struct rlimit rl;
	int errno_sav = errno;

	/* we handle stack growth and nothing else */
	switch(si->si_code) {
	case SEGV_MAPERR:
		if(addr >= map_stack->start
		   || addr < (map_stack->end - STACK_MAP_OFFSET))
			goto out_raise;
		break;
#ifndef linux
	case SEGV_ACCERR:
		if(addr >= map_stack->start
		   || addr < (map_stack->start - STACK_GUARD_SIZE))
			goto out_raise;
		break;
#endif
	default:
		goto out_raise;
	}

	/* grow stack by at least STACK_MIN_GROW */
	addr = (void*)((uintptr_t)addr & ~((unsigned long)page_size - 1));
	map_stack->start = min(addr, map_stack->start - STACK_MIN_GROW);
	size = map_stack->end - map_stack->start;
	map_stack->offset = STACK_MAP_OFFSET - size;

	if(getrlimit(RLIMIT_STACK, &rl) != 0)
		perr("getrlimit");
	if(size > rl.rlim_cur) {
		err_noabort("stack limit exceeded");
		goto out_raise;
	}

	rc = mmap(map_stack->start, size, map_stack->prot,
		  MAP_SHARED | MAP_FIXED, map_stack->fd,
		  map_stack->offset);
	if(rc == MAP_FAILED)
		perr("mmap");
	stack_guard_map();

	dbg("stack grows to %p", map_stack->start);
	errno = errno_sav;
	return;

out_raise:
	/* let the previous handler run, usually the default action
	 * if a core is produced, it will have the right stack (not this one) */
	if(sigaction(signum, &old_segv_sigaction, NULL) != 0)
		perr("sigaction");
	errno = errno_sav;
}

/*
 * return a new fd to a file suitable for memory mapping
 */
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

static int map_is_stack(struct map_rec *map) {
#ifdef linux
	return (strcmp(map->path, "[stack]") == 0);
#else
	void *p;

	p = stack_base();
	return (p >= map->start) && (p < map->end);
#endif
}

static int map_is_heap(struct map_rec *map) {
#ifdef linux
	return (strcmp(map->path, "[heap]") == 0);
#else
	void *buf;
	int rc;

	buf = malloc(1);
	rc = (buf >= map->start) && (buf < map->end);
	free(buf);
	return rc;
#endif
}

/*
 * replace a memory segment by a file-backed memory mapping
 */
static void map_rebuild(struct map_rec *map, int index) {
	size_t size;
	int rc;
	enum overload_t type;
	stack_t ss;
	struct sigaction sa;

	dbg("rebuild %d: %p %s", index, map->start, map->path);
	if(map_is_heap(map))
		type = OVERLOAD_HEAP;
	else if(map_is_stack(map))
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
		map->end = sbrk(0);
		dbg("switching to new heap: %p-%p", map->start, map->end);
		map_heap = map;
		/* FALL THROUGH */

	case OVERLOAD_REG:
		size = map->end - map->start;

		/* copy mapped area to file */
		if(lseek(map->fd, map->offset, SEEK_SET) < 0)
			perr("lseek");
		rc = write(map->fd, map->start, size);
		if(rc < 0)
			perr("write");
		if(rc < size)
			err("short write (%d instead of %zd)", rc, size);

		/* map file over original area */
		map_overload(map->start, size, map->prot,
			     MAP_SHARED | MAP_FIXED, map->fd, map->offset,
			     (type == OVERLOAD_HEAP) );
		break;

	case OVERLOAD_STACK:
		map_stack = map;
		//XXX stack may grow after map_parse - but how much??
		map_stack->start = min(stack_base(), map_stack->start);
		dbg("switching to new stack: %p-%p", map->start, map->end);

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

/*
 * trim or destroy the descriptions of memory segments
 * that were affected by a map or unmap operation
 */
static void map_invalidate_range(void *start, void *end) {
	struct map_rec *map;

	/* loop over all maps that intersect with [start-end] */
	while(1) {
		map = driller_lookup_map(start, end-start);
		if(map == NULL)
			return;

		/* notify user of the end of this map as it knows it */
		if(map_invalidate_cb != NULL)
			map_invalidate_cb(map);

		if( (start <= map->start)
		    && (map->end <= end) ) {
			/* map has disappeared completely */
			void *rc;

			rc = tdelete(map, &map_root, map_cmp);
			assert(rc != NULL);

			/* make sure memory is released *now* */
			if(ftruncate(map->fd, 0) != 0)
				perr("ftruncate");

			if(close(map->fd) != 0)
				perr("close");
			free(map->path);
			free(map);
			continue;
		}

		/* map needs trimming */
		if(start <= map->start) {
			void *new_start;

			/* trim the start */
			new_start = min(end, map->end);
			map->offset += new_start - map->start;
			map->start = new_start;
		} else if(map->end <= end) {
			/* trim the end */
			map->end = max(start, map->start);
			if(ftruncate(map->fd,
				     map->offset + map->end - map->start) != 0)
				perr("ftruncate");
		} else
			/* we should split the map!
			 * this can be done but seems very unlikely */
			err("unexpected condition: should split mapping");
	}
}


/******************/

/*
 * overloads the regular mmap
 * anonymous maps become shared file maps that can be used
 * by other processes
 */
void *mmap(void *start, size_t length, int prot, int flags,
	   int fd, off_t offset) {
	void *rc = MAP_FAILED;
	int new_flags;
	int errno_sav;

	if(!driller_initialized || driller_malloc_installed
	   || !(flags & MAP_ANONYMOUS)
	   || !(prot & PROT_READ) ) {
		rc = old_mmap(start, length, prot, flags, fd, offset);
		errno_sav = errno;
		goto out;
	}

	driller_malloc_install();

	fd = map_create_fd("%s/shmem-%d-anon", TMPDIR, getpid());
	if(ftruncate(fd, offset + length) != 0) {
		errno_sav = errno;
		if(close(fd) != 0)
			perr("close");
		goto out_restore;
	}

	new_flags = (flags & ~(MAP_ANONYMOUS|MAP_PRIVATE)) | MAP_SHARED;
	rc = old_mmap(start, length, prot, new_flags, fd, offset);
	errno_sav = errno;
	if(rc == MAP_FAILED) {
		if(close(fd) != 0)
			perr("close");
		goto out_restore;
	}

	map_invalidate_range(rc, rc + length);
	map_record(rc, rc + length, prot, offset, "", fd);
out_restore:
	driller_malloc_restore();
out:
	dbg("mmap(%p, %zd, 0x%x, 0x%x, %d, %ld) = %p %s",
	    start, length, prot, flags, fd, offset, rc,
	    rc == MAP_FAILED ? strerror(errno_sav) : "");
	errno = errno_sav;
	return rc;
}

/*
 * overload the regular munmap
 */
int munmap(void *start, size_t length) {
	int rc, errno_sav;

	if(!driller_initialized || driller_malloc_installed) {
		rc = old_munmap(start, length);
		errno_sav = errno;
		goto out_ret;
	}

	driller_malloc_install();

	rc = old_munmap(start, length);
	errno_sav = errno;
	if(rc == 0)
		map_invalidate_range(start, start + length);

	driller_malloc_restore();
out_ret:
	dbg("munmap(%p, %zd) = %d", start, length, rc);
	errno = errno_sav;
	return rc;
}

#ifndef linux
/*
 * minimalist replacement for mremap
 * should only be used to manage the heap (brk/sbrk)
 */
static void * driller_mremap(struct map_rec *map,
			     size_t new_size) {
	size_t old_size = map->end - map->start;
	void *rc;

	if(new_size > old_size) {
		rc = old_mmap(map->start + old_size, new_size - old_size,
			      map->prot, MAP_SHARED | MAP_FIXED,
			      map->fd, map->offset + old_size);
		if(rc != MAP_FAILED)
			rc = map->start;
	} else {
		if(old_munmap(map->start + new_size, old_size - new_size))
			rc = MAP_FAILED;
		else
			rc = map->start;
	}
	dbg("driller_mremap(address=%p, old_size=%d, new_size=%d) = %p (%s)",
	    map->start, old_size, new_size, rc, strerror(errno));
	return rc;
}
#endif

/*
 * overload the regular mremap
 */
#ifndef __GLIBC_PREREQ
#define __GLIBC_PREREQ(x,y) 0
#endif

#ifndef linux
static
#endif
#if __GLIBC_PREREQ(2,4)
void * mremap(void *old_address, size_t old_size ,
	      size_t new_size, int flags, ...) {
#else
void * mremap(void *old_address, size_t old_size ,
	      size_t new_size, int flags) {
#endif
	void *rc;
	int errno_sav;
	struct map_rec key;
	struct map_rec *map, **mptr;

#ifdef linux
	if(!driller_initialized || driller_malloc_installed) {
		rc = old_mremap(old_address, old_size, new_size, flags);
		errno_sav = errno;
		goto out;
	}
#else
	assert(driller_initialized);
	assert(driller_malloc_installed);
#endif

	/* flags other than MAYMOVE are not handled yet (extra arg) */
#ifdef linux
	assert((flags & ~MREMAP_MAYMOVE) == 0);
#else
	assert(flags == 0);
#endif

	/* identify affected mapping */
	key.start = old_address;
	key.end = key.start + old_size;

	mptr = tfind(&key, &map_root, map_cmp);
	map = (mptr != NULL ? *mptr : NULL);
	if(map == NULL)
		/* not one of our mappings, ignore it */
		goto do_remap;

	/* rule out unlikely corner cases */
	assert(map->start == key.start);
	assert(map->end == key.end);

do_remap:
#ifdef linux
	rc = old_mremap(old_address, old_size, new_size, flags);
#else
	rc = driller_mremap(map, new_size);
#endif
	errno_sav = errno;
	if(rc == MAP_FAILED || map == NULL)
		goto out;

	/* file size must agree with mapping size */
	if(ftruncate(map->fd, map->offset + new_size) != 0)
		perr("ftruncate");

	/* update map */
	if(old_address == rc)
		/* map did not move */
		map->end = map->start + new_size;
	else {
		/* need to reinsert map to keep the map tree sorted */
		void *rc2;

		driller_malloc_install();
		rc2 = tdelete(map, &map_root, map_cmp);
		assert(rc2 != NULL);
		map->start = rc;
		map->end = map->start + new_size;
		rc2 = tsearch((void *)map, &map_root, map_cmp);
		assert(rc2 != NULL);
		driller_malloc_restore();
	}

out:
	dbg("mremap(%p, %zd, %zd, %x) = %p",
	    old_address, old_size, new_size, flags, rc);
	errno = errno_sav;
	return rc;
}

/*
 * overload the regular brk
 * grow the memory map used for the heap
 */
static int driller_brk(void *end_data_segment){
	uintptr_t new_size;

	if(end_data_segment == map_heap->end)
		return 0;
	if(end_data_segment <= map_heap->start)
		return 0;
	new_size = end_data_segment - map_heap->start;
	if(ftruncate(map_heap->fd, map_heap->offset + new_size) != 0)
		perr("ftruncate");
	if(mremap(map_heap->start, map_heap->end - map_heap->start,
		  new_size, 0) == MAP_FAILED)
		perr("mremap");
	map_heap->end = end_data_segment;
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

	old_brk = map_heap->end;
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

/* must be called before any call to one of the overloaded syms */
static void driller_init_syms(void) __attribute__((constructor));
static void driller_init_syms(void) {
	/* locate overloaded functions */
	old_mmap = get_sym("mmap");
	old_munmap = get_sym("munmap");
#ifdef linux
	old_mremap = get_sym("mremap");
#endif
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

	/* force first call to brk, so heap becomes visible */
	free(malloc(1));

	driller_mspace = create_mspace(0, 0);
	driller_malloc_install();

	/* analyze own mappings */
	map_parse();

	/* replace own mappings */
	twalk(map_root, map_rebuild_action);

	driller_malloc_restore();

	driller_initialized = 1;
}

/*
 * register a callback
 * this callback notifies the user that a map has been changed or removed
 */
void driller_register_map_invalidate_cb(void (*f)(struct map_rec *map)) {
	map_invalidate_cb = f;
}

/*
 * find the map record for a given memory range
 */
struct map_rec *driller_lookup_map(void *start, size_t length) {
	struct map_rec key, **mptr;

	key.start = start;
	key.end = key.start + length;
	mptr = tfind(&key, &map_root, map_cmp);
	if(mptr == NULL)
		return NULL;
	else
		return *mptr;
}

/*
 * memory map a given file range
 * used to bypass the overloaded mmap
 */
void *driller_install_map(struct map_rec *map) {
	void *rc;

	rc = old_mmap(NULL, map->end - map->start, PROT_READ,
		      MAP_SHARED, map->fd, map->offset);
	if(rc == MAP_FAILED)
		perr("mmap");
	return rc;
}

/*
 * destroy the given file map
 * used to bypass the overloaded munmap
 */
void driller_remove_map(struct map_rec *map, void *p) {
	if(old_munmap(p, map->end - map->start) != 0)
		perr("munmap");
}

