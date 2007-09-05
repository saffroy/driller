#ifndef DRILLER_H
#define DRILLER_H

struct map_rec {
	off_t start;
	off_t end;
	int prot;
	off_t offset;
	char *path;
	int fd;
	void *user_data;
};

void driller_init(void);
void driller_register_map_invalidate_cb(void (*f)(struct map_rec *map));
struct map_rec *driller_lookup_map(void *start, size_t length);
void *driller_install_map(struct map_rec *map);
void driller_remove_map(struct map_rec *map, void *p);
void *driller_malloc(size_t bytes);
void driller_free(void *mem);

#endif /* DRILLER_H */
