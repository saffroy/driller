/*
 * spinlock.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef SPINLOCK_H
#define SPINLOCK_H

#define USE_SCHED_YIELD 1

#ifdef USE_SCHED_YIELD
#include <sched.h>
#endif

/*
 * spinlocks
 */

#define LOCK_MAGIC 0xf0010c4
struct spinlock {
#ifndef NDEBUG
	int magic;
#endif
	volatile unsigned int lck;
};

static void spin_lock_init(struct spinlock *lock) {
#ifndef NDEBUG
	lock->magic = LOCK_MAGIC;
#endif
	lock->lck = 1;
}

static inline int spin_trylock(struct spinlock *lock) {
	int oldval;

#if __x86_64__ || __i386__ 
	asm volatile(
		"xchgl %0,%1"
		: "=r" (oldval), "=m" (lock->lck)
		: "0" (0), "m" (lock->lck)
		: "memory");
#elif __sparc__
	asm volatile(
		"swap %1,%0"
		: "=r" (oldval), "=m" (lock->lck)
		: "0" (0), "m" (lock->lck)
		: "memory");
#else
#error function spin_trylock needs porting to your architecture!
#endif
        return oldval > 0;
}

static inline void nop(void) {
#if USE_SCHED_YIELD
	sched_yield();
#elif __x86_64__ || __i386__
	asm volatile("rep; nop" : : ); // XXX not sure it's a pause on x64
#elif __sparc__
	asm volatile("nop" : : ); // XXX not sure AT ALL it's a pause on sparc
#else
	/* nop */
#endif
}

static inline void spin_lock(struct spinlock *lock) {
	assert(lock->magic == LOCK_MAGIC);
	while(!spin_trylock(lock))
		nop();
}

static inline void spin_unlock(struct spinlock *lock) {
#ifdef __sparc__
	int one = 1;
#endif

	assert(lock->magic == LOCK_MAGIC);
#if __x86_64__ || __i386__ 
	asm volatile("movl $1,%0"
		     :"=m" (lock->lck)
		     :
		     : "memory");
#elif __sparc__
	asm volatile("st %1,%0"
		     :"=m" (lock->lck), "=r" (one)
		     :"m" (lock->lck), "r" (one)
		     : "memory");
#else
#error function spin_unlock needs porting to your architecture!
#endif
}

#endif /* SPINLOCK_H */
