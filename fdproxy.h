#ifndef FDPROXY_H
#define FDPROXY_H

#define FDPROXY_MAX_CLIENTS 32
#define CONNECT_TIMEOUT 5 /* seconds */

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

/*
 * request FD_NEW_KEY
 *  notify proxy of new (fd,key) pair
 *  followed by FD_ADD_KEY which has ancillary fd
 *  no response
 *
 * request FD_REQ_KEY
 *  ask for fd matching given key
 *  response FD_RSP_KEY which has ancillary fd
 */

#define REQUEST_MAGIC 0xf004242
enum fdproxy_reqtype {
	FD_NEW_KEY,
	FD_ADD_KEY,
	FD_REQ_KEY,
	FD_RSP_KEY,
};
struct fdproxy_request {
	int magic;
	int type;
	struct fdkey key;
};

/*
 * STATE_IDLE
 *  client has nothing in progress, expect anything
 * STATE_RCV_NEW_KEY
 *  client has sent FD_NEW_KEY, expect FD_ADD_KEY
 * STATE_RCV_REQ_KEY
 *  client has sent FD_REQ_KEY, need to send FD_RSP_KEY
 */

enum conn_state {
	STATE_IDLE,
	STATE_RCV_NEW_KEY,
	STATE_RCV_REQ_KEY,
};
struct connection_context {
	int sock;
	enum conn_state state;
	struct fdkey rcvd_key;
};

void fdproxy_init(int proxy_id, int do_fork);
void fdproxy_client_send_fd(int fd, struct fdkey *key);
int fdproxy_client_get_fd(struct fdkey *key);

#endif /* FDPROXY_H */
