/*
 * solaris.c
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 * fetch the list of memory mappings in the current process from /proc
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <procfs.h>

#include "driller_internal.h"
#include "log.h"

/*
 * parse and record the content of /proc/$pid/maps
 */
void map_parse(void) {
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path),
		 "/proc/%d/map", (int)getpid());

	fd = open(path, O_RDONLY);
	if(fd < 0)
		perr("open");
	for(;;) {
		prmap_t map;
		int prot;

		if(read(fd, &map, sizeof(map)) < sizeof(map))
			break;
		if(map.pr_mflags & MA_SHARED)
			/* not a private mapping, skip it */
			continue;
		prot =    (map.pr_mflags & MA_READ  ? PROT_READ : 0)
			| (map.pr_mflags & MA_WRITE ? PROT_WRITE : 0)
			| (map.pr_mflags & MA_EXEC  ? PROT_EXEC : 0);
		map_record((void*)map.pr_vaddr,
			   (void*)map.pr_vaddr + map.pr_size,
			   prot, map.pr_offset, map.pr_mapname, -1);
	}
	close(fd);
}
