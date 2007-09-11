#include <unistd.h>
#include <string.h>
#include <search.h>
#include <assert.h>

#include "log.h"
#include "fdproxy.h"
#include "driller.h"
#include "map_cache.h"

#define MAP_CACHE_HSIZE_INIT 32

static struct hsearch_data map_cache;
static int map_cache_hsize = MAP_CACHE_HSIZE_INIT;

static void map_cache_hash(struct map_cache *mc, struct fdkey *key) {
	char *buf;
	ENTRY e, *ep;
	int rc;

	buf = fdproxy_keystr(key);

	dbg("add <%s> = %p", buf, (mc ? mc->mc_addr : NULL));

	e.key = buf;
	rc = hsearch_r(e, FIND, &ep, &map_cache);
	if(ep != NULL) {
		ep->data = mc;
		return;
	}

	e.key = strdup(buf);
	assert(e.key != NULL);
	e.data = mc;
	rc = hsearch_r(e, ENTER, &ep, &map_cache);
	if(rc == 0) {
		/* retry with larger htable */
		map_cache_hsize += map_cache_hsize/2;
		if(hcreate_r(map_cache_hsize, &map_cache) == 0)
			err("cannot grow htable (size=%d)",
			    map_cache_hsize);
		rc = hsearch_r(e, ENTER, &ep, &map_cache);
		if(rc == 0)
			err("cannot insert into htable (size=%d)",
			    map_cache_hsize);
	}
}

struct map_cache *map_cache_unhash(struct fdkey *key) {
	char *buf;
	ENTRY e, *ep;
	int rc;
	struct map_cache *mc;

	buf = fdproxy_keystr(key);

	e.key = buf;
	rc = hsearch_r(e, FIND, &ep, &map_cache);
	if(ep != NULL) {
		mc = ep->data;
		ep->data = NULL;
	} else {
		dbg("cannot find '%s' in htable", buf);
		mc = NULL;
	}
	dbg("unhash <%s> = %p", buf, (mc ? mc->mc_addr : NULL));
	return mc;
}

struct map_cache *map_cache_lookup(struct fdkey *key) {
	char *buf;
	ENTRY e, *ep;
	int rc;
	struct map_cache *mc;

	buf = fdproxy_keystr(key);

	e.key = buf;
	rc = hsearch_r(e, FIND, &ep, &map_cache);
	if(ep != NULL) {
		mc = ep->data;
	} else {
		dbg2("cannot find '%s' in htable", buf);
		mc = NULL;
	}
	dbg2("lookup <%s> = %p", buf, (mc ? mc->mc_addr : NULL));
	return mc;
}

struct map_cache *map_cache_install(struct map_rec *map,
				    struct fdkey *key) {
	struct map_cache *mc;

	assert(map_cache_lookup(key) == NULL);

	mc = malloc(sizeof(*mc));
	assert(mc != NULL);
	memcpy(&mc->mc_map, map, sizeof(*map));
	mc->mc_addr = driller_install_map(map);
	map_cache_hash(mc, key);

	dbg("install <%s> @ %p", fdproxy_keystr(key), mc->mc_addr);
	return mc;
}

void map_cache_update(struct map_rec *map, struct fdkey *key,
		      struct map_cache *mc) {
	driller_remove_map(&mc->mc_map, mc->mc_addr);
	memcpy(&mc->mc_map, map, sizeof(*map));
	mc->mc_addr = driller_install_map(map);

	dbg("update <%s> @ %p", fdproxy_keystr(key), mc->mc_addr);
}

void map_cache_remove(struct fdkey *key) {
	struct map_cache *mc;

	mc = map_cache_unhash(key);
	if(mc != NULL) {
		dbg("remove <%s> = %p", fdproxy_keystr(key), mc->mc_addr);
		driller_remove_map(&mc->mc_map, mc->mc_addr);
		if(close(mc->mc_map.fd) != 0)
			perr("close");
		memset(mc, 0xf0, sizeof(mc));
		free(mc);
	}
}

void map_cache_init(void) {
	int rc;

	rc = hcreate_r(map_cache_hsize, &map_cache);
	assert(rc != 0);
}
