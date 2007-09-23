/*
 * driller.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef DRILLER_H
#define DRILLER_H

struct map_rec {
	void *start;
	void *end;
	int prot;
	off_t offset;
	char *path;
	int fd;
	void *user_data;
};

extern void driller_init(void);
extern void driller_register_map_invalidate_cb(void (*f)(struct map_rec *map));
extern struct map_rec *driller_lookup_map(void *start, size_t length);
extern void *driller_install_map(struct map_rec *map);
extern void driller_remove_map(struct map_rec *map, void *p);
extern void *driller_malloc(size_t bytes);
extern void driller_free(void *mem);

#endif /* DRILLER_H */
