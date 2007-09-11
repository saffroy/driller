/*
 * mini-MPI
 *  a shared-memory pseudo-MPI lib for testing ideas
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>

#include "mmpi.h"
#include "log.h"
#include "fdproxy.h"
#include "driller.h"
#include "spinlock.h"
#include "map_cache.h"
#include "mmpi_internal.h"

static struct shmem *shmem;
static int jobid;
static int nprocs;
static int rank;
static char flip = 1;

/*****************/

static void list_init(struct list_head *head) {
#ifndef NDEBUG
	head->magic = LIST_MAGIC;
#endif
	head->off_next = head->off_prev = 0;
}

static inline struct list_head *list_next(struct list_head *item) {
	return (struct list_head*)((char*)item + item->off_next);
}

static inline struct list_head *list_prev(struct list_head *item) {
	return (struct list_head*)((char*)item + item->off_prev);
}

static inline
off_t list_offset(struct list_head *left, struct list_head *right) {
	return (char*)right - (char*)left;
}

static inline
void list_set_next(struct list_head *left, struct list_head *right) {
	left->off_next = list_offset(left, right);
}

static inline
void list_set_prev(struct list_head *left, struct list_head *right) {
	left->off_prev = list_offset(left, right);
}

static inline void list_add(struct list_head *head, struct list_head *new) {
	struct list_head *next;

	assert(head->magic == LIST_MAGIC);
	assert(new->magic == LIST_MAGIC);

	next = list_next(head);
	list_set_next(new, next);
	list_set_prev(new, head);
	list_set_next(head, new);
	list_set_prev(next, new);
}

static inline
void list_add_tail(struct list_head *head, struct list_head *new) {
	struct list_head *last;

	assert(head->magic == LIST_MAGIC);

	last = list_prev(head);
	list_add(last, new);
}

static inline void list_del(struct list_head *item) {
	struct list_head *next, *prev;

	assert(item->magic == LIST_MAGIC);

	next = list_next(item);
	prev = list_prev(item);
	list_set_next(prev, next);
	list_set_prev(next, prev);
}

static inline int list_empty(struct list_head *head) {
	assert(head->magic == LIST_MAGIC);

	return (list_next(head) == head);
}

#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)

#define list_entry(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

#define list_for_each(pos, head) \
	for(pos = list_next(head); pos != head; pos = list_next(pos))

/*****************/

static void msg_queue_init(struct message_queue *q) {
	spin_lock_init(&q->q_lock);
	list_init(&q->q_list);
	q->q_length = 0;
}

static inline int __msg_queue_empty(struct message_queue *q) {
	return (q->q_length == 0);
}

static inline void __msg_enqueue(struct message_queue *q, struct message *m) {
	list_add_tail(&q->q_list, &m->m_list);
	q->q_length++;
}

static inline void __msg_enqueue_head(struct message_queue *q, struct message *m) {
	list_add(&q->q_list, &m->m_list);
	q->q_length++;
}

static inline void __msg_dequeue(struct message_queue *q, struct message *m) {
	list_del(&m->m_list);
	q->q_length--;
}

static inline void msg_queue_lock(struct message_queue *q) {
	spin_lock(&q->q_lock);
}

static inline void msg_queue_unlock(struct message_queue *q) {
	spin_unlock(&q->q_lock);
}

static inline void msg_enqueue(struct message_queue *q, struct message *m) {
	msg_queue_lock(q);
	__msg_enqueue(q, m);
	msg_queue_unlock(q);
}

static struct message *msg_dequeue_head(struct message_queue *q) {
	struct message *m = NULL;

	do {
		msg_queue_lock(q);
		if(!__msg_queue_empty(q)) {
			m = list_entry(list_next(&q->q_list),
				       struct message, m_list);
			__msg_dequeue(q, m);
		}
		msg_queue_unlock(q);
	} while(m == NULL);
	return m;
}

static struct message *msg_dequeue_from(struct message_queue *q, int src) {
	struct message *m = NULL;

	do {
		struct list_head *p;

		msg_queue_lock(q);
		list_for_each(p, &q->q_list) {
			m = list_entry(p, struct message, m_list);
			if(m->m_src == src) {
				__msg_dequeue(q, m);
				break;
			} else
				m = NULL;
		}
		msg_queue_unlock(q);
	} while(m == NULL);
	return m;
}

static struct message *msg_alloc(void) {
	struct shmem *my = shmem + rank;
	struct message *m;

	m = msg_dequeue_head(&my->free_q);
	assert(m->m_type == MSG_FREE);
	return m;
}

static void msg_free(struct message *m) {
	struct shmem *src = shmem + m->m_src;

	m->m_type = MSG_FREE;
	msg_queue_lock(&src->free_q);
	__msg_enqueue_head(&src->free_q, m);
	msg_queue_unlock(&src->free_q);
}

/*****************/

static void mmpi_send_driller_inval(int dest_rank,
				    struct map_rec *map, struct fdkey *key) {
	struct shmem *dest = shmem + dest_rank;
	struct message *m;

	dbg("send driller_inval to rank %d for <%s>",
	    dest_rank, fdproxy_keystr(key));

	m = msg_alloc();

	m->m_type = MSG_DRILLER_INVAL;
	memcpy(&m->m_drill.map, map, sizeof(*map));
	memcpy(&m->m_drill.key, key, sizeof(*key));

	msg_enqueue(&dest->recv_q, m);
}

static void mmpi_map_invalidate_cb(struct map_rec *map) {
	struct driller_udata *udata;
	struct fdkey *key;
	int i;

	udata = map->user_data;
	if(udata == NULL)
		return;
	key = &udata->key;
	dbg("invalidate <%s>", fdproxy_keystr(key));
	fdproxy_client_invalidate_fd(key);
	/* notify users of this map */
	for(i = 0; i < nprocs; i++)
		if(udata->references[i])
			mmpi_send_driller_inval(i, map, key);
	driller_free(udata);
	map->user_data = NULL;
}

static void mmpi_send_frags(int dest_rank, void *buf, size_t size) {
	struct shmem *dest = shmem + dest_rank;
	struct message *m;
	size_t remainder = size;
	char *p = buf;

	do {
		m = msg_alloc();

		m->m_size = min(remainder, MSG_PAYLOAD_SIZE_BYTES);
		memcpy(m->m_payload, p, m->m_size);
		p += m->m_size;
		remainder -= m->m_size;
		m->m_type = remainder ? MSG_FRAG : MSG_DATA;

		msg_enqueue(&dest->recv_q, m);
	} while(remainder > 0);
}

static void mmpi_send_driller(int dest_rank, void *buf, size_t size) {
	struct shmem *my = shmem + rank;
	struct shmem *dest = shmem + dest_rank;
	struct message *m;
	struct map_rec *map;
	struct fdkey *key;
	struct driller_udata *udata;

	map = driller_lookup_map(buf, size);
	if(map == NULL) {
		mmpi_send_frags(dest_rank, buf, size);
		return;
	}
	assert(map->start <= (off_t)buf);
	assert(map->end >= (off_t)buf + size);

	/* send the fd to fdproxy if not already done */
	if(map->user_data == NULL) {
		udata = driller_malloc(sizeof(*udata) + nprocs);
		assert(udata != NULL);
		memset(udata, 0, sizeof(*udata));
		map->user_data = udata;
		key = &udata->key;
		fdproxy_client_send_fd(map->fd, key);
		memset(udata->references, 0, nprocs);
	} else {
		udata = map->user_data;
		key = &udata->key;
	}
	/* mark dest_rank as user of this map */
	udata->references[dest_rank] = 1;

	m = msg_alloc();

	m->m_type = MSG_DRILLER;
	memcpy(&m->m_drill.map, map, sizeof(*map));
	memcpy(&m->m_drill.key, key, sizeof(*key));
	m->m_drill.offset = (off_t)buf - map->start;
	m->m_drill.length = size;
	m->m_size = sizeof(struct driller_payload);

	/* want to be notified of recv completion */
	my->driller_send_running = 1;

	msg_enqueue(&dest->recv_q, m);

	/* wait for recv completion */
	while(my->driller_send_running)
		nop();
}

void mmpi_send(int dest_rank, void *buf, size_t size) {

	if(size >= MSG_DRILLER_SIZE_THRESHOLD)
		mmpi_send_driller(dest_rank, buf, size);
	else
		mmpi_send_frags(dest_rank, buf, size);
}

static void mmpi_recv_driller(int src_rank, void *buf, size_t *size,
			      struct message *m) {
	struct shmem *src = shmem + src_rank;
	struct map_rec *map;
	struct fdkey *key;
	struct map_cache *mc;

	map = &m->m_drill.map;
	key = &m->m_drill.key;
	mc = map_cache_lookup(key);
	if(mc == NULL) {
		map->fd = fdproxy_client_get_fd(key);
		assert(map->fd >= 0);
		mc = map_cache_install(map, key);
	} else {
		/*
		 * a mapping exists already, but it may need an update,
		 * since we don't invalidate a map in a sibling until 
		 * it is destroyed in its home process
		 *
		 * the update is only required if the data we will read is
		 * not contained in the mapping we already have
		 *
		 * update costs two syscalls, but with the stack or the heap,
		 * it will be common to find the data even with a slightly
		 * stale mapping
		 *
		 * we compute offsets relative to the backing file
		 */
		off_t data_start, data_end;
		off_t local_map_start, local_map_len, local_map_end;

		data_start = map->offset + m->m_drill.offset;
		data_end = data_start + m->m_drill.length;

		local_map_start = mc->mc_map.offset;
		local_map_len = mc->mc_map.end - mc->mc_map.start;
		local_map_end = local_map_start + local_map_len;

		/* is data outside local map? */
		if((data_start < local_map_start)
		   || (data_start >= local_map_end)
		   || (data_end <= local_map_start)
		   || (data_end > local_map_end)) {
			/* it is: need to update the mapping */
			map_cache_update(map, key, mc);
		} else {
			/* it is not: need to fix the data offset */
			m->m_drill.offset = data_start - local_map_start;
		}
	}
	memcpy(buf, mc->mc_addr + m->m_drill.offset, m->m_drill.length);
	*size += m->m_drill.length;

	/* notify sender of recv completion */
	src->driller_send_running = 0;
}

void mmpi_recv(int src_rank, void *buf, size_t *size) {
	struct shmem *my = shmem + rank;
	struct message *m = NULL;
	int last_frag = 0;
	char *p = buf;
	struct fdkey *key;

	*size = 0;
	do {
		m = msg_dequeue_from(&my->recv_q, src_rank);

		switch(m->m_type) {
		case MSG_DATA:
		case MSG_FRAG:
			assert(m->m_size <= MSG_PAYLOAD_SIZE_BYTES);
			memcpy(p, m->m_payload, m->m_size);
			p += m->m_size;
			*size += m->m_size;
			last_frag = (m->m_type == MSG_DATA);
			break;
		case MSG_DRILLER:
			mmpi_recv_driller(src_rank, buf, size, m);
			last_frag = 1;
			break;
		case MSG_DRILLER_INVAL:
			key = &m->m_drill.key;
			dbg("driller_inval on <%s>", fdproxy_keystr(key));
			map_cache_remove(key);
			break;
		default:
			err("bad message type: %d in msg %p", m->m_type, m);
		}

		msg_free(m);
	} while(!last_frag);
}

void mmpi_barrier(void) {

#define box(rank) (shmem[rank].barrier_box)
#define set_box(rank) (box(rank) = flip)
#define box_is_set(rank) (box(rank) == flip)

	if(rank != 0) {
		int n = 0;

		set_box(rank);
		while(!box_is_set(0))
			if(n++ > 10)
				nop();
	} else {
		int i;
		for (i = 1; i < nprocs; i++) {
			int n = 0;

			while(!box_is_set(i))
				if(n++ > 10)
					nop();
		}
		set_box(0);
	}
	flip = !flip;
}

static void mmpi_init_shmem(void) {
	unsigned int page_size;
	unsigned int shmem_size;
	int shmem_fd;
	int i;
	struct fdkey key;

	shmem_size = nprocs*sizeof(*shmem);
	page_size = sysconf(_SC_PAGESIZE);
	shmem_size = (shmem_size + page_size - 1) & ~(page_size - 1);
	fdproxy_set_key_id(&key, SHMEM_KEY_MAGIC);

	if(rank == 0) {
		char *filename;
		int len;

		/* create file */
		len = snprintf(NULL, 0, "%s/mmpi_shmem-%d", TMPDIR, jobid);
		filename = malloc(1+len);
		sprintf(filename, "%s/mmpi_shmem-%d", TMPDIR, jobid);

		shmem_fd = open(filename, O_CREAT|O_TRUNC|O_RDWR, 0600);
		if(shmem_fd < 0)
			perr("open");
		if(unlink(filename) < 0)
			perr("unlink");
		free(filename);
		if(ftruncate(shmem_fd, shmem_size))
			perr("truncate");
		dbg("allocated %d kB of shared mem", shmem_size/1024);

		shmem = mmap(NULL, shmem_size, PROT_READ|PROT_WRITE, 
			     MAP_SHARED|MAP_NORESERVE, shmem_fd, 0);
		if(shmem == (void*)-1)
			perr("mmap");

		/* initialize shmem */
		for(i = 0; i < nprocs; i++) {
			struct shmem *shm = shmem + i;
			struct message *m;

			msg_queue_init(&shm->free_q);
			msg_queue_init(&shm->recv_q);
			for(m = shm->msg_pool;
			    m < shm->msg_pool + MSG_POOL_SIZE; m++) {
				list_init(&m->m_list);
				m->m_type = MSG_FREE;
				m->m_src = i;
				__msg_enqueue(&shm->free_q, m);
			}
		}

		/* now share it with siblings */
		fdproxy_client_send_fd(shmem_fd, &key);
	} else {
		/* retrieve fd for shmem created by rank 0 */
		for(i = 0; i < CONNECT_TIMEOUT; i++) {
			shmem_fd = fdproxy_client_get_fd(&key);
			if(shmem_fd >= 0)
				break;
			sleep(1);
		}
		if(shmem_fd < 0)
			err("could not retrieve shared mem fd after %d seconds",
			    CONNECT_TIMEOUT);

		shmem = mmap(NULL, shmem_size, PROT_READ|PROT_WRITE, 
			     MAP_SHARED|MAP_NORESERVE, shmem_fd, 0);
		if(shmem == (void*)-1)
			perr("mmap");
	}
}

void mmpi_init(int j, int n, int r) {
	jobid = j;
	nprocs = n;
	rank = r;

	if(rank == 0)
		/* only rank 0 forks fdproxy daemon */
		fdproxy_init(jobid, 1);
	else
		fdproxy_init(jobid, 0);

	mmpi_init_shmem();
	driller_init();
	driller_register_map_invalidate_cb(mmpi_map_invalidate_cb);
	map_cache_init();
	mmpi_barrier();
}
