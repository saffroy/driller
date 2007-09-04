#include <stdio.h>

#include "mmpi.h"
#include "log.h"
#include "fdproxy.h"

static void usage(char *progname) {
	err("usage: %s <job id> <job size> <rank>", progname);
}

int main(int argc, char**argv) {
	int jobid, nprocs, rank;

	/* parse args */
	if(argc != 4)
		usage(argv[0]);
	jobid = atoi(argv[1]);
	nprocs = atoi(argv[2]);
	rank = atoi(argv[3]);

	/* init fdproxy */
	if(rank == 0)
		/* fork daemon */
		fdproxy_init(1, jobid);
	else
		fdproxy_init(0, jobid);

	if(rank == 0) {
		printf("rank 0 sends stdout\n");
		fdproxy_client_send_fd(1, 1);
		printf("rank 0 sends stderr\n");
		fdproxy_client_send_fd(2, 2);
	}

	mmpi_init(jobid, nprocs, rank);
	mmpi_barrier();

	if(rank != 0) {
		int fd_out, fd_err;
		FILE *new_out, *new_err;

		printf("rank %d fetches new stdout\n", rank);
		fd_out = fdproxy_client_get_fd(1);
		printf("rank %d fetches new stderr\n", rank);
		fd_err = fdproxy_client_get_fd(2);

		new_out = fdopen(fd_out, "w");
		new_err = fdopen(fd_err, "w");

		fprintf(new_out, "rank %d uses new_out\n", rank);
		fprintf(new_err, "rank %d uses new_err\n", rank);
	}

	mmpi_barrier();
	printf("SUCCESS! rank %d exits\n", rank);

	return 0;
}
