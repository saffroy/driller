/*
 * test_mmpi.c
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <malloc.h>

#include "mmpi.h"
#include "log.h"

#define THRTEST_MIN_CHUNK_SIZE (1ULL << 8) /* 256 bytes */
#define THRTEST_MAX_CHUNK_SIZE (1ULL << 23) /* 8 MB */
#define THRTEST_VOLUME (1ULL << 27) /* 128 MB */

static void usage(char *progname) {
	err("usage: %s <job id> <job size> <rank> <iter>", progname);
}

int main(int argc, char**argv) {
	int jobid, nprocs, rank, iter;
	char *buf;

	/* parse args */
	if(argc != 5)
		usage(argv[0]);
	jobid = atoi(argv[1]);
	nprocs = atoi(argv[2]);
	rank = atoi(argv[3]);
	iter = atoi(argv[4]);

	mmpi_init(jobid, nprocs, rank);

	/* demonstrate barrier */
	printf("rank %d enters barrier\n", rank);
	mmpi_barrier();
	printf("rank %d exits barrier\n", rank);

	/* simple benchmark */
	if(rank != 0) {
		int i;

		for(i = 0; i < iter; i++)
			mmpi_barrier();
	} else {
		int i;
		struct timeval tv1, tv2;
		long delta;

		printf("now time barrier latency (%d iterations)...\n", 
		       iter);
		gettimeofday(&tv1, NULL);

		for(i = 0; i < iter; i++)
			mmpi_barrier();

		gettimeofday(&tv2, NULL);
		delta = (tv2.tv_sec - tv1.tv_sec) * 1000000
			+ tv2.tv_usec - tv1.tv_usec;
		printf("average barrier latency: %.2fusec\n",
		       (float)delta/(float)iter);
	}

	mmpi_barrier();

	/* test send/recv */
	if(rank != 0) {
		int i;

		printf("%d: send to %d\n", rank, 0);
		for(i = 0; i < iter; i++)
			mmpi_send(0, &rank, sizeof(rank));
	} else {
		int i, j;
		struct timeval tv1, tv2;
		long delta;

		printf("now time send/recv latency (%d iterations)...\n", 
		       iter);
		gettimeofday(&tv1, NULL);

		for(j = 1; j < nprocs; j++) {
			int r;
			size_t sz;

			printf("%d: recv from %d\n", rank, j);
			for(i = 0; i < iter; i++) {
				mmpi_recv(j, &r, &sz);
				assert(sz == sizeof(r));
				assert(r == j);
			}

		gettimeofday(&tv2, NULL);
		delta = (tv2.tv_sec - tv1.tv_sec) * 1000000
			+ tv2.tv_usec - tv1.tv_usec;
		printf("average send/recv latency: %.2fusec\n",
		       (float)delta/(float)iter);
		}
	}

	mmpi_barrier();

	/* test throughput */

#if 1 && defined(linux)
	/* increase the odds that buf is allocated with mmap
	 * (it won't be if there is enough free space in the heap) */
	mallopt(M_MMAP_THRESHOLD, THRTEST_MAX_CHUNK_SIZE);
#endif
	buf = malloc(THRTEST_MAX_CHUNK_SIZE);
	if(rank != 0) {
		int i, size, count;

		for(size = THRTEST_MIN_CHUNK_SIZE;
		    size <= THRTEST_MAX_CHUNK_SIZE; size <<= 1) {
			count = THRTEST_VOLUME / size;
			for(i = 0; i < count; i++) {
#if 0
				memset(buf, (char)i, size);
#else
				/* we don't necessarily want to benchmark memset */
				buf[0] = buf[size-1] = (char)i;
#endif

				mmpi_send(0, buf, size);

#if 0
				/* if buf is allocated with mmap, this will force
				 * invalidation of the fd and its memory mapping */
				free(buf);
				buf = malloc(size);
#endif
			}
			mmpi_barrier();
		}
	} else { /* in rank 0 */
		int i, j, size, count;
		struct timeval tv1, tv2;
		float delta;

		printf("now time send/recv throughput (%lld MB per iteration)...\n", 
		       THRTEST_VOLUME >> 20);

		for(size = THRTEST_MIN_CHUNK_SIZE;
		    size <= THRTEST_MAX_CHUNK_SIZE; size <<= 1) {
			count = THRTEST_VOLUME / size;

			gettimeofday(&tv1, NULL);
			for(j = 1; j < nprocs; j++) {
				size_t sz;

				//printf("%d: recv from %d\n", rank, j);
				for(i = 0; i < count; i++) {
					mmpi_recv(j, buf, &sz);
					assert(sz == size);
					assert(buf[0] == (char)i);
					assert(buf[size-1] == (char)i);
				}
			}
			gettimeofday(&tv2, NULL);
			delta = (float)(tv2.tv_usec - tv1.tv_usec) / 1E6
				+ (float)(tv2.tv_sec - tv1.tv_sec);
			printf("throughput: %6.1f MB/s (%.2fs) chunk % 8d\n",
			       (float)((nprocs-1) * (THRTEST_VOLUME >> 20)) / delta,
			       delta, size);
			mmpi_barrier();
		}
	}
	free(buf);

	mmpi_barrier();
	printf("SUCCESS! rank %d exits\n", rank);

	return 0;
}

