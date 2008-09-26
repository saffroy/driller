/*
 * driller_internal.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef DRILLER_INTERNAL_H
#define DRILLER_INTERNAL_H

enum overload_t {
	OVERLOAD_REG,
	OVERLOAD_HEAP,
	OVERLOAD_STACK,
};

extern void map_record(void *start, void *end, int prot, off_t offset,
		       char *path, int fd);
extern void map_parse(void);

#endif /* DRILLER_INTERNAL_H */
