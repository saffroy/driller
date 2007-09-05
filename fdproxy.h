#ifndef FDPROXY_H
#define FDPROXY_H

/* this identifies a file among all processes */
#define FDKEY_WELLKNOWN ((pid_t)0xf00a5a5)
struct fdkey {
	pid_t pid;	/* pid of creator, or FDKEY_WELLKNOWN */
	int fd;		/* fd num used by creator, or well-known id */
};

static inline void fdproxy_set_key_id(struct fdkey *key, int id) {
	key->pid = FDKEY_WELLKNOWN;
	key->fd = id;
}

void fdproxy_init(int proxy_id, int do_fork);
void fdproxy_client_send_fd(int fd, struct fdkey *key);
int fdproxy_client_get_fd(struct fdkey *key);
void fdproxy_client_invalidate_fd(struct fdkey *key);

#endif /* FDPROXY_H */
