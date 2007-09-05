#include <stdio.h>
#include <assert.h>

#include "mmpi.h"
#include "log.h"
#include "fdproxy.h"

static void usage(char *progname) {
	err("usage: %s <job id> <job size> <rank>", progname);
}

int main(int argc, char**argv) {
	int jobid, nprocs, rank, i;
	struct fdkey key1, key2;
	size_t sz;

	/* parse args */
	if(argc != 4)
		usage(argv[0]);
	jobid = atoi(argv[1]);
	nprocs = atoi(argv[2]);
	rank = atoi(argv[3]);

	mmpi_init(jobid, nprocs, rank);
	mmpi_barrier();

	/* init fdproxy */
	if(rank == 0)
		/* rank 0 forks fdproxy daemon */
		fdproxy_init(1, jobid);
	else
		fdproxy_init(0, jobid);

	if(rank == 0) {
		printf("rank 0 sends stdout\n");
		fdproxy_client_send_fd(1, &key1);
		for(i = 1; i < nprocs; i++)
			mmpi_send(i, &key1, sizeof(key1));
		printf("rank 0 sends stderr\n");
		fdproxy_client_send_fd(2, &key2);
		for(i = 1; i < nprocs; i++)
			mmpi_send(i, &key2, sizeof(key2));
	} else {
		mmpi_recv(0, &key1, &sz);
		assert(sz == sizeof(key1));
		mmpi_recv(0, &key2, &sz);
		assert(sz == sizeof(key2));
	}

	if(rank != 0) {
		int fd_out, fd_err;
		FILE *new_out, *new_err;

		printf("rank %d fetches new stdout\n", rank);
		fd_out = fdproxy_client_get_fd(&key1);
		printf("rank %d fetches new stderr\n", rank);
		fd_err = fdproxy_client_get_fd(&key2);

		new_out = fdopen(fd_out, "w");
		new_err = fdopen(fd_err, "w");

		fprintf(new_out, "rank %d writes to rank 0's stdout\n", rank);
		fprintf(new_err, "rank %d writes to rank 0's stderr\n", rank);
	}

	mmpi_barrier();
	printf("SUCCESS! rank %d exits\n", rank);

	return 0;
}
