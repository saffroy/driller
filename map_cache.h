#ifndef MAP_CACHE_H
#define MAP_CACHE_H

#include "fdproxy.h"
#include "driller.h"

struct map_cache {
	struct map_rec map;
	void *address;
};

extern void map_cache_add(struct map_cache *mc, struct fdkey *key);
extern struct map_cache *map_cache_lookup(struct fdkey *key);
extern struct map_cache *map_cache_install(struct map_rec *map,
					   struct fdkey *key);
extern void map_cache_update(struct map_rec *map, struct fdkey *key,
			     struct map_cache *mc);
extern void map_cache_remove(struct fdkey *key);
extern void map_cache_init(void);

#endif /* MAP_CACHE_H */