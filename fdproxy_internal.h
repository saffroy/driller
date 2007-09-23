/*
 * fdproxy_internal.h
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 */

#ifndef FDPROXY_INTERNAL_H
#define FDPROXY_INTERNAL_H

#define FDPROXY_MAX_CLIENTS 32
#define CONNECT_TIMEOUT 5 /* seconds */
#define FDTABLE_HSIZE_INIT 32

#define FDKEY_WELLKNOWN ((pid_t)0xf00a5a5)

/*
 * request FD_NEW_KEY
 *  notify proxy of new (fd,key) pair
 *  followed by FD_ADD_KEY which has ancillary fd
 *   response: FD_ADD_KEY_ACK
 *
 * request FD_REQ_KEY
 *  ask for fd matching given key
 *  if key found:
 *   response FD_RSP_KEYFOUND
 *   response FD_RSP_KEY which has ancillary fd
 *  else:
 *   response FD_RSP_NOKEY
 */

#define REQUEST_MAGIC 0xf004242
enum fdproxy_reqtype {
	FD_NEW_KEY,
	FD_ADD_KEY,
	FD_ADD_KEY_ACK,
	FD_REQ_KEY,
	FD_RSP_KEYFOUND,
	FD_RSP_KEY,
	FD_RSP_NOKEY,
	FD_INVAL_KEY,
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
 * STATE_RCV_ADD_KEY
 *  client has sent FD_ADD_KEY, need to send FD_ADD_KEY_ACK
 * STATE_RCV_REQ_KEY
 *  client has sent FD_REQ_KEY, need to send FD_RSP_{KEYFOUND,KEY,NOKEY}
 */

enum conn_state {
	STATE_IDLE,
	STATE_RCV_NEW_KEY,
	STATE_RCV_ADD_KEY,
	STATE_RCV_REQ_KEY,
};
struct connection_context {
	int sock;
	enum conn_state state;
	struct fdkey rcvd_key;
};

#endif /* FDPROXY_INTERNAL_H */
