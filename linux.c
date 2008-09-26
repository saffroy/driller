/*
 * linux.c
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

#include "driller_internal.h"
#include "log.h"

/*
 * parse and record the content of /proc/self/maps
 */
void map_parse(void) {
	const char *file = "/proc/self/maps";
	int fd;
	char buf[4096] = {};
	uintmax_t start, end, offset;
	char prot_str[5];
	long maj, min, ino;
	char *line, *p;
	int lineno, i;
	int prot;

	/* no allocation while we read maps, otherwise map_rebuild might
	   try to map a segment that is gone (or has shrinked) */
	fd = open(file, O_RDONLY);
	if(fd < 0)
		perr("open");
	if(read(fd, buf, sizeof(buf)) < 0)
		perr("read");
	/* make sure we've read the whole thing */
	if(read(fd, buf, sizeof(buf)) != 0)
		err("could not read %s entirely", file);
	if(close(fd) != 0)
		perr("close");

	line = buf;
	for(lineno = 0; ; lineno++) {
		if(sscanf(line, "%jx-%jx %s %jx %lx:%lx %ld", 
			  &start, &end, prot_str, &offset,
			  &maj, &min, &ino) != 7) {
			p = strchrnul(line, '\n');
			*p = '\0';
			err("could not parse line %d: '%s'\n",
			    lineno, line);
		}

		p = line;
		for(i = 0; i < 5; i++)
			p = strchr(p+1, ' ');
		while(*p == ' ')
			p++;
		line = strchrnul(p, '\n');
		*line++ = '\0'; /* remove trailing \n */
		dbg("% 2d: %jx-%jx %s %jx %lx:%lx %ld '%s'\n",
		    lineno, start, end, prot_str, offset, maj, min, ino, p);
		prot = ((prot_str[0] == 'r') ? PROT_READ : 0)
			| ((prot_str[1] == 'w') ? PROT_WRITE : 0)
			| ((prot_str[2] == 'x') ? PROT_EXEC : 0);
		if(prot_str[3] == 'p') /* private mapping */
			map_record((void*)(uintptr_t)start,
				   (void*)(uintptr_t)end,
				   prot, offset, p, -1);
		if(*line == '\0')
			return;
	}
}
