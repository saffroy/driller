#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "mmpi.h"
#include "log.h"


static void usage(char *progname) {
	err("usage: %s <job id> <job size> <rank> <iter>", progname);
}

int main(int argc, char**argv) {
	int jobid, nprocs, rank, iter;

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
	printf("SUCCESS! rank %d exits\n", rank);

	return 0;
}

