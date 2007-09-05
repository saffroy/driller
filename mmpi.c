/*
 * mini-MPI
 *  a shared-memory pseudo-MPI lib for testing ideas
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sched.h>
#include <assert.h>

#include "mmpi.h"
#include "mmpi_internal.h"
#include "log.h"
#include "fdproxy.h"

#define CONNECT_TIMEOUT 5 /* seconds */
#define USE_TMPFS 1
#define USE_SCHED_YIELD 1

#if USE_TMPFS
#define TMPDIR "/dev/shm"
#else
#define TMPDIR "/tmp"
#endif


static inline void nop(void) {
#if USE_SCHED_YIELD
	sched_yield();
#elif __x86_64__ || __i386__
	asm volatile("rep; nop" : : ); // XXX not sure it's a pause on x64
#else
	/* nop */
#endif
}

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

static void spin_lock_init(struct spinlock *lock) {
#ifndef NDEBUG
	lock->magic = LOCK_MAGIC;
#endif
	lock->lck = 1;
}

static inline int spin_trylock(struct spinlock *lock) {
	int oldval;

#if __x86_64__ || __i386__ 
	asm volatile(
		"xchgl %0,%1"
		:"=q" (oldval), "=m" (lock->lck)
		:"0" (0) : "memory");
#else
#error function spin_trylock needs porting to your architecture!
#endif
        return oldval > 0;
}

static inline void spin_lock(struct spinlock *lock) {
	assert(lock->magic == LOCK_MAGIC);
	while(!spin_trylock(lock))
		nop();
}

static inline void spin_unlock(struct spinlock *lock) {
	assert(lock->magic == LOCK_MAGIC);
	lock->lck = 1;
}

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

static struct message *msg_dequeue_head_from(struct message_queue *q, int src) {
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

/*****************/

static struct shmem *shmem;
static int jobid;
static int nprocs;
static int rank;

static void mmpi_init_shmem(void) {
	unsigned int page_size;
	unsigned int shmem_size;
	int shmem_fd;
	int i;
	struct fdkey key;

	shmem_size = nprocs*sizeof(*shmem);
	page_size = sysconf(_SC_PAGESIZE);
	shmem_size = (shmem_size + page_size - 1) & ~(page_size - 1);
	fdproxy_set_key_id(&key, 0xf003333);

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

void mmpi_init(int jobid, int n, int r) {
	nprocs = n;
	rank = r;

	if(rank == 0)
		/* only rank 0 forks fdproxy daemon */
		fdproxy_init(jobid, 1);
	else
		fdproxy_init(jobid, 0);

	mmpi_init_shmem();
	mmpi_barrier();
}

void mmpi_send(int dest_rank, void *buf, size_t size) {
	struct shmem *my = shmem + rank;
	struct shmem *dest = shmem + dest_rank;
	struct message *m;
	size_t remainder = size;
	char *p = buf;

	do {
		m = msg_dequeue_head(&my->free_q);

		m->m_size = min(remainder, MSG_PAYLOAD_SIZE_BYTES);
		memcpy(m->m_payload, p, m->m_size);
		p += m->m_size;
		remainder -= m->m_size;
		m->m_flags = remainder ? MSGFLAG_NONE : MSGFLAG_LAST_FRAG;

		msg_queue_lock(&dest->recv_q);
		__msg_enqueue(&dest->recv_q, m);
		msg_queue_unlock(&dest->recv_q);
	} while(remainder > 0);
}

void mmpi_recv(int src_rank, void *buf, size_t *size) {
	struct shmem *my = shmem + rank;
	struct shmem *src = shmem + src_rank;
	struct message *m = NULL;
	int last_frag;
	char *p = buf;

	*size = 0;
	do {
		m = msg_dequeue_head_from(&my->recv_q, src_rank);

		memcpy(p, m->m_payload, m->m_size);
		p += m->m_size;
		*size += m->m_size;
		last_frag = (m->m_flags & MSGFLAG_LAST_FRAG);

		msg_queue_lock(&src->free_q);
		__msg_enqueue_head(&src->free_q, m);
		msg_queue_unlock(&src->free_q);
	} while(!last_frag);
}

void mmpi_barrier(void) {
	static char flip = 1;

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
