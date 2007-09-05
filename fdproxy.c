#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <poll.h>
#include <search.h>

#include "log.h"
#include "fdproxy.h"

static int fdproxy_id;
static int client_sock = -1;
static int server_sock = -1;

static int fdtable_hsize = 32;

static void fdtable_init(void) {
	assert(hcreate(fdtable_hsize) != 0);
}

static void fdtable_add(int fd, struct fdkey *key) {
	char buf[20];
	int len;
	ENTRY e, *ep;

	dbg("add <%d/%d:%d>", key->pid, key->fd, fd);

	len = snprintf(buf, sizeof(buf), "%d/%d", key->pid, key->fd);
	assert(len < sizeof(buf));

	e.key = strdup(buf);
	assert(e.key != NULL);
	e.data = (void*)(long)fd;
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

static int fdtable_lookup(struct fdkey *key) {
	char buf[20];
	int len;
	ENTRY e, *ep;
	int fd;

	len = snprintf(buf, sizeof(buf), "%d/%d", key->pid, key->fd);
	assert(len < sizeof(buf));

	e.key = buf;
	ep = hsearch(e, FIND);
	if(ep != NULL)
		fd = (int)(long)ep->data;
	else {
		warn("cannot find '%s' in htable", buf);
		fd = -1;
	}
	dbg("lookup %s = %d", buf, fd);
	return fd;
}

/* rcv_request, send_request implement fd passing with UNIX socket ancillary data
   code copied (and fixed) from: 
   http://linux-vserver.org/Secure_chroot_Barrier */
static
void rcv_request(int fd, void *buf, size_t buf_len, int *fds, size_t fd_len) {
	struct msghdr msg;
	struct iovec iov[1];
	struct cmsghdr *cmptr;
	size_t len;
	size_t msg_size = sizeof(fds[0]) * fd_len;
	char control[CMSG_SPACE(msg_size)];

	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_control = fd_len > 0 ? control : NULL;
	msg.msg_controllen = fd_len > 0 ? sizeof(control) : 0;
	msg.msg_flags = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	iov[0].iov_base = buf;
	iov[0].iov_len = buf_len;

	do {
		len = recvmsg(fd, &msg, 0);
	} while (len == (size_t) (-1) && (errno == EINTR || errno == EAGAIN));

	if (len == (size_t) (-1))
		perr("recvmsg");
	if (len != buf_len)
		err("len (%zd) != buf_len (%zd)", len, buf_len);
	if (fd_len == 0)
		return;

	if (msg.msg_controllen < sizeof(struct cmsghdr))
		err("msg.msg_controllen < sizeof(struct cmsghdr)");

	for (cmptr = CMSG_FIRSTHDR(&msg); cmptr != NULL;
	     cmptr = CMSG_NXTHDR(&msg, cmptr)) {
		if (cmptr->cmsg_len != CMSG_LEN(msg_size) ||
		    cmptr->cmsg_level != SOL_SOCKET ||
		    cmptr->cmsg_type != SCM_RIGHTS)
			continue;

		memcpy(fds, CMSG_DATA(cmptr), msg_size);
		return;
	}

	err("bad data");
}

static void send_request(int fd, void const *buf, size_t buf_len, int const *fds,
			 size_t fd_len) {
	struct cmsghdr *cmsg;
	size_t msg_size = sizeof(fds[0]) * fd_len;
	char control[CMSG_SPACE(msg_size)];
	int *fdptr;
	struct iovec iov[1];
	size_t len;
	struct msghdr msg = {
		.msg_name = 0,
		.msg_namelen = 0,
		.msg_iov = iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof control,
		.msg_flags = 0,
	};

	iov[0].iov_base = (void *) (buf);
	iov[0].iov_len = buf_len;

	// from cmsg(3)
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(msg_size);
	msg.msg_controllen = cmsg->cmsg_len;

	fdptr = (void *) (CMSG_DATA(cmsg));
	memcpy(fdptr, fds, msg_size);

	len = sendmsg(fd, &msg, 0);
	if (len == (size_t) (-1))
		perr("sendmsg");
	if(len != buf_len)
		warn("sendmsg returned %zd expected %zd", len, buf_len);
}

static int fdproxy_server_get_fd(int sock, struct fdkey *key) {
	struct fdproxy_request req;
	int fd;

	/* expect FD_ADD_KEY with ancillary fd */
	rcv_request(sock, &req, sizeof(req), &fd, 1);
	assert(req.magic == REQUEST_MAGIC);
	assert(req.type == FD_ADD_KEY);
	assert(memcmp(&req.key, key, sizeof(*key)) == 0);
	return fd;
}

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

static void fdproxy_handle_in(struct connection_context *cl) {
	struct fdproxy_request req;
	int fd;

	switch(cl->state) {
	case STATE_IDLE:
		rcv_request(cl->sock, &req, sizeof(req), NULL, 0);
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
		default:
			err("bad request %d", req.type);
		}
		break;

	case STATE_RCV_NEW_KEY:
		fd = fdproxy_server_get_fd(cl->sock, &cl->rcvd_key);
		fdtable_add(fd, &cl->rcvd_key);
		cl->state = STATE_IDLE;
		break;

	default:
		err("bad client state: %d", cl->state);
	}
}

static void fdproxy_handle_out(struct connection_context *cl) {
	int fd;

	switch(cl->state) {
	case STATE_RCV_REQ_KEY:
		fd = fdtable_lookup(&cl->rcvd_key);
		fdproxy_server_send_fd(cl->sock, fd, &cl->rcvd_key);
		cl->state = STATE_IDLE;
		memset(&cl->rcvd_key, 0, sizeof(cl->rcvd_key));
		break;

	default:
		err("bad client state: %d", cl->state);
	}
}

void fdproxy_client_send_fd(int fd, struct fdkey *key) {
	struct fdproxy_request req;

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
}

int fdproxy_client_get_fd(struct fdkey *key) {
	struct fdproxy_request req;
	int fd;

	/* send request for key */
	req.magic = REQUEST_MAGIC;
	req.type = FD_REQ_KEY;
	req.key = *key;
	send_request(client_sock, &req, sizeof(req), NULL, 0);

	/* receive response */
	rcv_request(client_sock, &req, sizeof(req), NULL, 0);
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
	rcv_request(client_sock, &req, sizeof(req), &fd, 1);
	assert(req.magic == REQUEST_MAGIC);
	assert(memcmp(&req.key, key, sizeof(*key)) == 0);
	assert(req.type == FD_RSP_KEY);
	return fd;
}


/* init addr to bind UNIX socket in "abstract" name space */
static void fdproxy_init_addr(struct sockaddr_un *addr) {
	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	snprintf(&addr->sun_path[1], sizeof(addr->sun_path)-1,
		 "fdproxy-%d", fdproxy_id);
}

static void fdproxy_daemon(void) {
	struct sockaddr_un addr;

	struct pollfd ctx_pollfd[FDPROXY_MAX_CLIENTS+1]; /* +1 for server sock */
	struct connection_context ctx[FDPROXY_MAX_CLIENTS];
	int nctx = 0;

	fdtable_init();

	/* bind socket, listen */
	server_sock = socket(PF_UNIX, SOCK_STREAM, 0);
	fdproxy_init_addr(&addr);
	if(bind(server_sock, (struct sockaddr *) &addr, sizeof(addr)))
		perr("bind");
	if(listen(server_sock, 5))
		perr("listen");

	for(;;) {
		int i, rc;

		for(i = 0; i < nctx; i++) {
			ctx_pollfd[i].fd  = ctx[i].sock;
			switch(ctx[i].state) {
			case STATE_IDLE:	/* expect any request */
			case STATE_RCV_NEW_KEY:	/* expect FD_ADD_KEY */
				ctx_pollfd[i].events = POLLIN;
				break;
			case STATE_RCV_REQ_KEY:	/* need to send response */
				ctx_pollfd[i].events = POLLOUT;
				break;
			}
		}
		ctx_pollfd[nctx].fd = server_sock;
		ctx_pollfd[nctx].events = POLLIN;

		rc = poll(ctx_pollfd, nctx+1, -1);
		if(rc < 0)
			perr("poll");

		for(i = 0; i < nctx; i++) {
			int revents = ctx_pollfd[i].revents;

			//XXX todo: handle HUP gracefully
			if(revents & (POLLHUP|POLLERR|POLLNVAL)) {
				err("client %d revents = %s%s%s",
				    i,
				    revents & POLLHUP ? "HUP " : "",
				    revents & POLLERR ? "ERR " : "",
				    revents & POLLNVAL ? "NVAL " : "");
			}
			if(revents & POLLIN)
				fdproxy_handle_in(ctx + i);
			if(revents & POLLOUT)
				fdproxy_handle_out(ctx + i);
		}

		/* accept new clients */
		if(ctx_pollfd[nctx].revents & POLLIN) {
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
		if(errno != ECONNREFUSED)
			perr("connect");
		sleep(1);
	}
	if(rc)
		err("could not connect to fdproxy daemon after %d seconds",
		    CONNECT_TIMEOUT);
}
