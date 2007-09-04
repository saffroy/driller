#ifndef FDPROXY_H
#define FDPROXY_H

#define FDPROXY_MAX_CLIENTS 32
#define CONNECT_TIMEOUT 5 /* seconds */

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
	long key;
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
	int rcvd_key;
};

void fdproxy_init(int do_fork, int proxy_id);
void fdproxy_client_send_fd(int fd, long key);
int fdproxy_client_get_fd(long key);

#endif /* FDPROXY_H */
