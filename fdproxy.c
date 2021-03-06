/*
 * fdproxy.c
 *
 * Copyright 2007 Jean-Marc Saffroy <saffroy@gmail.com>
 * This file is part of the Driller library.
 * Driller is free software, distributed under the terms of the
 * GNU Lesser General Public License version 2.1.
 *
 * enable client processes to exchange file descriptors
 * using Unix sockets
 * a daemon process is forked and receives/serves file descriptors
 * from/to client processes
 */

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <poll.h>
#include <search.h>

#include "log.h"
#include "tunables.h"
#include "fdproxy.h"
#include "fdproxy_internal.h"

static int fdproxy_id;
static int client_sock = -1;
static int server_sock = -1;
static int fdtable_hsize = FDTABLE_HSIZE_INIT;
static char keystr_buf[30];

/*
 * give a specific id to a fd key
 */
void fdproxy_set_key_id(struct fdkey *key, int id) {
	key->pid = FDKEY_WELLKNOWN;
	key->fd = id;
}

/*
 * return a (static) string representing a key
 */
char *fdproxy_keystr(struct fdkey *key) {
	int len;

	len = snprintf(keystr_buf, sizeof(keystr_buf),
		       "%d/%d", key->pid, key->fd);
	assert(len < sizeof(keystr_buf));
	return keystr_buf;
}

static void fdtable_init(void) {
	int rc;

	rc = hcreate(fdtable_hsize);
	assert(rc != 0);
}

/*
 * record a (key, fd) pair
 */
static void fdtable_hash(int fd, struct fdkey *key) {
	char *buf;
	ENTRY e, *ep;

	buf = fdproxy_keystr(key);

	dbg("add <%s> = %d", buf, fd);

	e.key = buf;
	ep = hsearch(e, FIND);
	if(ep != NULL) {
		ep->data = (void*)(long)fd;
		return;
	}

	e.key = strdup(buf);
	e.data = (void*)(long)fd;
	assert(e.key != NULL);
	ep = hsearch(e, ENTER);
	if(ep == NULL) {
		/* retry with larger htable */
		fdtable_hsize += fdtable_hsize/2;
		if(hcreate(fdtable_hsize) == 0)
			err("cannot grow htable (size=%d)",
			    fdtable_hsize);
		ep = hsearch(e, ENTER);
		if(ep == NULL)
			err("cannot insert into htable (size=%d)",
			    fdtable_hsize);
	}
}

/*
 * find and return fd matching key
 */
static int fdtable_lookup(struct fdkey *key) {
	char *buf;
	ENTRY e, *ep;
	int fd;

	buf = fdproxy_keystr(key);

	e.key = buf;
	ep = hsearch(e, FIND);
	if(ep != NULL)
		fd = (int)(long)ep->data;
	else {
		dbg("cannot find '%s' in htable", buf);
		fd = -1;
	}
	dbg("lookup <%s> = %d", buf, fd);
	return fd;
}

/*
 * remove record of (key, fd) pair
 */
static int fdtable_unhash(struct fdkey *key) {
	char *buf;
	ENTRY e, *ep;
	int fd;

	buf = fdproxy_keystr(key);

	e.key = buf;
	ep = hsearch(e, FIND);
	if(ep != NULL) {
		fd = (int)(long)ep->data;
		ep->data = (void*)(long)-1;
	} else {
		dbg("cannot find '%s' in htable", buf);
		fd = -1;
	}
	dbg("unhash <%s> = %d", buf, fd);
	return fd;
}

/*
 * unhash and close fd for the given key
 */
static void fdtable_invalidate(struct fdkey *key) {
	int fd;

	dbg("invalidate <%s>", fdproxy_keystr(key));
	fd = fdtable_unhash(key);
	if(fd != -1) {
		if(close(fd) != 0)
			perr("close");
	}
}

/* recv_request, send_request implement fd passing with UNIX socket ancillary data
   see unix(7) cmsg(3) recvmsg(2) readv(2) */
static void recv_request(int sock, void *buf, size_t buflen,
			 int *fds, size_t fd_len) {
	struct msghdr msgh;
	struct iovec iov;
	struct cmsghdr *cmsg;
	size_t ctl_size = sizeof(*fds) * fd_len;
	char ctl_buf[CMSG_SPACE(ctl_size)];
	size_t len;

	iov.iov_base = buf;
	iov.iov_len = buflen;

	memset(&msgh, 0, sizeof(msgh));
	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = ctl_buf;
	msgh.msg_controllen = sizeof(ctl_buf);

	len = recvmsg(sock, &msgh, 0);
	if(len == (size_t) (-1))
		perr("recvmsg");
	if(len != buflen)
		err("len (%zd) != buflen (%zd)", len, buflen);
	if(fd_len == 0)
		return;
	if(msgh.msg_flags & MSG_CTRUNC)
		err("msgh.flags has MSG_CTRUNC:"
		    " check the number of open file descriptors");
	assert(msgh.msg_controllen >= sizeof(struct cmsghdr));

	for(cmsg = CMSG_FIRSTHDR(&msgh);
	     cmsg != NULL;
	     cmsg = CMSG_NXTHDR(&msgh, cmsg)) {

		if(cmsg->cmsg_level == SOL_SOCKET
		   && cmsg->cmsg_type == SCM_RIGHTS) {
			assert(cmsg->cmsg_len == CMSG_LEN(ctl_size));
			memcpy(fds, CMSG_DATA(cmsg), ctl_size);
			return;
		}
	}

	err("inconsistent cmsg structure");
}

static void send_request(int sock, void *buf, size_t buflen,
			 int *fds, size_t fd_len) {
	struct msghdr msgh;
	struct iovec iov;
	struct cmsghdr *cmsg;
	size_t ctl_size = sizeof(*fds) * fd_len;
	char ctl_buf[CMSG_SPACE(ctl_size)];
	size_t len;

	iov.iov_base = buf;
	iov.iov_len = buflen;

	memset(&msgh, 0, sizeof(msgh));
	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = ctl_buf;
	msgh.msg_controllen = sizeof(ctl_buf);

	cmsg = CMSG_FIRSTHDR(&msgh);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(ctl_size);
	msgh.msg_controllen = cmsg->cmsg_len;
	memcpy(CMSG_DATA(cmsg), fds, ctl_size);

	len = sendmsg(sock, &msgh, 0);
	if(len == (size_t) (-1))
		perr("sendmsg");
	if(len != buflen)
		err("sendmsg returned %zd expected %zd", len, buflen);
}

/*
 * daemon: receive fd after reception of FD_NEW_KEY
 */
static int fdproxy_server_get_fd(int sock, struct fdkey *key) {
	struct fdproxy_request req;
	int fd;

	/* expect FD_ADD_KEY with ancillary fd */
	recv_request(sock, &req, sizeof(req), &fd, 1);
	assert(req.magic == REQUEST_MAGIC);
	assert(req.type == FD_ADD_KEY);
	assert(memcmp(&req.key, key, sizeof(*key)) == 0);
	return fd;
}

/*
 * daemon: send ACK after reception of FD_ADD_KEY
 */
static void fdproxy_server_get_fd_ack(int sock, struct fdkey *key) {
	struct fdproxy_request req;

	/* send ack to FD_ADD_KEY */
	req.magic = REQUEST_MAGIC;
	req.key = *key;
	req.type = FD_ADD_KEY_ACK;
	send_request(sock, &req, sizeof(req), NULL, 0);
}

/*
 * daemon: send key after reception of FD_REQ_KEY
 */
static void fdproxy_server_send_fd(int sock, int fd, struct fdkey *key) {
	struct fdproxy_request req;

	/* send response to FD_REQ_KEY */
	req.magic = REQUEST_MAGIC;
	req.key = *key;
	if(fd >= 0) {
		req.type = FD_RSP_KEYFOUND;
		send_request(sock, &req, sizeof(req), NULL, 0);

		req.type = FD_RSP_KEY;
		send_request(sock, &req, sizeof(req), &fd, 1);
	} else {
		req.type = FD_RSP_NOKEY;
		send_request(sock, &req, sizeof(req), NULL, 0);
	}
}

/*
 * update client state when receiving messages
 */
static void fdproxy_handle_in(struct connection_context *cl) {
	struct fdproxy_request req;
	int fd;

	switch(cl->state) {
	case STATE_IDLE:
		recv_request(cl->sock, &req, sizeof(req), NULL, 0);
		assert(req.magic == REQUEST_MAGIC);
		switch(req.type) {
		case FD_NEW_KEY:
			cl->state = STATE_RCV_NEW_KEY;
			cl->rcvd_key = req.key;
			break;
		case FD_REQ_KEY:
			cl->state = STATE_RCV_REQ_KEY;
			cl->rcvd_key = req.key;
			break;
		case FD_INVAL_KEY:
			fdtable_invalidate(&req.key);
			break;
		default:
			err("bad request %d", req.type);
		}
		break;

	case STATE_RCV_NEW_KEY:
		fd = fdproxy_server_get_fd(cl->sock, &cl->rcvd_key);
		fdtable_hash(fd, &cl->rcvd_key);
		cl->state = STATE_RCV_ADD_KEY;
		break;

	default:
		err("bad client state: %d", cl->state);
	}
}

/*
 * update client state when sending messages
 */
static void fdproxy_handle_out(struct connection_context *cl) {
	int fd;

	switch(cl->state) {
	case STATE_RCV_REQ_KEY:
		fd = fdtable_lookup(&cl->rcvd_key);
		fdproxy_server_send_fd(cl->sock, fd, &cl->rcvd_key);
		cl->state = STATE_IDLE;
		memset(&cl->rcvd_key, 0, sizeof(cl->rcvd_key));
		break;
	case STATE_RCV_ADD_KEY:
		fdproxy_server_get_fd_ack(cl->sock, &cl->rcvd_key);
		cl->state = STATE_IDLE;
		memset(&cl->rcvd_key, 0, sizeof(cl->rcvd_key));
		break;

	default:
		err("bad client state: %d", cl->state);
	}
}

/*
 * client: send (key, fd) pair to daemon
 */
void fdproxy_client_send_fd(int fd, struct fdkey *key) {
	struct fdproxy_request req;

	dbg("send <%s>", fdproxy_keystr(key));
	/* send request notifying new key */
	if(key->pid != FDKEY_WELLKNOWN) {
		key->pid = getpid();
		key->fd = fd;
	}
	req.magic = REQUEST_MAGIC;
	req.type = FD_NEW_KEY;
	req.key = *key;
	send_request(client_sock, &req, sizeof(req), NULL, 0);

	/* send fd proper */
	req.type = FD_ADD_KEY;
	send_request(client_sock, &req, sizeof(req), &fd, 1);

	/* receive ack */
	recv_request(client_sock, &req, sizeof(req), NULL, 0);
	assert(req.magic == REQUEST_MAGIC);
	assert(memcmp(&req.key, key, sizeof(*key)) == 0);
	assert(req.type == FD_ADD_KEY_ACK);
}

/*
 * client: request fd for the given key from daemon
 */
int fdproxy_client_get_fd(struct fdkey *key) {
	struct fdproxy_request req;
	int fd;

	/* send request for key */
	req.magic = REQUEST_MAGIC;
	req.type = FD_REQ_KEY;
	req.key = *key;
	send_request(client_sock, &req, sizeof(req), NULL, 0);

	/* receive response */
	recv_request(client_sock, &req, sizeof(req), NULL, 0);
	assert(req.magic == REQUEST_MAGIC);
	assert(memcmp(&req.key, key, sizeof(*key)) == 0);
	switch(req.type) {
	case FD_RSP_NOKEY:
		return -1;
	case FD_RSP_KEYFOUND:
		break;
	default:
		err("bad server reply: %d", req.type);
	}

	/* receive fd */
	recv_request(client_sock, &req, sizeof(req), &fd, 1);
	assert(req.magic == REQUEST_MAGIC);
	assert(memcmp(&req.key, key, sizeof(*key)) == 0);
	assert(req.type == FD_RSP_KEY);

	dbg("get <%s> = %d", fdproxy_keystr(key), fd);
	return fd;
}

/*
 * client: tell daemon to drop fd paired to given key
 */
void fdproxy_client_invalidate_fd(struct fdkey *key) {
	struct fdproxy_request req;

	dbg("invalidate <%s>", fdproxy_keystr(key));
	/* send request notifying stale key */
	req.magic = REQUEST_MAGIC;
	req.type = FD_INVAL_KEY;
	req.key = *key;
	send_request(client_sock, &req, sizeof(req), NULL, 0);
}


/*
 * init addr struct to bind UNIX socket in "abstract" name space
 */
static void fdproxy_init_addr(struct sockaddr_un *addr) {
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
#ifdef linux
	snprintf(&addr->sun_path[1], sizeof(addr->sun_path)-1,
		 "fdproxy-%d", fdproxy_id);
#else
	snprintf(addr->sun_path, sizeof(addr->sun_path),
		 "%s/fdproxy-%d", TMPDIR, fdproxy_id);
#endif
}

/*
 * daemon: main loop
 */
static void fdproxy_daemon(void) {
	struct sockaddr_un addr;

	struct pollfd ctx_pollfd[FDPROXY_MAX_CLIENTS+1]; /* +1 for server sock */
	struct connection_context ctx[FDPROXY_MAX_CLIENTS];
	int nctx = 0;

	fdtable_init();

	/* bind socket, listen */
	server_sock = socket(PF_UNIX, SOCK_STREAM, 0);
	fdproxy_init_addr(&addr);
#ifndef linux
	unlink(addr.sun_path);
#endif
	if(bind(server_sock, (struct sockaddr *) &addr, sizeof(addr)))
		perr("bind");
	if(listen(server_sock, 5))
		perr("listen");

	for(;;) {
		int i, rc, nactive;

		for(i = 0, nactive = 0; i < nctx; i++) {
			if(ctx[i].sock == -1)
				continue;
			ctx_pollfd[nactive].fd  = ctx[i].sock;
			switch(ctx[i].state) {
			case STATE_IDLE:	/* expect any request */
			case STATE_RCV_NEW_KEY:	/* expect FD_ADD_KEY */
				ctx_pollfd[nactive].events = POLLIN;
				break;
			case STATE_RCV_REQ_KEY:	/* need to send response */
			case STATE_RCV_ADD_KEY:	/* need to send ack */
				ctx_pollfd[nactive].events = POLLOUT;
				break;
			}
			nactive++;
		}
		if((nactive == 0) && (nctx != 0)) {
			dbg("last client disconnected, exiting");
			_exit(0);
		}
		ctx_pollfd[nactive].fd = server_sock;
		ctx_pollfd[nactive].events = POLLIN;

		rc = poll(ctx_pollfd, nactive+1, -1);
		if(rc < 0)
			perr("poll");

		for(i = 0, nactive = 0; i < nctx; i++) {
			int revents = ctx_pollfd[nactive].revents;

			if(ctx[i].sock == -1)
				continue;
			nactive++;
			if(revents & POLLHUP) {
				ctx[i].sock = -1;
				dbg("client %d closed its connection", i);
				continue;
			}
			if(revents & (POLLERR|POLLNVAL)) {
				err("client %d revents = %s%s",
				    i,
				    revents & POLLERR ? "ERR " : "",
				    revents & POLLNVAL ? "NVAL " : "");
			}
			if(revents & POLLIN)
				fdproxy_handle_in(ctx + i);
			if(revents & POLLOUT)
				fdproxy_handle_out(ctx + i);
		}

		/* accept new clients */
		if(ctx_pollfd[nactive].revents & POLLIN) {
			assert(nctx < FDPROXY_MAX_CLIENTS);

			ctx[nctx].sock = accept(server_sock, NULL, NULL);
			if(ctx[nctx].sock < 0)
				perr("accept");
			ctx[nctx].state = STATE_IDLE;
			nctx++;
		}
	}

	/* NOT REACHED */
}

void fdproxy_init(int proxy_id, int do_fork) {
	struct sockaddr_un addr;
	int i, rc;

	fdproxy_id = proxy_id;
	if(do_fork) {
		int rc;

		rc = fork();
		assert(rc >= 0);
		if(rc == 0)
			fdproxy_daemon(); /* NO RETURN */
	}

	/* connect to daemon */
	client_sock = socket(PF_UNIX, SOCK_STREAM, 0);
	fdproxy_init_addr(&addr);
	for(i = 0; i < CONNECT_TIMEOUT; i++) {
		rc = connect(client_sock, (struct sockaddr *) &addr, sizeof(addr));
		if(rc == 0)
			break;
		if(errno != ECONNREFUSED && errno != ENOENT)
			perr("connect");
		sleep(1);
	}
	if(rc)
		err("could not connect to fdproxy daemon after %d seconds",
		    CONNECT_TIMEOUT);
}
