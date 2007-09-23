/*
 * map_cache.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef MAP_CACHE_H
#define MAP_CACHE_H

#include "fdproxy.h"
#include "driller.h"

struct map_cache {
	struct map_rec mc_map;
	void *mc_addr;
};

extern struct map_cache *map_cache_lookup(struct fdkey *key);
extern struct map_cache *map_cache_install(struct map_rec *map,
					   struct fdkey *key);
extern void map_cache_update(struct map_rec *map, struct fdkey *key,
			     struct map_cache *mc);
extern void map_cache_remove(struct fdkey *key);
extern void map_cache_init(void);

#endif /* MAP_CACHE_H */
