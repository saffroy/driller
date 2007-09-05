#ifndef MMPI_H
#define MMPI_H

#include <sys/types.h>

extern void mmpi_init(int jobid, int nprocs, int rank);
extern void mmpi_barrier(void);
extern void mmpi_send(int rank, void *buf, size_t size);
extern void mmpi_recv(int rank, void *buf, size_t *size);

#endif /* MMPI_H */
