/*
 * fdproxy.h
 *
 * Copyright (C) Jean-Marc Saffroy <saffroy@gmail.com> 2007
 * This program is free software, distributed under the terms of the
 * GNU General Public License version 2.
 *
 */

#ifndef FDPROXY_H
#define FDPROXY_H

/* this identifies a file among all processes */
struct fdkey {
	pid_t pid;	/* pid of creator, or FDKEY_WELLKNOWN */
	int fd;		/* fd num used by creator, or well-known id */
};

extern void fdproxy_init(int proxy_id, int do_fork);
extern void fdproxy_client_send_fd(int fd, struct fdkey *key);
extern int fdproxy_client_get_fd(struct fdkey *key);
extern void fdproxy_client_invalidate_fd(struct fdkey *key);
extern char *fdproxy_keystr(struct fdkey *key);
extern void fdproxy_set_key_id(struct fdkey *key, int id);

#endif /* FDPROXY_H */
