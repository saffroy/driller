#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "mmpi.h"
#include "log.h"
#include "fdproxy.h"

static void usage(char *progname) {
	err("usage: %s <job id> <job size> <rank> <iter>", progname);
}

int main(int argc, char**argv) {
	int jobid, nprocs, rank, iter, i, j;
	struct fdkey key1, key2;
	size_t sz;

	/* parse args */
	if(argc != 5)
		usage(argv[0]);
	jobid = atoi(argv[1]);
	nprocs = atoi(argv[2]);
	rank = atoi(argv[3]);
	iter = atoi(argv[4]);

	mmpi_init(jobid, nprocs, rank);
	mmpi_barrier();

	/* fdproxy already initialized by mmpi_init */

	/* let siblings duplicate stdout/stderr from rank 0
	 * - for stdout we have fdproxy make the fd key,
	 *   and we send it to siblings
	 * - for stderr we use a "well-known" key id
	 */
	if(rank == 0) {
		printf("rank 0 sends stdout\n");
		memset(&key1, 0, sizeof(key1));
		fdproxy_client_send_fd(1, &key1);
		for(i = 1; i < nprocs; i++)
			mmpi_send(i, &key1, sizeof(key1));

		printf("rank 0 sends stderr\n");
		fdproxy_set_key_id(&key2, 0x123);
		fdproxy_client_send_fd(2, &key2);
	} else {
		mmpi_recv(0, &key1, &sz);
		assert(sz == sizeof(key1));
		fdproxy_set_key_id(&key2, 0x123);
	}

	mmpi_barrier();

	if(rank != 0) {
		int fd_out, fd_err;
		FILE *new_out, *new_err;
		struct fdkey bogus;

		printf("rank %d fetches bogus fd\n", rank);
		fd_err = fdproxy_client_get_fd(&bogus);
		assert(fd_err == -1);

		printf("rank %d fetches new stdout\n", rank);
		fd_out = fdproxy_client_get_fd(&key1);
		printf("rank %d fetches new stderr\n", rank);
		fd_err = fdproxy_client_get_fd(&key2);

		new_out = fdopen(fd_out, "w");
		if(new_out == NULL)
			perr("fdopen");
		new_err = fdopen(fd_err, "w");
		if(new_err == NULL)
			perr("fdopen");

		fprintf(new_out, "rank %d writes to rank 0's stdout\n", rank);
		fprintf(new_err, "rank %d writes to rank 0's stderr\n", rank);

		fclose(new_out);
		fclose(new_err);
	}

	mmpi_barrier();

	/* repeatedly fetch rank 0 fd 1 */
	for(i = 0; i < iter/nprocs ; i++) {
		int fd;

		fd = fdproxy_client_get_fd(&key1);
		assert(fd != -1);
		assert(close(fd) == 0);
	}

	mmpi_barrier();

	if(rank == 0) {
		fdproxy_client_invalidate_fd(&key1);
		fdproxy_client_invalidate_fd(&key2);
	}

	mmpi_barrier();

	/* repeatedly send / inval rank 0 fd 1 */
	for(i = 0; i < iter/nprocs ; i++) {
		int fd;
		size_t sz;
		struct fdkey key;

		if(rank == 0) {
			fd = dup(1);
			assert(fd >= 0);
			memset(&key, 0, sizeof(key));
			fdproxy_client_send_fd(fd, &key);
			for(j = 1; j < nprocs; j++)
				mmpi_send(j, &key, sizeof(key));
			mmpi_barrier();
			fdproxy_client_invalidate_fd(&key);
			assert(close(fd) == 0);
		} else {
			mmpi_recv(0, &key, &sz);
			assert(sz == sizeof(key));
			fd = fdproxy_client_get_fd(&key);
			assert(fd != -1);
			assert(close(fd) == 0);
			mmpi_barrier();
		}
	}

	mmpi_barrier();
	printf("SUCCESS! rank %d exits\n", rank);

	return 0;
}
