#ifndef DRILLER_H
#define DRILLER_H

struct map_rec {
	off_t start;
	off_t end;
	int prot;
	off_t offset;
	char *path;
	int fd;
	int user_flags;
};

void driller_init(void);
void driller_register_map_invalidate_cb(void (*f)(struct map_rec *map));
struct map_rec *driller_lookup_map(void *start, size_t length);

#endif /* DRILLER_H */
