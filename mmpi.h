/*
 * mmpi.h
 *
 * Copyright (C) Jean-Marc Saffroy <saffroy@gmail.com> 2007
 * This program is free software, distributed under the terms of the
 * GNU General Public License version 2.
 *
 */

#ifndef MMPI_H
#define MMPI_H

#include <sys/types.h>

extern void mmpi_init(int jobid, int nprocs, int rank);
extern void mmpi_barrier(void);
extern void mmpi_send(int rank, void *buf, size_t size);
extern void mmpi_recv(int rank, void *buf, size_t *size);

#endif /* MMPI_H */
