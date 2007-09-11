/*
 * test_driller.c
 *
 * Copyright (C) Jean-Marc Saffroy <saffroy@gmail.com> 2007
 * This program is free software, distributed under the terms of the
 * GNU General Public License version 2.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driller.h"

#define HEAP_ALLOC_SIZE (1L<<24) /* 16MB */
#define HEAP_ALLOC_CHUNK 3210
#define HEAP_ALLOC_CHUNK_COUNT (HEAP_ALLOC_SIZE/HEAP_ALLOC_CHUNK)

void f(int n) {
	char buf[1024];

	memset(buf, 0, sizeof(buf));
	if(n > 0)
		f(n-1);
}

#ifndef NODRILL
static void map_invalidate(struct map_rec *map) {
	printf("map invalidate: Ox%lx-0x%lx\n",
	       map->start, map->end);
}
#endif

int main(int argc, char**argv) {
	int i;
	void **a;
	void *b;

#ifndef NODRILL
	driller_init();
	driller_register_map_invalidate_cb(map_invalidate);
#endif

	/* test heap */
	a = malloc(HEAP_ALLOC_CHUNK_COUNT * sizeof(void*));
	for(i = 0; i < HEAP_ALLOC_CHUNK_COUNT; i++)
		a[i] = malloc(HEAP_ALLOC_CHUNK);
	for(i = 0; i < HEAP_ALLOC_CHUNK_COUNT; i++)
		free(a[i]);
	free(a);

	/* have dlmalloc call mremap */
	b = malloc(HEAP_ALLOC_SIZE);
	b = realloc(b, HEAP_ALLOC_SIZE * 2);
	b = realloc(b, HEAP_ALLOC_SIZE / 2);
	free(b);

	/* vfork/exec should work */
	system("env echo system: foobar");

	/* test stack */
	printf("grow the stack a bit\n");
	f(1000);
#if 0
	printf("try to exceed the stack limit\n");
	f(8000);
#endif

	printf("SUCCESS! exiting\n");
	return 0;
}
