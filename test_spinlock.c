/*
 * test_spinlock.c
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/wait.h>

#include "spinlock.h"

#define SHM_SIZE (1L << 20) /* 1 MB */
#define LOOPS 100L

int main(int argc, char **argv) {
	char *mem;
	int *buf;
	struct spinlock *lock;
	long bufsz;
	int i, j;
	int rc;

	mem = mmap(NULL, SHM_SIZE, PROT_READ|PROT_WRITE,
		   MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	assert(mem != MAP_FAILED);

	lock = (struct spinlock*)mem;
	spin_lock_init(lock);

	buf = (int*)(mem + 4096);
	bufsz = (SHM_SIZE - 4096)/sizeof(*buf);
	printf("buf @ %p sz %ld\n", buf, bufsz);

	rc = fork();

	for(j = 0; j < LOOPS; j++)
		for(i = 0; i < bufsz; i++) {
			spin_lock(lock);
			buf[i]++;
			spin_unlock(lock);
		}

	if(rc)
		wait(NULL);
	else
		_exit(0);

	for(i = 0; i < bufsz; i++)
		assert(buf[i] = 2 * LOOPS);

	printf("SUCCESS! exiting\n");
	return 0;
}


