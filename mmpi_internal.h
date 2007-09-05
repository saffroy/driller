#ifndef MMPI_INTERNAL_H
#define MMPI_INTERNAL_H

/*
 * lists for shared mem
 */

#define LIST_MAGIC 0xf001157
struct list_head {
#ifndef NDEBUG
	int magic;
#endif
	off_t off_next, off_prev;
};

/*
 * spinlocks
 */

#define LOCK_MAGIC 0xf0010c4
struct spinlock {
#ifndef NDEBUG
	int magic;
#endif
	volatile unsigned int lck;
};

/*
 * shared mem and messages
 */

#define CONNECT_TIMEOUT 5 /* seconds */
#define USE_TMPFS 1
#define USE_SCHED_YIELD 1
#define SHMEM_KEY_MAGIC 0xf003333

#if USE_TMPFS
#define TMPDIR "/dev/shm"
#else
#define TMPDIR "/tmp"
#endif

#define __cacheline_aligned __attribute__((__aligned__(64)))
#define MSG_PAYLOAD_SIZE_BYTES 4096
#define MSG_POOL_SIZE 1024

enum msg_flags {
	MSGFLAG_NONE = 0,
	MSGFLAG_LAST_FRAG = 1,
};

struct message {
	struct list_head m_list;
	enum msg_flags m_flags;
	int m_size;
	int m_src;
	char m_payload[MSG_PAYLOAD_SIZE_BYTES];
};


struct message_queue {
	struct spinlock q_lock __cacheline_aligned;
	struct list_head q_list;
	int q_length;
};

struct shmem {
	volatile int barrier_box __cacheline_aligned;
	struct message_queue free_q;
	struct message_queue recv_q;
	struct message msg_pool[MSG_POOL_SIZE];
};


#endif /* MMPI_INTERNAL_H */
