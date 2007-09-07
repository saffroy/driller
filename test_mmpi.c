#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "mmpi.h"
#include "log.h"

#define THRTEST_CHUNK_SIZE (1 << 24) /* 16 MB */
#define THRTEST_VOLUME (1 << 30) /* 1 GB */
#define THRTEST_CHUNK_COUNT (THRTEST_VOLUME/THRTEST_CHUNK_SIZE)

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
			mmpi_send(0, (char*)&rank, sizeof(rank));
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
				mmpi_recv(j, (char*)&r, &sz);
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
	buf = malloc(THRTEST_CHUNK_SIZE);
	if(rank != 0) {
		int i;

		printf("%d: send to %d\n", rank, 0);
		for(i = 0; i < THRTEST_CHUNK_COUNT; i++) {
			memset(buf, (char)i, THRTEST_CHUNK_SIZE);
			mmpi_send(0, buf, THRTEST_CHUNK_SIZE);
		}
	} else {
		int i, j;
		struct timeval tv1, tv2;
		float delta;

		printf("now time send/recv throughput (%d MB in %dkB chunks)...\n", 
		       THRTEST_VOLUME >> 20, THRTEST_CHUNK_SIZE >> 10);
		gettimeofday(&tv1, NULL);

		for(j = 1; j < nprocs; j++) {
			size_t sz;

			printf("%d: recv from %d\n", rank, j);
			for(i = 0; i < THRTEST_CHUNK_COUNT; i++) {
				mmpi_recv(j, buf, &sz);
				assert(sz == THRTEST_CHUNK_SIZE);
				assert(buf[0] == (char)i);
				assert(buf[THRTEST_CHUNK_SIZE-1] == (char)i);
			}
		}
		gettimeofday(&tv2, NULL);
		delta = (float)(tv2.tv_usec - tv1.tv_usec) / 1000000
			+ (float)(tv2.tv_sec - tv1.tv_sec);
		printf("average send/recv throughput: %.2f MB/s (%.2fs)\n",
		       (float)((nprocs-1) * THRTEST_VOLUME >> 20) / delta,
		       delta);
	}
	free(buf);

	mmpi_barrier();
	printf("SUCCESS! rank %d exits\n", rank);

	return 0;
}

